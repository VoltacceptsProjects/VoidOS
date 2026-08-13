#include "fs.h"
#include "disk.h"
#include "multiboot.h"
#include "vga.h"

#define VOIDFS_START_LBA 2048
#define VOIDFS_DIRECTORY_SECTORS 8
#define VOIDFS_FIRST_DATA_LBA (VOIDFS_START_LBA + 1 + VOIDFS_DIRECTORY_SECTORS)
#define VOIDFS_DISK_MAGIC "VOIDFS1"
#define VOIDFS_DISK_VERSION 1
#define VOIDFS_MAX_PACKAGE_SIZE (8 * 1024 * 1024)

struct voidfs_superblock {
    char magic[8];
    uint32_t version;
    uint32_t disk_index;
    uint32_t directory_lba;
    uint32_t directory_sectors;
    uint32_t max_files;
    uint32_t first_data_lba;
    uint32_t total_sectors;
    uint32_t next_free_lba;
    uint8_t reserved[472];
} __attribute__((packed));

struct voidfs_disk_entry {
    uint8_t used;
    uint8_t reserved0[3];
    uint32_t size;
    uint32_t start_lba;
    uint32_t sectors;
    char name[VOIDFS_NAME_MAX];
    char mime[VOIDFS_MIME_MAX];
    uint8_t reserved[24];
} __attribute__((packed));

static uint8_t storage[VOIDFS_STORAGE_SIZE];
static struct voidfs_file directory[VOIDFS_MAX_FILES];
static unsigned int directory_count = 0;
static uint32_t storage_used = 0;
static int persistent_volume = 0;
static unsigned int persistent_disk = 0;
static uint32_t disk_next_lba = 0;

enum {
    VOIDFS_OK = 0,
    VOIDFS_BAD_PACKAGE = -1,
    VOIDFS_WRONG_MIME = -2,
    VOIDFS_NOT_VAPP = -3,
    VOIDFS_NO_SPACE = -4,
    VOIDFS_DIRECTORY_FULL = -5,
    VOIDFS_DISK_ERROR = -6,
};

static unsigned int text_length(const char* text, unsigned int cap) {
    unsigned int n = 0;
    while (n < cap && text[n]) n++;
    return n;
}

static int text_equal(const char* a, const char* b, unsigned int cap) {
    unsigned int i = 0;
    while (i < cap && a[i] && b[i] && a[i] == b[i]) i++;
    return i < cap && a[i] == '\0' && b[i] == '\0';
}

static int has_vapp_suffix(const char* name, unsigned int cap) {
    unsigned int length = text_length(name, cap);
    return length >= 5 &&
           name[length - 5] == '.' &&
           name[length - 4] == 'v' &&
           name[length - 3] == 'a' &&
           name[length - 2] == 'p' &&
           name[length - 1] == 'p';
}

