#ifndef GUARD_H
#define GUARD_H

#include <stdbool.h>

// Install the "hide every USB drive from the desktop" udev rule.
bool guardInstall(void);

// Remove the rule and give every USB drive back to the desktop.
void guardRemove(void);

// Password accepted: let the desktop manage THIS drive normally (and only this one).
// All other / future USB drives stay hidden.
bool guardAuthorize(const char* disk);

// Forget authorized drives that were unplugged / ejected / lost their media, so the guard takes
// authority again and a different drive that gets the same name is not let through.
void guardRevokeGone(void);

#endif