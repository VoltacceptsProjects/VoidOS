#ifndef ARP_H
#define ARP_H

#include <stdint.h>

/* RFC 826 ARP, IPv4/Ethernet only (hardware type 1, protocol type
 * 0x0800) - the only combination this kernel ever needs. This layer
 * is fully correct/testable on its own; it just has nothing real to
 * talk to until netdev has a real backing device (see netdev.h). */

void arp_init(void);

/* Called by netdev_rx() for every frame with ethertype 0x0806.
 * src_mac is the sender hardware address taken from the Ethernet
 * header (redundant with the ARP payload's own sender-HA field on a
 * well-formed packet, kept separate so this doesn't have to trust the
 * payload before validating it). */
void arp_handle_frame(const uint8_t src_mac[6], const uint8_t* payload, uint32_t len);

/* Sends an ARP request for target_ip and returns immediately (this
 * kernel has no scheduler to block a caller on). Poll arp_lookup()
 * afterward; see ip.c for how the IP layer uses this. */
void arp_request(const uint8_t target_ip[4]);

/* Returns 1 and fills out_mac if target_ip is in the cache, 0
 * otherwise. */
int arp_lookup(const uint8_t target_ip[4], uint8_t out_mac[6]);

#endif
