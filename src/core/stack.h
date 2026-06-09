/*
 * struct pf_stack — composition root for one stack instance. No global
 * mutable state: everything hangs off this object and is passed explicitly.
 */
#ifndef PF_CORE_STACK_H
#define PF_CORE_STACK_H

#include "arp/arp.h"
#include "core/pf.h"
#include "core/stats.h"
#include "ipv4/ip_reass.h"
#include "tcp/tcp.h"
#include "udp/sock.h"

#define PF_MAX_DEVS 8

struct netdev;

struct pf_stack {
    struct netdev *devs[PF_MAX_DEVS];
    int ndevs;

    bool hexdump_rx; /* phase-0 debugging: hexdump every received frame */
    bool forwarding; /* router mode (phase 5) */

    struct arp_cache arp;
    struct ip_reass reass;
    struct pf_socktab socks;
    struct tcp_globals tcp;

    uint32_t default_gw;       /* host order; 0 = none (full FIB in phase 5) */
    uint16_t ip_id;            /* IPv4 identification counter */
    uint64_t icmp_last_err_ms; /* ICMP error rate limiting (RFC 1812 §4.3.2.8) */

    /* Platform event-loop pump used by blocking socket calls (D-002).
     * The Linux app layer points this at pf_loop_once. */
    int (*poll_fn)(struct pf_stack *s, int timeout_ms);

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
