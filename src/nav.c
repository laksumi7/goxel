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
#ifdef _WIN32
#include <windows.h>
#endif

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
static uint64_t s_navrev;     // PM review #541 (7): registry mutation token
static int s_count;

int nav_count(void) { return s_count; }

uint64_t nav_revision(void) { return s_navrev; }

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
    r->fp_kind = -1;          // not a shape record until nav_tag says so
    s_count++;
    s_navrev++;
    return r;
}

void nav_remove(nav_record_t *rec)
{
    if (!rec) return;
    DL_DELETE(s_records, rec);
    free(rec);
    s_count--;
    s_navrev++;
}

const char *nav_kind_name(int kind)
{
    switch (kind) {
    case NAV_KIND_MUZZLE:  return "muzzle";
    case NAV_KIND_EXHAUST: return "exhaust";
    case NAV_KIND_PIVOT:   return "pivot";
    case NAV_KIND_HINGE:   return "hinge";
    case NAV_KIND_RCS:     return "rcs";
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
    s_navrev++;
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
    // PM review #541 follow-up: an invalidated draft is flagged too — an
    // orphaned draft must not sit invisible in the registry.
    if (r->status == NAV_STATUS_REVIEW && !strcmp(r->reason, reason)) return;
    r->status = NAV_STATUS_REVIEW;
    snprintf(r->reason, sizeof(r->reason), "%s", reason);
}

// PM review #541 (1,5): the Z plane the end cap is actually centred on.  The
// generated surface measures from the anchor plane (surf_frame in slope.c):
// a closed barrel or ellipsoid is centred on `centre`, an open section sits
// on its base plane.  The region mid-plane is only right for a Mid anchor.
static float slope_cap_z(const slope_mod_t *m)
{
    const float floor_p = (float)m->aabb[0][2];
    const float ceil_p = (float)m->aabb[1][2] + 1.f;

    if (m->closed && (m->kind == SLOPE_KIND_CYLINDER ||
                      m->kind == SLOPE_KIND_SPHERE)) {
        switch (m->anchor) {
        case SLOPE_ANCHOR_CEIL: return ceil_p - m->rise;
        case SLOPE_ANCHOR_MID:  return (floor_p + ceil_p) * 0.5f;
        default:                return floor_p + m->rise;
        }
    }
    switch (m->anchor) {
    case SLOPE_ANCHOR_CEIL: return ceil_p;
    case SLOPE_ANCHOR_MID:  return (floor_p + ceil_p) * 0.5f - m->rise * 0.5f;
    default:                return floor_p;
    }
}

// The world-space meaning of a record right now; either output may be NULL.
bool nav_shape_end(const nav_record_t *rec, int sign, float out_pos[3],
                   float out_dir[3])
{
    layer_t *l;
    const slope_mod_t *m;
    float u[3];
    int a, i, phys;

    if (!rec) return false;
    l = nav_layer(rec->layer_id);
    if (!l) return false;
    if (rec->at != NAV_AT_SHAPE_END) return false;
    m = l->slope;
    if (!m) return false;
    nav_dir_vec(m->dir, u);
    a = (m->dir == SLOPE_DIR_PX || m->dir == SLOPE_DIR_NX) ? 0 : 1;
    // PM review #541 (1): shape_sign is relative to the signed axis, so
    // the physical face is the max-coordinate one only when the signed
    // axis points at it.  On an authored NX/NY shape the axis points at
    // the min face, and the marker has to follow the axis, not the sign.
    phys = (u[a] * (float)sign > 0.f) ? 1 : -1;
    if (out_pos)
        for (i = 0; i < 3; i++) {
            if (i == a)
                out_pos[i] = phys > 0 ? (float)m->aabb[1][i] + 1.f
                                      : (float)m->aabb[0][i];
            else if (i == 2)
                out_pos[i] = slope_cap_z(m);
            else
                out_pos[i] = (m->aabb[0][i] + m->aabb[1][i] + 1.f) / 2.f;
        }
    if (out_dir)
        vec3_set(out_dir, u[0] * sign, u[1] * sign, u[2] * sign);
    return true;
}

bool nav_ring(const nav_record_t *rec, float out_pos[3], float out_dir[3])
{
    layer_t *l;
    int i;

    if (!rec) return false;
    if (rec->at == NAV_AT_SHAPE_END)
        return nav_shape_end(rec, rec->shape_sign, out_pos, out_dir);
    l = nav_layer(rec->layer_id);
    if (!l) return false;
    if (!l->volume) return false;
    if (volume_get_alpha_at(l->volume, NULL, rec->cell) == 0) return false;
    if (out_pos)
        for (i = 0; i < 3; i++) out_pos[i] = rec->cell[i] + 0.5f;
    if (out_dir) vec3_copy(rec->dir, out_dir);
    return true;
}

// #### Revalidation #######################################################

// PM review #541 (3,4): one record's meaning against the live geometry.
// Returns true when the current authored geometry still supports what the
// record says; `why` explains the first failure.  Revalidation and the
// confirm/publication gates share this single authority.
static bool nav_record_valid(const nav_record_t *r, char *why, size_t why_size)
{
    layer_t *l = nav_layer(r->layer_id);
    const slope_mod_t *m;
    int i, ext[3];

    if (why && why_size) why[0] = 0;
    if (!l) { snprintf(why, why_size, "owning layer removed"); return false; }
    if (r->at == NAV_AT_SHAPE_END) {
        m = l->slope;
        if (!m) { snprintf(why, why_size, "the surface modifier is gone"); return false; }
        if (m->stale) {
            snprintf(why, why_size, "source cells changed under the surface");
            return false;
        }
        if (m->closed != 1) { snprintf(why, why_size, "the end cap is open"); return false; }
        if (!(m->rise > 0.f && m->rise <= 1.f)) {
            snprintf(why, why_size, "the surface shape degenerated");
            return false;
        }
        // PM review #541 (4): the semantic attachment.  A rigid move keeps
        // kind, run axis, caps, anchor, rise and region size; re-authoring
        // any of them means a different physical muzzle, so the record has
        // to be re-anchored, not silently kept green.  A sidecar written
        // before this fingerprint (fp_kind < 0 after adopt) cannot be
        // compared and is left to the other checks.
        if (r->fp_kind >= 0) {
            for (i = 0; i < 3; i++) ext[i] = m->aabb[1][i] - m->aabb[0][i] + 1;
            if (m->kind != r->fp_kind || m->dir != r->fp_dir ||
                m->closed != r->fp_closed || m->anchor != r->fp_anchor ||
                fabsf(m->rise - r->fp_rise) > 1e-4f ||
                ext[0] != r->fp_ext[0] || ext[1] != r->fp_ext[1] ||
                ext[2] != r->fp_ext[2]) {
                snprintf(why, why_size, "the shape was re-authored");
                return false;
            }
        }
        return true;
    }
    if (!l->volume || volume_get_alpha_at(l->volume, NULL, r->cell) == 0) {
        snprintf(why, why_size, "the anchor cell was removed");
        return false;
    }
    return true;
}

// PM rev3 R3-2: the cached `stale` flag is only refreshed by the shape's own
// edit paths, so a gate that publishes must ask the source again right now.
// Only ever called on an action (Confirm, publish) — never per frame — so
// hashing the shape's own layer region here is affordable.
static bool nav_fresh_valid(nav_record_t *r, char *why, size_t why_size)
{
    layer_t *l = nav_layer(r->layer_id);
    if (r->at == NAV_AT_SHAPE_END && l && l->slope)
        slope_refresh_stale(l);
    return nav_record_valid(r, why, why_size);
}

void nav_revalidate(void)
{
    nav_record_t *r;
    char why[96];

    DL_FOREACH(s_records, r) {
        if (!nav_record_valid(r, why, sizeof(why))) nav_fail(r, why);
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
        if (r->layer_id != layer->id) continue;
        if (s_replay_in_flight && !r->_moving) continue;
        if (r->at != NAV_AT_CELL) {
            // PM review #541 (4): slope_move_surface has already turned the
            // modifier's signed axis with the surface.  When that is exactly
            // the flip this mirror implies, keep the fingerprint in step so
            // a legitimate mirror is not mistaken for a re-author; anything
            // else is left for revalidation to flag.
            if (layer->slope && r->fp_kind >= 0) {
                int run = (layer->slope->dir == SLOPE_DIR_PX ||
                           layer->slope->dir == SLOPE_DIR_NX) ? 0 : 1;
                if (run == axis && layer->slope->dir == (r->fp_dir ^ 1))
                    r->fp_dir = layer->slope->dir;
            }
            continue;
        }
        // PM review #541 (2): volume_mirror reflects voxel c inside the
        // half-open span [aabb[0], aabb[1]) to aabb[0]+aabb[1]-1-c; the
        // marker has to land on the surviving cell, not one past it.
        c0 = r->cell[axis];
        if (c0 < aabb[0][axis] || c0 >= aabb[1][axis]) continue;
        c1 = aabb[0][axis] + aabb[1][axis] - 1 - c0;
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
    nav_record_t t, *r;
    layer_t *l;
    float u[3], tvec[3], d[3];
    int i, sign;
    int aabb[2][3];

#define FAIL(...) do { snprintf(err, err_size, ##__VA_ARGS__); return NULL; } while (0)

    if (!goxel.image || !goxel.image->active_layer)
        FAIL("no document");
    // PM #541 GUI finding: picking a shape layer opens the editing strip by
    // design, so a merely-open strip must not block tagging; only parameters
    // the Owner has actually moved (or a shape still being created) may.
    if (slope_session_is_dirty())
        FAIL("a shape session has uncommitted edits");
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

    // PM rev3 R3-1: everything is computed and checked into a temporary
    // first.  A rejected tag must not create, change or confirm anything —
    // the old code appended the draft before validating the axis token, so
    // a failed request could leave a half-assigned draft that a later
    // Confirm would publish as a wrong muzzle.
    memset(&t, 0, sizeof(t));
    t.fp_kind = -1;           // a cell record carries no shape fingerprint
    snprintf(t.id, sizeof(t.id), "%s", id);
    nav_sanitize(t.group, sizeof(t.group), group ? group : "");
    if (!t.group[0])
        nav_sanitize(t.group, sizeof(t.group), l->name);
    nav_sanitize(t.label, sizeof(t.label), label ? label : "");
    t.kind = kind;
    t.at = at;
    t.layer_id = l->id;
    t.status = NAV_STATUS_DRAFT;
    t.reason[0] = 0;
    t.pin_mismatch = false;

    if (at == NAV_AT_SHAPE_END) {
        // "+Y" means: the end face whose outward normal points along +Y.
        nav_dir_vec(l->slope->dir, u);
        vec3_set(tvec, (float)token[0], (float)token[1], (float)token[2]);
        sign = (vec3_dot(u, tvec) >= 0.f) ? 1 : -1;
        vec3_mul(u, (float)sign, d);
        if (fabsf(vec3_dot(d, tvec)) < 0.99f)
            FAIL("axis token is not parallel to this shape");
        t.shape_sign = sign;
        for (i = 0; i < 3; i++) t.cell[i] = 0;
        // PM review #541 (4): capture the semantic attachment of this very
        // modifier so a later re-authoring cannot hide behind a green mark.
        t.fp_kind = l->slope->kind;
        t.fp_dir = l->slope->dir;
        t.fp_closed = l->slope->closed;
        t.fp_anchor = l->slope->anchor;
        t.fp_rise = l->slope->rise;
        for (i = 0; i < 3; i++)
            t.fp_ext[i] = l->slope->aabb[1][i] - l->slope->aabb[0][i] + 1;
        if (!nav_ring(&t, NULL, NULL))
            FAIL("the shape cannot carry this marker right now");
    } else {
        // The anchor cell: the centre of the committed selection, literally
        // the cell the Owner pointed at (the selection includes its faces).
        int center[3];
        for (i = 0; i < 3; i++)
            center[i] = (int)floorf((aabb[0][i] + aabb[1][i] + 1.f) / 2.f);
        // PM review #541 (8): the centre only counts when it is inside the
        // selection itself — an irregular or disconnected selection must not
        // anchor its marker on a cell nobody selected.
        if (volume_get_alpha_at(goxel.image->selection_mask, NULL, center)) {
            for (i = 0; i < 3; i++) t.cell[i] = center[i];
        }
        if (!volume_get_alpha_at(goxel.image->selection_mask, NULL, t.cell) ||
            !volume_get_alpha_at(l->volume, NULL, t.cell)) {
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
            for (i = 0; i < 3; i++) t.cell[i] = best[i];
        }
        vec3_set(tvec, (float)token[0], (float)token[1], (float)token[2]);
        if (fabsf(tvec[0]) + fabsf(tvec[1]) + fabsf(tvec[2]) < 0.5f)
            FAIL("bad direction");
        for (i = 0; i < 3; i++) {
            t.dir[i] = (tvec[i] > 0.5f) ? 1.f : (tvec[i] < -0.5f) ? -1.f : 0.f;
            t.cell0[i] = t.cell[i];
            t.dir0[i] = t.dir[i];
        }
        t.shape_sign = 0;
        if (!nav_ring(&t, NULL, NULL))
            FAIL("anchor cell is empty on layer '%s'", l->name);
    }

    // Every check passed: only now does the registry move (PM rev3 R3-1).
    if (!r) {
        r = nav_alloc();
        if (!r) FAIL("out of memory");
    }
    {
        struct nav_record *pv = r->prev, *nx = r->next;
        bool mv = r->_moving;
        *r = t;
        r->prev = pv;
        r->next = nx;
        r->_moving = mv;
    }
    r->seq = ++s_seq;
    s_navrev++;               // the registry moved, even onto an old draft id
    *err = 0;
    return r;
#undef FAIL
}

bool nav_confirm(nav_record_t *rec, char *err, size_t err_size)
{
    char why[96];

    if (!rec) { snprintf(err, err_size, "no such record"); return false; }
    // PM review #541 (3): Confirm is a publication gate, not a button that
    // blesses whatever is on screen.  The current authored geometry has to
    // support the meaning right now; a meaning that was invalidated needs a
    // deliberate untag + tag re-anchor, never a second click.  PM rev3
    // R3-2: the shape's source cells are asked afresh, not from the cache.
    if (!nav_fresh_valid(rec, why, sizeof(why))) {
        snprintf(err, err_size, "record '%s' cannot be confirmed: %s; "
                 "untag and tag again to re-anchor", rec->id, why);
        return false;
    }
    if (rec->status == NAV_STATUS_REVIEW && !rec->pin_mismatch) {
        snprintf(err, err_size, "record '%s' changed meaning (%s): untag and "
                 "tag again to re-anchor", rec->id, rec->reason);
        return false;
    }
    // A pin_mismatch review is the document-level mismatch from adopt: the
    // Owner clicking Confirm here is the explicit acceptance of the changed
    // gox, so it clears — the geometry itself just validated clean.
    rec->status = NAV_STATUS_CONFIRMED;
    rec->reason[0] = 0;
    rec->pin_mismatch = false;
    s_navrev++;
    if (rec->at == NAV_AT_CELL) {
        memcpy(rec->cell0, rec->cell, sizeof(rec->cell0));
        vec3_copy(rec->dir, rec->dir0);
    }
    return true;
}

// #### Sidecar JSON: the small grammar we write ourselves #################

// PM review #541 (5): strings are written as proper JSON.  A Windows path
// carries backslashes, so raw insertion produced a malformed file; escaping
// is mandatory for every string, and the reader has to undo it.
static void json_escape(const char *in, char *out, size_t size)
{
    size_t n = 0;
    unsigned char c;

    if (size) out[0] = 0;
    for (; in && (c = (unsigned char)*in) != 0; in++) {
        char tmp[8];
        const char *add;
        if (c == '"' || c == '\\') { tmp[0] = '\\'; tmp[1] = (char)c; tmp[2] = 0; add = tmp; }
        else if (c == '\n') add = "\\n";
        else if (c == '\r') add = "\\r";
        else if (c == '\t') add = "\\t";
        else if (c < 32) { snprintf(tmp, sizeof(tmp), "\\u%04x", c); add = tmp; }
        else { tmp[0] = (char)c; tmp[1] = 0; add = tmp; }
        if (n + strlen(add) + 1 > size) break;
        while (*add) out[n++] = *add++;
    }
    out[n] = 0;
}

// Undo json_escape.  \uXXXX is encoded as UTF-8 (the sidecar is UTF-8).
static void json_unescape(const char *in, char *out, size_t size)
{
    size_t n = 0;
    char c;

    if (size) out[0] = 0;
    for (; (c = *in) != 0; in++) {
        if (c != '\\') {
            if (n + 2 > size) break;
            out[n++] = c;
            continue;
        }
        in++;
        if (!*in) break;
        switch (*in) {
        case '"': case '\\': case '/': c = *in; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u': {
            unsigned int cp = 0;
            int k;
            for (k = 0; k < 4 && in[1 + k]; k++) {
                char h = in[1 + k];
                unsigned int d = (h >= '0' && h <= '9') ? (unsigned)(h - '0')
                             : (h >= 'a' && h <= 'f') ? (unsigned)(h - 'a' + 10)
                             : (h >= 'A' && h <= 'F') ? (unsigned)(h - 'A' + 10)
                             : 0xFFFF;
                if (d == 0xFFFF) { cp = 0; break; }
                cp = cp * 16 + d;
            }
            if (cp == 0) { in += 4; continue; }
            if (n + 4 > size) break;
            if (cp < 0x80) out[n++] = (char)cp;
            else if (cp < 0x800) {
                out[n++] = (char)(0xC0 | (cp >> 6));
                out[n++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[n++] = (char)(0xE0 | (cp >> 12));
                out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (cp & 0x3F));
            }
            in += 4;
            continue;
        }
        default: c = *in; break;
        }
        if (n + 2 > size) break;
        out[n++] = c;
    }
    out[n] = 0;
}

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
    char raw[1024];
    size_t n = 0;

    if (!q || *q != '"') return false;
    q++;
    raw[0] = 0;
    while (*q && *q != '"' && n + 1 < sizeof(raw)) {
        if (*q == '\\' && q[1]) { raw[n++] = *q++; raw[n++] = *q++; }
        else raw[n++] = *q++;
    }
    raw[n] = 0;
    if (*q != '"') return false;
    json_unescape(raw, out, out_size);
    return true;
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

static bool json_get_float(const char *s, const char *key, float *out)
{
    const char *q = json_after_key(s, key);
    char *end;
    if (!q) return false;
    *out = (float)strtod(q, &end);
    return end != q;
}

// Adopt a sidecar next to `gox_path` after verifying that pin.
//
// PM review #541 (5,6, follow-up): the whole sidecar is validated into a
// temporary registry first — a malformed, duplicated or truncated file is
// rejected as a unit and cannot partially overwrite the working registry.
// A pin mismatch no longer discards the records: they are adopted as
// needs_review with pin_mismatch, so the Owner sees what the ship used to
// mean and decides, while publication stays closed until then.
int nav_adopt(const char *gox_path)
{
    char sidecar[600];
    char text[16 * 1024];
    char val[1024], kind[24], at[16], status[24];
    uint8_t sha[32];
    char hex[65];
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

    val[0] = 0;
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

    // Parse into a temporary list; commit only when every record parsed.
    {
        nav_record_t *tmp_head = NULL, *r, *tmp, *other;
        int nrec = 0;
        bool bad = false;

        brace = p + 1;
        while (!bad && (brace = strchr(brace, '{')) && brace < aend) {
            char *bend = strchr(brace, '}');
            int kindv, signv = 0, iv;
            float cell[3] = {0, 0, 0}, dir[3] = {0, 0, 0}, fdir[3];
            bool have_cell = false, have_dir = false, have_sign = false;

            if (!bend || bend > aend) { bad = true; break; }
            r = calloc(1, sizeof(*r));
            if (!r) { bad = true; break; }
            r->seq = ++s_seq;
            r->fp_kind = -1;
            DL_APPEND(tmp_head, r);
            nrec++;

            *bend = 0;        // every read below stays inside this record
            if (!json_get_str(brace, "id", val, sizeof(val)) || !val[0])
                bad = true;
            snprintf(r->id, sizeof(r->id), "%s", val);
            DL_FOREACH(tmp_head, other) {
                if (other != r && !strcmp(other->id, r->id)) bad = true;
            }
            if (json_get_str(brace, "group", val, sizeof(val)))
                snprintf(r->group, sizeof(r->group), "%s", val);
            if (json_get_str(brace, "label", val, sizeof(val)))
                snprintf(r->label, sizeof(r->label), "%s", val);
            json_get_int(brace, "layer_id", &r->layer_id);
            kindv = json_get_str(brace, "kind", kind, sizeof(kind))
                        ? parse_kind(kind) : -1;
            if (!json_get_str(brace, "at", at, sizeof(at)))
                snprintf(at, sizeof(at), "");
            if (!json_get_str(brace, "status", status, sizeof(status)))
                snprintf(status, sizeof(status), "");
            have_sign = json_get_int(brace, "shape_sign", &signv);
            have_cell = json_get_vec(brace, "cell", cell);
            have_dir = json_get_vec(brace, "dir", dir);
            // PM review #541 (4): the fingerprint travels with the record.
            if (json_get_int(brace, "fp_kind", &iv) && iv >= 0) {
                r->fp_kind = iv;
                json_get_int(brace, "fp_dir", &r->fp_dir);
                json_get_int(brace, "fp_closed", &r->fp_closed);
                json_get_int(brace, "fp_anchor", &r->fp_anchor);
                json_get_float(brace, "fp_rise", &r->fp_rise);
                if (json_get_vec(brace, "fp_ext", fdir))
                    for (iv = 0; iv < 3; iv++)
                        r->fp_ext[iv] = (int)floorf(fdir[iv] + 0.5f);
            }
            *bend = '}';
            if (bad) break;

            r->status = NAV_STATUS_REVIEW;
            r->pin_mismatch = !pin_ok;
            snprintf(r->reason, sizeof(r->reason), "%s",
                     pin_ok ? "not a known record kind" :
                     "the sidecar does not pin this gox");

            // The record's data is restored faithfully whatever the pin
            // says — pin_ok only decides the status.  A pin-mismatch record
            // must still know it is a cell record, or the Owner's explicit
            // Confirm would judge it against the wrong geometry.
            {
                bool data_ok = false;
                if (kindv >= 0) {
                    r->kind = kindv;
                    if (!strcmp(at, "shape_end") && have_sign &&
                        (signv == 1 || signv == -1)) {
                        r->at = NAV_AT_SHAPE_END;
                        r->shape_sign = signv;
                        data_ok = true;
                    } else if (!strcmp(at, "cell") && have_cell && have_dir) {
                        int i;
                        r->at = NAV_AT_CELL;
                        for (i = 0; i < 3; i++) {
                            r->cell[i] = r->cell0[i] = (int)floorf(cell[i]);
                            r->dir[i] = r->dir0[i] = dir[i];
                        }
                        data_ok = true;
                    }
                }
                if (data_ok && pin_ok && !strcmp(status, "confirmed")) {
                    if (r->at == NAV_AT_SHAPE_END && r->fp_kind < 0) {
                        // PM rev3 R3-4: a rev1 record carries no semantic
                        // fingerprint, so re-authoring could never be caught
                        // for it.  The meaning is kept, but only a deliberate
                        // re-anchor can make it green again — never a silent
                        // indefinite trust of a historical file.
                        snprintf(r->reason, sizeof(r->reason),
                                 "the sidecar predates fingerprints");
                    } else {
                        r->status = NAV_STATUS_CONFIRMED;
                        r->reason[0] = 0;
                        count++;
                    }
                }
            }
            brace = bend + 1;
        }

        if (bad) {
            DL_FOREACH_SAFE(tmp_head, r, tmp) { DL_DELETE(tmp_head, r); free(r); }
            printf("nav: sidecar rejected, the working registry is untouched\n");
            fflush(stdout);
            return -1;
        }
        // Commit: the old registry goes, the validated one moves in.
        DL_FOREACH_SAFE(s_records, r, tmp) { DL_DELETE(s_records, r); free(r); }
        s_records = tmp_head;
        s_count = nrec;
        s_navrev++;
    }
    nav_revalidate();
    snprintf(s_doc, sizeof(s_doc), "%s", gox_path);
    printf("nav: adopted %d record(s) from %s\n", count, sidecar);
    fflush(stdout);
    return count;
}

// PM rev3 R3-3: gox_export cannot report a failed write (save_to_file is
// void and the wrapper returns 0 unconditionally), so published bytes are
// trusted only after they read back as a real gox: openable, right magic,
// non-trivial size.  Returns true when the file looks like a gox.
static bool nav_gox_reads_ok(const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[4];
    long sz;

    if (!f) return false;
    if (fread(magic, 1, 4, f) != 4 || strncmp(magic, "GOX ", 4) != 0) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fclose(f);
    return sz > 4;
}

// PM rev4 (P0-2): the GOX loader itself is not robust against a truncated
// file (it logs and keeps going with garbage), so before trusting a
// candidate to the loader, walk its chunk table: every chunk's declared
// length must fit inside the file and the chunks must end exactly at EOF.
// A chunk is type[4] + len[4] + data[len] + crc[4] (chunk_write_finish).
// A truncated export fails here instead of reaching the loader.
static bool nav_gox_chunks_ok(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz, pos;
    char type[4];
    uint8_t b[4];
    int32_t len;

    if (!f) return false;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 8) {
        fclose(f);
        return false;
    }
    fseek(f, 8, SEEK_SET);              // magic + version
    pos = 8;
    while (pos < sz) {
        if (fread(type, 1, 4, f) != 4 || fread(b, 1, 4, f) != 4) {
            fclose(f);
            return false;
        }
        len = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                        ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
        pos += 12 + (long)len;          // header + data + CRC
        if (len < 0 || pos > sz) {
            fclose(f);
            return false;
        }
        fseek(f, pos, SEEK_SET);
    }
    fclose(f);
    return pos == sz;
}

