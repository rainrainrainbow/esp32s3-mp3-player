#ifndef FATFS_MANAGER_H
#define FATFS_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

bool fatfs_mount_spiflash(void);
bool fatfs_unmount(void);
bool fatfs_file_exists(const char *path);
uint32_t fatfs_get_file_size(const char *path);
bool fatfs_read_file(const char *path, uint8_t *buf, size_t *size, size_t max_size);

#endif // FATFS_MANAGER_H