---
author: pi_b48
title: AMFM #534 — P0.0 / P0.1 / P1 evidence (Goxel selective planar slope)
date: 2026-11-05
---

# AMFM #534 — what is proven

## Environment (pinned, reproducible)

| item | value |
|---|---|
| Goxel pin | `33be5c8ed826105ab35746f893065e9ef00d3134` (`v0.15.1-61-g33be5c8e`) |
| build | `scons -j 20 mode=release werror=false` (MSYS2 ucrt64, GCC 16.2.0) |
| workspace | `C:\projects_tools\goxel-lab\` (`goxel\` clone, `assets\`, `evidence\`, `notes\`) |
| input asset | `assets\vox1.vox` sha256 `3625ec92410af2d36edc387f33576b7d4778cbd68641792ce4dfa8d288a4b9a1` |
| import fidelity | bbox `[-64 -34 -25] .. [64 34 18]`, **148,383** occupied cells (matches AMFM3D docs) |

`-Werror` is switched off with the built-in `werror=false` option so the pinned
source stays untouched; GCC 16 is newer than what upstream pinned.

## Design (as built)

* The modifier lives **on a layer**. Selecting a region and pressing *Slope*
  moves the region's cells into a dedicated `Slope N` layer and replaces that
  layer's cube geometry with a generated planar wedge mesh.
* Source-cell authority is kept: the cells are still voxels, only relocated.
  Total cell count is conserved exactly.
* Adjacent preservation is structural — every other layer keeps rendering
  cubes, so nothing outside the region can move.
* One edit = one `image_history_push()` → a single Undo/Redo step.
* `layer_get_key()` includes the slope hash, so every render cache that merges
  or caches layers invalidates correctly. Layers carrying a slope are never
  merged with neighbours.
* Approved UX preserved: **Select → Shape → Drag → Apply/Cancel**, with a
  contextual strip (direction, Rise, Apply, Cancel) and a draggable yellow
  handle plus a region wireframe in the viewport.
* Persistence: `LAYR` gains `slope` / `slope-rise` / `slope-color` dict keys
  (older Goxel builds ignore unknown keys).

## Self test — 15/15 on the real ship

`GOXEL_SELFTEST=1`, headless, on `vox1.vox`:

```
suggested region: [-64 -5 -5] .. [-44 5 -1] (1463 cells)
before: cells=148383 region_cells=715
[PASS] total cell count preserved
[PASS] cells outside the region untouched
[PASS] modifier layer created -- layers=2
[PASS] modifier holds the region cells -- moved=715 expected=715
[PASS] modifier keeps the region bounds
[PASS] modifier parameters kept
[PASS] the handle grabs a drag
[PASS] dragging the handle changes the height -- before=12.00 after=6.00
[PASS] releasing gives the tool back
[PASS] document saved as gox
[PASS] reopen keeps the cell count -- pre=148383 post=148383
[PASS] reopen keeps the modifier layer
[PASS] reopen keeps the modifier parameters
== self test: 15 checks, 0 failed ==
```

The drag checks feed the same `inputs_t` a real mouse drag produces, so the
ray/plane handle path is exercised end to end (grab → height change → tool
restored), not just the parameter setters.

## Workflow demo — rendered, not simulated

`GOXEL_SLOPE_DEMO=1` drives the normal editor path (select → Slope → adjust →
Apply) and Goxel writes the frames itself with `glReadPixels`, so the shots do
not depend on window z-order and no window had to be raised.

```
demo region: [-64 -34 -25]..[-44 34 18] rise 38.0
demo: saved evidence/reopen.gox
layer 0: name='Layer.1' cells=136447 slope=no
layer 1: name='Slope 1'  cells=11936 slope=yes      → 136447 + 11936 = 148383
```

A **separate process** then opens `reopen.gox` and renders it: the wedge is
still there and the strip re-attaches at Rise 26.00, i.e. the saved document is
editable, not baked.

### Screens (`evidence\`)

| file | shows |
|---|---|
| `frame060.png` | live preview (warm tint), strip, region wireframe, rest of hull untouched |
| `frame200.png` | side profile zoomed on the region — one straight diagonal, **no staircase** |
| `frame340.png` | after Apply, surface in the sampled hull colour |
| `reopened.png` | fresh process opening the saved `.gox` — modifier intact and re-editable |
| `selftest.gox`, `reopen.gox` | the saved documents themselves |

## Two upstream defects found while proving this

1. `save_to_file()` renders a 128×128 preview through the default framebuffer;
   with a windowless run that segfaults. Now skipped when `GOXEL_HEADLESS` is set.
2. `load_from_file(replace=true)` released layers with `volume_delete()` +
   `free()`, bypassing the refcounted `layer_delete()`. With a slope attached
   that corrupted the heap (double free of the generated mesh, caught under
   gdb). Now it goes through `layer_delete()`.

## Not done yet

* Bevel / cylinder / sphere operators (P0.1 is deliberately one operator).
* AMFM export and the AI/MCP bridge (P2, deferred by the issue).
* The mid-loop "replace the whole document" path is still fragile in stock
  Goxel; the demo therefore saves and lets a second process reopen instead.
