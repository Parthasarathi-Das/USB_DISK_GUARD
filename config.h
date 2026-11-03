#ifndef CONFIG_H
#define CONFIG_H

#define CORRECT_PASSWORD "1234" // Change your password here
#define MAX_ATTEMPTS 3

// /run is RAM-only: the rule disappears on reboot even after a crash or power loss.
#define UDEV_RULE_DIR "/run/udev/rules.d"
#define UDEV_RULE_FILE "/run/udev/rules.d/99-usb-guard.rules"
#define UDEV_RULE_TMP "/run/udev/rules.d/99-usb-guard.rules.tmp" // ignored by udev (not *.rules)
#define LEGACY_RULE_FILE "/etc/udev/rules.d/99-usb-guard.rules"  // from the old version

#define MAX_DISKS 16
#define MAX_PARTS 16
#define MOUNT_RETRIES 3

#endif
