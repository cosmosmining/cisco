#include "fakes.h"
#include "ut.h"

#include "ipv4/checksum.h"
#include "tcp/tcp.h"
#include "udp/sock.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

static const uint8_t HOST_MAC[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01};
static const uint32_t STACK_IP = IP(10, 190, 0, 2);
static const uint32_t HOST_IP = IP(10, 190, 0, 1);
static const uint16_t CPORT = 43210;

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

/* Inject one TCP segment from the "client" (HOST_IP:CPORT). */
static void inject_tcp(uint32_t seq, uint32_t ack, uint8_t flags, uint16_t wnd, const void *pay,
                       uint16_t plen, uint16_t mss_opt, uint16_t dport)
{
    uint8_t optlen = mss_opt ? 4 : 0;
    uint16_t l4len = (uint16_t)(TCP_HDR_MIN + optlen + plen);

    struct pkt *p = pkt_alloc();
    struct ipv4_hdr *ih = pkt_put(p, IPV4_HDR_MIN);
    memset(ih, 0, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->total_len = pf_htons((uint16_t)(IPV4_HDR_MIN + l4len));
    ih->ttl = 64;
    ih->proto = IP_PROTO_TCP;
    ih->saddr = pf_htonl(HOST_IP);
    ih->daddr = pf_htonl(STACK_IP);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));

    struct tcp_hdr *th = pkt_put(p, (uint16_t)(TCP_HDR_MIN + optlen));
    memset(th, 0, (size_t)TCP_HDR_MIN + optlen);
    th->sport = pf_htons(CPORT);
    th->dport = pf_htons(dport);
    th->seq = pf_htonl(seq);
    th->ack = pf_htonl(ack);
    th->doff = (uint8_t)(((TCP_HDR_MIN + optlen) / 4) << 4);
    th->flags = flags;
    th->wnd = pf_htons(wnd);
    if (mss_opt) {
        uint8_t *o = (uint8_t *)th + TCP_HDR_MIN;
        o[0] = TCP_OPT_MSS;
        o[1] = 4;
        pf_put_be16(o + 2, mss_opt);
    }
    if (plen)
        memcpy(pkt_put(p, plen), pay, plen);

    uint32_t sum = csum_pseudo(HOST_IP, STACK_IP, IP_PROTO_TCP, l4len);
    th->csum = pf_htons(csum_fold(csum_partial(th, l4len, sum)));

    p->dev = dev;
    ip_input(&stk, dev, p);
}

/* Most recent transmitted TCP header. */
static const struct tcp_hdr *tx_tcp(int idx)
{
    UT_ASSERT(fake_tx.count > idx);
    return (const struct tcp_hdr *)(fake_tx.frame[idx] + ETH_HDR_LEN + IPV4_HDR_MIN);
}
static const struct tcp_hdr *last_tcp(void)
{
    return tx_tcp(fake_tx.count - 1);
}
static const uint8_t *tx_payload(int idx, uint16_t *plen)
{
    const uint8_t *f = fake_tx.frame[idx];
    const struct ipv4_hdr *ih = (const struct ipv4_hdr *)(f + ETH_HDR_LEN);
    const struct tcp_hdr *th = (const struct tcp_hdr *)((const uint8_t *)ih + IPV4_HDR_MIN);
    uint16_t total = pf_ntohs(ih->total_len);
    *plen = (uint16_t)(total - IPV4_HDR_MIN - tcp_hdr_len(th));
    return (const uint8_t *)th + tcp_hdr_len(th);
}

/* Three-way handshake against a fresh listener on port 7.
 * Returns the accepted socket; *srv_iss_out gets the server's ISS. */
