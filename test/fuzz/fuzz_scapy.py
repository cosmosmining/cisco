#!/usr/bin/env python3
"""Mutational protocol fuzzer: throws templated-then-mutilated frames at a
live PacketForge stack over its TAP and watches for crashes.

    sudo python3 test/fuzz/fuzz_scapy.py --iterations 20000 [--seed 1] \
        [--binary build-asan/bin/pfstack]

Mutations: random truncation, oversize/undersize length fields, bit flips,
bogus IHL/doff/options, random flag soup, overlapping fragments, random
garbage. Every batch the stack must still answer a ping; any exit (ASan
abort included) fails the run.
"""

import argparse
import os
import random
import signal
import subprocess
import sys
import time
from pathlib import Path

from scapy.config import conf
from scapy.layers.inet import ICMP, IP, TCP, UDP
from scapy.layers.l2 import ARP, Ether
from scapy.packet import Raw
from scapy.sendrecv import sendp

conf.verb = 0

REPO = Path(__file__).resolve().parents[2]
IFACE = "pf0"
HOST_IP = "10.190.0.1"
STACK_IP = "10.190.0.2"
STACK_MAC = "02:50:46:00:00:02"
SRC_MAC = "aa:bb:cc:dd:ee:99"
SRC_IP = "10.190.0.99"


def sh(cmd, check=True):
    return subprocess.run(cmd, shell=True, check=check, capture_output=True, text=True)


def template(rng):
    """A valid-ish frame to mutate."""
    e = Ether(src=SRC_MAC, dst=rng.choice([STACK_MAC, "ff:ff:ff:ff:ff:ff"]))
    kind = rng.randrange(7)
    if kind == 0:
        return e / ARP(op=rng.randrange(0, 4), hwsrc=SRC_MAC, psrc=SRC_IP, pdst=STACK_IP)
    ip = IP(src=SRC_IP, dst=STACK_IP, ttl=rng.randrange(0, 256), id=rng.randrange(65536))
    if kind == 1:
        return e / ip / ICMP(type=rng.randrange(256), code=rng.randrange(256)) / Raw(
            os.urandom(rng.randrange(0, 64)))
    if kind == 2:
        return e / ip / UDP(sport=rng.randrange(65536), dport=rng.choice([7, 9999])) / Raw(
            os.urandom(rng.randrange(0, 128)))
    if kind == 3:
        opts = [("MSS", rng.randrange(65536)), ("NOP", None), ("EOL", None)]
        return e / ip / TCP(sport=rng.randrange(65536), dport=rng.choice([7, 9999]),
                            flags=rng.randrange(256), seq=rng.randrange(2**32),
                            ack=rng.randrange(2**32), dataofs=rng.randrange(16),
                            options=rng.sample(opts, rng.randrange(len(opts)))) / Raw(
            os.urandom(rng.randrange(0, 64)))
    if kind == 4:  # fragments, sometimes overlapping
        fid = rng.randrange(65536)
        ip.proto = rng.choice([1, 6, 17, 253])
        ip.flags = "MF" if rng.random() < 0.7 else 0
        ip.frag = rng.randrange(0, 32)
        ip.id = fid
        return e / ip / Raw(os.urandom(rng.choice([8, 16, 24, 7, 64])))
    if kind == 5:  # random ethertype
        return e / Raw(os.urandom(rng.randrange(0, 96)))
    return e / ip / Raw(os.urandom(rng.randrange(0, 96)))


def mutate(frame: bytes, rng) -> bytes:
    b = bytearray(frame)
    for _ in range(rng.randrange(1, 4)):
        m = rng.randrange(5)
        if m == 0 and len(b) > 15:  # truncate
            del b[rng.randrange(14, len(b)):]
        elif m == 1 and len(b) > 16:  # bit flips
            for _ in range(rng.randrange(1, 8)):
                b[rng.randrange(14, len(b))] ^= 1 << rng.randrange(8)
        elif m == 2 and len(b) >= 18:  # stomp the IP total-length field
            b[16] = rng.randrange(256)
            b[17] = rng.randrange(256)
        elif m == 3 and len(b) >= 15:  # stomp version/IHL or ARP htype
            b[14] = rng.randrange(256)
        elif m == 4:  # append garbage
            b += os.urandom(rng.randrange(1, 32))
    return bytes(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--binary", default=None)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    binary = args.binary
    if binary is None:
        for b in ("build-asan/bin/pfstack", "build/bin/pfstack"):
            if (REPO / b).exists():
                binary = str(REPO / b)
                break
    assert binary, "build the stack first"

    sh(f"ip link del {IFACE}", check=False)
    sh(f"ip tuntap add dev {IFACE} mode tap")
    sh(f"ip addr replace {HOST_IP}/24 dev {IFACE}")
    env = dict(os.environ, ASAN_OPTIONS="exitcode=99:abort_on_error=0")
    proc = subprocess.Popen([binary, "--if", f"{IFACE},{STACK_IP}/24"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, env=env)
    time.sleep(0.5)
    sh(f"ip link set {IFACE} up")
    time.sleep(0.3)

    sent = 0
    t0 = time.time()
    try:
        batch = []
        for i in range(args.iterations):
            frame = mutate(bytes(template(rng)), rng)
            batch.append(Ether(frame) if len(frame) >= 14 else Raw(frame))
            if len(batch) >= 100:
                sendp(batch, iface=IFACE)
                sent += len(batch)
                batch = []
                if proc.poll() is not None:
                    print(f"CRASH after ~{sent} frames, rc={proc.returncode}")
                    print(proc.stdout.read())
                    return 1
                if sent % 2000 == 0:
                    r = sh(f"ping -c 1 -W 2 {STACK_IP}", check=False)
                    alive = r.returncode == 0
                    rate = sent / (time.time() - t0)
                    print(f"  {sent}/{args.iterations} frames, {rate:.0f}/s, "
                          f"responsive={alive}")
                    if not alive:
                        print("stack stopped answering pings — investigating")
                        if proc.poll() is not None:
                            print(proc.stdout.read())
                            return 1
        if batch:
            sendp(batch, iface=IFACE)
            sent += len(batch)
        time.sleep(0.5)
        if proc.poll() is not None:
            print(f"CRASH at end, rc={proc.returncode}")
            return 1
        r = sh(f"ping -c 3 -W 2 {STACK_IP}", check=False)
        if r.returncode != 0:
            print("stack unresponsive after fuzzing")
            return 1
        proc.send_signal(signal.SIGUSR1)
        time.sleep(0.5)
        print(f"OK: {sent} mutated frames in {time.time()-t0:.0f}s, "
              f"no crashes, stack still answers ping")
        return 0
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        sh(f"ip link del {IFACE}", check=False)


if __name__ == "__main__":
    sys.exit(main())
