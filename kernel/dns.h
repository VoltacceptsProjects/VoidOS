#ifndef DNS_H
#define DNS_H

#include <stdint.h>

/* Minimal DNS (RFC 1035) A-record resolution over UDP: one query,
 * one question, standard query flags, no recursion-desired-off
 * trickery, no caching beyond "ask again every call" (fine - this
 * kernel resolves one host, voidos.infinityfree.io, at most a
 * handful of times per boot). No AAAA, no CNAME chase beyond the
 * first answer record whose type is A (good enough for the one
 * simple hosting setup this targets; a CNAME-only response with no
 * A record anywhere in the answer section will fail to resolve here,
 * same as it would confuse a from-scratch client written to any
 * similarly small scope). */

/* dns_server_ip should be a resolver reachable on the local network
 * (typically handed out by DHCP - this kernel has no DHCP client, so
 * callers currently have to know one; see the honest gap noted where
 * net.c calls this). Returns 1 and fills out_ip on success, 0 on
 * timeout/failure/no-A-record. Blocks in a busy poll loop internally
 * up to a fixed number of retries - see dns.c. */
int dns_resolve(const uint8_t dns_server_ip[4], const char* hostname, uint8_t out_ip[4]);

#endif
