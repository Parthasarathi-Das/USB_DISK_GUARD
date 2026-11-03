#define _GNU_SOURCE
#include "disk.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

// ---------------------------------------------------------------------------
// Discovery (via /sys)
// ---------------------------------------------------------------------------

bool blockExists(const char* name) {
    char path[300];
    snprintf(path, sizeof(path), "/sys/class/block/%s", name);
    return access(path, F_OK) == 0;
}

static bool isUsbDisk(const char* disk) {
    char link[300], resolved[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/block/%s", disk);
    if (!realpath(link, resolved))
        return false;
    return strstr(resolved, "/usb") != NULL;
}

static long diskSize(const char* disk) {
    char path[300];
    snprintf(path, sizeof(path), "/sys/block/%s/size", disk);
    FILE* fp = fopen(path, "r");
    if (!fp)
        return 0;
    long size = 0;
    if (fscanf(fp, "%ld", &size) != 1)
        size = 0;
    fclose(fp);
    return size;
}

bool diskPresent(const char* disk) {
    return blockExists(disk) && diskSize(disk) > 0;
}

// Whole USB disks (sdb, sdc...). Empty card-reader slots (size 0) are skipped,
// they show up as "new" once a card is actually inserted.
int getUsbDisks(char disks[MAX_DISKS][32]) {
    DIR* d = opendir("/sys/block");
    if (!d)
        return 0;
    int count = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && count < MAX_DISKS) {
        if (strncmp(e->d_name, "sd", 2) != 0)
            continue;
        if (!isUsbDisk(e->d_name) || diskSize(e->d_name) == 0)
            continue;
        snprintf(disks[count++], 32, "%s", e->d_name);
    }
    closedir(d);
    return count;
}

// Partitions of one disk (sdb -> sdb1, sdb2). Returns 0 if the disk has none.
int getPartitions(const char* disk, char parts[MAX_PARTS][32]) {
    char path[300];
    snprintf(path, sizeof(path), "/sys/block/%s", disk);
    DIR* d = opendir(path);
    if (!d)
        return 0;
    int count = 0;
    struct dirent* e;
    size_t n = strlen(disk);
    while ((e = readdir(d)) != NULL && count < MAX_PARTS) {
        if (strncmp(e->d_name, disk, n) == 0 && e->d_name[n] != '\0')
            snprintf(parts[count++], 32, "%s", e->d_name);
    }
    closedir(d);
    return count;
}

bool inList(char list[][32], int count, const char* name) {
    for (int i = 0; i < count; i++)
        if (strcmp(list[i], name) == 0)
            return true;
    return false;
}

// Read ID_SERIAL from udev's database entry of the disk (/run/udev/data/b<major>:<minor>)
bool diskSerial(const char* disk, char* out, size_t len) {
    out[0] = '\0';
    char path[300];
    snprintf(path, sizeof(path), "/sys/block/%s/dev", disk);
    FILE* fp = fopen(path, "r");
    if (!fp)
        return false;
    unsigned maj = 0, min = 0;
    int ok = fscanf(fp, "%u:%u", &maj, &min) == 2;
    fclose(fp);
    if (!ok)
        return false;

    snprintf(path, sizeof(path), "/run/udev/data/b%u:%u", maj, min);
    fp = fopen(path, "r");
    if (!fp)
        return false;
    const char key[] = "E:ID_SERIAL=";
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, sizeof(key) - 1) == 0) {
            snprintf(out, len, "%s", line + sizeof(key) - 1);
            out[strcspn(out, "\r\n")] = '\0';
            break;
        }
    }
    fclose(fp);
    return true;
}

// ---------------------------------------------------------------------------
// Lock / mount / eject
// ---------------------------------------------------------------------------

// Unmount the disk and all its partitions (covers a mount done before the rule applied)
void lockDisk(const char* disk) {
    char parts[MAX_PARTS][32];
    int n = getPartitions(disk, parts);
    char dev[64];
    for (int i = 0; i < n; i++) {
        snprintf(dev, sizeof(dev), "/dev/%s", parts[i]);
        char* argv[] = {"umount", dev, NULL};
        run_cmd(argv);
    }
    snprintf(dev, sizeof(dev), "/dev/%s", disk);
    char* argv[] = {"umount", dev, NULL};
    run_cmd(argv);
}

static bool isMounted(const char* devpath) {
    FILE* fp = fopen("/proc/mounts", "r");
    if (!fp)
        return false;
    char line[1024];
    size_t n = strlen(devpath);
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, devpath, n) == 0 && line[n] == ' ') {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

// The mount is requested through udisks as the normal desktop user. That makes it a
// regular desktop mount (/run/media/<user>/...): the file manager shows it and the user
// can unmount / eject it any time without root. Nothing is opened on screen.
static bool mountOne(const char* part) {
    char dev[64];
    snprintf(dev, sizeof(dev), "/dev/%s", part);

    for (int attempt = 0; attempt < MOUNT_RETRIES && g_running; attempt++) {
        if (isMounted(dev))
            return true; // the desktop already mounted it itself
        if (!blockExists(part))
            return false;
        char* argv[] = {"udisksctl", "mount", "-b", dev, "--no-user-interaction", NULL};
        if (run_as_user(argv) == 0)
            return true;
        sleep_ms(500); // udisks may need a moment to notice the freshly unlocked device
    }
    return isMounted(dev);
}

// Mount every partition (or the disk itself if it has no partition table)
bool mountDisk(const char* disk) {
    char parts[MAX_PARTS][32];
    int n = getPartitions(disk, parts);
    if (n == 0) {
        snprintf(parts[0], 32, "%s", disk);
        n = 1;
    }

    int ok = 0;
    for (int i = 0; i < n && g_running; i++) {
        if (mountOne(parts[i])) {
            printf("[+] Mounted /dev/%s (normal desktop mount, eject it as usual)\n", parts[i]);
            ok++;
        }
    }
    return ok > 0;
}

// Unmount, flush, then tell the kernel to remove the disk (it can be unplugged safely)
void ejectDisk(const char* disk) {
    lockDisk(disk);
    sync();
    char path[300];
    snprintf(path, sizeof(path), "/sys/block/%s/device/delete", disk);
    FILE* fp = fopen(path, "w");
    if (fp) {
        fputs("1", fp);
        fclose(fp);
    }
}