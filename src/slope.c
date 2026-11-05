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

#define SLOPE_EPS     0.004f  // Keeps the generated skirts off the neighbour faces.
#define SLOPE_PICK_PX 18.0f   // Handle grab radius, in screen pixels.
#define SLOPE_UP      2       // Goxel is Z-up.

enum { S_MODE_NONE = 0, S_MODE_NEW, S_MODE_EDIT };

// Persistent fields that a cancel has to restore (never memcpy slope_mod_t:
// it owns a GL mesh).
typedef struct {
    int   aabb[2][3];
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

// Append one quad as two triangles, wound so that the geometric normal faces
// the same way as `outward`.
static void add_quad(model3d_t *mo, int *vi, float p[4][3],
                     const float outward[3])
{
    static const int IDX[6] = {0, 1, 2, 2, 3, 0};
    float e1[3], e2[3], n[3], tmp[3];
    int i;

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
        vec3_set(v->uv, 0, 0, 0);
    }
}

// The generated surface: one planar slope face, four skirts down to the
// region bottom and a closing bottom face.  The footprint is the region's
// cell boundary, inflated by SLOPE_EPS so it covers the neighbouring cube
// faces instead of z-fighting with them.
static model3d_t *build_mesh(const slope_mod_t *m)
{
    model3d_t *mo = calloc(1, sizeof(*mo));
    float lo[3], hi[3], p[4][3], out[3];
    float h_high, h_low, bottom, r_lo, r_hi;
    int run, cross, sign, i, vi = 0;

    run_axes(m->dir, &run, &cross, &sign);
    for (i = 0; i < 3; i++) {
        lo[i] = m->aabb[0][i];
        hi[i] = m->aabb[1][i] + 1.f;
    }
    lo[run] -= SLOPE_EPS;   hi[run] += SLOPE_EPS;
    lo[cross] -= SLOPE_EPS; hi[cross] += SLOPE_EPS;

    // The wedge stands on the region's bottom plane: `rise` is the height of
    // the high edge above it, so the surface never dips under the model.
    bottom = m->aabb[0][SLOPE_UP] - SLOPE_EPS;
    h_low  = m->aabb[0][SLOPE_UP];
    h_high = m->aabb[0][SLOPE_UP] + m->rise;
    r_lo = (sign > 0) ? lo[run] : hi[run];
    r_hi = (sign > 0) ? hi[run] : lo[run];

    mo->nb_vertices = 6 * 6;
    mo->vertices = calloc(mo->nb_vertices, sizeof(*mo->vertices));
    mo->solid = true;
    mo->cull = true;
    mo->dirty = true;

    // 1) the slope face (a single plane, not a staircase).
    set_pt(p[0], run, cross, r_lo, h_low,  lo[cross]);
    set_pt(p[1], run, cross, r_hi, h_high, lo[cross]);
    set_pt(p[2], run, cross, r_hi, h_high, hi[cross]);
    set_pt(p[3], run, cross, r_lo, h_low,  hi[cross]);
    vec3_set(out, 0, 0, 0); out[SLOPE_UP] = 1;
    add_quad(mo, &vi, p, out);

    // 2) skirt under the high edge.
    set_pt(p[0], run, cross, r_hi, h_high, lo[cross]);
    set_pt(p[1], run, cross, r_hi, bottom, lo[cross]);
    set_pt(p[2], run, cross, r_hi, bottom, hi[cross]);
    set_pt(p[3], run, cross, r_hi, h_high, hi[cross]);
    vec3_set(out, 0, 0, 0); out[run] = sign;
    add_quad(mo, &vi, p, out);

    // 3) skirt under the low edge.
    set_pt(p[0], run, cross, r_lo, h_low,  lo[cross]);
    set_pt(p[1], run, cross, r_lo, bottom, lo[cross]);
    set_pt(p[2], run, cross, r_lo, bottom, hi[cross]);
    set_pt(p[3], run, cross, r_lo, h_low,  hi[cross]);
    vec3_set(out, 0, 0, 0); out[run] = -sign;
    add_quad(mo, &vi, p, out);

    // 4) + 5) skirts on the two cross edges.
    set_pt(p[0], run, cross, r_lo, h_low,  lo[cross]);
    set_pt(p[1], run, cross, r_hi, h_high, lo[cross]);
    set_pt(p[2], run, cross, r_hi, bottom, lo[cross]);
    set_pt(p[3], run, cross, r_lo, bottom, lo[cross]);
    vec3_set(out, 0, 0, 0); out[cross] = -1;
    add_quad(mo, &vi, p, out);

    set_pt(p[0], run, cross, r_lo, h_low,  hi[cross]);
    set_pt(p[1], run, cross, r_hi, h_high, hi[cross]);
    set_pt(p[2], run, cross, r_hi, bottom, hi[cross]);
    set_pt(p[3], run, cross, r_lo, bottom, hi[cross]);
    vec3_set(out, 0, 0, 0); out[cross] = 1;
    add_quad(mo, &vi, p, out);

    // 6) bottom, so the wedge is a closed solid.
    set_pt(p[0], run, cross, r_lo, bottom, lo[cross]);
    set_pt(p[1], run, cross, r_hi, bottom, lo[cross]);
    set_pt(p[2], run, cross, r_hi, bottom, hi[cross]);
    set_pt(p[3], run, cross, r_lo, bottom, hi[cross]);
    vec3_set(out, 0, 0, 0); out[SLOPE_UP] = -1;
    add_quad(mo, &vi, p, out);

    assert(vi == mo->nb_vertices);
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
    out[run]   = (sign > 0) ? m->aabb[1][run] + 1.f : (float)m->aabb[0][run];
    out[cross] = (m->aabb[0][cross] + m->aabb[1][cross] + 1.f) / 2.f;
    out[SLOPE_UP] = m->aabb[0][SLOPE_UP] + m->rise;
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

    // The slope direction, drawn along the generated face.
    run_axes(m->dir, &run, &cross, &sign);
    set_pt(a, run, cross,
           (sign > 0) ? m->aabb[0][run] : m->aabb[1][run] + 1.f,
           m->aabb[0][SLOPE_UP],
           (m->aabb[0][cross] + m->aabb[1][cross] + 1.f) / 2.f);
    set_pt(b, run, cross,
           (sign > 0) ? m->aabb[1][run] + 1.f : m->aabb[0][run],
           m->aabb[0][SLOPE_UP] + m->rise,
           (m->aabb[0][cross] + m->aabb[1][cross] + 1.f) / 2.f);
    render_line(&goxel.rend, a, b, col, EFFECT_NO_DEPTH_TEST);
    snprintf(hint, sizeof(hint), "rise %.2f", m->rise);
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
    dst->dir = m->dir;
    dst->rise = m->rise;
}

