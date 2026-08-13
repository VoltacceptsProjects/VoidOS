#ifndef VOIDOS_FS_H
#define VOIDOS_FS_H

#include <stdint.h>
#include <stddef.h>

struct multiboot_info;

#define VOIDFS_MAX_FILES 24
#define VOIDFS_NAME_MAX 48
#define VOIDFS_MIME_MAX 40
#define VOIDFS_STORAGE_SIZE (128 * 1024)
#define VOIDFS_VAPP_MIME "application/x-voidos-app"

/* A .vapp is a self-describing package. The payload is intentionally opaque
 * to VoidFS: the application runtime can decide how to execute it later. */
struct vapp_header {
    char magic[4];              /* "VAPP" */
    uint16_t format_version;    /* currently 1 */
    uint16_t header_size;
    uint32_t package_size;
    uint32_t manifest_offset;
    uint32_t manifest_size;
    uint32_t payload_offset;
    uint32_t payload_size;
    char mime[VOIDFS_MIME_MAX];
    char name[VOIDFS_NAME_MAX];
    char app_version[16];
} __attribute__((packed));

struct voidfs_file {
    char name[VOIDFS_NAME_MAX];
    char mime[VOIDFS_MIME_MAX];
    uint32_t size;
    uint32_t storage_offset;
    uint32_t disk_start_lba;
    uint32_t disk_sectors;
};

/* Detects ATA, SATA/AHCI, and NVMe drives, mounts the first VoidFS volume it
 * finds, and otherwise creates an empty RAM-backed volume. */
void voidfs_initialize(void);

/* Installs one complete .vapp package into the volume.
 * Returns 0 on success, or a negative error code on validation/storage
 * failure. The package bytes are copied, so the caller may release them. */
int voidfs_install_vapp(const uint8_t* package, uint32_t package_size,
                        const char* source_name);

/* Imports every Multiboot module whose name ends in ".vapp". This is the
 * boot-time installation path until a block-device driver is available. */
void voidfs_install_multiboot_modules(struct multiboot_info* mbi);

/* Explicitly formats a drive as VoidFS. This is destructive and is not
 * called automatically. */
int voidfs_format_drive(unsigned int disk_index);
int voidfs_is_persistent(void);
unsigned int voidfs_backing_disk(void);

unsigned int voidfs_file_count(void);
const struct voidfs_file* voidfs_file_at(unsigned int index);

/* Reads an installed file into caller-owned memory. */
int voidfs_read_file(unsigned int index, uint32_t offset,
                     uint8_t* out, uint32_t length);

/* Writes a human-readable directory listing into the terminal scrollback. */
void voidfs_print_files(void);

#endif