// PM rev4 (P0-2): magic and size are not content.  The candidate is loaded
// back into a throwaway image and must read as the same model: same layer
// count and same occupied-cell count as the document being published.  The
// swap is contained (single-threaded, one action), and the live image is
// back in place before this returns.
bool nav_gox_validate(const char *path, char *why, size_t why_size)
{
    image_t *saved = goxel.image, *tmp;
    layer_t *l;
    nav_record_t *r;
    volume_iterator_t it;
    uint8_t c[4];
    int p[3];
    int want_layers = 0, want_cells = 0, got_layers = 0, got_cells = 0;
    bool ok = false;

    if (!saved) {
        snprintf(why, why_size, "no live document to compare against");
        return false;
    }
    if (!nav_gox_chunks_ok(path)) {
        snprintf(why, why_size, "the candidate gox is truncated or malformed");
        return false;
    }
    DL_FOREACH(saved->layers, l) {
        want_layers++;
        if (!l->volume) continue;
        it = volume_get_iterator(l->volume, VOLUME_ITER_VOXELS |
                                                  VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&it, p)) {
            volume_get_at(l->volume, &it, p, c);
            if (c[3]) want_cells++;
        }
    }
    goxel.image = tmp = image_new();
    if (load_from_file(path, true) != 0) {
        snprintf(why, why_size, "the candidate gox does not load back");
    } else {
        DL_FOREACH(tmp->layers, l) {
            got_layers++;
            if (!l->volume) continue;
            it = volume_get_iterator(l->volume, VOLUME_ITER_VOXELS |
                                                      VOLUME_ITER_SKIP_EMPTY);
            while (volume_iter(&it, p)) {
                volume_get_at(l->volume, &it, p, c);
                if (c[3]) got_cells++;
            }
        }
        if (got_layers != want_layers || got_cells != want_cells) {
            snprintf(why, why_size,
                     "the candidate gox reads back as %d layer(s)/%d cell(s), "
                     "expected %d/%d", got_layers, got_cells,
                     want_layers, want_cells);
        } else {
            // PM rev5: counts are not meaning.  Every registry record must
            // still own its object in the reloaded image — the layer id has
            // to exist, a shape record's modifier must carry the recorded
            // semantic fingerprint (kind/dir/closed/anchor/rise/extent) and
            // still resolve its end face, and a cell record's anchor cell
            // must still be occupied.  A focused semantic comparison, not a
            // GOX parser rewrite.
            ok = true;
            DL_FOREACH(s_records, r) {
                layer_t *tl = NULL;
                DL_FOREACH(tmp->layers, l) {
                    if (l->id == r->layer_id) {
                        tl = l;
                        break;
                    }
                }
                if (!tl) {
                    snprintf(why, why_size,
                             "record '%s' loses its layer %d on reload",
                             r->id, r->layer_id);
                    ok = false;
                    break;
                }
                if (r->at == NAV_AT_SHAPE_END) {
                    if (!tl->slope) {
                        snprintf(why, why_size,
                                 "record '%s' loses its shape modifier on "
                                 "reload", r->id);
                        ok = false;
                    } else if (r->fp_kind >= 0 &&
                               (tl->slope->kind != r->fp_kind ||
                                tl->slope->dir != r->fp_dir ||
                                tl->slope->closed != r->fp_closed ||
                                tl->slope->anchor != r->fp_anchor ||
                                fabsf(tl->slope->rise - r->fp_rise) > 1e-4f ||
                                tl->slope->aabb[1][0] - tl->slope->aabb[0][0] +
                                    1 != r->fp_ext[0] ||
                                tl->slope->aabb[1][1] - tl->slope->aabb[0][1] +
                                    1 != r->fp_ext[1] ||
                                tl->slope->aabb[1][2] - tl->slope->aabb[0][2] +
                                    1 != r->fp_ext[2])) {
                        snprintf(why, why_size,
                                 "record '%s' reloads with different authored "
                                 "shape attributes", r->id);
                        ok = false;
                    } else if (!nav_ring(r, NULL, NULL)) {
                        snprintf(why, why_size,
                                 "record '%s' cannot resolve its end face on "
                                 "the reloaded shape", r->id);
                        ok = false;
                    }
                    if (!ok) break;
                } else if (!tl->volume ||
                           volume_get_alpha_at(tl->volume, NULL, r->cell) == 0) {
                    snprintf(why, why_size,
                             "record '%s' loses its anchor cell on reload",
                             r->id);
                    ok = false;
                    break;
                }
            }
        }
    }
    image_delete(goxel.image);
    goxel.image = saved;
    return ok;
}

