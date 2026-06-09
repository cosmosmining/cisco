#include "fakes.h"
#include "ut.h"

#include "arp/arp.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

static const uint8_t HOST_MAC[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01};
static const uint32_t STACK_IP = IP(10, 190, 0, 2);
static const uint32_t HOST_IP = IP(10, 190, 0, 1);

static struct pf_stack stk;
static struct netdev *dev;

static void reset(void)
{
    fake_tx_reset();
    fake_ndevs = 0;
    fake_now_ms = 1000000;
    pf_stack_fini(&stk); /* free anything a previous test left queued */
    pf_stack_init(&stk);
    dev = fake_dev_add(&stk, STACK_IP, 0xffffff00);
}

static void build_arp(struct arp_hdr *ah, uint16_t op, const uint8_t sha[6], uint32_t spa,
                      const uint8_t tha[6], uint32_t tpa)
{
    memset(ah, 0, sizeof(*ah));
    ah->htype = pf_htons(1);
    ah->ptype = pf_htons(ETH_TYPE_IP4);
    ah->hlen = 6;
    ah->plen = 4;
    ah->op = pf_htons(op);
    memcpy(ah->sha, sha, 6);
    pf_put_be32(ah->spa, spa);
    if (tha)
        memcpy(ah->tha, tha, 6);
    pf_put_be32(ah->tpa, tpa);
}

static void test_reply_to_request_for_our_ip(void)
{
    reset();
    struct arp_hdr req;
    build_arp(&req, ARP_OP_REQUEST, HOST_MAC, HOST_IP, NULL, STACK_IP);
    inject_eth(&stk, dev, ETH_BCAST, HOST_MAC, ETH_TYPE_ARP, &req, sizeof(req));

    UT_ASSERT_EQ(fake_tx.count, 1);
    const struct eth_hdr *eh = (const struct eth_hdr *)fake_tx.frame[0];
    UT_ASSERT(memcmp(eh->dst, HOST_MAC, 6) == 0); /* unicast back to requester */
    UT_ASSERT(memcmp(eh->src, dev->mac, 6) == 0);
    UT_ASSERT_EQ(pf_ntohs(eh->type), ETH_TYPE_ARP);

    const struct arp_hdr *rep = (const struct arp_hdr *)(fake_tx.frame[0] + ETH_HDR_LEN);
    UT_ASSERT_EQ(pf_ntohs(rep->op), ARP_OP_REPLY);
    UT_ASSERT(memcmp(rep->sha, dev->mac, 6) == 0);
    UT_ASSERT_EQ(pf_get_be32(rep->spa), STACK_IP);
    UT_ASSERT(memcmp(rep->tha, HOST_MAC, 6) == 0);
    UT_ASSERT_EQ(pf_get_be32(rep->tpa), HOST_IP);

    /* RFC 826 merge: we were the target, so the sender got learned. */
    struct arp_entry *e = arp_lookup(&stk.arp, HOST_IP);
    UT_ASSERT(e && e->state == ARP_RESOLVED);
    UT_ASSERT(memcmp(e->mac, HOST_MAC, 6) == 0);
}

static void test_ignores_request_not_for_us(void)
{
    reset();
    struct arp_hdr req;
    build_arp(&req, ARP_OP_REQUEST, HOST_MAC, HOST_IP, NULL, IP(10, 190, 0, 77));
    inject_eth(&stk, dev, ETH_BCAST, HOST_MAC, ETH_TYPE_ARP, &req, sizeof(req));

    UT_ASSERT_EQ(fake_tx.count, 0);
    /* Not the target → no cache entry created (pollution defense). */
    UT_ASSERT(arp_lookup(&stk.arp, HOST_IP) == NULL);
}

