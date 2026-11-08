/* Goxel 3D voxels editor
 *
 * AMFM #532 A3-P0 / A3-P2 (under #534): see lab_bridge.h.
 */

#include "lab_bridge.h"

// winsock2 must come before windows.h (which goxel.h pulls in through GLFW).
#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#endif

#include "goxel.h"
#include "image.h"
#include "layer.h"
#include "nav.h"
#include "slope.h"
#include "volume.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    typedef SOCKET sock_t;
#    define BADSOCK INVALID_SOCKET
#    define sock_close(s) closesocket(s)
#    define sock_lasterr() WSAGetLastError()
#    define sock_wouldblock(e) ((e) == WSAEWOULDBLOCK)
#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
    typedef int sock_t;
#    define BADSOCK (-1)
#    define sock_close(s) close(s)
#    define sock_lasterr() errno
#    define sock_wouldblock(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#endif

#define LAB_IN_MAX   (8 * 1024)
#define LAB_OUT_MAX  (64 * 1024)
#define LAB_MAXPOS   100000

// The history walk needs the raw uthash list macros.
#include "uthash.h"

static struct {
    sock_t  listen_fd;
    sock_t  client_fd;
    int     port;
    char    doc[512];
    char    in[LAB_IN_MAX];
    size_t  in_len;
    char    out[LAB_OUT_MAX];
    size_t  out_len;    // bytes queued
    size_t  out_pos;    // bytes already sent
    int     enabled;
} s;

static int count_cells(const volume_t *vol)
{
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3], n = 0;

    if (!vol) return 0;
    // The iterator walks whole tiles, so the alpha byte is what makes a cell
    // occupied; without this check every layer counts one full tile.
    iter = volume_get_iterator(vol, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, p)) {
        volume_get_at(vol, &iter, p, c);
        if (c[3]) n++;
    }
    return n;
}

static void out_add(const char *text)
{
    size_t n = strlen(text);

    if (s.out_len + n + 1 > sizeof(s.out)) return;   // drop, never overflow
    memcpy(s.out + s.out_len, text, n);
    s.out_len += n;
    s.out[s.out_len] = '\0';
}

static void out_fmt(const char *fmt, ...)
{
    char buf[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out_add(buf);
}

static void drop_client(const char *why)
{
    if (s.client_fd != BADSOCK) {
        printf("lab bridge: client gone (%s)\n", why);
        fflush(stdout);
        sock_close(s.client_fd);
        s.client_fd = BADSOCK;
    }
    s.in_len = 0;
    s.out_len = s.out_pos = 0;
}

static void set_nonblock(sock_t fd)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
#else
    fcntl(fd, F_SETFL, O_NONBLOCK);
#endif
}

