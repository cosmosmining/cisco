/* UDP — RFC 768, with the RFC 1122 §4.1 host requirements. */
#ifndef PF_UDP_UDP_H
#define PF_UDP_UDP_H

#include "core/pkt.h"
#include "ipv4/ipv4.h"

#define UDP_HDR_LEN 8

struct udp_hdr {
    uint16_t sport; /* network byte order */
    uint16_t dport;
    uint16_t len; /* header + data */
    uint16_t csum;
} __attribute__((packed));

struct pf_stack;
struct netdev;

/* Handle a received UDP datagram (p->data at the IP header). Consumes p. */
void udp_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p,
               const struct ipv4_hdr *ih);

/* Prepend a UDP header (checksum incl. pseudo-header) and hand to IP.
 * Ports in host byte order. Consumes p. */
int udp_output(struct pf_stack *stack, uint32_t src_ip, uint16_t sport, uint32_t dst_ip,
               uint16_t dport, struct pkt *p);

#endif /* PF_UDP_UDP_H */
