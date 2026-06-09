/* Ethernet II framing. */
#ifndef PF_ETH_ETH_H
#define PF_ETH_ETH_H

#include "core/pkt.h"

#define ETH_HDR_LEN  14
#define ETH_ADDR_LEN 6

#define ETH_TYPE_IP4 0x0800
#define ETH_TYPE_ARP 0x0806

/* Wire image — packed (DECISIONS.md D-008). */
struct eth_hdr {
    uint8_t dst[ETH_ADDR_LEN];
    uint8_t src[ETH_ADDR_LEN];
    uint16_t type; /* network byte order */
} __attribute__((packed));

struct pf_stack;
struct netdev;

extern const uint8_t ETH_BCAST[ETH_ADDR_LEN];

/* Handle a received frame. Consumes p. */
void eth_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p);

/* Prepend an Ethernet header and transmit. Consumes p. */
int eth_output(struct pf_stack *stack, struct netdev *dev, const uint8_t dst[ETH_ADDR_LEN],
               uint16_t ethertype, struct pkt *p);

#endif /* PF_ETH_ETH_H */
