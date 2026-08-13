#include "disk.h"
#include "io.h"
#include "vga.h"
#include <stdint.h>

/*
 * VoidOS starts without paging, so physical PCI MMIO addresses and the
 * kernel's static buffers share the same 32-bit address space.  The drivers
 * below intentionally use polling and one-sector transfers: that keeps them
 * usable before interrupts and a general page allocator exist, while still
 * making SATA/AHCI and NVMe disks real VoidFS backing volumes.
 */

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_REG_DATA     0
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA0     3
#define ATA_REG_LBA1     4
#define ATA_REG_LBA2     5
#define ATA_REG_DEVICE   6
#define ATA_REG_STATUS   7
#define ATA_REG_COMMAND  7

#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_READ_PIO     0x20
#define ATA_CMD_WRITE_PIO    0x30
#define ATA_CMD_CACHE_FLUSH  0xE7

#define ATA_STATUS_ERR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_DF  0x20
#define ATA_STATUS_BSY 0x80

#define AHCI_MAX_PORTS 32
#define AHCI_CMD_SLOTS 1
#define AHCI_SIG_ATA   0x00000101
#define AHCI_PORT_DET_PRESENT 3
#define AHCI_PORT_IPM_ACTIVE  1

#define NVME_MAX_CONTROLLERS 4
#define NVME_QUEUE_DEPTH     16

struct ata_channel {
    uint16_t io;
    uint16_t ctrl;
};

struct ahci_port_regs {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t reserved0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t devslp;
    uint32_t reserved1[10];
    uint32_t vendor[4];
};

struct ahci_cmd_header {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
};

struct ahci_prdt {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;
};

struct ahci_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct ahci_prdt prdt[8];
};

struct nvme_command {
    uint32_t cdw0;
    uint32_t nsid;
    uint32_t reserved2[2];
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

struct nvme_completion {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;
};

struct nvme_state {
    uint32_t mmio;
    uint32_t doorbell_stride;
    uint16_t queue_depth;
    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    uint16_t io_sq_tail;
    uint16_t io_cq_head;
    uint16_t next_cid;
    uint8_t admin_phase;
    uint8_t io_phase;
    uint8_t io_ready;
    uint32_t namespace_id;
    uint32_t sectors;
    char model[41];
};

static const struct ata_channel channels[2] = {
    { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL },
    { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL },
};

static struct void_disk_info disks[VOIDOS_MAX_DISKS];
static struct ata_channel disk_channels[VOIDOS_MAX_DISKS];
static uint8_t disk_backend[VOIDOS_MAX_DISKS];
static uint8_t disk_backend_index[VOIDOS_MAX_DISKS];
static uint8_t disk_ahci_port[VOIDOS_MAX_DISKS];
static volatile struct ahci_port_regs* disk_ahci_ports[VOIDOS_MAX_DISKS];
static unsigned int disk_count_value;

static volatile uint8_t ahci_clb[VOIDOS_MAX_DISKS][1024]
    __attribute__((aligned(1024)));
static volatile uint8_t ahci_fis[VOIDOS_MAX_DISKS][256]
    __attribute__((aligned(256)));
static volatile struct ahci_cmd_table ahci_tables[VOIDOS_MAX_DISKS]
    __attribute__((aligned(128)));
static uint16_t ahci_identify_data[VOIDOS_MAX_DISKS][256]
    __attribute__((aligned(2)));

static volatile struct nvme_command nvme_admin_sq[NVME_MAX_CONTROLLERS][NVME_QUEUE_DEPTH]
    __attribute__((aligned(4096)));
static volatile struct nvme_completion nvme_admin_cq[NVME_MAX_CONTROLLERS][NVME_QUEUE_DEPTH]
    __attribute__((aligned(4096)));
static volatile struct nvme_command nvme_io_sq[NVME_MAX_CONTROLLERS][NVME_QUEUE_DEPTH]
    __attribute__((aligned(4096)));
static volatile struct nvme_completion nvme_io_cq[NVME_MAX_CONTROLLERS][NVME_QUEUE_DEPTH]
    __attribute__((aligned(4096)));
static uint8_t nvme_identify_controller[NVME_MAX_CONTROLLERS][4096]
    __attribute__((aligned(4096)));
static uint8_t nvme_identify_namespace[NVME_MAX_CONTROLLERS][4096]
    __attribute__((aligned(4096)));
static uint8_t nvme_io_buffer[NVME_MAX_CONTROLLERS][VOIDOS_DISK_SECTOR_SIZE]
    __attribute__((aligned(4096)));
static struct nvme_state nvme_controllers[NVME_MAX_CONTROLLERS];
static unsigned int nvme_controller_count;
static unsigned int storage_controller_count;

static inline void memory_barrier(void) {
    __asm__ volatile ("" ::: "memory");
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((1u << 31) | ((uint32_t)bus << 16) |
                                  ((uint32_t)dev << 11) |
                                  ((uint32_t)func << 8) | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_vendor(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint16_t)pci_read32(bus, dev, func, 0x00);
}

static uint8_t pci_class(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint8_t)(pci_read32(bus, dev, func, 0x08) >> 24);
}

static uint8_t pci_subclass(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint8_t)(pci_read32(bus, dev, func, 0x08) >> 16);
}

