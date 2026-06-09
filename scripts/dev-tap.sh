#!/bin/sh
# Create the dev TAP interface and give the host side an address.
# Default subnet 10.190.0.0/24: host side .1, the stack claims .2.
# (Deliberately not RFC 5737 TEST-NET — some CI containers use that
# range for their own uplink.)
set -eu
IF=${1:-pf0}
HOST_ADDR=${2:-10.190.0.1/24}

ip tuntap add dev "$IF" mode tap 2>/dev/null || true
ip addr replace "$HOST_ADDR" dev "$IF"
ip link set "$IF" up
echo "$IF up, host side $HOST_ADDR"
