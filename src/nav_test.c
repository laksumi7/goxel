/* Goxel 3D voxels editor
 *
 * AMFM #541: the equipment navigator, headless verification.  Driven by
 * GOXEL_NAV_TEST=<output dir>, with the fixture on GOXEL_NAV_FIXTURE.
 *
 * The test drives the exact bridge lines a worker would send, through the
 * exact dispatch, and the same nav calls the editor frame would make.
 *
 * P0 scope:
 *   - the selection reports the exact object (layer ids, shape, both ends),
 *   - a tag proposes with both ends visible and the Owner confirms it,
 *   - a whole-part move carries the marker and the surface together,
 *   - a move outside the marker's cells leaves the marker alone,
 *   - a removed anchor cell flips the record to needs_review and publication
 *     stays closed,
 *   - undo does not resurrect a green light by itself,
 *   - navsave writes a gox + sidecar pair pinned by the gox SHA-256, and a
 *     reopen re-adopts it automatically,
 *   - a worker write carrying a stale base revision is refused.
 *
 * PM review #541 corrections (this revision):
 *   (1) negative authored axes resolve to the physically correct cap face,
 *       position and direction verified independently,
 *   (2) a mirrored cell marker lands on the surviving cell (half-open span),
 *   (3) Confirm validates current geometry: an invalidated meaning needs a
 *       deliberate re-anchor, never a second click,
 *   (4) re-authoring the shape (axis, anchor, ...) forces review through the
 *       semantic fingerprint, while rigid moves keep the marker aligned,
 *   (5) quoted spaced paths publish, and the sidecar is valid JSON,
 *   (6) a pin mismatch on reopen keeps the records as needs_review instead
 *       of silently clearing them, and an explicit Confirm accepts them,
 *   (7) tag/confirm/untag require base= AND nbase=; a stale nbase is refused,
 *   (8) a cell anchor must be inside the selection, not merely occupied;
 *   - an orphaned draft is flagged, not left invisible.
 */

#include "goxel.h"
#include "image.h"
#include "lab_bridge.h"
#include "layer.h"
#include "nav.h"
#include "slope.h"
#include "volume.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <sys/stat.h>

static int s_checks, s_failed;

// NOTE: `cond` is evaluated exactly once — several checks call the bridge
// through call_fails(), and a second evaluation would run the call twice.
#define CHECK(cond, ...) do {                                           \
    bool cond_ = (cond);                                                \
    s_checks++;                                                         \
    if (!cond_) { s_failed++; printf("  FAIL: "); }                     \
    else        printf("  ok  : ");                                     \
    printf(__VA_ARGS__);                                                \
    if (!cond_) printf("\n");                                           \
    printf("\n");                                                       \
    fflush(stdout);                                                     \
} while (0)

// 64 KiB replies (the bridge drops instead of overflowing).
static char s_reply[64 * 1024 + 8];

static const char *bridge(const char *line)
{
    lab_bridge_test_call(line, s_reply, sizeof(s_reply));
    if (getenv("GOXEL_NAV_VERBOSE")) {
        printf("---- %s\n%s\n", line, s_reply);
        fflush(stdout);
    }
    return s_reply;
}

static bool reply_has(const char *needle)
{
    return strstr(s_reply, needle) != NULL;
}

static bool call_fails(const char *line)
{
    bridge(line);
    return strstr(s_reply, "err ") != NULL;
}

// PM review #541 (7): the two revision tokens every semantic write carries.
static unsigned s_rev;
static unsigned long long s_nrev;

static void read_revs(void)
{
    const char *q;
    bridge("state");
    q = strstr(s_reply, "rev ");
    s_rev = q ? (unsigned)strtoul(q + 4, NULL, 10) : 0;
    q = strstr(s_reply, "navrev ");
    s_nrev = q ? strtoull(q + 7, NULL, 10) : 0;
}

// Deliberate re-anchor of a cell record against the active layer's current
// selection: untag, tag again, confirm — the PM review #541 (3) contract.
static void reanchor_cell(const char *id, const char *kind)
{
    char cmd[900];

    read_revs();
    snprintf(cmd, sizeof(cmd), "untag id=%s base=%u nbase=%llu", id, s_rev,
             s_nrev);
    bridge(cmd);
    read_revs();
    snprintf(cmd, sizeof(cmd), "tag kind=%s id=%s end=+Z group=pd base=%u "
             "nbase=%llu", kind, id, s_rev, s_nrev);
    bridge(cmd);
    read_revs();
    snprintf(cmd, sizeof(cmd), "confirm id=%s base=%u nbase=%llu", id, s_rev,
             s_nrev);
    bridge(cmd);
}

static layer_t *layer_by_kind(bool want_slope)
{
    layer_t *l;
    DL_FOREACH(goxel.image->layers, l) {
        if (want_slope == (l->slope != NULL)) return l;
    }
    return NULL;
}

// The first ordinary voxel layer that actually holds cells: the fixture
// starts with helpers that are empty in a fresh PD.
static layer_t *body_layer_with_cells(void)
{
    layer_t *l;
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3];

    DL_FOREACH(goxel.image->layers, l) {
        if (l->slope || !l->volume) continue;
        iter = volume_get_iterator(l->volume,
                                   VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&iter, p)) {
            volume_get_at(l->volume, &iter, p, c);
            if (c[3]) return l;
        }
    }
    return NULL;
}

static void add_cell(volume_t *vol, int x, int y, int z)
{
    static const uint8_t col[4] = {255, 255, 255, 255};
    int p[3] = {x, y, z};
    volume_set_at(vol, NULL, p, (uint8_t *)col);
}

// The selection of exactly one layer's cells: what a box select on that
// layer means in the live editor.
static void select_layer(layer_t *l)
{
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3];

    if (goxel.image->selection_mask)
        volume_delete(goxel.image->selection_mask);
    goxel.image->selection_mask = volume_new();
    iter = volume_get_iterator(l->volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, p)) {
        volume_get_at(l->volume, &iter, p, c);
        if (c[3]) add_cell(goxel.image->selection_mask, p[0], p[1], p[2]);
    }
}

static bool parse_tag_pos(const char *id, float out_pos[3])
{
    char pat[64];
    const char *q;

    snprintf(pat, sizeof(pat), "tag id=%s", id);
    q = strstr(s_reply, pat);
    if (!q) return false;
    q = strstr(q, " pos ");
    if (!q) return false;
    return sscanf(q + 5, "%f %f %f", &out_pos[0], &out_pos[1],
                  &out_pos[2]) == 3;
}

static bool parse_tag_dir(const char *id, float out_dir[3])
{
    char pat[64];
    const char *q;

    snprintf(pat, sizeof(pat), "tag id=%s", id);
    q = strstr(s_reply, pat);
    if (!q) return false;
    q = strstr(q, " dir ");
    if (!q) return false;
    return sscanf(q + 5, "%f %f %f", &out_dir[0], &out_dir[1],
                  &out_dir[2]) == 3;
}

static bool parse_anchored_cell(const char *id, int out[3])
{
    char pat[64];
    const char *q;

    snprintf(pat, sizeof(pat), "tag id=%s", id);
    q = strstr(s_reply, pat);       // the just-written tag reply
    if (!q) return false;
    q = strstr(q, "anchored cell ");
    if (!q) return false;
    return sscanf(q + 14, "%d %d %d", &out[0], &out[1], &out[2]) == 3;
}

// Read "picked x y z" from the tag reply (PM review #541 (1): the face the
// token actually means, reported by the same authority that places it).
static bool parse_picked(float out_pos[3])
{
    const char *q = strstr(s_reply, "picked ");
    if (!q) return false;
    return sscanf(q + 7, "%f %f %f", &out_pos[0], &out_pos[1],
                  &out_pos[2]) == 3;
}

// A compact dump of what the document actually holds, for the evidence log.
static void dump_doc(const char *when)
{
    layer_t *l;
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3], n;

    printf("  [doc %s]\n", when);
    DL_FOREACH(goxel.image->layers, l) {
        n = 0;
        iter = volume_get_iterator(l->volume, VOLUME_ITER_VOXELS |
                                                  VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&iter, p)) {
            volume_get_at(l->volume, &iter, p, c);
            if (c[3]) n++;
        }
        printf("    layer id=%d '%s' cells=%d slope=%d\n", l->id, l->name, n,
               l->slope ? 1 : 0);
    }
    printf("    active=%s\n", goxel.image->active_layer
                                  ? goxel.image->active_layer->name : "(none)");
}

