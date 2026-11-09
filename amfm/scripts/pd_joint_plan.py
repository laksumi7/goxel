#!/usr/bin/env python3
"""AMFM #534 editor Worker — measure the PD joints from the saved GOX and write
the regroup spec that gives yaw and pitch their own editable layers.

Read-only on the Owner's file. Everything here is measured from cells; nothing is
inferred from layer names or from the game's provisional rig.
"""
import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import goxread  # noqa: E402

SRC = os.environ.get("AMFM_SRC_GOX", "ship.gox")
OUT = os.environ.get("AMFM_OUT_DIR", "pd_joint_plan_out")
if len(sys.argv) > 1:                     # --gox/--out: re-normalize a copy, same steps
    _ap = argparse.ArgumentParser()
    _ap.add_argument("--gox", default=SRC)
    _ap.add_argument("--out", default=OUT)
    _o = _ap.parse_args()
    SRC, OUT = _o.gox, _o.out
PAD_GREY = (46, 46, 46)
BODY = (37, 37, 37)
WHITE = (255, 255, 255)
BOW = (0, -1, 0)          # hull forward, measured from the battery and engine markers
os.makedirs(OUT, exist_ok=True)

layers = goxread.load(SRC)
by_name = {l["name"]: l for l in layers}
report = []
spec = []

for i in range(1, 9):
    name = f"pd_{i:02d}"
    body = by_name[name]
    pts = sorted(body["cells"])
    grey = [p for p in pts if body["cells"][p][:3] == PAD_GREY]
    dark = [p for p in pts if body["cells"][p][:3] == BODY]
    white = [p for p in pts if body["cells"][p][:3] == WHITE]
    assert len(white) == 1, (name, white)

    # The 3x3 hardpoint pad is the grey slab; the turntable is the lone grey cell
    # on its own plane.
    by_z = {}
    for p in grey:
        by_z.setdefault(p[2], []).append(p)
    zs = sorted(by_z, key=lambda z: len(by_z[z]))
    yaw_cell = by_z[zs[0]][0]
    pad = [p for z in zs[1:] for p in by_z[z]]
    pad_z = pad[0][2]
    assert len(pad) == 9 and len(by_z[pad_z]) == 9, (name, pad)
    pad_c = [sum(p[a] for p in pad) / 9.0 for a in range(3)]

    # Outward = the direction the body sits from the turntable (up on the deck,
    # down under the hull).
    body_z = dark[0][2]
    n = body_z - yaw_cell[2]
    assert abs(n) == 1, (name, body_z, yaw_cell)
    outward = (0, 0, 1 if n > 0 else -1)

    # The pitch hinge is the YawBase/PitchAssembly interface: the column of the
    # turntable, at the height of the body plate. Same rule the Owner's own
    # pd_turret.gox conventions record states.
    hinge = (float(yaw_cell[0]), float(yaw_cell[1]), float(body_z))
    # Right-handed local frame: forward = the bore, up = outward, so the pitch
    # axis is forward x up... derived, not chosen.
    pitch_axis = (outward[1] * BOW[2] - outward[2] * BOW[1],
                  outward[2] * BOW[0] - outward[0] * BOW[2],
                  outward[0] * BOW[1] - outward[1] * BOW[0])
    assert pitch_axis in ((1, 0, 0), (-1, 0, 0)), (name, pitch_axis)

    barrels = {}
    for side in ("L", "R"):
        bl = by_name[f"{name}_barrel_{side}"]
        p = sorted(bl["cells"])[0]
        s = bl["slope"]
        barrels[side] = {
            "source_cell": list(p),
            "axis": goxread.DIR_NAME[s["dir"]],
            "rise": s["rise"],
            "closed": bool(s["closed"]),
            "anchor": goxread.ANCHOR_NAME[s["anchor"]],
        }
        # The bore points away from the body it is mounted on: the barrels sit
        # forward of every body cell, so the muzzle is the -Y face of the cell.
        assert p[1] < min(q[1] for q in dark), (name, side, p)

    # The game's provisional rig, for the conflict report only. The published
    # group pivot is the bbox centre of everything the group owns, barrels
    # included, which is what the exporter pins today.
    everything = pts + [tuple(barrels[s]["source_cell"]) for s in barrels]
    lo = [min(p[a] for p in everything) for a in range(3)]
    hi = [max(p[a] for p in everything) for a in range(3)]
    old_pivot = [(a + b) / 2.0 for a, b in zip(lo, hi)]
    bmid = [(barrels["L"]["source_cell"][a] + barrels["R"]["source_cell"][a]) / 2.0
            for a in range(3)]
    prov = [round(b - o, 3) for b, o in zip(bmid, old_pivot)]

    rec = {
        "group": name,
        "mount": "dorsal" if outward[2] > 0 else "underside",
        "pad_centre_xyz": pad_c,
        "pad_cells": [list(p) for p in sorted(pad)],
        "yaw_cell": list(yaw_cell),
        "yaw_pivot_xyz": [float(yaw_cell[0]), float(yaw_cell[1]), float(yaw_cell[2])],
        "yaw_axis_source": list(outward),
        "pitch_hinge_xyz": list(hinge),
        "pitch_axis_source": list(pitch_axis),
        "forward_source": list(BOW),
        "body_cells": [list(p) for p in sorted(dark)],
        "sensor_cell": list(white[0]),
        "barrels": barrels,
        "muzzle_xyz": {s: [float(barrels[s]["source_cell"][0]),
                           barrels[s]["source_cell"][1] - 0.5,
                           float(barrels[s]["source_cell"][2])] for s in barrels},
        "muzzle_direction_source": list(BOW),
        "provisional_group_pivot_xyz": old_pivot,
        "provisional_barrel_mid_offset_m": prov,
        "barrel_mid_xyz": bmid,
        "authored_hinge_minus_published_pivot_m": [round(h - o, 3)
                                                   for h, o in zip(hinge, old_pivot)],
    }
    report.append(rec)

    spec.append(f"N {name}_yaw")
    spec.append(f"N {name}_pitch")
    spec.append(f"M {name} {name}_yaw {yaw_cell[0]} {yaw_cell[1]} {yaw_cell[2]}")
    for p in sorted(dark) + sorted(white):
        spec.append(f"M {name} {name}_pitch {p[0]} {p[1]} {p[2]}")

