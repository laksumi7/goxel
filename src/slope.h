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

// Save numbered frames of the finished UI when GOXEL_SHOTS=<dir> is set, or a
// single frame to the file named by GOXEL_SHOTS_ONCE.
void slope_capture(void);

#endif // SLOPE_H