static void restore_params(slope_mod_t *m, const slope_saved_t *src)
{
    memcpy(m->aabb, src->aabb, sizeof(m->aabb));
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

static void begin_new_slope(void)
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
    snprintf(mod->name, sizeof(mod->name), "Slope %d", s.counter);
    mod->slope = slope_new();
    memcpy(mod->slope->aabb, aabb, sizeof(aabb));
    mod->slope->dir = SLOPE_DIR_PX;
    mod->slope->rise = (aabb[1][SLOPE_UP] + 1) - aabb[0][SLOPE_UP];

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
    // Sit next to the region, not on top of it.
    out[0] = clamp(sp[0] + 70.f, 8.f, goxel.screen_size[0] - 200.f);
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
        // Offer the operation next to the current selection.
        slope_mod_t tmp = {};
        memcpy(tmp.aabb, aabb, sizeof(aabb));
        strip_position(&tmp, pos);
        gui_window_begin("SlopeOps", pos[0], pos[1], 0, 0, 0);
        gui_text("Selection: %d cells region",
                 (aabb[1][0] - aabb[0][0] + 1) *
                 (aabb[1][1] - aabb[0][1] + 1) *
                 (aabb[1][2] - aabb[0][2] + 1));
        if (gui_button("Slope", 0, 0)) begin_new_slope();
        gui_text("(Bevel / Cylinder / Sphere: next)");
        gui_window_end();
        return;
    }

    if (!s.mode || !s.mod_layer) return;
    m = s.mod_layer->slope;
    strip_position(m, pos);
    gui_window_begin("SlopeOps", pos[0], pos[1], 0, 0, 0);
    gui_text("%s", s.mod_layer->name);
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