static int cur_pid(void)
{
#ifdef _WIN32
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

// The bridge must never fight the manual editing flow: a shape session owns the
// viewport handles, and a generated-shape layer holds almost no voxels, so the
// brush (and the bridge) can neither add nor subtract there.
static const char *edit_guard(void)
{
    if (slope_session_is_open())
        return "a shape session is open in the editor";
    if (!goxel.image || !goxel.image->active_layer)
        return "no active layer";
    if (goxel.image->active_layer->slope)
        return "the active layer is a generated-shape layer";
    return NULL;
}

// AMFM #541: defined with the other navigator verbs; the earlier verbs (state,
// set) report and guard on the same revision.
static uint32_t doc_rev(void);
static bool rev_check(const char *args, uint32_t *rev_output);

static void cmd_state(void)
{
    int aabb[2][3] = {{0, 0, 0}, {0, 0, 0}};
    layer_t *l;
    image_t *img = goxel.image;
    image_t *hist;
    int i = 0, depth = 0;

    out_fmt("lab 1\npid %d\nport %d\ndoc %s\nframe %d\nmodified %d\n",
            cur_pid(), s.port, s.doc[0] ? s.doc : "-", goxel.frame_count,
            img ? (int)(image_get_key(img) != img->saved_key) : 0);
    if (!img) {
        out_add("no image\nok\n");
        return;
    }
    // AMFM #541: the document revision a worker carries in `base=` on every
    // mutating verb; it changes whenever the image changes at all.  The
    // registry revision (PM review #541 (7)) rides along as `nbase=`: it
    // moves on every semantic write, which the document revision cannot see.
    out_fmt("rev %u\nnavrev %llu\ntags %d\n", doc_rev(),
            (unsigned long long)nav_revision(), nav_count());
    out_fmt("painter_mode %d\n", goxel.painter.mode);
    out_fmt("pickmode %d\n", tool_selection_get_pick());
    out_fmt("ptr %d\n", nav_pointer_count());
    if (img->active_layer)
        out_fmt("active '%s' lid=%d shape=%d cells=%d\n",
                img->active_layer->name, img->active_layer->id,
                img->active_layer->slope ? 1 : 0,
                count_cells(img->active_layer->volume));
    if (img->selection_mask && volume_get_bbox(img->selection_mask, aabb, true))
        out_fmt("selection %d %d %d %d %d %d\n", aabb[0][0], aabb[0][1],
                aabb[0][2], aabb[1][0], aabb[1][1], aabb[1][2]);
    else
        out_add("selection none\n");
    DL_FOREACH(img->layers, l) {
        out_fmt("layer[%d] '%s' lid=%d cells=%d shape=%d%s\n", i++, l->name,
                l->id, count_cells(l->volume), l->slope ? 1 : 0,
                l == img->active_layer ? " ACTIVE" : "");
    }
    // The history is a plain list of snapshots and `img` itself is not part of
    // it, so the walk has to stop on NULL (upstream's image_history_resize
    // compares against img and would run off the end).
    DL_FOREACH2(img->history, hist, history_next) depth++;
    out_fmt("history %d\nok\n", depth);
}

// One bounded edit: a single voxel, through the same history the brush uses.
static void cmd_set(const char *args, int del)
{
    int pos[3], before, after;
    uint8_t col[4] = {0, 0, 0, 0};
    const char *err;
    layer_t *l;

    if (sscanf(args, "%d %d %d", &pos[0], &pos[1], &pos[2]) != 3) {
        out_add("err usage: set x y z [r g b] | del x y z\n");
        return;
    }
    if (pos[0] < -LAB_MAXPOS || pos[0] > LAB_MAXPOS ||
        pos[1] < -LAB_MAXPOS || pos[1] > LAB_MAXPOS ||
        pos[2] < -LAB_MAXPOS || pos[2] > LAB_MAXPOS) {
        out_add("err position out of range\n");
        return;
    }
    // AMFM #541: a worker edit that assumed a document revision the editor
    // has already left behind is refused, never replayed.
    if (!rev_check(args, NULL)) return;
    err = edit_guard();
    if (err) {
        out_fmt("err %s\n", err);
        return;
    }
    l = goxel.image->active_layer;
    if (del) {
        volume_get_at(l->volume, NULL, pos, col);
        if (col[3] == 0) {
            out_fmt("noop %d %d %d already empty\n", pos[0], pos[1], pos[2]);
            return;
        }
        // The removal writes transparent: reading the cell back and writing
        // it again (the old behaviour) left the voxel standing.
        col[0] = col[1] = col[2] = col[3] = 0;
    } else if (sscanf(args, "%*d %*d %*d %hhu %hhu %hhu",
                      &col[0], &col[1], &col[2]) == 3) {
        col[3] = 255;
    } else {
        memcpy(col, goxel.painter.color, sizeof(col));
        col[3] = 255;
    }

    before = count_cells(l->volume);
    volume_set_at(l->volume, NULL, pos, col);
    image_history_push(goxel.image);
    after = count_cells(l->volume);
    out_fmt("%s %d %d %d cells %d->%d history pushed\n", del ? "del" : "set",
            pos[0], pos[1], pos[2], before, after);
    out_add("ok\n");
}

// One round barrel: rounds off a single voxel through the editor's own shape
// path.  Same guards as the brush edits, and the result is one history entry,
// so a single Ctrl+Z in the editor takes the barrel back to its square cell.
static void cmd_shape(const char *args)
{
    char buf[192];
    int cell[3];
    int before, layers_before = 0, layers_after = 0;
    float rise = 0.45f;
    char name[64] = "";
    const char *err;
    layer_t *l;
    int rc;

    // A local copy: the base= guard token cannot reach the layer name.
    snprintf(buf, sizeof(buf), "%s", args ? args : "");
    // AMFM #541: refuse a stale worker write before it touches the model.
    if (!rev_check(args, NULL)) return;
    // Revcheck acted on the guard value; drop it from the parse copy.
    {
        char *q = strstr(buf, " base=");
        if (q) *q = 0;
    }
    if (sscanf(buf, "%d %d %d %f %63s", &cell[0], &cell[1], &cell[2],
               &rise, name) < 3) {
        out_add("err usage: shape x y z [rise] [name]\n");
        return;
    }
    if (cell[0] < -LAB_MAXPOS || cell[0] > LAB_MAXPOS ||
        cell[1] < -LAB_MAXPOS || cell[1] > LAB_MAXPOS ||
        cell[2] < -LAB_MAXPOS || cell[2] > LAB_MAXPOS) {
        out_add("err position out of range\n");
        return;
    }
    if (rise <= 0.f || rise > 1.f) {
        out_add("err rise must be in (0, 1]\n");
        return;
    }
    err = edit_guard();
    if (err) {
        out_fmt("err %s\n", err);
        return;
    }
    DL_FOREACH(goxel.image->layers, l) layers_before++;
    before = count_cells(goxel_get_layers_volume(goxel.image));
    rc = slope_apply_shape_at(cell, name[0] ? name : NULL, rise);
    DL_FOREACH(goxel.image->layers, l) layers_after++;

    switch (rc) {
        case 0:
            out_fmt("shape %d %d %d rise %.3f layers %d->%d cells %d history "
                    "pushed\n", cell[0], cell[1], cell[2], rise, layers_before,
                    layers_after, before);
            out_add("ok\n");
            break;
        case -1: out_add("err a shape session is open in the editor\n"); break;
        case -3: out_fmt("err no voxel at %d %d %d\n", cell[0], cell[1], cell[2]); break;
        default: out_fmt("err shape refused (%d)\n", rc); break;
    }
}

// #### AMFM #541: the navigator verbs #####################################

// A value of a space-separated KEY=VALUE list; longest match wins.
// PM review #541 (5): a quoted value may contain spaces — the Star Marine
// Trooper art directory does — so `path="C:\some dir\pd.gox"` survives.
static bool opt_get(const char *args, const char *key, char out[], size_t size)
{
    char pat[40];
    const char *p;
    size_t n = 0;

    snprintf(pat, sizeof(pat), "%s=", key);
    p = args;
    while (p) {
        p = strstr(p, pat);
        if (!p) return false;
        // Only a token that starts here: either a fresh argument or the
        // whole string.
        if (p != args && (p[-1] != ' ' && p[-1] != '\t')) {
            p = NULL;
            continue;
        }
        break;
    }
    if (!p) return false;
    p += strlen(pat);
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 1 < size) out[n++] = *p++;
        out[n] = 0;
        return *p == '"';
    }
    while (*p && *p != ' ' && *p != '\t' && n + 1 < size) out[n++] = *p++;
    out[n] = 0;
    return true;
}

