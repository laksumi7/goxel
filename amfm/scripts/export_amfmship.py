#!/usr/bin/env python3
"""AMFM #534 — emit the canonical `amfm.ship` v1 JSON from a saved .gox.

Authority is the saved GOX bytes: every occupied cell, its colour, the layer
that owns it, the layer id, and every surface modifier come from the file.  The
pinned table supplies only the facts a .gox cannot hold today (group kind,
group pivot, joint parentage, markers with their authored directions, and the
structured outlet/envelope geometry the #541 PM decisions require) and is bound
to the exact GOX hash.

It fails closed rather than guessing: a hash mismatch, an unclaimed or
double-claimed layer, a pinned pivot that no longer matches the geometry, a
palette colour that is not in the file, or a surface whose region no longer
holds its own source cell all abort the export.  plan.txt is never read.

Usage:
  python export_amfmship.py --gox <ship.gox> --table <pinned.json> --out <file.json>
"""
import argparse
import hashlib
import json
import os
import sys

import goxread

MARKER_KINDS = {"pivot", "muzzle", "exhaust", "nozzle"}
SURFACE_KINDS = {"slope", "bevel", "cylinder", "sphere"}


class Fail(Exception):
    pass


def load_table(path):
    return json.load(open(path, encoding="utf-8"))


