/*
 * TCP — RFC 793 as amended by RFC 1122 §4.2, with the RFC 9293 (793bis)
 * consolidated event processing, RFC 6298 retransmission timing, RFC 5681
 * congestion control (slow start, congestion avoidance, fast retransmit)
 * and RFC 5961 challenge ACKs.
 */
#ifndef PF_TCP_TCP_H
#define PF_TCP_TCP_H

#include "core/pkt.h"
#include "ipv4/ipv4.h"

#define TCP_HDR_MIN     20
#define TCP_HDR_MAX     60

#define TCP_FIN         0x01
#define TCP_SYN         0x02
#define TCP_RST         0x04
#define TCP_PSH         0x08
#define TCP_ACK         0x10
#define TCP_URG         0x20

#define TCP_OPT_END     0
#define TCP_OPT_NOP     1
#define TCP_OPT_MSS     2

#define TCP_MAX_CONNS   16
#define TCP_DEFAULT_MSS 536  /* RFC 1122 §4.2.2.6 when no MSS option received */
#define TCP_OUR_MSS     1460 /* 1500 MTU − 40 */
#define TCP_SNDBUF      65535
#define TCP_RCVBUF      65535 /* max advertised window; no window scaling (D-013) */
#define TCP_OOO_MAX     16    /* out-of-order segments parked per connection */
#define TCP_ACCEPT_QLEN 8

/* RFC 6298 timing. Lower bound 200 ms instead of the RFC's conservative
 * 1 s — the same choice Linux makes; see DECISIONS.md D-014. */
#define TCP_RTO_MIN_MS     200
#define TCP_RTO_MAX_MS     60000
#define TCP_RTO_INITIAL_MS 1000 /* RFC 6298 §2 */
#define TCP_CLOCK_G_MS     10   /* clock granularity */
#define TCP_MAX_RTX        10   /* give up after ~R2 retries (RFC 1122 §4.2.3.5) */

#define TCP_MSL_MS         5000 /* 2MSL TIME_WAIT = 10 s (test-friendly, D-015) */
#define TCP_DELACK_MS      40   /* delayed ACK (≤500 ms per RFC 1122 §4.2.3.2) */

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

struct tcp_hdr {
    uint16_t sport; /* network byte order */
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t doff; /* data offset (high 4 bits), in 32-bit words */
    uint8_t flags;
    uint16_t wnd;
    uint16_t csum;
    uint16_t urg;
} __attribute__((packed));

static inline uint8_t tcp_hdr_len(const struct tcp_hdr *th)
{
    return (uint8_t)((th->doff >> 4) * 4);
}

/* Modular 32-bit sequence comparisons (RFC 793 §3.3). */
static inline bool seq_lt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}
static inline bool seq_le(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}
static inline bool seq_gt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}
static inline bool seq_ge(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

struct pf_stack;
struct netdev;

struct tcp_cb {
    bool active;
    enum tcp_state state;
    struct pf_stack *stack;
    int sock_idx; /* owning pf_sock, -1 when detached */

    uint32_t local_ip, remote_ip; /* host byte order */
    uint16_t local_port, remote_port;

    /* Send sequence space (RFC 9293 §3.3.1). */
    uint32_t iss, snd_una, snd_nxt;
    uint32_t snd_wnd, snd_wl1, snd_wl2;

    /* Receive sequence space. */
    uint32_t irs, rcv_nxt;
    uint16_t rcv_wnd_advertised; /* last window we advertised */

    uint16_t mss; /* effective send MSS */

    /* Send byte-ring. Sequence snd_una maps to sndbuf[snd_head] once the
     * SYN is acknowledged; SYN/FIN occupy sequence space but no buffer. */
    uint8_t *sndbuf;
    uint32_t snd_head, snd_len;
    bool fin_queued; /* app closed; emit FIN once the ring drains */
    bool fin_sent;   /* FIN occupies snd_nxt−1 */

    /* Receive ring (in-order, app-readable) + out-of-order parking. */
    uint8_t *rcvbuf;
    uint32_t rcv_head, rcv_len;
    bool rcv_eof; /* FIN consumed */
    struct pktq ooo;

    /* RFC 6298 retransmission state. */
    int64_t srtt_ms, rttvar_ms; /* −1 until the first sample */
    uint32_t rto_ms;
    uint64_t rto_deadline; /* 0 = timer off */
    uint32_t rtx_count;
    bool rtt_timing;
    uint32_t rtt_seq;
    uint64_t rtt_start_ms;

    /* RFC 5681 congestion control. */
    uint32_t cwnd, ssthresh;
    uint32_t dupacks;

    bool nodelay; /* Nagle off when true (RFC 896 algorithm by default) */

    uint64_t timewait_deadline;
    uint64_t delack_deadline;
    uint8_t delack_pending; /* segments received since last ACK sent */

    /* Listener side. */
    uint16_t backlog;
    struct tcp_cb *acceptq[TCP_ACCEPT_QLEN];
    int acceptq_n;
    struct tcp_cb *listener; /* for embryonic children */
};

struct tcp_globals {
    struct tcp_cb tcbs[TCP_MAX_CONNS];
    uint32_t iss_next;
};

void tcp_init(struct tcp_globals *tg);
void tcp_fini(struct tcp_globals *tg);

/* Handle a received TCP segment (p->data at the IP header). Consumes p. */
void tcp_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p,
               const struct ipv4_hdr *ih);

void tcp_tick(struct pf_stack *stack, uint64_t now_ms);

/* Connection-level API used by the socket layer (tcp_sock.c). */
struct tcp_cb *tcb_alloc(struct pf_stack *stack);
void tcb_free(struct pf_stack *stack, struct tcp_cb *tcb);
struct tcp_cb *tcp_open_listen(struct pf_stack *stack, int sock_idx, uint32_t ip, uint16_t port,
                               uint16_t backlog);
struct tcp_cb *tcp_open_active(struct pf_stack *stack, int sock_idx, uint32_t lip, uint16_t lport,
                               uint32_t rip, uint16_t rport);
void tcp_app_close(struct pf_stack *stack, struct tcp_cb *tcb);
void tcp_app_abort(struct pf_stack *stack, struct tcp_cb *tcb); /* RST + free */

/* Push pending data/FIN within the send window (Nagle-aware). */
void tcp_output(struct pf_stack *stack, struct tcp_cb *tcb);

/* Free space in the send ring / bytes readable in the receive ring. */
static inline uint32_t tcp_snd_space(const struct tcp_cb *tcb)
{
    return TCP_SNDBUF - tcb->snd_len;
}

/* Socket-layer error codes surfaced via pf_sock.err. */
#define PF_ECONNRESET   1
#define PF_ECONNREFUSED 2
#define PF_ETIMEDOUT    3

/* Internals shared between tcp*.c files. */
void tcp_send_ctl(struct pf_stack *stack, struct tcp_cb *tcb, uint8_t flags);
void tcp_send_reset_for(struct pf_stack *stack, const struct ipv4_hdr *ih, const struct tcp_hdr *th,
                        uint32_t seg_paylen);
void tcp_retransmit(struct pf_stack *stack, struct tcp_cb *tcb);
void tcp_send_probe(struct pf_stack *stack, struct tcp_cb *tcb);
void tcp_conn_error(struct pf_stack *stack, struct tcp_cb *tcb, int err);
uint16_t tcp_rcv_window(const struct tcp_cb *tcb);
void tcp_arm_rto(struct tcp_cb *tcb, uint64_t now);

#endif /* PF_TCP_TCP_H */