static int establish(uint32_t cli_iss, uint16_t peer_mss, uint16_t peer_wnd, uint32_t *srv_iss_out,
                     int *lsock_out)
{
    int ls = pf_socket(&stk, PF_SOCK_TCP);
    UT_ASSERT(ls >= 0);
    UT_ASSERT_EQ(pf_bind(&stk, ls, 0, 7), 0);
    UT_ASSERT_EQ(pf_listen(&stk, ls, 4), 0);

    inject_tcp(cli_iss, 0, TCP_SYN, peer_wnd, NULL, 0, peer_mss, 7);
    UT_ASSERT_EQ(fake_tx.count, 1); /* SYN|ACK */
    const struct tcp_hdr *sa = tx_tcp(0);
    UT_ASSERT_EQ(sa->flags, TCP_SYN | TCP_ACK);
    UT_ASSERT_EQ(pf_ntohl(sa->ack), cli_iss + 1);
    UT_ASSERT_EQ(tcp_hdr_len(sa), TCP_HDR_MIN + 4); /* carries our MSS option */
    uint32_t srv_iss = pf_ntohl(sa->seq);

    inject_tcp(cli_iss + 1, srv_iss + 1, TCP_ACK, peer_wnd, NULL, 0, 0, 7);
    int cs = pf_accept(&stk, ls, NULL, NULL, 0);
    UT_ASSERT(cs >= 0);

    if (srv_iss_out)
        *srv_iss_out = srv_iss;
    if (lsock_out)
        *lsock_out = ls;
    fake_tx_reset();
    return cs;
}

static void test_handshake_and_accept(void)
{
    reset();
    uint32_t srv_iss;
    int ls;
    int cs = establish(1000, 1460, 65535, &srv_iss, &ls);
    struct pf_sock *s = pf_sock_get(&stk, cs);
    UT_ASSERT(s && s->tcb && s->tcb->state == TCP_ESTABLISHED);
    UT_ASSERT_EQ(s->tcb->mss, 1460);
    UT_ASSERT_EQ(stk.stats.tcp_conns_established, 1);
    pf_close(&stk, cs);
    pf_close(&stk, ls);
}

static void test_syn_to_closed_port_gets_rst(void)
{
    reset();
    inject_tcp(5000, 0, TCP_SYN, 1024, NULL, 0, 0, 9999);
    UT_ASSERT_EQ(stk.stats.tcp_rx_no_match, 1);
    UT_ASSERT_EQ(fake_tx.count, 1);
    const struct tcp_hdr *r = last_tcp();
    /* RFC 9293 §3.10.7.1: RST|ACK, SEQ=0, ACK=SEG.SEQ+1 (SYN counts). */
    UT_ASSERT_EQ(r->flags, TCP_RST | TCP_ACK);
    UT_ASSERT_EQ(pf_ntohl(r->seq), 0);
    UT_ASSERT_EQ(pf_ntohl(r->ack), 5001);
}

static void test_data_delivery_and_delayed_ack(void)
{
    reset();
    uint32_t srv_iss;
    int cs = establish(2000, 1460, 65535, &srv_iss, NULL);

    inject_tcp(2001, srv_iss + 1, TCP_ACK | TCP_PSH, 65535, "HELLO", 5, 0, 7);
    /* One segment: ACK is delayed (RFC 1122 §4.2.3.2), not immediate. */
    UT_ASSERT_EQ(fake_tx.count, 0);
    fake_now_ms += TCP_DELACK_MS;
    tcp_tick(&stk, fake_now_ms);
    UT_ASSERT_EQ(fake_tx.count, 1);
    UT_ASSERT_EQ(pf_ntohl(last_tcp()->ack), 2006);

    char buf[16];
    UT_ASSERT_EQ(pf_recv(&stk, cs, buf, sizeof(buf), 0), 5);
    UT_ASSERT(memcmp(buf, "HELLO", 5) == 0);

    /* Second segment within the window: 2nd seg forces an immediate ACK. */
    fake_tx_reset();
    inject_tcp(2006, srv_iss + 1, TCP_ACK, 65535, "AB", 2, 0, 7);
    inject_tcp(2008, srv_iss + 1, TCP_ACK, 65535, "CD", 2, 0, 7);
    UT_ASSERT_EQ(fake_tx.count, 1);
    UT_ASSERT_EQ(pf_ntohl(last_tcp()->ack), 2010);
}

