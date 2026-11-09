#!/usr/bin/env python3
"""AMFM #534 P0 evidence check.

    python compare_dumps.py before.txt after.txt --region x0,y0,z0,x1,y1,z1 \
            (--dx N --dy N --dz N | --rot z) [--undo undo.txt]

A group move takes three things by the same transform: the cells of every layer
inside the selection, the whole of any generated surface the selection reached
(a shape is one object, so all of its cells move even if the box only covered
one of them), and the region that surface is built in.  Everything outside the
selection has to be untouched.
"""
import sys


def load(path):
    layers, aabbs = {}, {}
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split()
        if not p:
            continue
        if p[0] == "cell":
            layers.setdefault(p[6], set()).add((int(p[2]), int(p[3]), int(p[4])))
        elif p[0] == "#" and len(p) > 2 and p[1].startswith("layer["):
            name = line.split("'")[1]
            if " aabb=" in line:
                a = line.split("aabb=")[1].split()[0]
                lo, hi = a.split("..")
                aabbs[name] = (
                    (tuple(int(v) for v in lo.split(",")), tuple(int(v) for v in hi.split(","))),
                    int(line.split("rev=")[1].split()[0]),
                    int(line.split("stale=")[1].split()[0]),
                )
    return layers, aabbs


def main():
    a = sys.argv[1:]
    before, after = a[0], a[1]
    args = a[2:]

    def opt(name, cast=str):
        return cast(args[args.index(name) + 1]) if name in args else None

    undo = opt("--undo")
    rot = opt("--rot")
    reopen = "--reopen" in args
    dx, dy, dz = (opt("--dx", int) or 0, opt("--dy", int) or 0, opt("--dz", int) or 0)
    rx0, ry0, rz0, rx1, ry1, rz1 = (int(v) for v in opt("--region").split(","))

    def inside(c):
        return (rx0 <= c[0] <= rx1 and ry0 <= c[1] <= ry1 and rz0 <= c[2] <= rz1)

    if rot == "z":
        tf = lambda c: (-c[1], c[0], c[2])
    elif rot == "x":
        tf = lambda c: (c[0], -c[2], c[1])
    elif rot == "y":
        tf = lambda c: (c[2], c[1], -c[0])
    else:
        tf = lambda c: (c[0] + dx, c[1] + dy, c[2] + dz)
    what = f"rot90-{rot}" if rot else f"offset {dx},{dy},{dz}"

    b, ba = load(before)
    ac, aa = load(after)
    fails = []
    nb, na = sum(len(v) for v in b.values()), sum(len(v) for v in ac.values())
    print(f"[{what}] cells {nb} -> {na}")
    if nb != na:
        fails.append("cell count changed")

    for name in sorted(set(b) | set(ac)):
        cb, ca = b.get(name, set()), ac.get(name, set())
        if not cb and not ca:
            continue
        touched = any(inside(c) for c in cb)
        whole = touched and name in ba          # a grabbed shape moves as one
        exp = {tf(c) if (inside(c) or whole) else c for c in cb}
        if exp != ca:
            fails.append(f"layer '{name}': {len(exp ^ ca)} cell(s) not where the "
                         f"transform puts them "
                         f"(left {sorted(exp - ca)[:3]} extra {sorted(ca - exp)[:3]})")
        if name in ba:
            (blo, bhi), brev, bstale = ba[name]
            if name not in aa:
                fails.append(f"layer '{name}' lost its shape")
                continue
            (alo, ahi), arev, astale = aa[name]
            pts = [tf((x, y, z)) for x in (blo[0], bhi[0])
                   for y in (blo[1], bhi[1]) for z in (blo[2], bhi[2])]
            e_lo = tuple(min(p[k] for p in pts) for k in range(3))
            e_hi = tuple(max(p[k] for p in pts) for k in range(3))
            if touched or whole:
                print(f"  shape '{name}': aabb {blo}..{bhi} -> {alo}..{ahi} "
                      f"(expected {e_lo}..{e_hi}) stale {bstale}->{astale}")
                if (alo, ahi) != (e_lo, e_hi):
                    fails.append(f"shape '{name}' region did not follow its cells")
            elif (alo, ahi) != (blo, bhi):
                fails.append(f"shape '{name}' moved although nothing of it was selected")
            if astale:
                fails.append(f"shape '{name}' became stale after the move")
            if touched and not reopen and arev == brev:
                fails.append(f"shape '{name}' revision did not change (mesh would not rebuild)")

    if undo:
        u, ua = load(undo)
        if u != b:
            fails.append("undo did not restore the original cells")
        if ua != ba:
            fails.append("undo did not restore the original shape regions")
        else:
            print("undo: cells and shape regions restored exactly")

    print("RESULT:", "PASS" if not fails else "FAIL")
    for f in fails:
        print("  -", f)
    sys.exit(0 if not fails else 1)


main()
