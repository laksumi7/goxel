/* Goxel 3D voxels editor
 *
 * AMFM #541: the equipment navigator — persistent, geometry-linked semantics
 * for the ship being edited (muzzle, exhaust, pivot), proposed by a worker
 * that reads the live selection, confirmed by the Owner in the editor.
 *
 * Design rules (from the issue):
 *  - records are anchored to geometry, never to copied coordinates: a shape
 *    layer marker follows the layer's own shape parameters, a cell marker
 *    follows the cells it was made from.  A rigid whole-part move keeps
 *    markers aligned automatically, and an edit that invalidates a meaning
 *    flips the record to Needs Review instead of a false green marker;
 *  - the editable .gox stays the single source of geometry: semantics live
 *    in a record file written next to the document, pinned by the document's
 *    SHA-256, so "the two files" are exactly one gox + one record pair and a
 *    mismatch is detectable everywhere (editor, exporter, game);
 *  - semantic state is not in the undo history: undo rewinds geometry and
 *    revalidation decides what the rewound geometry means.
 */

#ifndef NAV_H
#define NAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct layer layer_t;
typedef struct image image_t;

// What the marker means.  Muzzle/exhaust/pivot map to the canonical amfm.ship
// v1 marker kinds; hinge is the pivot of a moving sub-assembly.  rcs is a
// thruster attachment point (Owner #541 session request): a cell or shape end
// plus the thrust direction, tagged exactly like muzzle/exhaust.
enum {
    NAV_KIND_MUZZLE = 0,
    NAV_KIND_EXHAUST,
    NAV_KIND_PIVOT,
    NAV_KIND_HINGE,
    NAV_KIND_RCS,
    NAV_KIND_COUNT,
};

// How a record knows where it is.
enum {
    NAV_AT_SHAPE_END = 0,   // one end face of the layer's shape modifier
    NAV_AT_CELL,            // a specific cell on the owning layer
    NAV_AT_COUNT,
};

enum {
    NAV_STATUS_DRAFT = 0,   // proposed, waiting for the Owner
    NAV_STATUS_CONFIRMED,   // the Owner accepted it
    NAV_STATUS_REVIEW,      // the geometry moved away from the meaning
    NAV_STATUS_COUNT,
};

typedef struct nav_record {
    struct nav_record *next, *prev;
    char     id[64];        // unique in the registry (and in the sidecar)
    char     group[64];     // owning equipment group (display/export only)
    char     label[96];     // free text the Owner can read
    int      kind;          // NAV_KIND_*
    int      at;            // NAV_AT_*
    int      layer_id;      // owning layer; resolved by id, never by name
    // NAV_AT_SHAPE_END: which end of the shape axis, relative to the axis
    // itself (+1 = the end the axis points at), so a rigid move keeps it.
    int      shape_sign;
    // NAV_AT_CELL: the anchor cell, with the committed snapshot the planned
    // replays start from.
    int      cell[3];
    int      cell0[3];
    // Where the marker points: a unit vector in world axes at creation time,
    // re-mapped by exactly the matrices the cells move with.
    float    dir[3];
    float    dir0[3];
    int      status;
    char     reason[80];    // empty unless status == needs_review
    uint64_t seq;
    // PM review #541 (4): the semantic attachment captured when the record
    // was tagged.  A legitimate rigid move keeps every one of these; re-
    // authoring the surface (kind, run axis, caps, anchor, rise, region
    // size) changes at least one and forces a review instead of letting a
    // green marker silently denote a different physical muzzle.
    int      fp_kind;       // slope kind at tag time; -1 = not a shape record
    int      fp_dir;        // signed run axis (SLOPE_DIR_*)
    int      fp_closed;
    int      fp_anchor;
    float    fp_rise;
    int      fp_ext[3];     // region extent per axis, inclusive cell counts
    // PM review #541 (6): the sidecar did not pin the gox it was adopted
    // with, but the records are kept for an explicit Owner decision.
    bool     pin_mismatch;
    // Internal: this record's anchor is inside the mask of a transform in
    // flight (drag replay).
    bool     _moving;
} nav_record_t;

// Registry -------------------------------------------------------------------

int nav_count(void);
nav_record_t *nav_records(void);
nav_record_t *nav_find(const char *id);
// Create a DRAFT in the registry, or NULL on illegal arguments; `err` gets
// the reason.  `token` selects the semantics: for shape-end records which end
// of the shape axis (a world axis vector like "+Y" = {0,1,0}); for cell
// records the marker direction itself.  `cell_override` (rev9) anchors a cell
// record at an exact cell the Owner cannot box-select (e.g. a pivot buried
// inside the body); the cell must still be occupied on the active layer.
nav_record_t *nav_tag(const char *id, int kind, int at, const int token[3],
                      const int cell_override[3],
                      const char *group, const char *label, char *err,
                      size_t err_size);
