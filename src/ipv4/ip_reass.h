/*
 * IPv4 receive-side fragment reassembly — RFC 791 §3.2 with modern
 * hardening: overlapping fragments abort the whole reassembly (teardrop
 * defense, cf. RFC 5722's rule for IPv6 and Linux's post-CVE-2018-5391
 * behavior), datagram size and context counts are strictly bounded, and
 * timeouts emit ICMP time-exceeded code 1 per RFC 1122 §3.3.2.
 */
#ifndef PF_IPV4_IP_REASS_H
#define PF_IPV4_IP_REASS_H

#include "core/pkt.h"
#include "ipv4/ipv4.h"

#define IP_REASS_CTXS        8
#define IP_REASS_TIMEOUT_MS  15000 /* RFC 791 §3.2 suggests 15 s */
#define IP_REASS_MAX_RANGES  32
#define IP_REASS_MAX_PAYLOAD 4000 /* embedded-style bound, see DECISIONS.md D-010 */

struct ip_reass_ctx {
    bool in_use;
    uint32_t src, dst; /* host byte order */
    uint16_t id;
    uint8_t proto;
    struct netdev *dev;
    uint64_t deadline_ms;

    bool have_first;
    int32_t expected_total; /* payload bytes, -1 until MF=0 fragment seen */
    struct {
        uint16_t start, end; /* [start, end) payload byte range */
    } ranges[IP_REASS_MAX_RANGES];
    int nranges;

    uint8_t hdr[IPV4_HDR_MAX]; /* first fragment's IP header, as received */
    uint8_t hdr_len;
    uint8_t *payload; /* malloc'd IP_REASS_MAX_PAYLOAD */
};

struct ip_reass {
    struct ip_reass_ctx ctxs[IP_REASS_CTXS];
};

struct pf_stack;

void ip_reass_init(struct ip_reass *r);
void ip_reass_fini(struct ip_reass *r);

/* Take one validated fragment (p->data at its IP header). Consumes p; on
 * completion hands the rebuilt datagram to ip_local_deliver(). */
void ip_reass_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p,
                    const struct ipv4_hdr *ih);

void ip_reass_tick(struct pf_stack *stack, uint64_t now_ms);

#endif /* PF_IPV4_IP_REASS_H */