static void test_out_of_order_reassembly(void)
{
    reset();
    uint32_t srv_iss;
    int cs = establish(3000, 1460, 65535, &srv_iss, NULL);

    /* Second half first: immediate duplicate ACK pointing at the hole. */
    inject_tcp(3006, srv_iss + 1, TCP_ACK, 65535, "WORLD", 5, 0, 7);
    UT_ASSERT_EQ(fake_tx.count, 1);
    UT_ASSERT_EQ(pf_ntohl(last_tcp()->ack), 3001);
    UT_ASSERT_EQ(stk.stats.tcp_ooo_queued, 1);

    /* Hole filled: cumulative ACK covers both. */
    inject_tcp(3001, srv_iss + 1, TCP_ACK, 65535, "HELLO", 5, 0, 7);
    fake_now_ms += TCP_DELACK_MS;
    tcp_tick(&stk, fake_now_ms);
    UT_ASSERT_EQ(pf_ntohl(last_tcp()->ack), 3011);

    char buf[16];
    UT_ASSERT_EQ(pf_recv(&stk, cs, buf, sizeof(buf), 0), 10);
    UT_ASSERT(memcmp(buf, "HELLOWORLD", 10) == 0);
}

static void test_rto_retransmit_and_backoff(void)
{
    reset();
    uint32_t srv_iss;
    int cs = establish(4000, 1460, 65535, &srv_iss, NULL);
    struct tcp_cb *t = pf_sock_get(&stk, cs)->tcb;

    UT_ASSERT_EQ(pf_send(&stk, cs, "PAYLOAD!", 8), 8);
    UT_ASSERT_EQ(fake_tx.count, 1);
    uint32_t rto0 = t->rto_ms;
    UT_ASSERT_EQ(rto0, TCP_RTO_INITIAL_MS); /* RFC 6298 §2: 1 s before samples */

    /* No ACK → timer fires, segment retransmitted, RTO doubles (§5.5). */
    fake_now_ms += rto0;
    tcp_tick(&stk, fake_now_ms);
    UT_ASSERT_EQ(fake_tx.count, 2);
    UT_ASSERT_EQ(pf_ntohl(last_tcp()->seq), srv_iss + 1);
    UT_ASSERT_EQ(t->rto_ms, 2 * rto0);
    UT_ASSERT_EQ(t->cwnd, t->mss); /* RFC 5681 §3.1 eq.4: collapse on timeout */
    UT_ASSERT_EQ(stk.stats.tcp_rto_fires, 1);

    fake_now_ms += t->rto_ms;
    tcp_tick(&stk, fake_now_ms);
    UT_ASSERT_EQ(fake_tx.count, 3);
    UT_ASSERT_EQ(t->rto_ms, 4 * rto0);

    inject_tcp(4001, srv_iss + 1 + 8, TCP_ACK, 65535, NULL, 0, 0, 7);
    UT_ASSERT_EQ(t->snd_len, 0);
    UT_ASSERT_EQ(t->rto_deadline, 0); /* §5.2: all data acked → timer off */
}

static void test_rfc6298_srtt_rttvar_math(void)
{
    reset();
    uint32_t srv_iss;
    int cs = establish(5000, 1460, 65535, &srv_iss, NULL);
    struct tcp_cb *t = pf_sock_get(&stk, cs)->tcb;

    /* First sample R=100: SRTT=100, RTTVAR=50, RTO=100+max(G,200)=300 (§2.2). */
    UT_ASSERT_EQ(pf_send(&stk, cs, "AAAA", 4), 4);
    fake_now_ms += 100;
    inject_tcp(5001, srv_iss + 1 + 4, TCP_ACK, 65535, NULL, 0, 0, 7);
    UT_ASSERT_EQ(t->srtt_ms, 100);
    UT_ASSERT_EQ(t->rttvar_ms, 50);
    UT_ASSERT_EQ(t->rto_ms, 300);

    /* Second sample R=200 (§2.3): RTTVAR=(3·50+|100−200|)/4=62,
     * SRTT=(7·100+200)/8=112, RTO=112+4·62=360. */
    UT_ASSERT_EQ(pf_send(&stk, cs, "BBBB", 4), 4);
    fake_now_ms += 200;
    inject_tcp(5001, srv_iss + 1 + 8, TCP_ACK, 65535, NULL, 0, 0, 7);
    UT_ASSERT_EQ(t->srtt_ms, 112);
    UT_ASSERT_EQ(t->rttvar_ms, 62);
    UT_ASSERT_EQ(t->rto_ms, 360);
    UT_ASSERT_EQ(stk.stats.tcp_rtt_samples, 2);
}