static uint8_t pci_prog_if(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint8_t)(pci_read32(bus, dev, func, 0x08) >> 8);
}

static uint8_t pci_header_type(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint8_t)(pci_read32(bus, dev, func, 0x0C) >> 16);
}

static uint64_t pci_bar(uint8_t bus, uint8_t dev, uint8_t func,
                        uint8_t bar_index, int* is_memory) {
    uint8_t offset = (uint8_t)(0x10 + bar_index * 4);
    uint32_t low = pci_read32(bus, dev, func, offset);
    if (low & 1) {
        *is_memory = 0;
        return 0;
    }
    *is_memory = 1;
    uint64_t address = low & ~0x0Fu;
    if (((low >> 1) & 3) == 2 && bar_index < 5) {
        address |= (uint64_t)pci_read32(bus, dev, func, (uint8_t)(offset + 4)) << 32;
    }
    return address;
}

static void clear_bytes(volatile uint8_t* bytes, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) bytes[i] = 0;
}

static void copy_bounded(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    if (!cap) return;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
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

static void model_from_bytes(char* out, const uint8_t* data,
                             unsigned int offset, unsigned int length) {
    unsigned int pos = 0;
    while (length && data[offset + length - 1] == ' ') length--;
    while (pos + 1 < 41 && pos < length) {
        uint8_t c = data[offset + pos];
        out[pos] = (c >= 32 && c <= 126) ? (char)c : '?';
        pos++;
    }
    out[pos] = '\0';
}

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t* p) {
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static uint32_t sectors_from_ata_identify(const uint16_t* identify) {
    uint64_t sectors = 0;
    if (identify[83] & (1u << 10)) {
        sectors = (uint64_t)identify[100] |
                  ((uint64_t)identify[101] << 16) |
                  ((uint64_t)identify[102] << 32) |
                  ((uint64_t)identify[103] << 48);
    }
    if (!sectors) sectors = ((uint32_t)identify[61] << 16) | identify[60];
    return sectors > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sectors;
}

static int add_disk(uint8_t type, uint8_t channel, uint8_t slave,
                    uint32_t sectors, const char* model,
                    uint8_t backend, uint8_t backend_index, uint8_t ahci_port) {
    if (!sectors || disk_count_value >= VOIDOS_MAX_DISKS) return -1;
    unsigned int index = disk_count_value++;
    disks[index].present = 1;
    disks[index].type = type;
    disks[index].channel = channel;
    disks[index].slave = slave;
    disks[index].sectors = sectors;
    copy_bounded(disks[index].model, model, sizeof(disks[index].model));
    disk_backend[index] = backend;
    disk_backend_index[index] = backend_index;
    disk_ahci_port[index] = ahci_port;
    return (int)index;
}

/* ---- legacy ATA PIO -------------------------------------------------- */

static void io_delay(uint16_t ctrl) {
    (void)inb(ctrl); (void)inb(ctrl); (void)inb(ctrl); (void)inb(ctrl);
}

static uint8_t ata_wait_status(const struct ata_channel* channel) {
    uint8_t status = 0;
    for (uint32_t timeout = 0; timeout < 1000000; timeout++) {
        status = inb(channel->io + ATA_REG_STATUS);
        if (!(status & ATA_STATUS_BSY)) return status;
    }
    return status;
}

static int ata_select_drive(const struct ata_channel* channel, uint8_t slave,
                            uint32_t lba) {
    outb(channel->io + ATA_REG_DEVICE,
         (uint8_t)(0xE0 | (slave << 4) | ((lba >> 24) & 0x0F)));
    io_delay(channel->ctrl);
    uint8_t status = inb(channel->io + ATA_REG_STATUS);
    return status != 0xFF && status != 0;
}

static int identify_ata_drive(uint8_t channel_index, uint8_t slave) {
    const struct ata_channel* channel = &channels[channel_index];
    if (!ata_select_drive(channel, slave, 0)) return 0;
    outb(channel->io + ATA_REG_SECCOUNT, 0);
    outb(channel->io + ATA_REG_LBA0, 0);
    outb(channel->io + ATA_REG_LBA1, 0);
    outb(channel->io + ATA_REG_LBA2, 0);
    outb(channel->io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(channel->io + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) return 0;
    status = ata_wait_status(channel);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF) || !(status & ATA_STATUS_DRQ)) return 0;

    unsigned int slot = disk_count_value;
    uint16_t* identify = ahci_identify_data[slot];
    for (unsigned int i = 0; i < 256; i++) identify[i] = inw(channel->io + ATA_REG_DATA);
    uint32_t sectors = sectors_from_ata_identify(identify);
    if (!sectors) return 0;
    if (add_disk(VOID_DISK_ATA, channel_index, slave, sectors,
                 identify[27] ? "ATA disk" : "ATA disk",
                 VOID_DISK_ATA, 0, 0) < 0) return 0;
    model_from_identify(disks[slot].model, identify);
    disk_channels[slot] = *channel;
    return 1;
}

