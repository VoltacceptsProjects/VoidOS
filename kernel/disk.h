#ifndef VOIDOS_DISK_H
#define VOIDOS_DISK_H

#include <stdint.h>

#define VOIDOS_DISK_SECTOR_SIZE 512
#define VOIDOS_MAX_DISKS 16

enum void_disk_type {
    VOID_DISK_ATA = 0,
    VOID_DISK_AHCI = 1,
    VOID_DISK_NVME = 2,
};

struct void_disk_info {
    uint8_t present;
    uint8_t type;
    uint8_t channel;
    uint8_t slave;
    uint32_t sectors;
    char model[41];
};

/* Probes legacy IDE, PCI AHCI/SATA, and PCI NVMe controllers. */
void disk_initialize(void);
unsigned int disk_count(void);
const struct void_disk_info* disk_at(unsigned int index);

int disk_read_sector(unsigned int index, uint32_t lba, uint8_t* buffer);
int disk_write_sector(unsigned int index, uint32_t lba, const uint8_t* buffer);

void disk_print_info(void);

const char* disk_type_name(uint8_t type);

#endif