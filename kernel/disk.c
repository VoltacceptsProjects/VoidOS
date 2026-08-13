#include "disk.h"
#include "io.h"
#include "vga.h"

#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_REG_DATA 0
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA0 3
#define ATA_REG_LBA1 4
#define ATA_REG_LBA2 5
#define ATA_REG_DEVICE 6
#define ATA_REG_STATUS 7
#define ATA_REG_COMMAND 7

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_STATUS_ERR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_DF 0x20
#define ATA_STATUS_BSY 0x80

struct ata_channel {
    uint16_t io;
    uint16_t ctrl;
};

static const struct ata_channel channels[2] = {
    { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL },
    { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL },
};

static struct void_disk_info disks[VOIDOS_MAX_DISKS];
static struct ata_channel disk_channels[VOIDOS_MAX_DISKS];
static unsigned int disk_count_value = 0;

static void io_delay(uint16_t ctrl) {
    (void)inb(ctrl);
    (void)inb(ctrl);
    (void)inb(ctrl);
    (void)inb(ctrl);
}

static uint8_t wait_status(const struct ata_channel* channel) {
    uint8_t status = 0;
    for (uint32_t timeout = 0; timeout < 1000000; timeout++) {
        status = inb(channel->io + ATA_REG_STATUS);
        if (!(status & ATA_STATUS_BSY)) return status;
    }
    return status;
}

static int select_drive(const struct ata_channel* channel, uint8_t slave,
                        uint32_t lba) {
    outb(channel->io + ATA_REG_DEVICE,
         (uint8_t)(0xE0 | (slave << 4) | ((lba >> 24) & 0x0F)));
    io_delay(channel->ctrl);
    uint8_t status = inb(channel->io + ATA_REG_STATUS);
    return status != 0xFF && status != 0;
}

static void model_from_identify(char* out, const uint16_t* identify) {
    unsigned int pos = 0;
    for (unsigned int word = 27; word <= 46 && pos + 1 < 41; word++) {
        char high = (char)(identify[word] >> 8);
        char low = (char)(identify[word] & 0xFF);
        if (high != ' ' && high != '\0') out[pos++] = high;
        if (low != ' ' && low != '\0' && pos + 1 < 41) out[pos++] = low;
    }
    while (pos && out[pos - 1] == ' ') pos--;
    out[pos] = '\0';
}

static int identify_drive(unsigned int disk_index, uint8_t channel_index,
                          uint8_t slave) {
    const struct ata_channel* channel = &channels[channel_index];
    if (!select_drive(channel, slave, 0)) return 0;

    outb(channel->io + ATA_REG_SECCOUNT, 0);
    outb(channel->io + ATA_REG_LBA0, 0);
    outb(channel->io + ATA_REG_LBA1, 0);
    outb(channel->io + ATA_REG_LBA2, 0);
    outb(channel->io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(channel->io + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) return 0;
    status = wait_status(channel);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) return 0;
    if (!(status & ATA_STATUS_DRQ)) return 0;

    uint16_t identify[256];
    for (unsigned int i = 0; i < 256; i++) identify[i] = inw(channel->io + ATA_REG_DATA);

    uint32_t sectors = ((uint32_t)identify[61] << 16) | identify[60];
    if (!sectors) {
        sectors = ((uint32_t)identify[103] << 16) |
                  identify[102];
    }
    if (!sectors || disk_index >= VOIDOS_MAX_DISKS) return 0;

    disks[disk_index].present = 1;
    disks[disk_index].channel = channel_index;
    disks[disk_index].slave = slave;
    disks[disk_index].sectors = sectors;
    model_from_identify(disks[disk_index].model, identify);
    disk_channels[disk_index] = *channel;
    return 1;
}

void disk_initialize(void) {
    disk_count_value = 0;
    for (unsigned int i = 0; i < VOIDOS_MAX_DISKS; i++) {
        disks[i].present = 0;
        disks[i].model[0] = '\0';
        disks[i].sectors = 0;
    }
    for (uint8_t channel = 0; channel < 2; channel++) {
        for (uint8_t slave = 0; slave < 2; slave++) {
            if (disk_count_value >= VOIDOS_MAX_DISKS) return;
            if (identify_drive(disk_count_value, channel, slave)) disk_count_value++;
        }
    }
}

