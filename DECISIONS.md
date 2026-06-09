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
