"""Phase 4 gate: curl, nc echo, scapy state-machine probes, lossy 1 MB."""

import hashlib
import socket
import subprocess
import textwrap
import time

import pytest
from scapy.layers.inet import IP, TCP
from scapy.layers.l2 import ARP, Ether
from scapy.packet import Raw
from scapy.sendrecv import sendp, srp1

from conftest import (HOST_IP, IFACE, SCAPY_IP, SCAPY_MAC, STACK_IP, STACK_MAC,
                      sh, start_app)


@pytest.fixture
def tcp_echo(request):
    return start_app(request, "tcp_echo", ["--if", f"{IFACE},{STACK_IP}/24", "--port", "7"])


@pytest.fixture
def httpd(request):
    return start_app(request, "httpd", ["--if", f"{IFACE},{STACK_IP}/24", "--port", "80"])


def seed_scapy_arp():
    """Teach the stack SCAPY_IP↔SCAPY_MAC (we are the target of a request)."""
    sendp(
        Ether(src=SCAPY_MAC, dst="ff:ff:ff:ff:ff:ff")
        / ARP(op=1, hwsrc=SCAPY_MAC, psrc=SCAPY_IP, pdst=STACK_IP),
        iface=IFACE,
    )
    time.sleep(0.2)


def tcp_pkt(sport, dport, flags, seq, ack=0, payload=b""):
    p = Ether(src=SCAPY_MAC, dst=STACK_MAC) / IP(src=SCAPY_IP, dst=STACK_IP) / TCP(
        sport=sport, dport=dport, flags=flags, seq=seq, ack=ack, window=65000
    )
    if payload:
        p = p / Raw(payload)
    return p


def xchg(pkt, timeout=2):
    return srp1(pkt, iface=IFACE, timeout=timeout)


# ---------------------------------------------------------------- gates ----


def test_curl_fetches_page(httpd):
    """Gate 4: curl http://<stack-ip>/ fetches the page."""
    r = subprocess.run(
        ["curl", "-s", "--max-time", "15", f"http://{STACK_IP}/"],
        capture_output=True,
        text=True,
    )
    assert r.returncode == 0
    assert "PacketForge" in r.stdout
    assert "from-scratch TCP/IP stack" in r.stdout


def test_curl_blob_1mb_hash(httpd):
    """1 MiB over HTTP with end-to-end hash verification."""
    r = subprocess.run(
        ["curl", "-s", "--max-time", "60", f"http://{STACK_IP}/blob"],
        capture_output=True,
    )
    assert r.returncode == 0
    assert len(r.stdout) == 1024 * 1024
    assert (
        hashlib.sha256(r.stdout).hexdigest()
        == "3e75ec671075e4115fc876c4c55d5abbac3bfe487a67ff4ca2db38386a014f27"
    )


def test_nc_interactive_echo(tcp_echo):
    """Gate 4: `nc <stack-ip> 7` echo — kernel TCP client, several writes."""
    s = socket.create_connection((STACK_IP, 7), timeout=5)
    for i in range(5):
        msg = f"line {i}: the quick brown fox\n".encode()
        s.sendall(msg)
        got = b""
        while len(got) < len(msg):
            chunk = s.recv(1024)
            assert chunk, "connection closed early"
            got += chunk
        assert got == msg
    s.close()

    r = subprocess.run(
        f"printf 'one-shot nc check\\n' | nc -N -w 3 {STACK_IP} 7",
        shell=True,
        capture_output=True,
        text=True,
        timeout=15,
    )
    assert "one-shot nc check" in r.stdout


def test_syn_to_closed_port_gets_rst(tcp_echo):
    """Gate 4: SYN to a closed port → RST|ACK with ACK=ISS+1 (RFC 9293
    §3.10.7.1)."""
    seed_scapy_arp()
    iss = 0x1000
    r = xchg(tcp_pkt(40001, 9999, "S", iss))
    assert r is not None and r.haslayer(TCP)
    t = r[TCP]
    assert t.flags.R and t.flags.A
    assert t.seq == 0
    assert t.ack == iss + 1


