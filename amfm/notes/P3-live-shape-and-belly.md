# P3 — live shape work and the belly PD mounts (#534)

Follows `P2-live-bridge.md`. Same bridge, same rules: loopback only, one client,
every edit through the editor's own history so Ctrl+Z reverts it.

## What was added to the code

* `slope_apply_shape_at(cell, name, rise)` in `src/slope.c` — rounds one voxel
  into a closed cylinder using the normal authoring path (select the cell,
  begin the shape, set the parameters, apply). Returns 0 or a refusal code
  (-1 a shape session is open, -3 the cell is empty, -4 the region was refused).
  It finds the layer that actually holds the cell, so it works whatever the
  active layer is.
* `shape x y z [rise] [name]` in `src/lab_bridge.c` — one bounded call to the
  above, with the same guards as `set`/`del`.
* `slope_dump_cells()` now ends with a `# colour r g b a count=...` summary, so
  a script can reuse the exact colours of an existing part.

## Turret census (how many PD mounts the ship has)

`scripts/turret_census.py` finds mounts from geometry: a flat 3×3 single-colour
plate, a turntable cell on it, a body above that, and a white sensor. Run on
both the untouched original `StarMarineTrooper.vox` (24,538 cells) and the
Owner's `StarMarineTrooper_andrew.gox` (24,537) it returns the same **five**
dorsal mounts, so the count is from before any cylinder work:

| mount | pad | barrel cells |
|---|---|---|
| m1 | x−23..−21 y10..12 z6 | (−23,7,8) (−21,7,8) |
| m2 | x−6..−4 y20..22 z8 | (−6,17,10) (−4,17,10) |
| m3 | x3..5 y−5..−3 z7 | (3,−8,9) (5,−8,9) — already round |
| m4 | x3..5 y20..22 z8 | (3,17,10) (5,17,10) |
| m5 | x20..22 y9..11 z6 | (20,6,8) (22,6,8) |

Two further 3×3 hits (x±14..16 y6..8 z−4) are below deck, symmetric and have no
sensor: not turrets.

`scripts/pd_round_live.sh` converts the four remaining mounts live: 8 `shape`
calls, cell count unchanged at 24,537, layers 5 → 13, history 2 → 10.

## The three belly mounts the Owner asked for

One under the bow, one under the middle of each wing. `scripts/pad_check.py`
picked the spots: a flat 3×3 belly plate, five levels of clear air below it and
a clear barrel corridor forward.

| mount | pad | barrels (cylinders) |
|---|---|---|
| bow | x−1..1 y−31..−29 @ z−9 | (−1,−34,−11) (1,−34,−11) |
| port wing | x−17..−15 y13..15 @ z−10 | (−17,10,−12) (−15,10,−12) |
| stbd wing | x14..16 y13..15 @ z−10 | (14,10,−12) (16,10,−12) |

`scripts/belly_mounts.py` builds them live: 23 cells per mount through `set`
(pad 9, turntable 1, body 10, barrels 2, sensor 1) then `shape` on the barrels.
Total **24,537 → 24,606**, shape layers 10 → 16. `scripts/verify_belly.py`
re-checks every cell and colour after a save/reopen.

## Reproduce

```sh
# 1. build
export PATH=/ucrt64/bin:/usr/bin:$PATH
LDFLAGS=-mwindows scons -j 20 mode=release werror=false

# 2. launch a copy of the ship with the bridge on the laptop panel
GOXEL_LAB_PORT=47534 GOXEL_FULLSCREEN_AT=3840,956 \
  ./goxel.exe "/z/data_art/Ref_SF/voxels/Star Marine Trooper/StarMarineTrooper_andrew_cyl.gox" &

# 3. census / clearance / build / verify
python amfm/scripts/turret_census.py <cell-dump.txt>
python amfm/scripts/pad_check.py <cell-dump.txt> bow,-1,-31 wing,-17,13
python amfm/scripts/belly_mounts.py 47534 [--dry]
python amfm/scripts/verify_belly.py <cell-dump-after-save.txt>
```

The cell dump comes from `GOXEL_CELL_DUMP=<file>` together with
`GOXEL_SEL_DUMP=<file>` (the cell dump is nested inside that block).

## Known gaps this exposed

1. **Error replies have no terminator.** Only success paths send `ok`; a client
   waiting for it hangs. The first belly run stopped mid-mount because of this
   (and because the barrels were shaped before they were placed). Every reply
   must terminate.
2. **No camera verb**, so the agent cannot frame the model and has to ask the
   Owner to zoom. `frame` is the highest-value addition.
3. **No save verb**, so the work only reaches disk when the Owner presses
   Ctrl+S.
4. **No bulk fill**, so a turret costs 23 round trips.

The Owner's directive (see the #534 report) is to design the command layer so
all the real-time commands we need can be added, not to keep patching one verb
at a time.
