"""Shared fixtures: bring up a TAP, run the stack binary on it, capture pcaps.

Everything here needs root (TAP creation + raw sockets for scapy).
"""

import os
import signal
import subprocess
import time
from pathlib import Path

import pytest
from scapy.config import conf

conf.verb = 0

REPO = Path(__file__).resolve().parents[2]
ARTIFACTS = REPO / "test" / "artifacts"

IFACE = "pf0"
HOST_IP = "10.190.0.1"
STACK_IP = "10.190.0.2"
STACK_MAC = "02:50:46:00:00:02"
# Address scapy impersonates so the kernel doesn't fight over the flows.
SCAPY_IP = "10.190.0.99"
SCAPY_MAC = "aa:bb:cc:dd:ee:99"


def sh(cmd, check=True):
    return subprocess.run(cmd, shell=True, check=check, capture_output=True, text=True)


def find_bin(name):
    """Prefer the sanitizer build when present (CI builds SAN=asan)."""
    for b in ("build-asan", "build"):
        p = REPO / b / "bin" / name
        if p.exists():
            return p
    pytest.fail(f"{name} not built — run `make` (or `make SAN=asan`) first")


class Stack:
    """A pfstack process (or any stack app) attached to IFACE."""

    def __init__(self, proc, iface):
        self.proc = proc
        self.iface = iface

    def alive(self):
        return self.proc.poll() is None

    def output_so_far(self):
        return self.proc.stdout  # only valid after stop()


def start_app(request, name, args, iface=IFACE, host_ip=HOST_IP, ready_line="ready"):
    """Create TAP, start a stack app on it, wait for its ready line."""
    sh(f"ip tuntap add dev {iface} mode tap", check=False)
    sh(f"ip addr replace {host_ip}/24 dev {iface}")

    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "exitcode=99:abort_on_error=0"
    proc = subprocess.Popen(
        [str(find_bin(name))] + args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
    )
    deadline = time.time() + 5
    for line in proc.stdout:
        if ready_line in line:
            break
        if time.time() > deadline or proc.poll() is not None:
            raise RuntimeError(f"{name} failed to start: {line}")
    sh(f"ip link set {iface} up")
    # The TAP is recreated per test with a new ifindex; scapy caches these.
    conf.ifaces.reload()

    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    pcap = ARTIFACTS / f"{request.node.name}.pcap"
    td = subprocess.Popen(
        ["tcpdump", "-i", iface, "-w", str(pcap), "-U", "-q"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(0.3)  # let tcpdump attach

    stack = Stack(proc, iface)

    def fin():
        td.terminate()
        td.wait(timeout=5)
        rc = None
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                rc = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                rc = proc.wait()
        else:
            rc = proc.returncode
        out = proc.stdout.read()
        sh(f"ip link del {iface}", check=False)
        # A sanitizer abort (99) or crash anywhere in the run fails the test.
        assert rc in (0, -signal.SIGINT.value), f"stack exited rc={rc}\n{out}"

    request.addfinalizer(fin)
    return stack


@pytest.fixture
def stack(request):
    """Plain pfstack bound to IFACE with STACK_IP."""
    return start_app(request, "pfstack", ["--if", f"{IFACE},{STACK_IP}/24"])