static int ata_read_sector(unsigned int index, uint32_t lba, uint8_t* buffer) {
    const struct ata_channel* channel = &disk_channels[index];
    if (!ata_select_drive(channel, disks[index].slave, lba)) return -1;
    outb(channel->io + ATA_REG_SECCOUNT, 1);
    outb(channel->io + ATA_REG_LBA0, (uint8_t)lba);
    outb(channel->io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(channel->io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outb(channel->io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    uint8_t status = ata_wait_status(channel);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF) || !(status & ATA_STATUS_DRQ)) return -1;
    for (unsigned int i = 0; i < 256; i++) {
        uint16_t word = inw(channel->io + ATA_REG_DATA);
        buffer[i * 2] = (uint8_t)word;
        buffer[i * 2 + 1] = (uint8_t)(word >> 8);
    }
    return 0;
}

static int ata_write_sector(unsigned int index, uint32_t lba, const uint8_t* buffer) {
    const struct ata_channel* channel = &disk_channels[index];
    if (!ata_select_drive(channel, disks[index].slave, lba)) return -1;
    outb(channel->io + ATA_REG_SECCOUNT, 1);
    outb(channel->io + ATA_REG_LBA0, (uint8_t)lba);
    outb(channel->io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(channel->io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outb(channel->io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    uint8_t status = ata_wait_status(channel);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF) || !(status & ATA_STATUS_DRQ)) return -1;
    for (unsigned int i = 0; i < 256; i++) {
        uint16_t word = (uint16_t)buffer[i * 2] |
                        ((uint16_t)buffer[i * 2 + 1] << 8);
        outw(channel->io + ATA_REG_DATA, word);
    }
    outb(channel->io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return (ata_wait_status(channel) & (ATA_STATUS_ERR | ATA_STATUS_DF)) ? -1 : 0;
}

/* ---- AHCI / SATA ----------------------------------------------------- */

static void ahci_stop_engine(volatile struct ahci_port_regs* port) {
    port->cmd &= ~1u; /* ST */
    for (uint32_t timeout = 0; timeout < 1000000 && (port->cmd & (1u << 15)); timeout++) {}
    port->cmd &= ~(1u << 4); /* FRE */
    for (uint32_t timeout = 0; timeout < 1000000 && (port->cmd & (1u << 14)); timeout++) {}
}

static int ahci_wait_command(volatile struct ahci_port_regs* port) {
    for (uint32_t timeout = 0; timeout < 1000000; timeout++) {
        uint32_t task = port->tfd;
        if (task & (ATA_STATUS_ERR | ATA_STATUS_DF)) return -1;
        if (!(port->ci & 1u)) return 0;
    }
    return -1;
}

static int ahci_prepare_port(uint8_t port_index, volatile struct ahci_port_regs* port) {
    if (port_index >= VOIDOS_MAX_DISKS) return -1;
    ahci_stop_engine(port);
    clear_bytes(ahci_clb[port_index], 1024);
    clear_bytes(ahci_fis[port_index], 256);
    clear_bytes((volatile uint8_t*)&ahci_tables[port_index], sizeof(ahci_tables[port_index]));
    port->clb = (uint32_t)(uintptr_t)ahci_clb[port_index];
    port->clbu = 0;
    port->fb = (uint32_t)(uintptr_t)ahci_fis[port_index];
    port->fbu = 0;
    port->serr = 0xFFFFFFFFu;
    port->is = 0xFFFFFFFFu;
    port->ie = 0;
    port->cmd |= (1u << 4) | 1u; /* FRE + ST */
    return 0;
}

static int ahci_identify_port(uint8_t port_index, volatile struct ahci_port_regs* port) {
    if (ahci_prepare_port(port_index, port) != 0) return -1;
    volatile struct ahci_cmd_header* headers =
        (volatile struct ahci_cmd_header*)(uintptr_t)port->clb;
    volatile struct ahci_cmd_table* table = &ahci_tables[port_index];
    headers[0].flags = 5;
    headers[0].prdtl = 1;
    headers[0].ctba = (uint32_t)(uintptr_t)table;
    table->cfis[0] = 0x27;
    table->cfis[1] = 0;
    table->cfis[2] = ATA_CMD_IDENTIFY;
    table->cfis[7] = 0xA0;
    table->prdt[0].dba = (uint32_t)(uintptr_t)ahci_identify_data[port_index];
    table->prdt[0].dbau = 0;
    table->prdt[0].dbc = VOIDOS_DISK_SECTOR_SIZE - 1;
    memory_barrier();
    port->ci = 1;
    if (ahci_wait_command(port) != 0) return -1;
    return 0;
}

static int ahci_io_sector(unsigned int index, uint32_t lba, uint8_t* buffer,
                          int write) {
    volatile struct ahci_port_regs* port = disk_ahci_ports[index];
    uint8_t port_index = disk_ahci_port[index];
    volatile struct ahci_cmd_header* headers =
        (volatile struct ahci_cmd_header*)(uintptr_t)port->clb;
    volatile struct ahci_cmd_table* table = &ahci_tables[port_index];
    clear_bytes((volatile uint8_t*)table, sizeof(*table));
    headers[0].flags = (uint16_t)(5 | (write ? (1u << 6) : 0));
    headers[0].prdtl = 1;
    headers[0].prdbc = 0;
    table->cfis[0] = 0x27;
    table->cfis[1] = 0;
    table->cfis[2] = write ? 0x35 : 0x25;
    table->cfis[7] = 0x40;
    table->cfis[4] = (uint8_t)lba;
    table->cfis[5] = (uint8_t)(lba >> 8);
    table->cfis[6] = (uint8_t)(lba >> 16);
    table->cfis[8] = (uint8_t)(lba >> 24);
    table->cfis[9] = 0;
    table->cfis[10] = 0;
    table->cfis[11] = 0;
    table->cfis[12] = 1;
    table->cfis[13] = 0;
    table->prdt[0].dba = (uint32_t)(uintptr_t)buffer;
    table->prdt[0].dbau = 0;
    table->prdt[0].dbc = VOIDOS_DISK_SECTOR_SIZE - 1;
    memory_barrier();
    port->is = 0xFFFFFFFFu;
    port->ci = 1;
    return ahci_wait_command(port);
}

static int ahci_probe_controller(uint8_t bus, uint8_t dev, uint8_t func) {
    int memory = 0;
    uint64_t abar = pci_bar(bus, dev, func, 5, &memory);
    if (!memory || !abar || (abar >> 32)) return 0;
    volatile uint8_t* base = (volatile uint8_t*)(uintptr_t)abar;
    volatile uint32_t* pi_reg = (volatile uint32_t*)(base + 0x0C);
    uint32_t implemented = *pi_reg;
    int found = 0;
    storage_controller_count++;
    for (uint8_t port_index = 0; port_index < AHCI_MAX_PORTS &&
                                  disk_count_value < VOIDOS_MAX_DISKS; port_index++) {
        if (!(implemented & (1u << port_index))) continue;
        volatile struct ahci_port_regs* port =
            (volatile struct ahci_port_regs*)(base + 0x100 + port_index * 0x80);
        uint32_t ssts = port->ssts;
        uint32_t signature = port->sig;
        uint8_t det = (uint8_t)(ssts & 0x0F);
        uint8_t ipm = (uint8_t)((ssts >> 8) & 0x0F);
        if (det != AHCI_PORT_DET_PRESENT || ipm != AHCI_PORT_IPM_ACTIVE ||
            signature != AHCI_SIG_ATA) continue;
        unsigned int slot = disk_count_value;
        if (ahci_identify_port((uint8_t)slot, port) != 0) continue;
        uint32_t sectors = sectors_from_ata_identify(ahci_identify_data[slot]);
        if (!sectors) continue;
        if (add_disk(VOID_DISK_AHCI, bus, func, sectors, "SATA disk",
                     VOID_DISK_AHCI, 0, port_index) >= 0) {
            disk_ahci_ports[slot] = port;
            model_from_identify(disks[slot].model, ahci_identify_data[slot]);
            found = 1;
        }
    }
    return found;
}

/* ---- NVMe ------------------------------------------------------------ */

static uint32_t nvme_read32(const struct nvme_state* state, uint32_t offset) {
    return *(volatile uint32_t*)(uintptr_t)(state->mmio + offset);
}

static uint64_t nvme_read64(const struct nvme_state* state, uint32_t offset) {
    uint32_t lo = nvme_read32(state, offset);
    uint32_t hi = nvme_read32(state, offset + 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static void nvme_write32(const struct nvme_state* state, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(uintptr_t)(state->mmio + offset) = value;
}

static void nvme_write64(const struct nvme_state* state, uint32_t offset, uint64_t value) {
    nvme_write32(state, offset, (uint32_t)value);
    nvme_write32(state, offset + 4, (uint32_t)(value >> 32));
}

static int nvme_wait_ready(const struct nvme_state* state, int ready) {
    for (uint32_t timeout = 0; timeout < 1000000; timeout++) {
        uint32_t csts = nvme_read32(state, 0x1C);
        if (csts & (1u << 1)) return -1; /* controller fatal status */
        if (((csts & 1u) != 0) == (ready != 0)) return 0;
    }
    return -1;
}

static int nvme_wait_completion(struct nvme_state* state, int admin,
                                uint16_t cid) {
    unsigned int controller = (unsigned int)(state - nvme_controllers);
    uint16_t* head = admin ? &state->admin_cq_head : &state->io_cq_head;
    uint8_t* phase = admin ? &state->admin_phase : &state->io_phase;
    volatile struct nvme_completion* cpl;
    for (uint32_t timeout = 0; timeout < 1000000; timeout++) {
        cpl = admin ? &nvme_admin_cq[controller][*head]
                    : &nvme_io_cq[controller][*head];
        uint16_t status = cpl->status;
        if ((status & 1) != *phase) continue;
        int success = cpl->command_id == cid && ((status >> 1) & 0xFF) == 0;
        *head = (uint16_t)((*head + 1) % state->queue_depth);
        if (*head == 0) *phase ^= 1;
        return success ? 0 : -1;
    }
    return -1;
}

static int nvme_submit_admin(struct nvme_state* state, struct nvme_command command) {
    unsigned int controller = (unsigned int)(state - nvme_controllers);
    uint16_t cid = state->next_cid++;
    command.cdw0 = (command.cdw0 & 0xFFFF0000u) | cid << 16;
    nvme_admin_sq[controller][state->admin_sq_tail] = command;
    state->admin_sq_tail = (uint16_t)((state->admin_sq_tail + 1) % state->queue_depth);
    memory_barrier();
    nvme_write32(state, 0x1000, state->admin_sq_tail);
    return nvme_wait_completion(state, 1, cid);
}

static int nvme_submit_io(struct nvme_state* state, struct nvme_command command) {
    unsigned int controller = (unsigned int)(state - nvme_controllers);
    uint16_t cid = state->next_cid++;
    command.cdw0 = (command.cdw0 & 0xFFFF0000u) | cid << 16;
    nvme_io_sq[controller][state->io_sq_tail] = command;
    state->io_sq_tail = (uint16_t)((state->io_sq_tail + 1) % state->queue_depth);
    memory_barrier();
    uint32_t stride = state->doorbell_stride;
    nvme_write32(state, 0x1000 + 2 * stride, state->io_sq_tail);
    return nvme_wait_completion(state, 0, cid);
}

static int nvme_create_io_queues(struct nvme_state* state) {
    unsigned int controller = (unsigned int)(state - nvme_controllers);
    struct nvme_command command = {0};
    command.cdw0 = 0x05;
    command.prp1 = (uint64_t)(uintptr_t)nvme_io_cq[controller];
    command.cdw10 = (state->queue_depth - 1) | (1u << 16);
    command.cdw11 = 1; /* physically contiguous, interrupts disabled */
    if (nvme_submit_admin(state, command) != 0) return -1;

    command = (struct nvme_command){0};
    command.cdw0 = 0x01;
    command.prp1 = (uint64_t)(uintptr_t)nvme_io_sq[controller];
    command.cdw10 = (state->queue_depth - 1) | (1u << 16);
    command.cdw11 = 1 | (1u << 16); /* contiguous, completion queue 1 */
    return nvme_submit_admin(state, command);
}

static int nvme_identify(struct nvme_state* state, unsigned int controller) {
    struct nvme_command command = {0};
    command.cdw0 = 0x06;
    command.prp1 = (uint64_t)(uintptr_t)nvme_identify_controller[controller];
    command.cdw10 = 1; /* identify controller */
    if (nvme_submit_admin(state, command) != 0) return -1;

    command = (struct nvme_command){0};
    command.cdw0 = 0x06;
    command.nsid = 1;
    command.prp1 = (uint64_t)(uintptr_t)nvme_identify_namespace[controller];
    command.cdw10 = 0; /* identify namespace */
    if (nvme_submit_admin(state, command) != 0) return -1;

    uint64_t sectors64 = read_le64(nvme_identify_namespace[controller]);
    uint8_t flbas = nvme_identify_namespace[controller][26] & 0x0F;
    uint32_t lbaf_offset = 128 + (uint32_t)flbas * 4;
    uint8_t lbads = nvme_identify_namespace[controller][lbaf_offset + 2];
    if (!sectors64 || lbads != 9) return -1; /* VoidFS uses 512-byte sectors */
    state->sectors = sectors64 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sectors64;
    model_from_bytes(state->model, nvme_identify_controller[controller], 24, 40);
    return 0;
}

static int nvme_probe_controller(uint8_t bus, uint8_t dev, uint8_t func) {
    if (nvme_controller_count >= NVME_MAX_CONTROLLERS) return 0;
    int memory = 0;
    uint64_t bar = pci_bar(bus, dev, func, 0, &memory);
    if (!memory || !bar || (bar >> 32)) return 0;
    unsigned int controller = nvme_controller_count++;
    struct nvme_state* state = &nvme_controllers[controller];
    *state = (struct nvme_state){0};
    state->mmio = (uint32_t)bar;
    uint64_t cap = nvme_read64(state, 0);
    state->queue_depth = (uint16_t)((cap & 0xFFFF) + 1);
    if (state->queue_depth > NVME_QUEUE_DEPTH) state->queue_depth = NVME_QUEUE_DEPTH;
    if (state->queue_depth < 2) return 0;
    state->doorbell_stride = 4u << ((uint32_t)(cap >> 32) & 0x0F);
    state->next_cid = 1;
    state->admin_phase = 1;
    state->io_phase = 1;
    state->admin_sq_tail = 0;
    state->admin_cq_head = 0;
    clear_bytes((volatile uint8_t*)nvme_admin_sq[controller], sizeof(nvme_admin_sq[controller]));
    clear_bytes((volatile uint8_t*)nvme_admin_cq[controller], sizeof(nvme_admin_cq[controller]));
    clear_bytes((volatile uint8_t*)nvme_io_sq[controller], sizeof(nvme_io_sq[controller]));
    clear_bytes((volatile uint8_t*)nvme_io_cq[controller], sizeof(nvme_io_cq[controller]));
    uint32_t cc = nvme_read32(state, 0x14);
    if (cc & 1) {
        nvme_write32(state, 0x14, cc & ~1u);
        if (nvme_wait_ready(state, 0) != 0) return 0;
    }
    nvme_write32(state, 0x24, (state->queue_depth - 1) |
                                ((uint32_t)(state->queue_depth - 1) << 16));
    nvme_write64(state, 0x28, (uint64_t)(uintptr_t)nvme_admin_sq[controller]);
    nvme_write64(state, 0x30, (uint64_t)(uintptr_t)nvme_admin_cq[controller]);
    nvme_write32(state, 0x14, (6u << 16) | (4u << 20) | 1u);
    if (nvme_wait_ready(state, 1) != 0) return 0;
    if (nvme_identify(state, controller) != 0) return 0;
    if (nvme_create_io_queues(state) != 0) return 0;
    state->io_ready = 1;
    storage_controller_count++;
    if (add_disk(VOID_DISK_NVME, bus, func, state->sectors, state->model,
                 VOID_DISK_NVME, (uint8_t)controller, 0) < 0) return 0;
    return 1;
}

static int nvme_io_sector(unsigned int index, uint32_t lba, uint8_t* buffer,
                          int write) {
    struct nvme_state* state = &nvme_controllers[disk_backend_index[index]];
    unsigned int controller = (unsigned int)(state - nvme_controllers);
    for (unsigned int i = 0; i < VOIDOS_DISK_SECTOR_SIZE; i++)
        nvme_io_buffer[controller][i] = write ? buffer[i] : 0;
    struct nvme_command command = {0};
    command.cdw0 = write ? 0x01 : 0x02;
    command.nsid = state->namespace_id ? state->namespace_id : 1;
    command.prp1 = (uint64_t)(uintptr_t)nvme_io_buffer[controller];
    command.cdw10 = lba;
    command.cdw12 = 0; /* one logical block */
    if (nvme_submit_io(state, command) != 0) return -1;
    if (!write) {
        for (unsigned int i = 0; i < VOIDOS_DISK_SECTOR_SIZE; i++)
            buffer[i] = nvme_io_buffer[controller][i];
    }
    return 0;
}

/* ---- public disk API ------------------------------------------------- */

const char* disk_type_name(uint8_t type) {
    if (type == VOID_DISK_AHCI) return "SATA/AHCI";
    if (type == VOID_DISK_NVME) return "NVMe";
    return "ATA PIO";
}

void disk_initialize(void) {
    disk_count_value = 0;
    nvme_controller_count = 0;
    storage_controller_count = 0;
    for (unsigned int i = 0; i < VOIDOS_MAX_DISKS; i++) {
        disks[i] = (struct void_disk_info){0};
        disk_backend[i] = 0;
        disk_backend_index[i] = 0;
        disk_ahci_port[i] = 0;
        disk_ahci_ports[i] = (volatile struct ahci_port_regs*)0;
    }

    for (uint8_t channel = 0; channel < 2; channel++) {
        for (uint8_t slave = 0; slave < 2; slave++) {
            if (disk_count_value >= VOIDOS_MAX_DISKS) break;
            (void)identify_ata_drive(channel, slave);
        }
    }

    for (uint16_t bus = 0; bus < 256 && disk_count_value < VOIDOS_MAX_DISKS; bus++) {
        for (uint8_t dev = 0; dev < 32 && disk_count_value < VOIDOS_MAX_DISKS; dev++) {
            if (pci_vendor((uint8_t)bus, dev, 0) == 0xFFFF) continue;
            uint8_t funcs = (pci_header_type((uint8_t)bus, dev, 0) & 0x80) ? 8 : 1;
            for (uint8_t func = 0; func < funcs && disk_count_value < VOIDOS_MAX_DISKS; func++) {
                uint8_t b = (uint8_t)bus;
                if (pci_vendor(b, dev, func) == 0xFFFF) continue;
                if (pci_class(b, dev, func) != 0x01) continue;
                uint8_t subclass = pci_subclass(b, dev, func);
                if (subclass == 0x06 && pci_prog_if(b, dev, func) == 0x01) {
                    (void)ahci_probe_controller(b, dev, func);
                } else if (subclass == 0x08) {
                    (void)nvme_probe_controller(b, dev, func);
                }
            }
        }
    }
}

unsigned int disk_count(void) {
    return disk_count_value;
}

const struct void_disk_info* disk_at(unsigned int index) {
    return index < disk_count_value ? &disks[index] : (const struct void_disk_info*)0;
}

int disk_read_sector(unsigned int index, uint32_t lba, uint8_t* buffer) {
    if (index >= disk_count_value || !buffer || lba >= disks[index].sectors) return -1;
    if (disk_backend[index] == VOID_DISK_ATA) return ata_read_sector(index, lba, buffer);
    if (disk_backend[index] == VOID_DISK_AHCI) return ahci_io_sector(index, lba, buffer, 0);
    if (disk_backend[index] == VOID_DISK_NVME) return nvme_io_sector(index, lba, buffer, 0);
    return -1;
}

int disk_write_sector(unsigned int index, uint32_t lba, const uint8_t* buffer) {
    if (index >= disk_count_value || !buffer || lba >= disks[index].sectors) return -1;
    if (disk_backend[index] == VOID_DISK_ATA)
        return ata_write_sector(index, lba, buffer);
    if (disk_backend[index] == VOID_DISK_AHCI)
        return ahci_io_sector(index, lba, (uint8_t*)buffer, 1);
    if (disk_backend[index] == VOID_DISK_NVME)
        return nvme_io_sector(index, lba, (uint8_t*)buffer, 1);
    return -1;
}

void disk_print_info(void) {
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_DARK_GREY);
    terminal_writestring("Detected storage devices\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    if (!disk_count_value) {
        terminal_writestring("  No usable ATA, SATA/AHCI, or NVMe disk detected.\n");
        terminal_writestring("  PCI storage controllers scanned: ");
        terminal_write_uint(storage_controller_count);
        terminal_writestring("\n");
        return;
    }
    for (unsigned int i = 0; i < disk_count_value; i++) {
        terminal_writestring("  Drive ");
        terminal_write_uint(i);
        terminal_writestring(" [");
        terminal_writestring(disk_type_name(disks[i].type));
        terminal_writestring("]: ");
        terminal_writestring(disks[i].model[0] ? disks[i].model : "storage device");
        terminal_writestring("  ");
        terminal_write_uint(disks[i].sectors);
        terminal_writestring(" sectors\n");
    }
}