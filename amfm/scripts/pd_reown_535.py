#!/usr/bin/env python3
"""AMFM #535 Phase A — re-own the PD turret cells for the Owner's clarified mechanics.

The Owner's correction (AMFM #534 #6688000129, #535 #6687983223, 2026-11-07):

  * the pad stays fixed;
  * the yaw stage carries the horizontal connecting housing and the sensor and
    turns the whole upper assembly in azimuth;
  * ONLY the two long L/R side-arm + barrel assemblies elevate, together and
    symmetrically, about their own attachment hinges;
  * the crossbar must not rise or tip with the arms.

The promoted #534 package puts the whole 9-10 cell plate plus the sensor in
`pd_NN_pitch`, so it encodes the opposite motion.  This script re-measures the
mounts from the saved GOX cells and emits the regroup spec that moves the
connecting housing and the sensor onto the yaw layer, leaving the pitch layer
holding exactly the two arm subassemblies.

Nothing is taken from display names: the columns are found from the turntable
cell and the body plate geometry, and every claim is asserted or the script dies.

Read-only on the file it is given.  It writes a spec; Goxel applies it to a copy.

  python pd_reown_535.py --gox <candidate.gox> --out <dir>
"""
import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, r"C:\projects_tools\goxel-lab\goxel\amfm\scripts")
import goxread  # noqa: E402

PAD_GREY = (46, 46, 46)
BODY = (37, 37, 37)
WHITE = (255, 255, 255)
BOW = (0, -1, 0)          # hull forward, measured from the battery and engine markers


