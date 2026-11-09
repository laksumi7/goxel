#!/usr/bin/env python3
"""Check candidate belly hardpoints: flat surface, and clear air below it."""
import sys

cells = set()
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    p = line.split()
    if len(p) < 7 or p[0] != "cell":
        continue
    cells.add((int(p[2]), int(p[3]), int(p[4])))

low = {}
for (x, y, z) in cells:
    k = (x, y)
    if k not in low or z < low[k]:
        low[k] = z

def check(name, px, py, depth=5):
    zs = [low.get((px + dx, py + dy)) for dx in (0, 1, 2) for dy in (0, 1, 2)]
    flat = len(set(zs)) == 1 and None not in zs
    zb = zs[0] if flat else None
    print(f"{name}: pad x{px}..{px+2} y{py}..{py+2}  belly z {sorted(set(str(z) for z in zs))} "
          f"flat={flat}")
    if not flat:
        return
    # air below the belly inside the footprint, and one row forward for barrels
    blocked = [(x, y, z) for (x, y, z) in cells
               if px <= x <= px + 2 and py <= y <= py + 2 and zb - depth <= z < zb]
    fwd = [(x, y, z) for (x, y, z) in cells
           if px <= x <= px + 2 and py - 4 <= y <= py - 1 and zb - depth <= z <= zb]
    print(f"    hull cells below the pad within {depth} levels: {len(blocked)} "
          f"{sorted(blocked)[:6] if blocked else ''}")
    print(f"    hull cells in the barrel corridor forward:      {len(fwd)} "
          f"{sorted(fwd)[:6] if fwd else ''}")
    print(f"    mount would occupy z {zb-1} (pad) .. {zb-4} (sensor)")

for spec in sys.argv[2:]:
    name, px, py = spec.split(",")
    check(name, int(px), int(py))