static void test_cache_expiry_with_injected_clock(void)
{
    reset();
    struct arp_hdr req;
    build_arp(&req, ARP_OP_REQUEST, HOST_MAC, HOST_IP, NULL, STACK_IP);
    inject_eth(&stk, dev, ETH_BCAST, HOST_MAC, ETH_TYPE_ARP, &req, sizeof(req));
    UT_ASSERT(arp_lookup(&stk.arp, HOST_IP) != NULL);

    fake_now_ms += ARP_CACHE_TTL_MS - 1;
    arp_tick(&stk, fake_now_ms);
    UT_ASSERT(arp_lookup(&stk.arp, HOST_IP) != NULL); /* one ms early: still there */

    fake_now_ms += 1;
    arp_tick(&stk, fake_now_ms);
    UT_ASSERT(arp_lookup(&stk.arp, HOST_IP) == NULL); /* expired */
    UT_ASSERT_EQ(stk.stats.arp_cache_expired, 1);
}

static void test_resolve_queues_then_flushes_on_reply(void)
{
    reset();
    const uint32_t peer = IP(10, 190, 0, 50);
    const uint8_t peer_mac[6] = {0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0x50};

    struct pkt *ip_pkt = pkt_alloc();
    pkt_reserve(ip_pkt, PKT_TX_HEADROOM);
    memcpy(pkt_put(ip_pkt, 5), "HELLO", 5);

    uint8_t mac[6];
    int rc = arp_resolve(&stk, dev, peer, mac, ip_pkt);
    UT_ASSERT_EQ(rc, 1); /* queued; request went out */
    UT_ASSERT_EQ(fake_tx.count, 1);

    const struct eth_hdr *eh = (const struct eth_hdr *)fake_tx.frame[0];
    UT_ASSERT(memcmp(eh->dst, ETH_BCAST, 6) == 0);
    const struct arp_hdr *q = (const struct arp_hdr *)(fake_tx.frame[0] + ETH_HDR_LEN);
    UT_ASSERT_EQ(pf_ntohs(q->op), ARP_OP_REQUEST);
    UT_ASSERT_EQ(pf_get_be32(q->tpa), peer);
    UT_ASSERT_EQ(pf_get_be32(q->spa), STACK_IP);

    /* Reply arrives → parked packet must go out with the resolved MAC. */
    struct arp_hdr rep;
    build_arp(&rep, ARP_OP_REPLY, peer_mac, peer, dev->mac, STACK_IP);
    inject_eth(&stk, dev, dev->mac, peer_mac, ETH_TYPE_ARP, &rep, sizeof(rep));

    UT_ASSERT_EQ(fake_tx.count, 2);
    eh = (const struct eth_hdr *)fake_tx.frame[1];
    UT_ASSERT(memcmp(eh->dst, peer_mac, 6) == 0);
    UT_ASSERT_EQ(pf_ntohs(eh->type), ETH_TYPE_IP4);
    UT_ASSERT(memcmp(fake_tx.frame[1] + ETH_HDR_LEN, "HELLO", 5) == 0);

    /* Now resolved synchronously. */
    UT_ASSERT_EQ(arp_resolve(&stk, dev, peer, mac, NULL), 0);
    UT_ASSERT(memcmp(mac, peer_mac, 6) == 0);
}

static void test_pending_retries_then_gives_up(void)
{
    reset();
    const uint32_t peer = IP(10, 190, 0, 60);
    struct pkt *p = pkt_alloc();
    pkt_reserve(p, PKT_TX_HEADROOM);
    memcpy(pkt_put(p, 4), "DATA", 4);

    uint8_t mac[6];
    UT_ASSERT_EQ(arp_resolve(&stk, dev, peer, mac, p), 1);
    UT_ASSERT_EQ(fake_tx.count, 1); /* initial request */

    fake_now_ms += ARP_PENDING_RETRY_MS;
    arp_tick(&stk, fake_now_ms);
    UT_ASSERT_EQ(fake_tx.count, 2); /* retry #2 */

    fake_now_ms += ARP_PENDING_RETRY_MS;
    arp_tick(&stk, fake_now_ms);
    UT_ASSERT_EQ(fake_tx.count, 3); /* retry #3 (max) */

    fake_now_ms += ARP_PENDING_RETRY_MS;
    arp_tick(&stk, fake_now_ms);
    UT_ASSERT_EQ(fake_tx.count, 3); /* gave up: no more requests */
    UT_ASSERT(arp_lookup(&stk.arp, peer) == NULL);
    UT_ASSERT_EQ(stk.stats.arp_resolve_fails, 1);
    UT_ASSERT_EQ(stk.stats.arp_waitq_drops, 1); /* the parked packet was freed */
}

