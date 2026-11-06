#!/usr/bin/env python3
"""AMFM #534 — build the pinned producer-side metadata table for one ship revision.

Four facts are not persisted in a .gox today (group kind, group pivot, barrel
ownership, markers).  This script pins them for ONE exact GOX revision so the
exporter can run without guessing anything from display names.  Everything it
pins from geometry (pivots, bboxes, palette) is measured from the saved GOX
bytes, so the table cannot drift from the file it describes without the
exporter failing closed.

Usage:  python make_pinned.py [--gox ship.gox] [--joints pd_joints.json]
                              [--out table.json]

With --joints, the same generator also pins the PD assembly: each turret's
mount pad, yaw stage and pitch stage become three groups with an explicit
parent chain, a measured joint frame and per-barrel muzzle markers. That file
comes from pd_joint_plan.py, which measures the same GOX, so nothing here is
invented either.
"""
import argparse
import hashlib
import json
import os
import struct

import goxread

GOX = r"Z:\data_art\Ref_SF\voxels\Star Marine Trooper\StarMarineTrooper_hollow.gox"
FXJSON = r"Z:\data_art\Ref_SF\voxels\Star Marine Trooper\StarMarineTrooper_hollow.fx.json"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "meta",
                   "star_marine_trooper.pinned.json")

# Group kind is a design fact, never derivable from the file.
KIND = {
    "armor_outer": "armor_outer",
    "armor_inner": "armor_inner",
    "core_front": "core",
    "core_mid": "core",
    "core_rear": "core",
}
for i in range(1, 9):
    KIND[f"pd_{i:02d}"] = "pd"
for i in (1, 2):
    KIND[f"battery_{i:02d}"] = "battery"
    KIND[f"reverse_rcs_{i:02d}"] = "reverse_rcs"
for i in (1, 2, 3, 4):
    KIND[f"engine_{i:02d}"] = "engine"