// PM rev4 (P0-1): the gox wrapper cannot report write failures itself, so
// the two promotion switches carry a one-shot fault the self-test can arm
// to prove the restore paths.  The variable is unset in normal use, and
// each fault fires at most once per process.
static bool nav_fault(const char *which)
{
    static char fired[64];
    const char *env = getenv("GOXEL_NAV_FAULT");

    if (!env || strcmp(env, which) != 0 || strstr(fired, which)) return false;
    strncat(fired, which, sizeof(fired) - strlen(fired) - 1);
    return true;
}

// PM rev7: the backup rotation needs a rename that replaces an existing
// destination.  The Windows CRT rename() refuses to overwrite (the rest of
// the publication never hits that because the live path is renamed away
// first), so the demotion uses MoveFileEx with REPLACE_EXISTING: nothing is
// pre-deleted to make room and a failed demotion leaves both files exactly
// where they were.  POSIX rename() already carries replace semantics.
static int nav_rename_replace(const char *src, const char *dst)
{
#ifdef _WIN32
    return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(src, dst);
#endif
}

int nav_save(const char *path, char *err, size_t err_size)
{
    nav_record_t *r;
    uint8_t sha[32];
    char hex[65];
    char sidecar[600], base[256], why[160];
    char cand[640], cand_side[660], lg_gox[660], lg_side[680];
    char lgp_gox[680], lgp_side[700];
    char esc[1024], esc2[1024], esc3[2048], esc4[1024];
    char *keep_export_path;
    const char *keep_export_fmt;
    FILE *f;
    int rc, total = 0, i = 0;
    bool had_gox, had_side;

#define FAILS(...) do { snprintf(err, err_size, ##__VA_ARGS__); return -1; } while (0)

    if (!goxel.image) FAILS("no document");
    if (slope_session_is_dirty()) FAILS("a shape session has uncommitted edits");
    // PM review #541 (3): publication revalidates synchronously — a record
    // that went stale since the last frame cannot slip through into a pin.
    // PM rev3 R3-2: shape records ask their source cells afresh, not from
    // the cached flag, before anything is written.
    nav_revalidate();
    DL_FOREACH(s_records, r) {
        char why[96];
        if (!nav_fresh_valid(r, why, sizeof(why))) nav_fail(r, why);
    }
    DL_FOREACH(s_records, r) {
        total++;
        if (r->status != NAV_STATUS_CONFIRMED)
            FAILS("record '%s' is %s%s%s: confirm or reject first",
                  r->id, nav_status_name(r->status),
                  r->reason[0] ? ": " : "", r->reason);
    }
    if (!path || !*path) FAILS("no path");

    // PM rev4 (P0-1): both halves of the pair are staged and validated as
    // candidates BEFORE either live file is touched, and every failure from
    // the promotion on puts the previous pair back.  A failed publish can
    // leave the old pair exactly as it was or say where the recovery point
    // is — never a split or missing current pair that could masquerade as
    // valid.  PM rev3 R3-3 / rev4 P0-2: gox_export cannot report a failed
    // write, so the candidate gox is proven by reading it back — magic and
    // size first, then a real round-trip against the live document.
    snprintf(cand, sizeof(cand), "%s.new", path);
    remove(cand);
    // PM rev4 (P1): goxel_export_to_file remembers the destination on the
    // image (a later Ctrl+E exports there).  The staging path must never
    // become that memory: keep the Owner's setting and put it back.
    keep_export_path = goxel.image->export_path
                           ? strdup(goxel.image->export_path) : NULL;
    keep_export_fmt = goxel.image->export_fmt;
    rc = goxel_export_to_file(cand, "gox");
    free(goxel.image->export_path);
    goxel.image->export_path = keep_export_path;
    goxel.image->export_fmt = keep_export_fmt;
    if (rc) {
        remove(cand);
        FAILS("the gox export reported failure");
    }
    if (!nav_gox_reads_ok(cand)) {
        remove(cand);
        FAILS("the gox export did not produce a readable file");
    }
    if (!nav_gox_validate(cand, why, sizeof(why))) {
        remove(cand);
        FAILS("%s", why);
    }
    if (nav_sha256_file(cand, sha)) {
        remove(cand);
        FAILS("cannot hash the saved file");
    }
    nav_sha256_hex(sha, hex);
    nav_sidecar_path(path, sidecar, sizeof(sidecar));

    // Stage the sidecar too — pinned to the candidate's bytes — and read it
    // back before anything is promoted (P0-1).
    // PM review #541 (5): every string is JSON-escaped; a Windows path with
    // backslashes or spaces publishes as valid JSON, not a broken file.
    snprintf(cand_side, sizeof(cand_side), "%s.new", sidecar);
    remove(cand_side);
    f = fopen(cand_side, "wb");
    if (!f) {
        remove(cand);
        FAILS("cannot stage the sidecar next to %s", path);
    }
    nav_basename(path, base, sizeof(base));
    json_escape(base, esc, sizeof(esc));
    json_escape(path, esc3, sizeof(esc3));
    fprintf(f,
        "{\n \"format\": \"amfm.nav\",\n \"schema_version\": 1,\n"
        " \"asset_id\": \"%s\",\n \"source_gox\": \"%s\",\n"
        " \"source_sha256\": \"%s\",\n \"doc_key\": %u,\n"
        " \"records\": [\n",
        esc, esc3, hex, (unsigned)image_get_key(goxel.image));
    DL_FOREACH(s_records, r) {
        json_escape(r->id, esc, sizeof(esc));
        json_escape(r->group, esc2, sizeof(esc2));
        json_escape(r->label, esc4, sizeof(esc4));
        fprintf(f,
            "  {\"id\": \"%s\", \"group\": \"%s\", \"label\": \"%s\",\n"
            "   \"kind\": \"%s\", \"at\": \"%s\", \"layer_id\": %d,\n",
            esc, esc2, esc4, nav_kind_name(r->kind),
            r->at == NAV_AT_SHAPE_END ? "shape_end" : "cell", r->layer_id);
        if (r->at == NAV_AT_SHAPE_END) {
            fprintf(f, "   \"shape_sign\": %d,\n", r->shape_sign);
            // PM review #541 (4): the semantic attachment travels with the
            // record, so a reopen can tell a rigid move from a re-authoring.
            fprintf(f,
                "   \"fp_kind\": %d, \"fp_dir\": %d, \"fp_closed\": %d,\n"
                "   \"fp_anchor\": %d, \"fp_rise\": %.6f,"
                " \"fp_ext\": [%d, %d, %d],\n",
                r->fp_kind, r->fp_dir, r->fp_closed, r->fp_anchor,
                r->fp_rise, r->fp_ext[0], r->fp_ext[1], r->fp_ext[2]);
        } else {
            fprintf(f,
                "   \"cell\": [%d, %d, %d],\n   \"dir\": [%.6f, %.6f, %.6f],\n",
                r->cell[0], r->cell[1], r->cell[2],
                r->dir[0], r->dir[1], r->dir[2]);
        }
        fprintf(f, "   \"status\": \"confirmed\", \"seq\": %llu\n  }%s\n",
                (unsigned long long)r->seq, (++i < total) ? "," : "");
    }
    fprintf(f, " ]\n}\n");
    if (fclose(f) != 0) {
        remove(cand);
        remove(cand_side);
        FAILS("the staged sidecar did not flush");
    }
    {
        static char probe[8192];
        FILE *pf = fopen(cand_side, "rb");
        size_t n = 0;

        if (pf) {
            n = fread(probe, 1, sizeof(probe) - 1, pf);
            fclose(pf);
            probe[n] = 0;
        }
        if (n == 0 || !strstr(probe, hex) || !strstr(probe, "\"records\"")) {
            remove(cand);
            remove(cand_side);
            FAILS("the staged sidecar did not read back with its pin");
        }
    }

    // Both halves are staged and validated.  From here on, every failure
    // restores the previous live pair (P0-1).
    snprintf(lg_gox, sizeof(lg_gox), "%s.lastgood", path);
    snprintf(lg_side, sizeof(lg_side), "%s.lastgood", sidecar);
    snprintf(lgp_gox, sizeof(lgp_gox), "%s.lastgoodprev", path);
    snprintf(lgp_side, sizeof(lgp_side), "%s.lastgoodprev", sidecar);
    had_gox = nav_gox_reads_ok(path);
    {
        FILE *probe = fopen(sidecar, "rb");
        had_side = probe != NULL;
        if (probe) fclose(probe);
    }
    // PM rev5: the previous backup is demoted one notch to .lastgoodprev
    // instead of being deleted, so a discoverable backup pair exists at
    // every moment and only the oldest generation is ever discarded.
    // PM rev7: a failed demotion must never delete .lastgood — losing the
    // only recoverable backup exactly when the filesystem misbehaves is
    // the worst outcome.  rename() replaces an existing destination on
    // both Windows (MinGW) and POSIX, so nothing is pre-deleted: if the
    // demotion fails, every file stays exactly where it was and the
    // publication aborts fail-closed with a message saying where they are.
    bool rotate_ok = true;
    if (nav_gox_reads_ok(lg_gox)) {
        if (nav_fault("rotate_gox") || nav_rename_replace(lg_gox, lgp_gox) != 0)
            rotate_ok = false;
    }
    if (rotate_ok) {
        FILE *probe = fopen(lg_side, "rb");
        if (probe) {
            fclose(probe);
            if (nav_fault("rotate_side") || nav_rename_replace(lg_side, lgp_side) != 0)
                rotate_ok = false;
        }
    }
    if (!rotate_ok) {
        remove(cand);
        remove(cand_side);
        FAILS("cannot rotate the backup pair; nothing was published; the "
              "previous pair stays recoverable under the %s / %s names",
              lg_gox, lg_side);
    }
    if (had_gox && rename(path, lg_gox) != 0) {
        remove(cand);
        remove(cand_side);
        FAILS("cannot preserve the previous gox; nothing was published");
    }
    if (had_side && rename(sidecar, lg_side) != 0) {
        // PM rev5: the restore's own result is checked, never assumed.
        if (!had_gox || rename(lg_gox, path) == 0)
            FAILS("cannot preserve the previous sidecar; the previous pair "
                  "was restored");
        remove(cand);
        remove(cand_side);
        FAILS("cannot preserve the previous sidecar and automatic restore "
              "failed: recover manually from %s / %s", lg_gox, lg_side);
    }
    if (nav_fault("promote_gox") || rename(cand, path) != 0) {
        // PM rev5: a tested one-shot refusal and a genuinely failed
        // rollback are different claims; only the first may say "restored".
        bool ok_side = !had_side || rename(lg_side, sidecar) == 0;
        bool ok_gox = !had_gox || rename(lg_gox, path) == 0;
        remove(cand);
        remove(cand_side);
        if (ok_side && ok_gox)
            FAILS("cannot promote the gox; the previous pair was restored");
        FAILS("cannot promote the gox and automatic restore failed: the "
              "recoverable pair is at %s / %s", lg_gox, lg_side);
    }
    if (nav_fault("promote_side") || rename(cand_side, sidecar) != 0) {
        bool ok_gox = remove(path) == 0;  // the new gox alone would lie
        bool ok_side = !had_side || rename(lg_side, sidecar) == 0;
        if (had_gox && rename(lg_gox, path) != 0) ok_gox = false;
        remove(cand);
        remove(cand_side);
        if (!had_gox && !had_side && !ok_gox)
            FAILS("cannot promote the sidecar and the staged gox could not "
                  "be withdrawn: delete %s before retrying", path);
        if (ok_gox && ok_side)
            FAILS("cannot promote the sidecar; the previous pair was restored");
        FAILS("cannot promote the sidecar and automatic restore failed: "
              "recover manually from %s / %s", lg_gox, lg_side);
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
    // PM review #541 (6): an open action always re-settles the registry.
    // load_from_file replaces the image contents in place, so neither the
    // image pointer nor the path has to change when the Owner re-opens the
    // same file — the guard alone would keep the previous session's records
    // green over bytes that may no longer match the pin.
    s_open_doc[0] = 0;
}

static void check_document_for(image_t *img)
{
    char sidecar[600];
    const char *doc = NULL;
    FILE *f;

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

    // PM review #541 (6): a document with a sidecar always goes through the
    // adopt path.  A matching pin yields the confirmed records; a mismatch
    // keeps them as needs_review with the pin_mismatch flag, so a normal
    // reopen surfaces the previous meaning for an explicit Owner decision
    // instead of quietly clearing every equipment tag.  Only a document with
    // no sidecar at all starts from an empty registry, and publication stays
    // fail-closed either way (nav_save refuses while anything is in review).
    if (doc && strcmp(doc, "-")) {
        nav_sidecar_path(doc, sidecar, sizeof(sidecar));
        f = fopen(sidecar, "rb");
        if (f) {
            fclose(f);
            nav_adopt(doc);
            return;
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
        case NAV_KIND_RCS:     col[0] = 120; col[1] = 255; col[2] = 120; break;
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
    char idbuf[96];
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
        // PM #541 GUI finding: every record used to render its Confirm/Reject
        // buttons under the same group label, so ImGui saw duplicate item IDs
        // once two records existed: the second record's buttons did nothing
        // and the debug build threw a conflicting-ID warning on screen.  The
        // group's ID scope must be unique per record; the visible caption is
        // printed separately so the '##' trick stays invisible.
        gui_text("Owner decision");
        snprintf(idbuf, sizeof(idbuf), "##nav_%s", r->id);
        gui_group_begin(idbuf);
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
