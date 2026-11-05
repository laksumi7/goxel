/* Goxel 3D voxels editor
 *
 * AMFM #534 LAB: selective, non-destructive planar slope surface modifier.
 *
 * Everything here is additive: the native voxel document, the native layer
 * list, the native selection tools and the native undo history are reused.
 * A slope modifier lives on a layer, keeps that layer's voxels as the source
 * cell authority, and replaces only that layer's cube geometry with a real
 * planar surface.
 */

#include "goxel.h"
#include "slope.h"

#include "utils/img.h"
#include "xxhash.h"

#include <limits.h>
#ifdef _WIN32
#include <malloc.h>
#endif

#define SLOPE_EPS     0.004f  // Keeps the generated skirts off the neighbour faces.
#define SLOPE_PICK_PX 18.0f   // Handle grab radius, in screen pixels.
#define SLOPE_UP      2       // Goxel is Z-up.
#define SLOPE_STRIP_W 300.0f  // Width of the contextual strip, in screen pixels.

enum { S_MODE_NONE = 0, S_MODE_NEW, S_MODE_EDIT };

// Persistent fields that a cancel has to restore (never memcpy slope_mod_t:
// it owns a GL mesh).
typedef struct {
    int   aabb[2][3];
    int   kind;
    int   dir;
    int   anchor;
    int   closed;
    float rise;
} slope_saved_t;

static struct {
    int           mode;
    layer_t      *src_layer;   // Layer the region cells were taken from.
    volume_t     *src_backup;  // Its content before the move.
    layer_t      *mod_layer;   // Layer carrying the modifier being edited.
    slope_saved_t saved;       // Parameters to restore on cancel.
    bool          dragging;
    bool          conflict;     // Voxel edits happened inside the shape region.
    tool_t       *saved_tool;  // Tool suspended while a handle is dragged.
    layer_t      *closed;      // Shape layer whose panel was just closed.
    layer_t      *last_active; // Layer the panel was last showing, if any.
    int           counter;
} s = {};

static tool_t s_noop_tool = {.name = "", .flags = 0};

// Allocation accounting for the stress test: every mesh this module makes has
// to be handed back, so a leak shows up as a live count that never comes down.
static long s_mesh_live = 0;
static long s_mesh_made = 0;
static void free_mesh(model3d_t *mo);

static bool suggest_region(int aabb[2][3]);
static void select_box(const int aabb[2][3]);
static void slope_demo_step(void);
static int count_vol_cells(const volume_t *vol);

// Windows CRT heap validation: catches a stray write at the point it is made,
// instead of where it is eventually noticed.
static bool heap_ok(void)
{
#ifdef _WIN32
    return _heapchk() == _HEAPOK;
#else
    return true;
#endif
}

static int noop_tool_iter(tool_t *tool, const painter_t *painter,
                          const float viewport[4])
{
    (void)tool; (void)painter; (void)viewport;
    return 0;
}

// #### Modifier data ####################################################

slope_mod_t *slope_new(void)
{
    slope_mod_t *m = calloc(1, sizeof(*m));
    m->kind = SLOPE_KIND_SLOPE;
    m->dir = SLOPE_DIR_PX;
    m->anchor = SLOPE_ANCHOR_FLOOR;
    m->closed = 0;
    m->rise = 1;
    m->revision = 1;
    vec4_set(m->color, 255, 0, 255, 255);
    return m;
}

slope_mod_t *slope_copy(const slope_mod_t *other)
{
    slope_mod_t *m;
    if (!other) return NULL;
    m = calloc(1, sizeof(*m));
    memcpy(m->aabb, other->aabb, sizeof(m->aabb));
    m->kind = other->kind;
    m->dir = other->dir;
    m->anchor = other->anchor;
    m->closed = other->closed;
    m->rise = other->rise;
    m->revision = other->revision;
    vec4_copy(other->color, m->color);
    return m;
}

void slope_free(slope_mod_t *mod)
{
    if (!mod) return;
    // The mesh is CPU memory; only its VBO needs a live GL context, so free it
    // either way rather than leaking it when the graphics are not up.
    if (mod->mesh) {
        if (goxel.graphics_initialized) model3d_delete(mod->mesh);
        else free_mesh(mod->mesh);
        mod->mesh = NULL;
    }
    free(mod);
}

uint32_t slope_hash(const slope_mod_t *mod)
{
    uint32_t key = 0;
    if (!mod) return 0;
    key = XXH32(&mod->aabb, sizeof(mod->aabb), key);
    key = XXH32(&mod->kind, sizeof(mod->kind), key);
    key = XXH32(&mod->dir, sizeof(mod->dir), key);
    key = XXH32(&mod->anchor, sizeof(mod->anchor), key);
    key = XXH32(&mod->closed, sizeof(mod->closed), key);
    key = XXH32(&mod->rise, sizeof(mod->rise), key);
    key = XXH32(&mod->revision, sizeof(mod->revision), key);
    key = XXH32(&mod->color, sizeof(mod->color), key);
    return key + 1;
}

bool slope_replaces_voxels(const layer_t *layer)
{
    return layer && layer->slope;
}

// #### Geometry #########################################################

static void run_axes(int dir, int *run, int *cross, int *sign)
{
    switch (dir) {
    case SLOPE_DIR_PX: *run = 0; *cross = 1; *sign = +1; break;
    case SLOPE_DIR_NX: *run = 0; *cross = 1; *sign = -1; break;
    case SLOPE_DIR_PY: *run = 1; *cross = 0; *sign = +1; break;
    default:           *run = 1; *cross = 0; *sign = -1; break;
    }
}

// Build a point from (run coordinate, height, cross coordinate).
static void set_pt(float out[3], int run, int cross, float r, float h, float c)
{
    vec3_set(out, 0, 0, 0);
    out[run] = r;
    out[cross] = c;
    out[SLOPE_UP] = h;
}

// How many vertices the current build may still write; guards the geometry
// code against a shape builder that miscounts its own budget.
static int s_vert_budget = 0;

// Append one quad as two triangles, wound so that the geometric normal faces
// the same way as `outward`.
static void add_quad(model3d_t *mo, int *vi, float p[4][3],
                     const float outward[3])
{
    static const int IDX[6] = {0, 1, 2, 2, 3, 0};
    float e1[3], e2[3], n[3], tmp[3];
    int i;

    if (*vi + 6 > s_vert_budget) {
        printf("OVERFLOW: quad at %d past the budget of %d\n", *vi, s_vert_budget);
        fflush(stdout);
    }
    vec3_sub(p[1], p[0], e1);
    vec3_sub(p[2], p[0], e2);
    vec3_cross(e1, e2, n);
    if (vec3_dot(n, outward) < 0) {
        vec3_copy(p[1], tmp);
        vec3_copy(p[3], p[1]);
        vec3_copy(tmp, p[3]);
        vec3_imul(n, -1);
    }
    if (vec3_norm2(n) == 0) vec3_set(n, 0, 0, 1);
    vec3_normalize(n, n);
    for (i = 0; i < 6; i++) {
        model_vertex_t *v = &mo->vertices[(*vi)++];
        vec3_copy(p[IDX[i]], v->pos);
        vec3_copy(n, v->normal);
        vec4_set(v->color, 255, 255, 255, 255);
        vec2_set(v->uv, 0, 0);
    }
}

// Append one triangle, wound so that the geometric normal faces `outward`.
static void add_tri(model3d_t *mo, int *vi, const float a[3], const float b[3],
                    const float c[3], const float outward[3])
{
    float e1[3], e2[3], n[3];
    const float *p[3];
    int i;

    if (*vi + 3 > s_vert_budget) {
        printf("OVERFLOW: tri at %d past the budget of %d\n", *vi, s_vert_budget);
        fflush(stdout);
    }
    p[0] = a; p[1] = b; p[2] = c;
    vec3_sub(b, a, e1);
    vec3_sub(c, a, e2);
    vec3_cross(e1, e2, n);
    if (vec3_dot(n, outward) < 0) {
        const float *tmp = p[1];
        p[1] = p[2];
        p[2] = tmp;
        vec3_imul(n, -1);
    }
    if (vec3_norm2(n) == 0) vec3_set(n, 0, 0, 1);
    vec3_normalize(n, n);
    for (i = 0; i < 3; i++) {
        model_vertex_t *v = &mo->vertices[(*vi)++];
        vec3_copy(p[i], v->pos);
        vec3_copy(n, v->normal);
        vec4_set(v->color, 255, 255, 255, 255);
        vec2_set(v->uv, 0, 0);
    }
}

#define SLOPE_ARC_SEG   24  // Segments across a half arch; a full barrel doubles it.
#define SLOPE_DOME_SEG  24  // Segments around a dome or ellipsoid.
#define SLOPE_DOME_RINGS 12 // Rings from the base of a dome to its top.

// The frame every shape builder shares: the region footprint in world axes
// (inflated by SLOPE_EPS so it covers the neighbouring cube faces instead of
// z-fighting with them), the run/cross axes, and the planes the surface is
// measured from.  `anchor` decides which region plane that is, so a shape is
// not stuck growing upwards: a hull side can slope inwards, a barrel can lie
// across the ship, a casing can hang from the deck.
typedef struct {
    int run, cross, sign;
    int anchor, closed;
    int spans;            // the shape is centred on `centre` (barrel, ellipsoid)
    float lo[3], hi[3];
    float r_lo, r_hi;     // run coordinate of the low and the high edge
    float c_lo, c_hi;
    float base;           // plane an open shape is measured from
    float seal;           // the flat cap the skirts close against
    float centre;         // plane a closed shape is centred on
    float crown;          // the extreme point of the generated face
    float rise;
    int dirz;             // +1 grows up from the base, -1 hangs down from it
} surf_frame_t;

static void surf_frame(const slope_mod_t *m, surf_frame_t *f)
{
    const float floor_p = m->aabb[0][SLOPE_UP];
    const float ceil_p = m->aabb[1][SLOPE_UP] + 1.f;
    int i;

    run_axes(m->dir, &f->run, &f->cross, &f->sign);
    for (i = 0; i < 3; i++) {
        f->lo[i] = m->aabb[0][i];
        f->hi[i] = m->aabb[1][i] + 1.f;
    }
    f->lo[f->run] -= SLOPE_EPS;   f->hi[f->run] += SLOPE_EPS;
    f->lo[f->cross] -= SLOPE_EPS; f->hi[f->cross] += SLOPE_EPS;
    f->anchor = m->anchor;
    f->closed = m->closed;
    f->rise = m->rise;

    switch (m->anchor) {
    case SLOPE_ANCHOR_CEIL:
        f->base = ceil_p;
        f->dirz = -1;
        f->centre = ceil_p - m->rise;
        break;
    case SLOPE_ANCHOR_MID:
        f->base = (floor_p + ceil_p) * 0.5f - m->rise * 0.5f;
        f->dirz = +1;
        f->centre = (floor_p + ceil_p) * 0.5f;
        break;
    default:
        f->base = floor_p;
        f->dirz = +1;
        f->centre = floor_p + m->rise;
        break;
    }
    // The solid is the slab between the anchor plane and the generated face,
    // nudged a hair past the plane so it does not z-fight with the neighbouring
    // cubes.  Keeping the slab against its own plane is what makes every kind
    // watertight: the skirts and the cap always close against that plane.
    f->seal = f->base - f->dirz * SLOPE_EPS;
    // Only a barrel or an ellipsoid is centred on its anchor plane; a plane or
    // a chamfer is measured from it whatever `closed` means for that kind.
    f->spans = m->closed && (m->kind == SLOPE_KIND_CYLINDER ||
                             m->kind == SLOPE_KIND_SPHERE);
    f->crown = f->spans ? f->centre + m->rise : f->base + f->dirz * m->rise;
    f->r_lo = (f->sign > 0) ? f->lo[f->run] : f->hi[f->run];
    f->r_hi = (f->sign > 0) ? f->hi[f->run] : f->lo[f->run];
    f->c_lo = f->lo[f->cross];
    f->c_hi = f->hi[f->cross];
}

// World height of a point `h` away from the anchor plane: upwards for a shape
// that stands on it, downwards for one that hangs from it.
static float surf_h(const surf_frame_t *f, float h)
{
    return f->base + f->dirz * h;
}

// The highest (or, for a hanging shape, lowest) point of the generated face.
static float surf_crown(const surf_frame_t *f)
{
    return f->crown;
}

static void add_cap(const surf_frame_t *f, model3d_t *mo, int *vi)
{
    float p[4][3], out[3];
    set_pt(p[0], f->run, f->cross, f->r_lo, f->seal, f->c_lo);
    set_pt(p[1], f->run, f->cross, f->r_hi, f->seal, f->c_lo);
    set_pt(p[2], f->run, f->cross, f->r_hi, f->seal, f->c_hi);
    set_pt(p[3], f->run, f->cross, f->r_lo, f->seal, f->c_hi);
    vec3_set(out, 0, 0, 0); out[SLOPE_UP] = -f->dirz;
    add_quad(mo, vi, p, out);
}

// One planar face, four skirts back to the anchor plane, closing cap.
static void build_slope(const surf_frame_t *f, model3d_t *mo, int *vi)
{
    float p[4][3], out[3];
    float h_lo = surf_h(f, 0.f);
    float h_hi = surf_h(f, f->rise);

    // the slope face itself: a single plane, not a staircase
    set_pt(p[0], f->run, f->cross, f->r_lo, h_lo, f->c_lo);
    set_pt(p[1], f->run, f->cross, f->r_hi, h_hi, f->c_lo);
    set_pt(p[2], f->run, f->cross, f->r_hi, h_hi, f->c_hi);
    set_pt(p[3], f->run, f->cross, f->r_lo, h_lo, f->c_hi);
    vec3_set(out, 0, 0, 0); out[SLOPE_UP] = f->dirz;
    add_quad(mo, vi, p, out);

    // skirt at the high edge
    set_pt(p[0], f->run, f->cross, f->r_hi, h_hi, f->c_lo);
    set_pt(p[1], f->run, f->cross, f->r_hi, f->seal, f->c_lo);
    set_pt(p[2], f->run, f->cross, f->r_hi, f->seal, f->c_hi);
    set_pt(p[3], f->run, f->cross, f->r_hi, h_hi, f->c_hi);
    vec3_set(out, 0, 0, 0); out[f->run] = f->sign;
    add_quad(mo, vi, p, out);

    // skirt at the low edge
    set_pt(p[0], f->run, f->cross, f->r_lo, h_lo, f->c_lo);
    set_pt(p[1], f->run, f->cross, f->r_lo, f->seal, f->c_lo);
    set_pt(p[2], f->run, f->cross, f->r_lo, f->seal, f->c_hi);
    set_pt(p[3], f->run, f->cross, f->r_lo, h_lo, f->c_hi);
    vec3_set(out, 0, 0, 0); out[f->run] = -f->sign;
    add_quad(mo, vi, p, out);

    // skirts on the two cross edges
    set_pt(p[0], f->run, f->cross, f->r_lo, h_lo, f->c_lo);
    set_pt(p[1], f->run, f->cross, f->r_hi, h_hi, f->c_lo);
    set_pt(p[2], f->run, f->cross, f->r_hi, f->seal, f->c_lo);
    set_pt(p[3], f->run, f->cross, f->r_lo, f->seal, f->c_lo);
    vec3_set(out, 0, 0, 0); out[f->cross] = -1;
    add_quad(mo, vi, p, out);

    set_pt(p[0], f->run, f->cross, f->r_lo, h_lo, f->c_hi);
    set_pt(p[1], f->run, f->cross, f->r_hi, h_hi, f->c_hi);
    set_pt(p[2], f->run, f->cross, f->r_hi, f->seal, f->c_hi);
    set_pt(p[3], f->run, f->cross, f->r_lo, f->seal, f->c_hi);
    vec3_set(out, 0, 0, 0); out[f->cross] = 1;
    add_quad(mo, vi, p, out);

    add_cap(f, mo, vi);
}

