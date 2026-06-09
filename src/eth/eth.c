#include "eth/eth.h"
#include "arp/arp.h"
#include "core/stack.h"
#include "netdev/netdev.h"

#include <stdio.h>
#include <string.h>

const uint8_t ETH_BCAST[ETH_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

void eth_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p)
{
    stack->stats.eth_rx_frames++;

    /* Runt: smaller than a bare Ethernet header. (TAP does not deliver the
     * FCS, and modern virtual links don't pad to the classic 60-byte
     * minimum, so the only hard floor here is the header itself.) */
    if (p->len < ETH_HDR_LEN) {
        stack->stats.eth_rx_runts++;
        dev->st.rx_drops++;
        pkt_free(p);
        return;
    }

    const struct eth_hdr *eh = (const struct eth_hdr *)p->data;
    uint16_t ethertype = pf_ntohs(eh->type);

    if (stack->hexdump_rx) {
        char src[18], dst[18];
        printf("%s: rx %u bytes  %s -> %s  ethertype 0x%04x\n", dev->name, p->len,
               pf_mac_str(eh->src, src), pf_mac_str(eh->dst, dst), ethertype);
        pf_hexdump(p->data, p->len);
    }

    /* Not promiscuous: accept frames for our MAC, broadcast, or multicast
     * (bit 0 of the first dst octet — covers bcast too). */
    if (memcmp(eh->dst, dev->mac, ETH_ADDR_LEN) != 0 && (eh->dst[0] & 0x01) == 0) {
        stack->stats.eth_rx_other_dest++;
        pkt_free(p);
        return;
    }

    pkt_pull(p, ETH_HDR_LEN);

    switch (ethertype) {
    case ETH_TYPE_ARP:
        arp_input(stack, dev, p); /* consumes p */
        return;
    /* ETH_TYPE_IP4 demux lands here in phase 2. */
    default:
        stack->stats.eth_rx_unknown_ethertype++;
        pkt_free(p);
        return;
    }
}

int eth_output(struct pf_stack *stack, struct netdev *dev, const uint8_t dst[ETH_ADDR_LEN],
               uint16_t ethertype, struct pkt *p)
{
    struct eth_hdr *eh = pkt_push(p, ETH_HDR_LEN);
    memcpy(eh->dst, dst, ETH_ADDR_LEN);
    memcpy(eh->src, dev->mac, ETH_ADDR_LEN);
    eh->type = pf_htons(ethertype);

    stack->stats.eth_tx_frames++;
    int rc = netdev_tx(dev, p);
    pkt_free(p);
    return rc;
}
