---
author: pi_b48
title: AMFM #532 A3-P0 — Windows live-edit bridge, bounded proof
date: 2026-11-05
---

# P2 live co-editing — bounded proof (under #534)

Scope as authorized: connect to the actual Windows editor and perform **one
reversible, revision-safe small edit** in the current document and viewport,
leaving the manual editing flow intact. No new MCP stack. Owner go-ahead
2026-11-05; PM bounded the spike in the #534 review.

## Which codebase is which

* **Stock Goxel** (`guillaumechereau/goxel`, and this pinned tree) has **no
  socket / rpc / pipe / daemon code at all** — nothing to switch on.
* The earlier PM premise referred to the separate patched **`yberudev/Goxel-MCP`**
  repository, whose `bridge/mcp_bridge.c` does implement a Unix-domain-socket
  editor bridge and compiles out on Windows (`_WIN32` stub).
* Therefore the Windows transport had to be written for this candidate. It is a
  **LAB-local adapter**, not a second MCP architecture: no protocol stack, no
  session layer, no discovery, seven verbs.

## Design

| property | implementation |
|---|---|
| opt-in | `lab_bridge_init()` runs only when `GOXEL_LAB_PORT` is set |
| loopback | binds `127.0.0.1` only, backlog 1, one client at a time |
| never blocks | `lab_bridge_iter()` runs after `goxel_iter()`; accept/recv/send are non-blocking and do a bounded amount of work per frame |
| bounded input | 8 KiB request buffer, 64 KiB response buffer, oversized input drops the client |
| verbs | `ping`, `state`, `set x y z [r g b]`, `del x y z`, `undo`, `redo`, `help` |
| revision-safe | every edit goes through `image_history_push()`, the same history the brush uses, so editor Ctrl+Z reverts a remote edit exactly like a manual one |
| refuses bad targets | refuses while a shape session owns the viewport (`slope_session_is_open()`) or while the active layer is a generated-shape layer (a shape layer holds almost no voxels, so a subtract there would look like a no-op) |
| no destructive verbs | no save, no quit, no file write |

Client: `amfm/scripts/labctl.py`.

## Reproduce

```sh
# build (MSYS2 ucrt64, in the repo root)
export PATH=/ucrt64/bin:/usr/bin:$PATH
LDFLAGS=-mwindows scons -j 20 mode=release werror=false

# proof: needs a display; it opens and closes its own editor windows
bash amfm/scripts/p2_proof.sh /path/to/pd_turret.gox /path/to/StarMarineTrooper.vox
```

`p2_proof.sh` refuses to run if the build did not relink, and aborts if the ports
are already taken, so it cannot silently test a stale binary or talk to someone
else's instance.

## Proof result (see `evidence/p2/`)

| check | result |
|---|---|
| connect to the real Windows editor | `pong pid=28656 port=47534 frame=34` |
| read live state | doc, frame, modified flag, painter mode, active layer, selection, per-layer cells (`YawBase` 1, `PitchAssembly` 10, barrels 1+1), history depth |
| one bounded edit | `set 4 -4 12 cells 10->11 history pushed`, voxel visible in the viewport (`p2_2_edited.png`) |
| reversible | `undo applied`, voxel gone in the viewport (`p2_3_undone.png`); state identical apart from the frame counter and history depth |
| guards | `del` on an empty cell → `noop`; unknown verb → `err unknown command` |
| second-instance isolation | A on `pd_turret.gox` (pid 28656) and B on `StarMarineTrooper.vox` (pid 11288) answer separately; editing B leaves A untouched |
| manual flow intact | after a client that sends binary junk and one that half-closes, A still answers (`pong ... frame=1079`) |
| cleanup | only the test instances were closed |

## NOT TESTED (do not read this as P2 acceptance)

* **No scene revision / expected-state check.** Nothing rejects a write when the
  Owner edits between the AI read and the AI write; there is no conflict
  rejection or rebase.
* **No multi-step atomicity.** A sequence of `set`/`del` calls is a sequence of
  separate history entries; partial failure mid-sequence is not rolled back.
* **No preview / commit / discard** verbs, and no shape-modifier verbs yet.
* **No per-process authorization.** `127.0.0.1` is not an authorization boundary:
  any local process can connect while the env gate is on. Local-process exposure
  must be evaluated before broadening write operations.
* Owner GUI ergonomics of the bridge (there is no UI) and long-session stability
  under repeated remote edits are not measured.

## Bugs found on the way

1. **History walk** (mine, fixed): the undo history is a list of snapshots and
   `img` is not a member of it, so the walk must stop at `NULL`. Upstream's
   `image_history_resize()` compares against `img` and would run off the end of
   the list — a real latent bug, left untouched here as out of scope.
2. **Cell counting**: the volume iterator walks whole tiles, so occupancy has to
   be tested on the alpha byte; without that every layer reports one full tile.
3. **Stale binary trap**: a failed `scons` leaves the previous `goxel.exe`, so the
   next test silently exercises old code. The script now checks the relink.
4. `DL_FOREACH2` in the vendored uthash takes three arguments (`head, el, next`).

## Next gate (only if the Owner authorizes it separately)

Pick the smallest compatibility layer between this proven Windows transport and
the existing `yberudev/Goxel-MCP` command/session stack: a real shared-scene MCP
edit with optimistic revision / stale-write rejection, one coherent Undo, visible
preview/commit/cancel; then expose the Slope/Bevel/Cylinder/Sphere modifiers
through canonical Goxel commands with persistent metadata. Do not rebuild the
existing MCP feature set, do not add a competing geometry renderer, and do not
call bare `set`/`del` an AI-editing Product PASS.
