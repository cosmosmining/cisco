#include "fakes.h"
#include "ut.h"

#include "route/fib.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

static struct pf_stack stk;
static struct netdev *d0, *d1;

static void reset(void)
{
    fake_tx_reset();
    fake_ndevs = 0;
    fake_now_ms = 1000000;
    pf_stack_fini(&stk);
    pf_stack_init(&stk);
    d0 = fake_dev_add(&stk, IP(10, 1, 0, 2), 0xffffff00);
    d1 = fake_dev_add(&stk, IP(10, 2, 0, 2), 0xffffff00);
}

static void test_longest_prefix_wins(void)
{
    reset();
    struct fib *f = &stk.fib;
    UT_ASSERT_EQ(fib_add(f, IP(10, 0, 0, 0), 8, IP(10, 1, 0, 1), d0, false), 0);
    UT_ASSERT_EQ(fib_add(f, IP(10, 99, 0, 0), 16, IP(10, 2, 0, 1), d1, false), 0);
    UT_ASSERT_EQ(fib_add(f, IP(10, 99, 7, 0), 24, IP(10, 1, 0, 7), d0, false), 0);
    UT_ASSERT_EQ(fib_add(f, IP(10, 99, 7, 42), 32, IP(10, 2, 0, 9), d1, false), 0);

    struct fib_entry *e;
    e = fib_lookup(f, IP(10, 5, 5, 5)); /* only /8 covers */
    UT_ASSERT(e && e->plen == 8);
    e = fib_lookup(f, IP(10, 99, 200, 1)); /* /16 beats /8 */
    UT_ASSERT(e && e->plen == 16);
    e = fib_lookup(f, IP(10, 99, 7, 100)); /* /24 beats /16 */
    UT_ASSERT(e && e->plen == 24 && e->next_hop == IP(10, 1, 0, 7));
    e = fib_lookup(f, IP(10, 99, 7, 42)); /* host route beats all */
    UT_ASSERT(e && e->plen == 32 && e->next_hop == IP(10, 2, 0, 9));
}

static void test_default_route_and_no_match(void)
{
    reset();
    struct fib *f = &stk.fib;
    /* Connected routes exist for 10.1.0/24 and 10.2.0/24; nothing else. */
    UT_ASSERT(fib_lookup(f, IP(192, 168, 9, 9)) == NULL);

    UT_ASSERT_EQ(fib_add(f, 0, 0, IP(10, 1, 0, 1), d0, false), 0);
    struct fib_entry *e = fib_lookup(f, IP(192, 168, 9, 9));
    UT_ASSERT(e && e->plen == 0 && e->next_hop == IP(10, 1, 0, 1));

    /* More-specific still wins over default. */
    e = fib_lookup(f, IP(10, 2, 0, 77));
    UT_ASSERT(e && e->plen == 24 && e->connected);
}

static void test_delete_restores_shorter_match(void)
{
    reset();
    struct fib *f = &stk.fib;
    fib_add(f, IP(10, 50, 0, 0), 16, IP(10, 1, 0, 1), d0, false);
    fib_add(f, IP(10, 50, 1, 0), 24, IP(10, 2, 0, 1), d1, false);

    struct fib_entry *e = fib_lookup(f, IP(10, 50, 1, 9));
    UT_ASSERT(e && e->plen == 24);

    UT_ASSERT_EQ(fib_del(f, IP(10, 50, 1, 0), 24), 0);
    e = fib_lookup(f, IP(10, 50, 1, 9));
    UT_ASSERT(e && e->plen == 16); /* falls back to the /16 */

    UT_ASSERT_EQ(fib_del(f, IP(10, 50, 1, 0), 24), -1); /* already gone */
}

static void test_add_via_requires_connected_next_hop(void)
{
    reset();
    /* Next hop on a connected subnet → OK. */
    UT_ASSERT_EQ(fib_add_via(&stk, IP(172, 16, 0, 0), 16, IP(10, 1, 0, 99)), 0);
    struct fib_entry *e = fib_lookup(&stk.fib, IP(172, 16, 5, 5));
    UT_ASSERT(e && e->dev == d0 && e->next_hop == IP(10, 1, 0, 99));

    /* Unreachable next hop → rejected. */
    UT_ASSERT_EQ(fib_add_via(&stk, IP(172, 17, 0, 0), 16, IP(9, 9, 9, 9)), -1);
}

static void count_cb(const struct fib_entry *e, void *arg)
{
    (void)e;
    (*(int *)arg)++;
}

static void test_walk_and_replace(void)
{
    reset();
    int n = 0;
    fib_walk(&stk.fib, count_cb, &n);
    UT_ASSERT_EQ(n, 2); /* the two connected routes */

    fib_add(&stk.fib, IP(10, 70, 0, 0), 24, IP(10, 1, 0, 1), d0, false);
    fib_add(&stk.fib, IP(10, 70, 0, 0), 24, IP(10, 1, 0, 2), d0, false); /* replace */
    n = 0;
    fib_walk(&stk.fib, count_cb, &n);
    UT_ASSERT_EQ(n, 3);
    struct fib_entry *e = fib_lookup(&stk.fib, IP(10, 70, 0, 1));
    UT_ASSERT(e && e->next_hop == IP(10, 1, 0, 2));
}

int main(void)
{
    printf("test_fib:\n");
    UT_RUN(test_longest_prefix_wins);
    UT_RUN(test_default_route_and_no_match);
    UT_RUN(test_delete_restores_shorter_match);
    UT_RUN(test_add_via_requires_connected_next_hop);
    UT_RUN(test_walk_and_replace);
    pf_stack_fini(&stk);
    printf("test_fib: all passed\n");
    return 0;
}
