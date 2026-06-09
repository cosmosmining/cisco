/* TCP face of the minimal socket API: listen/accept/connect/send/recv. */
#include "core/stack.h"
#include "netdev/netdev.h"
#include "tcp/tcp.h"
#include "udp/sock.h"

#include <string.h>

static bool pump(struct pf_stack *stack)
{
    if (!stack->poll_fn)
        return false;
    stack->poll_fn(stack, 10);
    return true;
}

int pf_listen(struct pf_stack *stack, int sock, int backlog)
{
    struct pf_sock *s = pf_sock_get(stack, sock);
    if (!s || s->type != PF_SOCK_TCP || s->local_port == 0 || s->tcb)
        return -1;
    s->tcb = tcp_open_listen(stack, sock, s->local_ip, s->local_port, (uint16_t)backlog);
    return s->tcb ? 0 : -1;
}

int pf_accept(struct pf_stack *stack, int sock, uint32_t *rip, uint16_t *rport, int timeout_ms)
{
    struct pf_sock *s = pf_sock_get(stack, sock);
    if (!s || s->type != PF_SOCK_TCP || !s->tcb || s->tcb->state != TCP_LISTEN)
        return -1;

    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : pf_now_ms() + (uint64_t)timeout_ms;
    struct tcp_cb *l = s->tcb;
    while (l->acceptq_n == 0) {
        if (!s->tcb)
            return -1; /* listener died */
        if (pf_now_ms() >= deadline || !pump(stack))
            return -2; /* timeout */
    }

    struct tcp_cb *child = l->acceptq[0];
    for (int i = 1; i < l->acceptq_n; i++)
        l->acceptq[i - 1] = l->acceptq[i];
    l->acceptq_n--;

    int ns = pf_socket(stack, PF_SOCK_TCP);
    if (ns < 0) {
        tcp_app_abort(stack, child);
        return -1;
    }
    struct pf_sock *cs = &stack->socks.socks[ns];
    cs->local_ip = child->local_ip;
    cs->local_port = child->local_port;
    cs->tcb = child;
    child->sock_idx = ns;
    child->listener = NULL;
    if (rip)
        *rip = child->remote_ip;
    if (rport)
        *rport = child->remote_port;
    return ns;
}

int pf_connect(struct pf_stack *stack, int sock, uint32_t dst_ip, uint16_t dst_port, int timeout_ms)
{
    struct pf_sock *s = pf_sock_get(stack, sock);
    if (!s || s->type != PF_SOCK_TCP || s->tcb || dst_port == 0)
        return -1;

    uint32_t lip = s->local_ip;
    if (lip == 0) {
        uint32_t nh;
        struct netdev *dev = ip_route_lookup(stack, dst_ip, &nh);
        if (!dev)
            return -1;
        lip = dev->ip;
    }
    if (s->local_port == 0) {
        uint16_t port = pf_sock_ephemeral_port(stack);
        if (port == 0)
            return -1;
        s->local_port = port;
    }
    s->local_ip = lip;
    s->err = 0;

    s->tcb = tcp_open_active(stack, sock, lip, s->local_port, dst_ip, dst_port);
    if (!s->tcb)
        return -1;

    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : pf_now_ms() + (uint64_t)timeout_ms;
    while (s->tcb && s->tcb->state != TCP_ESTABLISHED) {
        if (pf_now_ms() >= deadline || !pump(stack)) {
            if (s->tcb) {
                tcp_app_abort(stack, s->tcb);
                s->tcb = NULL;
            }
            return -2;
        }
    }
    return s->tcb ? 0 : -1; /* NULL = refused/reset while connecting */
}

long pf_send(struct pf_stack *stack, int sock, const void *buf, size_t len)
{
    struct pf_sock *s = pf_sock_get(stack, sock);
    if (!s || s->type != PF_SOCK_TCP)
        return -1;

    const uint8_t *src = buf;
    size_t sent = 0;
    while (sent < len) {
        struct tcp_cb *t = s->tcb;
        if (!t || (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT))
            return -1; /* reset or closed underneath us */

        uint32_t space = tcp_snd_space(t);
        if (space == 0) {
            if (!pump(stack))
                return -1; /* would block forever without a loop */
            continue;
        }
        uint32_t n = PF_MIN((uint32_t)(len - sent), space);
        uint32_t w = (t->snd_head + t->snd_len) % TCP_SNDBUF;
        uint32_t first = PF_MIN(n, TCP_SNDBUF - w);
        memcpy(t->sndbuf + w, src + sent, first);
        memcpy(t->sndbuf, src + sent + first, n - first);
        t->snd_len += n;
        sent += n;
        tcp_output(stack, t);
    }
    return (long)sent;
}

long pf_recv(struct pf_stack *stack, int sock, void *buf, size_t cap, int timeout_ms)
{
    struct pf_sock *s = pf_sock_get(stack, sock);
    if (!s || s->type != PF_SOCK_TCP)
        return -1;

    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : pf_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        struct tcp_cb *t = s->tcb;
        if (t && t->rcv_len > 0) {
            uint32_t n = PF_MIN((uint32_t)cap, t->rcv_len);
            uint32_t first = PF_MIN(n, TCP_RCVBUF - t->rcv_head);
            memcpy(buf, t->rcvbuf + t->rcv_head, first);
            memcpy((uint8_t *)buf + first, t->rcvbuf, n - first);
            t->rcv_head = (t->rcv_head + n) % TCP_RCVBUF;
            t->rcv_len -= n;

            /* Receiver-side SWS avoidance (RFC 1122 §4.2.3.3): announce the
             * reopened window promptly once it's at least one MSS. */
            if (t->rcv_wnd_advertised == 0 && tcp_rcv_window(t) >= t->mss)
                tcp_send_ctl(stack, t, TCP_ACK);
            return (long)n;
        }
        if (t && t->rcv_eof)
            return 0; /* clean EOF */
        if (!t)
            return s->err ? -1 : 0;
        if (pf_now_ms() >= deadline || !pump(stack))
            return -2; /* timeout */
    }
}

int pf_sock_set_nodelay(struct pf_stack *stack, int sock, bool nodelay)
{
    struct pf_sock *s = pf_sock_get(stack, sock);
    if (!s || s->type != PF_SOCK_TCP || !s->tcb)
        return -1;
    s->tcb->nodelay = nodelay; /* Nagle toggle (RFC 896) */
    return 0;
}