static void test_fast_retransmit_on_three_dupacks(void)
{
    reset();
    uint32_t srv_iss;
    int cs = establish(6000, 1000, 65535, &srv_iss, NULL);
    struct tcp_cb *t = pf_sock_get(&stk, cs)->tcb;
    UT_ASSERT_EQ(t->mss, 1000);

    static uint8_t big[3000];
    memset(big, 'X', sizeof(big));
    UT_ASSERT_EQ(pf_send(&stk, cs, big, sizeof(big)), 3000);
    UT_ASSERT_EQ(fake_tx.count, 3); /* 3 × MSS */
    fake_tx_reset();

    /* Three duplicate ACKs → retransmit of the first segment (§3.2). */
    for (int i = 0; i < 3; i++)
        inject_tcp(6001, srv_iss + 1, TCP_ACK, 65535, NULL, 0, 0, 7);
    UT_ASSERT_EQ(stk.stats.tcp_dupacks_rx, 3);
    UT_ASSERT_EQ(stk.stats.tcp_fast_rtx, 1);
    UT_ASSERT(fake_tx.count >= 1);
    UT_ASSERT_EQ(pf_ntohl(tx_tcp(0)->seq), srv_iss + 1);
    uint16_t plen;
    tx_payload(0, &plen);
    UT_ASSERT_EQ(plen, 1000);
    UT_ASSERT_EQ(t->ssthresh, PF_MAX(3000u / 2, 2000u));

    /* Recovery ACK deflates cwnd to ssthresh. */
    inject_tcp(6001, srv_iss + 1 + 3000, TCP_ACK, 65535, NULL, 0, 0, 7);
    UT_ASSERT_EQ(t->cwnd, t->ssthresh);
}

static void test_challenge_ack_for_inwindow_syn_and_rst(void)
{
    reset();
    uint32_t srv_iss;
    int cs = establish(7000, 1460, 65535, &srv_iss, NULL);
    struct pf_sock *s = pf_sock_get(&stk, cs);

    /* RFC 5961 §4: in-window SYN gets a challenge ACK, not a reset. */
    inject_tcp(7100, srv_iss + 1, TCP_SYN, 65535, NULL, 0, 0, 7);
    UT_ASSERT_EQ(stk.stats.tcp_challenge_acks_tx, 1);
    UT_ASSERT(s->tcb && s->tcb->state == TCP_ESTABLISHED);

    /* RFC 5961 §3.2: in-window-but-inexact RST also challenged. */
    inject_tcp(7100, srv_iss + 1, TCP_RST, 65535, NULL, 0, 0, 7);
    UT_ASSERT_EQ(stk.stats.tcp_challenge_acks_tx, 2);
    UT_ASSERT(s->tcb && s->tcb->state == TCP_ESTABLISHED);

    /* Exact RST kills the connection. */
    inject_tcp(7001, srv_iss + 1, TCP_RST, 65535, NULL, 0, 0, 7);
    UT_ASSERT(s->tcb == NULL);
    UT_ASSERT_EQ(s->err, PF_ECONNRESET);
    UT_ASSERT_EQ(stk.stats.tcp_conns_reset, 1);
}

