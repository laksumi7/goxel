#!/usr/bin/env python3
"""AMFM #534: build the three belly PD mounts live through the LAB bridge.

The Owner asked for one PD under the bow and one under the middle of each wing.
The mount is the dorsal PD template flipped in Z - pad on the belly, then
turntable, body, two forward barrels and the white sensor hanging below - and
every cell goes in through `set` so each one is a separate undo step in the
editor.  The two barrel cells of each mount are then rounded with `shape`.

    python belly_mounts.py [port] [--dry]

Notes:
  * The bridge does not send the terminating "ok" on error replies, so the
    reader below tolerates a reply without one instead of hanging.
  * Positions were chosen with pad_check.py: a flat 3x3 belly plate with clear
    air below it and a clear barrel corridor forward.
"""
import socket
import sys
import time

PAD = (46, 46, 46)
BODY = (37, 37, 37)
SENSOR = (255, 255, 255)

# name        pad x   pad y   belly z
MOUNTS = [
    ("bow",        -1,  -31,   -8),
    ("port_wing", -17,   13,   -9),
    ("stbd_wing",  14,   13,   -9),
]


def mount_cells(px, py, zb):
    """(cells, barrels); the barrels are included in cells, placed last."""
    pz = zb - 1
    bz = pz - 2
    cells = [(px + dx, py + dy, pz, PAD) for dx in range(3) for dy in range(3)]
    cells.append((px + 1, py + 1, pz - 1, PAD))            # turntable
    cells.append((px + 1, py + 1, bz, BODY))
    for dx in range(3):
        for dy in (0, -1, -2):
            cells.append((px + dx, py + dy, bz, BODY))
    cells.append((px + 1, py, pz - 3, SENSOR))             # sensor
    barrels = [(px, py - 3, bz), (px + 2, py - 3, bz)]
    cells += [(x, y, z, BODY) for (x, y, z) in barrels]
    return cells, barrels


class Bridge:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)

    def run(self, cmd, wait=4.0):
        self.s.sendall((cmd + "\n").encode())
        self.s.settimeout(wait)
        buf = b""
        while True:
            try:
                d = self.s.recv(65536)
            except (socket.timeout, TimeoutError):
                break
            if not d:
                break
            buf += d
            if buf.endswith(b"ok\n") or b"\nok\n" in buf:
                break
        return buf.decode(errors="replace").strip().replace("\n", " ")


def main():
    args = [a for a in sys.argv[1:]]
    dry = "--dry" in args
    args = [a for a in args if not a.startswith("--")]
    port = int(args[0]) if args else 47534

    if dry:
        for name, px, py, zb in MOUNTS:
            cells, barrels = mount_cells(px, py, zb)
            print(f"{name}: {len(cells)} cells, barrels {barrels}")
        return

    b = Bridge(port)
    print("before:", b.run("state"))
    for name, px, py, zb in MOUNTS:
        cells, barrels = mount_cells(px, py, zb)
        print(f"--- {name}: pad x{px}..{px+2} y{py}..{py+2} belly z{zb}")
        for (x, y, z, col) in cells:
            r = b.run(f"set {x} {y} {z} {col[0]} {col[1]} {col[2]}")
            if "pushed" not in r and "noop" not in r:
                print(f"  !! {x} {y} {z}: {r}")
            time.sleep(0.09)
        for i, (x, y, z) in enumerate(barrels):
            tag = f"PD_belly_{name}_{'LR'[i]}"
            print(f"  {tag}: {b.run(f'shape {x} {y} {z} 0.45 {tag}')}")
            time.sleep(0.4)
    print("after:", b.run("state"))


main()
