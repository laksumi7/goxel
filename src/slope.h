/* Goxel 3D voxels editor
 *
 * AMFM #534 LAB: selective, non-destructive surface shape modifier.
 *
 * A modifier is attached to a layer.  The layer keeps the underlying voxel
 * occupancy (the source cells) but its cube geometry is replaced by a
 * generated surface, so neighbouring layers keep their ordinary blocky voxels
 * untouched.
 */

#ifndef SLOPE_H
#define SLOPE_H

#include "model3d.h"

#include <stdbool.h>
#include <stdint.h>

#include <stdio.h>

typedef struct layer layer_t;
typedef struct inputs inputs_t;

// Goxel is Z-up: the surface runs along X or Y and rises along Z.
enum {
    SLOPE_DIR_PX = 0,
    SLOPE_DIR_NX,
    SLOPE_DIR_PY,
    SLOPE_DIR_NY,
    SLOPE_DIR_COUNT,
};

// The generated surface kinds.  Every kind is built inside the region box and
// measured from the plane chosen by `anchor`.
enum {
    SLOPE_KIND_SLOPE = 0,
    SLOPE_KIND_BEVEL,
    SLOPE_KIND_CYLINDER,
    SLOPE_KIND_SPHERE,
    SLOPE_KIND_COUNT,
};

// Which region plane the shape is measured from, so a shape is not stuck
// growing upwards: a hull side can slope inwards, a barrel can lie across the
// ship, a casing can hang from the deck.
enum {
    SLOPE_ANCHOR_FLOOR = 0,  // grows up from the region bottom
    SLOPE_ANCHOR_MID,        // centred on the region mid plane
    SLOPE_ANCHOR_CEIL,       // grows down from the region top
    SLOPE_ANCHOR_COUNT,
};

typedef struct slope_mod {
    int      aabb[2][3];  // Inclusive source-cell region replaced by the surface.
    int      kind;        // SLOPE_KIND_*
    int      dir;         // SLOPE_DIR_*: axis the surface is built along.
    int      anchor;      // SLOPE_ANCHOR_*: which region plane it is measured from.
    int      closed;      // Full barrel / ellipsoid / all-edge chamfer instead of
                          // an arch, dome or single directed chamfer.
    float    rise;        // Height of the surface above its anchor plane, in voxels.
    uint32_t revision;
    uint32_t applied_hash;  // Cells of this layer inside the region when the
                            // shape was applied; edits after that are a conflict.
                            // Persisted, so a stale shape stays stale across save.
    int      stale;         // Derived: applied_hash no longer matches the cells.
    uint8_t  color[4];    // Surface colour (sampled from the source cells).
    // Transient render cache: not copied, not hashed.
    model3d_t *mesh;
    uint32_t  mesh_hash;
} slope_mod_t;

slope_mod_t *slope_new(void);
slope_mod_t *slope_copy(const slope_mod_t *other);
void         slope_free(slope_mod_t *mod);
uint32_t     slope_hash(const slope_mod_t *mod);

// True when the layer's cube geometry is replaced by a generated surface.
bool slope_replaces_voxels(const layer_t *layer);

// Display name of a SLOPE_KIND_* value.
const char *slope_kind_name(int kind);

// Frame update for the viewport handles.  Must run before the tools so that
// dragging a handle never paints voxels.
void slope_iter(const inputs_t *inputs, const float viewport[4]);

// Queue the generated surfaces of the current image.
void slope_render(void);

// Draw the live handles of the modifier being edited.
void slope_render_handles(void);

// Contextual action strip, called from the imgui pass.
void slope_gui(void);

// Headless verification of the modifier invariants (GOXEL_SELFTEST=1).
void slope_selftest(void);

// Drive the modifier through the normal editor path for screenshots
// (GOXEL_SLOPE_DEMO=1), or every shape kind in turn (GOXEL_SHAPE_DEMO=1).
void slope_demo_begin(void);
void slope_demo_begin_shapes(void);
void slope_demo_begin_placement(void);
void slope_demo_begin_edits(void);
void slope_demo_begin_pd(void);
void slope_demo_begin_extract(void);
void slope_dump_layers(FILE *f);
void slope_dump_cells(const char *path);
void slope_ensure_art_layer(void);

// True while a shape session owns the viewport handles.  The live bridge (#532
// A3-P0) refuses to edit then, so it can never fight the manual flow.
bool slope_session_is_open(void);

// Round off one voxel as a closed cylinder through the normal authoring path
// (select -> shape -> apply), so the edit is undoable and adds no voxels.
// Used by the live bridge's `shape` verb.  Returns 0 or a negative refusal code.
int slope_apply_shape_at(const int cell[3], const char *name, float rise);

/*
 * AMFM #534 P0: a generated surface has to travel with the cells it was made
 * from.  The surface is built inside `aabb`, in world cells, so any transform
 * that moves the cells must move the region too or the surface is left behind.
 */

// Transform an inclusive cell region by a matrix made of translations, 90
// degree rotations and axis flips, and normalise it back to a cell box.
void slope_transform_aabb(const int src[2][3], const float mat[4][4], int out[2][3]);

// Move the surface of `layer` along with a transform already applied to its
// voxels.  Also turns `dir` when the matrix rotates or flips the shape's axis.
// Returns 0, or 1 when the axis could not be represented after the transform.
int slope_move_surface(layer_t *layer, const float mat[4][4]);

// AMFM #541 rev3 (R3-2): recompute the layer's shape staleness from the live
// source cells right now (and record it on the modifier).  The cached `stale`
// flag is only refreshed by the shape's own edit paths; a gate that publishes
// semantics asks the source afresh through this.  Hashes only the shape's own
// layer, so it belongs on an action (confirm, publish), not on every frame.
bool slope_refresh_stale(layer_t *layer);

// True when the layer's shape region holds cells but the mask does not cover
// all of them — a partial grab that must be flagged rather than silently
// leaving part of the surface behind.
bool slope_region_partly_in(const layer_t *layer, const volume_t *mask);

// AMFM #534 P0 proof: select a region, move it through the same calls the Move
// tool makes, and dump before/after/undo.  Driven by GOXEL_MOVE_TEST.
void slope_demo_begin_move_test(void);

// Save numbered frames of the finished UI when GOXEL_SHOTS=<dir> is set, or a
// single frame to the file named by GOXEL_SHOTS_ONCE.
void slope_capture(void);

// AMFM #534: move the cells named by a spec file into named layers, keeping the
// id of every layer it does not touch, and delete the cells a D line names.
// Returns the save result.
int slope_regroup_apply(const char *spec_path, const char *out);

#endif // SLOPE_H
