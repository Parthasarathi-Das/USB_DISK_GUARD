#define _GNU_SOURCE
#include "guard.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "disk.h"
#include "util.h"

typedef struct {
    char disk[32];
    char serial[128];
    bool has_serial; // false: serial could not be read, match by device name only
} AuthEntry;

static AuthEntry auth[MAX_DISKS];
static int auth_count = 0;

// Make udev re-evaluate the given USB block devices (sysname glob, or NULL for all of them)
static void refreshUdev(const char* sysname_glob) {
    char* reload[] = {"udevadm", "control", "--reload-rules", NULL};
    run_cmd(reload);

    if (sysname_glob) {
        char match[64];
        snprintf(match, sizeof(match), "--sysname-match=%s", sysname_glob);
        char* trigger[] = {"udevadm", "trigger", "--subsystem-match=block", match,
                           "--action=change", NULL};
        run_cmd(trigger);
    } else {
        char* trigger[] = {"udevadm", "trigger", "--subsystem-match=block",
                           "--property-match=ID_BUS=usb", "--action=change", NULL};
        run_cmd(trigger);
    }
    char* settle[] = {"udevadm", "settle", "--timeout=5", NULL};
    run_cmd(settle);
}

// Anything that is not a plain character becomes a '?' wildcard, so a hostile serial
// number can never inject extra udev rule syntax.
static void sanitizeSerial(const char* in, char* out, size_t len) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < len; i++) {
        unsigned char c = (unsigned char)in[i];
        out[j++] = (isalnum(c) || c == '_' || c == '.' || c == ':' || c == '+' || c == '-')
                       ? (char)c
                       : '?';
    }
    out[j] = '\0';
}

// Write the rule file atomically. open_all=true writes a rule that releases every drive.
static bool writeRule(bool open_all) {
    mkdir_p(UDEV_RULE_DIR);
    FILE* fp = fopen(UDEV_RULE_TMP, "w");
    if (!fp)
        return false;

    fprintf(fp, "# Installed by USB Guard (lives in RAM, gone after reboot)\n");
    if (open_all) {
        fprintf(fp, "ACTION==\"add|change\", SUBSYSTEM==\"block\", ENV{ID_BUS}==\"usb\", "
                    "ENV{UDISKS_IGNORE}=\"0\"\n");
    } else {
        // Drives that passed the password check: explicitly NOT ignored, so udisks and the
        // file manager treat them like any normal drive (mount, unmount, eject by the user).
        for (int i = 0; i < auth_count; i++) {
            char ser_clause[160] = "";
            if (auth[i].has_serial) {
                char ser[128];
                sanitizeSerial(auth[i].serial, ser, sizeof(ser));
                snprintf(ser_clause, sizeof(ser_clause), "ENV{ID_SERIAL}==\"%s\", ", ser);
            }
            fprintf(fp,
                    "ACTION==\"add|change\", SUBSYSTEM==\"block\", ENV{ID_BUS}==\"usb\", "
                    "KERNEL==\"%s|%s[0-9]*\", %s"
                    "ENV{UDISKS_IGNORE}=\"0\", GOTO=\"usb_guard_end\"\n",
                    auth[i].disk, auth[i].disk, ser_clause);
        }
        // Everyone else stays hidden and unmounted.
        fprintf(fp, "ACTION==\"add|change\", SUBSYSTEM==\"block\", ENV{ID_BUS}==\"usb\", "
                    "ENV{UDISKS_IGNORE}=\"1\"\n");
        fprintf(fp, "LABEL=\"usb_guard_end\"\n");
    }
    if (fclose(fp) != 0) {
        unlink(UDEV_RULE_TMP);
        return false;
    }
    if (rename(UDEV_RULE_TMP, UDEV_RULE_FILE) != 0) {
        unlink(UDEV_RULE_TMP);
        return false;
    }
    return true;
}

bool guardInstall(void) {
    if (access(LEGACY_RULE_FILE, F_OK) == 0)
        unlink(LEGACY_RULE_FILE);
    auth_count = 0;
    if (!writeRule(false))
        return false;
    refreshUdev(NULL); // also applies to drives that are already plugged in
    return true;
}

void guardRemove(void) {
    bool have_rule = access(UDEV_RULE_FILE, F_OK) == 0;
    bool have_legacy = access(LEGACY_RULE_FILE, F_OK) == 0;
    if (!have_rule && !have_legacy)
        return;

    // udev keeps old properties on "change" events, so first publish an explicit
    // "not ignored" state, and only then delete the rule file.
    if (writeRule(true))
        refreshUdev(NULL);
    unlink(UDEV_RULE_FILE);
    unlink(LEGACY_RULE_FILE);
    char* reload[] = {"udevadm", "control", "--reload-rules", NULL};
    run_cmd(reload);
    auth_count = 0;
}

bool guardAuthorize(const char* disk) {
    AuthEntry e;
    snprintf(e.disk, sizeof(e.disk), "%s", disk);
    e.has_serial = diskSerial(disk, e.serial, sizeof(e.serial));
    if (!e.has_serial)
        printf("[!] Could not read the serial of /dev/%s, releasing it by device name.\n", disk);

    int idx = -1;
    bool added = false;
    for (int i = 0; i < auth_count; i++)
        if (strcmp(auth[i].disk, disk) == 0)
            idx = i;
    if (idx < 0) {
        if (auth_count >= MAX_DISKS) {
            printf("[!] Too many authorized drives.\n");
            return false;
        }
        idx = auth_count++;
        added = true;
    }
    auth[idx] = e;

    if (!writeRule(false)) {
        perror("[!] Could not write the udev rule");
        if (added)
            auth_count--;
        return false;
    }

    char glob[40];
    snprintf(glob, sizeof(glob), "%s*", disk);
    refreshUdev(glob);
    return true;
}

void guardRevokeGone(void) {
    bool changed = false;
    for (int i = auth_count - 1; i >= 0; i--) {
        if (!diskPresent(auth[i].disk)) { // unplugged, or media ejected (size 0)
            auth[i] = auth[auth_count - 1];
            auth_count--;
            changed = true;
        }
    }
    if (changed && writeRule(false)) {
        char* reload[] = {"udevadm", "control", "--reload-rules", NULL};
        run_cmd(reload);
    }
}