/* ICMP — RFC 792 message handling per RFC 1122 §3.2.2. */
#ifndef PF_ICMP_ICMP_H
#define PF_ICMP_ICMP_H

#include "core/pkt.h"
#include "ipv4/ipv4.h"

#define ICMP_HDR_LEN             8

#define ICMP_TYPE_ECHO_REPLY     0
#define ICMP_TYPE_DEST_UNREACH   3
#define ICMP_TYPE_SRC_QUENCH     4
#define ICMP_TYPE_REDIRECT       5
#define ICMP_TYPE_ECHO_REQUEST   8
#define ICMP_TYPE_TIME_EXCEEDED  11
#define ICMP_TYPE_PARAM_PROBLEM  12

#define ICMP_UNREACH_NET         0
#define ICMP_UNREACH_HOST        1
#define ICMP_UNREACH_PROTO       2
#define ICMP_UNREACH_PORT        3
#define ICMP_UNREACH_FRAG_NEEDED 4

#define ICMP_TIME_EXC_TTL        0
#define ICMP_TIME_EXC_REASS      1

/* Minimum spacing between generated ICMP errors (RFC 1812 §4.3.2.8 requires
 * rate limiting; a simple global interval is enough at our scale). */
#define ICMP_ERR_MIN_INTERVAL_MS 10

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t csum; /* network byte order */
    uint32_t rest; /* id/seq for echo, unused/MTU for errors */
} __attribute__((packed));

struct pf_stack;
struct netdev;

/* Handle a received ICMP message (p->data still at the IP header).
 * Consumes p. */
void icmp_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p,
                const struct ipv4_hdr *ih);

/* Generate an ICMP error about the datagram starting at `orig` with
 * `orig_avail` valid bytes. Applies the RFC 1122 §3.2.2 suppression rules
 * (never about an error, a non-first fragment, or non-unicast traffic). */
void icmp_send_error(struct pf_stack *stack, const struct ipv4_hdr *orig, uint16_t orig_avail,
                     uint8_t type, uint8_t code);

#endif /* PF_ICMP_ICMP_H */