// #### Driven demo (GOXEL_SLOPE_DEMO=1) #################################
// Drives the same code path the Owner uses, so the running editor can be
// screenshotted without hijacking the mouse.

static bool s_demo = false;
static layer_t *s_demo_layer = NULL;

void slope_demo_begin(void)
{
    s_demo = true;
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

static void slope_demo_step(void)
{
    int aabb[2][3];
    int f = goxel.frame_count;
    camera_t *cam = goxel.image->active_camera;

    if (!s_demo) return;
    if (f == 20) {
        set_view(cam, .7f, 1.05f);
    } else if (f == 40) {
        if (demo_region(aabb)) {
            int h = aabb[1][SLOPE_UP] - aabb[0][SLOPE_UP] + 1;
            select_box(aabb);
            begin_new_slope();
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
    static const char *dir = NULL;
    static int idx = 0;
    static const char *once = NULL;
    char path[512];

    // GOXEL_SHOTS_ONCE=<file>: grab a single frame of an opened document.
    if (!once) once = getenv("GOXEL_SHOTS_ONCE") ?: "";
    if (*once && goxel.frame_count >= 60) {
        slope_grab(once);
        goxel.quit = true;
        return;
    }

    if (!dir) dir = getenv("GOXEL_SHOTS") ?: "";
    if (!*dir || idx >= (int)(sizeof(AT) / sizeof(AT[0]))) return;
    if (goxel.frame_count < AT[idx]) return;

    snprintf(path, sizeof(path), "%s/frame%03d.png", dir, AT[idx]);
    if (slope_grab(path)) {
        printf("slope: wrote %s\n", path);
        fflush(stdout);
    }
    idx++;
    // Once every requested frame is on disk there is nothing left to show.
    if (idx >= (int)(sizeof(AT) / sizeof(AT[0]))) goxel.quit = true;
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

    begin_new_slope();
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

    // #### save / reopen ####
    // An editable document has to survive a save and a reopen.
    {
        const char *save_path = getenv("GOXEL_SELFTEST_SAVE");
        if (save_path && *save_path) {
            layer_t *l;
            layer_t *src = NULL;
            int pre_cells = count_cells(goxel.image);
            int pre_aabb[6], post_aabb[6];
            int pre_dir = -1, i;
            float pre_rise = 0.f;

            DL_FOREACH(goxel.image->layers, l) {
                if (!l->slope) continue;
                src = l;
                pre_dir = l->slope->dir;
                pre_rise = l->slope->rise;
            }
            if (src) {
                for (i = 0; i < 3; i++) {
                    pre_aabb[i] = src->slope->aabb[0][i];
                    pre_aabb[3 + i] = src->slope->aabb[1][i];
                }
            }
            if (goxel_export_to_file(save_path, "gox") == 0) {
                check("document saved as gox", true, save_path);
                if (load_from_file(save_path, true) == 0) {
                    layer_t *back = NULL;
                    DL_FOREACH(goxel.image->layers, l) {
                        if (l->slope) back = l;
                    }
                    if (back) {
                        for (i = 0; i < 3; i++) {
                            post_aabb[i] = back->slope->aabb[0][i];
                            post_aabb[3 + i] = back->slope->aabb[1][i];
                        }
                    }
                    snprintf(buf, sizeof(buf), "pre=%d post=%d",
                             pre_cells, count_cells(goxel.image));
                    check("reopen keeps the cell count",
                          count_cells(goxel.image) == pre_cells, buf);
                    check("reopen keeps the modifier layer", back != NULL, NULL);
                    check("reopen keeps the modifier parameters",
                          back && back->slope->dir == pre_dir &&
                          back->slope->rise == pre_rise &&
                          memcmp(post_aabb, pre_aabb, sizeof(pre_aabb)) == 0,
                          NULL);
                } else {
                    check("reopen the saved document", false, save_path);
                }
            } else {
                check("document saved as gox", false, save_path);
            }
        }
    }

    printf("== self test: %d checks, %d failed ==\n", g_checks, g_failed);
}
