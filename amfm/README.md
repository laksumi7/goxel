---
author: pi_b48
title: AMFM #534 LAB candidate — selective surface shapes in Goxel
date: 2026-11-05
---

# AMFM #534 — LAB candidate (isolated, not for merge)

This branch holds a **known-good candidate** for issue #534 (AMFM repo). It is a
LAB artifact only: no PR, no merge, no production adoption, and it must not be
mixed with #532 work.

## Source pin

| item | value |
|---|---|
| upstream | `https://github.com/guillaumechereau/goxel.git` |
| pinned base | `33be5c8ed826105ab35746f893065e9ef00d3134` (`v0.15.1-61-g33be5c8e`) |
| toolchain | MSYS2 ucrt64, GCC 16.2.0 |
| input asset | `vox1.vox` sha256 `3625ec92410af2d36edc387f33576b7d4778cbd68641792ce4dfa8d288a4b9a1` (AMFM3D `labs/phase1/assets`, = `MagicaVoxelWork/car_armed.vox`) |
| import fidelity | bbox `[-64 -34 -25] .. [64 34 18]`, 148,383 occupied cells |

## Build

```sh
# inside MSYS2 ucrt64, in this directory
export PATH=/ucrt64/bin:/usr/bin:$PATH
scons -j 20 mode=release werror=false
```

`werror=false` is Goxel's own option; GCC 16 is newer than upstream pinned, so
`-Werror` has to be off. The pinned source is otherwise untouched.

## Verify (headless — no window, no screen use)

```sh
# the whole canonical pass: build, self test x3, stress, all three demos + reopens
bash scripts/regress.sh

# invariants on the real ship, incl. GOX save/reopen -> expect 160 checks, 0 failed
GOXEL_HEADLESS=1 GOXEL_SELFTEST=1 \
GOXEL_SELFTEST_SAVE=/tmp/selftest.gox ./goxel.exe /path/to/vox1.vox

# the same, plus 120 stress cycles (2,880 generated meshes + undo churn)
GOXEL_HEADLESS=1 GOXEL_SELFTEST=1 GOXEL_STRESS=120 \
GOXEL_SELFTEST_SAVE=/tmp/stress.gox ./goxel.exe /path/to/vox1.vox

# the whole workflow, rendered, and it writes reopen.gox at the end
GOXEL_HEADLESS=1 GOXEL_SLOPE_DEMO=1 GOXEL_SHOTS=/tmp/shots ./goxel.exe /path/to/vox1.vox

# the four shape kinds together
GOXEL_HEADLESS=1 GOXEL_SHAPE_DEMO=1 GOXEL_SHOTS=/tmp/shots ./goxel.exe /path/to/vox1.vox

# the placement/orientation matrix, with per-shape mesh bounds in the log
GOXEL_HEADLESS=1 GOXEL_PLACE_DEMO=1 GOXEL_SHOTS=/tmp/shots ./goxel.exe /path/to/vox1.vox

# one inside-region deletion and one recolour, before/after, plus the resolve
GOXEL_HEADLESS=1 GOXEL_EDIT_DEMO=1 GOXEL_SHOTS=/tmp/shots ./goxel.exe /path/to/vox1.vox

# a fresh process opens the saved document back
GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE=/tmp/shots/reopened.png ./goxel.exe /tmp/shots/reopen.gox
```

`scripts/` holds the exact scripts used (they assume the LAB layout
`goxel-lab/{goxel,assets,evidence,notes}`; adjust the paths if you move it).

## Interactive launch (manual smoke test)

```
Godot-free, plain Goxel:   goxel.exe --screen 2 --fullscreen <path to vox1.vox>
```

On the LAB machine, test windows go on the laptop built-in panel only
(`--screen 2`), never on the external monitors, and never with a console window.

## Live co-editing bridge (#532 A3-P0 spike)

`src/lab_bridge.c` adds an opt-in, loopback-only control channel to the running
editor. It is a LAB adapter for a bounded feasibility proof, not an MCP server
and not a remote-control service. Stock Goxel has no socket/rpc code at all, so
this transport had to be written here; the separate patched `yberudev/Goxel-MCP`
repository is a different codebase whose bridge is Unix-socket only.