static void test_passive_close(void)
{
    reset();
    uint32_t srv_iss;
    int cs = establish(8000, 1460, 65535, &srv_iss, NULL);
    struct tcp_cb *t = pf_sock_get(&stk, cs)->tcb;

    /* Peer FIN → our ACK, CLOSE_WAIT, EOF visible to the app. */
    inject_tcp(8001, srv_iss + 1, TCP_FIN | TCP_ACK, 65535, NULL, 0, 0, 7);
    UT_ASSERT_EQ(t->state, TCP_CLOSE_WAIT);
    UT_ASSERT_EQ(fake_tx.count, 1);
    UT_ASSERT_EQ(pf_ntohl(last_tcp()->ack), 8002);
    char buf[4];
    UT_ASSERT_EQ(pf_recv(&stk, cs, buf, sizeof(buf), 0), 0); /* EOF */

    /* Our close → FIN, LAST_ACK; final ACK frees the TCB. */
    fake_tx_reset();
    pf_close(&stk, cs);
    UT_ASSERT_EQ(t->state, TCP_LAST_ACK);
    UT_ASSERT_EQ(last_tcp()->flags & (TCP_FIN | TCP_ACK), TCP_FIN | TCP_ACK);
    inject_tcp(8002, srv_iss + 2, TCP_ACK, 65535, NULL, 0, 0, 7);
    UT_ASSERT(!t->active);
}

static void test_active_close_through_timewait(void)
{
    reset();
    uint32_t srv_iss;
    int cs = establish(9000, 1460, 65535, &srv_iss, NULL);
    struct tcp_cb *t = pf_sock_get(&stk, cs)->tcb;

    pf_close(&stk, cs); /* our FIN */
    UT_ASSERT_EQ(t->state, TCP_FIN_WAIT_1);
    inject_tcp(9001, srv_iss + 2, TCP_ACK, 65535, NULL, 0, 0, 7); /* FIN acked */
    UT_ASSERT_EQ(t->state, TCP_FIN_WAIT_2);
    inject_tcp(9001, srv_iss + 2, TCP_FIN | TCP_ACK, 65535, NULL, 0, 0, 7);
    UT_ASSERT_EQ(t->state, TCP_TIME_WAIT);
    UT_ASSERT_EQ(pf_ntohl(last_tcp()->ack), 9002);

    fake_now_ms += 2 * TCP_MSL_MS;
    tcp_tick(&stk, fake_now_ms);
    UT_ASSERT(!t->active); /* 2MSL elapsed (RFC 9293 §3.10.7) */
}

static void test_send_respects_peer_window(void)
{
    reset();
    uint32_t srv_iss;
    int cs = establish(10000, 1000, 2500, &srv_iss, NULL); /* tiny window */
    static uint8_t big[8000];
    memset(big, 'W', sizeof(big));
    UT_ASSERT_EQ(pf_send(&stk, cs, big, sizeof(big)), 8000);

    /* Only ~2500 bytes (the offered window) may be in flight. */
    uint32_t sent = 0;
    for (int i = 0; i < fake_tx.count; i++) {
        uint16_t plen;
        tx_payload(i, &plen);
        sent += plen;
    }
    UT_ASSERT(sent <= 2500);

    /* ACK + window opening releases more. */
    int before = fake_tx.count;
    inject_tcp(10001, srv_iss + 1 + sent, TCP_ACK, 65535, NULL, 0, 0, 7);
    UT_ASSERT(fake_tx.count > before);
}

static void test_zero_window_probe(void)
{
    reset();
    uint32_t srv_iss;
    int cs = establish(11000, 1000, 1000, &srv_iss, NULL);
    struct tcp_cb *t = pf_sock_get(&stk, cs)->tcb;

    UT_ASSERT_EQ(pf_send(&stk, cs, "0123456789", 10), 10);
    /* Peer acknowledges everything but closes the window. */
    inject_tcp(11001, srv_iss + 11, TCP_ACK, 0, NULL, 0, 0, 7);
    UT_ASSERT_EQ(t->snd_wnd, 0);

    UT_ASSERT_EQ(pf_send(&stk, cs, "PROBE", 5), 5); /* queues, can't send */
    int before = fake_tx.count;
    fake_now_ms += TCP_RTO_INITIAL_MS + 10;
    tcp_tick(&stk, fake_now_ms);
    tcp_tick(&stk, fake_now_ms); /* probe armed then fired */
    fake_now_ms += t->rto_ms + 10;
    tcp_tick(&stk, fake_now_ms);
    UT_ASSERT(stk.stats.tcp_zero_wnd_probes >= 1);
    UT_ASSERT(fake_tx.count > before);

    /* Window reopens → the rest flows. */
    inject_tcp(11001, srv_iss + 12, TCP_ACK, 4096, NULL, 0, 0, 7);
    uint16_t plen;
    tx_payload(fake_tx.count - 1, &plen);
    UT_ASSERT(plen > 0);
}

