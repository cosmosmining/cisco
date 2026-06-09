#!/bin/sh
# Router test topology (gate 5):
#
#   [netns pfhost]            root ns                  [netns pfserver]
#   rt0 10.191.1.1/24 ── PacketForge router ──  rt1 10.191.2.1/24
#   default via .1.2     rt0 .1.2 / rt1 .2.2     default via .2.2
#
# The router process creates the TAPs in the root ns and keeps their fds;
# this script then MOVES the interfaces into the namespaces (the fd keeps
# working — the tun endpoint stays with the process).
#
# usage: topo.sh up      (after the router process opened rt0/rt1)
#        topo.sh down
set -eu

HOST_NS=pfhost
SRV_NS=pfserver

up() {
    ip netns add $HOST_NS
    ip netns add $SRV_NS

    ip link set rt0 netns $HOST_NS
    ip link set rt1 netns $SRV_NS

    ip netns exec $HOST_NS ip addr add 10.191.1.1/24 dev rt0
    ip netns exec $HOST_NS ip link set rt0 up
    ip netns exec $HOST_NS ip link set lo up
    ip netns exec $HOST_NS ip route add default via 10.191.1.2
    ip netns exec $HOST_NS sysctl -qw net.ipv4.conf.all.rp_filter=0

    ip netns exec $SRV_NS ip addr add 10.191.2.1/24 dev rt1
    ip netns exec $SRV_NS ip link set rt1 up
    ip netns exec $SRV_NS ip link set lo up
    ip netns exec $SRV_NS ip route add default via 10.191.2.2
    ip netns exec $SRV_NS sysctl -qw net.ipv4.conf.all.rp_filter=0
    echo "topology up"
}

down() {
    ip netns del $HOST_NS 2>/dev/null || true
    ip netns del $SRV_NS 2>/dev/null || true
    echo "topology down"
}

case "${1:-}" in
up) up ;;
down) down ;;
*) echo "usage: $0 up|down" >&2; exit 1 ;;
esac