def test_half_open_teardown(tcp_echo):
    """Gate 4: half-open connection discovery per RFC 9293 §3.6 fig. 10 —
    a crashed peer's fresh SYN gets a challenge ACK (RFC 5961 §4); its RST
    at that ACK's sequence then kills the stale connection."""
    seed_scapy_arp()
    sport, iss = 40002, 0x2000

    synack = xchg(tcp_pkt(sport, 7, "S", iss))
    assert synack is not None and synack[TCP].flags.S and synack[TCP].flags.A
    srv_nxt = synack[TCP].seq + 1
    sendp(tcp_pkt(sport, 7, "A", iss + 1, srv_nxt), iface=IFACE)
    time.sleep(0.2)  # established on the stack side

    # "Crashed" client comes back with a brand-new SYN on the same 4-tuple.
    # (Sniff instead of srp1: the challenge ACK acks the OLD connection, so
    # scapy's answer matching would reject it — that mismatch is the point.)
    from scapy.sendrecv import AsyncSniffer

    iss2 = 0x9000
    sn = AsyncSniffer(iface=IFACE, filter=f"tcp and src host {STACK_IP}")
    sn.start()
    time.sleep(0.2)
    sendp(tcp_pkt(sport, 7, "S", iss2), iface=IFACE)
    time.sleep(0.7)
    pkts = sn.stop()
    chals = [p for p in pkts if p.haslayer(TCP) and p[TCP].dport == sport]
    assert chals, "no response to the in-window SYN"
    t = chals[0][TCP]
    assert t.flags == "A", f"expected challenge ACK, got {t.flags}"  # not RST
    assert t.ack == iss + 1  # still the OLD connection's state

    # Per RFC 5961 the legitimate-but-stateless peer answers RST at SEG.ACK.
    sendp(tcp_pkt(sport, 7, "R", t.ack), iface=IFACE)
    time.sleep(0.3)

    # The old connection must be gone: data on it now draws a RST.
    sn = AsyncSniffer(iface=IFACE, filter=f"tcp and src host {STACK_IP}")
    sn.start()
    time.sleep(0.2)
    sendp(tcp_pkt(sport, 7, "PA", iss + 1, srv_nxt, b"ghost"), iface=IFACE)
    time.sleep(0.7)
    pkts = sn.stop()
    rsts = [p for p in pkts if p.haslayer(TCP) and p[TCP].dport == sport and p[TCP].flags.R]
    assert rsts, "stale connection still answered without RST"

    st = tcp_echo.stats()
    assert st["tcp_challenge_acks_tx"] >= 1
    assert st["tcp_conns_reset"] >= 1


def test_out_of_order_segments_reassembled(tcp_echo):
    """Gate 4: segment 2 before segment 1; echo returns the joined payload."""
    seed_scapy_arp()
    sport, iss = 40003, 0x3000

    synack = xchg(tcp_pkt(sport, 7, "S", iss))
    assert synack is not None
    srv_nxt = synack[TCP].seq + 1
    sendp(tcp_pkt(sport, 7, "A", iss + 1, srv_nxt), iface=IFACE)
    time.sleep(0.2)

    # Out of order: WORLD (seq+5) first → immediate dup ACK at the hole.
    dup = xchg(tcp_pkt(sport, 7, "PA", iss + 6, srv_nxt, b"WORLD"))
    assert dup is not None and dup[TCP].ack == iss + 1

    # HELLO fills the hole → cumulative ACK + the echoed 10 bytes.
    from scapy.sendrecv import AsyncSniffer

    sn = AsyncSniffer(iface=IFACE, filter=f"tcp and src host {STACK_IP}")
    sn.start()
    time.sleep(0.2)
    sendp(tcp_pkt(sport, 7, "PA", iss + 1, srv_nxt, b"HELLO"), iface=IFACE)
    time.sleep(1.0)
    pkts = sn.stop()

    acks = [p for p in pkts if p.haslayer(TCP) and p[TCP].ack == iss + 11]
    assert acks, "no cumulative ACK covering both segments"
    echoed = b"".join(bytes(p[Raw]) for p in pkts if p.haslayer(Raw))
    assert b"HELLOWORLD" in echoed

    st = tcp_echo.stats()
    assert st["tcp_ooo_queued"] >= 1
    sendp(tcp_pkt(sport, 7, "R", iss + 11, srv_nxt), iface=IFACE)  # cleanup


# ------------------------------------------------- lossy 1 MB transfer ----

NSNAME = "pfclient"
VETH_HOST = "192.168.77.1"
VETH_CLIENT = "192.168.77.2"


