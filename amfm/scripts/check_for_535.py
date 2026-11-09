#!/usr/bin/env python3
"""AMFM #534 — pre-flight a published amfm.ship v1 file against the rules the
#535 consumer actually enforces (src/asset/canonical_ship_v1.gd, read 2026-11-06).

This is a producer-side check. It proves the file would be accepted by the
loader's validation; it is NOT a claim that the game imported it.

Usage: python check_for_535.py <file.amfmship.json>
"""
import json
import re
import sys

FRAME_MATRIX = [[-1, 0, 0], [0, 0, 1], [0, 1, 0]]
ROLE_ALIASES = {
    "armor_outer": "armor_outer", "armor_inner": "armor_inner", "core": "core",
    "pd": "point_defense", "point_defense": "point_defense",
    "battery": "main_battery", "main_battery": "main_battery",
    "engine": "main_engine", "main_engine": "main_engine",
    "reverse_rcs": "reverse_rcs", "structure": "structure", "decorative": "decorative",
}
ROLE_INFO = {
    "armor_outer": ("hull", "armor_outer"), "armor_inner": ("hull", "armor_inner"),
    "core": ("core", "core"), "point_defense": ("equipment", "equipment"),
    "main_battery": ("equipment", "equipment"), "main_engine": ("equipment", "equipment"),
    "reverse_rcs": ("equipment", "equipment"), "structure": ("structure", "structure"),
    "decorative": ("decorative", "decorative"),
}
GROUP_CLASS_ALLOWED = {
    "hull": ["armor_outer", "armor_inner", "structure"], "core": ["core"],
    "equipment": ["equipment"], "structure": ["structure"], "decorative": ["decorative"],
}
MARKER_KINDS = ["muzzle", "nozzle", "exhaust", "pivot", "sensor", "attachment", "forward", "up"]
SURFACE_KINDS = ["cylinder", "slope", "bevel", "sphere"]
MEMBER_ID = re.compile(r"^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+){0,3}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
ASSET_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")

errors, warnings = [], []


def err(code, path, msg):
    errors.append(f"{code} {path}: {msg}")


def warn(code, path, msg):
    warnings.append(f"{code} {path}: {msg}")


def det3(m):
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))


