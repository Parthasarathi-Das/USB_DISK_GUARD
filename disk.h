#ifndef DISK_H
#define DISK_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"

bool blockExists(const char* name);
bool diskPresent(const char* disk); // node exists AND media is inserted (size > 0)
int getUsbDisks(char disks[MAX_DISKS][32]);
int getPartitions(const char* disk, char parts[MAX_PARTS][32]);
bool inList(char list[][32], int count, const char* name);

// udev's ID_SERIAL of the disk ("" if it has none). Returns false if it cannot be read.
bool diskSerial(const char* disk, char* out, size_t len);

void lockDisk(const char* disk);  // unmount disk + partitions
bool mountDisk(const char* disk); // mount through udisks as the desktop user
void ejectDisk(const char* disk); // unmount, flush, remove from the kernel

#endif