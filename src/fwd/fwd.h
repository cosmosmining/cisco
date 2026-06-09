/* L3 forwarding path (router mode). */
#ifndef PF_FWD_FWD_H
#define PF_FWD_FWD_H

#include "core/pkt.h"
#include "ipv4/ipv4.h"

struct pf_stack;
struct netdev;

/* Forward a validated, not-for-us IPv4 packet (p->data at the IP header).
 * Consumes p. */
void ip_forward(struct pf_stack *stack, struct netdev *indev, struct pkt *p,
                const struct ipv4_hdr *ih);

#endif /* PF_FWD_FWD_H */
