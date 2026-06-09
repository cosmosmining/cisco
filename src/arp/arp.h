/*
 * ARP for IPv4-over-Ethernet — RFC 826, with the cache-validity guidance of
 * RFC 1122 §2.3.2.1 (entries time out; requests are retransmitted with
 * backoff while packets queue behind resolution).
 */
#ifndef PF_ARP_ARP_H
#define PF_ARP_ARP_H

#include "core/pkt.h"

#define ARP_HDR_LEN           28
#define ARP_OP_REQUEST        1
#define ARP_OP_REPLY          2
#define ARP_CACHE_SIZE        64
#define ARP_CACHE_TTL_MS      60000 /* RFC 1122 §2.3.2.1: cache entries must age out */
#define ARP_PENDING_RETRY_MS  1000
#define ARP_PENDING_MAX_TRIES 3
#define ARP_WAITQ_MAX         8 /* RFC 1122 §2.3.2.2: queue (not drop) packets awaiting ARP */

/* Wire image (htype 1 = Ethernet, ptype 0x0800 = IPv4). Protocol addresses
 * are byte arrays: they sit at odd offsets, so they are read/written with
 * pf_get_be32/pf_put_be32. */
struct arp_hdr {
    uint16_t htype; /* network byte order */
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t op;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
} __attribute__((packed));

enum arp_state { ARP_FREE = 0, ARP_PENDING, ARP_RESOLVED };

struct netdev;
struct pf_stack;

struct arp_entry {
    enum arp_state state;
    uint32_t ip; /* host byte order */
    uint8_t mac[6];
    uint8_t tries;
    struct netdev *dev;
    uint64_t deadline_ms; /* RESOLVED: expiry; PENDING: next retransmit */
    struct pktq waitq;    /* IPv4 packets parked until resolution */
};

struct arp_cache {
    struct arp_entry entries[ARP_CACHE_SIZE];
};

void arp_init(struct arp_cache *c);

/* Handle a received ARP frame (Ethernet header already stripped).
 * Consumes p. */
void arp_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p);

/* Resolve `next_hop` on `dev`.
 * Returns 0: mac_out filled, p untouched (caller transmits it).
 * Returns 1: p consumed (queued behind a pending resolution, or dropped on
 *            queue overflow); a request has been (re)sent as needed.
 * p may be NULL to warm the cache without queueing anything. */
int arp_resolve(struct pf_stack *stack, struct netdev *dev, uint32_t next_hop, uint8_t mac_out[6],
                struct pkt *p);

void arp_tick(struct pf_stack *stack, uint64_t now_ms);

struct arp_entry *arp_lookup(struct arp_cache *c, uint32_t ip);
void arp_send_request(struct pf_stack *stack, struct netdev *dev, uint32_t target_ip);

#endif /* PF_ARP_ARP_H */
