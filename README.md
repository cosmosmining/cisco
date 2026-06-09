# PacketForge

[![CI](https://github.com/cosmosmining/cisco/actions/workflows/ci.yml/badge.svg)](https://github.com/cosmosmining/cisco/actions/workflows/ci.yml)

A from-scratch embedded-style TCP/IP stack and L3 router data plane in C11.
No lwIP, no picoTCP, no kernel stack on the data path — just libc and Linux
TAP devices. Built as a portfolio project demonstrating low-level networking,
OS fundamentals, and robust packet-processing code.

```
   host (Linux kernel stack)                PacketForge process
   ┌───────────────────────┐               ┌──────────────────────────────┐
   │  ping / curl / iperf3 │               │  apps: udp_echo tcp_echo httpd│
   │          │            │               │  ────────────────────────────│
   │      kernel TCP/IP    │   Ethernet    │  TCP   UDP   ICMP            │
   │          │            │    frames     │     \   |   /                │
   │        tap0 ──────────┼───────────────┼──── IPv4 ── route/fwd        │
   │                       │  (/dev/net/tun)│       │                      │
   └───────────────────────┘               │  ARP ─ eth ─ netdev(TAP)     │
                                           └──────────────────────────────┘
```

## Features

- **Link layer**: TAP netdev abstraction, Ethernet II framing, ARP with
  cache timeout + pending-packet queue
- **IPv4**: full header validation, shared Internet-checksum routine,
  RX fragment reassembly with overlap rejection (teardrop defense)
- **ICMP**: echo reply, destination-unreachable, TTL-exceeded
- **UDP**: checksum incl. pseudo-header, port demux, minimal socket API
  (`pf_socket/pf_bind/pf_sendto/pf_recvfrom`)
- **TCP**: full RFC 793/1122 state machine, RFC 6298 retransmission
  (SRTT/RTTVAR + exponential backoff), receive-window flow control,
  out-of-order reassembly
- **Router mode**: multi-interface, binary-trie longest-prefix-match FIB,
  TTL decrement with incremental checksum update (RFC 1624), IOS-style CLI
  (`show ip route`, `show interfaces`, …) over a UNIX socket
- Every protocol behavior cites its RFC section in a code comment.

## Build & run

```sh
make                 # build/bin/{pfstack,udp_echo,tcp_echo,httpd}
make test            # unit tests
make SAN=asan test   # unit tests under ASan+UBSan
make check-format    # clang-format
make tidy            # clang-tidy

# bring up a TAP device and run the stack on it (needs root):
./scripts/dev-tap.sh                      # creates pf0, host side 10.190.0.1/24
./build/bin/pfstack --if pf0,10.190.0.2/24 --hexdump
ping 10.190.0.2                           # from another shell
```

Integration tests (root, Linux):

```sh
make
python3 -m pytest test/integration -v
```

## Repo layout

See the [architecture notes](docs/architecture.md) for the packet walk.
`PROGRESS.md` records per-phase gate evidence (real command output);
`DECISIONS.md` records design decisions and their rationale.

## Status

Built in phases with hard gates — see [PROGRESS.md](PROGRESS.md).
