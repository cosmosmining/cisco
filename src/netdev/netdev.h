/*
 * Network device abstraction. This header is portable (no platform types);
 * Linux TAP specifics live in tap_linux.c behind the `tx` hook and `priv`.
 */
#ifndef PF_NETDEV_NETDEV_H
#define PF_NETDEV_NETDEV_H

#include "core/pf.h"
#include "core/pkt.h"

struct pf_stack;

struct netdev_stats {
    uint64_t rx_pkts, rx_bytes, rx_drops;
    uint64_t tx_pkts, tx_bytes, tx_errs;
};

struct netdev {
    char name[16];
    int ifindex; /* index within the owning stack */
    uint8_t mac[6];
    uint32_t ip;   /* stack-side address, host byte order; 0 = unset */
    uint32_t mask; /* host byte order */
    uint16_t mtu;
    bool up;

    struct pf_stack *stack;

    /* Emit frame p->data[0..len). Does NOT take ownership of p. */
    int (*tx)(struct netdev *dev, struct pkt *p);
    void *priv;

    struct netdev_stats st;
};

/* Counted transmit wrapper around dev->tx. Does not take ownership. */
int netdev_tx(struct netdev *dev, struct pkt *p);

#endif /* PF_NETDEV_NETDEV_H */
