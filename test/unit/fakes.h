/*
 * Test fakes: an injected clock (overrides the platform pf_now_ms — the
 * archive member never gets linked) and a capture netdev whose tx hook
 * records frames instead of writing to a TAP.
 */
#ifndef UT_FAKES_H
#define UT_FAKES_H

#include "core/stack.h"
#include "eth/eth.h"
#include "netdev/netdev.h"

#include <stdio.h>
#include <string.h>

static uint64_t fake_now_ms;

uint64_t pf_now_ms(void)
{
    return fake_now_ms;
}

#define FAKE_TX_MAX  64
#define FAKE_TX_SIZE 2048

static struct {
    uint8_t frame[FAKE_TX_MAX][FAKE_TX_SIZE];
    uint16_t len[FAKE_TX_MAX];
    int count;
} fake_tx;

static int fake_dev_tx(struct netdev *dev, struct pkt *p)
{
    (void)dev;
    if (fake_tx.count < FAKE_TX_MAX) {
        memcpy(fake_tx.frame[fake_tx.count], p->data, p->len);
        fake_tx.len[fake_tx.count] = p->len;
    }
    fake_tx.count++;
    return 0;
}

static void fake_tx_reset(void)
{
    fake_tx.count = 0;
}

static struct netdev fake_devs[4];
static int fake_ndevs;

static struct netdev *fake_dev_add(struct pf_stack *s, uint32_t ip, uint32_t mask)
{
    struct netdev *d = &fake_devs[fake_ndevs++];
    memset(d, 0, sizeof(*d));
    d->mtu = 1500;
    d->up = true;
    d->tx = fake_dev_tx;
    pf_stack_add_dev(s, d);
    snprintf(d->name, sizeof(d->name), "fake%d", d->ifindex);
    d->mac[0] = 0x02;
    d->mac[1] = 0x50;
    d->mac[2] = 0x46;
    d->mac[5] = (uint8_t)(0x02 + d->ifindex);
    d->ip = ip;
    d->mask = mask;
    return d;
}

/* Inject a frame as if received from the wire. Consumed by the stack. */
static void inject_eth(struct pf_stack *s, struct netdev *d, const uint8_t dst[6],
                       const uint8_t src[6], uint16_t ethertype, const void *payload, uint16_t plen)
{
    struct pkt *p = pkt_alloc();
    struct eth_hdr *eh = pkt_put(p, ETH_HDR_LEN);
    memcpy(eh->dst, dst, 6);
    memcpy(eh->src, src, 6);
    eh->type = pf_htons(ethertype);
    memcpy(pkt_put(p, plen), payload, plen);
    p->dev = d;
    p->ts_ms = fake_now_ms;
    eth_input(s, d, p);
}

#endif /* UT_FAKES_H */