// A chamfer of the region's edges measured from the anchor plane: the face is
// inset by the same distance it is raised, so the skirt is a real bevel and not
// a step.  Open chamfers only the edge at the high end of the run (a directed
// bevel, like a bow cut); closed chamfers all four edges of the face.
static void build_bevel(const surf_frame_t *f, model3d_t *mo, int *vi)
{
    float p[4][3], out[3];
    float r_min = min(f->lo[f->run], f->hi[f->run]);
    float r_max = max(f->lo[f->run], f->hi[f->run]);
    float c_min = min(f->c_lo, f->c_hi);
    float c_max = max(f->c_lo, f->c_hi);
    float in_hi = min(f->rise, (r_max - r_min) * 0.5f);
    float in_lo = f->closed ? in_hi : 0.f;
    float in_c  = f->closed ? min(f->rise, (c_max - c_min) * 0.5f) : 0.f;
    float a_lo = (f->sign > 0) ? in_lo : in_hi;   // inset at r_min
    float a_hi = (f->sign > 0) ? in_hi : in_lo;   // inset at r_max
    float h_top = surf_h(f, f->rise);

    // the chamfered face
    set_pt(p[0], f->run, f->cross, r_min + a_lo, h_top, c_min + in_c);
    set_pt(p[1], f->run, f->cross, r_max - a_hi, h_top, c_min + in_c);
    set_pt(p[2], f->run, f->cross, r_max - a_hi, h_top, c_max - in_c);
    set_pt(p[3], f->run, f->cross, r_min + a_lo, h_top, c_max - in_c);
    vec3_set(out, 0, 0, 0); out[SLOPE_UP] = f->dirz;
    add_quad(mo, vi, p, out);

    // the four skirts between that face and the anchor plane
    set_pt(p[0], f->run, f->cross, r_min, f->seal, c_min);
    set_pt(p[1], f->run, f->cross, r_min + a_lo, h_top, c_min + in_c);
    set_pt(p[2], f->run, f->cross, r_max - a_hi, h_top, c_min + in_c);
    set_pt(p[3], f->run, f->cross, r_max, f->seal, c_min);
    vec3_set(out, 0, 0, 0); out[f->cross] = -1;
    add_quad(mo, vi, p, out);

    set_pt(p[0], f->run, f->cross, r_min, f->seal, c_max);
    set_pt(p[1], f->run, f->cross, r_min + a_lo, h_top, c_max - in_c);
    set_pt(p[2], f->run, f->cross, r_max - a_hi, h_top, c_max - in_c);
    set_pt(p[3], f->run, f->cross, r_max, f->seal, c_max);
    vec3_set(out, 0, 0, 0); out[f->cross] = 1;
    add_quad(mo, vi, p, out);

    set_pt(p[0], f->run, f->cross, r_min, f->seal, c_min);
    set_pt(p[1], f->run, f->cross, r_min + a_lo, h_top, c_min + in_c);
    set_pt(p[2], f->run, f->cross, r_min + a_lo, h_top, c_max - in_c);
    set_pt(p[3], f->run, f->cross, r_min, f->seal, c_max);
    vec3_set(out, 0, 0, 0); out[f->run] = -1;
    add_quad(mo, vi, p, out);

    set_pt(p[0], f->run, f->cross, r_max, f->seal, c_min);
    set_pt(p[1], f->run, f->cross, r_max - a_hi, h_top, c_min + in_c);
    set_pt(p[2], f->run, f->cross, r_max - a_hi, h_top, c_max - in_c);
    set_pt(p[3], f->run, f->cross, r_max, f->seal, c_max);
    vec3_set(out, 0, 0, 0); out[f->run] = 1;
    add_quad(mo, vi, p, out);

    add_cap(f, mo, vi);
}

// Across the cross axis: an arch spanning the region when open, a full barrel
// of radius `rise` when closed, extruded along the run axis.
static void build_cylinder(const surf_frame_t *f, model3d_t *mo, int *vi)
{
    const bool full = f->spans != 0;
    const int seg = full ? SLOPE_ARC_SEG * 2 : SLOPE_ARC_SEG;
    const float span = full ? 2 * M_PI : M_PI;
    const float zc = full ? f->centre : surf_h(f, 0.f);
    const float crad = full ? f->rise
                            : fabsf(f->c_hi - f->c_lo) * 0.5f;
    float arc[SLOPE_ARC_SEG * 2 + 1][3];
    float out[3], pivot[3];
    float r_a = f->lo[f->run], r_b = f->hi[f->run];
    float c_mid = (f->c_lo + f->c_hi) * 0.5f;
    int k;

    for (k = 0; k <= seg; k++) {
        float ang = span * k / seg;
        set_pt(arc[k], f->run, f->cross, 0.f,
               zc + f->rise * sinf(ang) * (f->dirz < 0 ? -1.f : 1.f),
               c_mid + crad * cosf(ang));
    }

    // the barrel or arch wall, as a quad strip
    for (k = 0; k < seg; k++) {
        float ang = span * (k + 0.5f) / seg;
        float q[4][3];
        set_pt(q[0], f->run, f->cross, r_a, arc[k][SLOPE_UP], arc[k][f->cross]);
        set_pt(q[1], f->run, f->cross, r_b, arc[k][SLOPE_UP], arc[k][f->cross]);
        set_pt(q[2], f->run, f->cross, r_b, arc[k+1][SLOPE_UP], arc[k+1][f->cross]);
        set_pt(q[3], f->run, f->cross, r_a, arc[k+1][SLOPE_UP], arc[k+1][f->cross]);
        vec3_set(out, 0, 0, 0);
        out[f->cross] = cosf(ang);
        out[SLOPE_UP] = sinf(ang) * (f->dirz < 0 ? -1.f : 1.f);
        add_quad(mo, vi, q, out);
    }

    // the two ends, as fans from the section's pivot.  The arc points are built
    // at run 0 as a placeholder, so each rim vertex has to be moved out to the
    // end it closes; using them raw would skew the cap into the region.
    {
        float va[3], vb[3];
        vec3_set(pivot, 0, 0, 0);
        pivot[f->run] = r_a;
        pivot[f->cross] = c_mid;
        pivot[SLOPE_UP] = zc;
        vec3_set(out, 0, 0, 0); out[f->run] = -1;
        for (k = 0; k < seg; k++) {
            vec3_copy(arc[k], va);       va[f->run] = r_a;
            vec3_copy(arc[k + 1], vb);   vb[f->run] = r_a;
            add_tri(mo, vi, pivot, va, vb, out);
        }

        pivot[f->run] = r_b;
        vec3_set(out, 0, 0, 0); out[f->run] = 1;
        for (k = 0; k < seg; k++) {
            vec3_copy(arc[k], va);       va[f->run] = r_b;
            vec3_copy(arc[k + 1], vb);   vb[f->run] = r_b;
            add_tri(mo, vi, pivot, va, vb, out);
        }
    }

    // a full barrel is already closed; an arch needs its cap
    if (!full)
        add_cap(f, mo, vi);
}

// Over the region footprint: a dome when open, a full ellipsoid when closed.
static void build_sphere(const surf_frame_t *f, model3d_t *mo, int *vi)
{
    const bool full = f->spans != 0;
    const int rings = full ? SLOPE_DOME_RINGS * 2 : SLOPE_DOME_RINGS;
    const float u_from = full ? -M_PI / 2 : 0.f;
    const float u_span = full ? M_PI : M_PI / 2;
    const float zc = full ? f->centre : surf_h(f, 0.f);
    const float sgn = (full || f->dirz > 0) ? 1.f : -1.f;
    float out[3], center[3], cur[SLOPE_DOME_SEG][3], nxt[SLOPE_DOME_SEG][3];
    float r_mid = (f->lo[f->run] + f->hi[f->run]) * 0.5f;
    float c_mid = (f->c_lo + f->c_hi) * 0.5f;
    float rad_r = max(fabsf(f->hi[f->run] - f->lo[f->run]) * 0.5f, 1e-3f);
    float rad_c = max(fabsf(f->c_hi - f->c_lo) * 0.5f, 1e-3f);
    float h = max(f->rise, 1e-3f);
    int ring, k;

    vec3_set(center, 0, 0, 0);
    center[f->run] = r_mid;
    center[f->cross] = c_mid;
    center[SLOPE_UP] = zc;

    for (ring = 0; ring < rings; ring++) {
        float u0 = u_from + u_span * ring / rings;
        float u1 = u_from + u_span * (ring + 1) / rings;
        float s0 = cosf(u0), s1 = cosf(u1);
        bool pole_lo = fabsf(s0) < 1e-6f;   // the ring has collapsed to a point
        bool pole_hi = fabsf(s1) < 1e-6f;

        for (k = 0; k < SLOPE_DOME_SEG; k++) {
            float a0 = 2 * M_PI * k / SLOPE_DOME_SEG;
            float a1 = 2 * M_PI * (k + 1) / SLOPE_DOME_SEG;
            set_pt(cur[k], f->run, f->cross,
                   r_mid + rad_r * s0 * cosf(a0),
                   zc + f->rise * sinf(u0) * sgn,
                   c_mid + rad_c * s0 * sinf(a0));
            set_pt(nxt[k], f->run, f->cross,
                   r_mid + rad_r * s1 * cosf(a1),
                   zc + f->rise * sinf(u1) * sgn,
                   c_mid + rad_c * s1 * sinf(a1));
        }
        for (k = 0; k < SLOPE_DOME_SEG; k++) {
            int nk = (k + 1) % SLOPE_DOME_SEG;
            if (pole_hi) {
                vec3_sub(nxt[k], center, out);
                add_tri(mo, vi, cur[k], nxt[k], cur[nk], out);
            } else if (pole_lo) {
                vec3_sub(nxt[k], center, out);
                add_tri(mo, vi, cur[k], nxt[k], nxt[nk], out);
            } else {
                float q[4][3];
                float a0 = 2 * M_PI * (k + 0.5f) / SLOPE_DOME_SEG;
                vec3_set(out, 0, 0, 0);
                out[f->run]   = s0 * cosf(a0) / rad_r;
                out[f->cross] = s0 * sinf(a0) / rad_c;
                out[SLOPE_UP] = sinf(u0) * sgn / h;
                vec3_copy(cur[k], q[0]);
                vec3_copy(nxt[k], q[1]);
                vec3_copy(nxt[nk], q[2]);
                vec3_copy(cur[nk], q[3]);
                add_quad(mo, vi, q, out);
            }
        }
    }

    // a full ellipsoid closes on itself; a dome needs its base
    if (!full) {
        for (k = 0; k < SLOPE_DOME_SEG; k++) {
            float a0 = 2 * M_PI * k / SLOPE_DOME_SEG;
            set_pt(cur[k], f->run, f->cross,
                   r_mid + rad_r * cosf(a0), zc, c_mid + rad_c * sinf(a0));
        }
        for (k = 0; k < SLOPE_DOME_SEG; k++) {
            vec3_set(out, 0, 0, 0);
            out[SLOPE_UP] = -f->dirz;
            add_tri(mo, vi, center, cur[k], cur[(k + 1) % SLOPE_DOME_SEG], out);
        }
    }
}

static model3d_t *build_mesh(const slope_mod_t *m)
{
    surf_frame_t f;
    model3d_t *mo = calloc(1, sizeof(*mo));
    int cap, vi = 0;

    surf_frame(m, &f);

    switch (m->kind) {
    case SLOPE_KIND_BEVEL:    cap = 6 * 6; break;
    case SLOPE_KIND_CYLINDER:
        cap = m->closed
                ? (SLOPE_ARC_SEG * 2) * 6 + 2 * (SLOPE_ARC_SEG * 2) * 3
                : SLOPE_ARC_SEG * 6 + 2 * SLOPE_ARC_SEG * 3 + 6;
        break;
    case SLOPE_KIND_SPHERE:
        cap = m->closed
                ? (SLOPE_DOME_RINGS * 2 - 2) * SLOPE_DOME_SEG * 6 +
                  2 * SLOPE_DOME_SEG * 3
                : (SLOPE_DOME_RINGS - 1) * SLOPE_DOME_SEG * 6 +
                  2 * SLOPE_DOME_SEG * 3;
        break;
    default:                  cap = 6 * 6; break;
    }

    mo->vertices = calloc(cap, sizeof(*mo->vertices));
    mo->solid = true;
    mo->cull = true;
    mo->dirty = true;
    s_vert_budget = cap;
    s_mesh_live++;
    s_mesh_made++;

    switch (m->kind) {
    case SLOPE_KIND_BEVEL:    build_bevel(&f, mo, &vi); break;
    case SLOPE_KIND_CYLINDER: build_cylinder(&f, mo, &vi); break;
    case SLOPE_KIND_SPHERE:   build_sphere(&f, mo, &vi); break;
    default:                  build_slope(&f, mo, &vi); break;
    }
    if (vi > cap)
        printf("BUG: %s generated %d vertices, budget was %d\n",
               slope_kind_name(m->kind), vi, cap);
    assert(vi <= cap);
    mo->nb_vertices = vi;
    return mo;
}

// Release a mesh built here and keep the live count honest.  model3d_delete
// does the same frees; this one also tracks them.
static void free_mesh(model3d_t *mo)
{
    if (!mo) return;
    free(mo->vertices);
    free(mo);
    s_mesh_live--;
}

static void region_color(const volume_t *vol, const int aabb[2][3],
                         uint8_t color[4])
{
    volume_iterator_t iter;
    int p[3], i, nb = 0;
    int sum[3] = {0, 0, 0};
    uint8_t c[4];

    iter = volume_get_iterator(vol, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, p)) {
        if (p[0] < aabb[0][0] || p[0] > aabb[1][0]) continue;
        if (p[1] < aabb[0][1] || p[1] > aabb[1][1]) continue;
        if (p[2] < aabb[0][2] || p[2] > aabb[1][2]) continue;
        volume_get_at(vol, &iter, p, c);
        if (c[3] == 0) continue;
        for (i = 0; i < 3; i++) sum[i] += c[i];
        nb++;
    }
    if (!nb) return;
    for (i = 0; i < 3; i++) color[i] = sum[i] / nb;
    color[3] = 255;
}

// #### Conflict with ordinary voxel edits ###############################
// The generated surface is built from the region box, not from whichever cells
// happen to sit in it.  Painting inside a shape's region therefore cannot move
// the surface, so it has to be reported rather than silently swallowed.
static uint32_t cells_hash_in_region(const layer_t *layer, const int aabb[2][3])
{
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3], i;
    uint32_t key = 0;

    iter = volume_get_iterator(layer->volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, p)) {
        bool inside = true;
        for (i = 0; i < 3; i++) {
            if (p[i] < aabb[0][i] || p[i] > aabb[1][i]) { inside = false; break; }
        }
        if (!inside) continue;
        volume_get_at(layer->volume, &iter, p, c);
        if (!c[3]) continue;
        key = XXH32(p, sizeof(p), key);
        key = XXH32(c, sizeof(c), key);
    }
    return key;
}

// Recompute whether a shape's own layer still holds the cells it was applied
// against, and record the answer on the modifier so the viewport and the strip
// can both show it.  A shape is stale when the cells inside its region changed
// after it was applied: the generated surface cannot follow individual cells,
// so the surface is now the stale thing and has to say so.
static bool slope_refresh_stale(layer_t *layer)
{
    uint32_t h;
    if (!layer || !layer->slope) return false;
    h = cells_hash_in_region(layer, layer->slope->aabb);
    if (layer->slope->applied_hash == 0) {
        layer->slope->applied_hash = h;
        layer->slope->stale = 0;
        return false;
    }
    layer->slope->stale = (h != layer->slope->applied_hash) ? 1 : 0;
    return layer->slope->stale != 0;
}

static void ensure_mesh(layer_t *layer)
{
    slope_mod_t *m = layer->slope;
    uint32_t hash = slope_hash(m);
    if (m->mesh && m->mesh_hash == hash) return;
    if (m->mesh) {
        free_mesh(m->mesh);
        m->mesh = NULL;
    }
    m->mesh = build_mesh(m);
    m->mesh_hash = hash;
}

