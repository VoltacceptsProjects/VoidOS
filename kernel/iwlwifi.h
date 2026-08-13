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

/* --- firmware self-load (context-info method) --------------------------
 *
 * Correction to the note above: the 9260 is an "8000 family" part
 * (Thunder Peak MAC / Jefferson Peak RF). Those parts, unlike the
 * older 3160/7260-era chips, do NOT use the FH_TCSR/TFDIB per-chunk
 * DMA channel to push firmware into SRAM. They use a newer, simpler
 * "context info" self-load: the driver builds one struct in memory
 * describing where each firmware section lives (by physical address),
 * points a single CSR register at it, and the device DMAs everything
 * in itself. Confirmed by reading the current upstream iwlwifi source
 * (drivers/net/wireless/intel/iwlwifi/pcie/ctxt-info.c and
 * iwl-context-info.h) - the struct layout and CSR_CTXT_INFO_BA offset
 * below are taken from there, not guessed.
 *
 * What iwlwifi_load_firmware() actually does:
 *   1. Splits the already-parsed IWL_UCODE_TLV_SEC_RT sections into
 *      LMAC / UMAC groups using the CPU1/CPU2 separator marker (same
 *      split iwl_pcie_init_fw_sec() does upstream). The IWL_UCODE_TLV_SEC_INIT
 *      (calibration) image and anything after the paging separator
 *      are not loaded - see the honest gaps below.
 *   2. Points the context-info DRAM map entries directly at the
 *      firmware section bytes already sitting in the Multiboot module
 *      - this kernel never enables paging, so a pointer here already
 *      *is* the physical address the device needs, with no bounce
 *      buffer or copy required (that's a genuine simplification a
 *      paging OS like Linux doesn't get).
 *   3. Builds a minimal 8-entry RX buffer ring and an empty (never
 *      posted-to) TX command ring, because the context-info struct
 *      has no "optional" field for either - the device expects both
 *      to be present and validly sized before it will boot, even
 *      though nothing past this file's own poll loop uses them.
 *   4. Writes the struct's physical address to CSR_CTXT_INFO_BA (CSR
 *      offset 0x40, a 64-bit register) to kick off self-load, then
 *      polls CSR_INT for CSR_INT_BIT_ALIVE (bit 0) - "uCode interrupts
 *      once it initializes", straight from the upstream comment on
 *      that bit.
 *
 * What this does NOT do, stated as plainly as the file above does:
 *   - It does not parse the ALIVE notification the firmware posts to
 *     the RX ring (UMAC/LMAC error-table pointers, firmware status,
 *     etc). The 8-entry RX ring exists only so the device has
 *     somewhere valid to land that packet; this file never reads it.
 *     Getting from "the ALIVE interrupt fired" to "I decoded the
 *     ALIVE command" is most of what a real RX transport is.
 *   - It does not load the INIT/calibration image, service any host
 *     command, bring up TX, or implement any 802.11 MAC behavior.
 *     Same scope boundary as iwlwifi_bringup() above, just moved one
 *     checkpoint further: this now gets far enough to tell you,
 *     definitively and from real hardware, whether the firmware
 *     itself booted - which CSR_GP_CNTRL alone never could.
 *
 * Call only after iwlwifi_bringup() has returned 1 (MAC clock must be
 * running before the device will act on CSR_CTXT_INFO_BA). Returns 1
 * if CSR_INT_BIT_ALIVE was observed within the timeout, 0 otherwise -
 * check the serial log either way, it explains what it saw. */
int iwlwifi_load_firmware(uint32_t bar0, const struct iwlwifi_fw_image* fw);

#endif
