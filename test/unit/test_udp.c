#include "fakes.h"
#include "ut.h"

#include "icmp/icmp.h"
#include "ipv4/checksum.h"
#include "udp/sock.h"
#include "udp/udp.h"

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

/* Build and inject an IP/UDP datagram. csum_mode: 0=correct, 1=zero, 2=bad */
static void inject_udp(uint32_t src, uint16_t sport, uint16_t dport, const void *payload,
                       uint16_t plen, int csum_mode)
{
    struct pkt *p = pkt_alloc();
    struct ipv4_hdr *ih = pkt_put(p, IPV4_HDR_MIN);
    uint16_t ulen = (uint16_t)(UDP_HDR_LEN + plen);
    ih->vihl = 0x45;
    ih->tos = 0;
    ih->total_len = pf_htons((uint16_t)(IPV4_HDR_MIN + ulen));
    ih->id = pf_htons(42);
    ih->frag_off = 0;
    ih->ttl = 64;
    ih->proto = IP_PROTO_UDP;
    ih->csum = 0;
    ih->saddr = pf_htonl(src);
    ih->daddr = pf_htonl(STACK_IP);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));

    struct udp_hdr *uh = pkt_put(p, UDP_HDR_LEN);
    uh->sport = pf_htons(sport);
    uh->dport = pf_htons(dport);
    uh->len = pf_htons(ulen);
    uh->csum = 0;
    if (plen)
        memcpy(pkt_put(p, plen), payload, plen);

    if (csum_mode == 0) {
        uint16_t c =
            csum_fold(csum_partial(uh, ulen, csum_pseudo(src, STACK_IP, IP_PROTO_UDP, ulen)));
        uh->csum = pf_htons(c == 0 ? 0xffff : c);
    } else if (csum_mode == 2) {
        uh->csum = pf_htons(0xbeef);
    }

    p->dev = dev;
    ip_input(&stk, dev, p);
}

static void test_demux_and_recvfrom(void)
{
    reset();
    int sock = pf_socket(&stk, PF_SOCK_UDP);
    UT_ASSERT(sock >= 0);
    UT_ASSERT_EQ(pf_bind(&stk, sock, 0, 7), 0);

    inject_udp(HOST_IP, 5555, 7, "ping!", 5, 0);
    UT_ASSERT_EQ(stk.stats.udp_rx_delivered, 1);

    char buf[32];
    uint32_t sip;
    uint16_t sport;
    long n = pf_recvfrom(&stk, sock, buf, sizeof(buf), &sip, &sport, 0);
    UT_ASSERT_EQ(n, 5);
    UT_ASSERT(memcmp(buf, "ping!", 5) == 0);
    UT_ASSERT_EQ(sip, HOST_IP);
    UT_ASSERT_EQ(sport, 5555);

    /* Queue now empty → timeout 0 returns 0. */
    UT_ASSERT_EQ(pf_recvfrom(&stk, sock, buf, sizeof(buf), NULL, NULL, 0), 0);
    pf_close(&stk, sock);
}

static void test_sendto_checksum_verifies(void)
{
    reset();
    int sock = pf_socket(&stk, PF_SOCK_UDP);
    UT_ASSERT_EQ(pf_bind(&stk, sock, 0, 2000), 0);
    UT_ASSERT_EQ(pf_sendto(&stk, sock, "ABCDEFG", 7, HOST_IP, 5353), 0);
    UT_ASSERT_EQ(stk.stats.udp_tx, 1);
    UT_ASSERT_EQ(fake_tx.count, 1);

    const struct ipv4_hdr *ih = (const struct ipv4_hdr *)(fake_tx.frame[0] + ETH_HDR_LEN);
    UT_ASSERT_EQ(ih->proto, IP_PROTO_UDP);
    const struct udp_hdr *uh = (const struct udp_hdr *)((const uint8_t *)ih + IPV4_HDR_MIN);
    uint16_t ulen = pf_ntohs(uh->len);
    UT_ASSERT_EQ(ulen, UDP_HDR_LEN + 7);
    UT_ASSERT_EQ(pf_ntohs(uh->sport), 2000);
    UT_ASSERT_EQ(pf_ntohs(uh->dport), 5353);
    /* Receiver-style verify over pseudo + datagram must fold to 0. */
    uint32_t sum = csum_pseudo(pf_ntohl(ih->saddr), pf_ntohl(ih->daddr), IP_PROTO_UDP, ulen);
    UT_ASSERT_EQ(csum_fold(csum_partial(uh, ulen, sum)), 0);
    pf_close(&stk, sock);
}

static void test_zero_checksum_accepted(void)
{
    reset();
    int sock = pf_socket(&stk, PF_SOCK_UDP);
    pf_bind(&stk, sock, 0, 7);
    inject_udp(HOST_IP, 1234, 7, "nocsum", 6, 1);
    /* RFC 768: zero checksum = not computed; must still deliver. */
    UT_ASSERT_EQ(stk.stats.udp_rx_nocsum, 1);
    UT_ASSERT_EQ(stk.stats.udp_rx_delivered, 1);
    pf_close(&stk, sock);
}

