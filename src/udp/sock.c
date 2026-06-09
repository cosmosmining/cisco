#include "udp/sock.h"
#include "core/stack.h"
#include "udp/udp.h"

#include <string.h>

void pf_socktab_init(struct pf_socktab *t)
{
    memset(t, 0, sizeof(*t));
    t->ephemeral_next = PF_EPHEMERAL_BASE;
    for (int i = 0; i < PF_SOCK_MAX; i++)
        pktq_init(&t->socks[i].rxq);
}

void pf_socktab_fini(struct pf_socktab *t)
{
    for (int i = 0; i < PF_SOCK_MAX; i++)
        pktq_free_all(&t->socks[i].rxq);
}

struct pf_sock *pf_sock_get(struct pf_stack *stack, int sock)
{
    if (sock < 0 || sock >= PF_SOCK_MAX || stack->socks.socks[sock].type == PF_SOCK_NONE)
        return NULL;
    return &stack->socks.socks[sock];
}

bool pf_sock_port_in_use(struct pf_stack *stack, enum pf_sock_type type, uint32_t ip, uint16_t port)
{
    for (int i = 0; i < PF_SOCK_MAX; i++) {
        struct pf_sock *s = &stack->socks.socks[i];
        if (s->type != type || s->local_port != port)
            continue;
        if (s->local_ip == 0 || ip == 0 || s->local_ip == ip)
            return true;
    }
    return false;
}

uint16_t pf_sock_ephemeral_port(struct pf_stack *stack)
{
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t port = stack->socks.ephemeral_next++;
        if (stack->socks.ephemeral_next == 0)
            stack->socks.ephemeral_next = PF_EPHEMERAL_BASE;
        if (port >= PF_EPHEMERAL_BASE && !pf_sock_port_in_use(stack, PF_SOCK_UDP, 0, port) &&
            !pf_sock_port_in_use(stack, PF_SOCK_TCP, 0, port))
            return port;
    }
    return 0;
}

int pf_socket(struct pf_stack *stack, enum pf_sock_type type)
{
    if (type != PF_SOCK_UDP && type != PF_SOCK_TCP)
        return -1;
    for (int i = 0; i < PF_SOCK_MAX; i++) {
        struct pf_sock *s = &stack->socks.socks[i];
        if (s->type == PF_SOCK_NONE) {
            memset(s, 0, sizeof(*s));
            pktq_init(&s->rxq);
            s->type = type;
            return i;
        }
    }
    return -1;
}

int pf_bind(struct pf_stack *stack, int sock, uint32_t local_ip, uint16_t port)
{
    struct pf_sock *s = pf_sock_get(stack, sock);
    if (!s || port == 0)
        return -1;
    if (pf_sock_port_in_use(stack, s->type, local_ip, port))
        return -1;
    s->local_ip = local_ip;
    s->local_port = port;
    return 0;
}

int pf_close(struct pf_stack *stack, int sock)
{
    struct pf_sock *s = pf_sock_get(stack, sock);
    if (!s)
        return -1;
    pktq_free_all(&s->rxq);
    memset(s, 0, sizeof(*s));
    pktq_init(&s->rxq);
    return 0;
}

struct pf_sock *pf_sock_lookup_udp(struct pf_stack *stack, uint32_t dst_ip, uint16_t dport)
{
    struct pf_sock *wildcard = NULL;
    for (int i = 0; i < PF_SOCK_MAX; i++) {
        struct pf_sock *s = &stack->socks.socks[i];
        if (s->type != PF_SOCK_UDP || s->local_port != dport)
            continue;
        if (s->local_ip == dst_ip)
            return s; /* exact beats wildcard */
        if (s->local_ip == 0)
            wildcard = s;
    }
    return wildcard;
}

int pf_sendto(struct pf_stack *stack, int sock, const void *buf, size_t len, uint32_t dst_ip,
              uint16_t dst_port)
{
    struct pf_sock *s = pf_sock_get(stack, sock);
    if (!s || s->type != PF_SOCK_UDP || dst_port == 0)
        return -1;
    if (len > PKT_BUF_SIZE - PKT_TX_HEADROOM)
        return -1;

    if (s->local_port == 0) { /* auto-bind, like the BSD API */
        uint16_t port = pf_sock_ephemeral_port(stack);
        if (port == 0)
            return -1;
        s->local_port = port;
    }

    struct pkt *p = pkt_alloc();
    pkt_reserve(p, PKT_TX_HEADROOM);
    if (len)
        memcpy(pkt_put(p, (uint16_t)len), buf, len);
    return udp_output(stack, s->local_ip, s->local_port, dst_ip, dst_port, p);
}

long pf_recvfrom(struct pf_stack *stack, int sock, void *buf, size_t cap, uint32_t *src_ip,
                 uint16_t *src_port, int timeout_ms)
{
    struct pf_sock *s = pf_sock_get(stack, sock);
    if (!s || s->type != PF_SOCK_UDP)
        return -1;

    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : pf_now_ms() + (uint64_t)timeout_ms;
    while (pktq_empty(&s->rxq)) {
        if (!stack->poll_fn || pf_now_ms() >= deadline)
            return 0;
        stack->poll_fn(stack, 10); /* pump the event loop (D-002) */
    }

    struct pkt *p = pktq_pop(&s->rxq);
    size_t n = PF_MIN((size_t)p->len, cap);
    memcpy(buf, p->data, n);
    if (src_ip)
        *src_ip = p->meta_ip;
    if (src_port)
        *src_port = p->meta_port;
    pkt_free(p);
    return (long)n;
}