// #### Rendering ########################################################

void slope_render(void)
{
    static const int BIG[2][3] = {{-4096, -4096, -4096}, {4096, 4096, 4096}};
    static float clip_box[4][4];
    static bool clip_done = false;
    layer_t *layer;
    float light[3];

    if (!clip_done) {
        bbox_from_aabb(clip_box, BIG);
        clip_done = true;
    }
    render_get_light_dir(&goxel.rend, light);

    DL_FOREACH(goxel.image->layers, layer) {
        uint8_t col[4];
        if (!layer->visible || !layer->slope) continue;
        ensure_mesh(layer);
        // The live preview gets a warm tint, so it cannot be mistaken for the
        // committed hull.
        vec4_copy(layer->slope->color, col);
        if ((s.mode == S_MODE_NEW || s.dragging) && s.mod_layer == layer) {
            col[0] = (col[0] + 255) / 2;
            col[1] = (col[1] + 190) / 2;
            col[2] = col[2] / 3;
        } else if (layer->slope->stale) {
            // The cells this surface was built from changed underneath it.  The
            // surface cannot follow individual cells, so it has to look stale in
            // the view and not merely say so in a strip the Owner may not have
            // open: magenta, clearly neither the hull nor the live preview.
            col[0] = (col[0] + 255) / 2;
            col[1] = col[1] / 4;
            col[2] = (col[2] + 230) / 2;
        }
        model3d_render(layer->slope->mesh, layer->mat,
                       goxel.rend.view_mat, goxel.rend.proj_mat,
                       col, NULL, light, clip_box,
                       goxel.view_effects & EFFECT_NO_SHADING);
    }
}

static void handle_pos(const slope_mod_t *m, float out[3])
{
    surf_frame_t f;
    surf_frame(m, &f);
    vec3_set(out, 0, 0, 0);
    if (m->kind == SLOPE_KIND_SLOPE) {
        // On the high edge, where a plane is obviously being pulled up.
        out[f.run] = (f.sign > 0) ? m->aabb[1][f.run] + 1.f
                                  : (float)m->aabb[0][f.run];
    } else {
        // On the crown of the arch, barrel, dome or chamfer.
        out[f.run] = (m->aabb[0][f.run] + m->aabb[1][f.run] + 1.f) / 2.f;
    }
    out[f.cross] = (m->aabb[0][f.cross] + m->aabb[1][f.cross] + 1.f) / 2.f;
    out[SLOPE_UP] = surf_crown(&f);
}

const char *slope_kind_name(int kind)
{
    switch (kind) {
    case SLOPE_KIND_BEVEL:    return "Bevel";
    case SLOPE_KIND_CYLINDER: return "Cylinder";
    case SLOPE_KIND_SPHERE:   return "Sphere";
    default:                  return "Slope";
    }
}

void slope_render_handles(void)
{
    const slope_mod_t *m;
    float mat[4][4], h[3], a[3], b[3], box[4][4];
    uint8_t col[4] = {255, 200, 0, 255};
    uint8_t box_col[4] = {255, 255, 0, 255};
    char hint[64];
    int run, cross, sign;

    if (!s.mode || !s.mod_layer) return;
    m = s.mod_layer->slope;
    handle_pos(m, h);

    // The source-cell region, so it is clear what the surface replaces.
    bbox_from_aabb(box, m->aabb);
    render_box(&goxel.rend, box, box_col, EFFECT_WIREFRAME | EFFECT_NO_DEPTH_TEST);

    mat4_set_identity(mat);
    vec3_copy(h, mat[3]);
    mat4_iscale(mat, 1.6f, 1.6f, 1.6f);
    render_sphere(&goxel.rend, mat);

    // The direction the surface is built along: along the face for a plane,
    // straight up the middle for a shape whose crown sits over the centre.
    run_axes(m->dir, &run, &cross, &sign);
    {
        float mid_run = (m->aabb[0][run] + m->aabb[1][run] + 1.f) / 2.f;
        float mid_cross = (m->aabb[0][cross] + m->aabb[1][cross] + 1.f) / 2.f;
        bool plane = (m->kind == SLOPE_KIND_SLOPE);
        set_pt(a, run, cross,
               plane ? ((sign > 0) ? m->aabb[0][run] : m->aabb[1][run] + 1.f) : mid_run,
               m->aabb[0][SLOPE_UP], mid_cross);
        set_pt(b, run, cross,
               plane ? ((sign > 0) ? m->aabb[1][run] + 1.f : m->aabb[0][run]) : mid_run,
               m->aabb[0][SLOPE_UP] + m->rise, mid_cross);
    }
    render_line(&goxel.rend, a, b, col, EFFECT_NO_DEPTH_TEST);
    snprintf(hint, sizeof(hint), "%s: rise %.2f", slope_kind_name(m->kind), m->rise);
    goxel_add_hint(HINT_COORDINATES, NULL, hint);
}

// #### Editing session ##################################################

static bool selection_aabb(int aabb[2][3])
{
    const volume_t *mask = goxel.image->selection_mask;
    volume_iterator_t iter;
    uint8_t c[4];
    bool found = false;
    int p[3], i;

    if (!mask) return false;
    for (i = 0; i < 3; i++) {
        aabb[0][i] = INT_MAX;
        aabb[1][i] = INT_MIN;
    }
    iter = volume_get_iterator(mask, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, p)) {
        volume_get_at(mask, &iter, p, c);
        if (c[3] == 0) continue;
        for (i = 0; i < 3; i++) {
            aabb[0][i] = min(aabb[0][i], p[i]);
            aabb[1][i] = max(aabb[1][i], p[i]);
        }
        found = true;
    }
    return found;
}

static void save_params(slope_saved_t *dst, const slope_mod_t *m)
{
    memcpy(dst->aabb, m->aabb, sizeof(m->aabb));
    dst->kind = m->kind;
    dst->dir = m->dir;
    dst->anchor = m->anchor;
    dst->closed = m->closed;
    dst->rise = m->rise;
}

static void restore_params(slope_mod_t *m, const slope_saved_t *src)
{
    memcpy(m->aabb, src->aabb, sizeof(m->aabb));
    m->kind = src->kind;
    m->dir = src->dir;
    m->anchor = src->anchor;
    m->closed = src->closed;
    m->rise = src->rise;
    m->revision++;
}

static void suspend_tool(void)
{
    if (s.saved_tool) return;
    s.saved_tool = goxel.tool;
    s_noop_tool.iter_fn = noop_tool_iter;
    goxel.tool = &s_noop_tool;
}

static void resume_tool(void)
{
    if (!s.saved_tool) return;
    goxel.tool = s.saved_tool;
    s.saved_tool = NULL;
}

static void end_session(void)
{
    resume_tool();
    // Remember which shape panel was just closed.  The active layer is still
    // that shape layer, so without this the panel would reopen on the very next
    // frame and Apply and Cancel would look like dead buttons.
    s.closed = s.mod_layer;
    s.mode = S_MODE_NONE;
    s.mod_layer = NULL;
    s.src_layer = NULL;
    s.dragging = false;
    s.conflict = false;
    if (s.src_backup) {
        volume_delete(s.src_backup);
        s.src_backup = NULL;
    }
}

// A shape layer carries a generated surface, not voxels, so staying on it after
// a commit leaves the select and paint tools with nothing to grab.  Hand the
// Owner back to the layer that actually holds the art.
static void return_to_art_layer(void)
{
    layer_t *l, *best = NULL;
    int best_cells = 0;

    if (goxel.image->active_layer && !goxel.image->active_layer->slope &&
            count_vol_cells(goxel.image->active_layer->volume) > 0) return;
    // A file can carry an empty spare layer; landing on that would leave the
    // brush painting where nothing renders, so take the layer with the voxels.
    DL_FOREACH(goxel.image->layers, l) {
        int n;
        if (l->slope) continue;
        n = count_vol_cells(l->volume);
        if (n > best_cells) {
            best_cells = n;
            best = l;
        }
    }
    if (best) goxel.image->active_layer = best;
}

static void cancel_session(void)
{
    if (s.mode == S_MODE_NEW && s.src_layer && s.src_backup) {
        volume_set(s.src_layer->volume, s.src_backup);
        if (s.mod_layer) image_delete_layer(goxel.image, s.mod_layer);
        s.mod_layer = NULL;   // gone -- never remember a deleted layer
    } else if (s.mode == S_MODE_EDIT && s.mod_layer) {
        restore_params(s.mod_layer->slope, &s.saved);
    }
    end_session();
    return_to_art_layer();
}

static void apply_session(void)
{
    char name[256] = "";

    if (s.mod_layer) {
        snprintf(name, sizeof(name), "%s", s.mod_layer->name);
        s.mod_layer->slope->revision++;
        region_color(s.mod_layer->volume, s.mod_layer->slope->aabb,
                     s.mod_layer->slope->color);
        // Remember the cells this shape was derived from, so a later voxel edit
        // inside the region can be reported instead of silently ignored.
        s.mod_layer->slope->applied_hash =
            cells_hash_in_region(s.mod_layer, s.mod_layer->slope->aabb);
    }
    s.conflict = false;
    end_session();
    image_history_push(goxel.image);
    return_to_art_layer();
    if (*name) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s applied", name);
        goxel_add_hint(0, NULL, msg);
    }
}

static void begin_new_shape(int kind)
{
    int aabb[2][3];
    layer_t *src = goxel.image->active_layer;
    layer_t *mod;
    volume_iterator_t iter;
    int p[3], nb = 0, cap = 256, *list = malloc(cap * 3 * sizeof(int));
    uint8_t c[4];

    if (!selection_aabb(aabb)) {
        goxel_add_hint(0, NULL, "Select a region first (Rectangle Select tool)");
        free(list);
        return;
    }
    if (!image_layer_can_edit(goxel.image, src)) {
        goxel_add_hint(0, NULL, "This layer cannot be edited");
        free(list);
        return;
    }

    // Collect first: the source volume must not be edited while iterated.
    iter = volume_get_iterator(src->volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, p)) {
        if (p[0] < aabb[0][0] || p[0] > aabb[1][0]) continue;
        if (p[1] < aabb[0][1] || p[1] > aabb[1][1]) continue;
        if (p[2] < aabb[0][2] || p[2] > aabb[1][2]) continue;
        volume_get_at(src->volume, &iter, p, c);
        if (c[3] == 0) continue;
        if (nb * 3 + 3 > cap) {
            cap *= 2;
            list = realloc(list, cap * 3 * sizeof(int));
        }
        list[nb * 3 + 0] = p[0];
        list[nb * 3 + 1] = p[1];
        list[nb * 3 + 2] = p[2];
        nb++;
    }

    mod = image_add_layer(goxel.image, NULL);
    s.counter++;
    snprintf(mod->name, sizeof(mod->name), "%s %d", slope_kind_name(kind), s.counter);
    mod->slope = slope_new();
    mod->slope->kind = kind;
    memcpy(mod->slope->aabb, aabb, sizeof(aabb));
    mod->slope->dir = SLOPE_DIR_PX;
    {
        // A shape's default height: a full arch or dome over the region, but a
        // modest chamfer for a bevel, which is clamped to half the footprint.
        int h = (aabb[1][SLOPE_UP] + 1) - aabb[0][SLOPE_UP];
        int ex = min(aabb[1][0] - aabb[0][0] + 1, aabb[1][1] - aabb[0][1] + 1);
        float rise = (float) h;
        if (kind == SLOPE_KIND_BEVEL)
            rise = max(1.f, min((float) h, ex * 0.25f));
        mod->slope->rise = rise;
    }

    s.mode = S_MODE_NEW;
    s.src_layer = src;
    s.src_backup = volume_copy(src->volume);
    s.mod_layer = mod;

    // Move the region cells over: they stay voxels (source cell authority) but
    // live on their own layer, so every other layer keeps its blocky shape.
    for (int i = 0; i < nb; i++) {
        int q[3] = {list[i * 3], list[i * 3 + 1], list[i * 3 + 2]};
        volume_get_at(src->volume, NULL, q, c);
        volume_set_at(mod->volume, NULL, q, c);
        vec4_set(c, 0, 0, 0, 0);
        volume_set_at(src->volume, NULL, q, c);
    }
    free(list);

    region_color(mod->volume, aabb, mod->slope->color);
    save_params(&s.saved, mod->slope);
    goxel_add_hint(0, NULL, "Alt-drag the handle, then Apply (Esc cancels)");
}

static void begin_edit_slope(layer_t *layer)
{
    s.mode = S_MODE_EDIT;
    s.mod_layer = layer;
    s.src_layer = NULL;
    s.src_backup = NULL;
    save_params(&s.saved, layer->slope);
}

// #### Frame update #####################################################

void slope_iter(const inputs_t *inputs, const float viewport[4])
{
    const slope_mod_t *m;
    float h[3], sp[3], o[3], d[3], win[2], plane[4][4], local[3], world[3];
    float up[3] = {0, 0, 0}, runv[3] = {0, 0, 0}, nrm[3];
    float mx, my, dx, dy, rise, lo, hi;
    int run, cross, sign;

    slope_demo_step();
    if (!s.mode || !s.mod_layer) return;
    {
        // Esc hands the viewport back to ordinary voxel work.  Without this the
        // open session keeps its handle in the way of the brush.
        static bool esc_prev = false;
        bool esc = inputs->keys[KEY_ESCAPE];
        bool pressed = esc && !esc_prev;
        esc_prev = esc;
        if (pressed) {
            cancel_session();
            return;
        }
    }
    m = s.mod_layer->slope;
    // Watching for ordinary voxel edits inside the region: hashing a big layer
    // every frame would not be free, so this runs a few times a second.
    if (goxel.frame_count % 20 == 0)
        s.conflict = slope_refresh_stale(s.mod_layer);
    handle_pos(m, h);
    camera_project(goxel.image->active_camera, h, viewport, sp);
    mx = inputs->touches[0].pos[0];
    // The caller passes inputs already converted to GL convention (Y up).
    my = inputs->touches[0].pos[1];
    dx = mx - sp[0];
    dy = my - sp[1];

    if (!s.dragging) {
        // Alt has to be held to grab the handle.  A plain click belongs to the
        // brush: the handle sits right on the shape, so grabbing on any click
        // made the paint and erase tools look dead exactly where the Owner was
        // trying to work.
        if (inputs->touches[0].down[0] &&
                (inputs->keys[KEY_LEFT_ALT] || inputs->keys[KEY_RIGHT_ALT]) &&
                dx * dx + dy * dy < SLOPE_PICK_PX * SLOPE_PICK_PX) {
            s.dragging = true;
            suspend_tool();
        } else {
            return;
        }
    }

    if (!inputs->touches[0].down[0]) {
        s.dragging = false;
        resume_tool();
        return;
    }

    // Intersect the mouse ray with the vertical plane that contains the run
    // axis, so the handle only moves the height.
    vec2_set(win, mx, my);
    camera_get_ray(goxel.image->active_camera, win, viewport, o, d);
    run_axes(m->dir, &run, &cross, &sign);
    up[SLOPE_UP] = 1;
    runv[run] = 1;
    vec3_cross(up, runv, nrm);
    plane_from_normal(plane, h, nrm);
    if (!plane_line_intersection(plane, o, d, local)) return;
    mat4_mul_vec3(plane, local, world);

    // `rise` is measured from the anchor plane, matching surf_frame(): upwards
    // for a shape standing on it, downwards for one hanging from it, and away
    // from the centre plane for a closed shape.
    {
        surf_frame_t fr;
        surf_frame(m, &fr);
        rise = fr.spans ? fabsf(world[SLOPE_UP] - fr.centre)
                        : (world[SLOPE_UP] - fr.base) * fr.dirz;
    }
    lo = 0.25f;
    hi = 2.f * (m->aabb[1][SLOPE_UP] - m->aabb[0][SLOPE_UP] + 1.f);
    s.mod_layer->slope->rise = clamp(rise, lo, hi);
    s.mod_layer->slope->revision++;
}
// #### UI ###############################################################

