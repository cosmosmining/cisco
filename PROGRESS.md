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

## Phase 1 — Ethernet + ARP (2026-06-09)

Built: Ethernet demux with dst-MAC filter, full ARP (RFC 826 merge logic,
reply generation, request transmit), cache with TTL expiry + bounded
pending-packet waitq + retransmit/backoff (RFC 1122 §2.3.2), 8 unit tests
incl. injected-clock expiry, 4 scapy integration tests, pcap artifacts.

**Gate 1 evidence**

`arping <stack-ip>` gets replies:

```
$ arping -I pf0 -c 3 10.190.0.2
ARPING 10.190.0.2 from 10.190.0.1 pf0
Unicast reply from 10.190.0.2 [02:50:46:00:00:02]  0.621ms
Unicast reply from 10.190.0.2 [02:50:46:00:00:02]  0.642ms
Unicast reply from 10.190.0.2 [02:50:46:00:00:02]  0.640ms
Sent 3 probes (1 broadcast(s))
Received 3 response(s)
```

scapy field assertions + cache expiry with injected clock:

```
$ python3 -m pytest test/integration -v
test/integration/test_arp.py::test_arping_gets_replies PASSED            [ 25%]
test/integration/test_arp.py::test_arp_reply_fields PASSED               [ 50%]
test/integration/test_arp.py::test_arp_not_for_us_ignored PASSED         [ 75%]
test/integration/test_arp.py::test_malformed_arp_does_not_kill_stack PASSED [100%]
============================== 4 passed in 5.93s ===============================

$ make SAN=asan test          # same result with plain make test and SAN=ubsan
test_arp:
  test_reply_to_request_for_our_ip                ok
  test_ignores_request_not_for_us                 ok
  test_cache_expiry_with_injected_clock           ok
  test_resolve_queues_then_flushes_on_reply       ok
  test_pending_retries_then_gives_up              ok
  test_waitq_is_bounded                           ok
  test_malformed_arp_is_counted_not_crashed       ok
  test_runt_frame_dropped                         ok
test_arp: all passed
```

ASan initially flagged 8 leaked packet buffers: test reset wiped ARP waitqs
via memset without freeing queued packets. Fixed by adding `pf_stack_fini()`
(also used on daemon shutdown).
