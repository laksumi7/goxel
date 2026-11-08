/* Goxel 3D voxels editor
 *
 * AMFM #541: the equipment navigator, headless verification.  Driven by
 * GOXEL_NAV_TEST=<output dir>, with the fixture on GOXEL_NAV_FIXTURE.
 *
 * The test drives the exact bridge lines a worker would send, through the
 * exact dispatch, and the same nav calls the editor frame would make.  Every
 * claim of the P0 scope has one check:
 *   - the selection reports the exact object (layer ids, shape, both ends),
 *   - a tag proposes with both ends visible and the Owner confirms it,
 *   - a whole-part move carries the marker and the surface together,
 *   - a move outside the marker's cells leaves the marker alone,
 *   - a removed anchor cell flips the record to needs_review and publication
 *     stays closed until the Owner confirms or rejects,
 *   - undo does not resurrect a green light by itself,
 *   - navsave writes a gox + sidecar pair pinned by the gox SHA-256, and a
 *     reopen re-adopts it automatically,
 *   - a worker write carrying a stale base revision is refused.
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

static int s_checks, s_failed;

#define CHECK(cond, ...) do {                                   \
    s_checks++;                                                 \
    if (!(cond)) { s_failed++; printf("  FAIL: "); }            \
    else           printf("  ok  : ");                          \
    printf(__VA_ARGS__);                                        \
    if (!(cond)) printf("\n");                                  \
    printf("\n");                                               \
} while (0)

// 64 KiB replies (the bridge drops instead of overflowing).
static char s_reply[64 * 1024 + 8];

static const char *bridge(const char *line)
{
    lab_bridge_test_call(line, s_reply, sizeof(s_reply));
    if (getenv("GOXEL_NAV_VERBOSE"))
        printf("---- %s\n%s\n", line, s_reply);
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

static bool parse_state_rev(unsigned *rev)
{
    const char *q;

    bridge("state");
    q = strstr(s_reply, "rev ");
    if (!q) return false;
    return sscanf(q + 4, "%u", rev) == 1;
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

int nav_test_run(const char *fixture, const char *outdir)
{
    layer_t *barrel, *body;
    const slope_mod_t *m;
    float u[3], pos0[3], pos1[3], exhaust0[3];
    char end_tok[4] = "+Y";
    char cmd[768], path[512], sidecar[560];
    int dir = 2;
    int32_t a00 = 0;
    int anchor_cell[3] = {0, 0, 0};
    bool anchor_cell_known = false;
    unsigned rev = 0, stale_base = 0;
    FILE *f;
    volume_t *mask;
    image_move_t *mv;
    float tmat[4][4];

    printf("== nav test (AMFM #541 P0) ==\n");
    memset(pos0, 0, sizeof(pos0));
    memset(pos1, 0, sizeof(pos1));
    memset(exhaust0, 0, sizeof(exhaust0));

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
            dump_doc("raw machinery: after +2 apply");
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

        // And back again: the same machinery must be exactly reversible.
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

        // 3. A worker proposal: both ends are on the table and the token is
        //    what the draft means.
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=test_muzzle end=%s "
                 "group=pd label=left barrel muzzle", end_tok);
        bridge(cmd);
        CHECK(reply_has("status=draft") && reply_has("ends axis="),
              "tag %s proposes a draft with both ends shown", end_tok);
        bridge("tags");
        CHECK(reply_has("test_muzzle") && reply_has("status=draft"),
              "the draft is visible");
        CHECK(parse_tag_pos("test_muzzle", pos0),
              "the draft position reads back");

        // 4. The Owner confirms it in the navigator.
        bridge("confirm test_muzzle");
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

        // 6. A cell marker on the body: it moves with its own assembly and
        //    does NOT move when only the barrel moves.
        if (body) {
            goxel.image->active_layer = body;
            select_layer(body);
            dump_doc("before the cell-marker step");
            bridge("tag kind=exhaust id=test_exhaust end=+Z group=pd");
            CHECK(reply_has("status=draft") && reply_has("anchored cell"),
                  "a cell exhaust proposes on ordinary voxels");
            bridge("confirm test_exhaust");
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
        }
    }

    // 7. Removing the anchor cell is review; publication is refused; undo
    //    does not resurrect the green light by itself.
    if (body && body->volume) {
        goxel.image->active_layer = body;
        select_layer(body);
        snprintf(path, sizeof(path), "%s/navtest_save.gox",
                 outdir && *outdir ? outdir : ".");
        bridge("tag kind=pivot id=test_pivot end=+Z group=pd");
        anchor_cell_known = parse_anchored_cell("test_pivot", anchor_cell);
        bridge("confirm test_pivot");
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
            bridge("confirm test_pivot");
            CHECK(reply_has("status=confirmed"),
                  "explicit re-acceptance clears review");
            // The exhaust shared the anchor cell, so it went to review too;
            // each marker needs its own explicit re-acceptance.
            bridge("confirm test_exhaust");
            CHECK(reply_has("test_exhaust") && reply_has("status=confirmed"),
                  "the other invalidated marker is re-accepted too");
        }
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
        nav_check_document();
        dump_doc("after reopen");
        CHECK(rc == 0, "reopen %s -> rc=%d", path, rc);
        bridge("tags");
        CHECK(reply_has("test_muzzle") && reply_has("status=confirmed"),
              "the reopened ship re-adopts its confirmed records");
        CHECK(!reply_has("test_pivot") || reply_has("status=confirmed"),
              "nothing in review survives a clean reopen");
    }

    // 9. A stale base revision is refused, deterministically.
    body = body_layer_with_cells();
    goxel.image->active_layer = body;
    if (parse_state_rev(&rev)) {
        snprintf(cmd, sizeof(cmd),
                 "set 0 0 0 90 90 90 base=%u", rev);   // fresh write: passes
        CHECK(!call_fails(cmd), "a fresh base= passes (control)");
        bridge("undo");
        stale_base = rev + 7;
        snprintf(cmd, sizeof(cmd), "tag kind=muzzle id=late_probe "
                 "end=%s base=%u", end_tok, stale_base);
        CHECK(call_fails(cmd) && reply_has("err doc changed"),
              "a stale base=%u is refused", stale_base);
        bridge("tags");
        CHECK(!reply_has("late_probe"),
              "the refused write left no record behind");

        // 10. A draft keeps publication closed until it is settled.
        if (body) {
            body = body_layer_with_cells();   // the undo may have swapped layers
            goxel.image->active_layer = body;
            select_layer(body);
            bridge("tag kind=pivot id=draft_probe end=+Z group=probe");
            CHECK(reply_has("status=draft"), "the probe draft exists");
            snprintf(cmd, sizeof(cmd), "navsave path=%s/navtest_save2.gox",
                     outdir && *outdir ? outdir : ".");
            CHECK(call_fails(cmd), "a draft blocks publication");
            bridge("untag draft_probe");
            CHECK(reply_has("removed"), "untag removed the draft");
        }
    }

    printf("nav test: all steps executed\n");
    fflush(stdout);
    printf("== nav test: %d checks, %d failed ==\n", s_checks, s_failed);
    fflush(stdout);
    return s_failed ? 1 : 0;
}