// An axis token like "+Y" or "-Z" into a unit-ish vector, or false.
static bool axis_token(const char *in, int out[3])
{
    int sign, axis;

    if (!in || ((in[0] != '+') && (in[0] != '-'))) return false;
    sign = (in[0] == '+') ? 1 : -1;
    axis = (in[1] == 'x' || in[1] == 'X') ? 0 :
           (in[1] == 'y' || in[1] == 'Y') ? 1 :
           (in[1] == 'z' || in[1] == 'Z') ? 2 : -1;
    if (axis < 0 || in[2]) return false;
    out[0] = out[1] = out[2] = 0;
    out[axis] = sign;
    return true;
}

// The document revision a worker last saw: it changes when the image changes,
// so every mutating verb can refuse a stale write deterministically.
static uint32_t doc_rev(void)
{
    return goxel.image ? image_get_key(goxel.image) : 0;
}

static bool rev_check(const char *args, uint32_t *rev_output)
{
    char v[32];
    uint32_t base;
    uint32_t rev = doc_rev();
    if (rev_output) *rev_output = rev;
    if (!args) return true;
    if (!opt_get(args, "base", v, sizeof(v))) return true;
    base = (uint32_t)strtoul(v, NULL, 10);
    if (base == rev) return true;
    out_fmt("err doc changed since base %u (now rev %u): re-read state\n",
            base, rev);
    return false;
}

// PM review #541 (7): the revision contract for worker semantic writes
// (tag, confirm, untag).  A write must carry both the document revision it
// read (base=) and the registry revision (nbase=): the document hash cannot
// see the registry, so without nbase a second worker's tag or confirm would
// be invisible and silently overwritten.  A missing token is an explicit
// error, never silently treated as fresh.  The GUI confirms against the
// current state directly and does not go through this path.
static bool sem_guards(const char *args)
{
    char vb[32], vn[32];

    if (!opt_get(args, "base", vb, sizeof(vb))) {
        out_add("err base=<rev> is required; read it from 'state'\n");
        return false;
    }
    if (!rev_check(args, NULL)) return false;
    if (!opt_get(args, "nbase", vn, sizeof(vn))) {
        out_add("err nbase=<navrev> is required; read it from 'state'\n");
        return false;
    }
    if (strtoull(vn, NULL, 10) != nav_revision()) {
        out_fmt("err registry changed since nbase %s (now %llu): re-read state\n",
                vn, (unsigned long long)nav_revision());
        return false;
    }
    return true;
}