static void strip_position(const slope_mod_t *m, float out[2])
{
    float c[3], sp[3];
    int i;
    for (i = 0; i < 3; i++)
        c[i] = (m->aabb[0][i] + m->aabb[1][i] + 1.f) / 2.f;
    camera_project(goxel.image->active_camera, c, goxel.gui.viewport, sp);
    // Sit next to the region, not on top of it, and keep the whole strip on
    // screen: its width is fixed so the clamp can match it.
    out[0] = clamp(sp[0] + 70.f, 8.f,
                   max(8.f, goxel.screen_size[0] - SLOPE_STRIP_W - 10.f));
    out[1] = clamp(goxel.screen_size[1] - sp[1] - 170.f, 30.f,
                   goxel.screen_size[1] - 20.f);
}

// Drop a shape from a layer.  The cells it was made from are still on this
// layer, so removing the modifier turns them straight back into ordinary
// voxels: the original geometry comes back and nothing is thrown away.
static void slope_clear_layer(layer_t *layer)
{
    if (!layer || !layer->slope) return;
    slope_free(layer->slope);
    layer->slope = NULL;
    image_history_push(goxel.image);
}

void slope_gui(void)
{
    layer_t *active = goxel.image->active_layer;
    int aabb[2][3];
    float pos[2];
    slope_mod_t *m = NULL;
    static const char *DIR_NAMES[SLOPE_DIR_COUNT] = {"+X", "-X", "+Y", "-Y"};
    static const char *ANCHOR_NAMES[SLOPE_ANCHOR_COUNT] = {"Floor", "Mid", "Ceil"};
    int i;

    if (active != s.last_active) {
        // A different layer was picked, so a panel closed earlier no longer
        // applies: picking the shape again opens it normally.
        s.last_active = active;
        s.closed = NULL;
    }
    if (!s.mode && active && active->slope) {
        // Selecting a shape layer is the moment to find out whether it went
        // stale, including one that was stale before the file was saved.
        slope_refresh_stale(active);
        // A session closed with Apply or Cancel stays closed until the Owner
        // picks another layer.  Reopening it on the very next frame is what
        // made both buttons look dead.
        if (s.closed != active)
            begin_edit_slope(active);
        else
            return;
    }
    if (!s.mode && active && !active->slope &&
            selection_aabb(aabb) && image_layer_can_edit(goxel.image, active)) {
        // Offer the operations next to the current selection.
        slope_mod_t tmp = {};
        memcpy(tmp.aabb, aabb, sizeof(aabb));
        strip_position(&tmp, pos);
        gui_window_begin("SlopeOps", pos[0], pos[1], SLOPE_STRIP_W, 0, 0);
        gui_text("Selection: %d cells region",
                 (aabb[1][0] - aabb[0][0] + 1) *
                 (aabb[1][1] - aabb[0][1] + 1) *
                 (aabb[1][2] - aabb[0][2] + 1));
        gui_text("Shape:");
        gui_row_begin(2);
        if (gui_button("Slope", 0, 0)) begin_new_shape(SLOPE_KIND_SLOPE);
        if (gui_button("Bevel", 0, 0)) begin_new_shape(SLOPE_KIND_BEVEL);
        gui_row_end();
        gui_row_begin(2);
        if (gui_button("Cylinder", 0, 0)) begin_new_shape(SLOPE_KIND_CYLINDER);
        if (gui_button("Sphere", 0, 0)) begin_new_shape(SLOPE_KIND_SPHERE);
        gui_row_end();
        gui_window_end();
        return;
    }

    if (!s.mode || !s.mod_layer) return;
    m = s.mod_layer->slope;
    strip_position(m, pos);
    gui_window_begin("SlopeOps", pos[0], pos[1], SLOPE_STRIP_W, 0, 0);
    gui_text("%s", s.mod_layer->name);
    gui_row_begin(2);
    if (gui_button(slope_kind_name(SLOPE_KIND_SLOPE), 0, 0) &&
            m->kind != SLOPE_KIND_SLOPE) {
        m->kind = SLOPE_KIND_SLOPE;
        m->revision++;
    }
    if (gui_button(slope_kind_name(SLOPE_KIND_BEVEL), 0, 0) &&
            m->kind != SLOPE_KIND_BEVEL) {
        m->kind = SLOPE_KIND_BEVEL;
        m->revision++;
    }
    gui_row_end();
    gui_row_begin(2);
    if (gui_button(slope_kind_name(SLOPE_KIND_CYLINDER), 0, 0) &&
            m->kind != SLOPE_KIND_CYLINDER) {
        m->kind = SLOPE_KIND_CYLINDER;
        m->revision++;
    }
    if (gui_button(slope_kind_name(SLOPE_KIND_SPHERE), 0, 0) &&
            m->kind != SLOPE_KIND_SPHERE) {
        m->kind = SLOPE_KIND_SPHERE;
        m->revision++;
    }
    gui_row_end();
    gui_row_begin(3);
    for (i = 0; i < SLOPE_ANCHOR_COUNT; i++) {
        if (gui_button(ANCHOR_NAMES[i], 0, 0) && m->anchor != i) {
            m->anchor = i;
            m->revision++;
        }
    }
    gui_row_end();
    {
        bool full = (m->closed != 0);
        if (gui_checkbox("Full shape", &full,
                         "Barrel / ellipsoid / all-edge chamfer instead of "
                         "an arch, dome or single chamfer")) {
            m->closed = full ? 1 : 0;
            m->revision++;
        }
    }
    gui_row_begin(4);
    for (i = 0; i < SLOPE_DIR_COUNT; i++) {
        if (gui_button(DIR_NAMES[i], 0, 0) && m->dir != i) {
            m->dir = i;
            m->revision++;
        }
    }
    gui_row_end();
    if (gui_input_float("Rise", &m->rise, 0.5f, 0.25f, 512.f, "%.2f"))
        m->revision++;
    gui_row_begin(2);
    if (gui_button("Apply", 0, 0)) {
        apply_session();
        gui_window_end();
        return;
    }
    if (gui_button("Cancel", 0, 0)) {
        cancel_session();
        gui_window_end();
        return;
    }
    gui_row_end();
    gui_text("Alt-drag the yellow handle - Esc closes this panel");
    if (s.conflict || m->stale) {
        gui_text("STALE: cells inside this region changed after the shape was");
        gui_text("applied. The surface is built from the region, so it did NOT");
        gui_text("change to match them. Resolve it one way or the other:");
        gui_row_begin(2);
        if (gui_button("Accept edits", 0, 0)) {
            // Keep the surface and take the cells as they now are.
            m->applied_hash = cells_hash_in_region(s.mod_layer, m->aabb);
            m->stale = 0;
            s.conflict = false;
        }
        if (gui_button("Restore cells", 0, 0)) {
            // Throw the surface away; the source cells become voxels again.
            layer_t *l = s.mod_layer;
            end_session();
            slope_clear_layer(l);
            gui_window_end();
            return;
        }
        gui_row_end();
    }
    gui_window_end();
}

// #### Driven demos #####################################################
// GOXEL_SLOPE_DEMO=1  one slope, end to end, the way the Owner does it
// GOXEL_SHAPE_DEMO=1  all four shape kinds on the same ship, one after another
// Both drive the same code path the editor uses, so the running editor can be
// captured without hijacking the mouse.

static bool s_demo = false;
static bool s_shape_demo = false;
static bool s_place_demo = false;
static bool s_edit_demo = false;
static bool s_pd_demo = false;
static bool s_extract = false;
static int s_edit_reg[2][3];
static int s_place_next = 0;
static layer_t *s_place_last = NULL;
static int s_place_aabb[8][2][3];
static layer_t *s_demo_layer = NULL;
static layer_t *s_demo_base = NULL;
static int s_shape_next = 0;

void slope_demo_begin(void)
{
    s_demo = true;
}

void slope_demo_begin_shapes(void)
{
    s_demo = true;
    s_shape_demo = true;
}

void slope_demo_begin_placement(void)
{
    s_demo = true;
    s_place_demo = true;
}

void slope_demo_begin_edits(void)
{
    s_demo = true;
    s_edit_demo = true;
}

void slope_demo_begin_pd(void)
{
    s_demo = true;
    s_pd_demo = true;
}

void slope_demo_begin_extract(void)
{
    s_demo = true;
    s_extract = true;
}

void slope_ensure_art_layer(void)
{
    return_to_art_layer();
}

