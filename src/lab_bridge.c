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
    out_fmt("painter_mode %d\n", goxel.painter.mode);
    if (img->active_layer)
        out_fmt("active '%s' shape=%d cells=%d\n", img->active_layer->name,
                img->active_layer->slope ? 1 : 0,
                count_cells(img->active_layer->volume));
    if (img->selection_mask && volume_get_bbox(img->selection_mask, aabb, true))
        out_fmt("selection %d %d %d %d %d %d\n", aabb[0][0], aabb[0][1],
                aabb[0][2], aabb[1][0], aabb[1][1], aabb[1][2]);
    else
        out_add("selection none\n");
    DL_FOREACH(img->layers, l) {
        out_fmt("layer[%d] '%s' cells=%d shape=%d%s\n", i++, l->name,
                count_cells(l->volume), l->slope ? 1 : 0,
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
    int cell[3];
    int before, layers_before = 0, layers_after = 0;
    float rise = 0.45f;
    char name[64] = "";
    const char *err;
    layer_t *l;
    int rc;

    if (sscanf(args, "%d %d %d %f %63s", &cell[0], &cell[1], &cell[2],
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
    } else if (!strcmp(cmd, "help")) {
        out_add("commands: ping state set x y z [r g b] | del x y z | "
                "shape x y z [rise] [name] | undo | redo | help\nok\n");
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
