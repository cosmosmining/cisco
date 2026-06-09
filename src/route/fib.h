/*
 * FIB: longest-prefix-match over a binary (unibit) trie.
 *
 * Lookup walks destination bits MSB-first, remembering the last node that
 * carried a route — that's the longest match when the walk ends. Inserts
 * are O(plen), lookups O(32) worst case with no backtracking.
 */
#ifndef PF_ROUTE_FIB_H
#define PF_ROUTE_FIB_H

#include "core/pf.h"

#define FIB_MAX_ROUTES 64

struct netdev;
struct pf_stack;

struct fib_entry {
    bool in_use;
    uint32_t prefix; /* host byte order, masked */
    uint8_t plen;
    uint32_t next_hop; /* 0 = directly connected */
    struct netdev *dev;
    bool connected; /* auto-installed from an interface address */
};

struct fib_node {
    struct fib_node *child[2];
    struct fib_entry *entry; /* route terminating at this node, or NULL */
};

struct fib {
    struct fib_node *root;
    struct fib_entry pool[FIB_MAX_ROUTES];
};

void fib_init(struct fib *f);
void fib_fini(struct fib *f);

/* Returns 0, -1 on pool exhaustion/bad args. Replaces an existing entry
 * with the same prefix/plen. */
int fib_add(struct fib *f, uint32_t prefix, uint8_t plen, uint32_t next_hop, struct netdev *dev,
            bool connected);

/* Returns 0, -1 if no such route. Connected routes can also be removed. */
int fib_del(struct fib *f, uint32_t prefix, uint8_t plen);

/* Longest-prefix match; NULL if no route covers dst. */
struct fib_entry *fib_lookup(struct fib *f, uint32_t dst);

/* Convenience for CLI/apps: derive the egress device from the next hop
 * (which must be reachable through a connected route). */
int fib_add_via(struct pf_stack *stack, uint32_t prefix, uint8_t plen, uint32_t next_hop);

/* Iterate all routes (for `show ip route`). */
typedef void (*fib_walk_cb)(const struct fib_entry *e, void *arg);
void fib_walk(struct fib *f, fib_walk_cb cb, void *arg);

#endif /* PF_ROUTE_FIB_H */