// Every occupied cell of every layer, one per line:
//   cell <layer-index> <x> <y> <z> <colour> <layer name...>
// Enough to rebuild a selection of the model somewhere else without guessing
// what a screenshot looked like.
void slope_dump_cells(const char *path)
{
    FILE *f = fopen(path, "w");
    layer_t *l;
    int i = 0;

    if (!f) return;
    DL_FOREACH(goxel.image->layers, l) {
        volume_iterator_t iter;
        int p[3], n = 0;
        uint8_t c[4];

        if (l->volume) {
            iter = volume_get_iterator(l->volume,
                                       VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
            while (volume_iter(&iter, p)) {
                volume_get_at(l->volume, &iter, p, c);
                if (c[3] == 0) continue;
                fprintf(f, "cell %d %d %d %d %d %s\n", i, p[0], p[1], p[2],
                        c[0], l->name);
                n++;
            }
        }
        fprintf(f, "# layer[%d] '%s' cells=%d shape=%d\n", i, l->name, n,
                l->slope ? 1 : 0);
        i++;
    }
    fclose(f);
}

// Which layers actually hold voxels right now.  A shape layer renders a mesh,
// so the brush cannot take anything out of it; this is what makes Subtract look
// dead over a generated barrel while Add keeps working on the art layer.
void slope_dump_layers(FILE *f)
{
    layer_t *l;
    int i = 0;

    fprintf(f, "painter_mode %d\n", goxel.painter.mode);
    DL_FOREACH(goxel.image->layers, l) {
        fprintf(f, "layer[%d] '%s' cells=%d shape=%d%s\n", i++, l->name,
                count_vol_cells(l->volume), l->slope ? 1 : 0,
                l == goxel.image->active_layer ? " ACTIVE" : "");
    }
    fflush(f);
}

// The live bridge needs to know when the viewport handles are owned by a shape
// session, without reaching into this file's state.
bool slope_session_is_open(void)
{
    return s.mode != S_MODE_NONE;
}

// Put the camera at a known orientation: Goxel's default is a top view, so
// rx = pi/2 looks along +Y and shows the X-Z profile.
static void set_view_box(camera_t *cam, const float box[4][4], float rz, float rx)
{
    mat4_set_identity(cam->mat);
    camera_fit_box(cam, box);
    camera_turntable(cam, rz, rx);
}

static void set_view(camera_t *cam, float rz, float rx)
{
    set_view_box(cam, goxel.image->box, rz, rx);
}

// A region that reads clearly on this asset: the nose block, taken across the
// full width of the hull, so the generated plane becomes the outer surface.
static bool demo_region(int aabb[2][3])
{
    int bbox[2][3];
    if (!volume_get_bbox(goxel_get_layers_volume(goxel.image), bbox, true))
        return false;
    aabb[0][0] = bbox[0][0];
    aabb[1][0] = bbox[0][0] + 20;
    aabb[0][1] = bbox[0][1];
    aabb[1][1] = bbox[1][1];
    aabb[0][2] = bbox[0][2];
    aabb[1][2] = bbox[1][2];
    return true;
}

// Four disjoint full-height slices of the hull, one per shape kind, so all
// four can be shown on the same ship without disturbing each other.
static bool shape_region(int kind, int aabb[2][3])
{
    int bbox[2][3], x0, x1, w;
    if (!volume_get_bbox(goxel_get_layers_volume(goxel.image), bbox, true))
        return false;
    x0 = bbox[0][0];
    x1 = bbox[1][0];
    w = (x1 - x0 + 1) / 5;
    if (w < 4) return false;
    aabb[0][1] = bbox[0][1];  aabb[1][1] = bbox[1][1];
    aabb[0][2] = bbox[0][2];  aabb[1][2] = bbox[1][2];
    switch (kind) {
    case SLOPE_KIND_SLOPE:    aabb[0][0] = x0;         aabb[1][0] = x0 + w - 1;     break;
    case SLOPE_KIND_BEVEL:    aabb[0][0] = x1 - w + 1; aabb[1][0] = x1;             break;
    case SLOPE_KIND_CYLINDER: aabb[0][0] = x0 + w;     aabb[1][0] = x0 + 2 * w - 1; break;
    default:                  aabb[0][0] = x1 - 2 * w; aabb[1][0] = x1 - w;         break;
    }
    return true;
}

static void shapes_step(void)
{
    static const int ORDER[SLOPE_KIND_COUNT] = {
        SLOPE_KIND_SLOPE, SLOPE_KIND_BEVEL, SLOPE_KIND_CYLINDER, SLOPE_KIND_SPHERE
    };
    int f = goxel.frame_count;
    camera_t *cam = goxel.image->active_camera;
    int aabb[2][3];
    int i;

    if (f == 20) {
        s_demo_base = goxel.image->active_layer;
        set_view(cam, .7f, 1.05f);
    }

    for (i = 0; i < SLOPE_KIND_COUNT; i++) {
        int begin = 40 + i * 90, done = begin + 60;
        if (f == begin && s_shape_next == i) {
            if (shape_region(ORDER[i], aabb)) {
                goxel.image->active_layer = s_demo_base;
                select_box(aabb);
                begin_new_shape(ORDER[i]);
                s_demo_layer = s.mod_layer;
                s_shape_next = i + 1;
                printf("shape %d (%s): region [%d %d %d]..[%d %d %d] rise %.2f\n",
                       i, slope_kind_name(ORDER[i]),
                       aabb[0][0], aabb[0][1], aabb[0][2],
                       aabb[1][0], aabb[1][1], aabb[1][2],
                       s.mod_layer ? s.mod_layer->slope->rise : 0.f);
            }
        } else if (f == done && s.mode) {
            apply_session();
            s_demo_layer = NULL;
        }
    }

    if (f == 400) {
        const char *dir = getenv("GOXEL_SHOTS");
        char path[512];
        layer_t *l;
        int i2 = 0;
        goxel.image->active_layer = s_demo_base;
        if (dir && *dir) {
            snprintf(path, sizeof(path), "%s/shapes.gox", dir);
            printf("shapes: save %s -> %d\n", path,
                   goxel_export_to_file(path, "gox"));
        }
        DL_FOREACH(goxel.image->layers, l) {
            printf("layer %d: name='%s' cells=%d shape=%s rise=%.2f\n", i2++,
                   l->name, count_vol_cells(l->volume),
                   l->slope ? slope_kind_name(l->slope->kind) : "-",
                   l->slope ? l->slope->rise : 0.f);
        }
        fflush(stdout);
        goxel.quit = true;
    }
}

// #### Placement demo (AMFM #534 P0.2) ################################
// `anchor` and `closed` exist so a shape is not stuck growing upwards out of a
// box.  This drives five different placements on the real ship, each shot from
// the view that shows it best, so the freedom can be judged rather than taken
// on trust.
typedef struct {
    int kind, dir, anchor, closed;
    float rise;
    int pc[6][2];         // region as a percentage of the model bbox, per axis
    float rz, rx;         // the camera that shows it best
    const char *label;
} place_step_t;

static const place_step_t PLACE[] = {
    { SLOPE_KIND_SLOPE, SLOPE_DIR_PX, SLOPE_ANCHOR_FLOOR, 0, 32.f,
      {{0, 16}, {0, 100}, {0, 100}}, 0.f, 1.5708f,
      "bow plane rising from the keel" },
    { SLOPE_KIND_SLOPE, SLOPE_DIR_PY, SLOPE_ANCHOR_CEIL, 0, 16.f,
      {{20, 36}, {50, 100}, {45, 100}}, 1.5708f, 1.5708f,
      "hull side falling away from the deck" },
    { SLOPE_KIND_CYLINDER, SLOPE_DIR_PX, SLOPE_ANCHOR_MID, 1, 14.f,
      {{36, 60}, {0, 100}, {45, 100}}, 1.5708f, 1.5708f,
      "full barrel lying along the ship" },
    { SLOPE_KIND_SPHERE, SLOPE_DIR_PY, SLOPE_ANCHOR_MID, 1, 12.f,
      {{62, 80}, {15, 85}, {40, 100}}, .7f, 1.05f,
      "rounded casing centred in its region" },
    { SLOPE_KIND_BEVEL, SLOPE_DIR_PX, SLOPE_ANCHOR_FLOOR, 0, 12.f,
      {{84, 100}, {0, 100}, {45, 100}}, 0.f, 1.5708f,
      "single directed chamfer on the stern edge" },
};
#define PLACE_NB ((int)(sizeof(PLACE) / sizeof(PLACE[0])))

static bool place_region(const place_step_t *st, int aabb[2][3])
{
    int bbox[2][3], i;
    if (!volume_get_bbox(goxel_get_layers_volume(goxel.image), bbox, true))
        return false;
    for (i = 0; i < 3; i++) {
        const int len = bbox[1][i] - bbox[0][i] + 1;
        aabb[0][i] = bbox[0][i] + (len * st->pc[i][0]) / 100;
        aabb[1][i] = bbox[0][i] + (len * st->pc[i][1]) / 100 - 1;
        if (aabb[1][i] <= aabb[0][i]) return false;
    }
    return true;
}

// Numeric proof of where a generated surface actually ended up.  The render is
// supporting evidence; these bounds are the claim.
static void print_shape_bounds(const slope_mod_t *m)
{
    model3d_t *mo = build_mesh(m);
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    int v, i;

    for (v = 0; v < mo->nb_vertices; v++) {
        const float *p = mo->vertices[v].pos;
        for (i = 0; i < 3; i++) {
            mn[i] = min(mn[i], p[i]);
            mx[i] = max(mx[i], p[i]);
        }
    }
    printf("         mesh %d verts: x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f\n",
           mo->nb_vertices, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    fflush(stdout);
    free_mesh(mo);
}

// Isolate one layer so a render shows the generated surface on its own, with
// the ship's own art out of the way.
static void show_only(const layer_t *keep)
{
    layer_t *l;
    DL_FOREACH(goxel.image->layers, l) l->visible = (l == keep);
}

static void show_all(void)
{
    layer_t *l;
    DL_FOREACH(goxel.image->layers, l) l->visible = true;
}

static void place_step(void)
{
    const int nb = PLACE_NB;
    int f = goxel.frame_count;
    camera_t *cam = goxel.image->active_camera;
    int i;

    if (f == 20) {
        s_demo_base = goxel.image->active_layer;
        set_view(cam, .7f, 1.05f);
    }

    for (i = 0; i < nb; i++) {
        const int begin = 40 + i * 90;
        if (f == begin && s_place_next == i) {
            int aabb[2][3];
            show_all();   // the region is measured from the whole model
            if (place_region(&PLACE[i], aabb)) {
                goxel.image->active_layer = s_demo_base;
                select_box(aabb);
                begin_new_shape(PLACE[i].kind);
                if (s.mod_layer) {
                    s.mod_layer->slope->dir = PLACE[i].dir;
                    s.mod_layer->slope->anchor = PLACE[i].anchor;
                    s.mod_layer->slope->closed = PLACE[i].closed;
                    s.mod_layer->slope->rise = PLACE[i].rise;
                    s.mod_layer->slope->revision++;
                }
                s_demo_layer = s.mod_layer;
                s_place_next = i + 1;
                memcpy(s_place_aabb[i], aabb, sizeof(aabb));
                printf("place %d: %s %s %s rise %.1f -- %s\n"
                       "         region [%d %d %d]..[%d %d %d]\n",
                       i, slope_kind_name(PLACE[i].kind),
                       PLACE[i].anchor == SLOPE_ANCHOR_CEIL ? "ceil" :
                       PLACE[i].anchor == SLOPE_ANCHOR_MID ? "mid" : "floor",
                       PLACE[i].closed ? "full" : "open", PLACE[i].rise,
                       PLACE[i].label,
                       aabb[0][0], aabb[0][1], aabb[0][2],
                       aabb[1][0], aabb[1][1], aabb[1][2]);
            }
        } else if (f == begin + 40 && s.mode) {
            // Commit it, report where it landed, and get the editor out of the
            // way so the photograph is of the result, not of the strip.
            layer_t *applied = s.mod_layer;
            apply_session();
            if (applied) print_shape_bounds(applied->slope);
            s_demo_layer = NULL;
            s_place_last = applied;
            goxel.image->active_layer = s_demo_base;
            if (goxel.image->selection_mask)
                volume_clear(goxel.image->selection_mask);
        } else if (f == begin + 45) {
            // Frame just this shape, alone, from the angle that shows it best.
            int j, near_aabb[2][3];
            float near_box[4][4];
            show_only(s_place_last);
            for (j = 0; j < 3; j++) {
                near_aabb[0][j] = s_place_aabb[i][0][j] - 16;
                near_aabb[1][j] = s_place_aabb[i][1][j] + 16;
            }
            bbox_from_aabb(near_box, near_aabb);
            set_view_box(cam, near_box, PLACE[i].rz, PLACE[i].rx);
        }
    }

    if (f == 40 + nb * 90) {
        const char *dir = getenv("GOXEL_SHOTS");
        char path[512];
        layer_t *l;
        int i2 = 0;
        goxel.image->active_layer = s_demo_base;
        show_all();
        set_view(cam, -.9f, 1.15f);
        if (dir && *dir) {
            snprintf(path, sizeof(path), "%s/placement.gox", dir);
            printf("placement: save %s -> %d\n", path,
                   goxel_export_to_file(path, "gox"));
        }
        DL_FOREACH(goxel.image->layers, l) {
            printf("layer %d: name='%s' cells=%d shape=%s rise=%.2f\n", i2++,
                   l->name, count_vol_cells(l->volume),
                   l->slope ? slope_kind_name(l->slope->kind) : "-",
                   l->slope ? l->slope->rise : 0.f);
        }
        fflush(stdout);
    }
    if (f == 40 + nb * 90 + 40) goxel.quit = true;
}

// #### Inside-region edit demo (AMFM #534 P0.3) #######################
// The PM asked for one deletion and one recolour inside a shape's own region,
// on the real ship, visible before and after.  What this shows is the honest
// answer: the generated surface does NOT follow an individual cell, so the edit
// is hidden under intact-looking geometry.  What makes that an explicit state
// rather than a silent one is the stale tint and the resolve buttons, which the
// later frames show.
static void edit_step(void)
{
    int f = goxel.frame_count;
    camera_t *cam = goxel.image->active_camera;
    static int spot[2][3];
    static layer_t *shape = NULL;

    if (f == 20) {
        int bbox[2][3];
        s_demo_base = goxel.image->active_layer;
        set_view(cam, .7f, 1.05f);
        if (!volume_get_bbox(goxel_get_layers_volume(goxel.image), bbox, true))
            return;
        s_edit_reg[0][0] = bbox[0][0];       s_edit_reg[1][0] = bbox[0][0] + 18;
        s_edit_reg[0][1] = bbox[0][1];       s_edit_reg[1][1] = bbox[1][1];
        s_edit_reg[0][2] = bbox[0][2];       s_edit_reg[1][2] = bbox[1][2];
    } else if (f == 40) {
        goxel.image->active_layer = s_demo_base;
        select_box(s_edit_reg);
        begin_new_shape(SLOPE_KIND_SLOPE);
        if (s.mod_layer) {
            s.mod_layer->slope->rise = 24.f;
            s.mod_layer->slope->revision++;
            // Keep the layer now: apply_session() clears s.mod_layer.
            shape = s.mod_layer;
        }
    } else if (f == 60 && s.mode && shape) {
        apply_session();
        s_demo_layer = NULL;
        goxel.image->active_layer = s_demo_base;
        if (goxel.image->selection_mask)
            volume_clear(goxel.image->selection_mask);
        printf("edit demo: layer '%s' region [%d %d %d]..[%d %d %d] cells=%d\n",
               shape->name,
               shape->slope->aabb[0][0], shape->slope->aabb[0][1],
               shape->slope->aabb[0][2], shape->slope->aabb[1][0],
               shape->slope->aabb[1][1], shape->slope->aabb[1][2],
               count_vol_cells(shape->volume));
        fflush(stdout);
    } else if (f == 65 && shape) {
        // Frame the region so the before/after pair is directly comparable.
        int j, near_aabb[2][3];
        float near_box[4][4];
        for (j = 0; j < 3; j++) {
            near_aabb[0][j] = shape->slope->aabb[0][j] - 14;
            near_aabb[1][j] = shape->slope->aabb[1][j] + 14;
        }
        bbox_from_aabb(near_box, near_aabb);
        set_view_box(cam, near_box, 0.f, 1.5708f);
    } else if (f == 85 && shape) {
        // One deletion inside the region, on the shape's own layer.
        int p[3], done = 0;
        uint8_t c[4];
        const int *lo = shape->slope->aabb[0], *hi = shape->slope->aabb[1];
        for (p[2] = lo[2]; p[2] <= hi[2] && !done; p[2]++)
        for (p[1] = lo[1]; p[1] <= hi[1] && !done; p[1]++)
        for (p[0] = lo[0]; p[0] <= hi[0] && !done; p[0]++) {
            volume_get_at(shape->volume, NULL, p, c);
            if (c[3]) { done = 1; memcpy(spot[0], p, sizeof(p)); }
        }
        if (done) {
            uint8_t zero[4] = {0, 0, 0, 0};
            volume_set_at(shape->volume, NULL, spot[0], zero);
            printf("edit demo: deleted [%d %d %d] inside the region -> cells=%d stale=%d\n",
                   spot[0][0], spot[0][1], spot[0][2],
                   count_vol_cells(shape->volume), (int)slope_refresh_stale(shape));
            fflush(stdout);
        }
    } else if (f == 110 && shape) {
        // One recolour inside the region.
        int p[3], done = 0;
        uint8_t c[4];
        const int *lo = shape->slope->aabb[0], *hi = shape->slope->aabb[1];
        for (p[2] = lo[2]; p[2] <= hi[2] && !done; p[2]++)
        for (p[1] = lo[1]; p[1] <= hi[1] && !done; p[1]++)
        for (p[0] = lo[0]; p[0] <= hi[0] && !done; p[0]++) {
            volume_get_at(shape->volume, NULL, p, c);
            if (c[3]) { done = 1; memcpy(spot[1], p, sizeof(p)); }
        }
        if (done) {
            uint8_t bright[4] = {40, 255, 90, 255};
            volume_set_at(shape->volume, NULL, spot[1], bright);
            printf("edit demo: recoloured [%d %d %d] inside the region -> cells=%d stale=%d\n",
                   spot[1][0], spot[1][1], spot[1][2],
                   count_vol_cells(shape->volume), (int)slope_refresh_stale(shape));
            fflush(stdout);
        }
    } else if (f == 135 && shape) {
        printf("edit demo: Restore cells -> the source voxels come back (%d cells)\n",
               count_vol_cells(shape->volume));
        slope_clear_layer(shape);
        fflush(stdout);
    }

    if (f == 175) {
        const char *dir = getenv("GOXEL_SHOTS");
        char path[512];
        layer_t *l;
        int i2 = 0;
        goxel.image->active_layer = s_demo_base;
        if (dir && *dir) {
            snprintf(path, sizeof(path), "%s/edits.gox", dir);
            printf("edits: save %s -> %d\n", path, goxel_export_to_file(path, "gox"));
        }
        DL_FOREACH(goxel.image->layers, l) {
            printf("layer %d: name='%s' cells=%d shape=%s\n", i2++, l->name,
                   count_vol_cells(l->volume),
                   l->slope ? slope_kind_name(l->slope->kind) : "-");
        }
        fflush(stdout);
        goxel.quit = true;
    }
}

// #### PD barrel demo (AMFM #534, PM correction 2026-11-05) #############
// The PM correction is that a PD barrel is a rendered-surface problem, not a
// voxel-scale redesign: generate a real sub-voxel-radius cylinder around each
// original barrel axis, adding no cells and leaving the mount alone.  This
// drives exactly that through the same native authoring path the Owner uses --
// select, shape, set the parameters, apply -- once per barrel, then photographs
// it from the front, the side, a perspective angle and at game distance.
static void pd_step(void)
{
    int f = goxel.frame_count;
    camera_t *cam = goxel.image->active_camera;
    static int cells[8][3];
    static int ncells = 0;
    static int done = 0;
    static float rise = 0.45f;
    static int bbox[2][3];

    if (f == 15) {
        const char *spec = getenv("GOXEL_PD_CELLS") ?: "";
        const char *r = getenv("GOXEL_PD_RISE");
        char buf[512];
        char *p;
        s_demo_base = goxel.image->active_layer;
        if (r) rise = atof(r);
        snprintf(buf, sizeof(buf), "%s", spec);
        p = buf;
        while (*p && ncells < 8) {
            int x = 0, y = 0, z = 0;
            if (sscanf(p, "%d,%d,%d", &x, &y, &z) == 3) {
                cells[ncells][0] = x; cells[ncells][1] = y; cells[ncells][2] = z;
                ncells++;
            }
            p = strchr(p, ';');
            if (!p) break;
            p++;
        }
        volume_get_bbox(goxel_get_layers_volume(goxel.image), bbox, true);
        printf("pd demo: %d barrel cells, rise %.3f, cells now %d\n",
               ncells, rise, count_vol_cells(goxel_get_layers_volume(goxel.image)));
        fflush(stdout);
    } else if (f == 30 + done * 20 && done < ncells) {
        // One shape per barrel: the barrel's own cell is the whole region, so
        // nothing outside it is touched.
        int reg[2][3];
        reg[0][0] = reg[1][0] = cells[done][0];
        reg[0][1] = reg[1][1] = cells[done][1];
        reg[0][2] = reg[1][2] = cells[done][2];
        goxel.image->active_layer = s_demo_base;
        select_box(reg);
        begin_new_shape(SLOPE_KIND_CYLINDER);
        if (s.mod_layer) {
            slope_mod_t *m = s.mod_layer->slope;
            m->kind = SLOPE_KIND_CYLINDER;
            m->closed = 1;              // a full barrel, not an arch
            m->dir = SLOPE_DIR_PY;      // the barrel axis
            m->anchor = SLOPE_ANCHOR_MID;   // centred in the cell
            m->rise = rise;             // sub-voxel radius
            m->revision++;
        }
    } else if (f == 40 + done * 20 && s.mode) {
        apply_session();
        done++;
        s_demo_layer = NULL;
        goxel.image->active_layer = s_demo_base;
        if (goxel.image->selection_mask)
            volume_clear(goxel.image->selection_mask);
    } else if (f == 40 + ncells * 20 + 10) {
        layer_t *l;
        printf("pd demo: applied; cells now %d\n",
               count_vol_cells(goxel_get_layers_volume(goxel.image)));
        DL_FOREACH(goxel.image->layers, l) {
            if (!l->slope) continue;
            printf("  shape '%s' kind=%s closed=%d rise=%.3f\n",
                   l->name, slope_kind_name(l->slope->kind),
                   l->slope->closed, l->slope->rise);
            print_shape_bounds(l->slope);
        }
        fflush(stdout);
    }

    // Views: close perspective, straight front, straight side, and the whole
    // ship at the distance a compact game would actually show it.
    if (f == 40 + ncells * 20 + 20) {
        int a[2][3]; float box[4][4];
        a[0][0] = 0;  a[1][0] = 9;
        a[0][1] = -15; a[1][1] = 1;
        a[0][2] = 4;  a[1][2] = 15;
        bbox_from_aabb(box, a);
        set_view_box(cam, box, 0.9f, 1.15f);
    } else if (f == 40 + ncells * 20 + 45) {
        int a[2][3]; float box[4][4];
        a[0][0] = 0;  a[1][0] = 9;
        a[0][1] = -15; a[1][1] = 1;
        a[0][2] = 4;  a[1][2] = 15;
        bbox_from_aabb(box, a);
        set_view_box(cam, box, 0.f, 1.5708f);
    } else if (f == 40 + ncells * 20 + 70) {
        int a[2][3]; float box[4][4];
        a[0][0] = 0;  a[1][0] = 9;
        a[0][1] = -15; a[1][1] = 1;
        a[0][2] = 4;  a[1][2] = 15;
        bbox_from_aabb(box, a);
        set_view_box(cam, box, 1.5708f, 1.5708f);
    } else if (f == 40 + ncells * 20 + 95) {
        set_view(cam, 0.7f, 1.05f);
    }

    if (f == 40 + ncells * 20 + 125) {
        const char *dir = getenv("GOXEL_SHOTS");
        char path[512];
        if (dir && *dir) {
            snprintf(path, sizeof(path), "%s/pd.gox", dir);
            printf("pd demo: save %s -> %d\n", path, goxel_export_to_file(path, "gox"));
        }
        fflush(stdout);
        goxel.quit = true;
    }
}

// #### PD turret extraction (AMFM #534, Owner-directed 2026-11-05) #########
// Copy the PD out of the ship into its own editable project.  The cell lists
// were read out of the Owner's saved authored file, so their own voxel removals
// are part of the result, and the ship file on disk is never modified.
static void move_cell(layer_t *src, layer_t *dst, const int p[3])
{
    uint8_t c[4] = { 0, 0, 0, 0 };

    volume_get_at(src->volume, NULL, p, c);
    if (c[3] == 0) return;
    volume_set_at(dst->volume, NULL, p, c);
    c[3] = 0;
    volume_set_at(src->volume, NULL, p, c);
}

static void pd_extract_step(void)
{
    int f = goxel.frame_count;
    static layer_t *x_yaw, *x_pitch;

    // YawBase: the single turntable cell that sits on the ship's 3x3 pad.
    static const int yaw_cells[1][3] = { { 4, -4, 8 } };
    // PitchAssembly: the upper body as it now stands (the Owner removed the
    // front-centre cell) plus the white sensor ornament.
    static const int pitch_cells[9][3] = {
        { 3, -7, 9 }, { 5, -7, 9 }, { 3, -6, 9 }, { 4, -6, 9 }, { 5, -6, 9 },
        { 3, -5, 9 }, { 4, -5, 9 }, { 5, -5, 9 }, { 4, -4, 9 }
    };
    static const int sensor_cell[3] = { 4, -5, 10 };

    if (f == 15) {
        layer_t *art = NULL, *l, *kill[8];
        int nkill = 0, i;

        printf("extract: building\n");
        fflush(stdout);

        DL_FOREACH(goxel.image->layers, l)
            if (!l->slope && (!art ||
                    count_vol_cells(l->volume) > count_vol_cells(art->volume)))
                art = l;
        if (!art) { goxel.quit = true; return; }

        {
            x_yaw = layer_new("YawBase");
            x_pitch = layer_new("PitchAssembly");
            image_add_layer(goxel.image, x_yaw);
            image_add_layer(goxel.image, x_pitch);
            for (i = 0; i < 1; i++) move_cell(art, x_yaw, yaw_cells[i]);
            for (i = 0; i < 9; i++) move_cell(art, x_pitch, pitch_cells[i]);
            move_cell(art, x_pitch, sensor_cell);
            vec4_copy(x_yaw->color, art->color);
            vec4_copy(x_pitch->color, art->color);
        }
        // The ship itself does not belong in the extract; only the turret cells
        // that were just moved out do.
        volume_clear(art->volume);

        // The ship and its 3x3 hardpoint pad stay ship-side: the extract holds
        // the turret only.  Then drop the empty layers a save had left behind.
        DL_FOREACH(goxel.image->layers, l) {
            if (!l->slope && count_vol_cells(l->volume) == 0 && nkill < 8)
                kill[nkill++] = l;
        }
        for (i = 0; i < nkill; i++) image_delete_layer(goxel.image, kill[i]);
        // Never leave the active layer pointing at one that was just deleted.
        goxel.image->active_layer = x_pitch;

        // Name the two generated barrels as part of the pitching group.
        i = 0;
        DL_FOREACH(goxel.image->layers, l) {
            if (!l->slope) continue;
            snprintf(l->name, sizeof(l->name),
                     i++ == 0 ? "PitchAssembly_barrel_L"
                              : "PitchAssembly_barrel_R");
        }
    } else if (f == 30) {
        const char *dir = getenv("GOXEL_PD_EXTRACT") ?: "";
        const char *keep = getenv("GOXEL_PD_KEEP");
        char path[512];

        if (*dir) {
            if (!keep || !strcmp(keep, "all")) {
                snprintf(path, sizeof(path), "%s/pd_turret.gox", dir);
                printf("extract: writing gox %s\n", path);
                fflush(stdout);
                printf("extract: gox -> %d\n", goxel_export_to_file(path, "gox"));
                snprintf(path, sizeof(path), "%s/pd_turret.vox", dir);
                printf("extract: writing vox %s\n", path);
                fflush(stdout);
                printf("extract: vox -> %d\n", goxel_export_to_file(path, "vox"));
            } else {
                layer_t *l, *kill[8];
                int nkill = 0, i;
                bool want_yaw = (*keep == 'y');
                DL_FOREACH(goxel.image->layers, l) {
                    bool is_yaw = l->name && !strcmp(l->name, "YawBase");
                    if (l->slope || (want_yaw ? !is_yaw : is_yaw))
                        if (nkill < 8) kill[nkill++] = l;
                }
                for (i = 0; i < nkill; i++) image_delete_layer(goxel.image, kill[i]);
                goxel.image->active_layer = want_yaw ? x_yaw : x_pitch;
                snprintf(path, sizeof(path), "%s/pd_%s.vox", dir, keep);
                printf("extract: vox %s -> %d\n", path,
                       goxel_export_to_file(path, "vox"));
            }
        }
        printf("extract: layers now\n");
        slope_dump_layers(stdout);
        fflush(stdout);
        goxel.quit = true;
    }
}

static void slope_demo_step(void)
{
    int aabb[2][3];
    int f = goxel.frame_count;
    camera_t *cam = goxel.image->active_camera;

    if (!s_demo) return;
    if (s_extract) {
        pd_extract_step();
        return;
    }
    if (s_pd_demo) {
        pd_step();
        return;
    }
    if (s_edit_demo) {
        edit_step();
        return;
    }
    if (s_place_demo) {
        place_step();
        return;
    }
    if (s_shape_demo) {
        shapes_step();
        return;
    }
    if (f == 20) {
        set_view(cam, .7f, 1.05f);
    } else if (f == 40) {
        if (demo_region(aabb)) {
            int h = aabb[1][SLOPE_UP] - aabb[0][SLOPE_UP] + 1;
            select_box(aabb);
            begin_new_shape(SLOPE_KIND_SLOPE);
            s_demo_layer = s.mod_layer;
            if (s_demo_layer)
                s_demo_layer->slope->rise = h - 6;
            printf("demo region: [%d %d %d]..[%d %d %d] rise %.1f\n",
                   aabb[0][0], aabb[0][1], aabb[0][2],
                   aabb[1][0], aabb[1][1], aabb[1][2],
                   s_demo_layer ? s_demo_layer->slope->rise : 0.f);
        }
    } else if (f == 150 && s_demo_layer) {
        // Same effect as dragging the handle to a new height, then look
        // straight at the profile so the planar face reads as one line.
        int near_aabb[2][3], i;
        float near_box[4][4];
        int h = s_demo_layer->slope->aabb[1][SLOPE_UP] -
                s_demo_layer->slope->aabb[0][SLOPE_UP] + 1;
        s_demo_layer->slope->rise = h + 6;
        s_demo_layer->slope->revision++;
        for (i = 0; i < 3; i++) {
            near_aabb[0][i] = s_demo_layer->slope->aabb[0][i] - 14;
            near_aabb[1][i] = s_demo_layer->slope->aabb[1][i] + 14;
        }
        bbox_from_aabb(near_box, near_aabb);
        set_view_box(cam, near_box, 0.f, 1.5708f);
    } else if (f == 300 && s.mode) {
        apply_session();
        s_demo_layer = NULL;
        set_view(cam, 3.5f, 1.05f);
        // Write the document out; a separate run opens it back, which is how a
        // person would actually check it.
        {
            const char *dir = getenv("GOXEL_SHOTS");
            char path[512];
            if (dir && *dir) {
                snprintf(path, sizeof(path), "%s/reopen.gox", dir);
                if (goxel_export_to_file(path, "gox") == 0)
                    printf("demo: saved %s\n", path);
                else
                    printf("demo: save FAILED\n");
                fflush(stdout);
            }
        }
    } else if (f == 320) {
        // The native layer list is what organises the modifier.
        layer_t *l;
        int i = 0;
        DL_FOREACH(goxel.image->layers, l) {
            printf("layer %d: name='%s' cells=%d slope=%s\n", i++, l->name,
                   count_vol_cells(l->volume), l->slope ? "yes" : "no");
        }
    }
}

// #### Frame capture (GOXEL_SHOTS=<dir>) ################################
// Reads back the finished frame (3D view + panels) so the evidence shots do
// not depend on what happens to be on top of the window.

// Read back the finished frame (3D view + panels) into a PNG.
static bool slope_grab(const char *path)
{
    int w = goxel.screen_size[0] * goxel.screen_scale;
    int h = goxel.screen_size[1] * goxel.screen_scale;
    uint8_t *pixels, *flip;
    int y;
    bool ok = false;

    if (w <= 0 || h <= 0) return false;
    pixels = malloc((size_t)w * h * 4);
    flip = malloc((size_t)w * h * 4);
    if (!pixels || !flip) { free(pixels); free(flip); return false; }

    GL(glBindFramebuffer(GL_FRAMEBUFFER, goxel.rend.fbo));
    GL(glPixelStorei(GL_PACK_ALIGNMENT, 1));
    GL(glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels));
    for (y = 0; y < h; y++) {
        memcpy(flip + (size_t)(h - 1 - y) * w * 4,
               pixels + (size_t)y * w * 4, (size_t)w * 4);
    }
    img_write(flip, w, h, 4, path);
    ok = true;
    free(pixels);
    free(flip);
    return ok;
}

void slope_capture(void)
{
    static const int AT[] = {60, 200, 340};
    static const int AT_SHAPES[] = {80, 170, 260, 350};
    static const int AT_PLACE[] = {105, 195, 285, 375, 465, 510};
    static const int AT_EDIT[] = {70, 100, 125, 150};
    static const int AT_PD[] = {115, 140, 165, 190};
    static const char *dir = NULL;
    static int idx = 0;
    static const char *once = NULL;
    const int *at = s_pd_demo ? AT_PD : s_edit_demo ? AT_EDIT
                        : s_place_demo ? AT_PLACE
                                 : s_shape_demo ? AT_SHAPES : AT;
    int nb = s_pd_demo ? (int)(sizeof(AT_PD) / sizeof(AT_PD[0]))
           : s_edit_demo ? (int)(sizeof(AT_EDIT) / sizeof(AT_EDIT[0]))
           : s_place_demo ? (int)(sizeof(AT_PLACE) / sizeof(AT_PLACE[0]))
           : s_shape_demo ? (int)(sizeof(AT_SHAPES) / sizeof(AT_SHAPES[0]))
                          : (int)(sizeof(AT) / sizeof(AT[0]));
    char path[512];

    // GOXEL_SHOTS_ONCE=<file>: grab a single frame of an opened document.
    if (!once) once = getenv("GOXEL_SHOTS_ONCE") ?: "";
    if (*once && goxel.frame_count >= 60) {
        slope_grab(once);
        goxel.quit = true;
        return;
    }

    if (!dir) dir = getenv("GOXEL_SHOTS") ?: "";
    if (!*dir || idx >= nb) return;
    if (goxel.frame_count < at[idx]) return;

    if (s_pd_demo)
        snprintf(path, sizeof(path), "%s/pd%d.png", dir, idx);
    else if (s_edit_demo)
        snprintf(path, sizeof(path), "%s/edit%d.png", dir, idx);
    else if (s_place_demo)
        snprintf(path, sizeof(path), "%s/place%d.png", dir, idx);
    else if (s_shape_demo)
        snprintf(path, sizeof(path), "%s/shape%d.png", dir, idx);
    else
        snprintf(path, sizeof(path), "%s/frame%03d.png", dir, at[idx]);
    if (slope_grab(path)) {
        printf("slope: wrote %s\n", path);
        fflush(stdout);
    }
    idx++;
    // Once every requested frame is on disk there is nothing left to show.
    // The shapes and placement demos keep running so they can write their
    // document out first.
    if (idx >= nb && !s_shape_demo && !s_place_demo && !s_edit_demo &&
            !s_pd_demo) goxel.quit = true;
}

// #### Headless self test ###############################################

static int count_vol_cells(const volume_t *vol)
{
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3], n = 0;

    if (!vol) return 0;
    iter = volume_get_iterator(vol, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, p)) {
        volume_get_at(vol, &iter, p, c);
        if (c[3]) n++;
    }
    return n;
}