def main(path):
    d = json.load(open(path, encoding="utf-8"))
    if d.get("format") != "amfm.ship":
        err("E_FORMAT", "$.format", str(d.get("format")))
    if d.get("schema_version") != 1:
        err("E_SCHEMA", "$.schema_version", str(d.get("schema_version")))
    if not ASSET_ID.match(str(d.get("asset_id", ""))):
        err("E_ASSET_ID", "$.asset_id", str(d.get("asset_id")))
    if not SHA256.match(str(d.get("source_sha256", ""))):
        err("E_SOURCE_HASH", "$.source_sha256", "not 64 lowercase hex")

    f = d["frame"]
    if f["source_to_godot_basis"] != FRAME_MATRIX or det3(f["source_to_godot_basis"]) != 1:
        err("E_FRAME_MATRIX", "$.frame", "must be the accepted basis with determinant +1")
    if f["meters_per_voxel"] <= 0:
        err("E_FRAME_SCALE", "$.frame.meters_per_voxel", "must be positive")
    pivot = f["source_pivot_xyz"]

    palette = d["palette"]
    cells = d["cells"]
    src = {tuple(c["xyz"]) for c in cells}
    lo = [min(c["xyz"][a] for c in cells) for a in range(3)]
    hi = [max(c["xyz"][a] for c in cells) for a in range(3)]

    def in_bounds(p, pad=0.0):
        return all(lo[a] - pad <= p[a] <= hi[a] + pad for a in range(3))

    if not in_bounds(pivot):
        err("E_FRAME_ORIGIN", "$.frame.source_pivot_xyz", "pivot outside occupied bounds")

    groups, names = {}, set()
    for g in d["groups"]:
        gid = str(g["id"])
        if gid in groups:
            err("E_GROUP_DUPLICATE", "$.groups", f"duplicate id {gid}")
            continue
        role = ROLE_ALIASES.get(g.get("kind") or g.get("role") or "")
        if not role:
            err("E_GROUP_ROLE", f"$.groups[{gid}].kind", f"unknown kind {g.get('kind')}")
            continue
        kind, cls = ROLE_INFO[role]
        if cls not in GROUP_CLASS_ALLOWED[kind]:
            err("E_GROUP_CLASS", f"$.groups[{gid}]", f"class {cls} invalid for kind {kind}")
        if gid in names:
            err("E_GROUP_DUPLICATE", f"$.groups[{gid}].name", "duplicate name")
        names.add(gid)
        if g.get("transform"):
            t = g["transform"]
            fwd, up = t["forward_source"], t["up_source"]
            if sum(c * c for c in fwd) < 1e-9 or sum(c * c for c in up) < 1e-9:
                err("E_GROUP_TRANSFORM", f"$.groups[{gid}].transform", "zero forward or up")
            elif abs(sum(a * b for a, b in zip(fwd, up))) > 1e-3:
                err("E_GROUP_TRANSFORM", f"$.groups[{gid}].transform", "forward and up not orthogonal")
            origin = t["origin_source"]
        elif g.get("pivot_xyz") is not None:
            origin = g["pivot_xyz"]
        else:
            err("E_GROUP_TRANSFORM", f"$.groups[{gid}]", "transform or pivot_xyz required")
            origin = None
        if origin is not None and not in_bounds(origin):
            err("E_GROUP_TRANSFORM", f"$.groups[{gid}]", f"origin {origin} outside bounds")
        groups[gid] = {"role": role, "name": g.get("name"), "cells": 0,
                       "parent": g.get("parent_id"), "origin": origin}

    for c in cells:
        t = tuple(c["xyz"])
        if t in ():
            err("E_CELL_DUPLICATE", "$.cells", str(t))
        gid = str(c["group_id"])
        if gid not in groups:
            err("E_CELL_GROUP", "$.cells", f"unknown group {gid}")
            continue
        if not 0 <= c["palette_index"] < len(palette):
            err("E_CELL_COLOR", "$.cells", f"palette_index {c['palette_index']}")
        groups[gid]["cells"] += 1
    if len(src) != len(cells):
        err("E_CELL_DUPLICATE", "$.cells", "duplicate coordinates")
    for gid, g in groups.items():
        if g["cells"] == 0:
            err("E_GROUP_EMPTY", f"$.groups[{gid}]", "group has no owned cells")
        if g["parent"] is not None and str(g["parent"]) not in groups:
            err("E_GROUP_PARENT", f"$.groups[{gid}]", f"parent {g['parent']} does not exist")
        if str(g["parent"]) == gid:
            err("E_GROUP_PARENT", f"$.groups[{gid}]", "is its own parent")

    for s in d["surfaces"]:
        sid, gid = s["id"], str(s["group_id"])
        if not MEMBER_ID.match(sid):
            err("E_MODIFIER_ID", f"$.surfaces[{sid}]", "id not a stable member id")
        if gid not in groups:
            err("E_MODIFIER_GROUP", sid, f"unknown group {gid}")
        if s["kind"] not in SURFACE_KINDS:
            err("E_MODIFIER_KIND", sid, f"unknown kind {s['kind']}")
        rmin, rmax = s["region_min_xyz"], s["region_max_xyz"]
        for p in s["source_cell_xyz"]:
            if tuple(p) not in src:
                err("E_MODIFIER_SOURCE_MISSING", sid, f"source cell {p} not occupied")
            elif str(next(c["group_id"] for c in cells if tuple(c["xyz"]) == tuple(p))) != gid:
                err("E_MODIFIER_SOURCE_GROUP", sid, f"source cell {p} belongs to another group")
            if not all(rmin[a] <= p[a] <= rmax[a] for a in range(3)):
                err("E_MODIFIER_REGION", sid, f"source cell {p} outside region")
        pr = s["parameters"]
        if s["kind"] == "cylinder":
            if pr["rise"] <= 0:
                err("E_MODIFIER_PARAMS", sid, "rise must be positive")
            length = max(rmax[a] - rmin[a] + 1 for a in range(3))
            if length <= 0:
                err("E_MODIFIER_PARAMS", sid, "length must be positive")

    markers = set()
    for m in d["markers"]:
        mid, gid = m["id"], str(m["group_id"])
        if not MEMBER_ID.match(mid):
            err("E_MARKER_ID", mid, "id not a stable member id")
        if mid in markers:
            err("E_MARKER_DUPLICATE", mid, "duplicate marker id")
        markers.add(mid)
        if gid not in groups:
            err("E_MARKER_GROUP", mid, f"unknown group {gid}")
        if m["kind"] not in MARKER_KINDS:
            err("E_MARKER_KIND", mid, f"unknown kind {m['kind']}")
        dd = m.get("direction_xyz")
        if not dd or sum(c * c for c in dd) < 1e-9:
            err("E_MARKER_DIRECTION", mid, "direction must be non-zero")
        if not in_bounds(m["xyz"], 2.0):
            err("E_MARKER_BOUNDS", mid, f"origin {m['xyz']} implausibly far outside bounds")
        if m.get("source_cell") is not None:
            sc = tuple(m["source_cell"])
            if sc not in src:
                err("E_MARKER_SOURCE", mid, f"source cell {list(sc)} not occupied")
            elif str(next(c["group_id"] for c in cells if tuple(c["xyz"]) == sc)) != gid:
                err("E_MARKER_SOURCE", mid, f"source cell {list(sc)} belongs to another group")

    for gid, g in groups.items():
        if g["role"] != "point_defense":
            continue
        if not any(str(m["group_id"]) == gid and m["kind"] in ("muzzle", "pivot")
                   for m in d["markers"]):
            warn("W_EQUIPMENT_MARKER_MISSING", f"$.groups[{gid}]",
                 "PD group has no authored muzzle/pivot marker")

    pd_groups = [g for g in groups.values() if g["role"] == "point_defense"]
    print(f"groups {len(groups)} (point_defense {len(pd_groups)})  cells {len(cells)}  "
          f"surfaces {len(d['surfaces'])}  markers {len(d['markers'])}")
    print(f"errors   : {len(errors)}")
    for e in errors:
        print("   E", e)
    print(f"warnings : {len(warnings)}")
    for w in warnings:
        print("   W", w)
    return 1 if errors else 0


sys.exit(main(sys.argv[1]))
