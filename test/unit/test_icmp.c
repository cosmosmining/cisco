#include "fakes.h"
#include "ut.h"

#include "icmp/icmp.h"
#include "ipv4/checksum.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

static const uint8_t HOST_MAC[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01};
static const uint32_t STACK_IP = IP(10, 190, 0, 2);
static const uint32_t HOST_IP = IP(10, 190, 0, 1);

static struct pf_stack stk;
static struct netdev *dev;

static void seed_arp(void)
{
    struct arp_hdr req = {0};
    req.htype = pf_htons(1);
    req.ptype = pf_htons(ETH_TYPE_IP4);
    req.hlen = 6;
    req.plen = 4;
    req.op = pf_htons(ARP_OP_REQUEST);
    memcpy(req.sha, HOST_MAC, 6);
    pf_put_be32(req.spa, HOST_IP);
    pf_put_be32(req.tpa, STACK_IP);
    inject_eth(&stk, dev, ETH_BCAST, HOST_MAC, ETH_TYPE_ARP, &req, sizeof(req));
    fake_tx_reset();
}

static void reset(void)
{
    fake_tx_reset();
    fake_ndevs = 0;
    fake_now_ms = 1000000;
    pf_stack_fini(&stk);
    pf_stack_init(&stk);
    dev = fake_dev_add(&stk, STACK_IP, 0xffffff00);
    seed_arp();
}

/* Forge an original datagram header for icmp_send_error(). */
static uint16_t mk_orig(uint8_t *buf, uint32_t src, uint32_t dst, uint8_t proto,
                        uint16_t frag_field, const void *body, uint16_t blen)
{
    struct ipv4_hdr *ih = (struct ipv4_hdr *)buf;
    memset(ih, 0, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->total_len = pf_htons((uint16_t)(IPV4_HDR_MIN + blen));
    ih->frag_off = pf_htons(frag_field);
    ih->ttl = 64;
    ih->proto = proto;
    ih->saddr = pf_htonl(src);
    ih->daddr = pf_htonl(dst);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    if (blen)
        memcpy(buf + IPV4_HDR_MIN, body, blen);
    return (uint16_t)(IPV4_HDR_MIN + blen);
}

static void test_error_includes_header_plus_8(void)
{
    reset();
    uint8_t body[16] = "0123456789abcdef";
    uint8_t orig[64];
    uint16_t olen = mk_orig(orig, HOST_IP, STACK_IP, 253, 0, body, sizeof(body));

    icmp_send_error(&stk, (const struct ipv4_hdr *)orig, olen, ICMP_TYPE_DEST_UNREACH,
                    ICMP_UNREACH_PORT);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 1);
    UT_ASSERT_EQ(fake_tx.count, 1);

    const uint8_t *f = fake_tx.frame[0];
    const struct ipv4_hdr *ih = (const struct ipv4_hdr *)(f + ETH_HDR_LEN);
    UT_ASSERT_EQ(pf_ntohl(ih->daddr), HOST_IP);
    const uint8_t *ic = (const uint8_t *)ih + IPV4_HDR_MIN;
    /* RFC 792: type, code, then orig header + exactly 8 data octets. */
    UT_ASSERT_EQ(ic[0], ICMP_TYPE_DEST_UNREACH);
    UT_ASSERT_EQ(ic[1], ICMP_UNREACH_PORT);
    uint16_t icmp_len = (uint16_t)(pf_ntohs(ih->total_len) - IPV4_HDR_MIN);
    UT_ASSERT_EQ(icmp_len, ICMP_HDR_LEN + IPV4_HDR_MIN + 8);
    UT_ASSERT(memcmp(ic + ICMP_HDR_LEN + IPV4_HDR_MIN, body, 8) == 0);
    UT_ASSERT_EQ(inet_csum(ic, icmp_len), 0);
}

static void test_no_error_about_icmp_error(void)
{
    reset();
    /* Original is itself a dest-unreachable → must be suppressed
     * (RFC 1122 §3.2.2). */
    uint8_t inner[8] = {ICMP_TYPE_DEST_UNREACH, 0, 0, 0, 0, 0, 0, 0};
    uint8_t orig[64];
    uint16_t olen = mk_orig(orig, HOST_IP, STACK_IP, IP_PROTO_ICMP, 0, inner, sizeof(inner));
    icmp_send_error(&stk, (const struct ipv4_hdr *)orig, olen, ICMP_TYPE_TIME_EXCEEDED, 0);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 0);
    UT_ASSERT_EQ(fake_tx.count, 0);

    /* …but an echo *request* (a query) may be errored about. */
    inner[0] = ICMP_TYPE_ECHO_REQUEST;
    olen = mk_orig(orig, HOST_IP, STACK_IP, IP_PROTO_ICMP, 0, inner, sizeof(inner));
    icmp_send_error(&stk, (const struct ipv4_hdr *)orig, olen, ICMP_TYPE_TIME_EXCEEDED, 0);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 1);
}

