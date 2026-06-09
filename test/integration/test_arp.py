"""Phase 1 gate: ARP request/reply over a real TAP device."""

import subprocess

from scapy.layers.l2 import ARP, Ether
from scapy.sendrecv import srp1

from conftest import IFACE, SCAPY_IP, SCAPY_MAC, STACK_IP, STACK_MAC


def test_arping_gets_replies(stack):
    """Gate 1: `arping <stack-ip>` from the host gets replies."""
    r = subprocess.run(
        ["arping", "-I", IFACE, "-c", "3", STACK_IP],
        capture_output=True,
        text=True,
        timeout=15,
    )
    assert r.returncode == 0, r.stdout + r.stderr
    replies = [l for l in r.stdout.splitlines() if "reply from " + STACK_IP in l.lower()]
    assert len(replies) == 3, r.stdout
    assert STACK_MAC.upper() in r.stdout.upper()


def test_arp_reply_fields(stack):
    """Gate 1: scapy asserts opcode and sender/target fields of the reply."""
    req = Ether(src=SCAPY_MAC, dst="ff:ff:ff:ff:ff:ff") / ARP(
        op=1, hwsrc=SCAPY_MAC, psrc=SCAPY_IP, pdst=STACK_IP
    )
    rep = srp1(req, iface=IFACE, timeout=2)
    assert rep is not None, "no ARP reply"
    arp = rep[ARP]
    assert arp.op == 2  # is-at
    assert arp.hwsrc.lower() == STACK_MAC
    assert arp.psrc == STACK_IP
    assert arp.hwdst.lower() == SCAPY_MAC
    assert arp.pdst == SCAPY_IP
    assert rep[Ether].dst.lower() == SCAPY_MAC  # unicast reply, not broadcast


def test_arp_not_for_us_ignored(stack):
    req = Ether(src=SCAPY_MAC, dst="ff:ff:ff:ff:ff:ff") / ARP(
        op=1, hwsrc=SCAPY_MAC, psrc=SCAPY_IP, pdst="10.190.0.77"
    )
    rep = srp1(req, iface=IFACE, timeout=1)
    assert rep is None


def test_malformed_arp_does_not_kill_stack(stack):
    """Truncated and nonsense ARP frames must be dropped, not crash."""
    from scapy.sendrecv import sendp

    bad_frames = [
        Ether(src=SCAPY_MAC, dst="ff:ff:ff:ff:ff:ff", type=0x0806) / b"\x00\x01",
        Ether(src=SCAPY_MAC, dst="ff:ff:ff:ff:ff:ff", type=0x0806) / b"\xff" * 27,
        Ether(src=SCAPY_MAC, dst="ff:ff:ff:ff:ff:ff")
        / ARP(op=9, hwsrc=SCAPY_MAC, psrc=SCAPY_IP, pdst=STACK_IP),
    ]
    for f in bad_frames:
        sendp(f, iface=IFACE)

    assert stack.alive()
    # and it still answers a good request
    req = Ether(src=SCAPY_MAC, dst="ff:ff:ff:ff:ff:ff") / ARP(
        op=1, hwsrc=SCAPY_MAC, psrc=SCAPY_IP, pdst=STACK_IP
    )
    rep = srp1(req, iface=IFACE, timeout=2)
    assert rep is not None and rep[ARP].op == 2