// The exact confirmed cells of the selection, grouped by the layer they sit
// in, plus the shape parameters and computed end faces when relevant: the
// answer to "what am I actually holding", not a bounding box.
static void cmd_selinfo(void)
{
    volume_iterator_t iter;
    uint8_t c[4];
    int p[3], n = 0;
    layer_t *l;
    int hits;
    const slope_mod_t *m;

    if (!goxel.image) {
        out_add("err no image\n");
        return;
    }
    if (!goxel.image->selection_mask) {
        out_add("err no selection\nok\n");
        return;
    }
    out_fmt("selrev %u\n", doc_rev());
    DL_FOREACH(goxel.image->layers, l) {
        hits = 0;
        iter = volume_get_iterator(goxel.image->selection_mask,
                                   VOLUME_ITER_VOXELS |
                                   VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&iter, p)) {
            volume_get_at(goxel.image->selection_mask, &iter, p, c);
            if (!c[3]) continue;
            if (l->volume && volume_get_alpha_at(l->volume, NULL, p))
                hits++;
        }
        if (hits || l == goxel.image->active_layer) {
            out_fmt("obj layer=%d name='%s' cells=%d%s%s\n", l->id, l->name,
                    hits, l == goxel.image->active_layer ? " ACTIVE" : "",
                    l->slope ? " shape=yes" : "");
        }
    }
    // The shape layers of the selection: static params and the two ends.
    DL_FOREACH(goxel.image->layers, l) {
        if (!l->slope) continue;
        m = l->slope;
        if (!l->volume) continue;
        {
            nav_record_t probe;
            float pa[3], pb[3], da[3], db[3];
            int a, hits = 0;
            volume_iterator_t siter;
            uint8_t sc[4];
            int sp[3];

            memset(&probe, 0, sizeof(probe));
            probe.layer_id = l->id;
            probe.at = NAV_AT_SHAPE_END;
            // PM review #541 (1): the two candidate faces come from the same
            // authority that places the marker, negative axes included.
            nav_shape_end(&probe, +1, pa, da);
            nav_shape_end(&probe, -1, pb, db);
            a = (m->dir == SLOPE_DIR_PX || m->dir == SLOPE_DIR_NX) ? 0 : 1;
            // PM review #541 (8): say which shape is the selected target and
            // which is only document inventory.
            if (goxel.image->selection_mask) {
                siter = volume_get_iterator(goxel.image->selection_mask,
                                            VOLUME_ITER_VOXELS |
                                            VOLUME_ITER_SKIP_EMPTY);
                while (volume_iter(&siter, sp)) {
                    volume_get_at(goxel.image->selection_mask, &siter, sp, sc);
                    if (sc[3] && volume_get_alpha_at(l->volume, NULL, sp))
                        hits++;
                }
            }
            out_fmt("shape layer=%d name='%s' kind=%s axis=%c run=%+d closed=%d "
                    "rise=%.3f aabb=%d,%d,%d..%d,%d,%d rev=%u stale=%d sel=%d%s\n",
                    l->id, l->name, slope_kind_name(m->kind), 'x' + a,
                    (m->dir == SLOPE_DIR_PX || m->dir == SLOPE_DIR_PY) ? +1 : -1,
                    m->closed, m->rise,
                    m->aabb[0][0], m->aabb[0][1], m->aabb[0][2],
                    m->aabb[1][0], m->aabb[1][1], m->aabb[1][2],
                    m->revision, m->stale,
                    (l == goxel.image->active_layer || hits > 0) ? 1 : 0,
                    l == goxel.image->active_layer ? " ACTIVE" : "");
            out_fmt("ends layer=%d a %0.2f %0.2f %0.2f b %0.2f %0.2f %0.2f\n",
                    l->id, pa[0], pa[1], pa[2], pb[0], pb[1], pb[2]);
        }
    }
    // The exact cells, bounded: one line each; a selection on the ship is
    // small but a box can be huge.
    {
        int listed = 0;
        iter = volume_get_iterator(goxel.image->selection_mask,
                                   VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&iter, p)) {
            volume_get_at(goxel.image->selection_mask, &iter, p, c);
            if (!c[3]) continue;
            n++;
        }
        if (n > 2048) out_fmt("cells %d (first 2048)\n", n);
        else out_fmt("cells %d\n", n);
        iter = volume_get_iterator(goxel.image->selection_mask,
                                   VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&iter, p)) {
            volume_get_at(goxel.image->selection_mask, &iter, p, c);
            if (!c[3]) continue;
            if (++listed > 2048) break;
            out_fmt("cell %d %d %d\n", p[0], p[1], p[2]);
        }
    }
    out_add("ok\n");
}

