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
# invariants on the real ship, incl. GOX save/reopen  -> expect 15/15 PASS
GOXEL_HEADLESS=1 GOXEL_SELFTEST=1 \
GOXEL_SELFTEST_SAVE=/tmp/selftest.gox ./goxel.exe /path/to/vox1.vox

# the whole workflow, rendered, and it writes reopen.gox at the end
GOXEL_HEADLESS=1 GOXEL_SLOPE_DEMO=1 GOXEL_SHOTS=/tmp/shots ./goxel.exe /path/to/vox1.vox

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

## What is in here

* `src/slope.c`, `src/slope.h` — the surface modifier: four shape kinds
  (slope, bevel, cylinder, sphere), region → generated surface, viewport handle,
  contextual strip, self test, demo drivers.
* `src/layer.{c,h}`, `src/goxel.c`, `src/gui/app.c`, `src/main.c`,
  `src/formats/gox.c` — integration: layer field + hash + copy, render skip and
  generated-surface render, iter hook, strip entry, GOX persistence, and three
  defect fixes (headless preview render, refcounted layer replacement, and a
  four-byte heap overrun in the generated mesh).
* `notes/P0-evidence.md` — the one-slope workflow, what was proven and how.
* `notes/P0.2-shapes.md` — bevel, cylinder and sphere on the same path.
* `evidence/` — self test log, demo log, rendered frames and the saved
  documents. `evidence/shapes/` holds the four-shape run.

## Status

P0.0 (pin + real asset opens), P0.1 (one selective planar slope, end to end)
and the mandatory editable save/reopen are demonstrated headlessly, and Bevel,
Cylinder and Sphere now run through the same selective path: 49 checks, 0 failed,
three consecutive runs on the real ship. **Owner GUI ergonomics are NOT signed
off** — that needs the hands-on pass.
