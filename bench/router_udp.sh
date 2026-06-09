#!/bin/sh
# Measure UDP throughput across the PacketForge router (gate-5 topology).
# Usage: bench/router_udp.sh [bitrate]   (default 0 = unlimited)
# Needs root; builds nothing — run `make` first.
set -eu
cd "$(dirname "$0")/.."

RATE=${1:-0}
BIN=build/bin/pfstack
[ -x "$BIN" ] || { echo "run make first" >&2; exit 1; }

./test/topo.sh down >/dev/null 2>&1 || true
$BIN --if rt0,10.191.1.2/24 --if rt1,10.191.2.2/24 --forward &
ROUTER=$!
sleep 1
./test/topo.sh up >/dev/null

ip netns exec pfserver iperf3 -s -1 -p 5201 >/dev/null 2>&1 &
SRV=$!
sleep 0.5
ip netns exec pfhost iperf3 -u -c 10.191.2.1 -b "$RATE" -t 5 -p 5201 -f m | tail -4

kill -INT $ROUTER 2>/dev/null || true
wait $SRV 2>/dev/null || true
./test/topo.sh down >/dev/null
