/*
 * Linux TAP backend: /dev/net/tun with IFF_TAP | IFF_NO_PI, i.e. raw
 * Ethernet frames with no packet-info prefix. The kernel side of the TAP
 * interface plays "the network"; this process is the device on the wire.
 */
#include "netdev/tap_linux.h"
#include "core/stack.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct tap_priv {
    int fd;
};

static int tap_tx(struct netdev *dev, struct pkt *p)
{
    struct tap_priv *tp = dev->priv;
    ssize_t n = write(tp->fd, p->data, p->len);
    if (n < 0) {
        PF_WARN("%s: tx write: %s", dev->name, strerror(errno));
        return -1;
    }
    return 0;
}

struct netdev *pf_tap_open(struct pf_stack *stack, const char *ifname)
{
    int fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        PF_ERR("open /dev/net/tun: %s", strerror(errno));
        return NULL;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        PF_ERR("TUNSETIFF %s: %s", ifname, strerror(errno));
        close(fd);
        return NULL;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        PF_ERR("fcntl O_NONBLOCK: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    struct netdev *dev = calloc(1, sizeof(*dev));
    struct tap_priv *tp = calloc(1, sizeof(*tp));
    PF_ASSERT(dev && tp);
    tp->fd = fd;
    snprintf(dev->name, sizeof(dev->name), "%s", ifr.ifr_name);
    dev->mtu = 1500;
    dev->up = true;
    dev->tx = tap_tx;
    dev->priv = tp;

    if (pf_stack_add_dev(stack, dev) < 0) {
        PF_ERR("too many devices");
        close(fd);
        free(tp);
        free(dev);
        return NULL;
    }

    /* Locally administered MAC: 02:50:46 ("PF") + stack-unique tail. */
    dev->mac[0] = 0x02;
    dev->mac[1] = 0x50;
    dev->mac[2] = 0x46;
    dev->mac[3] = 0x00;
    dev->mac[4] = 0x00;
    dev->mac[5] = (uint8_t)(0x02 + dev->ifindex);

    char macs[18];
    PF_INFO("%s: tap open, mac %s, mtu %u", dev->name, pf_mac_str(dev->mac, macs), dev->mtu);
    return dev;
}

int pf_tap_fd(struct netdev *dev)
{
    struct tap_priv *tp = dev->priv;
    return tp->fd;
}

void pf_tap_close(struct netdev *dev)
{
    struct tap_priv *tp = dev->priv;
    close(tp->fd);
    free(tp);
    free(dev);
}