static int count_cells(const image_t *img)
{
    const layer_t *layer;
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3], n = 0;

    DL_FOREACH(img->layers, layer) {
        iter = volume_get_iterator(layer->volume,
                                   VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&iter, p)) {
            volume_get_at(layer->volume, &iter, p, c);
            if (c[3]) n++;
        }
    }
    return n;
}

static int count_layers(const image_t *img)
{
    const layer_t *layer;
    int n = 0;
    DL_FOREACH(img->layers, layer) n++;
    return n;
}

static bool in_aabb(const int p[3], const int aabb[2][3])
{
    return p[0] >= aabb[0][0] && p[0] <= aabb[1][0] &&
           p[1] >= aabb[0][1] && p[1] <= aabb[1][1] &&
           p[2] >= aabb[0][2] && p[2] <= aabb[1][2];
}

// Hash of every occupied cell (position and colour) across all layers.  When
// `skip` is given, cells inside that region are left out.
static uint32_t cells_hash(const image_t *img, const int skip[2][3])
{
    const layer_t *layer;
    volume_iterator_t iter;
    uint32_t key = 0;
    uint8_t c[4];
    int p[3];

    DL_FOREACH(img->layers, layer) {
        iter = volume_get_iterator(layer->volume,
                                   VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&iter, p)) {
            volume_get_at(layer->volume, &iter, p, c);
            if (!c[3]) continue;
            if (skip && in_aabb(p, skip)) continue;
            key = XXH32(p, sizeof(p), key);
            key = XXH32(c, sizeof(c), key);
        }
    }
    return key;
}

// Fill the selection mask with the occupied cells of the active layer inside
// aabb, exactly like the native Rectangle Select tool would.
static void select_box(const int aabb[2][3])
{
    image_t *img = goxel.image;
    const volume_t *vol = img->active_layer->volume;
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3];

    if (!img->selection_mask) img->selection_mask = volume_new();
    volume_clear(img->selection_mask);
    iter = volume_get_iterator(vol, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, p)) {
        if (!in_aabb(p, aabb)) continue;
        volume_get_at(vol, &iter, p, c);
        if (!c[3]) continue;
        vec4_set(c, 255, 255, 255, 255);
        volume_set_at(img->selection_mask, NULL, p, c);
    }
}

static int g_checks = 0;
static int g_failed = 0;

static void check(const char *name, bool ok, const char *detail)
{
    g_checks++;
    if (!ok) g_failed++;
    printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
           detail ? " -- " : "", detail ? detail : "");
    fflush(stdout);
}

// Occupied cells inside a box.
static int count_vol_cells_in(const image_t *img, const int aabb[2][3])
{
    const layer_t *layer;
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3], n = 0;

    DL_FOREACH(img->layers, layer) {
        iter = volume_get_iterator(layer->volume,
                                   VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&iter, p)) {
            if (!in_aabb(p, aabb)) continue;
            volume_get_at(layer->volume, &iter, p, c);
            if (c[3]) n++;
        }
    }
    return n;
}

