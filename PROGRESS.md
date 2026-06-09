# Progress log

Per-phase gate evidence. Every claim below is backed by a pasted command +
output from an actual run in this repo (Ubuntu 24.04, Linux 6.18, x86_64,
gcc 13.3 / clang 18.1).

<!-- Phases appended below as gates pass. -->

## Phase 0 — Scaffold & link-layer bring-up (2026-06-09)

Built: Makefile (+SAN/COV knobs), CI workflow, clang-format/clang-tidy
configs, `src/core` (logging, packet buffers, stats X-macro, stack object),
`src/netdev` (TAP open via `/dev/net/tun` IFF_TAP|IFF_NO_PI, poll loop,
monotonic clock), minimal `eth_input` with runt drop + hexdump, `apps/pfstack`.

**Gate 0 evidence** — ping toward the TAP subnet makes the stack hexdump the
host's ARP requests:

```
$ ./scripts/dev-tap.sh                                   # pf0, host side 10.190.0.1/24
$ ./build/bin/pfstack --if pf0,10.190.0.2/24 --hexdump &
$ ping -c 3 -W 1 10.190.0.2                              # no ARP reply yet → 100% loss, expected

pfstack: ready (1 interface)
pf0: rx 42 bytes  36:be:9f:55:63:be -> ff:ff:ff:ff:ff:ff  ethertype 0x0806
  0000  ff ff ff ff ff ff 36 be  9f 55 63 be 08 06 00 01  |......6..Uc.....|
  0010  08 00 06 04 00 01 36 be  9f 55 63 be 0a be 00 01  |......6..Uc.....|
  0020  00 00 00 00 00 00 0a be  00 02                    |..........|
(x3, one per ping attempt — ARP request, opcode 0001, target 10.190.0.2)
```

Local lint: `make check-format` clean, `make tidy` clean (exit 0).
Note: initial dev subnet was 192.0.2.0/24 (TEST-NET-1); the CI container's
own uplink uses that range, so all test addressing moved to 10.190.0.0/16.