@pytest.fixture
def lossy_echo(request):
    """tcp_echo behind 5% loss in BOTH directions:

        [netns pfclient] veth ── root ns (router) ── tap pf0 ── PacketForge
                                netem 5%↑              netem 5%↓
    """
    stack = start_app(
        request,
        "tcp_echo",
        ["--if", f"{IFACE},{STACK_IP}/24", "--port", "7", "--gw", HOST_IP],
    )

    sh(f"ip netns del {NSNAME}", check=False)
    sh(f"ip netns add {NSNAME}")
    sh(f"ip link add pfv0 type veth peer name pfv1")
    sh(f"ip link set pfv1 netns {NSNAME}")
    sh(f"ip addr add {VETH_HOST}/24 dev pfv0 && ip link set pfv0 up")
    sh(f"ip netns exec {NSNAME} ip addr add {VETH_CLIENT}/24 dev pfv1")
    sh(f"ip netns exec {NSNAME} ip link set pfv1 up && ip netns exec {NSNAME} ip link set lo up")
    sh(f"ip netns exec {NSNAME} ip route add default via {VETH_HOST}")
    sh("sysctl -qw net.ipv4.ip_forward=1")
    sh("sysctl -qw net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.pfv0.rp_filter=0 "
       f"net.ipv4.conf.{IFACE}.rp_filter=0")

    # 5% loss in both directions. Prefer tc netem (the gate's tool — present
    # on CI kernels); fall back to iptables xt_statistic random drop on the
    # FORWARD chain when the kernel lacks CONFIG_NET_SCH_NETEM (this drops
    # both directions, since the root ns forwards client↔stack traffic).
    DROPRULE = "FORWARD -m statistic --mode random --probability 0.05 -j DROP"
    netem_ok = sh(f"tc qdisc add dev {IFACE} root netem loss 5% limit 1000",
                  check=False).returncode == 0
    if netem_ok:
        sh("tc qdisc add dev pfv0 root netem loss 5% limit 1000")
        print("loss via tc netem 5%")
    else:
        sh(f"iptables -A {DROPRULE}")
        print("kernel lacks sch_netem; loss via iptables statistic 5%")

    def fin():
        if not netem_ok:
            sh(f"iptables -D {DROPRULE}", check=False)
        sh("ip link del pfv0", check=False)
        sh(f"ip netns del {NSNAME}", check=False)

    request.addfinalizer(fin)
    return stack


CLIENT_SCRIPT = textwrap.dedent(
    """
    import hashlib, socket, sys, threading
    SIZE = 1024 * 1024
    data = bytes((i * 31 + 7) & 0xFF for i in range(SIZE))
    s = socket.create_connection(("%s", 7), timeout=60)
    s.settimeout(120)
    rx = hashlib.sha256()
    got = 0
    def reader():
        global got
        while got < SIZE:
            chunk = s.recv(65536)
            if not chunk:
                break
            rx.update(chunk)
            got += len(chunk)
    t = threading.Thread(target=reader)
    t.start()
    for off in range(0, SIZE, 8192):
        s.sendall(data[off:off + 8192])
    t.join(timeout=180)
    ok = got == SIZE and rx.hexdigest() == hashlib.sha256(data).hexdigest()
    print("HASH_OK" if ok else f"HASH_BAD got={got}")
    sys.exit(0 if ok else 1)
    """
    % STACK_IP
)


def test_1mb_transfer_with_5pct_loss(lossy_echo):
    """Gate 4: tc netem 5%% loss each way; 1 MiB echo round-trip completes
    with SHA-256 verified — our RTO/fast-retransmit does the recovery."""
    r = subprocess.run(
        ["ip", "netns", "exec", NSNAME, "python3", "-c", CLIENT_SCRIPT],
        capture_output=True,
        text=True,
        timeout=300,
    )
    assert r.returncode == 0, r.stdout + r.stderr
    assert "HASH_OK" in r.stdout

    st = lossy_echo.stats()
    print(
        f"lossy transfer stats: tx_segs={st['tcp_tx_segs']} rtx_segs={st['tcp_rtx_segs']} "
        f"rto_fires={st['tcp_rto_fires']} fast_rtx={st['tcp_fast_rtx']} "
        f"dupacks={st['tcp_dupacks_rx']} ooo_queued={st['tcp_ooo_queued']} "
        f"rtt_samples={st['tcp_rtt_samples']}"
    )
    # Real loss happened and was repaired by retransmission.
    assert st["tcp_rtx_segs"] > 0
    assert st["tcp_rx_bytes"] >= 1024 * 1024
    assert st["tcp_tx_bytes"] >= 1024 * 1024
