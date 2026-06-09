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

## Phase 2 — IPv4 + ICMP (2026-06-09)

Built: shared Internet checksum (RFC 1071 + RFC 1624 incremental + pseudo
header), full IPv4 header validation with per-reason drop counters, echo
reply, ICMP error generation (proto-unreachable, time-exceeded) with RFC
1122 §3.2.2 suppression rules + RFC 1812 rate limiting, sender-side
fragmentation (RFC 791 §3.2), RX reassembly with overlap-rejection
(teardrop defense, D-011) + bounded buffers (D-010) + timeout with ICMP
time-exceeded code 1. SIGUSR1 dumps all counters for test assertions.
28 unit tests total; 8 integration tests.

**Gate 2 evidence** (stack binary = `build-asan/bin/pfstack`, ASan+UBSan)

```
$ ping -c 100 -i 0.01 -q 10.190.0.2
100 packets transmitted, 100 received, 0% packet loss, time 1590ms

$ ping -c 5 -s 2000 -q 10.190.0.2          # request+reply both fragmented
5 packets transmitted, 5 received, 0% packet loss, time 4101ms

$ kill -USR1 <pfstack>; grep stat …
stat ip_frags_rx 10            # 5 × 2 request fragments reassembled
stat ip_reass_completed 5
stat ip_tx_frags 10            # 5 replies × 2 fragments
stat icmp_echo_req_rx 105
stat icmp_echo_reply_tx 105
```

Malformed-input gate (scapy, stack under ASan): 10 classes — bad version,
IHL<5, total_len>frame, total_len<IHL, bad IP csum, truncated header,
broadcast src, truncated ICMP, bad ICMP csum, teardrop overlap — no crash,
each lands in its dedicated counter, normal ping works afterwards:

```
$ python3 -m pytest test/integration/test_icmp.py -v
test_ping_100_packets_zero_loss PASSED
test_ping_fragmented_2000 PASSED
test_ten_malformed_classes_counted_not_crashed PASSED
test_unknown_protocol_unreachable PASSED        # RFC 1122 §3.2.2.1, code 2
============================== 4 passed in 8.04s ===============================
```

Unit tests (`make test`, also SAN=asan and SAN=ubsan — 4/4 binaries pass):
checksum vectors incl. odd length + double carry-fold + RFC 1624
incremental-vs-recompute over 65 TTL values; reassembly in/out-of-order,
duplicate-as-overlap, conflicting last fragment, timeout→ICMP, size bound,
8-byte alignment rule; ICMP error suppression rules + rate limit.

## Phase 3 — UDP + socket API + udp_echo (2026-06-09)

Built: UDP rx/tx with pseudo-header checksum (RFC 768; zero-csum accepted,
computed-zero sent as all-ones), port demux (exact-beats-wildcard), ICMP
port-unreachable for closed ports (RFC 1122 §4.1.3.1), bounded per-socket
rx queues, blocking socket API (`pf_socket/pf_bind/pf_sendto/pf_recvfrom` +
`pf_close`) that pumps the event loop through a platform hook, ephemeral
auto-bind, `apps/udp_echo`. 8 new unit tests; 3 integration tests.

**Gate 3 evidence** (apps run from `build-asan/`, ASan+UBSan)

```
$ python3 -m pytest test/integration/test_udp.py -v
test_udp_echo_1000_roundtrips PASSED        # 1,000 round-trips, payloads
                                            # 1..1200 B verified byte-for-byte
test_closed_port_gets_icmp_port_unreachable PASSED   # kernel raises
                                            # ECONNREFUSED from our ICMP
test_nc_udp_interop PASSED                  # echo 'hello packetforge' | nc -u
============================== 3 passed in 3.42s ===============================
```

Stats after the 1,000-roundtrip run (from SIGUSR1 dump, asserted in-test):
`udp_rx ≥ 1000`, `udp_rx_delivered ≥ 1000`, `udp_tx ≥ 1000`,
`udp_rx_bad_csum == 0`.

Full suite at this point: 11 integration tests + 36 unit tests, all green
(`make test`, `make SAN=asan test`, `make SAN=ubsan test`).

## Phase 4 — TCP core (2026-06-09)

Built: full RFC 793/1122 state machine via the RFC 9293 §3.10.7 event
processing (LISTEN/SYN_SENT/SYN_RCVD/ESTABLISHED/FIN_WAIT_1/2/CLOSE_WAIT/
CLOSING/LAST_ACK/TIME_WAIT), passive+active open, RST generation and
validation with RFC 5961 challenge ACKs, RFC 6298 RTO (SRTT/RTTVAR, Karn,
exponential backoff, 200 ms floor per D-014), RFC 5681 slow start +
congestion avoidance + fast retransmit/recovery, byte-ring send buffer with
rebuild-on-retransmit (D-012), receive-window flow control + zero-window
probes, out-of-order reassembly with bounded parking, delayed ACKs
(ack-every-2nd, 40 ms cap), Nagle with per-socket toggle, MSS option both
directions. Apps: `tcp_echo`, `httpd` (HTTP/1.0; `/` page + `/blob` 1 MiB
deterministic stream). 13 unit tests incl. exact RFC 6298 math; 7 scapy/
curl/nc integration tests.

**Gate 4 evidence** (apps from `build-asan/`, ASan+UBSan)

```
$ curl -s http://10.190.0.2/ | head -3
<!doctype html>
<html><head><title>PacketForge</title></head>
<body><h1>PacketForge</h1>

$ curl -s http://10.190.0.2/blob | sha256sum     # 1 MiB over our TCP
3e75ec671075e4115fc876c4c55d5abbac3bfe487a67ff4ca2db38386a014f27  -
(matches the generator hash computed independently in Python)

$ printf 'hello tcp stack\n' | nc -N -w 3 10.190.0.2 7
hello tcp stack

$ python3 -m pytest test/integration/test_tcp.py -v
test_curl_fetches_page PASSED
test_curl_blob_1mb_hash PASSED
test_nc_interactive_echo PASSED
test_syn_to_closed_port_gets_rst PASSED          # RST|ACK, SEQ=0, ACK=ISS+1
test_half_open_teardown PASSED                   # RFC 9293 §3.6 fig.10 via
                                                 # RFC 5961 challenge ACK
test_out_of_order_segments_reassembled PASSED
test_1mb_transfer_with_5pct_loss PASSED
```

1 MiB echo round-trip (2 MiB on the wire) across a routed namespace with
5% random loss in EACH direction, SHA-256 verified end-to-end:

```
lossy transfer stats: tx_segs=1411 rtx_segs=43 rto_fires=14 fast_rtx=29
                      dupacks=168 ooo_queued=50 rtt_samples=166
```

Loss tooling note: this dev VM's kernel lacks CONFIG_NET_SCH_NETEM, so the
fixture falls back to `iptables -m statistic --probability 0.05` on the
forwarding path (bidirectional); on CI kernels the same test applies the
gate's literal `tc qdisc … netem loss 5%`. Both are real 5% random loss.

ASan caught one real bug during bring-up: `tcp_ooo_drain` read `q->len`
after `pkt_free(q)` (use-after-free). Fixed by hoisting the length.

Unit tests now 49 across 6 binaries — all pass under plain, SAN=asan,
SAN=ubsan. RFC 6298 math is pinned exactly (SRTT=100→112, RTTVAR=50→62,
RTO=300→360 across two samples).
