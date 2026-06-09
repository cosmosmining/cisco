"""Shared fixtures: bring up a TAP, run the stack binary on it, capture pcaps.

Everything here needs root (TAP creation + raw sockets for scapy).
"""

import os
import signal
import subprocess
import threading
import time
from pathlib import Path

import pytest
import scapy.interfaces  # populate conf.ifaces even if a test imports no layers
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
    """Prefer PF_BUILD (e.g. coverage builds), then the sanitizer build."""
    candidates = [os.environ.get("PF_BUILD"), "build-asan", "build"]
    for b in candidates:
        if not b:
            continue
        p = REPO / b / "bin" / name
        if p.exists():
            return p
    pytest.fail(f"{name} not built — run `make` (or `make SAN=asan`) first")


class Stack:
    """A stack app subprocess with a line-buffered output reader."""

    def __init__(self, proc, iface):
        self.proc = proc
        self.iface = iface
        self.lines = []
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()

    def _drain(self):
        for line in self.proc.stdout:
            with self._lock:
                self.lines.append(line.rstrip("\n"))

    def wait_line(self, needle, timeout=5, start=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                for i in range(start, len(self.lines)):
                    if needle in self.lines[i]:
                        return i
            if self.proc.poll() is not None:
                break
            time.sleep(0.02)
        with self._lock:
            dump = "\n".join(self.lines)
        raise TimeoutError(f"never saw {needle!r} in stack output:\n{dump}")

    def alive(self):
        return self.proc.poll() is None

    def stats(self):
        """SIGUSR1 → parse the freshly dumped counter block."""
        with self._lock:
            start = len(self.lines)
        self.proc.send_signal(signal.SIGUSR1)
        end = self.wait_line("--- end stats ---", timeout=5, start=start)
        out = {}
        with self._lock:
            for line in self.lines[start:end]:
                parts = line.split()
                if len(parts) == 3 and parts[0] == "stat":
                    out[parts[1]] = int(parts[2])
        return out


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
    stack = Stack(proc, iface)
    stack.wait_line(ready_line, timeout=5)
    sh(f"ip link set {iface} up")
    # The TAP is recreated per test with a new ifindex; scapy caches these.
    if conf.ifaces is not None:
        conf.ifaces.reload()

    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    pcap = ARTIFACTS / f"{request.node.name}.pcap"
    td = subprocess.Popen(
        ["tcpdump", "-i", iface, "-w", str(pcap), "-U", "-q"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(0.3)  # let tcpdump attach

    def fin():
        td.terminate()
        td.wait(timeout=5)
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                rc = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                rc = proc.wait()
        else:
            rc = proc.returncode
        sh(f"ip link del {iface}", check=False)
        out = "\n".join(stack.lines)
        # A sanitizer abort (99) or crash anywhere in the run fails the test.
        assert rc in (0, -signal.SIGINT.value), f"stack exited rc={rc}\n{out}"

    request.addfinalizer(fin)
    return stack


@pytest.fixture
def stack(request):
    """Plain pfstack bound to IFACE with STACK_IP."""
    return start_app(request, "pfstack", ["--if", f"{IFACE},{STACK_IP}/24"])
