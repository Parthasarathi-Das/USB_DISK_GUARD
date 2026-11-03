// USB Guard for Linux (Ubuntu 24.04)
//
// Build:   make
// Run:     sudo ./usbguard
// Repair:  sudo ./usbguard --cleanup   (only if the program was killed with SIGKILL and you
//                                       don't want to wait for a reboot)
//
// How it works:
//  1. A udev rule (kept in /run, wiped on every reboot) makes the desktop IGNORE USB
//     storage: drives are never auto-mounted and never shown in the file manager.
//  2. The program watches for USB disks (including ones already plugged in at start).
//  3. For each new disk it asks for the password in a zenity window (3 tries).
//  4. Correct: only THAT drive is released to the desktop and mounted normally through
//     udisks. Nothing is opened on screen, and the user can unmount/eject it at any time.
//     The guard then steps back and only acts again when a new drive arrives.
//     Wrong 3 times: the drive is ejected.
//  5. On exit the rule is removed and everything behaves normally again.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "disk.h"
#include "guard.h"
#include "prompt.h"
#include "util.h"

static void handleNewDisk(const char* disk) {
    printf("\n[+] USB drive detected: /dev/%s\n", disk);

    lockDisk(disk); // undo any automount that slipped in before the guard reacted
    sleep_ms(1000); // let the partitions appear so we prompt only ONCE per physical drive
    if (!g_running || !blockExists(disk))
        return;

    lockDisk(disk);
    printf("[*] Drive locked (not mounted) until the password is verified.\n");

    switch (verifyPassword(disk)) {
    case V_OK:
        printf("[+] Access granted for /dev/%s.\n", disk);
        if (!guardAuthorize(disk)) {
            printf("[!] Could not release /dev/%s to the desktop.\n", disk);
            show_gui_message("error", "USB Security Guard",
                             "Password correct, but the drive could not be released.");
            break;
        }
        if (!mountDisk(disk)) {
            printf("[!] /dev/%s is unlocked but could not be mounted automatically.\n", disk);
            show_gui_message("error", "USB Security Guard",
                             "Password correct. The drive is unlocked but could not be "
                             "mounted automatically; open it from the file manager.");
        }
        printf("[*] Guard stepped back from /dev/%s. Waiting for new drives.\n", disk);
        break;
    case V_DENIED:
        printf("[x] Maximum attempts reached. Ejecting /dev/%s.\n", disk);
        show_gui_message("error", "Access Denied",
                         "Too many wrong passwords.\nThe USB drive has been ejected.");
        ejectDisk(disk);
        break;
    case V_GONE:
        printf("[*] /dev/%s was unplugged.\n", disk);
        break;
    case V_ABORT:
        break; // program is stopping or no way to ask; drive stays locked
    }
}

static void exit_cleanup(void) {
    guardRemove(); // does nothing if already removed
}

int main(int argc, char* argv[]) {
    if (geteuid() != 0) {
        fprintf(stderr, "[!] This program must run as root:  sudo %s\n", argv[0]);
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "--cleanup") == 0) {
        guardRemove();
        printf("[+] Cleanup done. USB drives behave normally again.\n");
        return 0;
    }

    printf("====================================================\n");
    printf("   USB GUARD (LINUX)\n");
    printf("====================================================\n");

    atexit(exit_cleanup);
    install_signal_handlers();

    if (!guardInstall()) {
        fprintf(stderr, "[!] Could not install the udev rule in %s.\n", UDEV_RULE_DIR);
        return 1;
    }
    printf("[*] Guard active: USB drives are hidden from the file manager until unlocked.\n");
    printf("[*] Press Ctrl+C to stop.\n");

    // Start with an empty list so drives that are ALREADY plugged in get locked too
    char prev[MAX_DISKS][32];
    int prev_count = 0;

    while (g_running) {
        char curr[MAX_DISKS][32];
        int curr_count = getUsbDisks(curr);

        guardRevokeGone(); // unplugged/ejected drives lose their authorization

        for (int i = 0; i < curr_count && g_running; i++) {
            if (!inList(prev, prev_count, curr[i]))
                handleNewDisk(curr[i]);
        }

        prev_count = curr_count;
        for (int i = 0; i < curr_count; i++)
            snprintf(prev[i], 32, "%s", curr[i]);

        sleep_ms(200);
    }

    printf("\n[*] Stopping, restoring normal USB behavior...\n");
    return 0; // exit_cleanup() runs here
}