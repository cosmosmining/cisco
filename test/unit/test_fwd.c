#include "fakes.h"
#include "ut.h"

#include "icmp/icmp.h"
#include "ipv4/checksum.h"
#include "ipv4/ipv4.h"
#include "route/fib.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

static const uint8_t H0_MAC[6] = {0xaa, 0, 0, 0, 0, 0x01}; /* host on dev0 */
static const uint8_t H1_MAC[6] = {0xbb, 0, 0, 0, 0, 0x01}; /* host on dev1 */
static const uint32_t H0_IP = IP(10, 1, 0, 1);
static const uint32_t H1_IP = IP(10, 2, 0, 1);

static struct pf_stack stk;
static struct netdev *d0, *d1;

static void seed_arp_on(struct netdev *dev, const uint8_t *mac, uint32_t ip)
{
    struct arp_hdr req = {0};
    req.htype = pf_htons(1);
    req.ptype = pf_htons(ETH_TYPE_IP4);
    req.hlen = 6;
    req.plen = 4;
    req.op = pf_htons(ARP_OP_REQUEST);
    memcpy(req.sha, mac, 6);
    pf_put_be32(req.spa, ip);
    pf_put_be32(req.tpa, dev->ip);
    inject_eth(&stk, dev, ETH_BCAST, mac, ETH_TYPE_ARP, &req, sizeof(req));
}

static void reset(void)
{
    fake_tx_reset();
    fake_ndevs = 0;
    fake_now_ms = 1000000;
    pf_stack_fini(&stk);
    pf_stack_init(&stk);
    stk.forwarding = true;
    d0 = fake_dev_add(&stk, IP(10, 1, 0, 2), 0xffffff00);
    d1 = fake_dev_add(&stk, IP(10, 2, 0, 2), 0xffffff00);
    seed_arp_on(d0, H0_MAC, H0_IP);
    seed_arp_on(d1, H1_MAC, H1_IP);
    fake_tx_reset();
}