static void cmd_tag(const char *args)
{
    char id[64] = "", kindname[24] = "", end[8] = "", group[64] = "";
    char label[96] = "", token_err[128] = "";
    int kind = -1, token[3] = {0, 0, 0}, axis_k;
    nav_record_t *r;
    layer_t *l;

    if (!sem_guards(args)) return;
    if (!opt_get(args, "kind", kindname, sizeof(kindname))) {
        out_add("err usage: tag kind=muzzle|exhaust|pivot|hinge|rcs end=+Y "
                "id=<name> [source=layer] [group=..] [label=..]\n");
        return;
    }
    if (!opt_get(args, "id", id, sizeof(id)))
        snprintf(id, sizeof(id), "%s", kindname);
    for (kind = 0; kind < NAV_KIND_COUNT; kind++)
        if (!strcmp(kindname, nav_kind_name(kind))) break;
    if (kind >= NAV_KIND_COUNT) {
        out_fmt("err unknown kind '%s'\n", kindname);
        return;
    }

    // The definition must come from the visible selection: a layer named by
    // the worker is only ever a refinement of the selected object.
    {
        char want[64];
        if (opt_get(args, "source", want, sizeof(want))) {
            bool ok = goxel.image && goxel.image->active_layer &&
                      !strcmp(want, goxel.image->active_layer->name);
            if (!ok) {
                out_fmt("err source '%s' is not the active selected layer "
                        "'%s'\n", want,
                        goxel.image && goxel.image->active_layer
                            ? goxel.image->active_layer->name : "-");
                return;
            }
        }
    }

    l = goxel.image ? goxel.image->active_layer : NULL;
    if (!l) { out_add("err no image\n"); return; }
    if (opt_get(args, "end", end, sizeof(end))) {
        if (!axis_token(end, token)) {
            out_fmt("err bad end token '%s' (use +X/-X/+Y/-Y/+Z/-Z)\n", end);
            return;
        }
    } else {
        // An end has to be named; the worker reads both candidate ends from
        // selinfo's `ends` lines and picks the right one, deterministically.
        out_add("err an end token is required; read the shape ends from "
                "selinfo and pick one\n");
        return;
    }
    opt_get(args, "group", group, sizeof(group));
    opt_get(args, "label", label, sizeof(label));

    axis_k = l->slope ? NAV_AT_SHAPE_END : NAV_AT_CELL;
    // rev9: cell=x,y,z anchors a cell record at an exact cell the Owner
    // cannot box-select (e.g. a hinge pivot buried inside the body).
    {
        char celltok[32];
        int celloc[3];
        const int *override = NULL;
        if (opt_get(args, "cell", celltok, sizeof(celltok))) {
            if (axis_k == NAV_AT_SHAPE_END) {
                out_add("err cell= is for cell anchors; the active layer is "
                        "a shape, use end=\n");
                return;
            }
            if (sscanf(celltok, "%d,%d,%d", &celloc[0], &celloc[1],
                       &celloc[2]) != 3) {
                out_fmt("err bad cell '%s' (use cell=x,y,z)\n", celltok);
                return;
            }
            override = celloc;
        }
        r = nav_tag(id, kind, axis_k, token, override, group, label, token_err,
                    sizeof(token_err));
    }
    if (!r) { out_fmt("err %s\n", token_err); return; }

    // Report both geometric ends, and what the token resolved to, so the
    // owner's question "+Y?" is answered for both eyes at once.  PM review
    // #541 (1): both faces come from nav_shape_end — the exact authority
    // that places the marker — so the reported position and direction are
    // the real ones on any authored axis, negative included.
    if (r->at == NAV_AT_SHAPE_END) {
        float pp[3], pd[3], po[3], od[3];
        const slope_mod_t *m = l->slope;
        int a = (m->dir == SLOPE_DIR_PX || m->dir == SLOPE_DIR_NX) ? 0 : 1;
        nav_shape_end(r, r->shape_sign, pp, pd);
        nav_shape_end(r, -r->shape_sign, po, od);
        out_fmt("ends axis=%c sign=%+d dir %0.2f %0.2f %0.2f picked %0.2f %0.2f %0.2f\n",
                'x' + a, r->shape_sign, pd[0], pd[1], pd[2],
                pp[0], pp[1], pp[2]);
        out_fmt("ends axis=%c sign=%+d dir %0.2f %0.2f %0.2f other %0.2f %0.2f %0.2f\n",
                'x' + a, -r->shape_sign, od[0], od[1], od[2],
                po[0], po[1], po[2]);
        {
            float u[3];
            nav_dir_vec(m->dir, u);
            out_fmt("axisdir %0.2f %0.2f %0.2f\n", u[0], u[1], u[2]);
        }
    }
    out_fmt("tag id=%s kind=%s at=%s layer=%d name='%s' group=%s "
            "status=draft\n", r->id, nav_kind_name(r->kind),
            r->at == NAV_AT_SHAPE_END ? "shape_end" : "cell", r->layer_id,
            l->name, r->group);
    if (r->at == NAV_AT_CELL) {
        out_fmt("anchored cell %d %d %d dir %d %d %d\n", r->cell[0],
                r->cell[1], r->cell[2], (int)r->dir[0], (int)r->dir[1],
                (int)r->dir[2]);
    }
    out_add("ok\n");
}

