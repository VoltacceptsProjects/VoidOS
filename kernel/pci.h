#ifndef PCI_H
#define PCI_H

#include <stdint.h>

void pci_print_display_devices(void);

/* Scans every PCI bus/device/function for class 0x02 (network
 * controller) and logs each one over serial: vendor/device ID,
 * bus/dev/func location, and BAR0 (the memory-mapped register window
 * every PCIe NIC exposes, including the 9260's firmware/command
 * interface). This is intentionally read-only right now - it doesn't
 * touch or reset anything, just confirms the card enumerates and
 * reports where its registers live, before any driver code goes near
 * it. Returns 1 if an Intel Wireless-AC 9260 (device ID 0x2526) was
 * found, 0 otherwise. */
int pci_probe_network_devices(void);

/* Valid only after pci_probe_network_devices() has returned 1. Physical
 * address of the 9260's BAR0 MMIO window. */
uint32_t pci_get_9260_bar0(void);

#endif
