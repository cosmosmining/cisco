/*
 * Single-threaded event loop (DECISIONS.md D-002): poll TAP fds, feed frames
 * up the stack, then run module timers. Blocking socket calls pump this loop
 * until their wakeup condition holds.
 */
#include "cli/cli.h"
#include "core/stack.h"
#include "eth/eth.h"
#include "netdev/tap_linux.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#define RX_BATCH 64 /* max frames per device per poll round */

static int rx_drain(struct pf_stack *stack, struct netdev *dev, int fd)
{
    int handled = 0;
    for (int i = 0; i < RX_BATCH; i++) {
        struct pkt *p = pkt_alloc();
        ssize_t n = read(fd, p->data, PKT_BUF_SIZE - PKT_RX_OFFSET);
        if (n < 0) {
            pkt_free(p);
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            PF_WARN("%s: rx read: %s", dev->name, strerror(errno));
            break;
        }
        if (n == 0) {
            pkt_free(p);
            break;
        }
        p->len = (uint16_t)n;
        p->dev = dev;
        p->ts_ms = pf_now_ms();
        dev->st.rx_pkts++;
        dev->st.rx_bytes += (uint64_t)n;
        eth_input(stack, dev, p); /* consumes p */
        handled++;
    }
    return handled;
}

int pf_loop_once(struct pf_stack *stack, int timeout_ms)
{
    struct pollfd fds[PF_MAX_DEVS + 1 + CLI_MAX_CLIENTS];
    int n = 0;
    for (int i = 0; i < stack->ndevs; i++, n++) {
        fds[n].fd = pf_tap_fd(stack->devs[i]);
        fds[n].events = POLLIN;
        fds[n].revents = 0;
    }
    int cli_listen_at = -1;
    int cli_slot_at[CLI_MAX_CLIENTS];
    if (cli_listen_fd(stack) >= 0) {
        cli_listen_at = n;
        fds[n].fd = cli_listen_fd(stack);
        fds[n].events = POLLIN;
        fds[n++].revents = 0;
        for (int s = 0; s < CLI_MAX_CLIENTS; s++) {
            cli_slot_at[s] = -1;
            int cfd = cli_client_fd(stack, s);
            if (cfd >= 0) {
                cli_slot_at[s] = n;
                fds[n].fd = cfd;
                fds[n].events = POLLIN;
                fds[n++].revents = 0;
            }
        }
    }

    int rc = poll(fds, (nfds_t)n, timeout_ms);
    if (rc < 0 && errno != EINTR) {
        PF_ERR("poll: %s", strerror(errno));
        return -1;
    }

    int handled = 0;
    if (rc > 0) {
        for (int i = 0; i < stack->ndevs; i++) {
            if (fds[i].revents & POLLIN)
                handled += rx_drain(stack, stack->devs[i], fds[i].fd);
        }
        if (cli_listen_at >= 0) {
            if (fds[cli_listen_at].revents & POLLIN)
                cli_accept(stack);
            for (int s = 0; s < CLI_MAX_CLIENTS; s++)
                if (cli_slot_at[s] >= 0 && (fds[cli_slot_at[s]].revents & (POLLIN | POLLHUP)))
                    cli_client_input(stack, s);
        }
    }

    pf_tick(stack, pf_now_ms());
    return handled;
}

void pf_loop_run(struct pf_stack *stack, const volatile int *stop)
{
    while (!*stop) {
        if (pf_loop_once(stack, 10) < 0)
            break;
    }
}
