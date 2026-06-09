"""Phase 2 gate: ping (incl. fragmented), malformed-packet robustness."""

import re
import subprocess
import time

from scapy.layers.inet import ICMP, IP
from scapy.layers.l2 import Ether
from scapy.packet import Raw
from scapy.sendrecv import AsyncSniffer, sendp

from conftest import HOST_IP, IFACE, SCAPY_IP, SCAPY_MAC, STACK_IP, STACK_MAC


def run_ping(args):
    r = subprocess.run(["ping"] + args, capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    m = re.search(r"(\d+) packets transmitted, (\d+) received", r.stdout)
    assert m, r.stdout
    return int(m.group(1)), int(m.group(2)), r.stdout


def test_ping_100_packets_zero_loss(stack):
    """Gate 2: 100 echo round-trips, 0% loss."""
    tx, rx, out = run_ping(["-c", "100", "-i", "0.01", "-W", "2", STACK_IP])
    assert (tx, rx) == (100, 100), out
    st = stack.stats()
    assert st["icmp_echo_req_rx"] >= 100
    assert st["icmp_echo_reply_tx"] >= 100


def test_ping_fragmented_2000(stack):
    """Gate 2: ping -s 2000 — request and reply both fragmented."""
    tx, rx, out = run_ping(["-c", "10", "-s", "2000", "-i", "0.05", "-W", "2", STACK_IP])
    assert (tx, rx) == (10, 10), out
    st = stack.stats()
    assert st["ip_reass_completed"] >= 10  # requests reassembled
    assert st["ip_tx_frags"] >= 20  # replies fragmented (2 frags each)


def eth(payload_bytes):
    return Ether(src=SCAPY_MAC, dst=STACK_MAC, type=0x0800) / Raw(payload_bytes)


def mutate(pkt, **fields):
    """Build an IP packet then force-break header bytes after scapy's
    checksum pass."""
    return bytearray(bytes(pkt))


def test_ten_malformed_classes_counted_not_crashed(stack):
    """Gate 2: 10 classes of malformed IP/ICMP; the stack must not crash
    (binary runs under ASan when built SAN=asan) and each class must land
    in a dedicated drop counter."""
    before = stack.stats()

    good_echo = IP(src=SCAPY_IP, dst=STACK_IP) / ICMP(type=8, id=7, seq=1) / Raw(b"x" * 8)

    # 1. version=5
    b = mutate(good_echo)
    b[0] = 0x55
    sendp(eth(bytes(b)), iface=IFACE)

    # 2. IHL=4 (<5)
    b = mutate(good_echo)
    b[0] = 0x44
    sendp(eth(bytes(b)), iface=IFACE)

    # 3. total_len larger than the frame
    sendp(eth(bytes(IP(src=SCAPY_IP, dst=STACK_IP, len=1400, chksum=None) / ICMP() / Raw(b"y" * 8))), iface=IFACE)

    # 4. total_len smaller than the header
    sendp(eth(bytes(IP(src=SCAPY_IP, dst=STACK_IP, len=12, chksum=None) / ICMP() / Raw(b"z" * 8))), iface=IFACE)

    # 5. corrupted IP checksum
    sendp(eth(bytes(IP(src=SCAPY_IP, dst=STACK_IP, chksum=0xDEAD) / ICMP() / Raw(b"w" * 8))), iface=IFACE)

    # 6. truncated header: 12 bytes of an IP header
    sendp(eth(bytes(IP(src=SCAPY_IP, dst=STACK_IP))[:12]), iface=IFACE)

    # 7. broadcast source address
    sendp(eth(bytes(IP(src="255.255.255.255", dst=STACK_IP) / ICMP())), iface=IFACE)

    # 8. ICMP truncated to 4 bytes
    sendp(eth(bytes(IP(src=SCAPY_IP, dst=STACK_IP, proto=1) / Raw(b"\x08\x00\x00\x00"[:4]))), iface=IFACE)

    # 9. ICMP with broken checksum
    sendp(eth(bytes(IP(src=SCAPY_IP, dst=STACK_IP) / ICMP(type=8, chksum=0xBEEF) / Raw(b"q" * 8))), iface=IFACE)

    # 10. teardrop-style overlapping fragments
    sendp(eth(bytes(IP(src=SCAPY_IP, dst=STACK_IP, proto=253, id=7777, flags="MF", frag=0) / Raw(b"A" * 16))), iface=IFACE)
    sendp(eth(bytes(IP(src=SCAPY_IP, dst=STACK_IP, proto=253, id=7777, flags="MF", frag=1) / Raw(b"B" * 16))), iface=IFACE)

    time.sleep(0.5)
    assert stack.alive(), "stack died on malformed input"
    after = stack.stats()

    for counter in [
        "ip_rx_bad_version",
        "ip_rx_bad_ihl",
        "ip_rx_bad_len",  # classes 3 and 4
        "ip_rx_bad_csum",
        "ip_rx_truncated",
        "ip_rx_bad_src",
        "icmp_rx_malformed",
        "icmp_rx_bad_csum",
        "ip_reass_overlap_drops",
    ]:
        assert after[counter] > before.get(counter, 0), f"{counter} not incremented"
    assert after["ip_rx_bad_len"] - before.get("ip_rx_bad_len", 0) >= 2

    # still alive and well: a normal ping must work afterwards
    tx, rx, _ = run_ping(["-c", "3", "-W", "2", STACK_IP])
    assert rx == 3


def test_unknown_protocol_unreachable(stack):
    """RFC 1122 §3.2.2.1: unknown transport → dest-unreachable code 2."""
    sn = AsyncSniffer(iface=IFACE, filter="icmp")
    sn.start()
    time.sleep(0.3)
    sendp(
        Ether(src=SCAPY_MAC, dst=STACK_MAC, type=0x0800)
        / IP(src=HOST_IP, dst=STACK_IP, proto=253)
        / Raw(b"PROTO253" * 4),
        iface=IFACE,
    )
    time.sleep(0.7)
    pkts = sn.stop()
    unreach = [
        p
        for p in pkts
        if p.haslayer(ICMP) and p[ICMP].type == 3 and p[ICMP].code == 2 and p[IP].src == STACK_IP
    ]
    assert unreach, f"no protocol-unreachable seen in {len(pkts)} sniffed packets"
    # RFC 792: error embeds the original header + 8 data bytes.
    inner = bytes(unreach[0][ICMP].payload)
    assert inner[9] == 253  # embedded original protocol field
    assert b"PROTO253" in inner