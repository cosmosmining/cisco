#ifndef PF_NETDEV_TAP_LINUX_H
#define PF_NETDEV_TAP_LINUX_H

#include "netdev/netdev.h"

struct pf_stack;

/* Open (or attach to) TAP interface `ifname` and register it with the stack.
 * Returns NULL on error. */
struct netdev *pf_tap_open(struct pf_stack *stack, const char *ifname);

int pf_tap_fd(struct netdev *dev);
void pf_tap_close(struct netdev *dev);

/* Event loop: poll all TAP fds, feed frames to eth_input, run pf_tick.
 * Returns number of frames processed, or -1 on fatal error. */
int pf_loop_once(struct pf_stack *stack, int timeout_ms);

/* Run until *stop becomes nonzero. */
void pf_loop_run(struct pf_stack *stack, const volatile int *stop);

#endif /* PF_NETDEV_TAP_LINUX_H */