static const char* basename(const char* path) {
    const char* last = path;
    if (!path) return "";
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

static void copy_bounded(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    if (cap == 0) return;
    while (i + 1 < cap && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void clear_directory(void) {
    directory_count = 0;
    storage_used = 0;
    for (unsigned int i = 0; i < VOIDFS_MAX_FILES; i++) {
        directory[i].name[0] = '\0';
        directory[i].mime[0] = '\0';
        directory[i].size = 0;
        directory[i].storage_offset = 0;
        directory[i].disk_start_lba = 0;
        directory[i].disk_sectors = 0;
    }
}

static void clear_bytes(uint8_t* bytes, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) bytes[i] = 0;
}

static void make_superblock(struct voidfs_superblock* super,
                            unsigned int disk_index,
                            uint32_t total_sectors) {
    clear_bytes((uint8_t*)super, sizeof(*super));
    copy_bounded(super->magic, VOIDFS_DISK_MAGIC, sizeof(super->magic));
    super->version = VOIDFS_DISK_VERSION;
    super->disk_index = disk_index;
    super->directory_lba = VOIDFS_START_LBA + 1;
    super->directory_sectors = VOIDFS_DIRECTORY_SECTORS;
    super->max_files = VOIDFS_MAX_FILES;
    super->first_data_lba = VOIDFS_FIRST_DATA_LBA;
    super->total_sectors = total_sectors;
    super->next_free_lba = VOIDFS_FIRST_DATA_LBA;
}

static int persist_superblock(void) {
    struct voidfs_superblock super;
    const struct void_disk_info* info = disk_at(persistent_disk);
    if (!info) return VOIDFS_DISK_ERROR;
    make_superblock(&super, persistent_disk, info->sectors);
    super.next_free_lba = disk_next_lba;
    return disk_write_sector(persistent_disk, VOIDFS_START_LBA,
                             (const uint8_t*)&super) == 0 ? VOIDFS_OK : VOIDFS_DISK_ERROR;
}

static int persist_directory(void) {
    uint8_t sector[VOIDOS_DISK_SECTOR_SIZE];
    for (unsigned int block = 0; block < VOIDFS_DIRECTORY_SECTORS; block++) {
        clear_bytes(sector, sizeof(sector));
        for (unsigned int slot = 0; slot < 4; slot++) {
            unsigned int index = block * 4 + slot;
            if (index >= VOIDFS_MAX_FILES) break;
            struct voidfs_disk_entry* entry =
                (struct voidfs_disk_entry*)(void*)(sector + slot * sizeof(struct voidfs_disk_entry));
            if (index >= directory_count) continue;
            entry->used = 1;
            entry->size = directory[index].size;
            entry->start_lba = directory[index].disk_start_lba;
            entry->sectors = directory[index].disk_sectors;
            copy_bounded(entry->name, directory[index].name, sizeof(entry->name));
            copy_bounded(entry->mime, directory[index].mime, sizeof(entry->mime));
        }
        if (disk_write_sector(persistent_disk, VOIDFS_START_LBA + 1 + block, sector) != 0) {
            return VOIDFS_DISK_ERROR;
        }
    }
    return VOIDFS_OK;
}

static int persist_metadata(void) {
    if (persist_superblock() != VOIDFS_OK) return VOIDFS_DISK_ERROR;
    return persist_directory();
}

static int load_directory(uint32_t total_sectors) {
    uint8_t sector[VOIDOS_DISK_SECTOR_SIZE];
    clear_directory();
    for (unsigned int block = 0; block < VOIDFS_DIRECTORY_SECTORS; block++) {
        if (disk_read_sector(persistent_disk, VOIDFS_START_LBA + 1 + block, sector) != 0) {
            return VOIDFS_DISK_ERROR;
        }
        for (unsigned int slot = 0; slot < 4; slot++) {
            unsigned int index = block * 4 + slot;
            if (index >= VOIDFS_MAX_FILES) break;
            const struct voidfs_disk_entry* entry =
                (const struct voidfs_disk_entry*)(const void*)(sector + slot * sizeof(struct voidfs_disk_entry));
            if (!entry->used || directory_count >= VOIDFS_MAX_FILES) continue;
            if (!entry->sectors ||
                entry->start_lba < VOIDFS_FIRST_DATA_LBA ||
                entry->start_lba >= total_sectors ||
                entry->sectors > total_sectors - entry->start_lba ||
                entry->size > entry->sectors * VOIDOS_DISK_SECTOR_SIZE) {
                return VOIDFS_BAD_PACKAGE;
            }
            copy_bounded(directory[directory_count].name, entry->name, VOIDFS_NAME_MAX);
            copy_bounded(directory[directory_count].mime, entry->mime, VOIDFS_MIME_MAX);
            directory[directory_count].size = entry->size;
            directory[directory_count].storage_offset = 0;
            directory[directory_count].disk_start_lba = entry->start_lba;
            directory[directory_count].disk_sectors = entry->sectors;
            directory_count++;
        }
    }
    return VOIDFS_OK;
}

static int mount_disk(unsigned int disk_index) {
    struct voidfs_superblock super;
    const struct void_disk_info* info = disk_at(disk_index);
    if (!info || info->sectors <= VOIDFS_FIRST_DATA_LBA ||
        disk_read_sector(disk_index, VOIDFS_START_LBA, (uint8_t*)&super) != 0) {
        return VOIDFS_DISK_ERROR;
    }
    if (!text_equal(super.magic, VOIDFS_DISK_MAGIC, sizeof(super.magic)) ||
        super.version != VOIDFS_DISK_VERSION ||
        super.disk_index != disk_index ||
        super.directory_lba != VOIDFS_START_LBA + 1 ||
        super.directory_sectors != VOIDFS_DIRECTORY_SECTORS ||
        super.max_files != VOIDFS_MAX_FILES ||
        super.first_data_lba != VOIDFS_FIRST_DATA_LBA ||
        super.total_sectors > info->sectors ||
        super.next_free_lba < super.first_data_lba ||
        super.next_free_lba > info->sectors) {
        return VOIDFS_BAD_PACKAGE;
    }
    persistent_volume = 1;
    persistent_disk = disk_index;
    disk_next_lba = super.next_free_lba;
    if (load_directory(super.total_sectors) != VOIDFS_OK) {
        persistent_volume = 0;
        clear_directory();
        return VOIDFS_DISK_ERROR;
    }
    return VOIDFS_OK;
}

void voidfs_initialize(void) {
    persistent_volume = 0;
    persistent_disk = 0;
    disk_next_lba = 0;
    clear_directory();
    disk_initialize();
    for (unsigned int i = 0; i < disk_count(); i++) {
        if (mount_disk(i) == VOIDFS_OK) return;
    }
}

int voidfs_format_drive(unsigned int disk_index) {
    const struct void_disk_info* info = disk_at(disk_index);
    if (!info || info->sectors <= VOIDFS_FIRST_DATA_LBA) return VOIDFS_DISK_ERROR;

    persistent_volume = 1;
    persistent_disk = disk_index;
    disk_next_lba = VOIDFS_FIRST_DATA_LBA;
    clear_directory();

    struct voidfs_superblock super;
    make_superblock(&super, disk_index, info->sectors);
    if (disk_write_sector(disk_index, VOIDFS_START_LBA,
                          (const uint8_t*)&super) != 0) {
        persistent_volume = 0;
        return VOIDFS_DISK_ERROR;
    }
    uint8_t blank[VOIDOS_DISK_SECTOR_SIZE];
    clear_bytes(blank, sizeof(blank));
    for (unsigned int i = 0; i < VOIDFS_DIRECTORY_SECTORS; i++) {
        if (disk_write_sector(disk_index, VOIDFS_START_LBA + 1 + i, blank) != 0) {
            persistent_volume = 0;
            return VOIDFS_DISK_ERROR;
        }
    }
    return VOIDFS_OK;
}

int voidfs_is_persistent(void) {
    return persistent_volume;
}

unsigned int voidfs_backing_disk(void) {
    return persistent_disk;
}

static int install_to_disk(const uint8_t* package, uint32_t package_size,
                           int existing) {
    uint32_t sectors_needed =
        (package_size + VOIDOS_DISK_SECTOR_SIZE - 1) / VOIDOS_DISK_SECTOR_SIZE;
    const struct void_disk_info* info = disk_at(persistent_disk);
    if (!info || !sectors_needed) return VOIDFS_DISK_ERROR;

    uint32_t start_lba = disk_next_lba;
    if (existing >= 0 && directory[existing].disk_sectors >= sectors_needed) {
        start_lba = directory[existing].disk_start_lba;
    } else {
        if (sectors_needed > info->sectors - disk_next_lba) return VOIDFS_NO_SPACE;
        disk_next_lba += sectors_needed;
    }

    uint8_t sector[VOIDOS_DISK_SECTOR_SIZE];
    for (uint32_t block = 0; block < sectors_needed; block++) {
        clear_bytes(sector, sizeof(sector));
        uint32_t offset = block * VOIDOS_DISK_SECTOR_SIZE;
        uint32_t remaining = package_size - offset;
        uint32_t copy_size = remaining > VOIDOS_DISK_SECTOR_SIZE
                                 ? VOIDOS_DISK_SECTOR_SIZE : remaining;
        for (uint32_t i = 0; i < copy_size; i++) sector[i] = package[offset + i];
        if (disk_write_sector(persistent_disk, start_lba + block, sector) != 0) {
            return VOIDFS_DISK_ERROR;
        }
    }

    directory[existing].disk_start_lba = start_lba;
    directory[existing].disk_sectors = sectors_needed;
    directory[existing].storage_offset = 0;
    directory[existing].size = package_size;
    return persist_metadata();
}

int voidfs_install_vapp(const uint8_t* package, uint32_t package_size,
                        const char* source_name) {
    if (!package || package_size < sizeof(struct vapp_header) ||
        package_size > VOIDFS_MAX_PACKAGE_SIZE) {
        return VOIDFS_BAD_PACKAGE;
    }

    const struct vapp_header* header = (const struct vapp_header*)package;
    if (header->magic[0] != 'V' || header->magic[1] != 'A' ||
        header->magic[2] != 'P' || header->magic[3] != 'P') {
        return VOIDFS_NOT_VAPP;
    }
    if (header->format_version != 1 ||
        header->header_size < sizeof(struct vapp_header) ||
        header->header_size > package_size ||
        header->package_size != package_size) {
        return VOIDFS_BAD_PACKAGE;
    }
    if (!text_equal(header->mime, VOIDFS_VAPP_MIME, VOIDFS_MIME_MAX)) {
        return VOIDFS_WRONG_MIME;
    }
    if (header->manifest_offset > package_size ||
        header->manifest_size > package_size - header->manifest_offset ||
        header->payload_offset > package_size ||
        header->payload_size > package_size - header->payload_offset) {
        return VOIDFS_BAD_PACKAGE;
    }
    if (header->name[0] &&
        !has_vapp_suffix(header->name, VOIDFS_NAME_MAX)) {
        return VOIDFS_BAD_PACKAGE;
    }

    const char* package_name = header->name[0] ? header->name : basename(source_name);
    if (!has_vapp_suffix(package_name, header->name[0] ? VOIDFS_NAME_MAX : 256) ||
        text_length(package_name, VOIDFS_NAME_MAX) >= VOIDFS_NAME_MAX) {
        return VOIDFS_BAD_PACKAGE;
    }

    int existing = -1;
    for (unsigned int i = 0; i < directory_count; i++) {
        if (text_equal(directory[i].name, package_name, VOIDFS_NAME_MAX)) {
            existing = (int)i;
            break;
        }
    }
    int added = existing < 0;
    if (added && directory_count >= VOIDFS_MAX_FILES) return VOIDFS_DIRECTORY_FULL;
    if (added) {
        existing = (int)directory_count++;
    }

    if (persistent_volume) {
        copy_bounded(directory[existing].name, package_name, VOIDFS_NAME_MAX);
        copy_bounded(directory[existing].mime, header->mime, VOIDFS_MIME_MAX);
        int result = install_to_disk(package, package_size, existing);
        if (result != VOIDFS_OK && added) directory_count--;
        return result;
    }

    uint32_t offset = storage_used;
    if (existing >= 0 && directory[existing].size >= package_size &&
        directory[existing].storage_offset + directory[existing].size <= storage_used) {
        offset = directory[existing].storage_offset;
    } else if (package_size > VOIDFS_STORAGE_SIZE - storage_used) {
        if (added) directory_count--;
        return VOIDFS_NO_SPACE;
    }
    for (uint32_t i = 0; i < package_size; i++) storage[offset + i] = package[i];
    if (offset == storage_used) storage_used += package_size;
    copy_bounded(directory[existing].name, package_name, VOIDFS_NAME_MAX);
    copy_bounded(directory[existing].mime, header->mime, VOIDFS_MIME_MAX);
    directory[existing].size = package_size;
    directory[existing].storage_offset = offset;
    directory[existing].disk_start_lba = 0;
    directory[existing].disk_sectors = 0;
    return VOIDFS_OK;
}

void voidfs_install_multiboot_modules(struct multiboot_info* mbi) {
    if (!mbi || !mbi->mods_count || !mbi->mods_addr) return;
    struct multiboot_module* modules =
        (struct multiboot_module*)(uintptr_t)mbi->mods_addr;
    for (uint32_t i = 0; i < mbi->mods_count; i++) {
        const char* name = (const char*)(uintptr_t)modules[i].string;
        if (!name || !has_vapp_suffix(name, 256)) continue;
        const uint8_t* data = (const uint8_t*)(uintptr_t)modules[i].mod_start;
        uint32_t size = modules[i].mod_end - modules[i].mod_start;
        (void)voidfs_install_vapp(data, size, basename(name));
    }
}

unsigned int voidfs_file_count(void) {
    return directory_count;
}

const struct voidfs_file* voidfs_file_at(unsigned int index) {
    if (index >= directory_count) return (const struct voidfs_file*)0;
    return &directory[index];
}

int voidfs_read_file(unsigned int index, uint32_t offset,
                     uint8_t* out, uint32_t length) {
    if (index >= directory_count || !out) return -1;
    const struct voidfs_file* file = &directory[index];
    if (offset > file->size || length > file->size - offset) return -1;
    if (!persistent_volume) {
        for (uint32_t i = 0; i < length; i++) {
            out[i] = storage[file->storage_offset + offset + i];
        }
        return 0;
    }

    uint8_t sector[VOIDOS_DISK_SECTOR_SIZE];
    uint32_t copied = 0;
    while (copied < length) {
        uint32_t absolute = offset + copied;
        uint32_t block = absolute / VOIDOS_DISK_SECTOR_SIZE;
        uint32_t in_sector = absolute % VOIDOS_DISK_SECTOR_SIZE;
        uint32_t take = VOIDOS_DISK_SECTOR_SIZE - in_sector;
        if (take > length - copied) take = length - copied;
        if (block >= file->disk_sectors ||
            disk_read_sector(persistent_disk, file->disk_start_lba + block, sector) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < take; i++) out[copied + i] = sector[in_sector + i];
        copied += take;
    }
    return 0;
}

void voidfs_print_files(void) {
    terminal_setcolor(VGA_WHITE, VGA_DARK_GREY);
    terminal_writestring("VoidFS\n\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    if (persistent_volume) {
        terminal_writestring("Persistent ATA volume on Drive ");
        terminal_write_uint(persistent_disk);
        terminal_writestring("\n");
    } else {
        terminal_writestring("No VoidFS volume mounted; using RAM storage\n");
    }
    /* Show only the drive VoidOS is actually installed on (or, if not
     * installed yet, drive 0 - the boot/system disk) rather than every
     * ATA position the controller can see. A machine can have several
     * disks attached that have nothing to do with this OS. */
    disk_print_drive(persistent_volume ? persistent_disk : 0);
    terminal_writestring("\n");
    if (!directory_count) {
        terminal_setcolor(VGA_DARK_GREY, VGA_DARK_GREY);
        terminal_writestring("No applications installed.\n");
        terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
        return;
    }
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_DARK_GREY);
    terminal_writestring("Installed packages\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    for (unsigned int i = 0; i < directory_count; i++) {
        terminal_writestring("  ");
        terminal_writestring(directory[i].name);
        terminal_writestring("  ");
        terminal_writestring(directory[i].mime);
        terminal_writestring("  ");
        terminal_write_uint(directory[i].size);
        terminal_writestring(" bytes\n");
    }
}