# Design decisions

Running log of engineering decisions and their rationale. Newest at the bottom.

## D-001: Project lives at the repository root
The prompt's layout shows a `packetforge/` top directory; since this repo is
dedicated to the project, the layout sits at the repo root so clones and CI
paths stay simple. Renaming/moving to a `packetforge` repo later is a plain
`git push` to a new remote.

## D-002: Single-threaded event-loop stack, no locks
The stack runs as one event loop: `poll()` on TAP fds + a periodic tick.
Blocking socket calls (`pf_recvfrom`, `pf_accept`, …) pump the loop
internally until their wakeup condition holds. Rationale: deterministic
execution, zero locking bugs, and it mirrors how embedded stacks
(lwIP `NO_SYS=1`, Zephyr native loop) actually run on MCUs — which is the
story this project tells. Trade-off: an app blocked in `pf_recv` can't do
other work; acceptable for echo/HTTP-1.0 apps and irrelevant in router mode.

## D-003: Packet buffers are malloc'd, fixed-size, single-owner
`struct pkt` is a fixed 2 KiB buffer allocated with `malloc` and freed by
whoever consumes it (no refcounts, no pools). Rationale: ASan can see
use-after-free/leaks during fuzzing, ownership stays auditable, and gate
throughput targets don't need a slab. A freelist would be the first perf
change on real hardware; noted in docs/architecture.md "what I'd do
differently".

## D-004: `src/core/` added to the prescribed layout
Common definitions (byte order, logging, packet buffers, the stack object,
stats) need a home that both protocol modules and the platform layer can
include without cycles. One small extra directory; everything else matches
the prescribed layout.

## D-005: Portability boundary for the embedded tie-in
Protocol modules (`eth arp ipv4 icmp udp tcp route fwd`) include only
`src/core` and each other — no Linux headers, no syscalls. All
platform-specific code (TAP, `poll`, monotonic clock) lives in
`src/netdev/*_linux.c` behind `struct netdev`'s function pointers and a
`pf_now_ms()` hook. Unit tests exploit the same seam to inject fake clocks
and capture TX frames.

## D-006: Addresses kept in host byte order inside the stack
IPv4 addresses are converted once at parse time (`pf_ntohl`) and handled in
host byte order internally (comparisons, LPM trie bit-walks read MSB-first
naturally). Network byte order appears only inside wire-format header
structs. Mixing the two is the classic source of subtle bugs; one explicit
boundary keeps it auditable.

## D-007: Justified global mutable state
Per project rules, globals are banned except where justified. Current list:
- `pf_log_threshold` (core/log.c): process-wide log verbosity knob; carries
  no protocol state and tests don't depend on it.
- `g_stop` (apps/*.c): `volatile sig_atomic_t` signal flag — the only async
  channel a signal handler can legally write.
Everything else hangs off a `struct pf_stack` instance passed explicitly.

## D-008: All wire-format header structs are `__attribute__((packed))`
Parsing casts buffer pointers to header structs. Packing makes the compiler
emit alignment-safe loads on every target (incl. RISC-V where misaligned
loads can trap), keeps UBSan quiet, and documents that these structs are
wire images, not ABI structs. RX frames are additionally read at a +2 offset
so the IPv4 header lands 4-byte aligned (the classic Ethernet alignment
trick) — a perf nicety, not a correctness requirement.

## D-009: ARP waitq drops oldest on overflow
When the per-entry pending queue (8 deep) overflows, the *oldest* packet is
dropped: under a burst the most recent data survives, and upper layers
(TCP retransmit, application retry) recover the head loss naturally.

## D-010: Reassembly is strictly bounded at 4000 payload bytes
RFC 791 allows 65,535-byte datagrams; supporting that means 64 KiB buffers
per reassembly context — exactly the resource-exhaustion vector fragment
floods abuse. This stack is embedded-style: contexts are capped at 8,
payload at 4000 bytes (fits the 4 KiB pkt buffer), ranges at 32, and
anything beyond is dropped *and counted* (`ip_reass_too_big`). `ping
-s 2000` (the gate) reassembles fine; a 5000-byte ping deliberately does
not. Raising the cap is a one-line change.

## D-011: Overlapping fragments abort the whole reassembly
Legitimate senders never overlap fragments; teardrop-style attacks rely on
them. Rather than arbitrating overlaps (the historically bug-prone path),
any overlap — duplicates included — kills the context, mirroring RFC 5722's
IPv6 rule and Linux's post-CVE-2018-5391 IPv4 behavior.

## D-012: TCP send buffer is a byte ring, not a segment queue
The retransmission "queue" is the unacked prefix of a 64 KiB byte ring;
retransmits rebuild a segment from `snd_una` on demand. Compared to keeping
sent-segment copies: one copy total, natural repacketization after MSS or
window changes, and no per-segment metadata to corrupt. Cost: a retransmit
re-copies up to one MSS — irrelevant at our rates.

## D-013: No window scaling, SACK, or timestamps
16-bit windows (≤64 KiB) saturate a TAP-RTT link easily, and each of these
options roughly doubles input-path complexity. They're the first things to
add for WAN-grade throughput; documented as future work in
docs/architecture.md.

## D-014: RTO floor is 200 ms (RFC 6298 says SHOULD be 1 s)
RFC 6298 §2.4 allows finer floors with finer clocks; Linux uses 200 ms.
On a sub-millisecond TAP link a 1 s floor turns every tail loss into a
full second stall and makes the 5 %-loss gate needlessly slow. The
RFC-conservative value is one #define away.

## D-015: MSL = 5 s (TIME_WAIT = 10 s)
RFC 793's 2-minute MSL would leave CI runs full of lingering TIME_WAIT
TCBs in a 16-connection table. 2MSL still comfortably exceeds any segment
lifetime on a virtual link. Production value is a one-line change.

## D-017: No fragmentation on the forwarding path
A forwarded packet larger than the egress MTU is dropped (with
frag-needed ICMP when DF is set, per RFC 1191 path-MTU discovery); we never
fragment in transit. Both router ports run the same 1500 MTU in every test
topology, modern networks rely on PMTUD anyway, and the counter
(`ip_fwd_mtu_drop`) makes the behavior observable.

## D-018: Weak host model
A packet addressed to ANY of the stack's interface addresses is delivered
locally regardless of ingress interface (RFC 1122 §3.3.4.2 allows either
model). Required for router ergonomics: `ping`/`traceroute` to the far-side
interface address must answer.

## D-016: ISS is clock-derived, not RFC 6528-hashed
The ISS uses the RFC 793 clock scheme (+ a per-connection stride), not the
keyed-hash ISS of RFC 6528. Sequence-prediction resistance matters on
hostile networks; this stack's lab scope doesn't warrant pulling in a hash
function, and RFC 5961 challenge ACKs (implemented) close the practical
blind-injection vectors the hashed ISS mainly defends against.

## D-019: Phase 7 (RISC-V/QEMU virtio-net port) descoped from this pass
The port needs a bare-metal virtio-net-MMIO driver (~500 lines), a RISC-V
runtime (linker script, start.S, minimal libc shims) and a cross/QEMU CI
job — a multi-day effort that adds no new protocol code. What phase 7
actually proves — that the datapath has no platform dependencies — is
enforced and demonstrated today: `eth arp ipv4 icmp udp tcp route fwd`
include no Linux headers, and the unit tests already run the whole stack
against a fake clock and a captured-frame netdev, which is exactly the
embedded netdev contract. The port remains the natural next milestone.