static void cmd_tags(void)
{
    nav_record_t *r;
    layer_t *l;
    float pos[3], dir[3];
    int n = 0;

    DL_FOREACH(nav_records(), r) {
        l = nav_layer(r->layer_id);
        pos[0] = pos[1] = pos[2] = 0;
        dir[0] = dir[1] = dir[2] = 0;
        {
            float pp[3], dd[3];
            if (nav_ring(r, pp, dd)) {
                vec3_copy(pp, pos);
                vec3_copy(dd, dir);
            }
        }
        out_fmt("tag id=%s kind=%s at=%s layer=%d lname='%s' group=%s "
                "status=%s pos %0.2f %0.2f %0.2f dir %0.2f %0.2f %0.2f\n",
                r->id, nav_kind_name(r->kind),
                r->at == NAV_AT_SHAPE_END ? "shape_end" : "cell",
                r->layer_id, l ? l->name : "(gone)", r->group,
                nav_status_name(r->status), pos[0], pos[1], pos[2],
                dir[0], dir[1], dir[2]);
        if (r->status != NAV_STATUS_CONFIRMED && r->reason[0])
            out_fmt("review id=%s %s\n", r->id, r->reason);
        n++;
    }
    out_fmt("tags %d\nok\n", n);
}

// PM review #541 (7): confirm and untag are semantic writes too.  The id is
// read as a token (a bare first word still works), and both revision tokens
// are enforced — the old code handed the whole argument string to nav_find,
// so appending base= would have broken the lookup instead of guarding it.
static void cmd_tag_alive(const char *args, bool confirm)
{
    nav_record_t *r;
    char err[192];
    char id[64] = "";
    const char *p = args ? args : "";

    if (!opt_get(p, "id", id, sizeof(id))) {
        if (strncmp(p, "base=", 5) && strncmp(p, "nbase=", 6))
            sscanf(p, "%63s", id);
    }
    if (!id[0]) {
        out_add("err usage: confirm id=<id> base=<rev> nbase=<navrev>\n");
        return;
    }
    if (!sem_guards(p)) return;
    r = nav_find(id);
    if (!r) { out_fmt("err no record '%s'\n", id); return; }
    if (confirm) {
        if (!nav_confirm(r, err, sizeof(err))) {
            out_fmt("err %s\n", err);
            return;
        }
        out_fmt("tag id=%s status=confirmed\nok\n", r->id);
    } else {
        nav_remove(r);
        out_fmt("removed %s\nok\n", id);
    }
}

static void cmd_save_nav(const char *args)
{
    char path[512] = "";
    char err[256];
    opt_get(args, "path", path, sizeof(path));
    if (!rev_check(args, NULL)) return;
    if (nav_save(*path ? path : NULL, err, sizeof(err))) {
        out_fmt("err navsave: %s\n", err);
        return;
    }
    out_add("saved\nok\n");
}

static void cmd_navadopt(const char *args)
{
    char path[512] = "";

    if (!goxel.image) { out_add("err no image\n"); return; }
    opt_get(args, "path", path, sizeof(path));
    if (!path[0]) {
        out_add("err navadopt needs the gox path of the reopened ship\n");
        return;
    }
    {
        int n = nav_adopt(path);
        if (n < 0) { out_add("err navadopt failed\n"); return; }
        out_fmt("adopted %d\nok\n", n);
    }
}

// AMFM #541 rev8: File->Open through the bridge.  Same entry point the menu
// item uses (goxel_open_file), so the document keeps natural open semantics
// (path set, title set, navigator re-settled).  Refuses rather than silently
// raising the unsaved-changes modal, which would block the bridge.
static void cmd_open(const char *args)
{
    char path[512] = "";

    if (!goxel.image) { out_add("err no image\n"); return; }
    if (!opt_get(args, "path", path, sizeof(path)) || !path[0]) {
        out_add("err usage: open path=<file.gox>\n");
        return;
    }
    if (image_get_key(goxel.image) != goxel.image->saved_key) {
        out_add("err unsaved changes; save or reset first\n");
        return;
    }
    {
        FILE *probe = fopen(path, "rb");
        if (!probe) { out_fmt("err cannot read '%s'\n", path); return; }
        fclose(probe);
    }
    goxel_open_file(path);
    snprintf(s.doc, sizeof(s.doc), "%s", path);
    out_fmt("opened %s\nok\n", path);
}

