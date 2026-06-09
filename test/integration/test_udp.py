"""Phase 3 gate: 1,000 UDP echo round-trips, port-unreachable, nc interop."""

import socket
import subprocess
import time

import pytest

from conftest import IFACE, STACK_IP, start_app


@pytest.fixture
def udp_echo(request):
    return start_app(request, "udp_echo", ["--if", f"{IFACE},{STACK_IP}/24", "--port", "7"])


def test_udp_echo_1000_roundtrips(udp_echo):
    """Gate 3: 1,000 echo round-trips with payload verification and zero
    checksum failures (Linux kernel verifies our checksums on its side;
    the stack's own counters prove the inbound direction)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    ok = 0
    for i in range(1000):
        payload = bytes((i + j) & 0xFF for j in range(1 + (i % 1200)))
        s.sendto(payload, (STACK_IP, 7))
        data, addr = s.recvfrom(2048)
        assert addr[0] == STACK_IP
        assert data == payload, f"payload mismatch on iteration {i}"
        ok += 1
    s.close()
    assert ok == 1000

    st = udp_echo.stats()
    assert st["udp_rx"] >= 1000
    assert st["udp_rx_delivered"] >= 1000
    assert st["udp_tx"] >= 1000
    assert st["udp_rx_bad_csum"] == 0, "checksum failures on echo traffic"


def test_closed_port_gets_icmp_port_unreachable(udp_echo):
    """Gate 3: UDP to a closed port → ICMP type 3 code 3, surfaced by the
    kernel as ECONNREFUSED on a connected UDP socket."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    s.connect((STACK_IP, 9999))
    s.send(b"anyone home?")
    time.sleep(0.2)
    with pytest.raises(ConnectionRefusedError):
        s.send(b"second")  # the queued ICMP error fires on the next call
        s.recv(64)
    s.close()

    st = udp_echo.stats()
    assert st["udp_rx_no_sock"] >= 1
    assert st["icmp_err_tx"] >= 1


def test_nc_udp_interop(udp_echo):
    """Gate 3: host-side `nc -u` round-trip."""
    r = subprocess.run(
        f"echo 'hello packetforge' | nc -u -w1 {STACK_IP} 7",
        shell=True,
        capture_output=True,
        text=True,
        timeout=15,
    )
    assert "hello packetforge" in r.stdout
