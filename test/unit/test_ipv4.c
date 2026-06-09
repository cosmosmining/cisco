#include "fakes.h"
#include "ut.h"

#include "icmp/icmp.h"
#include "ipv4/checksum.h"
#include "ipv4/ip_reass.h"
#include "ipv4/ipv4.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

static const uint8_t HOST_MAC[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01};
static const uint32_t STACK_IP = IP(10, 190, 0, 2);
static const uint32_t HOST_IP = IP(10, 190, 0, 1);

static struct pf_stack stk;
static struct netdev *dev;

static void seed_arp(void)
{
    /* Let the stack learn HOST_IP↔HOST_MAC the legitimate way: an ARP
     * request targeting us. */
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

/* Build an IPv4 packet (correct header checksum) ready for ip_input. */
static struct pkt *mk_ip(uint32_t src, uint32_t dst, uint8_t proto, uint16_t id,
                         uint16_t frag_field, const void *payload, uint16_t plen)
{
    struct pkt *p = pkt_alloc();
    struct ipv4_hdr *ih = pkt_put(p, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->tos = 0;
    ih->total_len = pf_htons((uint16_t)(IPV4_HDR_MIN + plen));
    ih->id = pf_htons(id);
    ih->frag_off = pf_htons(frag_field);
    ih->ttl = 64;
    ih->proto = proto;
    ih->csum = 0;
    ih->saddr = pf_htonl(src);
    ih->daddr = pf_htonl(dst);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    if (plen)
        memcpy(pkt_put(p, plen), payload, plen);
    p->dev = dev;
    p->ts_ms = fake_now_ms;
    return p;
}

static uint16_t mk_echo(uint8_t *buf, uint16_t datalen, uint8_t fill)
{
    struct icmp_hdr *ch = (struct icmp_hdr *)buf;
    ch->type = ICMP_TYPE_ECHO_REQUEST;
    ch->code = 0;
    ch->csum = 0;
    ch->rest = pf_htonl(0x12340001);
    memset(buf + ICMP_HDR_LEN, fill, datalen);
    uint16_t len = (uint16_t)(ICMP_HDR_LEN + datalen);
    ch->csum = pf_htons(inet_csum(buf, len));
    return len;
}

/* Parse the most recent tx frame; returns the IP header. */
static const struct ipv4_hdr *last_tx_ip(void)
{
    UT_ASSERT(fake_tx.count >= 1);
    const uint8_t *f = fake_tx.frame[fake_tx.count - 1];
    const struct eth_hdr *eh = (const struct eth_hdr *)f;
    UT_ASSERT_EQ(pf_ntohs(eh->type), ETH_TYPE_IP4);
    return (const struct ipv4_hdr *)(f + ETH_HDR_LEN);
}

static void test_echo_request_gets_reply(void)
{
    reset();
    uint8_t icmp[64];
    uint16_t ilen = mk_echo(icmp, 32, 0x5a);
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, IP_PROTO_ICMP, 1, 0, icmp, ilen));

    UT_ASSERT_EQ(stk.stats.icmp_echo_req_rx, 1);
    UT_ASSERT_EQ(stk.stats.icmp_echo_reply_tx, 1);
    UT_ASSERT_EQ(fake_tx.count, 1);

    const struct ipv4_hdr *ih = last_tx_ip();
    UT_ASSERT_EQ(pf_ntohl(ih->saddr), STACK_IP); /* RFC 1122 §3.2.2.6 */
    UT_ASSERT_EQ(pf_ntohl(ih->daddr), HOST_IP);
    UT_ASSERT_EQ(ih->proto, IP_PROTO_ICMP);
    UT_ASSERT_EQ(inet_csum(ih, IPV4_HDR_MIN), 0);

    const uint8_t *ric = (const uint8_t *)ih + IPV4_HDR_MIN;
    UT_ASSERT_EQ(ric[0], ICMP_TYPE_ECHO_REPLY);
    UT_ASSERT_EQ(inet_csum(ric, ilen), 0);
    UT_ASSERT_EQ(ric[ICMP_HDR_LEN + 5], 0x5a); /* payload echoed back */
}

static void test_eth_padding_is_trimmed(void)
{
    reset();
    uint8_t icmp[16];
    uint16_t ilen = mk_echo(icmp, 4, 0x11);
    struct pkt *p = mk_ip(HOST_IP, STACK_IP, IP_PROTO_ICMP, 2, 0, icmp, ilen);
    memset(pkt_put(p, 18), 0, 18); /* simulated 60-byte min-frame padding */
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.icmp_echo_reply_tx, 1); /* csum ran on trimmed len */
}