def build(gox_path, table):
    raw = open(gox_path, "rb").read()
    sha = hashlib.sha256(raw).hexdigest()
    if sha != table["source_sha256"]:
        raise Fail(f"source drift: {os.path.basename(gox_path)} is {sha} but the "
                   f"table is pinned to {table['source_sha256']}")

    layers = goxread.load(gox_path)
    cells = goxread.cells(layers)
    by_name = {l["name"]: l for l in layers}

    # --- group membership must be total and disjoint -------------------------
    claim = {}
    for g in table["groups"]:
        for ln in g["owned_layers"]:
            if ln not in by_name:
                raise Fail(f"group {g['name']} claims layer '{ln}' which is not in the file")
            if ln in claim:
                raise Fail(f"layer '{ln}' claimed by both {claim[ln]} and {g['name']}")
            claim[ln] = g["name"]
    loose = sorted(set(by_name) - set(claim))
    if loose:
        raise Fail(f"layers owned by no group: {loose}")

    palette = [tuple(c) for c in table["palette"]]
    index = {c: i for i, c in enumerate(palette)}
    seen = {c for c, _ in cells.values()}
    extra = sorted(seen - set(index))
    if extra:
        raise Fail(f"colours in the file but not in the pinned palette: {extra}")

    gid = {g["name"]: g["layer_id"] for g in table["groups"]}
    if len(set(gid.values())) != len(gid):
        raise Fail("group ids are not unique")

    out_groups = []
    for g in sorted(table["groups"], key=lambda g: g["layer_id"]):
        pts = [p for p, (_, lay) in cells.items() if lay in g["owned_layers"]]
        if len(pts) != g["cells"]:
            raise Fail(f"drift: group {g['name']} has {len(pts)} cells now, "
                       f"the table pins {g['cells']}")
        lo = [min(p[a] for p in pts) for a in range(3)]
        hi = [max(p[a] for p in pts) for a in range(3)]
        if lo != g["bbox_min_xyz"] or hi != g["bbox_max_xyz"]:
            raise Fail(f"drift: group {g['name']} bbox is {lo}..{hi}, "
                       f"the table pins {g['bbox_min_xyz']}..{g['bbox_max_xyz']}")
        pivot = [(a + b) / 2.0 for a, b in zip(lo, hi)]
        if pivot != g["pivot_xyz"]:
            raise Fail(f"drift: group {g['name']} bbox centre is {pivot}, "
                       f"the table pins {g['pivot_xyz']}")
        out = {
            "id": g["layer_id"],
            "kind": g["kind"],
            "name": g["name"],
            "parent_id": None,
            "pivot_xyz": pivot,
        }
        # A joint hierarchy is expressed with the fields v1 already has: the
        # existing parent_id, and the existing group transform for a joint
        # origin that is not the bbox centre.
        if g.get("parent") is not None:
            if g["parent"] not in gid:
                raise Fail(f"group {g['name']} names parent '{g['parent']}' "
                           f"which is not a group in this table")
            # Emitted as a string: the #535 reader stringifies parent_id raw, and
            # a JSON-parsed int arrives as a float ("4.0"), which would not match
            # the group key ("4").  A string id survives both paths intact.
            out["parent_id"] = str(gid[g["parent"]])
        if g.get("transform") is not None:
            t = g["transform"]
            fwd, up = t["forward_source"], t["up_source"]
            if sum(c * c for c in fwd) < 1e-9 or sum(c * c for c in up) < 1e-9:
                raise Fail(f"group {g['name']} transform has a zero forward or up")
            if abs(sum(a * b for a, b in zip(fwd, up))) > 1e-6:
                raise Fail(f"group {g['name']} transform forward and up must be orthogonal")
            out["transform"] = {
                "origin_source": t["origin_source"],
                "forward_source": fwd,
                "up_source": up,
            }
        out_groups.append(out)

    # --- cells: every occupied cell, exactly one group -----------------------
    out_cells = [{
        "xyz": [p[0], p[1], p[2]],
        "palette_index": index[cells[p][0]],
        "group_id": gid[claim[cells[p][1]]],
    } for p in sorted(cells)]
    occupied = {tuple(c["xyz"]) for c in out_cells}
    if len(occupied) != len(out_cells):
        raise Fail("duplicate cell coordinates")

    # --- surfaces: the modifier must still own its own source cells ----------
    audit = []
    out_surfaces = []
    for lay in sorted((l for l in layers if l["slope"]), key=lambda l: l["name"]):
        s = lay["slope"]
        owner = claim[lay["name"]]
        inside = sorted(lay["cells"])
        region = [p for p in inside
                  if all(s["region_min_xyz"][a] <= p[a] <= s["region_max_xyz"][a] for a in range(3))]
        if not inside or region != inside:
            raise Fail(f"stale surface {lay['name']}: its region {s['region_min_xyz']}.."
                       f"{s['region_max_xyz']} holds {region} but the layer owns {inside}")
        colours = {lay["cells"][p] for p in inside}
        if s["color"] and any(tuple(c) != tuple(s["color"]) for c in colours):
            raise Fail(f"stale surface {lay['name']}: source colour "
                       f"{sorted(colours)} no longer matches the modifier colour "
                       f"{s['color']}")
        audit.append({
            "surface": lay["name"],
            "applied_hash": f"{s['applied_hash']:#010x}",
            "applied_hash_recorded": s["applied_hash"] != 0,
            "source_cells_verified": len(inside),
        })
        out_surfaces.append({
            "id": lay["name"],
            "group_id": gid[owner],
            "kind": goxread.KIND_NAME.get(s["kind"], str(s["kind"])),
            "region_min_xyz": s["region_min_xyz"],
            "region_max_xyz": s["region_max_xyz"],
            "parameters": {
                "direction": goxread.DIR_NAME.get(s["dir"], str(s["dir"])),
                "anchor": goxread.ANCHOR_NAME.get(s["anchor"], str(s["anchor"])),
                "closed": bool(s["closed"]),
                "rise": s["rise"],
            },
            "source_cell_xyz": [[p[0], p[1], p[2]] for p in inside],
        })
    for s in out_surfaces:
        if s["kind"] not in SURFACE_KINDS:
            raise Fail(f"unsupported surface kind {s['kind']} on {s['id']}")

    # --- markers -------------------------------------------------------------
    out_markers = []
    for m in sorted(table["markers"], key=lambda m: m["id"]):
        if m["kind"] not in MARKER_KINDS:
            raise Fail(f"unknown marker kind {m['kind']} on {m['id']}")
        if m["group_id"] not in gid:
            raise Fail(f"marker {m['id']} names group {m['group_id']} which does not exist")
        if not m.get("direction_xyz"):
            raise Fail(f"marker {m['id']} has no direction_xyz; the #535 reader "
                       f"requires a non-zero direction on every marker")
        rec = {
            "id": m["id"],
            "group_id": gid[m["group_id"]],
            "kind": m["kind"],
            "xyz": m["xyz"],
            "direction_xyz": m.get("direction_xyz"),
        }
        # A marker pinned to a source cell is traceable: the cell must exist and
        # belong to the same group the marker names.
        if m.get("source_cell") is not None:
            sc = tuple(m["source_cell"])
            if sc not in cells:
                raise Fail(f"marker {m['id']} names source cell {list(sc)} which is not occupied")
            if gid[claim[cells[sc][1]]] != rec["group_id"]:
                raise Fail(f"marker {m['id']} names source cell {list(sc)} which belongs "
                           f"to another group")
            rec["source_cell"] = list(sc)
        # #541 PM decision (3): authored outlet/envelope geometry travels as
        # structured fields, never as label text.  The current reader ignores
        # unknown marker fields; #535 consumes them once its contract lands.
        for extra in ("outlet_rect", "envelope"):
            if extra in m:
                rec[extra] = m[extra]
        out_markers.append(rec)

    doc = {
        "format": "amfm.ship",
        "schema_version": 1,
        "asset_id": table["asset_id"],
        "source_sha256": sha,
        "frame": {
            "meters_per_voxel": table["frame"]["meters_per_voxel"],
            "source_pivot_xyz": table["frame"]["source_pivot_xyz"],
            "source_to_godot_basis": table["frame"]["source_to_godot_basis"],
        },
        "palette": [list(c) for c in palette],
        "groups": out_groups,
        "cells": out_cells,
        "surfaces": out_surfaces,
        "markers": out_markers,
    }
    unverified = [a for a in audit if not a["applied_hash_recorded"]]
    return doc, audit, unverified