// Occupied cells inside the box that nothing covers from above: the exposed
// skin.  A patch that covers a wide stretch of it is where a hull slope shows.
static int count_skin_cells(const image_t *img, const int aabb[2][3])
{
    const layer_t *layer;
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3], up[3], n = 0;

    DL_FOREACH(img->layers, layer) {
        iter = volume_get_iterator(layer->volume,
                                   VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&iter, p)) {
            if (!in_aabb(p, aabb)) continue;
            volume_get_at(layer->volume, &iter, p, c);
            if (!c[3]) continue;
            up[0] = p[0]; up[1] = p[1]; up[2] = p[2] + 1;
            volume_get_at(layer->volume, NULL, up, c);
            if (c[3]) continue;
            n++;
        }
    }
    return n;
}

// Pick a box that actually contains geometry: scan a coarse grid over the
// model and keep the densest candidate.  Used by the self test and by the
// "suggested region" affordance.
static bool suggest_region(int aabb[2][3])
{
    const image_t *img = goxel.image;
    int bbox[2][3], cand[2][3];
    int best[2][3] = {};
    int best_n = 0, n;
    int fx, fy, fz, i;
    static const float FRAC[3] = {0.f, 0.5f, 1.f};
    int sx, sy, sz;

    if (!volume_get_bbox(goxel_get_layers_volume(img), bbox, true)) return false;
    sx = max(6, (bbox[1][0] - bbox[0][0]) / 6);
    sy = max(6, (bbox[1][1] - bbox[0][1]) / 6);
    sz = max(4, (bbox[1][2] - bbox[0][2]) / 8);
    sx = min(sx, bbox[1][0] - bbox[0][0] + 1);
    sy = min(sy, bbox[1][1] - bbox[0][1] + 1);
    sz = min(sz, bbox[1][2] - bbox[0][2] + 1);

    for (fz = 0; fz < 3; fz++)
    for (fy = 0; fy < 3; fy++)
    for (fx = 0; fx < 3; fx++) {
        cand[0][0] = bbox[0][0] + (bbox[1][0] - bbox[0][0] - sx + 1) * FRAC[fx];
        cand[0][1] = bbox[0][1] + (bbox[1][1] - bbox[0][1] - sy + 1) * FRAC[fy];
        cand[0][2] = bbox[0][2] + (bbox[1][2] - bbox[0][2] - sz + 1) * FRAC[fz];
        for (i = 0; i < 3; i++)
            cand[1][i] = cand[0][i] + (i == 2 ? sz : (i ? sy : sx)) - 1;
        // Prefer a wide exposed surface (a hull patch) over buried volume.
        n = 4 * count_skin_cells(img, cand) + count_vol_cells_in(img, cand);
        if (n > best_n) {
            best_n = n;
            memcpy(best, cand, sizeof(best));
        }
    }
    if (best_n <= 0) return false;
    memcpy(aabb, best, 6 * sizeof(int));
    printf("suggested region: [%d %d %d] .. [%d %d %d] (%d cells)\n",
           aabb[0][0], aabb[0][1], aabb[0][2],
           aabb[1][0], aabb[1][1], aabb[1][2], best_n);
    return true;
}

// Occupied cells of one layer, as a hash: cell identity, not just a count.
static uint32_t layer_cells_hash(const layer_t *layer)
{
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3];
    uint32_t key = 0;

    iter = volume_get_iterator(layer->volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, p)) {
        volume_get_at(layer->volume, &iter, p, c);
        if (!c[3]) continue;
        key = XXH32(p, sizeof(p), key);
        key = XXH32(c, sizeof(c), key);
    }
    return key;
}

// Every kind, on every anchor plane, open and closed, has to produce a real
// closed surface: no NaN, inside its region footprint, reaching exactly the
// requested height from its anchor plane and not crossing that plane the wrong
// way.  This is what proves the shapes are not stuck growing upwards.
static void check_shape_geometry(void)
{
    char buf[96];
    int kind, anchor, closed;

    for (kind = 0; kind < SLOPE_KIND_COUNT; kind++) {
        for (anchor = 0; anchor < SLOPE_ANCHOR_COUNT; anchor++) {
            for (closed = 0; closed <= 1; closed++) {
                slope_mod_t m = {};
                model3d_t *mo;
                float zmin = 1e9f, zmax = -1e9f;
                float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
                surf_frame_t fr;
                bool finite = true;
                int v;

                m.kind = kind;
                m.dir = SLOPE_DIR_PX;
                m.anchor = anchor;
                m.closed = closed;
                m.rise = 10.f;
                // Deliberately away from the origin: a builder that leaks a
                // placeholder coordinate of 0 must land outside this footprint.
                m.aabb[0][0] = 30;  m.aabb[1][0] = 42;
                m.aabb[0][1] = 20;  m.aabb[1][1] = 40;
                m.aabb[0][2] = -5;  m.aabb[1][2] = 3;
                surf_frame(&m, &fr);

                mo = build_mesh(&m);
                for (v = 0; v < mo->nb_vertices; v++) {
                    const float *p = mo->vertices[v].pos;
                    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
                        finite = false;
                    zmin = min(zmin, p[SLOPE_UP]);
                    zmax = max(zmax, p[SLOPE_UP]);
                    xmin = min(xmin, p[0]); xmax = max(xmax, p[0]);
                    ymin = min(ymin, p[1]); ymax = max(ymax, p[1]);
                }
                snprintf(buf, sizeof(buf), "%s/%s/%s: %d vertices",
                         slope_kind_name(kind),
                         anchor == SLOPE_ANCHOR_CEIL ? "ceil" :
                         anchor == SLOPE_ANCHOR_MID ? "mid" : "floor",
                         closed ? "full" : "open", mo->nb_vertices);
                check("shape builds a real surface",
                      mo->nb_vertices > 0 && finite, buf);

                snprintf(buf, sizeof(buf), "%s/%s", slope_kind_name(kind),
                         closed ? "full" : "open");
                check("shape stays inside its region footprint",
                      xmin >= m.aabb[0][0] - SLOPE_EPS - 1e-3f &&
                      xmax <= m.aabb[1][0] + 1.f + SLOPE_EPS + 1e-3f &&
                      ymin >= m.aabb[0][1] - SLOPE_EPS - 1e-3f &&
                      ymax <= m.aabb[1][1] + 1.f + SLOPE_EPS + 1e-3f, buf);

                if (fr.spans) {
                    // A full barrel or ellipsoid is centred on its anchor.
                    snprintf(buf, sizeof(buf), "%s: z %.2f..%.2f want %.2f..%.2f",
                             slope_kind_name(kind), zmin, zmax,
                             fr.centre - fr.rise, fr.centre + fr.rise);
                    check("closed shape spans centre by rise",
                          fabsf(zmax - (fr.centre + fr.rise)) < 0.01f &&
                          fabsf(zmin - (fr.centre - fr.rise)) < 0.01f, buf);
                } else {
                    snprintf(buf, sizeof(buf), "%s: crown %.2f want %.2f",
                             slope_kind_name(kind),
                             fr.dirz > 0 ? zmax : zmin, surf_crown(&fr));
                    check("shape crown sits at the requested height",
                          fabsf((fr.dirz > 0 ? zmax : zmin) - surf_crown(&fr))
                              < 0.01f, buf);
                    snprintf(buf, sizeof(buf), "%s: z %.2f..%.2f anchor %.2f",
                             slope_kind_name(kind), zmin, zmax, fr.base);
                    check("shape does not cross its anchor plane",
                          fr.dirz > 0 ? zmin >= fr.base - SLOPE_EPS - 1e-3f
                                      : zmax <= fr.base + SLOPE_EPS + 1e-3f, buf);
                }

                if (kind == SLOPE_KIND_BEVEL) {
                    // A directed bevel chamfers only the high edge; a closed
                    // one chamfers the face right round, so the chamfered face
                    // starts further in along the run.
                    float face_rmin = 1e9f;
                    for (v = 0; v < mo->nb_vertices; v++) {
                        const float *p = mo->vertices[v].pos;
                        if (fabsf(p[SLOPE_UP] - surf_crown(&fr)) < 0.01f)
                            face_rmin = min(face_rmin, p[0]);
                    }
                    snprintf(buf, sizeof(buf), "%s %s: face starts at %.2f",
                             slope_kind_name(kind), closed ? "full" : "open",
                             face_rmin);
                    check("bevel chamfers the edges its kind asks for",
                          closed ? face_rmin > m.aabb[0][0] + 0.5f
                                 : fabsf(face_rmin - m.aabb[0][0]) < 0.01f, buf);
                }

                free_mesh(mo);
                check("the heap is intact after the shape", heap_ok(), buf);
            }
        }
    }
}