static void test_no_error_for_nonfirst_fragment(void)
{
    reset();
    uint8_t orig[32];
    uint16_t olen = mk_orig(orig, HOST_IP, STACK_IP, 253, 5 /* offset 40 */, NULL, 0);
    icmp_send_error(&stk, (const struct ipv4_hdr *)orig, olen, ICMP_TYPE_DEST_UNREACH, 0);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 0);
}

static void test_no_error_for_nonunicast(void)
{
    reset();
    uint8_t orig[32];
    /* multicast source */
    uint16_t olen = mk_orig(orig, IP(224, 0, 0, 5), STACK_IP, 253, 0, NULL, 0);
    icmp_send_error(&stk, (const struct ipv4_hdr *)orig, olen, ICMP_TYPE_DEST_UNREACH, 0);
    /* multicast destination */
    olen = mk_orig(orig, HOST_IP, IP(224, 0, 0, 5), 253, 0, NULL, 0);
    icmp_send_error(&stk, (const struct ipv4_hdr *)orig, olen, ICMP_TYPE_DEST_UNREACH, 0);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 0);
    UT_ASSERT_EQ(fake_tx.count, 0);
}

static void test_error_rate_limit(void)
{
    reset();
    uint8_t orig[32];
    uint16_t olen = mk_orig(orig, HOST_IP, STACK_IP, 253, 0, NULL, 0);

    icmp_send_error(&stk, (const struct ipv4_hdr *)orig, olen, ICMP_TYPE_DEST_UNREACH, 0);
    icmp_send_error(&stk, (const struct ipv4_hdr *)orig, olen, ICMP_TYPE_DEST_UNREACH, 0);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 1); /* second one inside the window */
    UT_ASSERT_EQ(stk.stats.icmp_err_suppressed, 1);

    fake_now_ms += ICMP_ERR_MIN_INTERVAL_MS;
    icmp_send_error(&stk, (const struct ipv4_hdr *)orig, olen, ICMP_TYPE_DEST_UNREACH, 0);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 2);
}

static void test_bad_icmp_inputs_counted(void)
{
    reset();
    /* Truncated ICMP (4 bytes). */
    struct pkt *p = pkt_alloc();
    struct ipv4_hdr *ih = pkt_put(p, IPV4_HDR_MIN + 4);
    memset(ih, 0, IPV4_HDR_MIN + 4);
    ih->vihl = 0x45;
    ih->total_len = pf_htons(IPV4_HDR_MIN + 4);
    ih->ttl = 64;
    ih->proto = IP_PROTO_ICMP;
    ih->saddr = pf_htonl(HOST_IP);
    ih->daddr = pf_htonl(STACK_IP);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    p->dev = dev;
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.icmp_rx_malformed, 1);

    /* Echo with broken ICMP checksum. */
    uint8_t icmp[16];
    struct icmp_hdr *ch = (struct icmp_hdr *)icmp;
    ch->type = ICMP_TYPE_ECHO_REQUEST;
    ch->code = 0;
    ch->csum = pf_htons(0xbeef);
    ch->rest = 0;
    memset(icmp + 8, 0, 8);
    p = pkt_alloc();
    ih = pkt_put(p, IPV4_HDR_MIN);
    memset(ih, 0, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->total_len = pf_htons(IPV4_HDR_MIN + 16);
    ih->ttl = 64;
    ih->proto = IP_PROTO_ICMP;
    ih->saddr = pf_htonl(HOST_IP);
    ih->daddr = pf_htonl(STACK_IP);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    memcpy(pkt_put(p, 16), icmp, 16);
    p->dev = dev;
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.icmp_rx_bad_csum, 1);
    UT_ASSERT_EQ(fake_tx.count, 0);
}

int main(void)
{
    printf("test_icmp:\n");
    UT_RUN(test_error_includes_header_plus_8);
    UT_RUN(test_no_error_about_icmp_error);
    UT_RUN(test_no_error_for_nonfirst_fragment);
    UT_RUN(test_no_error_for_nonunicast);
    UT_RUN(test_error_rate_limit);
    UT_RUN(test_bad_icmp_inputs_counted);
    pf_stack_fini(&stk);
    printf("test_icmp: all passed\n");
    return 0;
}