static void test_waitq_is_bounded(void)
{
    reset();
    const uint32_t peer = IP(10, 190, 0, 70);
    uint8_t mac[6];
    for (int i = 0; i < ARP_WAITQ_MAX + 2; i++) {
        struct pkt *p = pkt_alloc();
        pkt_reserve(p, PKT_TX_HEADROOM);
        memcpy(pkt_put(p, 4), "PKTx", 4);
        UT_ASSERT_EQ(arp_resolve(&stk, dev, peer, mac, p), 1);
    }
    struct arp_entry *e = arp_lookup(&stk.arp, peer);
    UT_ASSERT(e && e->state == ARP_PENDING);
    UT_ASSERT_EQ(e->waitq.n, ARP_WAITQ_MAX);
    UT_ASSERT_EQ(stk.stats.arp_waitq_drops, 2);
}

static void test_malformed_arp_is_counted_not_crashed(void)
{
    reset();
    /* Truncated header. */
    uint8_t junk[10] = {0};
    inject_eth(&stk, dev, ETH_BCAST, HOST_MAC, ETH_TYPE_ARP, junk, sizeof(junk));
    UT_ASSERT_EQ(stk.stats.arp_rx_malformed, 1);

    /* Wrong hardware type. */
    struct arp_hdr bad;
    build_arp(&bad, ARP_OP_REQUEST, HOST_MAC, HOST_IP, NULL, STACK_IP);
    bad.htype = pf_htons(6);
    inject_eth(&stk, dev, ETH_BCAST, HOST_MAC, ETH_TYPE_ARP, &bad, sizeof(bad));
    UT_ASSERT_EQ(stk.stats.arp_rx_malformed, 2);

    /* Bogus opcode. */
    build_arp(&bad, 7, HOST_MAC, HOST_IP, NULL, STACK_IP);
    inject_eth(&stk, dev, ETH_BCAST, HOST_MAC, ETH_TYPE_ARP, &bad, sizeof(bad));
    UT_ASSERT_EQ(stk.stats.arp_rx_malformed, 3);

    UT_ASSERT_EQ(fake_tx.count, 0);

    /* Still functional afterwards. */
    struct arp_hdr good;
    build_arp(&good, ARP_OP_REQUEST, HOST_MAC, HOST_IP, NULL, STACK_IP);
    inject_eth(&stk, dev, ETH_BCAST, HOST_MAC, ETH_TYPE_ARP, &good, sizeof(good));
    UT_ASSERT_EQ(fake_tx.count, 1);
}

static void test_runt_frame_dropped(void)
{
    reset();
    struct pkt *p = pkt_alloc();
    memcpy(pkt_put(p, 8), "\xff\xff\xff\xff\xff\xff\x02\x50", 8);
    p->dev = dev;
    eth_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.eth_rx_runts, 1);
}

int main(void)
{
    printf("test_arp:\n");
    UT_RUN(test_reply_to_request_for_our_ip);
    UT_RUN(test_ignores_request_not_for_us);
    UT_RUN(test_cache_expiry_with_injected_clock);
    UT_RUN(test_resolve_queues_then_flushes_on_reply);
    UT_RUN(test_pending_retries_then_gives_up);
    UT_RUN(test_waitq_is_bounded);
    UT_RUN(test_malformed_arp_is_counted_not_crashed);
    UT_RUN(test_runt_frame_dropped);
    pf_stack_fini(&stk);
    printf("test_arp: all passed\n");
    return 0;
}