/* Inject an IPv4 packet on d0 destined for `dst`. */
static void inject_ip_on_d0(uint32_t dst, uint8_t ttl, uint16_t frag_field, const void *pay,
                            uint16_t plen)
{
    struct pkt *p = pkt_alloc();
    pkt_reserve(p, 64); /* as if an Ethernet header had been pulled */
    struct ipv4_hdr *ih = pkt_put(p, IPV4_HDR_MIN);
    memset(ih, 0, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->total_len = pf_htons((uint16_t)(IPV4_HDR_MIN + plen));
    ih->id = pf_htons(777);
    ih->frag_off = pf_htons(frag_field);
    ih->ttl = ttl;
    ih->proto = 253;
    ih->saddr = pf_htonl(H0_IP);
    ih->daddr = pf_htonl(dst);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    if (plen)
        memcpy(pkt_put(p, plen), pay, plen);
    p->dev = d0;
    ip_input(&stk, d0, p);
}

static void test_forwards_with_ttl_decrement_and_csum(void)
{
    reset();
    uint8_t data[32];
    memset(data, 0x7e, sizeof(data));
    inject_ip_on_d0(H1_IP, 64, 0, data, sizeof(data));

    UT_ASSERT_EQ(stk.stats.ip_fwd_forwarded, 1);
    UT_ASSERT_EQ(fake_tx.count, 1);
    UT_ASSERT_EQ(fake_tx.dev_idx[0], d1->ifindex); /* egress on the right port */

    const struct eth_hdr *eh = (const struct eth_hdr *)fake_tx.frame[0];
    UT_ASSERT(memcmp(eh->dst, H1_MAC, 6) == 0); /* resolved next hop */
    const struct ipv4_hdr *ih = (const struct ipv4_hdr *)(fake_tx.frame[0] + ETH_HDR_LEN);
    UT_ASSERT_EQ(ih->ttl, 63); /* decremented */
    /* RFC 1624 incremental update must verify like a fresh checksum. */
    UT_ASSERT_EQ(inet_csum(ih, IPV4_HDR_MIN), 0);
    UT_ASSERT(memcmp((const uint8_t *)ih + IPV4_HDR_MIN, data, sizeof(data)) == 0);
}

static void test_ttl_expiry_sends_time_exceeded(void)
{
    reset();
    inject_ip_on_d0(H1_IP, 1, 0, "X", 1);
    UT_ASSERT_EQ(stk.stats.ip_fwd_ttl_exceeded, 1);
    UT_ASSERT_EQ(stk.stats.ip_fwd_forwarded, 0);
    UT_ASSERT_EQ(fake_tx.count, 1);
    UT_ASSERT_EQ(fake_tx.dev_idx[0], d0->ifindex); /* error goes back */
    const uint8_t *ic = fake_tx.frame[0] + ETH_HDR_LEN + IPV4_HDR_MIN;
    UT_ASSERT_EQ(ic[0], ICMP_TYPE_TIME_EXCEEDED);
    UT_ASSERT_EQ(ic[1], ICMP_TIME_EXC_TTL); /* RFC 1812 §5.3.1 */
}

static void test_no_route_sends_net_unreachable(void)
{
    reset();
    inject_ip_on_d0(IP(172, 31, 0, 1), 64, 0, "X", 1);
    UT_ASSERT_EQ(stk.stats.ip_fwd_no_route, 1);
    UT_ASSERT_EQ(fake_tx.count, 1);
    const uint8_t *ic = fake_tx.frame[0] + ETH_HDR_LEN + IPV4_HDR_MIN;
    UT_ASSERT_EQ(ic[0], ICMP_TYPE_DEST_UNREACH);
    UT_ASSERT_EQ(ic[1], ICMP_UNREACH_NET); /* RFC 1812 §4.3.3.1 */
}

static void test_static_route_via_gateway(void)
{
    reset();
    /* 172.20/16 via the host on d1. */
    UT_ASSERT_EQ(fib_add_via(&stk, IP(172, 20, 0, 0), 16, H1_IP), 0);
    inject_ip_on_d0(IP(172, 20, 3, 4), 64, 0, "Y", 1);
    UT_ASSERT_EQ(stk.stats.ip_fwd_forwarded, 1);
    UT_ASSERT_EQ(fake_tx.dev_idx[0], d1->ifindex);
    const struct eth_hdr *eh = (const struct eth_hdr *)fake_tx.frame[0];
    UT_ASSERT(memcmp(eh->dst, H1_MAC, 6) == 0); /* MAC of the GATEWAY */
    const struct ipv4_hdr *ih = (const struct ipv4_hdr *)(fake_tx.frame[0] + ETH_HDR_LEN);
    UT_ASSERT_EQ(pf_ntohl(ih->daddr), IP(172, 20, 3, 4)); /* dst untouched */
}

static void test_never_forwards_directed_broadcast(void)
{
    reset();
    inject_ip_on_d0(IP(10, 2, 0, 255), 64, 0, "Z", 1); /* d1's subnet bcast */
    UT_ASSERT_EQ(stk.stats.ip_fwd_bad_dst, 1);         /* RFC 2644 */
    UT_ASSERT_EQ(fake_tx.count, 0);
}

static void test_forwarding_disabled_drops(void)
{
    reset();
    stk.forwarding = false;
    inject_ip_on_d0(H1_IP, 64, 0, "Q", 1);
    UT_ASSERT_EQ(stk.stats.ip_rx_not_for_us, 1);
    UT_ASSERT_EQ(stk.stats.ip_fwd_forwarded, 0);
    UT_ASSERT_EQ(fake_tx.count, 0);
}

static void test_weak_host_delivery_to_far_interface(void)
{
    reset();
    /* ICMP echo to d1's address arriving on d0 must be answered locally
     * (weak host model) — this is what makes `ping <far-side>` work. */
    uint8_t icmp[16];
    struct icmp_hdr *ch = (struct icmp_hdr *)icmp;
    memset(icmp, 0, sizeof(icmp));
    ch->type = ICMP_TYPE_ECHO_REQUEST;
    ch->csum = pf_htons(inet_csum(icmp, sizeof(icmp)));

    struct pkt *p = pkt_alloc();
    pkt_reserve(p, 64);
    struct ipv4_hdr *ih = pkt_put(p, IPV4_HDR_MIN);
    memset(ih, 0, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->total_len = pf_htons(IPV4_HDR_MIN + sizeof(icmp));
    ih->ttl = 64;
    ih->proto = IP_PROTO_ICMP;
    ih->saddr = pf_htonl(H0_IP);
    ih->daddr = pf_htonl(d1->ip); /* far interface */
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    memcpy(pkt_put(p, sizeof(icmp)), icmp, sizeof(icmp));
    p->dev = d0;
    ip_input(&stk, d0, p);

    UT_ASSERT_EQ(stk.stats.icmp_echo_reply_tx, 1);
    UT_ASSERT_EQ(fake_tx.count, 1);
    UT_ASSERT_EQ(fake_tx.dev_idx[0], d0->ifindex); /* reply routed back out d0 */
    const struct ipv4_hdr *rih = (const struct ipv4_hdr *)(fake_tx.frame[0] + ETH_HDR_LEN);
    UT_ASSERT_EQ(pf_ntohl(rih->saddr), d1->ip); /* RFC 1122 §3.2.2.6 */
}

int main(void)
{
    printf("test_fwd:\n");
    UT_RUN(test_forwards_with_ttl_decrement_and_csum);
    UT_RUN(test_ttl_expiry_sends_time_exceeded);
    UT_RUN(test_no_route_sends_net_unreachable);
    UT_RUN(test_static_route_via_gateway);
    UT_RUN(test_never_forwards_directed_broadcast);
    UT_RUN(test_forwarding_disabled_drops);
    UT_RUN(test_weak_host_delivery_to_far_interface);
    pf_stack_fini(&stk);
    printf("test_fwd: all passed\n");
    return 0;
}