// Worker pointer overlay (#541 A2, PM correction 2026-11-08): the Worker
// points at cells without touching the Owner's native selection mask or the
// geometry.  Attributed magenta cubes, session-only, cleared on open.
static void cmd_pointer(const char *args)
{
    char sub[16] = "";
    int x, y, z, i, p[3];

    if (sscanf(args, "%15s", sub) != 1 || !strcmp(sub, "list")) {
        out_fmt("ptr count=%d\n", nav_pointer_count());
        for (i = 0; nav_pointer_get(i, p); i++)
            out_fmt("ptr %d %d %d\n", p[0], p[1], p[2]);
        out_add("ok\n");
        return;
    }
    if (!strcmp(sub, "clear")) {
        nav_pointer_clear();
        out_add("ptr cleared\nok\n");
        return;
    }
    if ((!strcmp(sub, "add") || !strcmp(sub, "del")) &&
        sscanf(args + strlen(sub), " %d %d %d", &x, &y, &z) == 3) {
        if (!strcmp(sub, "add")) {
            nav_pointer_add(x, y, z);
            out_fmt("ptr add %d %d %d count=%d\n", x, y, z,
                    nav_pointer_count());
        } else {
            nav_pointer_del(x, y, z);
            out_fmt("ptr del %d %d %d count=%d\n", x, y, z,
                    nav_pointer_count());
        }
        out_add("ok\n");
        return;
    }
    out_add("err usage: pointer [list] | add x y z | del x y z | clear\n");
}

static void cmd_pickmode(const char *args)
{
    if (!strcmp(args, "on")) tool_selection_set_pick(1);
    else if (!strcmp(args, "off")) tool_selection_set_pick(0);
    else if (*args && strcmp(args, "get")) {
        out_add("err usage: pickmode on|off|get\n");
        return;
    }
    out_fmt("pickmode %d\n", tool_selection_get_pick());
    out_add("ok\n");
}

static void handle_line(const char *line)
{
    char cmd[32] = "";
    const char *args;

    while (*line == ' ') line++;
    if (!*line) return;
    if (sscanf(line, "%31s", cmd) != 1) return;
    args = line + strlen(cmd);
    while (*args == ' ') args++;

    if (!strcmp(cmd, "ping")) {
        out_fmt("pong pid=%d port=%d frame=%d\n", cur_pid(), s.port,
                goxel.frame_count);
        out_add("ok\n");
    } else if (!strcmp(cmd, "state")) {
        cmd_state();
    } else if (!strcmp(cmd, "set")) {
        cmd_set(args, 0);
    } else if (!strcmp(cmd, "del")) {
        cmd_set(args, 1);
    } else if (!strcmp(cmd, "shape")) {
        cmd_shape(args);
    } else if (!strcmp(cmd, "undo")) {
        if (!goxel.image) { out_add("err no image\n"); return; }
        image_undo(goxel.image);
        out_add("undo applied\nok\n");
    } else if (!strcmp(cmd, "redo")) {
        if (!goxel.image) { out_add("err no image\n"); return; }
        image_redo(goxel.image);
        out_add("redo applied\nok\n");
    } else if (!strcmp(cmd, "selinfo")) {
        cmd_selinfo();
    } else if (!strcmp(cmd, "tag")) {
        cmd_tag(args);
    } else if (!strcmp(cmd, "tags")) {
        cmd_tags();
    } else if (!strcmp(cmd, "confirm")) {
        cmd_tag_alive(args, true);
    } else if (!strcmp(cmd, "untag")) {
        cmd_tag_alive(args, false);
    } else if (!strcmp(cmd, "navsave")) {
        cmd_save_nav(args);
    } else if (!strcmp(cmd, "navadopt")) {
        cmd_navadopt(args);
    } else if (!strcmp(cmd, "open")) {
        cmd_open(args);
    } else if (!strcmp(cmd, "pointer")) {
        cmd_pointer(args);
    } else if (!strcmp(cmd, "pickmode")) {
        cmd_pickmode(args);
    } else if (!strcmp(cmd, "help")) {
        out_add("commands: ping | state | set x y z [r g b] | del x y z | "
                "shape x y z [rise] [name] | undo | redo\n"
                "#541 navigator: selinfo | tag kind=muzzle|exhaust|pivot|hinge|rcs "
                "end=+Y|-.. id=<id> [source=..] [group=..] [label=..] [cell=x,y,z] "
                "base=rev nbase=navrev | tags | confirm id=<id> base=rev "
                "nbase=navrev | untag id=<id> base=rev nbase=navrev | "
                "navsave [path=\"..\"|path=..] [base=rev] | navadopt path=.. | "
                "open path=.. | pointer [list]|add x y z|del x y z|clear | "
                "pickmode on|off|get | help\n"
                "geometry verbs accept base=<rev>; the semantic verbs (tag,\n"
                "confirm, untag) require base=<rev> AND nbase=<navrev>, both\n"
                "read from 'state' (rev / navrev lines); a stale or missing\n"
                "token is refused.  A quoted path= value may contain spaces.\n"
                "ok\n");
    } else {
        out_fmt("err unknown command '%s'\n", cmd);
    }
}

