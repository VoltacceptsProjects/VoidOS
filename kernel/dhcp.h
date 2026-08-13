#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>

/* Minimal RFC 2131 DHCP client: DISCOVER -> OFFER -> REQUEST -> ACK,
 * no lease renewal (this kernel runs for one boot session, nowhere
 * near a lease's typical expiry), no rejecting-and-retrying a bad
 * OFFER (takes the first one whose ACK actually arrives). Yields the
 * IP/netmask/gateway/DNS-server config that ip_init() and dns_resolve()
 * need - without this, those would otherwise have to be hardcoded
 * guesses about a network this kernel has never seen. */

struct dhcp_config {
    uint8_t ip[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns_server[4];
};

/* Runs the full DISCOVER/OFFER/REQUEST/ACK exchange, busy-polling
 * internally (same model as tcp.h/dns.h) up to a fixed retry budget.
 * Returns 1 and fills out on success, 0 on timeout - which, same as
 * everywhere else in this file set, is what happens as long as
 * nothing drives netdev_rx() with real received frames (see
 * netdev.h). */
int dhcp_request(struct dhcp_config* out);

#endif
