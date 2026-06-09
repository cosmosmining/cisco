/*
 * Fuzz harness for the frame parsers (eth/arp/ipv4/icmp/udp/tcp/fwd).
 *
 * Reads one raw Ethernet frame from stdin and runs it through eth_input on
 * a two-interface forwarding stack with live UDP/TCP listeners, so demux,
 * reassembly, the TCP state machine and the forwarding path are all
 * reachable. Built with afl-clang-fast it runs in persistent mode; built
 * normally it replays single inputs / the corpus (regression mode).
 *
 * No TAP, no syscalls on the datapath: tx is discarded, time is synthetic.
 */
#include "core/stack.h"
#include "eth/eth.h"
#include "netdev/netdev.h"
#include "route/fib.h"
#include "udp/sock.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint64_t fake_now;
uint64_t pf_now_ms(void)
{
    return fake_now += 3; /* advances on every query: timers fire eventually */
}

static int null_tx(struct netdev *dev, struct pkt *p)
{
    (void)dev;
    (void)p;
    return 0;
}

static struct pf_stack stk;
static struct netdev devs[2];

static void stack_setup(void)
{
    pf_stack_fini(&stk);
    pf_stack_init(&stk);
    stk.forwarding = true;

    for (int i = 0; i < 2; i++) {
        memset(&devs[i], 0, sizeof(devs[i]));
        snprintf(devs[i].name, sizeof(devs[i].name), "fz%d", i);
        devs[i].mtu = 1500;
        devs[i].up = true;
        devs[i].tx = null_tx;
        devs[i].mac[0] = 0x02;
        devs[i].mac[5] = (uint8_t)(0x10 + i);
        pf_stack_add_dev(&stk, &devs[i]);
        pf_if_set_addr(&stk, &devs[i],
                       (uint32_t)(0x0a010002 + ((uint32_t)i << 16)), /* 10.1.0.2 / 10.2.0.2 */
                       0xffffff00);
    }
    fib_add_via(&stk, 0xac100000, 16, 0x0a020001); /* static 172.16/16 via 10.2.0.1 */

    int us = pf_socket(&stk, PF_SOCK_UDP);
    pf_bind(&stk, us, 0, 7);
    int ts = pf_socket(&stk, PF_SOCK_TCP);
    pf_bind(&stk, ts, 0, 7);
    pf_listen(&stk, ts, 4);
}

static void feed(const uint8_t *data, size_t len)
{
    if (len == 0 || len > PKT_BUF_SIZE - PKT_RX_OFFSET)
        return;
    struct pkt *p = pkt_alloc();
    memcpy(pkt_put(p, (uint16_t)len), data, len);
    p->dev = &devs[0];
    p->ts_ms = fake_now;
    eth_input(&stk, &devs[0], p);
    pf_tick(&stk, fake_now);
}

#ifndef __AFL_FUZZ_TESTCASE_LEN
/* Stub macros so the same source builds without afl-clang-fast. */
static unsigned char fuzz_buf[PKT_BUF_SIZE];
#define __AFL_FUZZ_TESTCASE_BUF fuzz_buf
#define __AFL_FUZZ_TESTCASE_LEN read(0, fuzz_buf, sizeof(fuzz_buf))
#define __AFL_FUZZ_INIT()
#define __AFL_INIT()
#define __AFL_LOOP(x) afl_loop_once()
static int afl_done;
static int afl_loop_once(void)
{
    return !afl_done++;
}
#endif

__AFL_FUZZ_INIT();

int main(void)
{
    stack_setup();
    __AFL_INIT();

    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
    int iter = 0;
    while (__AFL_LOOP(10000)) {
        ssize_t len = __AFL_FUZZ_TESTCASE_LEN;
        if (len > 0)
            feed(buf, (size_t)len);
        /* Periodically rebuild so saturated TCB/socket tables don't wall
         * off the interesting paths. */
        if (++iter % 256 == 0)
            stack_setup();
    }
    pf_stack_fini(&stk);
    return 0;
}
