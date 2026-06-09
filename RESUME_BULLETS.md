# Resume bullets (candidates)

Every number below comes from a recorded run in this repo; the command that
reproduces it is annotated under each bullet. Evidence transcripts live in
PROGRESS.md; CI re-runs the tests on every push.

---

**Built a from-scratch TCP/IP stack in C11 (Ethernet/ARP/IPv4/ICMP/UDP/TCP, ~4.5k LoC) over Linux TAP; 94 tests (70 unit + 24 scapy/pytest integration) pass under ASan/UBSan in CI.**

- LoC: `wc -l src/*/*.c src/*/*.h` → 4,473
- Tests: `make SAN=asan test` (8 binaries, 70 cases) +
  `sudo python3 -m pytest test/integration -v` (24 cases, runs the
  `build-asan/` binaries) — both jobs in `.github/workflows/ci.yml`
- 19 RFCs cited at the implementing line:
  `grep -rEo 'RFC [0-9]+' src/ | sort -u`

---

**Implemented RFC 6298 retransmission (SRTT/RTTVAR, Karn, exponential backoff) plus RFC 5681 fast retransmit; verified 1 MiB SHA-256-checked transfers over an emulated link with 5% loss in each direction.**

- `sudo python3 -m pytest test/integration/test_tcp.py::test_1mb_transfer_with_5pct_loss -s`
  → `HASH_OK`, stats from the run:
  `tx_segs=1411 rtx_segs=43 rto_fires=14 fast_rtx=29 dupacks=168 ooo_queued=50`
- RFC 6298 arithmetic pinned exactly in `test/unit/test_tcp.c`
  (`test_rfc6298_srtt_rttvar_math`: SRTT 100→112, RTTVAR 50→62, RTO 300→360)

---

**Developed an L3 forwarding plane (LPM binary-trie FIB, TTL/incremental-checksum per RFC 1624, ICMP errors per RFC 1812) with an IOS-style CLI; routed iperf3 UDP between network namespaces at ~1 Gbit/s, with forwarded-packet counters matching tcpdump 100/100.**

- `bench/router_udp.sh 1G` → 978 Mbit/s received of 1 Gbit/s offered
  (2.2% loss; ~2.8–3.4 Gbit/s at saturation, single shared vCPU VM);
  gate run at 50 Mbit/s: 0/12946 datagrams lost
- `sudo python3 -m pytest test/integration/test_fwd.py -s` →
  `counters: router fwd delta=100, tcpdump ground truth=100`
- CLI: `printf 'show ip route\n' | nc -U /tmp/pf-cli.sock`; live
  `ip route <prefix> <mask> <nh>` flips reachability mid-ping (same test file)

---

**Hardened the packet parsers: 100k scapy-mutated frames and 30.4M AFL++ executions with zero crashes/sanitizer reports; teardrop-style overlap rejection, RFC 5961 challenge ACKs, per-reason drop counters; 87% line coverage of the stack.**

- `sudo python3 test/fuzz/fuzz_scapy.py --iterations 20000 --seed 1` and
  `--iterations 80000 --seed 2` → "no crashes, stack still answers ping"
  (binary = `build-asan/bin/pfstack`)
- `fuzz/run_afl.sh 600` → `execs_done: 30380906`, `execs_per_sec: 50634`,
  `saved_crashes: 0`, `saved_hangs: 0` (ASan-instrumented harness)
- Coverage: `make COV=1 && make COV=1 test && sudo env PF_BUILD=build-cov
  python3 -m pytest test/integration -q && gcovr -r . build-cov --filter src/`
  → lines 87.0% (1868/2148), functions 96.6%, branches 68.9%

---

### Style-matched one-liners (pick 3, ≤1 line at 10.5 pt)

- Built a from-scratch TCP/IP stack in C (Ethernet/ARP/IPv4/ICMP/UDP/TCP) on Linux TAP; 94 scapy/unit tests pass under ASan/UBSan in CI.
- Implemented RFC 6298 retransmission and flow control; completed SHA-256-verified 1 MiB transfers over a 5%-loss emulated link.
- Developed an L3 forwarding plane with LPM-trie FIB and IOS-style CLI; routed iperf3 UDP between netns at ~1 Gbit/s, counters matching tcpdump 100/100.
- Fuzzed the packet parsers with scapy (100k frames) and AFL++ (30M execs): zero crashes; 87% line coverage published from CI.
