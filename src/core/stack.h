/*
 * struct pf_stack — composition root for one stack instance. No global
 * mutable state: everything hangs off this object and is passed explicitly.
 */
#ifndef PF_CORE_STACK_H
#define PF_CORE_STACK_H

#include "arp/arp.h"
#include "core/pf.h"
#include "core/stats.h"

#define PF_MAX_DEVS 8

struct netdev;

struct pf_stack {
    struct netdev *devs[PF_MAX_DEVS];
    int ndevs;

    bool hexdump_rx; /* phase-0 debugging: hexdump every received frame */
    bool forwarding; /* router mode (phase 5) */

    struct arp_cache arp;
    struct pf_stats stats;
};

void pf_stack_init(struct pf_stack *s);

/* Release everything the stack still owns (queued packets, etc.).
 * Safe on a zeroed struct; init may be called again afterwards. */
void pf_stack_fini(struct pf_stack *s);

int pf_stack_add_dev(struct pf_stack *s, struct netdev *dev);

/* Configure the stack-side address of an interface. */
void pf_if_set_addr(struct pf_stack *s, struct netdev *dev, uint32_t ip, uint32_t mask);

/* Periodic housekeeping for all modules; called from the event loop. */
void pf_tick(struct pf_stack *s, uint64_t now_ms);

#endif /* PF_CORE_STACK_H */