static void test_bad_checksum_dropped(void)
{
    reset();
    int sock = pf_socket(&stk, PF_SOCK_UDP);
    pf_bind(&stk, sock, 0, 7);
    inject_udp(HOST_IP, 1234, 7, "badsum", 6, 2);
    UT_ASSERT_EQ(stk.stats.udp_rx_bad_csum, 1);
    UT_ASSERT_EQ(stk.stats.udp_rx_delivered, 0);
    char buf[8];
    UT_ASSERT_EQ(pf_recvfrom(&stk, sock, buf, sizeof(buf), NULL, NULL, 0), 0);
    pf_close(&stk, sock);
}

static void test_closed_port_sends_port_unreachable(void)
{
    reset();
    inject_udp(HOST_IP, 4444, 9999, "nobody", 6, 0);
    /* RFC 1122 §4.1.3.1 → ICMP dest-unreachable code 3. */
    UT_ASSERT_EQ(stk.stats.udp_rx_no_sock, 1);
    UT_ASSERT_EQ(stk.stats.icmp_err_tx, 1);
    UT_ASSERT_EQ(fake_tx.count, 1);
    const uint8_t *ic = fake_tx.frame[0] + ETH_HDR_LEN + IPV4_HDR_MIN;
    UT_ASSERT_EQ(ic[0], ICMP_TYPE_DEST_UNREACH);
    UT_ASSERT_EQ(ic[1], ICMP_UNREACH_PORT);
}

static void test_malformed_udp_counted(void)
{
    reset();
    int sock = pf_socket(&stk, PF_SOCK_UDP);
    pf_bind(&stk, sock, 0, 7);

    /* udp len field larger than what IP delivered */
    struct pkt *p = pkt_alloc();
    struct ipv4_hdr *ih = pkt_put(p, IPV4_HDR_MIN);
    memset(ih, 0, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->total_len = pf_htons(IPV4_HDR_MIN + UDP_HDR_LEN + 4);
    ih->ttl = 64;
    ih->proto = IP_PROTO_UDP;
    ih->saddr = pf_htonl(HOST_IP);
    ih->daddr = pf_htonl(STACK_IP);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    struct udp_hdr *uh = pkt_put(p, UDP_HDR_LEN);
    uh->sport = pf_htons(1);
    uh->dport = pf_htons(7);
    uh->len = pf_htons(200); /* lies */
    uh->csum = 0;
    memset(pkt_put(p, 4), 0, 4);
    p->dev = dev;
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.udp_rx_malformed, 1);

    /* udp len field below the header size */
    p = pkt_alloc();
    ih = pkt_put(p, IPV4_HDR_MIN);
    memset(ih, 0, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->total_len = pf_htons(IPV4_HDR_MIN + UDP_HDR_LEN);
    ih->ttl = 64;
    ih->proto = IP_PROTO_UDP;
    ih->saddr = pf_htonl(HOST_IP);
    ih->daddr = pf_htonl(STACK_IP);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    uh = pkt_put(p, UDP_HDR_LEN);
    uh->sport = pf_htons(1);
    uh->dport = pf_htons(7);
    uh->len = pf_htons(4); /* < 8 */
    uh->csum = 0;
    p->dev = dev;
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.udp_rx_malformed, 2);
    pf_close(&stk, sock);
}

static void test_rxq_bounded(void)
{
    reset();
    int sock = pf_socket(&stk, PF_SOCK_UDP);
    pf_bind(&stk, sock, 0, 7);
    for (int i = 0; i < PF_SOCK_RXQ_CAP + 5; i++)
        inject_udp(HOST_IP, 1000, 7, "x", 1, 0);
    UT_ASSERT_EQ(stk.stats.udp_rx_q_drops, 5);
    UT_ASSERT_EQ(stk.socks.socks[sock].rxq.n, PF_SOCK_RXQ_CAP);
    pf_close(&stk, sock);
}

static void test_ephemeral_autobind_and_conflicts(void)
{
    reset();
    int a = pf_socket(&stk, PF_SOCK_UDP);
    int b = pf_socket(&stk, PF_SOCK_UDP);
    UT_ASSERT(a >= 0 && b >= 0 && a != b);

    UT_ASSERT_EQ(pf_bind(&stk, a, 0, 7), 0);
    UT_ASSERT_EQ(pf_bind(&stk, b, 0, 7), -1); /* conflict */

    /* sendto on unbound socket auto-binds an ephemeral port */
    UT_ASSERT_EQ(pf_sendto(&stk, b, "hi", 2, HOST_IP, 9), 0);
    UT_ASSERT(stk.socks.socks[b].local_port >= PF_EPHEMERAL_BASE);

    pf_close(&stk, a);
    pf_close(&stk, b);
}

int main(void)
{
    printf("test_udp:\n");
    UT_RUN(test_demux_and_recvfrom);
    UT_RUN(test_sendto_checksum_verifies);
    UT_RUN(test_zero_checksum_accepted);
    UT_RUN(test_bad_checksum_dropped);
    UT_RUN(test_closed_port_sends_port_unreachable);
    UT_RUN(test_malformed_udp_counted);
    UT_RUN(test_rxq_bounded);
    UT_RUN(test_ephemeral_autobind_and_conflicts);
    pf_stack_fini(&stk);
    printf("test_udp: all passed\n");
    return 0;
}
