# PacketForge architecture

~4,500 lines of C11 in `src/`, no global mutable state (everything hangs off
`struct pf_stack`), 19 RFCs cited at the line that implements them.

## The packet walk (receive → application)

```
            Linux kernel writes a frame into the TAP
                              │
   netdev/loop_linux.c   poll() → read(fd) → pkt_alloc (+2 align offset)
                              │ eth_input
   eth/eth.c             runt check ─ dst MAC filter ─ ethertype demux
                  ┌───────────┴─────────────┐
   arp/arp.c   ARP                        IPv4   ipv4/ipv4.c
        merge/learn → reply           version/IHL/len/csum/src checks
        resolve waitq flush                   │
                              ┌── not ours ───┴── ours ──┐
   fwd/fwd.c            ip_forward                 fragment? ── ipv4/ip_reass.c
        TTL≤1 → ICMP time-exceeded                 │   (overlap = abort ctx)
        LPM (route/fib.c trie)                ip_local_deliver
        TTL-- + RFC1624 csum                       │ proto demux
        ARP resolve → eth_output      ┌────────────┼────────────┐
                              icmp/icmp.c      udp/udp.c    tcp/tcp.c
                              echo reply       csum+demux   RFC 9293 §3.10.7
                              errors+rate      → sock rxq   "segment arrives"
                              limiting              │            │
                                              pf_recvfrom   recv ring / accept q
                                                    │            │
                                              apps/udp_echo  apps/tcp_echo, httpd
```

Transmit inverts the walk: the app writes into a socket, TCP/UDP prepend
their headers into pkt headroom (checksums over a pseudo-header), `ip_output`
routes via the FIB trie, fragments if needed, ARP resolves the next hop
(parking the packet on a bounded waitq when unresolved), and `eth_output`
prepends the Ethernet header and hands the frame to the TAP fd.

## Module dependency graph

```
   apps/* ──────────────┐
      │                 ▼
      │            udp/sock.h  (socket API: pf_socket/bind/sendto/recvfrom,
      │                 │       pf_listen/accept/connect/send/recv)
      ▼                 ▼
  netdev/ (Linux: TAP, poll loop, clock)      cli/ (UNIX socket, Linux)
      │  platform layer — the ONLY syscalls   │
══════╪═══════════ portability boundary ══════╪══════════ (D-005)
      ▼                                       ▼
   core/ (pkt bufs, stats, stack object) ◄── everything
      ▲
      │ include only core + each other, zero platform headers:
   eth/ ── arp/ ── ipv4/ (csum, reass) ── icmp/ ── udp/ ── tcp/ ── route/ ── fwd/
```

The protocol directories compile with no Linux headers; `pf_now_ms()` and
`netdev->tx` are the only seams, which is also exactly how the unit tests
inject fake clocks and capture frames.

## Concurrency model

Single-threaded event loop (DECISIONS.md D-002): `poll()` over TAP + CLI
fds, then per-module timer ticks (ARP aging, reassembly timeout, TCP
RTO/delack/TIME_WAIT). Blocking socket calls pump the loop until their
wakeup condition holds. No locks anywhere; determinism makes the state
machine testable byte-for-byte.

## TCP internals in one paragraph

Send side is a 64 KiB byte ring: the unacked prefix *is* the retransmission
queue, and retransmits rebuild a segment from `snd_una` (D-012) — Karn's
rule simply cancels the in-flight RTT sample. RFC 6298 SRTT/RTTVAR drive
the RTO with exponential backoff; RFC 5681 slow start/congestion avoidance/
fast retransmit ride on the ACK clock; RFC 5961 challenge ACKs answer
in-window SYN/RST so blind resets and half-open peers resolve safely.
Receive side is a ring plus a bounded sorted list of out-of-order segments
(overlaps rejected — retransmission refills what we decline).

## Hardening posture

Every drop has a named counter (`show ip traffic` lists ~90); malformed
input is counted, never trusted: header fields are bounds-checked before
use, fragment overlap aborts reassembly (teardrop, D-011), reassembly
memory is strictly bounded (D-010), ARP only *creates* cache entries when
we are the target (pollution defense), ICMP errors are rate-limited and
never sent about errors/fragments/non-unicast, directed broadcasts are
never forwarded (RFC 2644). 100k mutated frames + AFL++ runs are recorded
in PROGRESS.md; ASan/UBSan gate every CI run.

## What I'd do differently (or next)

- **Buffer pools + zero-copy**: malloc-per-packet (D-003) was the right
  call for ASan-era development; a slab of pkt buffers and scatter/gather
  tx would be the first perf change on real hardware.
- **SACK + window scaling + timestamps (RFC 7323/2018)**: 16-bit windows
  cap throughput at WAN RTTs and cumulative-ACK-only recovery wastes
  retransmissions on multi-loss windows.
- **Hashed demux**: socket and TCB lookups are linear scans — fine at 16
  connections, a hash on the 4-tuple at 10k.
- **Timer wheel**: per-tick full TCB scans don't scale past hundreds of
  connections.
- **Congestion control as a vtable**: NewReno-ish today; CUBIC/BBR want a
  pluggable interface.
- **IPv6**: the demux seams (ethertype, pseudo-header checksum, trie key
  width) were laid out with a second address family in mind.
- **DMA-friendly netdev API**: `tx(dev, pkt)` hides ring descriptors; a
  real NIC driver wants tx-done completions surfaced to the stack.
