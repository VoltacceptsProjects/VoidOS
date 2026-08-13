#ifndef IWLWIFI_H
#define IWLWIFI_H

#include <stdint.h>
#include "multiboot.h"

/* Stage 3: Intel Wireless-AC 9260 firmware load / NIC bring-up.
 *
 * Scope of what this file actually does, honestly stated:
 *
 *   1. Finds the .ucode firmware image among the Multiboot modules
 *      GRUB handed us (same mechanism the .vapp packages use - see
 *      voidfs_install_multiboot_modules() in fs.c).
 *   2. Parses the real iwlwifi TLV firmware container format and logs
 *      every section it finds over serial, so you can confirm the
 *      image you baked into the ISO is intact and being read
 *      correctly. This part is fully exercised by the code as
 *      written - the TLV walker below was validated byte-for-byte
 *      against a real iwlwifi-9260 firmware file before being ported
 *      in here (it consumes the file to the exact last byte and finds
 *      the known CPU1/CPU2 and paging separator markers).
 *   3. Does the first step of real hardware bring-up: pulses
 *      CSR_RESET and waits for CSR_GP_CNTRL to report the MAC clock
 *      is running. That's as far as this goes.
 *
 * What this file does NOT do: transfer the parsed firmware sections
 * into the device's SRAM over the FH DMA engine, wait for the
 * ALIVE notification, bring up TX/RX queues, or anything resembling
 * an 802.11 MAC (scanning, auth, association, encryption, a data
 * path). That is the bulk of what a real iwlwifi driver is, it's
 * thousands of lines even in Linux, and none of it can be verified
 * without real 9260 hardware in front of a serial cable - so rather
 * than hand you plausible-looking register pokes for a stage no one
 * has tested, this stops at a verifiable checkpoint and logs clearly
 * where it stopped. The OSDev wiki's "Iwlwifi" and "Intel Dual Band
 * Wireless" pages are the best next reference for continuing past
 * this point; even that community writeup is explicitly WIP for the
 * firmware-load and NIC-init steps, which tells you something about
 * how much hardware-in-hand iteration those steps need.
 */

#define IWLWIFI_MAX_SECTIONS 32

struct iwlwifi_fw_section {
    uint32_t offset;        /* load-address hint from the TLV, or a
                                separator marker (see is_separator) */
    const uint8_t* data;    /* pointer into the firmware image */
    uint32_t length;        /* payload length, not counting the
                                4-byte offset prefix */
    int is_init;            /* 1 = SEC_INIT (type 20), 0 = SEC_RT (type 19) */
    int is_separator;       /* 1 if offset is a marker (0xFFFFCCCC /
                                0xAAAABBBB) rather than a real address */
};

struct iwlwifi_fw_image {
    char human_readable[65];
    uint32_t ver_major;
    uint32_t ver_build_id;
    struct iwlwifi_fw_section sections[IWLWIFI_MAX_SECTIONS];
    uint32_t num_sections;
    uint32_t total_tlvs;
};

/* Scans Multiboot modules for a file whose name ends in ".ucode".
 * Returns 1 and fills out_data/out_size if found, 0 otherwise. */
int iwlwifi_find_firmware_module(const struct multiboot_info* mbi,
                                  const uint8_t** out_data, uint32_t* out_size);

/* Parses the TLV container format and logs a summary + every
 * SEC_RT/SEC_INIT section over serial. Returns 1 on a structurally
 * valid image, 0 if the magic/header didn't match. */
int iwlwifi_parse_firmware(const uint8_t* data, uint32_t size,
                            struct iwlwifi_fw_image* out);

/* Best-effort, unverified-on-hardware NIC wake-up: pulses CSR_RESET,
 * sets the INIT_DONE bit in CSR_GP_CNTRL, and polls for
 * MAC_CLOCK_READY. Logs every step and the final CSR register dump
 * over serial regardless of outcome. Returns 1 if the clock came up
 * within the timeout, 0 otherwise. bar0 must be the physical address
 * of the 9260's BAR0 MMIO window (see pci_probe_network_devices()). */
int iwlwifi_bringup(uint32_t bar0);

#endif
