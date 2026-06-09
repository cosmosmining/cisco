"""Phase 5 gate: routing between namespaces, traceroute hop, CLI, counters."""

import re
import socket
import subprocess
import time

import pytest

from conftest import REPO, find_bin, sh

ROUTER_IP_HOSTSIDE = "10.191.1.2"  # router on the pfhost segment
ROUTER_IP_SRVSIDE = "10.191.2.2"
HOST_ADDR = "10.191.1.1"
SRV_ADDR = "10.191.2.1"
CLI_SOCK = "/tmp/pf-cli-test.sock"


def cli(cmd, timeout=5):
    """One CLI exchange over the UNIX socket; returns the response text."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(CLI_SOCK)
    buf = b""
    while b"pf> " not in buf:  # banner + prompt
        buf += s.recv(4096)
    s.sendall(cmd.encode() + b"\n")
    buf = b""
    while not buf.endswith(b"pf> "):
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    s.close()
    return buf.decode(errors="replace").rsplit("pf> ", 1)[0]


@pytest.fixture
def router(request):
    """PacketForge as the router between two namespaces (test/topo.sh)."""
    import os
    import signal as sigmod

    sh(f"{REPO}/test/topo.sh down", check=False)
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "exitcode=99:abort_on_error=0"
    proc = subprocess.Popen(
        [
            str(find_bin("pfstack")),
            "--if", f"rt0,{ROUTER_IP_HOSTSIDE}/24",
            "--if", f"rt1,{ROUTER_IP_SRVSIDE}/24",
            "--forward",
            "--cli", CLI_SOCK,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
    )
    deadline = time.time() + 5
    for line in proc.stdout:
        if "ready" in line:
            break
        if time.time() > deadline:
            raise RuntimeError("router failed to start")
    sh(f"{REPO}/test/topo.sh up")
    time.sleep(0.3)

    def fin():
        if proc.poll() is None:
            proc.send_signal(sigmod.SIGINT)
            try:
                rc = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                rc = proc.wait()
        else:
            rc = proc.returncode
        sh(f"{REPO}/test/topo.sh down", check=False)
        assert rc in (0, -2), f"router exited rc={rc}"

    request.addfinalizer(fin)
    return proc


def ns(nsname, cmd, timeout=60, check=True):
    r = subprocess.run(
        f"ip netns exec {nsname} {cmd}", shell=True, capture_output=True, text=True,
        timeout=timeout,
    )
    if check:
        assert r.returncode == 0, f"{cmd}: {r.stdout}{r.stderr}"
    return r


def test_ping_across_router(router):
    """Gate 5: pfhost → router → pfserver, 0% loss."""
    r = ns("pfhost", f"ping -c 20 -i 0.05 -W 2 -q {SRV_ADDR}")
    assert "20 received, 0% packet loss" in r.stdout, r.stdout
    # and the router's own far-side address answers (weak host model)
    r = ns("pfhost", f"ping -c 3 -W 2 -q {ROUTER_IP_SRVSIDE}")
    assert "3 received" in r.stdout


def test_traceroute_shows_router_hop(router):
    """Gate 5: the PacketForge router appears as hop 1 (our ICMP
    time-exceeded), the server as hop 2."""
    r = ns("pfhost", f"traceroute -n -m 4 -w 2 -q 2 {SRV_ADDR}", timeout=90)
    lines = r.stdout.splitlines()
    hops = {int(m.group(1)): m.group(2)
            for l in lines if (m := re.match(r"\s*(\d+)\s+([\d.]+)", l))}
    assert hops.get(1) == ROUTER_IP_HOSTSIDE, r.stdout
    assert hops.get(2) == SRV_ADDR, r.stdout


def test_iperf3_udp_through_router(router):
    """Gate 5: iperf3 UDP crosses the router; report the real numbers."""
    srv = subprocess.Popen(
        f"ip netns exec pfserver iperf3 -s -1 -p 5201", shell=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(0.5)
    r = ns("pfhost", f"iperf3 -u -c {SRV_ADDR} -b 50M -t 3 -p 5201 -f m", timeout=60)
    srv.wait(timeout=10)
    m = re.search(r"([\d.]+) Mbits/sec\s+[\d.]+ ms\s+(\d+)/(\d+) \((\d+(?:\.\d+)?)%\)", r.stdout)
    assert m, r.stdout
    mbps, lost, total, loss_pct = float(m.group(1)), int(m.group(2)), int(m.group(3)), float(m.group(4))
    print(f"iperf3 UDP through router: {mbps} Mbit/s, {lost}/{total} lost ({loss_pct}%)")
    assert mbps > 10, "router forwarded implausibly slowly"
    assert loss_pct < 20, f"excessive loss {loss_pct}%"


def test_cli_route_add_remove_changes_forwarding(router):
    """Gate 5: a second address on the server is reachable only while the
    CLI route exists."""
    ns("pfserver", "ip addr add 10.191.3.1/32 dev rt1")
    ns("pfserver", "ip route add 10.191.3.0/24 dev rt1", check=False)

    r = ns("pfhost", f"ping -c 2 -W 1 -q 10.191.3.1", check=False)
    assert "0 received" in r.stdout or "100% packet loss" in r.stdout

    out = cli("ip route 10.191.3.0 255.255.255.0 10.191.2.1")
    assert "%" not in out, out  # no error
    r = ns("pfhost", f"ping -c 5 -i 0.1 -W 2 -q 10.191.3.1")
    assert "5 received, 0% packet loss" in r.stdout, r.stdout

    out = cli("no ip route 10.191.3.0 255.255.255.0")
    r = ns("pfhost", f"ping -c 2 -W 1 -q 10.191.3.1", check=False)
    assert "0 received" in r.stdout or "100% packet loss" in r.stdout

    # show ip route reflects the connected routes
    routes = cli("show ip route")
    assert "C    10.191.1.0/24 is directly connected, rt0" in routes
    assert "C    10.191.2.0/24 is directly connected, rt1" in routes


def test_cli_show_commands(router):
    out = cli("show interfaces")
    assert "rt0 is up" in out and "rt1 is up" in out
    assert "Internet address 10.191.1.2" in out

    out = cli("show tcp brief")
    assert "Local Address" in out

    out = cli("show ip traffic")
    assert "ip_fwd_forwarded" in out

    out = cli("show arp")
    assert "Address" in out


def get_fwd_count():
    out = cli("show ip traffic")
    m = re.search(r"ip_fwd_forwarded\s+(\d+)", out)
    return int(m.group(1))


def test_counters_match_tcpdump(router):
    """Gate 5: forwarded-packet counter matches tcpdump ground truth ±1%."""
    before = get_fwd_count()

    # tcpdump exits by itself at -c 100 (50 requests + 50 replies); the
    # timeout wrapper reaps it if packets go missing.
    td = subprocess.Popen(
        f"timeout 15 ip netns exec pfhost tcpdump -ni rt0 -c 100 icmp and host {SRV_ADDR}",
        shell=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    )
    time.sleep(0.8)
    ns("pfhost", f"ping -c 50 -i 0.02 -W 2 -q {SRV_ADDR}")
    out, _ = td.communicate(timeout=20)
    tcpdump_pkts = len([l for l in out.splitlines() if SRV_ADDR in l])

    delta = get_fwd_count() - before
    # 50 requests + 50 replies traverse rt0; both also cross the router.
    assert tcpdump_pkts == 100, f"tcpdump saw {tcpdump_pkts}"
    assert abs(delta - tcpdump_pkts) <= max(1, tcpdump_pkts // 100), (
        f"router forwarded {delta}, tcpdump saw {tcpdump_pkts}"
    )
    print(f"counters: router fwd delta={delta}, tcpdump ground truth={tcpdump_pkts}")