def fail(msg):
    raise SystemExit(f"REFUSED: {msg}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gox", required=True)
    ap.add_argument("--out", required=True)
    opt = ap.parse_args()
    os.makedirs(opt.out, exist_ok=True)

    raw = open(opt.gox, "rb").read()
    sha = hashlib.sha256(raw).hexdigest()
    layers = goxread.load(opt.gox)
    by_name = {l["name"]: l for l in layers}

    spec = []
    mounts = []
    audit = []

    for i in range(1, 9):
        name = f"pd_{i:02d}"
        pad_l, yaw_l, pitch_l = by_name[name], by_name[f"{name}_yaw"], by_name[f"{name}_pitch"]

        yaw_pts = sorted(yaw_l["cells"])
        if len(yaw_pts) != 1:
            fail(f"{name}: the yaw layer holds {len(yaw_pts)} cells, expected the lone turntable cell")
        yaw_cell = yaw_pts[0]
        x_c, y_c = yaw_cell[0], yaw_cell[1]

        pts = sorted(pitch_l["cells"])
        dark = [p for p in pts if pitch_l["cells"][p][:3] == BODY]
        white = [p for p in pts if pitch_l["cells"][p][:3] == WHITE]
        other = [p for p in pts if p not in dark and p not in white]
        if other:
            fail(f"{name}: pitch layer holds cells in unexpected colours: {other}")
        if len(white) != 1:
            fail(f"{name}: expected exactly one sensor cell, found {white}")
        zs = {p[2] for p in dark}
        if len(zs) != 1:
            fail(f"{name}: the body plate is not on one plane: {sorted(zs)}")
        z_body = zs.pop()

        # The turntable stands under the middle column, so the column whose x is
        # the turntable's x is the connecting housing; the two columns beside it
        # are the arms.
        cols = {}
        for p in dark:
            cols.setdefault(p[0], []).append(p)
        if set(cols) != {x_c - 1, x_c, x_c + 1}:
            fail(f"{name}: body columns are {sorted(cols)}, expected {x_c-1},{x_c},{x_c+1}")
        centre = sorted(cols[x_c])
        arm_l = sorted(cols[x_c - 1])
        arm_r = sorted(cols[x_c + 1])
        if [p[1] for p in arm_l] != [p[1] for p in arm_r]:
            fail(f"{name}: the arms are not mirror images: L {[p[1] for p in arm_l]} "
                 f"R {[p[1] for p in arm_r]}")
        if (x_c, y_c, z_body) not in centre:
            fail(f"{name}: no body cell sits directly above the turntable cell")

        # Outward is the way the plate sits from the turntable; it fixes which
        # way is up and therefore the derived pitch axis (forward x up).
        n = z_body - yaw_cell[2]
        if abs(n) != 1:
            fail(f"{name}: body plate {z_body} is not one cell out from the turntable {yaw_cell[2]}")
        outward = (0, 0, 1 if n > 0 else -1)
        pitch_axis = (outward[1] * BOW[2] - outward[2] * BOW[1],
                      outward[2] * BOW[0] - outward[0] * BOW[2],
                      outward[0] * BOW[1] - outward[1] * BOW[0])
        if pitch_axis not in ((1, 0, 0), (-1, 0, 0)):
            fail(f"{name}: derived pitch axis {pitch_axis} is not along x")

        # The arms attach to the housing over the y range they share with it.
        # Their rearmost cell (the one furthest from the bow, since the bore is
        # -Y) is the mounting cell, and both arms share it, so the two hinges are
        # two points of one axle parallel to x.
        y_h = max(p[1] for p in arm_l)
        if y_h >= y_c:
            fail(f"{name}: arm mounting row {y_h} is not forward of the turntable row {y_c}")
        for p in arm_l + arm_r:
            if p[1] > y_h:
                fail(f"{name}: arm cell {p} is behind the derived hinge row {y_h}")

        barrels = {}
        for side, col in (("L", x_c - 1), ("R", x_c + 1)):
            bl = by_name[f"{name}_barrel_{side}"]
            bp = sorted(bl["cells"])
            if len(bp) != 1:
                fail(f"{name}_barrel_{side}: expected one source cell, found {bp}")
            b = bp[0]
            if b[0] != col:
                fail(f"{name}: barrel_{side} sits at x={b[0]}, not in the {side} arm column {col}")
            if b[1] >= min(p[1] for p in (arm_l if side == "L" else arm_r)):
                fail(f"{name}: barrel_{side} at {b} is not clear of its arm")
            if b[2] != z_body:
                fail(f"{name}: barrel_{side} at {b} is not on the body plate plane")
            s = bl["slope"]
            barrels[side] = {
                "source_cell": list(b),
                "axis": goxread.DIR_NAME[s["dir"]],
                "rise": s["rise"],
                "closed": bool(s["closed"]),
                "anchor": goxread.ANCHOR_NAME[s["anchor"]],
            }

        hinge = [float(x_c), float(y_h), float(z_body)]
        hinge_l = [x_c - 1, y_h, z_body]
        hinge_r = [x_c + 1, y_h, z_body]

        # --- the regroup itself: the housing and the sensor go to the yaw layer
        move = centre + white
        for p in sorted(move):
            spec.append(f"M {name}_pitch {name}_yaw {p[0]} {p[1]} {p[2]}")

        mounts.append({
            "group": name,
            "mount": "dorsal" if outward[2] > 0 else "underside",
            "pad_centre_xyz": [sum(p[a] for p in sorted(pad_l["cells"])) / 9.0 for a in range(3)],
            "pad_cells": [list(p) for p in sorted(pad_l["cells"])],
            "yaw_cell": list(yaw_cell),
            "yaw_pivot_xyz": [float(yaw_cell[0]), float(yaw_cell[1]), float(yaw_cell[2])],
            "yaw_axis_source": list(outward),
            "yaw_carries_cells": [list(p) for p in sorted(centre)] + [list(white[0])],
            "pitch_hinge_xyz": hinge,
            "pitch_hinge_source_cell": hinge_l,
            "pitch_hinge_note": (
                "the two arm mounting cells share one axle parallel to the derived pitch axis, "
                f"through {tuple(hinge)}; the marker names the left arm cell {tuple(hinge_l)} "
                "because a marker's source cell must belong to its own group, and the pitch "
                "stage owns the arms, not the housing"),
            "pitch_axis_source": list(pitch_axis),
            "forward_source": list(BOW),
            "arm_hinge_L_xyz": hinge_l,
            "arm_hinge_R_xyz": hinge_r,
            "arm_L_cells": [list(p) for p in arm_l] + [barrels["L"]["source_cell"]],
            "arm_R_cells": [list(p) for p in arm_r] + [barrels["R"]["source_cell"]],
            "sensor_cell": list(white[0]),
            "barrels": barrels,
            "muzzle_xyz": {s: [float(barrels[s]["source_cell"][0]),
                               barrels[s]["source_cell"][1] - 0.5,
                               float(barrels[s]["source_cell"][2])] for s in barrels},
            "muzzle_direction_source": list(BOW),
        })

        audit.append({
            "name": name, "yaw_cell": yaw_cell, "z_body": z_body, "y_h": y_h,
            "centre": sorted(centre), "arm_l": sorted(arm_l), "arm_r": sorted(arm_r),
            "sensor": white[0], "barrels": {s: barrels[s]["source_cell"] for s in barrels},
            "hinge": hinge, "outward": outward, "axis": pitch_axis,
            "moved_to_yaw": sorted(move), "stays_on_pitch": sorted(arm_l + arm_r),
        })

    with open(os.path.join(opt.out, "reown.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# AMFM #535 Phase A — the connecting housing and the sensor leave the pitch\n")
        f.write("# layer and join the yaw stage; the pitch layer keeps only the two arms.\n")
        for line in spec:
            f.write(line + "\n")

    data = {
        "source_gox": opt.gox.replace("\\", "/"),
        "source_sha256": sha,
        "note": ("Owner mechanics correction 2026-11-07: yaw carries the connecting housing and "
                 "the sensor, pitch carries only the two arm/barrel subassemblies. Measured from "
                 "the saved GOX cells; bow = -Y, up = +Z, 1 voxel = 1 m. The two arm hinges are "
                 "collinear along the derived pitch axis, so one axle drives both arms together "
                 "and symmetrically; v1 needs no second pitch child."),
        "mounts": mounts,
    }
    with open(os.path.join(opt.out, "pd535_joints.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1)
        f.write("\n")

    with open(os.path.join(opt.out, "pd535_reown_audit.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"source gox {opt.gox}\nsource sha256 {sha}\n\n")
        f.write("Per mount: the body plate seen from above (x across, y down the page, bow is -Y).\n")
        f.write("  H connecting housing -> moves to the yaw stage\n")
        f.write("  S sensor             -> moves to the yaw stage\n")
        f.write("  a arm cell           -> stays on the pitch stage\n")
        f.write("  b barrel cell        -> stays on the pitch stage (its own shape layer)\n")
        f.write("  ^ the derived hinge row, on the arm mounting cells; the axle runs along x\n\n")
        for a in audit:
            ys = sorted({p[1] for p in a["centre"] + a["arm_l"] + a["arm_r"]
                         + [tuple(a["barrels"]["L"]), tuple(a["barrels"]["R"])]})
            xs = sorted({p[0] for p in a["centre"] + a["arm_l"] + a["arm_r"]})
            f.write(f"== {a['name']}  turntable {tuple(a['yaw_cell'])}  plate z={a['z_body']}  "
                    f"hinge {tuple(a['hinge'])}  axis {tuple(a['axis'])}\n")
            for y in ys:
                row = []
                for x in xs:
                    p = (x, y, a["z_body"])
                    if tuple(a["barrels"]["L"]) == p or tuple(a["barrels"]["R"]) == p:
                        row.append("b")
                    elif p in a["arm_l"] or p in a["arm_r"]:
                        row.append("a")
                    elif p in a["centre"]:
                        row.append("H")
                    else:
                        row.append(".")
                mark = "  ^" if y == a["y_h"] else "   "
                f.write(f"   y={y:>4} |{''.join(row)}|{mark}\n")
            f.write(f"   sensor {tuple(a['sensor'])} (above the housing)\n")
            f.write(f"   -> yaw gains {len(a['moved_to_yaw'])} cells, pitch keeps "
                    f"{len(a['stays_on_pitch'])} + 2 barrel cells\n\n")

    print(f"source sha256 {sha}")
    print(f"mounts {len(mounts)}  regroup lines {len(spec)}")
    for m in mounts:
        print(f"  {m['group']:6} {m['mount']:9} hinge {str(m['pitch_hinge_xyz']):>17} "
              f"axis {str(m['pitch_axis_source']):>10} yaw+{len(m['yaw_carries_cells'])} "
              f"pitch {len(m['arm_L_cells']) + len(m['arm_R_cells'])}")
    print(f"wrote {opt.out}/reown.txt, pd535_joints.json, pd535_reown_audit.txt")


main()
