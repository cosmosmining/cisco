#include "netdev/netdev.h"

int netdev_tx(struct netdev *dev, struct pkt *p)
{
    int rc = dev->tx(dev, p);
    if (rc == 0) {
        dev->st.tx_pkts++;
        dev->st.tx_bytes += p->len;
    } else {
        dev->st.tx_errs++;
    }
    return rc;
}