// Copy one file over another (for the mismatched-pair and bad-sidecar cases).
static bool copy_file(const char *src, const char *dst)
{
    FILE *a = fopen(src, "rb"), *b;
    static char buf[64 * 1024];
    size_t n;
    bool ok = false;

    if (!a) return false;
    b = fopen(dst, "wb");
    if (!b) { fclose(a); return false; }
    ok = true;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0)
        if (fwrite(buf, 1, n, b) != n) ok = false;
    fclose(a);
    fclose(b);
    return ok;
}

static bool write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

int nav_test_run(const char *fixture, const char *outdir)
{
    layer_t *barrel, *body;
    slope_mod_t *m;
    float u[3], pos0[3], pos1[3], dirv[3], exhaust0[3];
    char end_tok[4] = "+Y";
    char cmd[900], path[512], sidecar[560];
    int dir = 2;
    int32_t a00 = 0;
    int anchor_cell[3] = {0, 0, 0};
    bool anchor_cell_known = false;
    unsigned stale_base = 0;
    FILE *f;
    volume_t *mask;
    image_move_t *mv;
    float tmat[4][4];

    printf("== nav test (AMFM #541 P0 + PM review) ==\n");
    memset(pos0, 0, sizeof(pos0));
    memset(pos1, 0, sizeof(pos1));
    memset(exhaust0, 0, sizeof(exhaust0));
    memset(dirv, 0, sizeof(dirv));

    // 0. The sha pin is only a pin if the digest is the standard one.
    CHECK(nav_sha256_known_answer(), "sha-256 known-answer vectors");

    // 1. The document: a small, real fixture.
    if (fixture && *fixture) {
        int rc = goxel_import_file(fixture, NULL);
        CHECK(rc == 0, "import fixture %s -> rc=%d", fixture, rc);
    }
    nav_check_document();
    if (!goxel.image) {
        printf("== nav test: %d checks, %d failed ==\n", s_checks, s_failed);
        return 1;
    }
    dump_doc("after import");

    // 1b. The raw selection-move machinery, before any nav record exists:
    //     a whole-document move out and back must preserve every cell.
    {
        volume_t *m2 = volume_new();
        layer_t *l;
        volume_iterator_t iter;
        uint8_t c[4];
        int p[3], n_before = 0, n_after = 0;
        image_move_t *mv2;
        float t2[4][4];

        DL_FOREACH(goxel.image->layers, l) {
            iter = volume_get_iterator(l->volume,
                                       VOLUME_ITER_VOXELS |
                                       VOLUME_ITER_SKIP_EMPTY);
            while (volume_iter(&iter, p)) {
                volume_get_at(l->volume, &iter, p, c);
                if (c[3]) { add_cell(m2, p[0], p[1], p[2]); n_before++; }
            }
        }
        memset(t2, 0, sizeof(t2));
        t2[0][0] = t2[1][1] = t2[2][2] = t2[3][3] = 1.f;
        t2[3][0] = 2.f;
        mv2 = image_move_begin(goxel.image, m2);
        CHECK(mv2 != NULL, "raw machinery: move begins on %d cells", n_before);
        if (mv2) {
            image_move_apply(mv2, t2);
            image_move_end(mv2);
        }
        DL_FOREACH(goxel.image->layers, l) {
            iter = volume_get_iterator(l->volume,
                                       VOLUME_ITER_VOXELS |
                                       VOLUME_ITER_SKIP_EMPTY);
            while (volume_iter(&iter, p)) {
                volume_get_at(l->volume, &iter, p, c);
                if (c[3]) n_after++;
            }
        }
        CHECK(n_after == n_before,
              "raw machinery: cells preserved by +2 move (%d -> %d)",
              n_before, n_after);
        volume_delete(m2);

        m2 = volume_new();
        n_after = 0;
        DL_FOREACH(goxel.image->layers, l) {
            iter = volume_get_iterator(l->volume,
                                       VOLUME_ITER_VOXELS |
                                       VOLUME_ITER_SKIP_EMPTY);
            while (volume_iter(&iter, p)) {
                volume_get_at(l->volume, &iter, p, c);
                if (c[3]) { add_cell(m2, p[0], p[1], p[2]); n_after++; }
            }
        }
        t2[3][0] = -2.f;
        mv2 = image_move_begin(goxel.image, m2);
        if (mv2) {
            image_move_apply(mv2, t2);
            image_move_end(mv2);
        }
        volume_delete(m2);
        n_before = 0;
        DL_FOREACH(goxel.image->layers, l) {
            iter = volume_get_iterator(l->volume,
                                       VOLUME_ITER_VOXELS |
                                       VOLUME_ITER_SKIP_EMPTY);
            while (volume_iter(&iter, p)) {
                volume_get_at(l->volume, &iter, p, c);
                if (c[3]) n_before++;
            }
        }
        CHECK(n_before == 13, "raw machinery: move back restores 13 cells (%d)",
              n_before);
        dump_doc("raw machinery: after the round trip");
    }
    barrel = layer_by_kind(true);
    body = body_layer_with_cells();
    CHECK(barrel && barrel->slope, "a generated-shape layer exists");
    CHECK(body && body->volume, "an ordinary voxel layer with cells exists");

    // 2..6 exercise the shape-end flow on the fixture's PD barrel.
    if (barrel && barrel->slope) {
        m = barrel->slope;
        dir = m->dir;
        a00 = m->aabb[0][0];
        nav_dir_vec(dir, u);
        snprintf(end_tok, sizeof(end_tok), "%c%c",
                 u[1] > 0.f ? '+' : '-',
                 (dir == SLOPE_DIR_PX || dir == SLOPE_DIR_NX) ? 'X' : 'Y');

        // 2. The selection reports the exact object, not a bounding box.
        goxel.image->active_layer = barrel;
        select_layer(barrel);
        bridge("selinfo");
        CHECK(reply_has("shape layer=") && reply_has("ends layer=") &&
              reply_has("stale="),
              "selinfo gives shape params, both ends, rev and stale flags");
        CHECK(reply_has("selrev "), "selinfo carries the document revision");
        CHECK(reply_has("sel=1 ACTIVE"),
              "PM8: selinfo marks the selected shape apart from inventory");

        // 3. A worker proposal: both ends are on the table and the token is
        //    what the draft means.  PM7: the write carries both tokens.
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=test_muzzle end=%s "
                 "group=pd label=\"left barrel muzzle\" base=%u nbase=%llu",
                 end_tok, s_rev, s_nrev);
        bridge(cmd);
        CHECK(reply_has("status=draft") && reply_has("ends axis="),
              "tag %s proposes a draft with both ends shown", end_tok);
        bridge("tags");
        CHECK(reply_has("test_muzzle") && reply_has("status=draft"),
              "the draft is visible");
        CHECK(parse_tag_pos("test_muzzle", pos0),
              "the draft position reads back");

        // 4. The Owner confirms it in the navigator.
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        CHECK(reply_has("status=confirmed"), "confirm accepts the draft");

        // 5. A whole-assembly move carries the marker with the geometry.
        mask = volume_new();
        {
            layer_t *l;
            volume_iterator_t iter;
            uint8_t c[4];
            int p[3];
            DL_FOREACH(goxel.image->layers, l) {
                if (!l->volume) continue;
                iter = volume_get_iterator(l->volume, VOLUME_ITER_VOXELS |
                                                      VOLUME_ITER_SKIP_EMPTY);
                while (volume_iter(&iter, p)) {
                    volume_get_at(l->volume, &iter, p, c);
                    if (c[3]) add_cell(mask, p[0], p[1], p[2]);
                }
            }
        }
        memset(tmat, 0, sizeof(tmat));
        tmat[0][0] = tmat[1][1] = tmat[2][2] = tmat[3][3] = 1.f;
        tmat[3][0] = 2.f;
        mv = image_move_begin(goxel.image, mask);
        CHECK(mv != NULL, "whole-part move starts");
        if (mv) {
            image_move_apply(mv, tmat);
            image_move_end(mv);
            // The editor's own move tool pushes one history entry at the end
            // of the gesture; the test mirrors that so one undo reverts one
            // whole move.
            image_history_push(goxel.image);
        }
        volume_delete(mask);
        bridge("tags");
        CHECK(parse_tag_pos("test_muzzle", pos1) &&
              fabsf(pos1[0] - pos0[0] - 2.f) < 1e-3f &&
              fabsf(pos1[1] - pos0[1]) < 1e-3f,
              "the muzzle followed +2 X inside the moved cells");
        m = barrel->slope;
        CHECK(m->aabb[0][0] == a00 + 2, "the surface region moved too");
        CHECK(reply_has("status=confirmed"),
              "the muzzle is still confirmed after the move");

        // 5b. PM review follow-up: a repeated drag replays from the snapshot,
        //     so cumulative applies +1 +2 +3 land on +3, never on +6.
        {
            float posA[3];
            bridge("tags");
            parse_tag_pos("test_muzzle", posA);
            mask = volume_new();
            {
                layer_t *l;
                volume_iterator_t iter;
                uint8_t c[4];
                int p[3];
                DL_FOREACH(goxel.image->layers, l) {
                    if (!l->volume) continue;
                    iter = volume_get_iterator(l->volume,
                                               VOLUME_ITER_VOXELS |
                                               VOLUME_ITER_SKIP_EMPTY);
                    while (volume_iter(&iter, p)) {
                        volume_get_at(l->volume, &iter, p, c);
                        if (c[3]) add_cell(mask, p[0], p[1], p[2]);
                    }
                }
            }
            mv = image_move_begin(goxel.image, mask);
            if (mv) {
                float tm[4][4];
                memcpy(tm, tmat, sizeof(tm));
                tm[3][0] = 1.f; image_move_apply(mv, tm);
                tm[3][0] = 2.f; image_move_apply(mv, tm);
                tm[3][0] = 3.f; image_move_apply(mv, tm);
                image_move_end(mv);
                image_history_push(goxel.image);
            }
            volume_delete(mask);
            bridge("tags");
            CHECK(parse_tag_pos("test_muzzle", pos1) &&
                  fabsf(pos1[0] - posA[0] - 3.f) < 1e-3f,
                  "PM-follow: three cumulative applies move the marker +3, "
                  "not +6");
            // and back again
            mask = volume_new();
            {
                layer_t *l;
                volume_iterator_t iter;
                uint8_t c[4];
                int p[3];
                DL_FOREACH(goxel.image->layers, l) {
                    if (!l->volume) continue;
                    iter = volume_get_iterator(l->volume,
                                               VOLUME_ITER_VOXELS |
                                               VOLUME_ITER_SKIP_EMPTY);
                    while (volume_iter(&iter, p)) {
                        volume_get_at(l->volume, &iter, p, c);
                        if (c[3]) add_cell(mask, p[0], p[1], p[2]);
                    }
                }
            }
            mv = image_move_begin(goxel.image, mask);
            if (mv) {
                float tm[4][4];
                memcpy(tm, tmat, sizeof(tm));
                tm[3][0] = -3.f; image_move_apply(mv, tm);
                image_move_end(mv);
                image_history_push(goxel.image);
            }
            volume_delete(mask);
        }

        // 6. A cell marker on the body: it moves with its own assembly and
        //    does NOT move when only the barrel moves.
        if (body) {
            goxel.image->active_layer = body;
            select_layer(body);
            dump_doc("before the cell-marker step");
            read_revs();
            snprintf(cmd, sizeof(cmd), "tag kind=exhaust id=test_exhaust "
                     "end=+Z group=pd base=%u nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            CHECK(reply_has("status=draft") && reply_has("anchored cell"),
                  "a cell exhaust proposes on ordinary voxels");
            read_revs();
            snprintf(cmd, sizeof(cmd), "confirm id=test_exhaust base=%u "
                     "nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            bridge("tags");
            parse_tag_pos("test_exhaust", exhaust0);

            goxel.image->active_layer = barrel;
            select_layer(barrel);
            memset(tmat, 0, sizeof(tmat));
            tmat[0][0] = tmat[1][1] = tmat[2][2] = tmat[3][3] = 1.f;
            tmat[3][0] = 1.f;
            mv = image_move_begin(goxel.image, goxel.image->selection_mask);
            if (mv) {
                image_move_apply(mv, tmat);
                image_move_end(mv);
            }
            bridge("tags");
            CHECK(parse_tag_pos("test_exhaust", pos1) &&
                  fabsf(pos1[0] - exhaust0[0]) < 1e-3f,
                  "the exhaust stayed while only the barrel moved");
            // put the barrel back so later geometry checks read cleanly
            mv = image_move_begin(goxel.image, goxel.image->selection_mask);
            if (mv) {
                tmat[3][0] = -1.f;
                image_move_apply(mv, tmat);
                image_move_end(mv);
            }
        }
    }

    // 7. Removing the anchor cell is review; publication is refused; undo
    //    does not resurrect the green light; and PM3: a second click cannot
    //    bless an invalidated meaning — it needs a deliberate re-anchor.
    if (body && body->volume) {
        goxel.image->active_layer = body;
        select_layer(body);
        snprintf(path, sizeof(path), "%s/navtest_save.gox",
                 outdir && *outdir ? outdir : ".");
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=pivot id=test_pivot end=+Z "
                 "group=pd base=%u nbase=%llu", s_rev, s_nrev);
        bridge(cmd);
        anchor_cell_known = parse_anchored_cell("test_pivot", anchor_cell);
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=test_pivot base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        bridge("tags");
        CHECK(reply_has("test_pivot") && reply_has("status=confirmed"),
              "the pivot is confirmed before the edit");

        if (anchor_cell_known) {
            // Through the bridge, so the removal is a history entry.
            snprintf(cmd, sizeof(cmd), "del %d %d %d", anchor_cell[0],
                     anchor_cell[1], anchor_cell[2]);
            bridge(cmd);
            nav_revalidate();
            bridge("tags");
            CHECK(reply_has("test_pivot") && reply_has("status=needs_review"),
                  "removing the anchor cell flips the record to review");
            snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
            CHECK(call_fails(cmd),
                  "navsave refuses while a record is in review");
            bridge("undo");
            // An undo can hand back different layer objects; the pointers the
            // test carried in may no longer be the live ones.
            body = body_layer_with_cells();
            if (body) goxel.image->active_layer = body;
            nav_revalidate();
            bridge("tags");
            CHECK(reply_has("status=needs_review"),
                  "undo does not resurrect the marker by itself");
            // PM review #541 (3): the geometry came back with the undo, but
            // the meaning was invalidated — Confirm must refuse and demand a
            // deliberate re-anchor.
            read_revs();
            snprintf(cmd, sizeof(cmd), "confirm id=test_pivot base=%u "
                     "nbase=%llu", s_rev, s_nrev);
            CHECK(call_fails(cmd) && reply_has("re-anchor"),
                  "PM3: confirm refuses an invalidated meaning");
            // The deliberate re-anchor: untag, tag again, confirm.  The undo
            // restored an older selection, so the Owner's click is mirrored
            // by selecting the body again first.
            read_revs();
            snprintf(cmd, sizeof(cmd), "untag id=test_pivot base=%u "
                     "nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            if (body) select_layer(body);
            read_revs();
            snprintf(cmd, sizeof(cmd), "tag kind=pivot id=test_pivot end=+Z "
                     "group=pd base=%u nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            read_revs();
            snprintf(cmd, sizeof(cmd), "confirm id=test_pivot base=%u "
                     "nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            CHECK(reply_has("status=confirmed"),
                  "PM3: a deliberate re-anchor (untag+tag+confirm) restores "
                  "the marker");
            // The exhaust shared the anchor cell, so it went to review too;
            // each marker needs its own deliberate re-anchor.
            read_revs();
            snprintf(cmd, sizeof(cmd), "confirm id=test_exhaust base=%u "
                     "nbase=%llu", s_rev, s_nrev);
            CHECK(call_fails(cmd), "PM3: the other invalidated marker also "
                  "refuses a bare confirm");
            read_revs();
            snprintf(cmd, sizeof(cmd), "untag id=test_exhaust base=%u "
                     "nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            if (body) select_layer(body);
            read_revs();
            snprintf(cmd, sizeof(cmd), "tag kind=exhaust id=test_exhaust "
                     "end=+Z group=pd base=%u nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            read_revs();
            snprintf(cmd, sizeof(cmd), "confirm id=test_exhaust base=%u "
                     "nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            CHECK(reply_has("test_exhaust") && reply_has("status=confirmed"),
                  "PM3: and re-anchors the same way");
        }
    }

    // 7b. PM review #541 (1,4): a negative authored axis.  Flipping the
    //     modifier's run direction is a re-authoring (fingerprint), and a
    //     tag on the flipped axis must resolve to the min-coordinate cap
    //     face with the direction pointing along the signed axis.
    barrel = layer_by_kind(true);
    if (barrel && barrel->slope) {
        m = barrel->slope;
        m->dir = (m->dir == SLOPE_DIR_PY) ? SLOPE_DIR_NY : SLOPE_DIR_NX;
        m->revision++;
        nav_revalidate();
        bridge("tags");
        CHECK(reply_has("test_muzzle") && reply_has("re-authored"),
              "PM4: flipping the authored axis is caught as a re-authoring");
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        CHECK(call_fails(cmd), "PM3: the re-authored muzzle cannot be "
              "re-confirmed");
        read_revs();
        snprintf(cmd, sizeof(cmd), "untag id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        goxel.image->active_layer = barrel;
        select_layer(barrel);
        {
            char neg_tok[4];
            int a = (dir == SLOPE_DIR_PX || dir == SLOPE_DIR_NX) ? 0 : 1;
            // The flipped axis is always the negative variant (PX->NX, PY->NY).
            snprintf(neg_tok, sizeof(neg_tok), "-%c",
                     (dir == SLOPE_DIR_PX || dir == SLOPE_DIR_NX) ? 'X' : 'Y');
            read_revs();
            snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=neg_probe end=%s "
                     "base=%u nbase=%llu", neg_tok, s_rev, s_nrev);
            bridge(cmd);
            {
                float picked[3];
                CHECK(parse_picked(picked) &&
                      fabsf(picked[a] - (float)m->aabb[0][a]) < 1e-3f,
                      "PM1: on the negative axis 'picked' is the min-face "
                      "the signed axis points at");
            }
            bridge("tags");
            read_revs();
            snprintf(cmd, sizeof(cmd), "confirm id=neg_probe base=%u "
                     "nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            bridge("tags");
            CHECK(parse_tag_dir("neg_probe", dirv) &&
                  fabsf(dirv[a] + 1.f) < 1e-3f,
                  "PM1: and the emitted direction points along the signed "
                  "axis, not the coordinate max");
            read_revs();
            snprintf(cmd, sizeof(cmd), "untag id=neg_probe base=%u nbase=%llu",
                     s_rev, s_nrev);
            bridge(cmd);
        }
        // restore the authored axis and re-anchor the muzzle for later steps
        m->dir = dir;
        m->revision++;
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=test_muzzle end=%s "
                 "group=pd base=%u nbase=%llu", end_tok, s_rev, s_nrev);
        bridge(cmd);
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        CHECK(reply_has("status=confirmed"),
              "the muzzle is re-anchored on the restored axis");
    }

    // 7c. PM review #541 (5): the cap centre rides the anchor plane, not the
    //     region mid-plane: a Floor-anchored barrel's cap sits at floor+rise.
    barrel = layer_by_kind(true);
    if (barrel && barrel->slope) {
        m = barrel->slope;
        m->anchor = SLOPE_ANCHOR_FLOOR;
        m->revision++;
        nav_revalidate();
        bridge("tags");
        CHECK(reply_has("re-authored"),
              "PM4: changing the anchor plane is caught as a re-authoring");
        read_revs();
        snprintf(cmd, sizeof(cmd), "untag id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=test_muzzle end=%s "
                 "group=pd base=%u nbase=%llu", end_tok, s_rev, s_nrev);
        bridge(cmd);
        bridge("tags");
        CHECK(parse_tag_pos("test_muzzle", pos1) &&
              fabsf(pos1[2] - ((float)m->aabb[0][2] + m->rise)) < 1e-3f,
              "PM5: a Floor-anchored cap centre is floor+rise, not the "
              "region mid-plane");
        m->anchor = SLOPE_ANCHOR_MID;
        m->revision++;
        read_revs();
        snprintf(cmd, sizeof(cmd), "untag id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=test_muzzle end=%s "
                 "group=pd base=%u nbase=%llu", end_tok, s_rev, s_nrev);
        bridge(cmd);
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
    }

    // 7d. PM review #541 (8): a disconnected selection must anchor inside
    //     the selection, not on the bounding-box centre nobody selected.
    body = body_layer_with_cells();
    if (body && body->volume) {
        int bb[2][3];
        goxel.image->active_layer = body;
        if (goxel.image->selection_mask)
            volume_delete(goxel.image->selection_mask);
        goxel.image->selection_mask = volume_new();
        volume_get_bbox(body->volume, bb, false);
        add_cell(goxel.image->selection_mask, bb[0][0], bb[0][1], bb[0][2]);
        add_cell(body->volume, bb[0][0], bb[0][1], bb[0][2]);
        if (bb[1][0] > bb[0][0] + 2) {
            add_cell(goxel.image->selection_mask, bb[1][0], bb[0][1], bb[0][2]);
            add_cell(body->volume, bb[1][0], bb[0][1], bb[0][2]);
        }
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=pivot id=sel_probe end=+Z "
                 "base=%u nbase=%llu", s_rev, s_nrev);
        bridge(cmd);
        {
            int ac[3];
            bool in_sel = false, on_layer = false;
            if (parse_anchored_cell("sel_probe", ac)) {
                in_sel = volume_get_alpha_at(goxel.image->selection_mask,
                                             NULL, ac) != 0;
                on_layer = volume_get_alpha_at(body->volume, NULL, ac) != 0;
            }
            CHECK(in_sel && on_layer,
                  "PM8: the anchor of a disconnected selection is inside "
                  "both the selection and the layer");
        }
        read_revs();
        snprintf(cmd, sizeof(cmd), "untag id=sel_probe base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
    }

    // 7e. PM follow-up: an orphaned draft is flagged, not left invisible.
    body = body_layer_with_cells();
    if (body && body->volume) {
        goxel.image->active_layer = body;
        select_layer(body);
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=pivot id=draft_orphan end=+Z "
                 "base=%u nbase=%llu", s_rev, s_nrev);
        bridge(cmd);
        if (parse_anchored_cell("draft_orphan", anchor_cell)) {
            snprintf(cmd, sizeof(cmd), "del %d %d %d", anchor_cell[0],
                     anchor_cell[1], anchor_cell[2]);
            bridge(cmd);
            nav_revalidate();
            bridge("tags");
            CHECK(reply_has("draft_orphan") && reply_has("status=needs_review"),
                  "PM-follow: an invalidated draft is flagged, not invisible");
            bridge("undo");
            body = body_layer_with_cells();
            if (body) goxel.image->active_layer = body;
            nav_revalidate();
        }
        read_revs();
        snprintf(cmd, sizeof(cmd), "untag id=draft_orphan base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
    }

    // 8. The pair: gox + sidecar, pinned; then a reopen re-adopts.
    snprintf(path, sizeof(path), "%s/navtest_save.gox",
             outdir && *outdir ? outdir : ".");
    snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
    bridge(cmd);
    CHECK(reply_has("saved") && reply_has("ok"),
          "navsave published the pair to %s", path);
    f = fopen(path, "rb");
    CHECK(f != NULL, "the gox really exists");
    if (f) {
        long sz;
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fclose(f);
        printf("  [saved gox: %ld bytes]\n", sz);
    }
    nav_sidecar_path(path, sidecar, sizeof(sidecar));
    f = fopen(sidecar, "rb");
    CHECK(f != NULL, "the sidecar really exists");
    if (f) fclose(f);

    // A reopen the way the editor's Open action does it: a new image, the gox
    // loaded into it.  The gox still matches its pin, so the sidecar is
    // adopted on the first navigator pass.
    {
        int rc;
        image_delete(goxel.image);
        goxel.image = image_new();
        rc = load_from_file(path, true);
        // The editor's own open flow lands on a layer that can be painted;
        // a replaced load leaves the active layer unset otherwise.
        slope_ensure_art_layer();
        nav_notify_opened(path);   // the editor's open action re-settles here
        nav_check_document();
        dump_doc("after reopen");
        CHECK(rc == 0, "reopen %s -> rc=%d", path, rc);
        bridge("tags");
        CHECK(reply_has("test_muzzle") && reply_has("status=confirmed"),
              "the reopened ship re-adopts its confirmed records");
        CHECK(!reply_has("test_pivot") || reply_has("status=confirmed"),
              "nothing in review survives a clean reopen");
    }

    // 8b. PM review #541 (6): a pin mismatch on reopen keeps the records as
    //     needs_review (recoverable meaning) and an explicit Confirm accepts
    //     the changed document; publication stays closed until then.
    {
        int rc;
        body = body_layer_with_cells();
        goxel.image->active_layer = body;
        bridge("set 7 7 7 12 34 56");          // a real edit after publishing
        goxel_export_to_file(path, "gox");     // the gox bytes move on
        image_delete(goxel.image);
        goxel.image = image_new();
        rc = load_from_file(path, true);
        slope_ensure_art_layer();
        nav_notify_opened(path);   // the editor's open action re-settles here
        nav_check_document();
        CHECK(rc == 0, "reopen the changed gox -> rc=%d", rc);
        bridge("tags");
        CHECK(reply_has("test_muzzle") && reply_has("status=needs_review") &&
              reply_has("does not pin"),
              "PM6: a pin mismatch keeps the records as needs_review, not "
              "silently cleared");
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        CHECK(call_fails(cmd), "PM6: publication stays closed over the "
              "mismatched records");
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        CHECK(reply_has("status=confirmed"),
              "PM6: an explicit Confirm is the Owner's acceptance of the "
              "changed document");
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=test_exhaust base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=test_pivot base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        bridge(cmd);
        CHECK(reply_has("saved"), "PM6: after the acceptance the pair "
              "publishes and re-pins");
    }

    // 8c. PM review #541 (5): a spaced Windows path publishes through the
    //     bridge (quoted path=) and the sidecar round-trips through adopt.
    {
        char spaced[600], sside[640];
        snprintf(cmd, sizeof(cmd), "%s/nav test",
                 outdir && *outdir ? outdir : ".");
#ifdef _WIN32
        mkdir(cmd);
#else
        mkdir(cmd, 0777);
#endif
        snprintf(spaced, sizeof(spaced), "%s/nav test/spaced ship.gox",
                 outdir && *outdir ? outdir : ".");
        snprintf(cmd, sizeof(cmd), "navsave path=\"%s\"", spaced);
        bridge(cmd);
        CHECK(reply_has("saved") && reply_has("ok"),
              "PM5: a quoted path with spaces publishes through the bridge");
        f = fopen(spaced, "rb");
        CHECK(f != NULL, "the spaced gox really exists");
        if (f) fclose(f);
        nav_sidecar_path(spaced, sside, sizeof(sside));
        f = fopen(sside, "rb");
        CHECK(f != NULL, "the spaced sidecar really exists");
        if (f) fclose(f);
        snprintf(cmd, sizeof(cmd), "navadopt path=\"%s\"", spaced);
        bridge(cmd);
        CHECK(reply_has("adopted"), "PM5: the spaced pair re-adopts "
              "(escaped paths parse back)");
        // The real art directory on Windows: backslashes and spaces.  The
        // sidecar must still be valid JSON and must parse back.
        {
            char bs[600], bside[640];
            char *q;
            snprintf(bs, sizeof(bs), "%s", spaced);
            for (q = bs; *q; q++) if (*q == '/') *q = '\\';
            snprintf(cmd, sizeof(cmd), "navsave path=\"%s\"", bs);
            bridge(cmd);
            CHECK(reply_has("saved") && reply_has("ok"),
                  "PM5: a backslash Windows path publishes through the "
                  "bridge");
            nav_sidecar_path(bs, bside, sizeof(bside));
            f = fopen(bside, "rb");
            CHECK(f != NULL, "the backslash sidecar really exists");
            if (f) fclose(f);
            snprintf(cmd, sizeof(cmd), "navadopt path=\"%s\"", bs);
            bridge(cmd);
            CHECK(reply_has("adopted 3"), "PM5: the backslash pair re-adopts "
                  "(escaped backslashes parse back)");
        }
    }

    // 8d. PM follow-up: a duplicate-id sidecar is rejected as a unit and
    //     cannot partially overwrite the working registry.
    {
        char dupgox[600], dupside[640];
        snprintf(dupgox, sizeof(dupgox), "%s/navtest_dup.gox",
                 outdir && *outdir ? outdir : ".");
        copy_file(path, dupgox);
        nav_sidecar_path(dupgox, dupside, sizeof(dupside));
        write_text(dupside,
            "{ \"format\": \"amfm.nav\", \"schema_version\": 1,\n"
            " \"source_gox\": \"x\", \"source_sha256\": \"0\","
            " \"doc_key\": 0,\n"
            " \"records\": [\n"
            "  {\"id\": \"dup\", \"kind\": \"pivot\", \"at\": \"cell\","
            " \"layer_id\": 1, \"cell\": [0,0,0], \"dir\": [0,0,1],"
            " \"status\": \"confirmed\"},\n"
            "  {\"id\": \"dup\", \"kind\": \"muzzle\", \"at\": \"cell\","
            " \"layer_id\": 1, \"cell\": [0,0,0], \"dir\": [0,0,1],"
            " \"status\": \"confirmed\"}\n"
            " ] }\n");
        snprintf(cmd, sizeof(cmd), "navadopt path=%s", dupgox);
        bridge(cmd);
        CHECK(reply_has("err navadopt failed"),
              "PM-follow: a duplicate-id sidecar is rejected as a unit");
        bridge("tags");
        CHECK(reply_has("test_muzzle"),
              "PM-follow: the rejected adopt left the working registry "
              "untouched");
        remove(dupgox);
        remove(dupside);
    }

    // 9. A stale base revision is refused, deterministically; PM7: so is a
    //    stale registry token, and a missing token is an explicit error.
    body = body_layer_with_cells();
    goxel.image->active_layer = body;
    read_revs();
    {
        snprintf(cmd, sizeof(cmd), "set 0 0 0 90 90 90 base=%u", s_rev);
        CHECK(!call_fails(cmd), "a fresh base= passes (control)");
        bridge("undo");
        stale_base = s_rev + 7;
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=late_probe end=%s "
                 "base=%u nbase=%llu", end_tok, stale_base, s_nrev);
        CHECK(call_fails(cmd) && reply_has("err doc changed"),
              "a stale base=%u is refused", stale_base);
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=late_probe end=%s "
                 "base=%u nbase=%llu", end_tok, s_rev, s_nrev + 99);
        CHECK(call_fails(cmd) && reply_has("nbase"),
              "PM7: a stale nbase is refused even on a fresh document");
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=late_probe end=%s",
                 end_tok);
        CHECK(call_fails(cmd) && reply_has("base=<rev> is required"),
              "PM7: a missing base is an explicit error, not silently fresh");
        bridge("tags");
        CHECK(!reply_has("late_probe"),
              "the refused writes left no record behind");

        // 10. A draft keeps publication closed until it is settled.
        if (body) {
            body = body_layer_with_cells();   // the undo may have swapped layers
            goxel.image->active_layer = body;
            select_layer(body);
            read_revs();
            snprintf(cmd, sizeof(cmd), "tag kind=pivot id=draft_probe end=+Z "
                     "group=probe base=%u nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            CHECK(reply_has("status=draft"), "the probe draft exists");
            snprintf(cmd, sizeof(cmd), "navsave path=%s/navtest_save2.gox",
                     outdir && *outdir ? outdir : ".");
            CHECK(call_fails(cmd), "a draft blocks publication");
            read_revs();
            snprintf(cmd, sizeof(cmd), "untag id=draft_probe base=%u "
                     "nbase=%llu", s_rev, s_nrev);
            bridge(cmd);
            CHECK(reply_has("removed"), "untag removed the draft");
        }
    }

    // 11. PM review #541 (2): a mirrored cell marker lands on the surviving
    //     cell — the mirror's half-open span, not one voxel past it.
    body = body_layer_with_cells();
    if (body && body->volume) {
        int bb[2][3], aabb[2][3], before[3], after[3];
        float rmat[4][4];
        goxel.image->active_layer = body;
        select_layer(body);
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=exhaust id=mirror_probe end=+X "
                 "base=%u nbase=%llu", s_rev, s_nrev);
        bridge(cmd);
        if (parse_anchored_cell("mirror_probe", before)) {
            volume_get_bbox(body->volume, bb, false);
            aabb[0][0] = bb[0][0]; aabb[1][0] = bb[1][0] + 1;
            aabb[0][1] = bb[0][1]; aabb[1][1] = bb[1][1] + 1;
            aabb[0][2] = bb[0][2]; aabb[1][2] = bb[1][2] + 1;
            memset(rmat, 0, sizeof(rmat));
            rmat[0][0] = rmat[1][1] = rmat[2][2] = rmat[3][3] = 1.f;
            rmat[0][0] = -1.f;
            rmat[3][0] = (float)(aabb[0][0] + aabb[1][0] - 1);
            volume_move(body->volume, rmat);
            nav_move_mirror_layer(body, 0, aabb);
            // The editor pushes one history entry at the end of the filter,
            // so one undo takes the mirror back.
            image_history_push(goxel.image);
            bridge("tags");
            if (parse_tag_pos("mirror_probe", pos1)) {
                after[0] = (int)floorf(pos1[0] - 0.5f + 0.01f);
                after[1] = (int)floorf(pos1[1] - 0.5f + 0.01f);
                after[2] = (int)floorf(pos1[2] - 0.5f + 0.01f);
            } else {
                after[0] = after[1] = after[2] = 9999;
            }
            CHECK(after[0] == aabb[0][0] + aabb[1][0] - 1 - before[0] &&
                  after[1] == before[1] && after[2] == before[2],
                  "PM2: the mirrored marker sits on min+max-1-c (%d->%d on X)",
                  before[0], after[0]);
            body = body_layer_with_cells();
            CHECK(body && volume_get_alpha_at(body->volume, NULL, after) != 0,
                  "PM2: and that cell really holds the mirrored voxel");
            bridge("undo");
            // The undo rewinds geometry, never the registry (by design): the
            // mirror moved the other body markers with the cells it just
            // rewound, so each needs its deliberate re-anchor now.
            body = body_layer_with_cells();
            if (body) {
                goxel.image->active_layer = body;
                select_layer(body);
                reanchor_cell("test_pivot", "pivot");
                reanchor_cell("test_exhaust", "exhaust");
            }
        }
        read_revs();
        snprintf(cmd, sizeof(cmd), "untag id=mirror_probe base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
    }

    // 12. PM rev3 R3-1: a failed tag must not touch the registry at all —
    //     no half-assigned draft, no revision move, nothing to Confirm.
    barrel = layer_by_kind(true);
    if (barrel && barrel->slope) {
        unsigned rev_a;
        unsigned long long nrev_a;
        goxel.image->active_layer = barrel;
        read_revs();
        rev_a = s_rev;
        nrev_a = s_nrev;
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=bad_end end=+Z "
                 "base=%u nbase=%llu", s_rev, s_nrev);
        CHECK(call_fails(cmd) && reply_has("parallel"),
              "R3-1: a tag with a non-parallel end is refused");
        read_revs();
        CHECK(s_rev == rev_a && s_nrev == nrev_a,
              "R3-1: the refused tag moved neither the document nor the registry");
        bridge("tags");
        CHECK(!reply_has("bad_end"),
              "R3-1: the refused tag left no record behind");
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=bad_end base=%u nbase=%llu",
                 s_rev, s_nrev);
        CHECK(call_fails(cmd) && reply_has("no record"),
              "R3-1: there is nothing for Confirm to bless");
        // an existing draft survives a failed re-tag untouched and still
        // confirms with its original meaning
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=draft_keep end=%s "
                 "base=%u nbase=%llu", end_tok, s_rev, s_nrev);
        bridge(cmd);
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=draft_keep end=+Z "
                 "base=%u nbase=%llu", s_rev, s_nrev);
        CHECK(call_fails(cmd), "R3-1: a failed re-tag of a draft id is refused");
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=draft_keep base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        CHECK(reply_has("status=confirmed"),
              "R3-1: the untouched draft still confirms");
        read_revs();
        snprintf(cmd, sizeof(cmd), "untag id=draft_keep base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
    }

    // 13. PM rev3 R3-2: a source-cell edit under an INACTIVE authored shape
    //     must block publication immediately — no shape GUI, no re-selecting
    //     the layer, no waiting for the editor's own refresh paths.
    barrel = layer_by_kind(true);
    body = body_layer_with_cells();
    if (barrel && barrel->slope && body) {
        int bb[2][3];
        uint8_t old[4], col[4] = { 200, 30, 30, 255 };
        volume_get_bbox(barrel->volume, bb, true);   // exact: an occupied cell
        volume_get_at(barrel->volume, NULL, bb[0], old);
        volume_set_at(barrel->volume, NULL, bb[0], col);   // recolour the seed
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        CHECK(call_fails(cmd) && reply_has("source cells changed"),
              "R3-2: an inactive shape's source edit blocks publication "
              "without any shape GUI");
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        CHECK(call_fails(cmd),
              "R3-2: Confirm also refuses a muzzle whose source changed");
        volume_set_at(barrel->volume, NULL, bb[0], old);   // restore the cell
        read_revs();
        snprintf(cmd, sizeof(cmd), "untag id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        goxel.image->active_layer = barrel;
        read_revs();
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=test_muzzle end=%s "
                 "group=pd base=%u nbase=%llu", end_tok, s_rev, s_nrev);
        bridge(cmd);
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=test_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        bridge(cmd);
        CHECK(reply_has("status=confirmed"),
              "R3-2: with the source restored the muzzle re-anchors clean");
    }

    // 14. PM rev3 R3-3: republishing preserves the previous pair as
    //     .lastgood; an export that cannot write reports failure and touches
    //     nothing.
    {
        char lg[660], lgside[680], hex1[65], hex2[65];
        uint8_t sha1[32];
        FILE *pf;
        nav_sha256_file(path, sha1);
        nav_sha256_hex(sha1, hex1);
        body = body_layer_with_cells();
        goxel.image->active_layer = body;
        bridge("set 6 6 6 20 20 20");
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        bridge(cmd);
        CHECK(reply_has("saved") && reply_has("ok"),
              "R3-3: republishing over an existing pair succeeds");
        snprintf(lg, sizeof(lg), "%s.lastgood", path);
        snprintf(lgside, sizeof(lgside), "%s.lastgood", sidecar);
        pf = fopen(lg, "rb");
        CHECK(pf != NULL, "R3-3: the previous gox is preserved as last-good");
        if (pf) fclose(pf);
        {
            char text2[4096];
            bool ok_lg = false, ok_new = false;
            pf = fopen(lgside, "rb");
            if (pf) {
                size_t n = fread(text2, 1, sizeof(text2) - 1, pf);
                fclose(pf);
                text2[n] = 0;
                ok_lg = strstr(text2, hex1) != NULL;
            }
            nav_sha256_file(path, sha1);
            nav_sha256_hex(sha1, hex2);
            pf = fopen(sidecar, "rb");
            if (pf) {
                size_t n = fread(text2, 1, sizeof(text2) - 1, pf);
                fclose(pf);
                text2[n] = 0;
                ok_new = strcmp(hex1, hex2) != 0 && strstr(text2, hex2) != NULL;
            }
            CHECK(ok_lg, "R3-3: the last-good sidecar still pins the previous gox");
            CHECK(ok_new, "R3-3: the new sidecar pins the new gox");
        }
        // A publish that cannot write must report failure and touch nothing.
        nav_sha256_file(path, sha1);
        nav_sha256_hex(sha1, hex1);
        snprintf(cmd, sizeof(cmd), "navsave path=%s/no-such-dir/x.gox",
                 outdir && *outdir ? outdir : ".");
        CHECK(call_fails(cmd), "R3-3: an export that cannot write reports "
              "failure instead of pretending success");
        nav_sha256_file(path, sha1);
        nav_sha256_hex(sha1, hex2);
        CHECK(!strcmp(hex1, hex2),
              "R3-3: the failed publish left the approved pair untouched");
    }

    // 15. PM rev3 R3-4: a rev1 sidecar (pin matches, no fingerprints at all)
    //     keeps cell records green but holds shape records for re-anchor.
    {
        char oldgox[600], oldside[640], hex1[65], side_txt[2048];
        uint8_t sha1[32];
        int bb[2][3];
        snprintf(oldgox, sizeof(oldgox), "%s/navtest_rev1.gox",
                 outdir && *outdir ? outdir : ".");
        copy_file(path, oldgox);
        nav_sidecar_path(oldgox, oldside, sizeof(oldside));
        nav_sha256_file(oldgox, sha1);
        nav_sha256_hex(sha1, hex1);
        body = body_layer_with_cells();
        barrel = layer_by_kind(true);
        {
            // The first cell the layer actually occupies: a bbox corner can
            // combine minima of different cells and be empty itself.
            volume_iterator_t it;
            uint8_t c[4];
            it = volume_get_iterator(body->volume, VOLUME_ITER_VOXELS |
                                                   VOLUME_ITER_SKIP_EMPTY);
            bb[0][0] = bb[0][1] = bb[0][2] = 0;
            while (volume_iter(&it, bb[0])) {
                volume_get_at(body->volume, &it, bb[0], c);
                if (c[3]) break;
            }
        }
        snprintf(side_txt, sizeof(side_txt),
            "{ \"format\": \"amfm.nav\", \"schema_version\": 1,\n"
            " \"asset_id\": \"rev1\", \"source_gox\": \"x\",\n"
            " \"source_sha256\": \"%s\", \"doc_key\": 0,\n"
            " \"records\": [\n"
            "  {\"id\": \"old_muzzle\", \"kind\": \"muzzle\", \"at\": \"shape_end\","
            " \"layer_id\": %d, \"shape_sign\": 1, \"status\": \"confirmed\"},\n"
            "  {\"id\": \"old_pivot\", \"kind\": \"pivot\", \"at\": \"cell\","
            " \"layer_id\": %d, \"cell\": [%d, %d, %d], \"dir\": [0, 0, 1],"
            " \"status\": \"confirmed\"}\n"
            " ] }\n",
            hex1, barrel ? barrel->id : 1, body->id, bb[0][0], bb[0][1], bb[0][2]);
        write_text(oldside, side_txt);
        snprintf(cmd, sizeof(cmd), "navadopt path=%s", oldgox);
        bridge(cmd);
        bridge("tags");
        CHECK(reply_has("old_pivot") && reply_has("status=confirmed"),
              "R3-4: rev1 cell records (no fingerprint needed) stay confirmed");
        CHECK(reply_has("old_muzzle") && reply_has("predates fingerprints"),
              "R3-4: rev1 shape records without a fingerprint are held for "
              "re-anchor");
        read_revs();
        snprintf(cmd, sizeof(cmd), "confirm id=old_muzzle base=%u nbase=%llu",
                 s_rev, s_nrev);
        CHECK(call_fails(cmd) && reply_has("re-anchor"),
              "R3-4: the fingerprint-less shape cannot be re-confirmed");
        // and a shape change keeps it in review (the regression asked for)
        if (barrel && barrel->slope) {
            barrel->slope->rise += 0.01f;
            barrel->slope->revision++;
            nav_revalidate();
            bridge("tags");
            CHECK(reply_has("old_muzzle") && reply_has("status=needs_review"),
                  "R3-4: a shape change keeps the fingerprint-less record in "
                  "review");
            barrel->slope->rise -= 0.01f;
            barrel->slope->revision++;
        }
        // the current v2 sidecar still adopts green (v2 records preserved)
        snprintf(cmd, sizeof(cmd), "navadopt path=%s", path);
        bridge(cmd);
        CHECK(reply_has("adopted 3"),
              "R3-4: the current v2 sidecar still adopts green");
        remove(oldgox);
        remove(oldside);
    }

    // 16. PM rev4: paired publication is fail-safe (P0-1), the candidate is
    //     validated as real content (P0-2), and the staging path never
    //     becomes the Owner's remembered export destination (P1).
    {
        char hex_a[65], hex_b[65];
        uint8_t sha_a[32];
        char whyb[160];
        nav_sha256_file(path, sha_a);
        nav_sha256_hex(sha_a, hex_a);
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        bridge(cmd);
        CHECK(reply_has("saved") && reply_has("ok"),
              "rev4: a normal publish still succeeds");
        {
            char lgp[700];
            FILE *pf;
            snprintf(lgp, sizeof(lgp), "%s.lastgoodprev", path);
            pf = fopen(lgp, "rb");
            CHECK(pf != NULL,
                  "rev5: the earlier backup is demoted, not deleted");
            if (pf) fclose(pf);
        }
        CHECK(goxel.image->export_path == NULL ||
              !strstr(goxel.image->export_path, ".new"),
              "P1: the staging path never becomes the remembered export path");
        // P0-2: the published gox round-trips; a truncated one is refused
        CHECK(nav_gox_validate(path, whyb, sizeof(whyb)),
              "P0-2: the published gox loads back with matching layers/cells");
        {
            char trunc[600];
            FILE *src, *dst;
            long sz;
            char *buf;
            snprintf(trunc, sizeof(trunc), "%s/navtest_trunc.gox",
                     outdir && *outdir ? outdir : ".");
            src = fopen(path, "rb");
            dst = fopen(trunc, "wb");
            if (src && dst) {
                fseek(src, 0, SEEK_END);
                sz = ftell(src);
                fseek(src, 0, SEEK_SET);
                buf = malloc(sz);
                if (buf) {
                    size_t n = fread(buf, 1, sz, src);
                    fwrite(buf, 1, n / 3, dst);  // header survives, content does not
                    free(buf);
                }
            }
            if (src) fclose(src);
            if (dst) fclose(dst);
            CHECK(!nav_gox_validate(trunc, whyb, sizeof(whyb)),
                  "P0-2: a truncated gox with an intact header is refused (%s)",
                  whyb);
            remove(trunc);
        }
        // PM rev5: the roundtrip compares the equipment semantics, not just
        // counts.  Re-anchor the muzzle onto a re-authored rise: the counts
        // stay equal, but the published file no longer serializes what the
        // record references — the candidate comparison must say so.
        barrel = layer_by_kind(true);
        if (barrel && barrel->slope) {
            barrel->slope->rise += 0.02f;
            barrel->slope->revision++;
            goxel.image->active_layer = barrel;
            read_revs();
            snprintf(cmd, sizeof(cmd), "untag id=test_muzzle base=%u nbase=%llu",
                     s_rev, s_nrev);
            bridge(cmd);
            read_revs();
            snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=test_muzzle end=%s "
                     "group=pd base=%u nbase=%llu", end_tok, s_rev, s_nrev);
            bridge(cmd);
            read_revs();
            snprintf(cmd, sizeof(cmd), "confirm id=test_muzzle base=%u nbase=%llu",
                     s_rev, s_nrev);
            bridge(cmd);
            CHECK(!nav_gox_validate(path, whyb, sizeof(whyb)),
                  "rev5: a record whose authored attributes no longer match "
                  "the file is refused (%s)", whyb);
            barrel->slope->rise -= 0.02f;
            barrel->slope->revision++;
            read_revs();
            snprintf(cmd, sizeof(cmd), "untag id=test_muzzle base=%u nbase=%llu",
                     s_rev, s_nrev);
            bridge(cmd);
            read_revs();
            snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=test_muzzle end=%s "
                     "group=pd base=%u nbase=%llu", end_tok, s_rev, s_nrev);
            bridge(cmd);
            read_revs();
            snprintf(cmd, sizeof(cmd), "confirm id=test_muzzle base=%u nbase=%llu",
                     s_rev, s_nrev);
            bridge(cmd);
            CHECK(nav_gox_validate(path, whyb, sizeof(whyb)),
                  "rev5: with the shape restored the semantic roundtrip passes");
        }
        // change the document so a new publish would carry different bytes
        body = body_layer_with_cells();
        goxel.image->active_layer = body;
        bridge("set 5 5 5 30 30 30");
        CHECK(!nav_gox_validate(path, whyb, sizeof(whyb)),
              "P0-2: the previous gox now reads back short of the live "
              "cells (%s)", whyb);
        // P0-1: sidecar staging cannot open -> live pair untouched
        {
            char cdir[700], inner[740];
            FILE *ip;
            snprintf(cdir, sizeof(cdir), "%s.new", sidecar);
            remove(cdir);
            mkdir(cdir);
            snprintf(inner, sizeof(inner), "%s/x", cdir);
            ip = fopen(inner, "wb");
            if (ip) fclose(ip);
            snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
            CHECK(call_fails(cmd),
                  "P0-1: a sidecar staging failure is reported, not swallowed");
            remove(inner);
            rmdir(cdir);
            nav_sha256_file(path, sha_a);
            nav_sha256_hex(sha_a, hex_b);
            CHECK(!strcmp(hex_a, hex_b),
                  "P0-1: the staging failure left the live pair untouched");
        }
        // P0-1: interrupted promotion at the gox switch -> auto-restore
        _putenv("GOXEL_NAV_FAULT=promote_gox");
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        CHECK(call_fails(cmd),
              "P0-1: an interrupted gox promotion is reported");
        _putenv("GOXEL_NAV_FAULT=");
        nav_sha256_file(path, sha_a);
        nav_sha256_hex(sha_a, hex_b);
        CHECK(!strcmp(hex_a, hex_b),
              "P0-1: the previous pair was restored after the gox switch failed");
        // P0-1: interrupted promotion at the sidecar switch -> auto-restore
        _putenv("GOXEL_NAV_FAULT=promote_side");
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        CHECK(call_fails(cmd),
              "P0-1: an interrupted sidecar promotion is reported");
        _putenv("GOXEL_NAV_FAULT=");
        nav_sha256_file(path, sha_a);
        nav_sha256_hex(sha_a, hex_b);
        CHECK(!strcmp(hex_a, hex_b),
              "P0-1: the previous pair was restored after the sidecar switch failed");
        // no split pair: the live sidecar still pins the live gox
        {
            FILE *pf = fopen(sidecar, "rb");
            char t[4096];
            size_t n = 0;
            bool pinned = false;
            if (pf) {
                n = fread(t, 1, sizeof(t) - 1, pf);
                fclose(pf);
                t[n] = 0;
                pinned = strstr(t, hex_a) != NULL;
            }
            CHECK(pinned,
                  "P0-1: the live sidecar still pins the live gox after the "
                  "failed promotions");
        }
        // and a clean retry now publishes the changed document
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        bridge(cmd);
        nav_sha256_file(path, sha_a);
        nav_sha256_hex(sha_a, hex_b);
        CHECK(reply_has("saved") && strcmp(hex_a, hex_b) != 0,
              "rev4: the retry after the faults publishes the new pair");
    }

    // 17. PM rev7: a failed backup rotation never deletes the backup pair;
    //     the publication aborts fail-closed and every recoverable file stays.
    {
        char hex_a[65], hex_b[65];
        uint8_t sha_a[32];
        char lg[700], lgs[700], lgp[700], cand[700];
        FILE *pf;

        snprintf(lg,  sizeof(lg),  "%s.lastgood", path);
        snprintf(lgs, sizeof(lgs), "%s.lastgood", sidecar);
        snprintf(lgp, sizeof(lgp), "%s.lastgoodprev", path);
        goxel.image->active_layer = body_layer_with_cells();
        bridge("set 6 6 6 30 30 30");
        nav_sha256_file(path, sha_a);
        nav_sha256_hex(sha_a, hex_a);
        _putenv("GOXEL_NAV_FAULT=rotate_gox");
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        CHECK(call_fails(cmd) && reply_has("cannot rotate"),
              "rev7: a failed gox backup rotation aborts the publication");
        _putenv("GOXEL_NAV_FAULT=");
        pf = fopen(lg, "rb");
        CHECK(pf != NULL,
              "rev7: the failed rotation kept .lastgood instead of deleting it");
        if (pf) fclose(pf);
        pf = fopen(lgp, "rb");
        CHECK(pf != NULL, "rev7: .lastgoodprev also survived the failed rotation");
        if (pf) fclose(pf);
        pf = fopen(lgs, "rb");
        CHECK(pf != NULL, "rev7: the sidecar backup was not touched either");
        if (pf) fclose(pf);
        nav_sha256_file(path, sha_a);
        nav_sha256_hex(sha_a, hex_b);
        CHECK(!strcmp(hex_a, hex_b),
              "rev7: the live pair is byte-identical after the aborted rotation");
        snprintf(cand, sizeof(cand), "%s.new", path);
        CHECK(fopen(cand, "rb") == NULL, "rev7: no staging file stranded");
        // and the next clean publish rotates normally again
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        bridge(cmd);
        nav_sha256_file(path, sha_a);
        nav_sha256_hex(sha_a, hex_b);
        CHECK(reply_has("saved") && strcmp(hex_a, hex_b) != 0,
              "rev7: the retry after the rotation fault publishes");
        // the sidecar half of the rotation is guarded the same way
        bridge("set 7 7 7 30 30 30");
        nav_sha256_file(path, sha_a);
        nav_sha256_hex(sha_a, hex_a);
        _putenv("GOXEL_NAV_FAULT=rotate_side");
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        CHECK(call_fails(cmd) && reply_has("cannot rotate"),
              "rev7: a failed sidecar backup rotation aborts the publication");
        _putenv("GOXEL_NAV_FAULT=");
        pf = fopen(lgs, "rb");
        CHECK(pf != NULL, "rev7: the sidecar backup survived its own rotation fault");
        if (pf) fclose(pf);
        nav_sha256_file(path, sha_a);
        nav_sha256_hex(sha_a, hex_b);
        CHECK(!strcmp(hex_a, hex_b),
              "rev7: the live pair is byte-identical after the sidecar abort");
        snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
        bridge(cmd);
        CHECK(reply_has("saved"), "rev7: the final retry publishes clean");

        // Owner session requirement (2026-11-08): rcs is a first-class kind,
        // tagged exactly like a muzzle so thruster anchors can be authored.
        barrel = layer_by_kind(true);
        if (barrel) {
            char buf[16384];
            size_t n = 0;
            FILE *f;
            goxel.image->active_layer = barrel;
            select_layer(barrel);
            read_revs();
            snprintf(cmd, sizeof(cmd), "tag kind=rcs id=test_rcs end=%s group=pd "
                     "base=%u nbase=%llu", end_tok, s_rev, s_nrev);
            bridge(cmd);
            CHECK(reply_has("kind=rcs") && reply_has("status=draft"),
                  "rev7: rcs is a first-class tag kind");
            read_revs();
            snprintf(cmd, sizeof(cmd), "confirm id=test_rcs base=%u nbase=%llu",
                     s_rev, s_nrev);
            bridge(cmd);
            CHECK(reply_has("confirmed"), "rev7: the rcs record confirms");
            snprintf(cmd, sizeof(cmd), "navsave path=%s", path);
            bridge(cmd);
            CHECK(reply_has("saved"), "rev7: publishing with an rcs record works");
            f = fopen(sidecar, "rb");
            if (f) {
                n = fread(buf, 1, sizeof(buf) - 1, f);
                fclose(f);
                buf[n] = 0;
            }
            CHECK(n > 0 && strstr(buf, "\"kind\": \"rcs\"") != NULL,
                  "rev7: the sidecar carries the rcs kind verbatim");
        }
    }

    // rev10 (#541 A2, PM correction 2026-11-08): the Worker pointer overlay
    // — attributed session-only cells, never geometry, never the Owner mask.
    {
        int p[3];
        nav_pointer_clear();
        CHECK(nav_pointer_count() == 0, "rev10: pointers start empty");
        nav_pointer_add(1, 2, 3);
        nav_pointer_add(1, 2, 3);
        CHECK(nav_pointer_count() == 1, "rev10: duplicate pointer ignored");
        nav_pointer_add(4, 5, 6);
        CHECK(nav_pointer_count() == 2, "rev10: two distinct pointers");
        CHECK(nav_pointer_get(0, p) && p[0] == 4 && p[1] == 5 && p[2] == 6,
              "rev10: pointer readback exact");
        CHECK(!nav_pointer_get(2, p), "rev10: out-of-range pointer refused");
        nav_pointer_del(1, 2, 3);
        CHECK(nav_pointer_count() == 1 && nav_pointer_get(0, p) &&
              p[0] == 4, "rev10: pointer delete is exact");
        nav_pointer_clear();
        CHECK(nav_pointer_count() == 0, "rev10: pointer clear");

        bridge("pointer add 7 8 9");
        CHECK(reply_has("ptr add 7 8 9 count=1"),
              "rev10: bridge pointer add works");
        bridge("pointer add 7 8 9");
        bridge("pointer add -1 -2 -3");
        bridge("pointer");
        CHECK(reply_has("ptr count=2") && reply_has("ptr -1 -2 -3"),
              "rev10: bridge pointer list is exact");
        bridge("pointer del 7 8 9");
        bridge("pointer");
        CHECK(reply_has("ptr count=1"), "rev10: bridge pointer del works");
        bridge("pointer clear");
        bridge("pointer");
        CHECK(reply_has("ptr count=0"), "rev10: bridge pointer clear works");

        bridge("pickmode on");
        CHECK(reply_has("pickmode 1") && tool_selection_get_pick(),
              "rev10: bridge enables the single-voxel pick mode");
        bridge("pickmode off");
        CHECK(reply_has("pickmode 0") && !tool_selection_get_pick(),
              "rev10: bridge disables the single-voxel pick mode");
    }

    printf("nav test: all steps executed\n");
    fflush(stdout);
    printf("== nav test: %d checks, %d failed ==\n", s_checks, s_failed);
    fflush(stdout);
    return s_failed ? 1 : 0;
}