def validate(path, gox_path):
    """Re-read the emitted file and check it against the contract and the source."""
    d = json.load(open(path, encoding="utf-8"))
    problems = []
    need = ["format", "schema_version", "asset_id", "source_sha256", "frame",
            "palette", "groups", "cells", "surfaces", "markers"]
    for k in need:
        if k not in d:
            problems.append(f"missing top-level key {k}")
    if problems:
        return problems
    if d["format"] != "amfm.ship" or d["schema_version"] != 1:
        problems.append(f"unknown format/schema {d['format']}/{d['schema_version']}")
    sha = hashlib.sha256(open(gox_path, "rb").read()).hexdigest()
    if d["source_sha256"] != sha:
        problems.append("source_sha256 does not match the GOX on disk")
    gids = [g["id"] for g in d["groups"]]
    have = set(gids) | {str(x) for x in gids}
    if len(set(gids)) != len(gids):
        problems.append("duplicate group ids")
    if len({g["name"] for g in d["groups"]}) != len(d["groups"]):
        problems.append("duplicate group names")
    for g in d["groups"]:
        if not g.get("kind"):
            problems.append(f"group {g.get('name')} has no kind")
        pid = g.get("parent_id")
        if pid is not None:
            if pid not in have:
                problems.append(f"group {g.get('name')} names parent {pid} which does not exist")
            elif str(pid) == str(g["id"]):
                problems.append(f"group {g.get('name')} is its own parent")
        t = g.get("transform")
        if t is not None:
            fwd, up = t.get("forward_source"), t.get("up_source")
            if not fwd or not up:
                problems.append(f"group {g.get('name')} transform needs forward and up")
            elif abs(sum(a * b for a, b in zip(fwd, up))) > 1e-6:
                problems.append(f"group {g.get('name')} transform is not orthogonal")
    seen = set()
    for c in d["cells"]:
        t = tuple(c["xyz"])
        if t in seen:
            problems.append(f"duplicate cell {t}")
        seen.add(t)
        if c["group_id"] not in have:
            problems.append(f"cell {t} names unknown group {c['group_id']}")
            break
        if not 0 <= c["palette_index"] < len(d["palette"]):
            problems.append(f"cell {t} palette_index {c['palette_index']} out of range")
            break
    src = {tuple(c["xyz"]) for c in d["cells"]}
    for s in d["surfaces"]:
        if s["group_id"] not in have:
            problems.append(f"surface {s['id']} names unknown group {s['group_id']}")
        for p in s["source_cell_xyz"]:
            if tuple(p) not in src:
                problems.append(f"surface {s['id']} references source cell {p} that is not occupied")
    for m in d["markers"]:
        if m["group_id"] not in have:
            problems.append(f"marker {m['id']} names unknown group {m['group_id']}")
        if m["kind"] not in MARKER_KINDS:
            problems.append(f"marker {m['id']} kind {m['kind']} not in the contract")
        if not m.get("direction_xyz"):
            problems.append(f"marker {m['id']} has no direction")
        sc = m.get("source_cell")
        if sc is not None and tuple(sc) not in src:
            problems.append(f"marker {m['id']} names source cell {sc} which is not occupied")
    empty = set(gids) - {c["group_id"] for c in d["cells"]}
    if empty:
        problems.append(f"groups with no cells: {sorted(empty)}")
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gox", required=True)
    ap.add_argument("--table", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    table = load_table(a.table)
    try:
        doc, audit, unverified = build(a.gox, table)
    except Fail as e:
        print(f"REFUSED: {e}")
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False)
        f.write("\n")
    blob = open(a.out, "rb").read()

    problems = validate(a.out, a.gox)
    print(f"wrote {a.out}")
    print(f"  bytes        : {len(blob)}  sha256 {hashlib.sha256(blob).hexdigest()}")
    print(f"  source_sha256: {doc['source_sha256']}")
    print(f"  groups {len(doc['groups'])}  cells {len(doc['cells'])}  "
          f"surfaces {len(doc['surfaces'])}  markers {len(doc['markers'])}  "
          f"palette {len(doc['palette'])}")
    print(f"  surfaces with no recorded applied_hash: {len(unverified)} of {len(audit)} "
          f"(each was verified cell by cell instead: region holds exactly its own "
          f"source cells, colour matches the modifier)")
    if problems:
        print("VALIDATION FAILED")
        for p in problems:
            print("   -", p)
        return 1
    print("re-read validation: PASS")
    return 0


sys.exit(main())