static void test_malformed_tcp_counted(void)
{
    reset();
    /* Truncated header. */
    struct pkt *p = pkt_alloc();
    struct ipv4_hdr *ih = pkt_put(p, IPV4_HDR_MIN + 10);
    memset(ih, 0, IPV4_HDR_MIN + 10);
    ih->vihl = 0x45;
    ih->total_len = pf_htons(IPV4_HDR_MIN + 10);
    ih->ttl = 64;
    ih->proto = IP_PROTO_TCP;
    ih->saddr = pf_htonl(HOST_IP);
    ih->daddr = pf_htonl(STACK_IP);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    p->dev = dev;
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.tcp_rx_malformed, 1);

    /* doff pointing past the segment. */
    p = pkt_alloc();
    ih = pkt_put(p, IPV4_HDR_MIN);
    memset(ih, 0, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->total_len = pf_htons(IPV4_HDR_MIN + TCP_HDR_MIN);
    ih->ttl = 64;
    ih->proto = IP_PROTO_TCP;
    ih->saddr = pf_htonl(HOST_IP);
    ih->daddr = pf_htonl(STACK_IP);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    struct tcp_hdr *th = pkt_put(p, TCP_HDR_MIN);
    memset(th, 0, TCP_HDR_MIN);
    th->doff = 15 << 4; /* claims 60-byte header in a 20-byte segment */
    p->dev = dev;
    ip_input(&stk, dev, p);
    UT_ASSERT_EQ(stk.stats.tcp_rx_malformed, 2);

    /* Bad checksum. */
    inject_tcp(1, 0, TCP_SYN, 100, NULL, 0, 0, 7); /* no listener: RST path */
    struct pkt *q = pkt_alloc();
    ih = pkt_put(q, IPV4_HDR_MIN);
    memset(ih, 0, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->total_len = pf_htons(IPV4_HDR_MIN + TCP_HDR_MIN);
    ih->ttl = 64;
    ih->proto = IP_PROTO_TCP;
    ih->saddr = pf_htonl(HOST_IP);
    ih->daddr = pf_htonl(STACK_IP);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));
    th = pkt_put(q, TCP_HDR_MIN);
    memset(th, 0, TCP_HDR_MIN);
    th->doff = (TCP_HDR_MIN / 4) << 4;
    th->flags = TCP_SYN;
    th->csum = pf_htons(0xdead);
    q->dev = dev;
    ip_input(&stk, dev, q);
    UT_ASSERT_EQ(stk.stats.tcp_rx_bad_csum, 1);
}

int main(void)
{
    printf("test_tcp:\n");
    UT_RUN(test_handshake_and_accept);
    UT_RUN(test_syn_to_closed_port_gets_rst);
    UT_RUN(test_data_delivery_and_delayed_ack);
    UT_RUN(test_out_of_order_reassembly);
    UT_RUN(test_rto_retransmit_and_backoff);
    UT_RUN(test_rfc6298_srtt_rttvar_math);
    UT_RUN(test_fast_retransmit_on_three_dupacks);
    UT_RUN(test_challenge_ack_for_inwindow_syn_and_rst);
    UT_RUN(test_passive_close);
    UT_RUN(test_active_close_through_timewait);
    UT_RUN(test_send_respects_peer_window);
    UT_RUN(test_zero_window_probe);
    UT_RUN(test_malformed_tcp_counted);
    pf_stack_fini(&stk);
    printf("test_tcp: all passed\n");
    return 0;
}
