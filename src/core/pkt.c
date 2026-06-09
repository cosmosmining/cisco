#include "core/pkt.h"

#include <stdlib.h>
#include <string.h>

struct pkt *pkt_alloc(void)
{
    struct pkt *p = calloc(1, sizeof(*p));
    PF_ASSERT(p != NULL);
    p->data = p->buf + PKT_RX_OFFSET;
    p->len = 0;
    return p;
}

void pkt_free(struct pkt *p)
{
    if (p)
        free(p);
}

void pkt_reserve(struct pkt *p, uint16_t headroom)
{
    PF_ASSERT(p->len == 0 && headroom <= PKT_BUF_SIZE);
    p->data = p->buf + headroom;
}

void *pkt_push(struct pkt *p, uint16_t n)
{
    PF_ASSERT(pkt_headroom(p) >= n);
    p->data -= n;
    p->len = (uint16_t)(p->len + n);
    return p->data;
}

void *pkt_pull(struct pkt *p, uint16_t n)
{
    PF_ASSERT(p->len >= n);
    p->data += n;
    p->len = (uint16_t)(p->len - n);
    return p->data;
}

void *pkt_put(struct pkt *p, uint16_t n)
{
    PF_ASSERT(pkt_tailroom(p) >= n);
    void *tail = p->data + p->len;
    p->len = (uint16_t)(p->len + n);
    return tail;
}

void pkt_trim(struct pkt *p, uint16_t newlen)
{
    PF_ASSERT(newlen <= p->len);
    p->len = newlen;
}

void pktq_init(struct pktq *q)
{
    q->head = q->tail = NULL;
    q->n = 0;
}

void pktq_push(struct pktq *q, struct pkt *p)
{
    p->next = NULL;
    if (q->tail)
        q->tail->next = p;
    else
        q->head = p;
    q->tail = p;
    q->n++;
}

struct pkt *pktq_pop(struct pktq *q)
{
    struct pkt *p = q->head;
    if (!p)
        return NULL;
    q->head = p->next;
    if (!q->head)
        q->tail = NULL;
    p->next = NULL;
    q->n--;
    return p;
}

void pktq_free_all(struct pktq *q)
{
    struct pkt *p;
    while ((p = pktq_pop(q)) != NULL)
        pkt_free(p);
}
