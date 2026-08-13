#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* Generic ACPI System Description Table header, common to every table
 * (RSDT/XSDT/FADT/DSDT/SSDT/...). See ACPI spec 5.2.6. */
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;       /* total table length, including this header */
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* Locates the RSDP (by scanning the EBDA and the 0xE0000-0xFFFFF BIOS
 * area), then validates and walks the RSDT/XSDT to find and checksum
 * the FADT. Cheap - safe to call more than once. Must be called before
 * any other acpi_* function. */
void acpi_init(void);

/* Non-zero once acpi_init() has found a checksummed RSDP + root table. */
int acpi_available(void);

/* Walks the RSDT/XSDT looking for a table whose 4-byte signature
 * matches (e.g. "FACP", "SSDT", "APIC"). Returns NULL if not present
 * or if its checksum doesn't validate. This does NOT find the DSDT -
 * that's reached only through the FADT, use acpi_get_dsdt(). */
struct acpi_sdt_header* acpi_find_table(const char* signature);

/* The one DSDT, reached via the FADT's DSDT/X_DSDT field. NULL if ACPI
 * isn't available or the FADT/DSDT don't checksum. */
struct acpi_sdt_header* acpi_get_dsdt(void);

/* Iterates SSDT tables listed directly in the RSDT/XSDT (this misses
 * any SSDT only reachable via a dynamic AML Load() op - out of scope
 * for this minimal implementation). Pass NULL to get the first one,
 * then feed back the previous return value to get the next; NULL when
 * there are no more. */
struct acpi_sdt_header* acpi_next_ssdt(struct acpi_sdt_header* prev);

#endif