static void test_malformed_headers_each_counted(void)
{
    reset();
    uint8_t icmp[16];
    uint16_t ilen = mk_echo(icmp, 4, 0);
    struct pkt *p;

    p = mk_ip(HOST_IP, STACK_IP, IP_PROTO_ICMP, 3, 0, icmp, ilen);
    ((struct ipv4_hdr *)p->data)->vihl = 0x55; /* version 5 */
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.ip_rx_bad_version, 1);

    p = mk_ip(HOST_IP, STACK_IP, IP_PROTO_ICMP, 4, 0, icmp, ilen);
    ((struct ipv4_hdr *)p->data)->vihl = 0x44; /* IHL 16 < 20 */
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.ip_rx_bad_ihl, 1);

    p = mk_ip(HOST_IP, STACK_IP, IP_PROTO_ICMP, 5, 0, icmp, ilen);
    ((struct ipv4_hdr *)p->data)->total_len = pf_htons(1000); /* > frame */
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.ip_rx_bad_len, 1);

    p = mk_ip(HOST_IP, STACK_IP, IP_PROTO_ICMP, 6, 0, icmp, ilen);
    ((struct ipv4_hdr *)p->data)->total_len = pf_htons(8); /* < IHL */
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.ip_rx_bad_len, 2);

    p = mk_ip(HOST_IP, STACK_IP, IP_PROTO_ICMP, 7, 0, icmp, ilen);
    ((struct ipv4_hdr *)p->data)->csum ^= 0xffff;
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.ip_rx_bad_csum, 1);

    p = mk_ip(0xffffffffu, STACK_IP, IP_PROTO_ICMP, 8, 0, icmp, ilen); /* bcast src */
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.ip_rx_bad_src, 1);

    struct pkt *runt = pkt_alloc();
    const uint8_t stub[12] = {0x45, 0x00, 0x00, 0x14, 0xbe, 0xef, 0, 0, 64, 1, 0, 0};
    memcpy(pkt_put(runt, 12), stub, 12);
    runt->dev = dev;
    ip_input(&stk, dev, runt);
    UT_ASSERT_EQ(stk.stats.ip_rx_truncated, 1);

    UT_ASSERT_EQ(fake_tx.count, 0); /* none of those got a response */
}

static void test_unknown_proto_unreachable(void)
{
    reset();
    const uint8_t data[12] = "hello proto";
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 9, 0, data, sizeof(data)));

    /* RFC 1122 §3.2.2.1: dest-unreachable code 2 with orig header + 8. */
    UT_ASSERT_EQ(stk.stats.ip_rx_proto_unreach, 1);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 1);
    const struct ipv4_hdr *ih = last_tx_ip();
    UT_ASSERT_EQ(pf_ntohl(ih->daddr), HOST_IP);
    const uint8_t *ic = (const uint8_t *)ih + IPV4_HDR_MIN;
    UT_ASSERT_EQ(ic[0], ICMP_TYPE_DEST_UNREACH);
    UT_ASSERT_EQ(ic[1], ICMP_UNREACH_PROTO);
    const struct ipv4_hdr *orig = (const struct ipv4_hdr *)(ic + ICMP_HDR_LEN);
    UT_ASSERT_EQ(orig->proto, 253);
    UT_ASSERT(memcmp((const uint8_t *)orig + IPV4_HDR_MIN, data, 8) == 0);
}

static void test_not_for_us_dropped(void)
{
    reset();
    uint8_t icmp[16];
    uint16_t ilen = mk_echo(icmp, 4, 0);
    ip_input(&stk, dev, mk_ip(HOST_IP, IP(10, 190, 0, 77), IP_PROTO_ICMP, 10, 0, icmp, ilen));
    UT_ASSERT_EQ(stk.stats.ip_rx_not_for_us, 1);
    UT_ASSERT_EQ(fake_tx.count, 0);
}

/* -- reassembly ----------------------------------------------------------- */

/* Split an ICMP echo across two fragments; both correct. */
static void send_two_frags(uint16_t id, bool out_of_order)
{
    uint8_t icmp[32];
    mk_echo(icmp, 24, 0x77); /* 32 ICMP bytes total */
    struct pkt *f0 = mk_ip(HOST_IP, STACK_IP, IP_PROTO_ICMP, id, IP_FRAG_MF | 0, icmp, 16);
    struct pkt *f1 = mk_ip(HOST_IP, STACK_IP, IP_PROTO_ICMP, id, 16 / 8, icmp + 16, 16);
    if (out_of_order) {
        ip_input(&stk, dev, f1);
        ip_input(&stk, dev, f0);
    } else {
        ip_input(&stk, dev, f0);
        ip_input(&stk, dev, f1);
    }
}

static void test_reassembly_in_order(void)
{
    reset();
    send_two_frags(100, false);
    UT_ASSERT_EQ(stk.stats.ip_reass_completed, 1);
    UT_ASSERT_EQ(stk.stats.icmp_echo_reply_tx, 1); /* reassembled echo answered */
    const struct ipv4_hdr *ih = last_tx_ip();
    UT_ASSERT_EQ(pf_ntohs(ih->total_len), IPV4_HDR_MIN + 32);
}

static void test_reassembly_out_of_order(void)
{
    reset();
    send_two_frags(101, true);
    UT_ASSERT_EQ(stk.stats.ip_reass_completed, 1);
    UT_ASSERT_EQ(stk.stats.icmp_echo_reply_tx, 1);
}

