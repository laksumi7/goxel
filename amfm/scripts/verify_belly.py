#!/usr/bin/env python3
"""Check the three belly mounts survived a save/reopen: every cell present with
the right colour, and the six barrel cells living on their own shape layers."""
import collections
import sys

PAD = 46
BODY = 37
SENSOR = 255
MOUNTS = [("bow", -1, -31, -8), ("port_wing", -17, 13, -9), ("stbd_wing", 14, 13, -9)]

cells = {}
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    p = line.split()
    if len(p) < 7 or p[0] != "cell":
        continue
    cells[(int(p[2]), int(p[3]), int(p[4]))] = (int(p[5]), p[6])

print("total cells:", len(cells))
print("shape layers:", sum(1 for _, n in cells.values() if "PD_belly" in n or "Cylinder" in n))
bad = 0
for name, px, py, zb in MOUNTS:
    pz, bz = zb - 1, zb - 3
    want = {}
    for dx in range(3):
        for dy in range(3):
            want[(px + dx, py + dy, pz)] = PAD
    want[(px + 1, py + 1, pz - 1)] = PAD
    want[(px + 1, py + 1, bz)] = BODY
    for dx in range(3):
        for dy in (0, -1, -2):
            want[(px + dx, py + dy, bz)] = BODY
    want[(px + 1, py, pz - 3)] = SENSOR
    want[(px, py - 3, bz)] = BODY
    want[(px + 2, py - 3, bz)] = BODY
    miss = [k for k in want if k not in cells]
    wrong = [(k, cells[k]) for k in want if k in cells and cells[k][0] != want[k]]
    onshape = [(k, cells[k][1]) for k in ((px, py - 3, bz), (px + 2, py - 3, bz))
               if k in cells and "PD_belly" in cells[k][1]]
    print(f"{name:10s} cells={len(want)} missing={len(miss)} wrong_colour={len(wrong)} "
          f"barrels_on_shape_layers={len(onshape)} {onshape}")
    bad += len(miss) + len(wrong)
    if miss:
        print("   missing:", miss[:8])
    if wrong:
        print("   wrong:", wrong[:4])
print("RESULT:", "PASS" if bad == 0 else f"FAIL ({bad})")