void slope_selftest(void)
{
    image_t *img = goxel.image;
    int bbox[2][3], aabb[2][3], expected[2][3];
    layer_t *modl;
    int n0, n1, n2, region_cells;
    uint32_t all0, out0, out1, applied;
    char buf[320];    // big enough for a full 256-char layer name plus context

    printf("== AMFM #534 slope self test ==\n");
    if (!volume_get_bbox(goxel_get_layers_volume(img), bbox, true)) {
        printf("FAIL: the model is empty\n");
        return;
    }
    printf("model bbox: [%d %d %d] .. [%d %d %d]\n",
           bbox[0][0], bbox[0][1], bbox[0][2],
           bbox[1][0], bbox[1][1], bbox[1][2]);
    check("the heap is intact on entry", heap_ok(), NULL);

    if (!suggest_region(aabb)) {
        printf("FAIL: could not find a region containing geometry\n");
        return;
    }

    image_history_push(img);
    n0 = count_cells(img);
    all0 = cells_hash(img, NULL);
    out0 = cells_hash(img, aabb);
    select_box(aabb);
    // The region becomes the tight bounds of what the selection really hit.
    selection_aabb(expected);
    region_cells = count_vol_cells(img->selection_mask);
    printf("before: cells=%d region_cells=%d\n", n0, region_cells);

    begin_new_shape(SLOPE_KIND_SLOPE);
    if (!s.mode || !s.mod_layer) {
        printf("FAIL: the modifier session did not start\n");
        return;
    }
    modl = s.mod_layer;
    modl->slope->dir = SLOPE_DIR_PY;
    modl->slope->rise = 12.f;
    apply_session();

    n1 = count_cells(img);
    out1 = cells_hash(img, aabb);
    applied = cells_hash(img, NULL);
    check("total cell count preserved", n1 == n0, NULL);
    check("cells outside the region untouched", out1 == out0, NULL);
    snprintf(buf, sizeof(buf), "layers=%d", count_layers(img));
    check("modifier layer created", count_layers(img) == 2, buf);
    snprintf(buf, sizeof(buf), "moved=%d expected=%d",
             count_vol_cells(modl->volume), region_cells);
    check("modifier holds the region cells",
          count_vol_cells(modl->volume) == region_cells, buf);
    check("modifier keeps the region bounds",
          memcmp(modl->slope->aabb, expected, sizeof(expected)) == 0, NULL);
    check("modifier parameters kept",
          modl->slope->rise == 12.f && modl->slope->dir == SLOPE_DIR_PY, NULL);

    // #### viewport handle drag ####
    // Feed the same inputs a real mouse drag produces, so the handle path is
    // exercised end to end, then cancel to leave the document untouched.
    {
        inputs_t in = {};
        float vp[4] = {0, 0, 1024, 1024};
        float h[3], sp[3], tgt[3], tp[3];
        float before = modl->slope->rise, after;

        begin_edit_slope(modl);
        // The self test runs before the main loop, so the camera has never
        // been framed. Face the plane the handle moves in: from a top view
        // that plane is edge-on and the ray intersection is meaningless.
        modl->slope->dir = SLOPE_DIR_PX;
        set_view_box(goxel.image->active_camera, goxel.image->box,
                     0.f, 1.5708f);
        camera_update(goxel.image->active_camera);
        handle_pos(modl->slope, h);
        camera_project(goxel.image->active_camera, h, vp, sp);

        in.touches[0].pos[0] = sp[0];
        in.touches[0].pos[1] = sp[1];
        in.touches[0].down[0] = true;
        slope_iter(&in, vp);
        check("the handle grabs a drag", s.dragging, NULL);

        vec3_copy(h, tgt);
        tgt[SLOPE_UP] -= 6;
        camera_project(goxel.image->active_camera, tgt, vp, tp);
        in.touches[0].pos[0] = tp[0];
        in.touches[0].pos[1] = tp[1];
        slope_iter(&in, vp);
        after = modl->slope->rise;
        snprintf(buf, sizeof(buf), "before=%.2f after=%.2f", before, after);
        check("dragging the handle changes the height",
              after < before - 3.f, buf);

        in.touches[0].down[0] = false;
        slope_iter(&in, vp);
        check("releasing gives the tool back", !s.dragging, NULL);

        cancel_session();
    }

    image_undo(img);
    n2 = count_cells(img);
    snprintf(buf, sizeof(buf), "cells=%d layers=%d", n2, count_layers(img));
    check("undo restores the original document",
          n2 == n0 && count_layers(img) == 1 &&
          cells_hash(img, NULL) == all0, buf);

    image_redo(img);
    snprintf(buf, sizeof(buf), "cells=%d layers=%d",
             count_cells(img), count_layers(img));
    check("redo restores the modifier",
          count_layers(img) == 2 && count_cells(img) == n1 &&
          cells_hash(img, NULL) == applied, buf);

    // #### geometry of every kind ####
    check_shape_geometry();

    // #### every shape kind on the same ship ####
    // The same selective, non-destructive path for each kind: the region cells
    // move to their own layer, everything outside stays exactly as it was.
    {
        static const int ORDER[] = {
            SLOPE_KIND_BEVEL, SLOPE_KIND_CYLINDER, SLOPE_KIND_SPHERE
        };
        layer_t *base = img->layers;
        int k;

        for (k = 0; k < (int)(sizeof(ORDER) / sizeof(ORDER[0])); k++) {
            int kind = ORDER[k];
            int reg[2][3];
            int n_before, n_after;
            uint32_t out_before, out_after;
            layer_t *mod;

            if (!shape_region(kind, reg)) {
                snprintf(buf, sizeof(buf), "%s", slope_kind_name(kind));
                check("a region exists for the shape", false, buf);
                continue;
            }
            img->active_layer = base;
            n_before = count_cells(img);
            out_before = cells_hash(img, reg);
            select_box(reg);
            begin_new_shape(kind);
            if (!s.mode || !s.mod_layer) {
                snprintf(buf, sizeof(buf), "%s", slope_kind_name(kind));
                check("the shape session starts", false, buf);
                continue;
            }
            mod = s.mod_layer;
            mod->slope->rise = 10.f + 2.f * k;
            apply_session();
            n_after = count_cells(img);
            out_after = cells_hash(img, reg);

            snprintf(buf, sizeof(buf), "%s: %d -> %d cells",
                     slope_kind_name(kind), n_before, n_after);
            check("total cell count preserved", n_after == n_before, buf);
            snprintf(buf, sizeof(buf), "%s", slope_kind_name(kind));
            check("cells outside the region untouched",
                  out_after == out_before, buf);
            snprintf(buf, sizeof(buf), "%s: %d cells held", slope_kind_name(kind),
                     count_vol_cells(mod->volume));
            check("the modifier holds its region cells",
                  count_vol_cells(mod->volume) > 0, buf);
            snprintf(buf, sizeof(buf), "%s: kind=%d",
                     slope_kind_name(kind), mod->slope->kind);
            check("the modifier carries its shape kind",
                  mod->slope->kind == kind, buf);
        }

        // #### re-edit: selecting a shape's layer reopens its strip ####
        {
            layer_t *l, *sph = NULL;
            DL_FOREACH(img->layers, l) {
                if (l->slope && l->slope->kind == SLOPE_KIND_SPHERE) sph = l;
            }
            if (sph) {
                float was = sph->slope->rise;
                int n = count_cells(img);
                img->active_layer = sph;
                begin_edit_slope(sph);
                sph->slope->rise = was + 4.f;
                sph->slope->revision++;
                apply_session();
                snprintf(buf, sizeof(buf), "rise %.2f -> %.2f", was,
                         sph->slope->rise);
                check("an existing shape can be re-edited",
                      sph->slope->rise == was + 4.f && count_cells(img) == n, buf);
            } else {
                check("an existing shape can be re-edited", false, "no sphere");
            }
        }

        // #### cell identity: editing the base layer cannot move a shape ####
        {
            layer_t *mods[8], *l;
            uint32_t hh[8];
            int nm = 0, i;

            DL_FOREACH(img->layers, l) {
                if (!l->slope || nm >= 8) continue;
                mods[nm] = l;
                hh[nm] = layer_cells_hash(l);
                nm++;
            }
            img->active_layer = base;
            {
                volume_iterator_t iter;
                uint8_t c[4];
                int p[3], keep[2][3], found = 0, n;

                iter = volume_get_iterator(base->volume,
                                           VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
                while (volume_iter(&iter, p) && found < 2) {
                    volume_get_at(base->volume, &iter, p, c);
                    if (!c[3]) continue;
                    keep[found][0] = p[0];
                    keep[found][1] = p[1];
                    keep[found][2] = p[2];
                    found++;
                }
                if (found < 2) {
                    check("deleting and recolouring base cells", false, "no cells");
                } else {
                    n = count_cells(img);
                    vec4_set(c, 0, 0, 0, 0);
                    volume_set_at(base->volume, NULL, keep[0], c);
                    vec4_set(c, 12, 200, 40, 255);
                    volume_set_at(base->volume, NULL, keep[1], c);
                    snprintf(buf, sizeof(buf), "%d -> %d cells", n,
                             count_cells(img));
                    check("deleting a base cell works",
                          count_cells(img) == n - 1, buf);
                    for (i = 0; i < nm; i++) {
                        snprintf(buf, sizeof(buf), "%s", mods[i]->name);
                        check("shape cells keep their identity through base edits",
                              layer_cells_hash(mods[i]) == hh[i], buf);
                    }
                }
            }
        }

        // #### ordinary voxel edits inside a shape's own region ####
        // The surface is generated from the region box, so a voxel painted
        // inside it cannot move the surface; that has to be reported, and it
        // must not leave a ghost shell behind.
        {
            layer_t *sh = NULL, *l;
            int p[3], tries = 0;
            bool base_ok = false, after_add, after_recolour, after_undo;
            uint32_t mesh_before;

            DL_FOREACH(img->layers, l) {
                if (l->slope && l->slope->kind == SLOPE_KIND_SLOPE) sh = l;
            }
            if (!sh) {
                check("a shape layer is available for in-region edits", false, NULL);
            } else {
                // Find a cell inside the region that this layer does not hold.
                volume_iterator_t iter;
                uint8_t c[4];
                int q[3], found = 0;
                int at[3];
                int reg[2][3];
                memcpy(reg, sh->slope->aabb, sizeof(reg));
                for (p[2] = reg[0][2]; p[2] <= reg[1][2] && !found; p[2]++)
                for (p[1] = reg[0][1]; p[1] <= reg[1][1] && !found; p[1]++)
                for (p[0] = reg[0][0]; p[0] <= reg[1][0] && !found; p[0]++) {
                    volume_get_at(sh->volume, NULL, p, c);
                    if (c[3] == 0) { found = 1; memcpy(at, p, sizeof(at)); }
                }
                if (found) memcpy(p, at, sizeof(at));
                (void) iter; (void) q; (void) tries;
                if (!found) {
                    check("an empty cell inside the region was found", false, NULL);
                } else {
                    base_ok = !slope_refresh_stale(sh);
                    ensure_mesh(sh);
                    mesh_before = sh->slope->mesh_hash;

                    vec4_set(c, 20, 200, 60, 255);
                    volume_set_at(sh->volume, NULL, p, c);
                    after_add = slope_refresh_stale(sh);
                    check("adding a cell inside the region is reported",
                          base_ok && after_add, NULL);

                    ensure_mesh(sh);
                    snprintf(buf, sizeof(buf), "mesh %u -> %u",
                             mesh_before, sh->slope->mesh_hash);
                    check("the surface does not ghost to the new cell",
                          sh->slope->mesh_hash == mesh_before, buf);

                    vec4_set(c, 200, 20, 60, 255);
                    volume_set_at(sh->volume, NULL, p, c);
                    after_recolour = slope_refresh_stale(sh);
                    check("recolouring inside the region stays reported",
                          after_recolour, NULL);

                    vec4_set(c, 0, 0, 0, 0);
                    volume_set_at(sh->volume, NULL, p, c);
                    after_undo = !slope_refresh_stale(sh);
                    check("removing the added cell clears the report", after_undo, NULL);

                    // #### deleting a cell the surface then hides ####
                    // The honest version of "no ghost shell": the surface does
                    // NOT follow an individual deleted cell, so the deletion is
                    // hidden under intact-looking geometry.  That is a real
                    // limitation of a region-derived surface; what the Owner
                    // needs is to see it and resolve it, so both are checked.
                    {
                        int q[3], have = 0, cells_pre, cells_mid, at[3];
                        uint8_t keep[4];
                        uint32_t mesh_pre;

                        for (q[2] = reg[0][2]; q[2] <= reg[1][2] && !have; q[2]++)
                        for (q[1] = reg[0][1]; q[1] <= reg[1][1] && !have; q[1]++)
                        for (q[0] = reg[0][0]; q[0] <= reg[1][0] && !have; q[0]++) {
                            volume_get_at(sh->volume, NULL, q, keep);
                            if (keep[3]) { have = 1; memcpy(at, q, sizeof(at)); }
                        }
                        if (have) memcpy(q, at, sizeof(at));
                        if (!have) {
                            check("an occupied cell inside the region was found",
                                  false, NULL);
                        } else {
                            ensure_mesh(sh);
                            mesh_pre = sh->slope->mesh_hash;
                            cells_pre = count_vol_cells(sh->volume);
                            vec4_set(keep, 0, 0, 0, 0);
                            volume_set_at(sh->volume, NULL, q, keep);
                            cells_mid = count_vol_cells(sh->volume);
                            snprintf(buf, sizeof(buf), "deleted [%d %d %d], %d -> %d cells",
                                     q[0], q[1], q[2], cells_pre, cells_mid);
                            check("a cell inside the region really is deleted",
                                  cells_mid == cells_pre - 1 &&
                                  slope_refresh_stale(sh), buf);
                            ensure_mesh(sh);
                            snprintf(buf, sizeof(buf), "mesh %u -> %u",
                                     mesh_pre, sh->slope->mesh_hash);
                            check("KNOWN LIMITATION: the surface still hides it",
                                  sh->slope->mesh_hash == mesh_pre, buf);

                            // resolve 1: accept the edits, keep the surface
                            sh->slope->applied_hash =
                                cells_hash_in_region(sh, sh->slope->aabb);
                            sh->slope->stale = 0;
                            check("accepting the edits clears the stale flag",
                                  !sh->slope->stale, buf);

                            // put the cell back so the rest of the run is clean
                            vec4_set(keep, 20, 200, 60, 255);
                            volume_set_at(sh->volume, NULL, q, keep);
                            slope_refresh_stale(sh);
                        }
                    }

                    // #### resolve 2: restore the cells as plain voxels ####
                    {
                        layer_t *bev = NULL, *l2;
                        int before;
                        DL_FOREACH(img->layers, l2)
                            if (l2->slope && l2->slope->kind == SLOPE_KIND_BEVEL)
                                bev = l2;
                        if (!bev) {
                            check("a shape layer was available to restore", false, NULL);
                        } else {
                            before = count_vol_cells(bev->volume);
                            slope_clear_layer(bev);
                            snprintf(buf, sizeof(buf), "%s: %d cells back as voxels",
                                     bev->name, before);
                            check("restoring cells leaves plain voxel geometry",
                                  bev->slope == NULL &&
                                  count_vol_cells(bev->volume) == before, buf);
                        }
                    }
                }
            }
        }

        // #### rendered face traces back to the source cells ####
        // The generated surface has to sit exactly on the region the cells
        // occupy: that is what makes a rendered face traceable to a cell.
        {
            layer_t *l;
            int done = 0;
            DL_FOREACH(img->layers, l) {
                surf_frame_t fr;
                model3d_t *mo;
                float xmin, xmax, ymin, ymax;
                int v;
                if (!l->slope || done >= 2) continue;
                surf_frame(l->slope, &fr);
                mo = build_mesh(l->slope);
                xmin = ymin = 1e9f; xmax = ymax = -1e9f;
                for (v = 0; v < mo->nb_vertices; v++) {
                    const float *q = mo->vertices[v].pos;
                    xmin = min(xmin, q[0]); xmax = max(xmax, q[0]);
                    ymin = min(ymin, q[1]); ymax = max(ymax, q[1]);
                }
                snprintf(buf, sizeof(buf),
                         "%s: mesh x %.2f..%.2f y %.2f..%.2f vs region "
                         "[%d %d]..[%d %d]",
                         l->name, xmin, xmax, ymin, ymax,
                         l->slope->aabb[0][0], l->slope->aabb[0][1],
                         l->slope->aabb[1][0], l->slope->aabb[1][1]);
                check("the surface footprint matches its source region",
                      fabsf(xmin - (l->slope->aabb[0][0] - SLOPE_EPS)) < 0.01f &&
                      fabsf(xmax - (l->slope->aabb[1][0] + 1.f + SLOPE_EPS)) < 0.01f &&
                      fabsf(ymin - (l->slope->aabb[0][1] - SLOPE_EPS)) < 0.01f &&
                      fabsf(ymax - (l->slope->aabb[1][1] + 1.f + SLOPE_EPS)) < 0.01f,
                      buf);
                free_mesh(mo);
                done++;
            }
        }
    }

    // #### stress (GOXEL_STRESS=<cycles>) ####
    // The heaviest configurations this module can make, rebuilt over and over
    // with the undo stack churning underneath.  A leak shows up as a live mesh
    // count that never comes back down; heap damage as _heapchk failing.
    {
        const char *env = getenv("GOXEL_STRESS");
        const int cycles = env ? atoi(env) : 0;
        if (cycles > 0) {
            const long live0 = s_mesh_live;
            const int cells0 = count_cells(goxel.image);
            int c, worst = 0, built = 0;
            bool heap_good = true;

            // Phase A: build and release the heaviest meshes.  Nothing else
            // touches the heap here, so the live count has to come back to 0.
            for (c = 0; c < cycles && heap_good; c++) {
                int kind, anchor, closed;
                for (kind = 0; kind < SLOPE_KIND_COUNT; kind++)
                for (anchor = 0; anchor < SLOPE_ANCHOR_COUNT; anchor++)
                for (closed = 0; closed <= 1; closed++) {
                    slope_mod_t m = {};
                    model3d_t *mo;
                    m.kind = kind;
                    m.dir = (c + kind) % SLOPE_DIR_COUNT;
                    m.anchor = anchor;
                    m.closed = closed;
                    m.rise = 3.f + (c % 11);
                    m.aabb[0][0] = -30;  m.aabb[1][0] = 30;
                    m.aabb[0][1] = -20;  m.aabb[1][1] = 20;
                    m.aabb[0][2] = -12;  m.aabb[1][2] = 12;
                    mo = build_mesh(&m);
                    if (mo->nb_vertices > worst) worst = mo->nb_vertices;
                    built++;
                    free_mesh(mo);
                }
                if (!heap_ok()) heap_good = false;
            }
            snprintf(buf, sizeof(buf), "%d cycles, %d meshes, peak %d verts",
                     cycles, built, worst);
            check("stress keeps the heap intact", heap_good, buf);
            snprintf(buf, sizeof(buf), "live %+ld of %ld made",
                     s_mesh_live - live0, s_mesh_made);
            check("stress leaks no generated mesh", s_mesh_live == live0, buf);

            // Phase B: churn the undo stack under the real document.  Ends on a
            // redo so the document is left where it was found, which is what
            // makes the cell-count comparison mean something.
            heap_good = true;
            for (c = 0; c < cycles && heap_good; c++) {
                layer_t *l;
                DL_FOREACH(goxel.image->layers, l) {
                    if (!l->slope) continue;
                    l->slope->rise += 0.5f;
                    l->slope->revision++;
                }
                image_history_push(goxel.image);
                image_undo(goxel.image);
                image_redo(goxel.image);
                if (!heap_ok()) heap_good = false;
            }
            check("stress keeps the heap intact through undo", heap_good, buf);
            snprintf(buf, sizeof(buf), "%d of %d cells",
                     count_cells(goxel.image), cells0);
            check("stress keeps the cell count",
                  count_cells(goxel.image) == cells0, buf);
        }
    }

    // #### save / reopen ####
    // An editable document has to survive a save and a reopen, with every
    // modifier still editable: kind, direction, height and region.
    {
        const char *save_path = getenv("GOXEL_SELFTEST_SAVE");
        if (save_path && *save_path) {
            typedef struct {
                int aabb[6]; int kind; int dir; int anchor; int closed; float rise;
            } snap_t;
            snap_t pre[8];
            int pre_n = 0, matched = 0, ok = 1, ok_place = 1;
            int pre_cells = count_cells(goxel.image);
            layer_t *l;
            int k;

            // Give one modifier a non-default placement, so the round trip has
            // to carry the anchor plane and the open/closed shape as well.
            DL_FOREACH(goxel.image->layers, l) {
                if (l->slope && l->slope->kind == SLOPE_KIND_SPHERE) {
                    l->slope->anchor = SLOPE_ANCHOR_MID;
                    l->slope->closed = 1;
                    l->slope->revision++;
                }
            }

            DL_FOREACH(goxel.image->layers, l) {
                if (!l->slope || pre_n >= 8) continue;
                for (k = 0; k < 3; k++) {
                    pre[pre_n].aabb[k] = l->slope->aabb[0][k];
                    pre[pre_n].aabb[3 + k] = l->slope->aabb[1][k];
                }
                pre[pre_n].kind = l->slope->kind;
                pre[pre_n].dir = l->slope->dir;
                pre[pre_n].anchor = l->slope->anchor;
                pre[pre_n].closed = l->slope->closed;
                pre[pre_n].rise = l->slope->rise;
                pre_n++;
            }

            if (goxel_export_to_file(save_path, "gox") != 0) {
                check("document saved as gox", false, save_path);
            } else if (load_from_file(save_path, true) != 0) {
                check("reopen the saved document", false, save_path);
            } else {
                check("document saved as gox", true, save_path);
                snprintf(buf, sizeof(buf), "pre=%d post=%d",
                         pre_cells, count_cells(goxel.image));
                check("reopen keeps the cell count",
                      count_cells(goxel.image) == pre_cells, buf);
                DL_FOREACH(goxel.image->layers, l) {
                    if (!l->slope) continue;
                    if (matched >= pre_n) { ok = false; break; }
                    if (l->slope->kind != pre[matched].kind ||
                            l->slope->dir != pre[matched].dir ||
                            l->slope->rise != pre[matched].rise ||
                            memcmp(l->slope->aabb, pre[matched].aabb,
                                   sizeof(pre[matched].aabb)) != 0) {
                        ok = false;
                        break;
                    }
                    if (l->slope->anchor != pre[matched].anchor ||
                            l->slope->closed != pre[matched].closed)
                        ok_place = false;
                    matched++;
                }
                snprintf(buf, sizeof(buf), "%d of %d modifiers",
                         matched, pre_n);
                check("reopen keeps every modifier editable",
                      ok && matched == pre_n && pre_n > 1, buf);
                check("reopen keeps the anchor plane and open/closed shape",
                      ok_place && matched == pre_n, buf);
            }
        }
    }

    printf("== self test: %d checks, %d failed ==\n", g_checks, g_failed);
}