static void test_overlap_aborts_context(void)
{
    reset();
    uint8_t data[32];
    memset(data, 0xab, sizeof(data));
    /* [0,16) then [8,24) — classic teardrop-style overlap. */
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 102, IP_FRAG_MF | 0, data, 16));
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 102, IP_FRAG_MF | 1, data, 16));
    UT_ASSERT_EQ(stk.stats.ip_reass_overlap_drops, 1);

    /* Context is dead: completing the datagram now must not deliver. */
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 102, 16 / 8, data, 8));
    UT_ASSERT_EQ(stk.stats.ip_reass_completed, 0);
    UT_ASSERT_EQ(stk.stats.ip_rx_proto_unreach, 0);
}

static void test_duplicate_fragment_is_overlap(void)
{
    reset();
    uint8_t data[16];
    memset(data, 0xcd, sizeof(data));
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 103, IP_FRAG_MF | 0, data, 16));
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 103, IP_FRAG_MF | 0, data, 16));
    UT_ASSERT_EQ(stk.stats.ip_reass_overlap_drops, 1);
}

static void test_conflicting_last_fragment(void)
{
    reset();
    uint8_t data[16];
    memset(data, 0xee, sizeof(data));
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 104, 24 / 8, data, 16)); /* end 40 */
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 104, 8 / 8, data, 16));  /* end 24 ≠ */
    UT_ASSERT_EQ(stk.stats.ip_reass_overlap_drops, 1);
}

static void test_reassembly_timeout_sends_time_exceeded(void)
{
    reset();
    uint8_t data[16];
    memset(data, 0x42, sizeof(data));
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 105, IP_FRAG_MF | 0, data, 16));
    UT_ASSERT_EQ(fake_tx.count, 0);

    fake_now_ms += IP_REASS_TIMEOUT_MS + 1;
    ip_reass_tick(&stk, fake_now_ms);

    /* RFC 1122 §3.3.2: time-exceeded code 1, only because frag 0 arrived. */
    UT_ASSERT_EQ(stk.stats.ip_reass_timeouts, 1);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 1);
    const struct ipv4_hdr *ih = last_tx_ip();
    const uint8_t *ic = (const uint8_t *)ih + IPV4_HDR_MIN;
    UT_ASSERT_EQ(ic[0], ICMP_TYPE_TIME_EXCEEDED);
    UT_ASSERT_EQ(ic[1], ICMP_TIME_EXC_REASS);
    /* Embedded original keeps its as-received frag bits + our id. */
    const struct ipv4_hdr *orig = (const struct ipv4_hdr *)(ic + ICMP_HDR_LEN);
    UT_ASSERT_EQ(pf_ntohs(orig->id), 105);
    UT_ASSERT(pf_ntohs(orig->frag_off) & IP_FRAG_MF);
}

static void test_timeout_without_first_frag_is_silent(void)
{
    reset();
    uint8_t data[16];
    memset(data, 0x42, sizeof(data));
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 106, IP_FRAG_MF | 2, data, 16));
    fake_now_ms += IP_REASS_TIMEOUT_MS + 1;
    ip_reass_tick(&stk, fake_now_ms);
    UT_ASSERT_EQ(stk.stats.ip_reass_timeouts, 1);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 0); /* no frag 0 → no ICMP */
}

static void test_reassembly_size_bound(void)
{
    reset();
    uint8_t data[64];
    memset(data, 0x99, sizeof(data));
    uint16_t off_units = 3992 / 8; /* 3992 + 64 > 4000 cap */
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 107, IP_FRAG_MF | off_units, data, 64));
    UT_ASSERT_EQ(stk.stats.ip_reass_too_big, 1);
}

static void test_nonfinal_fragment_must_be_multiple_of_8(void)
{
    reset();
    uint8_t data[7];
    memset(data, 0x31, sizeof(data));
    ip_input(&stk, dev, mk_ip(HOST_IP, STACK_IP, 253, 108, IP_FRAG_MF | 0, data, 7));
    UT_ASSERT_EQ(stk.stats.ip_reass_bad_frag, 1);
}

int main(void)
{
    printf("test_ipv4:\n");
    UT_RUN(test_echo_request_gets_reply);
    UT_RUN(test_eth_padding_is_trimmed);
    UT_RUN(test_malformed_headers_each_counted);
    UT_RUN(test_unknown_proto_unreachable);
    UT_RUN(test_not_for_us_dropped);
    UT_RUN(test_reassembly_in_order);
    UT_RUN(test_reassembly_out_of_order);
    UT_RUN(test_overlap_aborts_context);
    UT_RUN(test_duplicate_fragment_is_overlap);
    UT_RUN(test_conflicting_last_fragment);
    UT_RUN(test_reassembly_timeout_sends_time_exceeded);
    UT_RUN(test_timeout_without_first_frag_is_silent);
    UT_RUN(test_reassembly_size_bound);
    UT_RUN(test_nonfinal_fragment_must_be_multiple_of_8);
    pf_stack_fini(&stk);
    printf("test_ipv4: all passed\n");
    return 0;
}