// Accept the current geometry as the meaning.  PM review #541 (3): this is a
// publication gate, not a button — it validates the *current* authored
// geometry and refuses a record whose meaning has been invalidated (those
// need an explicit untag + tag re-anchor).  A record held in review only
// because the sidecar did not pin the gox (pin_mismatch) may be confirmed:
// that click is the Owner's explicit acceptance of the changed document.
bool nav_confirm(nav_record_t *rec, char *err, size_t err_size);
void nav_remove(nav_record_t *rec);
const char *nav_kind_name(int kind);
const char *nav_status_name(int status);

// PM review #541 (7): the registry revision.  Bumped by every mutation of
// the registry itself (tag, confirm, remove, adopt, reset), so a worker can
// carry it as `nbase=` next to `base=` and cannot silently overwrite another
// worker's semantic write even though the document revision did not move.
uint64_t nav_revision(void);

// The world-space meaning of a record right now; either output may be NULL.
bool nav_ring(const nav_record_t *rec, float out_pos[3], float out_dir[3]);
// The same, for a hypothetical shape_sign on the record: how the bridge
// shows both candidate ends with exactly the authority that places the
// marker (PM review #541 (1): position and direction must agree everywhere).
bool nav_shape_end(const nav_record_t *rec, int sign, float out_pos[3],
                   float out_dir[3]);

// The owning layer of a record id, resolved by id.
layer_t *nav_layer(int id);

// The shape axis of a slope modifier as a world unit vector.
void nav_dir_vec(int dir, float out[3]);

// Per-frame bookkeeping: adopts a re-opened document's sidecar when the
// newly opened gox still matches the pinned SHA, resets otherwise.
void nav_check_document(void);

// Re-validate every record against the live document.  Called every frame.
void nav_revalidate(void);

// Settle the registry for whatever document is currently open: adopt the
// sidecar when there is one — a matching pin yields the confirmed records, a
// mismatch keeps them as needs_review with the pin_mismatch flag (PM review
// #541 (6): a mismatch is surfaced, never silently cleared).  Only a document
// with no sidecar at all starts from an empty registry.  The editor calls
// this every frame; the headless self-test calls it at document swaps.
void nav_check_document(void);

// Transform side hooks -------------------------------------------------------

// A one-move replay of a selection transform (image.c): markers anchored in
// the mask move with the same matrix the cells see, recomputed from their
// committed snapshot each apply so a drag cannot accumulate.
void nav_transform_begin(void);
void nav_transform_mark(layer_t *layer);
void nav_move_snap_layer(layer_t *layer, const float mat[4][4]);
void nav_transform_end(void);
// A per-frame delta transform (tools/move.c, mirror filter): apply the delta
// and re-base the snapshot.
void nav_move_delta_layer(layer_t *layer, const float mat[4][4]);
// The mirror filter reflects cells inside one axis span; nav records reflect
// the same way (only anchors actually inside that span).
void nav_move_mirror_layer(layer_t *layer, int axis, const int aabb[2][3]);

// Announced by goxel.c when a document was opened: remembers the path so the
// sidecar can be adopted when the gox still matches its pin.
void nav_notify_opened(const char *path);

// Persistence ----------------------------------------------------------------

// Save the gox and write the record sidecar in one step, only for a coherent
// package.  Returns 0, or a negative refusal (the buffer says why).
int nav_save(const char *path, char *err, size_t err_size);

// PM rev4 (P0-2): magic and size are not content.  A candidate gox is only
// publishable when it loads back as the same model: same layer count and
// same occupied-cell count as the live document.  Returns true on success;
// `why` explains otherwise.  Public so the self-test can feed it a
// deliberately broken file.
bool nav_gox_validate(const char *path, char *why, size_t why_size);
// Adopt the sidecar paired with `gox_path` (the worker re-adopting after a
// reopen, or the automatic path on a matching gox).  Marks everything needs_
// review when the gox does not match the pin.
int nav_adopt(const char *gox_path);
void nav_sidecar_path(const char *gox_path, char *buf, size_t buf_size);

// SHA-256 pinning helper.
void nav_sha256_data(const void *data, size_t len, uint8_t out[32]);
int  nav_sha256_file(const char *path, uint8_t out[32]);
void nav_sha256_hex(const uint8_t in[32], char out[65]);
// Known-answer check used by the self-test.
bool nav_sha256_known_answer(void);

// UI -------------------------------------------------------------------------

void nav_gui(void);
void nav_render(void);

// Worker pointer overlay (#541 A2): attributed session-only cells set via
// the lab bridge.  Never saved, never in undo, cleared on document switch.
void nav_pointer_add(int x, int y, int z);
void nav_pointer_del(int x, int y, int z);
void nav_pointer_clear(void);
int  nav_pointer_count(void);
int  nav_pointer_get(int i, int out[3]);

// Headless verification of every #541 P0 claim (fixtures/newed document),
// returning 0 or the number of failed checks.  Driven by GOXEL_NAV_TEST.
int nav_test_run(const char *fixture_path, const char *out_dir);

#endif // NAV_H