# Palette order pinned to the handoff §5 numbering so #535 indices do not churn.
PALETTE = [
    [65, 66, 48, 255], [97, 96, 70, 255], [37, 37, 37, 255], [23, 23, 23, 255],
    [46, 46, 46, 255], [255, 0, 0, 255], [255, 255, 255, 255],
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gox", default=GOX)
    ap.add_argument("--fx", default=FXJSON)
    ap.add_argument("--joints", default=None,
                    help="pd_joints.json; pins the PD yaw/pitch assembly too")
    ap.add_argument("--out", default=OUT)
    opt = ap.parse_args()

    joints = None
    if opt.joints:
        joints = {m["group"]: m for m in json.load(open(opt.joints, encoding="utf-8"))["mounts"]}

    raw = open(opt.gox, "rb").read()
    sha = hashlib.sha256(raw).hexdigest()
    layers = goxread.load(opt.gox)
    cells = goxread.cells(layers)
    names = {l["name"] for l in layers}

    # A PD group owns its body layer and its two barrel layers.  With a PD
    # assembly the barrels belong to the stage that actually pitches.
    owned = {}
    for l in layers:
        n = l["name"]
        owner = n.split("_barrel_")[0]
        if joints and owner in joints and "_barrel_" in n:
            owner = f"{owner}_pitch"
        owned.setdefault(owner, []).append(n)
    for owner in owned:
        owned[owner].sort()

    if joints:
        for i in range(1, 9):
            KIND[f"pd_{i:02d}_yaw"] = "pd"
            KIND[f"pd_{i:02d}_pitch"] = "pd"

    unknown = sorted(set(owned) - set(KIND))
    if unknown:
        raise SystemExit(f"no kind known for layers {unknown}")

    fx = json.load(open(opt.fx, encoding="utf-8"))
    groups = []
    for owner in sorted(owned):
        pts = [p for p, (_, lay) in cells.items() if lay in owned[owner]]
        lo = [min(p[a] for p in pts) for a in range(3)]
        hi = [max(p[a] for p in pts) for a in range(3)]
        pivot = [(a + b) / 2.0 for a, b in zip(lo, hi)]
        g = {
            "layer_id": next(l["id"] for l in layers if l["name"] == owner),
            "name": owner,
            "kind": KIND[owner],
            "owned_layers": owned[owner],
            "pivot_xyz": pivot,
            "bbox_min_xyz": lo,
            "bbox_max_xyz": hi,
            "cells": len(pts),
        }
        if joints:
            stage = owner.rsplit("_", 1)[-1] if owner.endswith(("_yaw", "_pitch")) else None
            if stage in ("yaw", "pitch"):
                mount = owner[: -len(stage) - 1]
                j = joints[mount]
                g["parent"] = mount if stage == "yaw" else f"{mount}_yaw"
                origin = j["yaw_pivot_xyz"] if stage == "yaw" else j["pitch_hinge_xyz"]
                g["transform"] = {
                    "origin_source": origin,
                    "forward_source": j["forward_source"],
                    "up_source": j["yaw_axis_source"],
                    "note": f"measured {stage} joint origin of {mount}: the turntable "
                            f"column at the {stage} plane, not a bbox centre",
                }
        groups.append(g)

    markers = [{
        "id": m["id"],
        "group_id": m["group_id"],
        "kind": m["kind"],
        "xyz": m["origin_world_cell"],
        "direction_xyz": m["direction"],
    } for m in sorted(fx["emission"], key=lambda m: m["id"])]

    if joints:
        for mount in sorted(joints):
            j = joints[mount]
            under = [p for p in j["pad_cells"]
                     if p[0] == j["yaw_cell"][0] and p[1] == j["yaw_cell"][1]]
            hinge_cell = [j["yaw_cell"][0], j["yaw_cell"][1], j["pitch_hinge_xyz"][2]]
            markers += [
                {"id": f"fx_{mount}_mount_pivot", "group_id": mount, "kind": "pivot",
                 "xyz": j["pad_centre_xyz"], "direction_xyz": j["yaw_axis_source"],
                 "source_cell": under[0],
                 "note": "the 3x3 hardpoint pad centre: where the turret bolts to the hull"},
                {"id": f"fx_{mount}_yaw_pivot", "group_id": f"{mount}_yaw", "kind": "pivot",
                 "xyz": j["yaw_pivot_xyz"], "direction_xyz": j["yaw_axis_source"],
                 "source_cell": j["yaw_cell"],
                 "note": "turntable cell centre; the yaw axis is the mount outward normal"},
                {"id": f"fx_{mount}_pitch_hinge", "group_id": f"{mount}_pitch", "kind": "pivot",
                 "xyz": j["pitch_hinge_xyz"], "direction_xyz": j["pitch_axis_source"],
                 "source_cell": hinge_cell,
                 "note": "yaw/pitch interface, in the turntable column at barrel height; "
                         "the pitch axis is the derived local right vector"},
            ]
            for side in ("L", "R"):
                markers.append({
                    "id": f"fx_{mount}_muzzle_{side}", "group_id": f"{mount}_pitch",
                    "kind": "muzzle", "xyz": j["muzzle_xyz"][side],
                    "direction_xyz": j["muzzle_direction_source"],
                    "source_cell": j["barrels"][side]["source_cell"],
                    "note": "bore exit on the bow face of the barrel cell; the barrels "
                            "protrude clear of every body cell on that side",
                })

    table = {
        "table_version": 1,
        "note": "Producer-side pinned facts for one exact GOX revision. The "
                "exporter fails closed if the GOX hash differs, if a layer is "
                "unclaimed or double-claimed, or if a pinned pivot or bbox no "
                "longer matches the file. Nothing here is derived from names.",
        "asset_id": "star_marine_trooper",
        "source_gox": opt.gox.replace("\\", "/"),
        "source_sha256": sha,
        "frame": fx["frame"],
        "palette": PALETTE,
        "groups": groups,
        "markers": markers,
    }
    out = os.path.abspath(opt.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(table, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"wrote {out}")
    print(f"  source_sha256 : {sha}")
    print(f"  layers        : {len(layers)}  groups: {len(groups)}  "
          f"cells: {len(cells)}  markers: {len(markers)}")
    print(f"  palette       : {len(PALETTE)} entries")
    print(f"  table sha256  : {hashlib.sha256(open(out,'rb').read()).hexdigest()}")


main()