```sh
# build
export PATH=/ucrt64/bin:/usr/bin:$PATH
LDFLAGS=-mwindows scons -j 20 mode=release werror=false

# one command against a running editor that was started with GOXEL_LAB_PORT
python amfm/scripts/labctl.py -p 47534 state

# the whole bounded proof (opens and closes its own editor windows)
bash amfm/scripts/p2_proof.sh /path/to/pd_turret.gox /path/to/StarMarineTrooper.vox
```

Commands: `ping`, `state`, `set x y z [r g b]`, `del x y z`, `undo`, `redo`,
`help`. Edits go through the same undo history as the brush, so Ctrl+Z in the
editor reverts them. Refused while a shape session is open or the active layer is
a generated-shape layer. See `amfm/notes/P2-live-bridge.md` for what was proven
and, importantly, what is **NOT TESTED** (no revision/stale-write rejection, no
multi-step atomicity, no per-process authorization).

## What is in here

* `src/slope.c`, `src/slope.h` — the surface modifier: four shape kinds
  (slope, bevel, cylinder, sphere), three anchor planes (floor, mid, ceil),
  open/closed form, region → generated surface, viewport handle, contextual
  strip, in-region edit conflict check, self test, stress mode, demo drivers.
* `src/layer.{c,h}`, `src/goxel.c`, `src/gui/app.c`, `src/main.c`,
  `src/formats/gox.c` — integration: layer field + hash + copy, render skip and
  generated-surface render, iter hook, strip entry, GOX persistence, and five
  defect fixes (headless preview render, refcounted layer replacement, a
  four-byte heap overrun in the generated mesh, a stack overrun in the handle
  drag, and a mesh leak in `slope_free`).
* `src/lab_bridge.{c,h}` — the opt-in loopback control channel for the #532
  A3-P0 spike (see the bridge section above), with the `slope_session_is_open()`
  accessor in `src/slope.{c,h}` that lets it refuse to fight a shape session.
* `amfm/scripts/` — `p2_proof.sh` (the bounded proof, PASS/FAIL per check),
  `labctl.py` (client) and `cap_pid.ps1` (window capture by pid).
* `notes/P0-evidence.md` — the one-slope workflow, what was proven and how.
* `notes/P0.2-shapes.md` — bevel, cylinder and sphere on the same path.
* `notes/P0-evidence.md` — the one-slope workflow, what was proven and how.
* `notes/P0.2-shapes.md` — bevel, cylinder and sphere on the same path.
* `notes/P0.3-placement.md` — anchor planes and open/closed forms, edits inside a
  shape region, and the stress run.
* `notes/P0.4-inside-region-edits.md` — edited cells vs the visible surface: the
  limitation stated honestly, the stale state and its two resolutions.
* `evidence/` — self test log, demo log, rendered frames and the saved
  documents. `evidence/shapes/` holds the four-shape run,
  `evidence/placement/` the placement matrix, `evidence/edits/` the
  inside-region before/after pair, `evidence/stress/` the stress runs,
  `evidence/clean_build.log` the from-scratch build.

## Status

P0.0 (pin + real asset opens), P0.1 (one selective planar slope, end to end)
and the mandatory editable save/reopen are demonstrated headlessly; Bevel,
Cylinder and Sphere run through the same selective path; shapes are not
upward-only (floor/mid/ceil anchors, open and closed forms) and were verified
numerically on the real ship. A voxel edit inside a shape's region does **not**
change the generated surface — that is a real limitation of a region-derived
surface and is recorded as one — but it now flags the surface as stale in the
viewport and offers Accept edits / Restore cells, demonstrated before and after
on the real ship. 120 stress cycles over 2,880 generated meshes plus the undo
stack leave the heap intact and leak nothing, from a clean from-scratch build
with no warnings in the changed files. **164 checks, 0 failed** three runs
running, **168** with the stress phase. **Owner GUI ergonomics are NOT signed
off** — that needs the hands-on pass on a free display.

The #532 A3-P0 live-edit spike is proven on Windows: connect, one undoable edit
seen in the viewport, undo, two-instance isolation, and the editor stays
responsive after malformed clients. That is a transport feasibility proof and is
explicitly **not** P2 acceptance — no scene revision check, no stale-write
rejection, no multi-step atomicity, no per-process authorization. See
`amfm/notes/P2-live-bridge.md`.
