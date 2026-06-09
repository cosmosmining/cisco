#!/usr/bin/env python3
"""Generate seed frames for the parser fuzzers (one binary frame per file)."""

from pathlib import Path

from scapy.layers.inet import ICMP, IP, TCP, UDP
from scapy.layers.l2 import ARP, Ether
from scapy.packet import Raw

OUT = Path(__file__).parent / "corpus"
OUT.mkdir(exist_ok=True)

DST = "02:00:00:00:00:10"  # harness dev0 MAC
SRC = "aa:bb:cc:dd:ee:01"
E = Ether(src=SRC, dst=DST)
EB = Ether(src=SRC, dst="ff:ff:ff:ff:ff:ff")

seeds = {
    "arp_request.bin": EB / ARP(op=1, hwsrc=SRC, psrc="10.1.0.1", pdst="10.1.0.2"),
    "arp_reply.bin": E / ARP(op=2, hwsrc=SRC, psrc="10.1.0.1",
                             hwdst=DST, pdst="10.1.0.2"),
    "icmp_echo.bin": E / IP(src="10.1.0.1", dst="10.1.0.2") / ICMP() / Raw(b"ping" * 8),
    "udp_echo_port.bin": E / IP(src="10.1.0.1", dst="10.1.0.2") / UDP(sport=4444, dport=7)
                          / Raw(b"data"),
    "udp_closed.bin": E / IP(src="10.1.0.1", dst="10.1.0.2") / UDP(sport=4444, dport=9999)
                       / Raw(b"x"),
    "tcp_syn.bin": E / IP(src="10.1.0.1", dst="10.1.0.2")
                    / TCP(sport=5555, dport=7, flags="S", seq=1000, options=[("MSS", 1460)]),
    "tcp_data.bin": E / IP(src="10.1.0.1", dst="10.1.0.2")
                     / TCP(sport=5555, dport=7, flags="PA", seq=1001, ack=1) / Raw(b"hello"),
    "tcp_rst.bin": E / IP(src="10.1.0.1", dst="10.1.0.2")
                    / TCP(sport=5555, dport=7, flags="R", seq=1001),
    "frag_first.bin": E / IP(src="10.1.0.1", dst="10.1.0.2", proto=1, id=99, flags="MF",
                             frag=0) / Raw(bytes(16)),
    "frag_last.bin": E / IP(src="10.1.0.1", dst="10.1.0.2", proto=1, id=99, frag=2)
                      / Raw(bytes(16)),
    "fwd_other_subnet.bin": E / IP(src="10.1.0.1", dst="10.2.0.55", ttl=9) / ICMP(),
    "fwd_static_route.bin": E / IP(src="10.1.0.1", dst="172.16.9.9", ttl=9)
                             / UDP(sport=1, dport=2) / Raw(b"f"),
    "fwd_ttl1.bin": E / IP(src="10.1.0.1", dst="10.2.0.55", ttl=1) / ICMP(),
}

for name, pkt in seeds.items():
    (OUT / name).write_bytes(bytes(pkt))
print(f"wrote {len(seeds)} seeds to {OUT}")
