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
    float rise;
} slope_saved_t;

static struct {
    int           mode;
    layer_t      *src_layer;   // Layer the region cells were taken from.
    volume_t     *src_backup;  // Its content before the move.
    layer_t      *mod_layer;   // Layer carrying the modifier being edited.
    slope_saved_t saved;       // Parameters to restore on cancel.
    bool          dragging;
    tool_t       *saved_tool;  // Tool suspended while a handle is dragged.
    int           counter;
} s = {};

static tool_t s_noop_tool = {.name = "", .flags = 0};

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
    m->rise = other->rise;
    m->revision = other->revision;
    vec4_copy(other->color, m->color);
    return m;
}

void slope_free(slope_mod_t *mod)
{
    if (!mod) return;
    if (mod->mesh && goxel.graphics_initialized) model3d_delete(mod->mesh);
    free(mod);
}

uint32_t slope_hash(const slope_mod_t *mod)
{
    uint32_t key = 0;
    if (!mod) return 0;
    key = XXH32(&mod->aabb, sizeof(mod->aabb), key);
    key = XXH32(&mod->kind, sizeof(mod->kind), key);
    key = XXH32(&mod->dir, sizeof(mod->dir), key);
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

#define SLOPE_ARC_SEG   24  // Segments across a cylinder arch or a dome.
#define SLOPE_DOME_SEG  24  // Segments around a dome.
#define SLOPE_DOME_RINGS 12 // Rings from the base of a dome to its top.

// The frame every shape builder shares: the region footprint in world axes
// (inflated by SLOPE_EPS so it covers the neighbouring cube faces instead of
// z-fighting with them), the run/cross axes and the two planes between which
// the surface is built.  Every kind stands on the region's bottom plane, so a
// shape can never dip under the model it was made from.
typedef struct {
    int run, cross, sign;
    float lo[3], hi[3];
    float r_lo, r_hi;     // run coordinate of the low and the high edge
    float c_lo, c_hi;
    float bottom, low, top, rise;
} surf_frame_t;

static void surf_frame(const slope_mod_t *m, surf_frame_t *f)
{
    int i;
    run_axes(m->dir, &f->run, &f->cross, &f->sign);
    for (i = 0; i < 3; i++) {
        f->lo[i] = m->aabb[0][i];
        f->hi[i] = m->aabb[1][i] + 1.f;
    }
    f->lo[f->run] -= SLOPE_EPS;   f->hi[f->run] += SLOPE_EPS;
    f->lo[f->cross] -= SLOPE_EPS; f->hi[f->cross] += SLOPE_EPS;
    f->bottom = m->aabb[0][SLOPE_UP] - SLOPE_EPS;
    f->low = m->aabb[0][SLOPE_UP];
    f->rise = m->rise;
    f->top = m->aabb[0][SLOPE_UP] + m->rise;
    f->r_lo = (f->sign > 0) ? f->lo[f->run] : f->hi[f->run];
    f->r_hi = (f->sign > 0) ? f->hi[f->run] : f->lo[f->run];
    f->c_lo = f->lo[f->cross];
    f->c_hi = f->hi[f->cross];
}

static void add_bottom(const surf_frame_t *f, model3d_t *mo, int *vi)
{
    float p[4][3], out[3];
    set_pt(p[0], f->run, f->cross, f->r_lo, f->bottom, f->c_lo);
    set_pt(p[1], f->run, f->cross, f->r_hi, f->bottom, f->c_lo);
    set_pt(p[2], f->run, f->cross, f->r_hi, f->bottom, f->c_hi);
    set_pt(p[3], f->run, f->cross, f->r_lo, f->bottom, f->c_hi);
    vec3_set(out, 0, 0, 0); out[SLOPE_UP] = -1;
    add_quad(mo, vi, p, out);
}

// One planar face, four skirts down to the region bottom, closing bottom.
static void build_slope(const surf_frame_t *f, model3d_t *mo, int *vi)
{
    float p[4][3], out[3];

    // the slope face itself: a single plane, not a staircase
    set_pt(p[0], f->run, f->cross, f->r_lo, f->low, f->c_lo);
    set_pt(p[1], f->run, f->cross, f->r_hi, f->top, f->c_lo);
    set_pt(p[2], f->run, f->cross, f->r_hi, f->top, f->c_hi);
    set_pt(p[3], f->run, f->cross, f->r_lo, f->low, f->c_hi);
    vec3_set(out, 0, 0, 0); out[SLOPE_UP] = 1;
    add_quad(mo, vi, p, out);

    // skirt under the high edge
    set_pt(p[0], f->run, f->cross, f->r_hi, f->top, f->c_lo);
    set_pt(p[1], f->run, f->cross, f->r_hi, f->bottom, f->c_lo);
    set_pt(p[2], f->run, f->cross, f->r_hi, f->bottom, f->c_hi);
    set_pt(p[3], f->run, f->cross, f->r_hi, f->top, f->c_hi);
    vec3_set(out, 0, 0, 0); out[f->run] = f->sign;
    add_quad(mo, vi, p, out);

    // skirt under the low edge
    set_pt(p[0], f->run, f->cross, f->r_lo, f->low, f->c_lo);
    set_pt(p[1], f->run, f->cross, f->r_lo, f->bottom, f->c_lo);
    set_pt(p[2], f->run, f->cross, f->r_lo, f->bottom, f->c_hi);
    set_pt(p[3], f->run, f->cross, f->r_lo, f->low, f->c_hi);
    vec3_set(out, 0, 0, 0); out[f->run] = -f->sign;
    add_quad(mo, vi, p, out);

    // skirts on the two cross edges
    set_pt(p[0], f->run, f->cross, f->r_lo, f->low, f->c_lo);
    set_pt(p[1], f->run, f->cross, f->r_hi, f->top, f->c_lo);
    set_pt(p[2], f->run, f->cross, f->r_hi, f->bottom, f->c_lo);
    set_pt(p[3], f->run, f->cross, f->r_lo, f->bottom, f->c_lo);
    vec3_set(out, 0, 0, 0); out[f->cross] = -1;
    add_quad(mo, vi, p, out);

    set_pt(p[0], f->run, f->cross, f->r_lo, f->low, f->c_hi);
    set_pt(p[1], f->run, f->cross, f->r_hi, f->top, f->c_hi);
    set_pt(p[2], f->run, f->cross, f->r_hi, f->bottom, f->c_hi);
    set_pt(p[3], f->run, f->cross, f->r_lo, f->bottom, f->c_hi);
    vec3_set(out, 0, 0, 0); out[f->cross] = 1;
    add_quad(mo, vi, p, out);

    add_bottom(f, mo, vi);
}

// A 45 degree chamfer of the region's top edges: the top face is inset by the
// same distance it is raised, so the skirt is a real bevel and not a step.
static void build_bevel(const surf_frame_t *f, model3d_t *mo, int *vi)
{
    float p[4][3], out[3];
    float r_min = min(f->lo[f->run], f->hi[f->run]);
    float r_max = max(f->lo[f->run], f->hi[f->run]);
    float c_min = min(f->c_lo, f->c_hi);
    float c_max = max(f->c_lo, f->c_hi);
    float in_r = min(f->rise, (r_max - r_min) * 0.5f);
    float in_c = min(f->rise, (c_max - c_min) * 0.5f);

    // top face
    set_pt(p[0], f->run, f->cross, r_min + in_r, f->top, c_min + in_c);
    set_pt(p[1], f->run, f->cross, r_max - in_r, f->top, c_min + in_c);
    set_pt(p[2], f->run, f->cross, r_max - in_r, f->top, c_max - in_c);
    set_pt(p[3], f->run, f->cross, r_min + in_r, f->top, c_max - in_c);
    vec3_set(out, 0, 0, 0); out[SLOPE_UP] = 1;
    add_quad(mo, vi, p, out);

    // four chamfered skirts
    set_pt(p[0], f->run, f->cross, r_min, f->bottom, c_min);
    set_pt(p[1], f->run, f->cross, r_min + in_r, f->top, c_min + in_c);
    set_pt(p[2], f->run, f->cross, r_max - in_r, f->top, c_min + in_c);
    set_pt(p[3], f->run, f->cross, r_max, f->bottom, c_min);
    vec3_set(out, 0, 0, 0); out[f->cross] = -1;
    add_quad(mo, vi, p, out);

    set_pt(p[0], f->run, f->cross, r_min, f->bottom, c_max);
    set_pt(p[1], f->run, f->cross, r_min + in_r, f->top, c_max - in_c);
    set_pt(p[2], f->run, f->cross, r_max - in_r, f->top, c_max - in_c);
    set_pt(p[3], f->run, f->cross, r_max, f->bottom, c_max);
    vec3_set(out, 0, 0, 0); out[f->cross] = 1;
    add_quad(mo, vi, p, out);

    set_pt(p[0], f->run, f->cross, r_min, f->bottom, c_min);
    set_pt(p[1], f->run, f->cross, r_min + in_r, f->top, c_min + in_c);
    set_pt(p[2], f->run, f->cross, r_min + in_r, f->top, c_max - in_c);
    set_pt(p[3], f->run, f->cross, r_min, f->bottom, c_max);
    vec3_set(out, 0, 0, 0); out[f->run] = -1;
    add_quad(mo, vi, p, out);

    set_pt(p[0], f->run, f->cross, r_max, f->bottom, c_min);
    set_pt(p[1], f->run, f->cross, r_max - in_r, f->top, c_min + in_c);
    set_pt(p[2], f->run, f->cross, r_max - in_r, f->top, c_max - in_c);
    set_pt(p[3], f->run, f->cross, r_max, f->bottom, c_max);
    vec3_set(out, 0, 0, 0); out[f->run] = 1;
    add_quad(mo, vi, p, out);

    add_bottom(f, mo, vi);
}

// A half-cylinder arch across the cross axis, extruded along the run axis.
static void build_cylinder(const surf_frame_t *f, model3d_t *mo, int *vi)
{
    float arc[SLOPE_ARC_SEG + 1][3];
    float out[3], pivot[3];
    float r_a = f->lo[f->run], r_b = f->hi[f->run];
    float c_mid = (f->c_lo + f->c_hi) * 0.5f;
    float c_rad = fabsf(f->c_hi - f->c_lo) * 0.5f;
    int k;

    for (k = 0; k <= SLOPE_ARC_SEG; k++) {
        float ang = M_PI * k / SLOPE_ARC_SEG;
        set_pt(arc[k], f->run, f->cross, 0.f,
               f->bottom + f->rise * sinf(ang),
               c_mid + c_rad * cosf(ang));
    }

    // the arch itself, as a quad strip
    for (k = 0; k < SLOPE_ARC_SEG; k++) {
        float ang = M_PI * (k + 0.5f) / SLOPE_ARC_SEG;
        float q[4][3];
        set_pt(q[0], f->run, f->cross, r_a, arc[k][SLOPE_UP], arc[k][f->cross]);
        set_pt(q[1], f->run, f->cross, r_b, arc[k][SLOPE_UP], arc[k][f->cross]);
        set_pt(q[2], f->run, f->cross, r_b, arc[k+1][SLOPE_UP], arc[k+1][f->cross]);
        set_pt(q[3], f->run, f->cross, r_a, arc[k+1][SLOPE_UP], arc[k+1][f->cross]);
        vec3_set(out, 0, 0, 0);
        out[f->cross] = cosf(ang);
        out[SLOPE_UP] = sinf(ang);
        add_quad(mo, vi, q, out);
    }

    // the two half-ellipse ends, as fans from the arch pivot
    vec3_set(pivot, 0, 0, 0);
    pivot[f->run] = r_a;
    pivot[f->cross] = c_mid;
    pivot[SLOPE_UP] = f->bottom;
    vec3_set(out, 0, 0, 0); out[f->run] = -1;
    for (k = 0; k < SLOPE_ARC_SEG; k++)
        add_tri(mo, vi, pivot, arc[k], arc[k+1], out);

    pivot[f->run] = r_b;
    vec3_set(out, 0, 0, 0); out[f->run] = 1;
    for (k = 0; k < SLOPE_ARC_SEG; k++)
        add_tri(mo, vi, pivot, arc[k], arc[k+1], out);

    add_bottom(f, mo, vi);
}

// A dome: an elliptical shell over the region footprint, closed underneath.
static void build_sphere(const surf_frame_t *f, model3d_t *mo, int *vi)
{
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
    center[SLOPE_UP] = f->bottom;

    for (ring = 0; ring < SLOPE_DOME_RINGS; ring++) {
        float u0 = (M_PI / 2) * ring / SLOPE_DOME_RINGS;
        float u1 = (M_PI / 2) * (ring + 1) / SLOPE_DOME_RINGS;
        bool top = (ring == SLOPE_DOME_RINGS - 1);
        float s0 = cosf(u0), s1 = cosf(u1);

        for (k = 0; k < SLOPE_DOME_SEG; k++) {
            float a0 = 2 * M_PI * k / SLOPE_DOME_SEG;
            float a1 = 2 * M_PI * (k + 1) / SLOPE_DOME_SEG;
            set_pt(cur[k], f->run, f->cross,
                   r_mid + rad_r * s0 * cosf(a0),
                   f->bottom + f->rise * sinf(u0),
                   c_mid + rad_c * s0 * sinf(a0));
            // the crown ring collapses onto the top point
            set_pt(nxt[k], f->run, f->cross,
                   r_mid + (top ? 0.f : rad_r * s1 * cosf(a1)),
                   f->bottom + f->rise * sinf(u1),
                   c_mid + (top ? 0.f : rad_c * s1 * sinf(a1)));
        }
        for (k = 0; k < SLOPE_DOME_SEG; k++) {
            int nk = (k + 1) % SLOPE_DOME_SEG;
            if (top) {
                vec3_sub(nxt[k], center, out);
                add_tri(mo, vi, cur[k], nxt[k], cur[nk], out);
            } else {
                float q[4][3];
                float a0 = 2 * M_PI * (k + 0.5f) / SLOPE_DOME_SEG;
                vec3_set(out, 0, 0, 0);
                out[f->run]   = s0 * cosf(a0) / rad_r;
                out[f->cross] = s0 * sinf(a0) / rad_c;
                out[SLOPE_UP] = sinf(u0) / h;
                vec3_copy(cur[k], q[0]);
                vec3_copy(nxt[k], q[1]);
                vec3_copy(nxt[nk], q[2]);
                vec3_copy(cur[nk], q[3]);
                add_quad(mo, vi, q, out);
            }
        }
    }

    // close the base so the dome is a solid
    for (k = 0; k < SLOPE_DOME_SEG; k++) {
        float a0 = 2 * M_PI * k / SLOPE_DOME_SEG;
        set_pt(cur[k], f->run, f->cross,
               r_mid + rad_r * cosf(a0), f->bottom, c_mid + rad_c * sinf(a0));
    }
    for (k = 0; k < SLOPE_DOME_SEG; k++) {
        vec3_set(out, 0, 0, 0);
        out[SLOPE_UP] = -1;
        add_tri(mo, vi, center, cur[k], cur[(k + 1) % SLOPE_DOME_SEG], out);
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
    case SLOPE_KIND_CYLINDER: cap = SLOPE_ARC_SEG * 6 + 2 * SLOPE_ARC_SEG * 3 + 6; break;
    case SLOPE_KIND_SPHERE:   cap = (SLOPE_DOME_RINGS - 1) * SLOPE_DOME_SEG * 6 +
                                    2 * SLOPE_DOME_SEG * 3; break;
    default:                  cap = 6 * 6; break;
    }

    mo->vertices = calloc(cap, sizeof(*mo->vertices));
    mo->solid = true;
    mo->cull = true;
    mo->dirty = true;
    s_vert_budget = cap;

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

static void ensure_mesh(layer_t *layer)
{
    slope_mod_t *m = layer->slope;
    uint32_t hash = slope_hash(m);
    if (m->mesh && m->mesh_hash == hash) return;
    if (m->mesh) {
        model3d_delete(m->mesh);
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
        }
        model3d_render(layer->slope->mesh, layer->mat,
                       goxel.rend.view_mat, goxel.rend.proj_mat,
                       col, NULL, light, clip_box,
                       goxel.view_effects & EFFECT_NO_SHADING);
    }
}

static void handle_pos(const slope_mod_t *m, float out[3])
{
    int run, cross, sign;
    run_axes(m->dir, &run, &cross, &sign);
    vec3_set(out, 0, 0, 0);
    if (m->kind == SLOPE_KIND_SLOPE) {
        // On the high edge, where a plane is obviously being pulled up.
        out[run] = (sign > 0) ? m->aabb[1][run] + 1.f : (float)m->aabb[0][run];
    } else {
        // On the crown of the arch, dome or chamfer.
        out[run] = (m->aabb[0][run] + m->aabb[1][run] + 1.f) / 2.f;
    }
    out[cross] = (m->aabb[0][cross] + m->aabb[1][cross] + 1.f) / 2.f;
    out[SLOPE_UP] = m->aabb[0][SLOPE_UP] + m->rise;
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
    dst->rise = m->rise;
}

static void restore_params(slope_mod_t *m, const slope_saved_t *src)
{
    memcpy(m->aabb, src->aabb, sizeof(m->aabb));
    m->kind = src->kind;
    m->dir = src->dir;
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
    s.mode = S_MODE_NONE;
    s.mod_layer = NULL;
    s.src_layer = NULL;
    s.dragging = false;
    if (s.src_backup) {
        volume_delete(s.src_backup);
        s.src_backup = NULL;
    }
}

static void cancel_session(void)
{
    if (s.mode == S_MODE_NEW && s.src_layer && s.src_backup) {
        volume_set(s.src_layer->volume, s.src_backup);
        if (s.mod_layer) image_delete_layer(goxel.image, s.mod_layer);
    } else if (s.mode == S_MODE_EDIT && s.mod_layer) {
        restore_params(s.mod_layer->slope, &s.saved);
    }
    end_session();
}

static void apply_session(void)
{
    if (s.mod_layer) {
        s.mod_layer->slope->revision++;
        region_color(s.mod_layer->volume, s.mod_layer->slope->aabb,
                     s.mod_layer->slope->color);
    }
    end_session();
    image_history_push(goxel.image);
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
    goxel_add_hint(0, NULL, "Drag the handle, then Apply");
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
    m = s.mod_layer->slope;
    handle_pos(m, h);
    camera_project(goxel.image->active_camera, h, viewport, sp);
    mx = inputs->touches[0].pos[0];
    // The caller passes inputs already converted to GL convention (Y up).
    my = inputs->touches[0].pos[1];
    dx = mx - sp[0];
    dy = my - sp[1];

    if (!s.dragging) {
        if (inputs->touches[0].down[0] &&
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
    vec3_set(win, mx, my, 0);
    camera_get_ray(goxel.image->active_camera, win, viewport, o, d);
    run_axes(m->dir, &run, &cross, &sign);
    up[SLOPE_UP] = 1;
    runv[run] = 1;
    vec3_cross(up, runv, nrm);
    plane_from_normal(plane, h, nrm);
    if (!plane_line_intersection(plane, o, d, local)) return;
    mat4_mul_vec3(plane, local, world);

    // `rise` is measured from the region bottom, matching build_mesh().
    rise = world[SLOPE_UP] - m->aabb[0][SLOPE_UP];
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

void slope_gui(void)
{
    layer_t *active = goxel.image->active_layer;
    int aabb[2][3];
    float pos[2];
    slope_mod_t *m = NULL;
    static const char *DIR_NAMES[SLOPE_DIR_COUNT] = {"+X", "-X", "+Y", "-Y"};
    int i;

    if (!s.mode && active && active->slope) begin_edit_slope(active);
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
    gui_text("Drag the yellow handle in the view");
    gui_window_end();
}

// #### Driven demos #####################################################
// GOXEL_SLOPE_DEMO=1  one slope, end to end, the way the Owner does it
// GOXEL_SHAPE_DEMO=1  all four shape kinds on the same ship, one after another
// Both drive the same code path the editor uses, so the running editor can be
// captured without hijacking the mouse.

static bool s_demo = false;
static bool s_shape_demo = false;
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

static void slope_demo_step(void)
{
    int aabb[2][3];
    int f = goxel.frame_count;
    camera_t *cam = goxel.image->active_camera;

    if (!s_demo) return;
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
    static const char *dir = NULL;
    static int idx = 0;
    static const char *once = NULL;
    const int *at = s_shape_demo ? AT_SHAPES : AT;
    int nb = s_shape_demo ? (int)(sizeof(AT_SHAPES) / sizeof(AT_SHAPES[0]))
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

    if (s_shape_demo)
        snprintf(path, sizeof(path), "%s/shape%d.png", dir, idx);
    else
        snprintf(path, sizeof(path), "%s/frame%03d.png", dir, at[idx]);
    if (slope_grab(path)) {
        printf("slope: wrote %s\n", path);
        fflush(stdout);
    }
    idx++;
    // Once every requested frame is on disk there is nothing left to show.
    // The shapes demo keeps running so it can write its document out first.
    if (idx >= nb && !s_shape_demo) goxel.quit = true;
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

// Every kind has to produce a real closed surface: no NaN, nothing underneath
// the region it was made from, and the crown exactly where the parameter says.
static void check_shape_geometry(void)
{
    char buf[96];
    int kind;

    for (kind = 0; kind < SLOPE_KIND_COUNT; kind++) {
        slope_mod_t m = {};
        model3d_t *mo;
        float zmin = 1e9f, zmax = -1e9f;
        bool finite = true;
        int v;

        m.kind = kind;
        m.dir = SLOPE_DIR_PX;
        m.rise = 10.f;
        m.aabb[0][0] = 0;   m.aabb[1][0] = 12;
        m.aabb[0][1] = 0;   m.aabb[1][1] = 20;
        m.aabb[0][2] = -5;  m.aabb[1][2] = 3;

        mo = build_mesh(&m);
        for (v = 0; v < mo->nb_vertices; v++) {
            const float *p = mo->vertices[v].pos;
            if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
                finite = false;
            zmin = min(zmin, p[SLOPE_UP]);
            zmax = max(zmax, p[SLOPE_UP]);
        }
        snprintf(buf, sizeof(buf), "%s: %d vertices",
                 slope_kind_name(kind), mo->nb_vertices);
        check("shape builds a real surface", mo->nb_vertices > 0 && finite, buf);
        snprintf(buf, sizeof(buf), "%s: zmin %.3f floor %.3f",
                 slope_kind_name(kind), zmin, (float)m.aabb[0][2]);
        check("shape never dips under its region",
              zmin >= (float)m.aabb[0][2] - SLOPE_EPS - 1e-3f, buf);
        snprintf(buf, sizeof(buf), "%s: zmax %.3f want %.3f",
                 slope_kind_name(kind), zmax, (float)m.aabb[0][2] + m.rise);
        check("shape crown sits at the requested height",
              fabsf(zmax - ((float)m.aabb[0][2] + m.rise)) < 0.01f, buf);
        free(mo->vertices);
        free(mo);
        check("the heap is intact after the shape", heap_ok(),
              slope_kind_name(kind));
    }
}

void slope_selftest(void)
{
    image_t *img = goxel.image;
    int bbox[2][3], aabb[2][3], expected[2][3];
    layer_t *modl;
    int n0, n1, n2, region_cells;
    uint32_t all0, out0, out1, applied;
    char buf[128];

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
    }

    // #### save / reopen ####
    // An editable document has to survive a save and a reopen, with every
    // modifier still editable: kind, direction, height and region.
    {
        const char *save_path = getenv("GOXEL_SELFTEST_SAVE");
        if (save_path && *save_path) {
            typedef struct { int aabb[6]; int kind; int dir; float rise; } snap_t;
            snap_t pre[8];
            int pre_n = 0, matched = 0, ok = 1;
            int pre_cells = count_cells(goxel.image);
            layer_t *l;
            int k;

            DL_FOREACH(goxel.image->layers, l) {
                if (!l->slope || pre_n >= 8) continue;
                for (k = 0; k < 3; k++) {
                    pre[pre_n].aabb[k] = l->slope->aabb[0][k];
                    pre[pre_n].aabb[3 + k] = l->slope->aabb[1][k];
                }
                pre[pre_n].kind = l->slope->kind;
                pre[pre_n].dir = l->slope->dir;
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
                    matched++;
                }
                snprintf(buf, sizeof(buf), "%d of %d modifiers",
                         matched, pre_n);
                check("reopen keeps every modifier editable",
                      ok && matched == pre_n && pre_n > 1, buf);
            }
        }
    }

    printf("== self test: %d checks, %d failed ==\n", g_checks, g_failed);
}