unsigned int disk_count(void) {
    return disk_count_value;
}

const struct void_disk_info* disk_at(unsigned int index) {
    if (index >= disk_count_value) return (const struct void_disk_info*)0;
    return &disks[index];
}

int disk_read_sector(unsigned int index, uint32_t lba, uint8_t* buffer) {
    if (index >= disk_count_value || !buffer || lba >= disks[index].sectors) return -1;
    const struct ata_channel* channel = &disk_channels[index];
    uint8_t slave = disks[index].slave;
    if (!select_drive(channel, slave, lba)) return -1;
    outb(channel->io + ATA_REG_SECCOUNT, 1);
    outb(channel->io + ATA_REG_LBA0, (uint8_t)lba);
    outb(channel->io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(channel->io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outb(channel->io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    uint8_t status = wait_status(channel);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF) || !(status & ATA_STATUS_DRQ)) return -1;
    for (unsigned int i = 0; i < 256; i++) {
        uint16_t word = inw(channel->io + ATA_REG_DATA);
        buffer[i * 2] = (uint8_t)word;
        buffer[i * 2 + 1] = (uint8_t)(word >> 8);
    }
    return 0;
}

int disk_write_sector(unsigned int index, uint32_t lba, const uint8_t* buffer) {
    if (index >= disk_count_value || !buffer || lba >= disks[index].sectors) return -1;
    const struct ata_channel* channel = &disk_channels[index];
    if (!select_drive(channel, disks[index].slave, lba)) return -1;
    outb(channel->io + ATA_REG_SECCOUNT, 1);
    outb(channel->io + ATA_REG_LBA0, (uint8_t)lba);
    outb(channel->io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(channel->io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outb(channel->io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    uint8_t status = wait_status(channel);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF) || !(status & ATA_STATUS_DRQ)) return -1;
    for (unsigned int i = 0; i < 256; i++) {
        uint16_t word = (uint16_t)buffer[i * 2] |
                        ((uint16_t)buffer[i * 2 + 1] << 8);
        outw(channel->io + ATA_REG_DATA, word);
    }
    outb(channel->io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return (wait_status(channel) & (ATA_STATUS_ERR | ATA_STATUS_DF)) ? -1 : 0;
}

void disk_print_info(void) {
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_DARK_GREY);
    terminal_writestring("Detected ATA drives\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    if (!disk_count_value) {
        terminal_writestring("  No ATA PIO drives detected.\n");
        terminal_writestring("  AHCI/NVMe support is not enabled yet.\n");
        return;
    }
    for (unsigned int i = 0; i < disk_count_value; i++) {
        terminal_writestring("  Drive ");
        terminal_write_uint(i);
        terminal_writestring(": ");
        terminal_writestring(disks[i].model[0] ? disks[i].model : "ATA disk");
        terminal_writestring("  ");
        terminal_write_uint(disks[i].sectors);
        terminal_writestring(" sectors\n");
    }
}

/* Reports a single drive - the one VoidOS is actually running from
 * (mounted VoidFS volume) or would install to - instead of every ATA
 * position the controller happens to expose. A VM/PC can have several
 * disks attached; the Files page only cares about VoidOS's own drive. */
void disk_print_drive(unsigned int index) {
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_DARK_GREY);
    terminal_writestring("Boot drive\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    if (index >= disk_count_value) {
        terminal_writestring("  No ATA PIO drive detected.\n");
        terminal_writestring("  AHCI/NVMe support is not enabled yet.\n");
        return;
    }
    terminal_writestring("  Drive ");
    terminal_write_uint(index);
    terminal_writestring(": ");
    terminal_writestring(disks[index].model[0] ? disks[index].model : "ATA disk");
    terminal_writestring("  ");
    terminal_write_uint(disks[index].sectors);
    terminal_writestring(" sectors\n");
}