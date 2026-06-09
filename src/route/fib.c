#include "route/fib.h"
#include "core/stack.h"
#include "netdev/netdev.h"

#include <stdlib.h>
#include <string.h>

void fib_init(struct fib *f)
{
    memset(f, 0, sizeof(*f));
}

static void node_free_rec(struct fib_node *n)
{
    if (!n)
        return;
    node_free_rec(n->child[0]);
    node_free_rec(n->child[1]);
    free(n);
}

void fib_fini(struct fib *f)
{
    node_free_rec(f->root);
    memset(f, 0, sizeof(*f));
}

static struct fib_entry *pool_alloc(struct fib *f)
{
    for (int i = 0; i < FIB_MAX_ROUTES; i++)
        if (!f->pool[i].in_use)
            return &f->pool[i];
    return NULL;
}

static inline int bit_at(uint32_t addr, uint8_t depth)
{
    return (addr >> (31 - depth)) & 1u;
}

int fib_add(struct fib *f, uint32_t prefix, uint8_t plen, uint32_t next_hop, struct netdev *dev,
            bool connected)
{
    if (plen > 32 || !dev)
        return -1;
    uint32_t mask = plen == 0 ? 0 : 0xffffffffu << (32 - plen);
    prefix &= mask;

    if (!f->root)
        f->root = calloc(1, sizeof(*f->root));
    struct fib_node *n = f->root;
    PF_ASSERT(n);
    for (uint8_t d = 0; d < plen; d++) {
        int b = bit_at(prefix, d);
        if (!n->child[b]) {
            n->child[b] = calloc(1, sizeof(*n));
            PF_ASSERT(n->child[b]);
        }
        n = n->child[b];
    }

    struct fib_entry *e = n->entry;
    if (!e) {
        e = pool_alloc(f);
        if (!e)
            return -1;
        n->entry = e;
    }
    e->in_use = true;
    e->prefix = prefix;
    e->plen = plen;
    e->next_hop = next_hop;
    e->dev = dev;
    e->connected = connected;
    return 0;
}

/* Recursive removal with pruning of empty branches. Returns true when the
 * subtree below (and including) n became empty and was freed. */
static bool del_rec(struct fib_node *n, uint32_t prefix, uint8_t plen, uint8_t depth, bool *found)
{
    if (!n)
        return false;
    if (depth == plen) {
        if (n->entry) {
            n->entry->in_use = false;
            n->entry = NULL;
            *found = true;
        }
    } else {
        int b = bit_at(prefix, depth);
        if (del_rec(n->child[b], prefix, plen, (uint8_t)(depth + 1), found))
            n->child[b] = NULL;
    }
    if (!n->entry && !n->child[0] && !n->child[1] && depth > 0) {
        free(n);
        return true;
    }
    return false;
}

int fib_del(struct fib *f, uint32_t prefix, uint8_t plen)
{
    if (plen > 32)
        return -1;
    uint32_t mask = plen == 0 ? 0 : 0xffffffffu << (32 - plen);
    bool found = false;
    del_rec(f->root, prefix & mask, plen, 0, &found);
    return found ? 0 : -1;
}

struct fib_entry *fib_lookup(struct fib *f, uint32_t dst)
{
    struct fib_node *n = f->root;
    struct fib_entry *best = NULL;
    uint8_t depth = 0;
    while (n) {
        if (n->entry)
            best = n->entry; /* longest match so far */
        if (depth == 32)
            break;
        n = n->child[bit_at(dst, depth)];
        depth++;
    }
    return best;
}

int fib_add_via(struct pf_stack *stack, uint32_t prefix, uint8_t plen, uint32_t next_hop)
{
    /* The next hop must already be reachable via a connected route
     * (single-level recursion, like `ip route` with an unresolved gateway). */
    struct fib_entry *via = fib_lookup(&stack->fib, next_hop);
    if (!via || !via->connected)
        return -1;
    return fib_add(&stack->fib, prefix, plen, next_hop, via->dev, false);
}

static void walk_rec(struct fib_node *n, fib_walk_cb cb, void *arg)
{
    if (!n)
        return;
    if (n->entry)
        cb(n->entry, arg);
    walk_rec(n->child[0], cb, arg);
    walk_rec(n->child[1], cb, arg);
}

void fib_walk(struct fib *f, fib_walk_cb cb, void *arg)
{
    walk_rec(f->root, cb, arg);
}
