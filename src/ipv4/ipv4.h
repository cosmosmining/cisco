/* IPv4 — RFC 791 with the RFC 1122 host-requirements corrections. */
#ifndef PF_IPV4_IPV4_H
#define PF_IPV4_IPV4_H

#include "core/pkt.h"

#define IPV4_HDR_MIN   20
#define IPV4_HDR_MAX   60
#define IP_DEFAULT_TTL 64

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

/* frag_off field bits (RFC 791 §3.1). */
#define IP_FRAG_DF      0x4000u
#define IP_FRAG_MF      0x2000u
#define IP_FRAG_OFFMASK 0x1fffu

struct ipv4_hdr {
    uint8_t vihl; /* version (4 bits) | IHL (4 bits) */
    uint8_t tos;
    uint16_t total_len; /* network byte order */
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t proto;
    uint16_t csum;
    uint32_t saddr; /* network byte order on the wire */
    uint32_t daddr;
} __attribute__((packed));

static inline uint8_t ip_hdr_version(const struct ipv4_hdr *ih)
{
    return ih->vihl >> 4;
}
static inline uint8_t ip_hdr_len(const struct ipv4_hdr *ih)
{
    return (uint8_t)((ih->vihl & 0x0f) * 4);
}

static inline bool ip4_is_loopback(uint32_t ip)
{
    return (ip >> 24) == 127;
}
static inline bool ip4_is_multicast(uint32_t ip)
{
    return (ip >> 28) == 0xe; /* class D */
}
static inline bool ip4_is_experimental(uint32_t ip)
{
    return (ip >> 28) == 0xf; /* class E, includes 255.255.255.255 */
}
static inline bool ip4_is_limited_bcast(uint32_t ip)
{
    return ip == 0xffffffffu;
}

struct pf_stack;
struct netdev;

/* Handle a received IPv4 packet (p->data at the IP header). Consumes p. */
void ip_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p);

/* Local delivery demux (p->data still at the IP header). Consumes p.
 * Exposed for the reassembler to hand up completed datagrams. */
void ip_local_deliver(struct pf_stack *stack, struct netdev *dev, struct pkt *p,
                      const struct ipv4_hdr *ih);

/* Prepend an IPv4 header to the L4 payload in p and route it out.
 * src == 0 picks the egress interface address. Consumes p.
 * Returns 0 if transmitted or parked behind ARP, -1 if dropped. */
int ip_output(struct pf_stack *stack, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl,
              struct pkt *p);

/* Route lookup: egress device + next hop for dst (host byte order).
 * Phase 2: connected subnets only; phase 5 swaps in the FIB trie. */
struct netdev *ip_route_lookup(struct pf_stack *stack, uint32_t dst, uint32_t *next_hop);

#endif /* PF_IPV4_IPV4_H */
