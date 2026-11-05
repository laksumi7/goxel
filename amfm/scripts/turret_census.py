#!/usr/bin/env python3
"""Turret census in the ship dump.

A turret is recognised from its mount, not from memory:
  * a flat 3x3 plate of one colour (the hardpoint pad)
  * a turntable cell on the pad centre
  * a body layer above it, optionally with a forward-facing barrel pair
"""
import collections
import sys

cells = {}
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    p = line.split()
    if len(p) < 7 or p[0] != "cell":
        continue
    cells[(int(p[2]), int(p[3]), int(p[4]))] = (int(p[5]), p[6])

print("cells:", len(cells), "colours:",
      collections.Counter(c for c, _ in cells.values()).most_common())

# --- connected components per (z, colour) plane --------------------------------
plane = collections.defaultdict(list)
for (x, y, z), (col, _) in cells.items():
    plane[(z, col)].append((x, y))

pads = []
for (z, col), pts in plane.items():
    pts = set(pts)
    seen = set()
    for p0 in pts:
        if p0 in seen:
            continue
        stack, comp = [p0], []
        seen.add(p0)
        while stack:
            x, y = stack.pop()
            comp.append((x, y))
            for nx, ny in ((x+1, y), (x-1, y), (x, y+1), (x, y-1)):
                if (nx, ny) in pts and (nx, ny) not in seen:
                    seen.add((nx, ny))
                    stack.append((nx, ny))
        xs = [a for a, _ in comp]; ys = [b for _, b in comp]
        w, h = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
        if len(comp) == 9 and w == 3 and h == 3:
            pads.append((min(xs), min(ys), z, col))

print("\n=== exact 3x3 single-colour plates ===")
for x, y, z, col in sorted(pads):
    print(f"  pad x{x}..{x+2} y{y}..{y+2} z{z} colour {col}")

# --- what sits on each plate --------------------------------------------------
print("\n=== turret signature on each plate ===")
turrets = []
for x, y, z, col in sorted(pads):
    cx, cy = x + 1, y + 1
    turn = cells.get((cx, cy, z + 1))
    body = [(cx + dx, cy + dy, z + 2) for dx in (-1, 0, 1) for dy in (-1, 0, 1)]
    body = [p for p in body if p in cells]
    # barrels: cells sticking out beyond the plate, same z as the body
    fwd = [(cx + dx, cy - dy, z + 2) for dx in (-1, 0, 1) for dy in (2, 3, 4)]
    fwd = [p for p in fwd if p in cells]
    tag = "TURRET" if turn and len(body) >= 3 else ("partial" if turn else "-")
    print(f"  {tag:8s} pad z{z} x{x}..{x+2} y{y}..{y+2}: turn={turn} "
          f"body={len(body)} forward={len(fwd)} {sorted(fwd) if fwd else ''}")
    if turn and len(body) >= 3:
        turrets.append((x, y, z, col, len(body), fwd))

print(f"\nturrets found: {len(turrets)}")
print("=== every cell above z of each plate, wider window (manual review) ===")
for x, y, z, col in sorted(pads):
    lo = [(p, cells[p]) for p in cells
          if x - 2 <= p[0] <= x + 4 and y - 2 <= p[1] <= y + 4 and z < p[2] <= z + 4]
    if lo:
        print(f"  pad x{x}..{x+2} y{y}..{y+2} z{z}: {len(lo)} cells above")

# --- barrel-pair signature anywhere (two cells, 2 apart in x, same z, facing -y)
print("\n=== forward barrel pairs (2 cells, dx=2, same z, nothing behind) ===")
seen = set()
for (x, y, z), (col, name) in sorted(cells.items()):
    if (x + 2, y, z) in cells and (x + 1, y, z) not in cells:
        key = (y, z)
        if key in seen:
            continue
        seen.add(key)
        print(f"  pair x{x} & x{x+2} y{y} z{z} colour {col} layers "
              f"{name}/{cells[(x+2, y, z)][1]}")