with open(os.path.join(OUT, "regroup.txt"), "w", encoding="utf-8", newline="\n") as f:
    f.write("# AMFM #534 — give every PD mount its own yaw and pitch layers.\n")
    for line in spec:
        f.write(line + "\n")

data = {
    "source_gox": SRC.replace("\\", "/"),
    "source_sha256": hashlib.sha256(open(SRC, "rb").read()).hexdigest(),
    "note": "measured from the saved GOX cells; bow = -Y, up = +Z, 1 voxel = 1 m",
    "mounts": report,
}
with open(os.path.join(OUT, "pd_joints.json"), "w", encoding="utf-8", newline="\n") as f:
    json.dump(data, f, indent=1)
    f.write("\n")

print(f"source sha256 {data['source_sha256']}")
print(f"{'mount':7} {'side':10} {'pad centre':>18} {'yaw pivot':>16} "
      f"{'pitch hinge':>16} {'yaw axis':>10} {'pitch axis':>11}")
for r in report:
    print(f"{r['group']:7} {r['mount']:10} {str(r['pad_centre_xyz']):>18} "
          f"{str(r['yaw_pivot_xyz']):>16} {str(r['pitch_hinge_xyz']):>16} "
          f"{str(r['yaw_axis_source']):>10} {str(r['pitch_axis_source']):>11}")
print("\nbarrel / muzzle (source, bore exit at the -Y face of the barrel cell):")
for r in report:
    for s in ("L", "R"):
        print(f"  {r['group']}_{s} cell={r['barrels'][s]['source_cell']} "
              f"rise={r['barrels'][s]['rise']} muzzle={r['muzzle_xyz'][s]} "
              f"dir={r['muzzle_direction_source']}")
print("\nconflict check — the game pitches about the barrel-pair midpoint, which is")
print("where the published group pivot puts its rig; the authored hinge is elsewhere:")
for r in report:
    off = r["provisional_barrel_mid_offset_m"]
    d = r["authored_hinge_minus_published_pivot_m"]
    print(f"  {r['group']}: published pivot {r['provisional_group_pivot_xyz']} "
          f"barrel mid {r['barrel_mid_xyz']} (offset {off}, |{sum(a * a for a in off) ** 0.5:.2f}| m) "
          f"| authored hinge {r['pitch_hinge_xyz']} = pivot {d} "
          f"= hardpoint {r['pad_centre_xyz']} + {r['yaw_axis_source'][2] * 2:+d} outward")
print(f"\nregroup spec lines: {len(spec)}")
