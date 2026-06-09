/*
 * Minimal blocking socket API (pf_socket/pf_bind/pf_sendto/pf_recvfrom).
 *
 * Blocking calls pump the stack's event loop via stack->poll_fn until their
 * wakeup condition holds (single-threaded model, DECISIONS.md D-002), so
 * protocol code never touches the platform directly.
 */
#ifndef PF_UDP_SOCK_H
#define PF_UDP_SOCK_H

#include "core/pkt.h"

#define PF_SOCK_MAX       32
#define PF_SOCK_RXQ_CAP   64
#define PF_EPHEMERAL_BASE 49152u /* IANA dynamic range */

enum pf_sock_type { PF_SOCK_NONE = 0, PF_SOCK_UDP, PF_SOCK_TCP };

struct pf_stack;
struct tcp_cb;

struct pf_sock {
    enum pf_sock_type type; /* PF_SOCK_NONE = free slot */
    uint32_t local_ip;      /* host order; 0 = any */
    uint16_t local_port;    /* host order; 0 = unbound */
    struct pktq rxq;        /* UDP: payload pkts with meta_ip/meta_port set */
    struct tcp_cb *tcb;     /* phase 4 */
};

struct pf_socktab {
    struct pf_sock socks[PF_SOCK_MAX];
    uint16_t ephemeral_next;
};

void pf_socktab_init(struct pf_socktab *t);
void pf_socktab_fini(struct pf_socktab *t);

/* All return >=0 / 0 on success, -1 on error. Socket ids are small ints. */
int pf_socket(struct pf_stack *stack, enum pf_sock_type type);
int pf_bind(struct pf_stack *stack, int sock, uint32_t local_ip, uint16_t port);
int pf_close(struct pf_stack *stack, int sock);

int pf_sendto(struct pf_stack *stack, int sock, const void *buf, size_t len, uint32_t dst_ip,
              uint16_t dst_port);

/* Blocks up to timeout_ms (-1 = forever, 0 = poll). Returns bytes received,
 * 0 on timeout, -1 on error. Fills src_ip/src_port when non-NULL. */
long pf_recvfrom(struct pf_stack *stack, int sock, void *buf, size_t cap, uint32_t *src_ip,
                 uint16_t *src_port, int timeout_ms);

/* Internals shared with udp.c / tcp.c. */
struct pf_sock *pf_sock_get(struct pf_stack *stack, int sock);
struct pf_sock *pf_sock_lookup_udp(struct pf_stack *stack, uint32_t dst_ip, uint16_t dport);
uint16_t pf_sock_ephemeral_port(struct pf_stack *stack);
bool pf_sock_port_in_use(struct pf_stack *stack, enum pf_sock_type type, uint32_t ip,
                         uint16_t port);

#endif /* PF_UDP_SOCK_H */
