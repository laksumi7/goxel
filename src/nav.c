/* Goxel 3D voxels editor
 *
 * AMFM #541: the equipment navigator.  See nav.h.
 */

#include "nav.h"

#include "goxel.h"
#include "image.h"
#include "layer.h"
#include "slope.h"
#include "volume.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAV_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NAV_MAX(a, b) ((a) > (b) ? (a) : (b))

// #### SHA-256 ############################################################

// Compact FIPS 180-4.  Correctness of the pin matters, speed does not.

typedef struct {
    uint8_t  buf[64];
    uint32_t h[8];
    uint64_t total;     // bytes fed so far
    size_t   off;       // bytes in buf
} sha_ctx_t;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha_block(sha_ctx_t *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h, t1, t2, s0, s1;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | ((uint32_t)p[i*4+3]);
    for (i = 16; i < 64; i++) {
        s0 = ((w[i-15] >> 7)  | (w[i-15] << 25)) ^
             ((w[i-15] >> 18) | (w[i-15] << 14)) ^ (w[i-15] >> 3);
        s1 = ((w[i-2] >> 17)  | (w[i-2] << 15)) ^
             ((w[i-2] >> 19)  | (w[i-2] << 13)) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ((e >> 6)  | (e << 26)) ^ ((e >> 11) | (e << 21)) ^
                      ((e >> 25) | (e << 7));
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t S0 = ((a >> 2)  | (a << 30)) ^ ((a >> 13) | (a << 19)) ^
                      ((a >> 22) | (a << 10));
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + K[i] + w[i];
        t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha_put(sha_ctx_t *c, const uint8_t *p, size_t len)
{
    while (len) {
        size_t take = 64 - c->off;
        if (take > len) take = len;
        memcpy(c->buf + c->off, p, take);
        c->off += take;
        c->total += take;
        p += take;
        len -= take;
        if (c->off == 64) { sha_block(c, c->buf); c->off = 0; }
    }
}

static void sha_finish(sha_ctx_t *c, uint8_t out[32])
{
    uint64_t bits;
    uint8_t last[128];
    int i, at;

    bits = c->total * 8;
    at = (int)c->off;
    memset(last, 0, sizeof(last));
    memcpy(last, c->buf, (size_t)at);
    last[at++] = 0x80;
    if (at > 56) {
        sha_block(c, last);
        memset(last, 0, sizeof(last));
        at = 0;
    }
    while (at < 56) last[at++] = 0;
    for (i = 0; i < 8; i++)
        last[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_block(c, last);
    for (i = 0; i < 8; i++) {
        out[i*4+0] = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

static void sha_init(sha_ctx_t *c)
{
    static const uint32_t IV[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    int i;
    memset(c, 0, sizeof(*c));
    for (i = 0; i < 8; i++) c->h[i] = IV[i];
}

// The known-answer check runs from the NAV self-test (a wrong pin would be
// worse than no pin: it would bless a changed file).
bool nav_sha256_known_answer(void)
{
    sha_ctx_t c;
    uint8_t sha[32];
    char hex[65];
    bool ok = true;

    sha_init(&c);
    sha_finish(&c, sha);
    nav_sha256_hex(sha, hex);
    if (strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb924"
                    "27ae41e4649b934ca495991b7852b855")) ok = false;

    sha_init(&c);
    sha_put(&c, (const uint8_t *)"abc", 3);
    sha_finish(&c, sha);
    nav_sha256_hex(sha, hex);
    if (strcmp(hex, "ba7816bf8f01cfea414140de5dae2223"
                    "b00361a396177a9cb410ff61f20015ad")) ok = false;

    sha_init(&c);
    sha_put(&c, (const uint8_t *)"abcdbcdecdefdefgefghfghighijhi"
                                 "jkijkljklmklmnlmnomnopnopq", 56);
    sha_finish(&c, sha);
    nav_sha256_hex(sha, hex);
    if (strcmp(hex, "248d6a61d20638b8e5c026930c3e6039"
                    "a33ce45964ff2167f6ecedd419db06c1")) ok = false;

    return ok;
}

void nav_sha256_data(const void *data, size_t len, uint8_t out[32])
{
    sha_ctx_t c;
    sha_init(&c);
    sha_put(&c, data, len);
    sha_finish(&c, out);
}

int nav_sha256_file(const char *path, uint8_t out[32])
{
    FILE *f;
    uint8_t buf[31 * 1024];
    sha_ctx_t c;
    size_t n;

    f = fopen(path, "rb");
    if (!f) return -1;
    sha_init(&c);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha_put(&c, buf, n);
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    sha_finish(&c, out);
    return 0;
}

void nav_sha256_hex(const uint8_t in[32], char out[65])
{
    static const char hexd[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2 + 0] = hexd[(in[i] >> 4) & 15];
        out[i * 2 + 1] = hexd[in[i] & 15];
    }
    out[64] = 0;
}

// #### Registry ###########################################################

static nav_record_t *s_records;
static image_t *s_last_image;
static char s_doc[512];
static char s_open_doc[512];
static uint64_t s_seq;
static int s_count;

int nav_count(void) { return s_count; }

nav_record_t *nav_records(void) { return s_records; }

nav_record_t *nav_find(const char *id)
{
    nav_record_t *r;
    if (!id || !*id) return NULL;
    DL_FOREACH(s_records, r)
        if (!strcmp(r->id, id)) return r;
    return NULL;
}

static nav_record_t *nav_alloc(void)
{
    nav_record_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    DL_APPEND(s_records, r);
    r->seq = ++s_seq;
    s_count++;
    return r;
}

void nav_remove(nav_record_t *rec)
{
    if (!rec) return;
    DL_DELETE(s_records, rec);
    free(rec);
    s_count--;
}

const char *nav_kind_name(int kind)
{
    switch (kind) {
    case NAV_KIND_MUZZLE:  return "muzzle";
    case NAV_KIND_EXHAUST: return "exhaust";
    case NAV_KIND_PIVOT:   return "pivot";
    case NAV_KIND_HINGE:   return "hinge";
    default:               return "?";
    }
}

const char *nav_status_name(int status)
{
    switch (status) {
    case NAV_STATUS_DRAFT:     return "draft";
    case NAV_STATUS_CONFIRMED: return "confirmed";
    case NAV_STATUS_REVIEW:    return "needs_review";
    default:                   return "?";
    }
}

// The registry is per document: a new document is a new meaning space.
static void nav_reset(void)
{
    nav_record_t *r, *tmp;
    DL_FOREACH_SAFE(s_records, r, tmp) nav_remove(r);
    s_count = 0;
    s_seq = 0;
}

void nav_sidecar_path(const char *gox_path, char *buf, size_t buf_size)
{
    size_t n;
    if (!gox_path || !*gox_path) { snprintf(buf, buf_size, ""); return; }
    n = strlen(gox_path);
    if (n + 14 < buf_size && n >= 4 && !strcmp(gox_path + n - 4, ".gox"))
        snprintf(buf, buf_size, "%.*s.amfmnav.json", (int)(n - 4), gox_path);
    else
        snprintf(buf, buf_size, "%s.amfmnav.json", gox_path);
}

static void nav_basename(const char *path, char *buf, size_t size)
{
    const char *a, *b;
    size_t n;
    if (!path || !*path) { snprintf(buf, size, "ship"); return; }
    a = strrchr(path, '/');
    b = strrchr(path, '\\');
    if (b > a) a = b;
    snprintf(buf, size, "%s", a ? a + 1 : path);
    n = strlen(buf);
    if (n >= 4 && !strcmp(buf + n - 4, ".gox")) buf[n - 4] = 0;
}

// Strip everything a JSON round-trip would not survive.
static void nav_sanitize(char *buf, size_t size, const char *in)
{
    size_t i, n = 0;
    for (i = 0; in && in[i]; i++) {
        char c = in[i];
        if (c == '"' || c == '\\' || (unsigned char)c < 32) c = '_';
        buf[n++] = c;
        if (n >= size - 1) break;
    }
    buf[n] = 0;
}

// The shape axis of a modifier as a world unit vector (Goxel is Z-up).
void nav_dir_vec(int dir, float out[3])
{
    static const float AX[SLOPE_DIR_COUNT][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 },
    };
    if (dir < 0 || dir >= SLOPE_DIR_COUNT) { vec3_set(out, 0, 0, 1); return; }
    vec3_copy(AX[dir], out);
}

layer_t *nav_layer(int id)
{
    layer_t *l;
    if (!goxel.image) return NULL;
    DL_FOREACH(goxel.image->layers, l)
        if (l->id == id) return l;
    return NULL;
}

static void nav_fail(nav_record_t *r, const char *reason)
{
    if (r->status == NAV_STATUS_DRAFT) return;
    if (r->status == NAV_STATUS_REVIEW && !strcmp(r->reason, reason)) return;
    r->status = NAV_STATUS_REVIEW;
    snprintf(r->reason, sizeof(r->reason), "%s", reason);
}

// The world-space meaning of a record right now; either output may be NULL.
bool nav_ring(const nav_record_t *rec, float out_pos[3], float out_dir[3])
{
    layer_t *l;
    const slope_mod_t *m;
    float u[3];
    int a, i;

    if (!rec) return false;
    l = nav_layer(rec->layer_id);
    if (!l) return false;
    if (rec->at == NAV_AT_SHAPE_END) {
        m = l->slope;
        if (!m) return false;
        nav_dir_vec(m->dir, u);
        a = (m->dir == SLOPE_DIR_PX || m->dir == SLOPE_DIR_NX) ? 0 : 1;
        if (out_pos)
            for (i = 0; i < 3; i++)
                out_pos[i] = (i == a)
                    ? (rec->shape_sign > 0 ? (float)m->aabb[1][i] + 1.f
                                           : (float)m->aabb[0][i])
                    : (m->aabb[0][i] + m->aabb[1][i] + 1.f) / 2.f;
        if (out_dir)
            vec3_set(out_dir, u[0] * rec->shape_sign, u[1] * rec->shape_sign,
                     u[2] * rec->shape_sign);
        return true;
    }
    if (!l->volume) return false;
    if (volume_get_alpha_at(l->volume, NULL, rec->cell) == 0) return false;
    if (out_pos)
        for (i = 0; i < 3; i++) out_pos[i] = rec->cell[i] + 0.5f;
    if (out_dir) vec3_copy(rec->dir, out_dir);
    return true;
}

// #### Revalidation #######################################################

void nav_revalidate(void)
{
    nav_record_t *r;
    layer_t *l;
    const slope_mod_t *m;

    DL_FOREACH(s_records, r) {
        l = nav_layer(r->layer_id);
        if (!l) { nav_fail(r, "owning layer removed"); continue; }
        if (r->at == NAV_AT_SHAPE_END) {
            m = l->slope;
            if (!m) { nav_fail(r, "the surface modifier is gone"); continue; }
            if (m->stale)
                nav_fail(r, "source cells changed under the surface");
            if (m->closed != 1)
                nav_fail(r, "the end cap is open");
            if (!(m->rise > 0.f && m->rise <= 1.f))
                nav_fail(r, "the surface shape degenerated");
        } else {
            if (!l->volume ||
                volume_get_alpha_at(l->volume, NULL, r->cell) == 0) {
                nav_fail(r, "the anchor cell was removed");
                continue;
            }
        }
    }
}

// #### Transforms #########################################################

static bool s_replay_in_flight = false;

// A one-move replay of a selection transform: recompute the anchor from the
// committed snapshot with exactly the matrix the cells saw.  The image layer
// loop calls this once per moved layer per apply, so a dragging transform
// cannot accumulate: the snapshot never changes between begin and end.
static void cell_onto(const nav_record_t *r, const float mat[4][4], int out[3])
{
    float p[3], q[3];
    int i, j, mn[3], mx[3];

    for (i = 0; i < 8; i++) {
        for (j = 0; j < 3; j++)
            p[j] = (i & (1 << j)) ? r->cell0[j] + 1.f : (float)r->cell0[j];
        vec3_mul(p, 1.f, p);   // force no register aliasing surprises
        mat4_mul_vec3(mat, p, q);
        for (j = 0; j < 3; j++) {
            int v = (int)lroundf(q[j]);
            if (i == 0) { mn[j] = v; mx[j] = v; }
            else { mn[j] = NAV_MIN(mn[j], v); mx[j] = NAV_MAX(mx[j], v); }
        }
    }
    for (j = 0; j < 3; j++) out[j] = mn[j];
}

void nav_move_snap_layer(layer_t *layer, const float mat[4][4])
{
    nav_record_t *r;
    float d[3];

    if (!layer || !s_replay_in_flight) return;
    DL_FOREACH(s_records, r) {
        if (r->layer_id != layer->id || r->at != NAV_AT_CELL) continue;
        if (!r->_moving) continue;
        cell_onto(r, mat, r->cell);
        mat4_mul_dir3(mat, r->dir0, d);
        vec3_copy(d, r->dir);
    }
}

// A per-frame delta transform (the move tool, the mirror filter): apply the
// delta to the current anchor and re-base the snapshot, so the next delta
// continues exactly where the cells are now.
void nav_move_delta_layer(layer_t *layer, const float mat[4][4])
{
    nav_record_t *r;
    float p[3], q[3], d[3];
    int j;

    if (!layer) return;
    DL_FOREACH(s_records, r) {
        if (r->layer_id != layer->id || r->at != NAV_AT_CELL) continue;
        if (s_replay_in_flight && !r->_moving) continue;
        for (j = 0; j < 3; j++) p[j] = r->cell[j] + 0.5f;
        mat4_mul_vec3(mat, p, q);
        for (j = 0; j < 3; j++) {
            r->cell[j] = (int)floorf(q[j]);
            r->cell0[j] = r->cell[j];
        }
        mat4_mul_dir3(mat, r->dir0, d);
        vec3_copy(d, r->dir);
        vec3_copy(d, r->dir0);
    }
}

void nav_transform_begin(void)
{
    nav_record_t *r;
    DL_FOREACH(s_records, r) {
        if (r->at != NAV_AT_CELL) continue;
        memcpy(r->cell0, r->cell, sizeof(r->cell0));
        vec3_copy(r->dir, r->dir0);
        r->_moving = false;
    }
    s_replay_in_flight = true;
}

// Which records are anchored inside the mask of the transform in flight.
void nav_transform_mark(layer_t *layer)
{
    nav_record_t *r;
    volume_t *mask;
    if (!layer || !goxel.image || !goxel.image->selection_mask) return;
    mask = goxel.image->selection_mask;
    DL_FOREACH(s_records, r) {
        if (r->layer_id != layer->id || r->at != NAV_AT_CELL) continue;
        if (volume_get_alpha_at(mask, NULL, r->cell) != 0)
            r->_moving = true;
    }
}

void nav_transform_end(void)
{
    nav_record_t *r;
    if (!s_replay_in_flight) return;
    s_replay_in_flight = false;
    DL_FOREACH(s_records, r) {
        if (r->at != NAV_AT_CELL) continue;
        memcpy(r->cell0, r->cell, sizeof(r->cell0));
        vec3_copy(r->dir, r->dir0);
        r->_moving = false;
    }
}

// The mirror filter reflects the cells of a layer inside one axis span; the
// nav records follow, but only when their anchor is inside that span.
void nav_move_mirror_layer(layer_t *layer, int axis, const int aabb[2][3])
{
    nav_record_t *r;
    int j, c0, c1;

    if (!layer) return;
    DL_FOREACH(s_records, r) {
        if (r->layer_id != layer->id || r->at != NAV_AT_CELL) continue;
        if (s_replay_in_flight && !r->_moving) continue;
        c0 = r->cell[axis];
        if (c0 < aabb[0][axis] || c0 > aabb[1][axis]) continue;
        c1 = aabb[0][axis] + aabb[1][axis] - c0;
        r->cell[axis] = c1;
        r->cell0[axis] = c1;
        r->dir[axis] = -r->dir[axis];
        r->dir0[axis] = r->dir[axis];
        for (j = 0; j < 3; j++) r->cell0[j] = r->cell[j];
        vec3_copy(r->dir, r->dir0);
    }
}

// #### Creation ###########################################################

nav_record_t *nav_tag(const char *id, int kind, int at, const int token[3],
                      const char *group, const char *label, char *err,
                      size_t err_size)
{
    nav_record_t *r;
    layer_t *l;
    float u[3], tvec[3], d[3];
    int i, sign;
    int aabb[2][3];

#define FAIL(...) do { snprintf(err, err_size, ##__VA_ARGS__); return NULL; } while (0)

    if (!goxel.image || !goxel.image->active_layer)
        FAIL("no document");
    if (slope_session_is_open())
        FAIL("a shape session is open");
    if (kind < 0 || kind >= NAV_KIND_COUNT || at < 0 || at >= NAV_AT_COUNT)
        FAIL("bad kind/at");
    if (!token) FAIL("no axis token");
    if (!id || !*id || strlen(id) >= 64) FAIL("bad id");

    // The object is what the Owner selected; a layer named by a worker proposal
    // refines the same selection.
    l = goxel.image->active_layer;
    if (at == NAV_AT_SHAPE_END && !l->slope)
        FAIL("layer '%s' is not a shape layer", l->name);
    if (at == NAV_AT_CELL &&
        (!goxel.image->selection_mask ||
         !volume_get_bbox(goxel.image->selection_mask, aabb, false)))
        FAIL("no selection to anchor to");

    // A new tag with the same id replaces the unconfirmed one; a confirmed
    // record is never silently overwritten.
    r = nav_find(id);
    if (r && r->status != NAV_STATUS_DRAFT)
        FAIL("id already confirmed");
    if (!r) {
        r = nav_alloc();
        if (!r) FAIL("out of memory");
    }
    snprintf(r->id, sizeof(r->id), "%s", id);
    nav_sanitize(r->group, sizeof(r->group), group ? group : "");
    if (!r->group[0])
        nav_sanitize(r->group, sizeof(r->group), l->name);
    nav_sanitize(r->label, sizeof(r->label), label ? label : "");
    r->kind = kind;
    r->at = at;
    r->layer_id = l->id;
    r->status = NAV_STATUS_DRAFT;
    r->reason[0] = 0;
    r->seq = ++s_seq;

    if (at == NAV_AT_SHAPE_END) {
        // "+Y" means: the end face whose outward normal points along +Y.
        nav_dir_vec(l->slope->dir, u);
        vec3_set(tvec, (float)token[0], (float)token[1], (float)token[2]);
        sign = (vec3_dot(u, tvec) >= 0.f) ? 1 : -1;
        vec3_mul(u, (float)sign, d);
        if (fabsf(vec3_dot(d, tvec)) < 0.99f)
            FAIL("axis token is not parallel to this shape");
        r->shape_sign = sign;
        for (i = 0; i < 3; i++) r->cell[i] = 0;
    } else {
        // The anchor cell: the centre of the committed selection, literally
        // the cell the Owner pointed at (the selection includes its faces).
        int center[3];
        for (i = 0; i < 3; i++)
            center[i] = (int)floorf((aabb[0][i] + aabb[1][i] + 1.f) / 2.f);
        for (i = 0; i < 3; i++) r->cell[i] = center[i];
        // A concave selection can have an empty centre: anchor on the nearest
        // cell the layer actually occupies inside the selection instead, so
        // the marker sits on real geometry from the first draft.
        if (!volume_get_alpha_at(l->volume, NULL, r->cell)) {
            volume_iterator_t sit;
            int sp[3];
            uint8_t sc[4];
            int best[3] = {0, 0, 0};
            long bestd = LONG_MAX;
            bool found = false;
            sit = volume_get_iterator(goxel.image->selection_mask,
                                      VOLUME_ITER_VOXELS |
                                      VOLUME_ITER_SKIP_EMPTY);
            while (volume_iter(&sit, sp)) {
                long dx = sp[0] - center[0], dy = sp[1] - center[1],
                     dz = sp[2] - center[2];
                long d2 = dx * dx + dy * dy + dz * dz;
                volume_get_at(goxel.image->selection_mask, &sit, sp, sc);
                if (!sc[3]) continue;
                if (!volume_get_alpha_at(l->volume, NULL, sp)) continue;
                if (d2 < bestd) {
                    bestd = d2;
                    best[0] = sp[0]; best[1] = sp[1]; best[2] = sp[2];
                    found = true;
                }
            }
            if (!found) FAIL("no occupied cell in the selection to anchor to");
            for (i = 0; i < 3; i++) r->cell[i] = best[i];
        }
        vec3_set(tvec, (float)token[0], (float)token[1], (float)token[2]);
        if (fabsf(tvec[0]) + fabsf(tvec[1]) + fabsf(tvec[2]) < 0.5f)
            FAIL("bad direction");
        for (i = 0; i < 3; i++) {
            r->dir[i] = (tvec[i] > 0.5f) ? 1.f : (tvec[i] < -0.5f) ? -1.f : 0.f;
            r->cell0[i] = r->cell[i];
            r->dir0[i] = r->dir[i];
        }
        r->shape_sign = 0;
        if (!nav_ring(r, NULL, NULL))
            FAIL("anchor cell is empty on layer '%s'", l->name);
    }
    *err = 0;
    return r;
#undef FAIL
}

bool nav_confirm(nav_record_t *rec, char *err, size_t err_size)
{
    if (!rec) { snprintf(err, err_size, "no such record"); return false; }
    if (!nav_ring(rec, NULL, NULL)) {
        snprintf(err, err_size, "record '%s' has no geometry to pin", rec->id);
        return false;
    }
    rec->status = NAV_STATUS_CONFIRMED;
    rec->reason[0] = 0;
    if (rec->at == NAV_AT_CELL) {
        memcpy(rec->cell0, rec->cell, sizeof(rec->cell0));
        vec3_copy(rec->dir, rec->dir0);
    }
    return true;
}

// #### Sidecar JSON: the small grammar we write ourselves #################

static const char *json_after_key(const char *s, const char *key)
{
    char pat[64];
    const char *q;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    q = strstr(s, pat);
    if (!q) return NULL;
    q = strchr(q + strlen(pat), ':');
    if (!q) return NULL;
    q++;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
    return q;
}

static bool json_get_str(const char *s, const char *key,
                         char out[], size_t out_size)
{
    const char *q = json_after_key(s, key);
    size_t n = 0;
    if (!q || *q != '"') return false;
    q++;
    out[0] = 0;
    while (*q && *q != '"' && n + 1 < out_size) out[n++] = *q++;
    out[n] = 0;
    return *q == '"';
}

static bool json_get_int(const char *s, const char *key, int *out)
{
    const char *q = json_after_key(s, key);
    char *end;
    if (!q) return false;
    *out = (int)strtol(q, &end, 10);
    return end != q;
}

// "[ a, b, c ]" - ints or floats are both read with strtod.
static bool json_get_vec(const char *s, const char *key, float out[3])
{
    const char *q = json_after_key(s, key);
    int i;

    if (!q) return false;
    while (*q && *q != '[') q++;
    if (!*q) return false;
    q++;
    for (i = 0; i < 3; i++) {
        char *end;
        while (*q && (*q == ' ' || *q == ',' || *q == '\n' || *q == '\r' ||
                      *q == '\t')) q++;
        if (*q == ']') return false;
        out[i] = (float)strtod(q, &end);
        if (end == q) return false;
        q = end;
    }
    return true;
}

static int parse_kind(const char *name)
{
    int k;
    for (k = 0; k < NAV_KIND_COUNT; k++)
        if (!strcmp(name, nav_kind_name(k))) return k;
    return -1;
}

// Adopt a sidecar next to `gox_path` after verifying that pin.
int nav_adopt(const char *gox_path)
{
    char sidecar[600];
    char text[16 * 1024];
    char val[256], kind[24], at[16], status[24];
    uint8_t sha[32];
    char hex[65], pin[65];
    FILE *f;
    size_t n;
    const char *p;
    char *brace, *aend;
    bool pin_ok = true;
    int count = 0;

    if (!goxel.image || !gox_path || !*gox_path) return -1;
    nav_sidecar_path(gox_path, sidecar, sizeof(sidecar));
    f = fopen(sidecar, "rb");
    if (!f) { printf("nav: no sidecar %s\n", sidecar); fflush(stdout); return -1; }
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    if (n >= sizeof(text) - 1) {
        printf("nav: sidecar too large, rejected\n"); fflush(stdout);
        return -1;
    }

    pin[0] = 0;
    if (!nav_sha256_file(gox_path, sha)) {
        nav_sha256_hex(sha, hex);
        if (!json_get_str(text, "source_sha256", val, sizeof(val)) ||
            strcmp(val, hex)) {
            printf("nav: sidecar pin '%s' != gox sha %s: records need review\n",
                   val, hex);
            fflush(stdout);
            pin_ok = false;
        }
    } else {
        printf("nav: cannot hash the gox to adopt\n"); fflush(stdout);
        pin_ok = false;
    }

    p = strstr(text, "\"records\"");
    if (p) p = strchr(p, '[');
    if (!p) { printf("nav: sidecar has no records array\n"); fflush(stdout); return -1; }
    {
        // The records array holds nested [..] arrays; find the ] that
        // actually closes it by depth, not the first one.
        int depth = 0;
        char *q = p;
        for (; *q; q++) {
            if (*q == '[') depth++;
            else if (*q == ']') {
                depth--;
                if (!depth) break;
            }
        }
        aend = *q ? q : NULL;
    }
    if (!aend) { printf("nav: sidecar records never close\n"); fflush(stdout); return -1; }

    nav_reset();
    brace = p + 1;
    while ((brace = strchr(brace, '{')) && brace < aend) {
        char *bend = strchr(brace, '}');
        nav_record_t *r;
        int kindv, signv;

        if (!bend) break;
        r = nav_alloc();
        if (!r) return -1;
        *bend = 0;    // local scanning convenience restored below
        if (!json_get_str(brace, "id", val, sizeof(val)) || !val[0])
            snprintf(val, sizeof(val), "");
        snprintf(r->id, sizeof(r->id), "%s", val);
        json_get_str(brace, "group", val, sizeof(val));
        snprintf(r->group, sizeof(r->group), "%s", val);
        json_get_str(brace, "label", val, sizeof(val));
        snprintf(r->label, sizeof(r->label), "%s", val);
        json_get_int(brace, "layer_id", &r->layer_id);
        r->status = NAV_STATUS_REVIEW;
        snprintf(r->reason, sizeof(r->reason), "%s",
                 pin_ok ? "not a known record kind" :
                 "the sidecar does not pin this gox");
        kindv = json_get_str(brace, "kind", kind, sizeof(kind))
                    ? parse_kind(kind) : -1;
        if (!json_get_str(brace, "at", at, sizeof(at)))
            snprintf(at, sizeof(at), "");
        if (!json_get_str(brace, "status", status, sizeof(status)))
            snprintf(status, sizeof(status), "");
        *bend = '}';

        if (pin_ok && kindv >= 0 && !strcmp(status, "confirmed")) {
            if (!strcmp(at, "shape_end")) {
                signv = 0;
                json_get_int(brace, "shape_sign", &signv);
                if (signv == 1 || signv == -1) {
                    r->kind = kindv;
                    r->at = NAV_AT_SHAPE_END;
                    r->shape_sign = signv;
                    r->status = NAV_STATUS_CONFIRMED;
                    r->reason[0] = 0;
                    count++;
                }
            } else if (!strcmp(at, "cell")) {
                float cell[3] = {0, 0, 0}, dir[3] = {0, 0, 0};
                int i;
                if (json_get_vec(brace, "cell", cell) &&
                    json_get_vec(brace, "dir", dir))
                {
                    r->kind = kindv;
                    r->at = NAV_AT_CELL;
                    for (i = 0; i < 3; i++) {
                        r->cell[i] = r->cell0[i] = (int)floorf(cell[i]);
                        r->dir[i] = r->dir0[i] = dir[i];
                    }
                    r->status = NAV_STATUS_CONFIRMED;
                    r->reason[0] = 0;
                    count++;
                }
            }
        }
        brace = bend + 1;
    }
    nav_revalidate();
    snprintf(s_doc, sizeof(s_doc), "%s", gox_path);
    printf("nav: adopted %d record(s) from %s\n", count, sidecar);
    fflush(stdout);
    return count;
}

int nav_save(const char *path, char *err, size_t err_size)
{
    nav_record_t *r;
    uint8_t sha[32];
    char hex[65];
    char sidecar[600], tmp[640], base[256];
    FILE *f;
    int rc, total = 0, i = 0;

#define FAILS(...) do { snprintf(err, err_size, ##__VA_ARGS__); return -1; } while (0)

    if (!goxel.image) FAILS("no document");
    if (slope_session_is_open()) FAILS("a shape session is open");
    DL_FOREACH(s_records, r) {
        total++;
        if (r->status != NAV_STATUS_CONFIRMED)
            FAILS("record '%s' is %s%s%s: confirm or reject first",
                  r->id, nav_status_name(r->status),
                  r->reason[0] ? ": " : "", r->reason);
    }
    if (!path || !*path) FAILS("no path");

    rc = goxel_export_to_file((char *)path, "gox");
    if (rc) FAILS("gox save failed (%d)", rc);
    if (nav_sha256_file(path, sha)) FAILS("cannot hash the saved file");
    nav_sha256_hex(sha, hex);
    nav_sidecar_path(path, sidecar, sizeof(sidecar));
    snprintf(tmp, sizeof(tmp), "%s.tmp", sidecar);
    f = fopen(tmp, "wb");
    if (!f) FAILS("cannot write %s", tmp);

    nav_basename(path, base, sizeof(base));
    fprintf(f,
        "{\n \"format\": \"amfm.nav\",\n \"schema_version\": 1,\n"
        " \"asset_id\": \"%s\",\n \"source_gox\": \"%s\",\n"
        " \"source_sha256\": \"%s\",\n \"doc_key\": %u,\n"
        " \"records\": [\n",
        base, path, hex, (unsigned)image_get_key(goxel.image));
    DL_FOREACH(s_records, r) {
        fprintf(f,
            "  {\"id\": \"%s\", \"group\": \"%s\", \"label\": \"%s\",\n"
            "   \"kind\": \"%s\", \"at\": \"%s\", \"layer_id\": %d,\n",
            r->id, r->group, r->label, nav_kind_name(r->kind),
            r->at == NAV_AT_SHAPE_END ? "shape_end" : "cell", r->layer_id);
        if (r->at == NAV_AT_SHAPE_END)
            fprintf(f, "   \"shape_sign\": %d,\n", r->shape_sign);
        else
            fprintf(f,
                "   \"cell\": [%d, %d, %d],\n   \"dir\": [%.6f, %.6f, %.6f],\n",
                r->cell[0], r->cell[1], r->cell[2],
                r->dir[0], r->dir[1], r->dir[2]);
        fprintf(f, "   \"status\": \"confirmed\", \"seq\": %llu\n  }%s\n",
                (unsigned long long)r->seq, (++i < total) ? "," : "");
    }
    fprintf(f, " ]\n}\n");
    fclose(f);
    if (rename(tmp, sidecar) != 0) {
        remove(sidecar);
        if (rename(tmp, sidecar) != 0)
            FAILS("cannot move the sidecar into place");
    }
    snprintf(s_doc, sizeof(s_doc), "%s", path);
    printf("nav: saved %s + %s (gox sha %s)\n", path, sidecar, hex);
    fflush(stdout);
    return 0;
#undef FAILS
}

// #### Document tracking ##################################################

// The path of the document the registry currently belongs to: either what the
// notify hook heard, or what the loaded image itself carries.
void nav_notify_opened(const char *path)
{
    if (!path || !*path) return;
    snprintf(s_doc, sizeof(s_doc), "%s", path);
}

static void check_document_for(image_t *img)
{
    uint8_t sha[32];
    char hex[65], pin[65], sidecar[600];
    const char *doc = NULL;
    FILE *f;
    size_t n;
    char text[1024];

    if (!img) {
        if (s_last_image || s_doc[0] || nav_count()) {
            nav_reset();
            s_doc[0] = 0;
        }
        s_last_image = NULL;
        s_open_doc[0] = 0;
        return;
    }
    // The honest reopen path: whatever the editor believes it has open.  A
    // notify hook covers program-argument imports, the image.path covers the
    // file dialog and the save pair.
    doc = (img->path && img->path[0] && strcmp(img->path, "-"))
              ? img->path : s_doc;
    // The image pointer can be reused by a new document, so the path string
    // has to agree too before the registry is considered settled.
    if (img == s_last_image && !strcmp(doc ? doc : "", s_open_doc)) return;
    s_last_image = img;
    snprintf(s_open_doc, sizeof(s_open_doc), "%s", doc ? doc : "");
    if (doc && strcmp(doc, "-")) snprintf(s_doc, sizeof(s_doc), "%s", doc);

    // A record file is adopted only when the opened gox is byte-identical to
    // the one its pin sealed: a stale or foreign pair can never look fresh.
    if (doc && strcmp(doc, "-") && !nav_sha256_file(doc, sha)) {
        nav_sha256_hex(sha, hex);
        nav_sidecar_path(doc, sidecar, sizeof(sidecar));
        f = fopen(sidecar, "rb");
        if (f) {
            n = fread(text, 1, sizeof(text) - 1, f);
            fclose(f);
            text[n] = 0;
            pin[0] = 0;
            if (json_get_str(text, "source_sha256", pin, sizeof(pin)) &&
                !strcmp(pin, hex)) {
                nav_reset();
                nav_adopt(doc);
                return;
            }
        }
    }
    nav_reset();
}

void nav_check_document(void)
{
    check_document_for(goxel.image);
}

// #### Rendering ##########################################################

// An oriented cube frame: rows are half extents, row 3 the centre.
static void box_basis(const float pos[3], const float dir[3], float s,
                      float box[4][4])
{
    float u[3], v[3], w[3], a[3] = {0, 0, 1};
    int i;

    vec3_copy(dir, u);
    vec3_normalize(u, u);
    if (fabsf(vec3_dot(u, a)) > 0.92f) vec3_set(a, 1, 0, 0);
    vec3_cross(u, a, v);
    vec3_normalize(v, v);
    vec3_cross(u, v, w);
    for (i = 0; i < 3; i++) {
        box[0][i] = u[i] * s;
        box[1][i] = v[i] * s;
        box[2][i] = w[i] * s;
        box[3][i] = pos[i];
    }
}

static void record_color(const nav_record_t *r, bool bright, uint8_t col[4])
{
    switch (r->status) {
    case NAV_STATUS_DRAFT:
        col[0] = 200; col[1] = 200; col[2] = 230; break;
    case NAV_STATUS_CONFIRMED:
        switch (r->kind) {
        case NAV_KIND_MUZZLE:  col[0] = 255; col[1] = 140; col[2] = 0;   break;
        case NAV_KIND_EXHAUST: col[0] = 0;   col[1] = 190; col[2] = 255; break;
        case NAV_KIND_PIVOT:   col[0] = 255; col[1] = 220; col[2] = 0;   break;
        default:               col[0] = 255; col[1] = 120; col[2] = 180; break;
        }
        break;
    default:
        col[0] = 255; col[1] = 0; col[2] = 200; break;
    }
    col[3] = bright ? 255 : 150;
}

static void arrow_diagram(nav_record_t *r)
{
    float pos[3], dir[3], box[4][4], a[3], b[3], c2[3];
    uint8_t col[4];
    record_color(r, goxel.frame_count % 40 < 26, col);
    if (!nav_ring(r, pos, dir)) return;
    vec3_set(a, pos[0], pos[1], pos[2]);
    box_basis(a, dir, 0.30f, box);
    render_box(&goxel.rend, box, col,
               EFFECT_NO_DEPTH_TEST | EFFECT_UNLIT |
               (r->status != NAV_STATUS_CONFIRMED ? EFFECT_SEMI_TRANSPARENT : 0));
    vec3_set(b, pos[0] + dir[0] * 1.1f, pos[1] + dir[1] * 1.1f,
             pos[2] + dir[2] * 1.1f);
    render_line(&goxel.rend, a, b, col,
                EFFECT_NO_DEPTH_TEST | EFFECT_LINE_THICK);
    vec3_set(c2, b[0] + dir[0] * 0.18f, b[1] + dir[1] * 0.18f,
             b[2] + dir[2] * 0.18f);
    box_basis(c2, dir, 0.20f, box);
    render_box(&goxel.rend, box, col, EFFECT_NO_DEPTH_TEST | EFFECT_UNLIT);
}

void nav_render(void)
{
    nav_record_t *r;
    if (!goxel.image) return;
    nav_check_document();
    nav_revalidate();
    DL_FOREACH(s_records, r) arrow_diagram(r);
}

// #### GUI ################################################################

static int count_sel_cells(void)
{
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3], n = 0;
    if (!goxel.image || !goxel.image->selection_mask) return 0;
    iter = volume_get_iterator(goxel.image->selection_mask,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, p)) {
        volume_get_at(goxel.image->selection_mask, &iter, p, c);
        if (c[3]) n++;
    }
    return n;
}

void nav_gui(void)
{
    nav_record_t *r, *tmp;
    layer_t *l;
    float pos[3], dir[3];
    char err[128] = "";
    int i = 0;

    if (!goxel.image) return;
    nav_check_document();
    nav_revalidate();
    if (!s_records) return;

    gui_window_begin("Navigator",
                     goxel.screen_size[0] - 460.0f,
                     150.0f, 430, 0, GUI_WINDOW_MOVABLE);
    gui_panel_header("Navigator");
    gui_text("Selected: %s (%d cells)",
             goxel.image->active_layer ? goxel.image->active_layer->name : "-",
             count_sel_cells());
    DL_FOREACH_SAFE(s_records, r, tmp) {
        i++;
        gui_text("%d. %s '%s' [%s]", i, nav_kind_name(r->kind), r->id,
                 nav_status_name(r->status));
        if (nav_ring(r, pos, dir))
            gui_text("   at (%.1f %.1f %.1f) dir (%.1f %.1f %.1f)",
                     pos[0], pos[1], pos[2], dir[0], dir[1], dir[2]);
        l = nav_layer(r->layer_id);
        gui_text("   on '%s'%s%s", l ? l->name : "(gone)",
                 r->reason[0] ? ": " : "", r->reason);
        gui_group_begin("Owner decision");
        if (gui_button("Confirm", 0, 0)) {
            if (!nav_confirm(r, err, sizeof(err)))
                printf("nav: confirm %s refused: %s\n", r->id, err);
            fflush(stdout);
            err[0] = 0;
        }
        if (gui_button("Reject", 0, 0))
            nav_remove(r);
        gui_group_end();
    }
    gui_window_end();
}