static void flush_out(void)
{
    while (s.out_pos < s.out_len) {
        int n = send(s.client_fd, s.out + s.out_pos,
                     (int)(s.out_len - s.out_pos), 0);
        if (n > 0) {
            s.out_pos += n;
            continue;
        }
        if (n < 0 && sock_wouldblock(sock_lasterr())) return;  // try next frame
        drop_client("send failed");
        return;
    }
    s.out_len = s.out_pos = 0;
}

void lab_bridge_init(int port, const char *doc_path)
{
    struct sockaddr_in addr;
    int on = 1;

    memset(&s, 0, sizeof(s));
    s.listen_fd = s.client_fd = BADSOCK;
    if (port <= 0) return;
    s.enabled = 1;
    s.port = port;
    snprintf(s.doc, sizeof(s.doc), "%s", doc_path ? doc_path : "");

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            printf("lab bridge: WSAStartup failed\n");
            fflush(stdout);
            s.enabled = 0;
            return;
        }
    }
#endif

    s.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s.listen_fd == BADSOCK) {
        printf("lab bridge: socket() failed\n");
        fflush(stdout);
        s.enabled = 0;
        return;
    }
    setsockopt(s.listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on,
               sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(s.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(s.listen_fd, 1) < 0) {
        printf("lab bridge: cannot listen on 127.0.0.1:%d (err %d)\n", port,
               sock_lasterr());
        fflush(stdout);
        sock_close(s.listen_fd);
        s.listen_fd = BADSOCK;
        s.enabled = 0;
        return;
    }
    set_nonblock(s.listen_fd);
    printf("lab bridge: listening on 127.0.0.1:%d pid=%d doc=%s\n", port,
           cur_pid(), s.doc[0] ? s.doc : "-");
    fflush(stdout);
}

// AMFM #541: the headless self-test drives the very line the client would
// send, through the very parser, and reads the reply back.  No sockets in a
// test; the socket path stays covered by the scripted editor sessions.
void lab_bridge_test_call(const char *line, char *out, size_t out_size)
{
    out[0] = 0;
    s.out_len = 0;
    s.out_pos = 0;
    handle_line(line);
    s.out[s.out_len] = 0;
    snprintf(out, out_size, "%s", s.out);
    s.out_len = s.out_pos = 0;
}

void lab_bridge_iter(void)
{
    char buf[1024];
    size_t start;
    int n;

    if (!s.enabled || s.listen_fd == BADSOCK) return;

    flush_out();
    if (s.client_fd == BADSOCK) {
        sock_t c = accept(s.listen_fd, NULL, NULL);
        if (c == BADSOCK) return;           // nothing waiting
        set_nonblock(c);
        s.client_fd = c;
        s.in_len = 0;
        s.out_len = s.out_pos = 0;
        printf("lab bridge: client connected\n");
        fflush(stdout);
        return;
    }

    n = recv(s.client_fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        if (s.in_len + n >= sizeof(s.in)) {
            drop_client("input too large");
            return;
        }
        memcpy(s.in + s.in_len, buf, n);
        s.in_len += n;
        s.in[s.in_len] = '\0';
        // Dispatch every complete line, keep the partial tail for later.
        start = 0;
        for (;;) {
            char *nl = memchr(s.in + start, '\n', s.in_len - start);
            if (!nl) break;
            *nl = '\0';
            handle_line(s.in + start);
            start += (size_t)(nl - (s.in + start)) + 1;
        }
        if (start > 0) {
            s.in_len -= start;
            memmove(s.in, s.in + start, s.in_len);
            s.in[s.in_len] = '\0';
        }
        flush_out();
    } else if (n == 0) {
        drop_client("closed by client");
    } else if (!sock_wouldblock(sock_lasterr())) {
        drop_client("recv error");
    }
}
