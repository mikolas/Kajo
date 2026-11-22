/* comp_screenshot_ui.c — interactive selection overlay UI, off-thread libpng
 * encoder & file writer, and non-blocking wl_seat image/png clipboard pipe.
 */
#include "comp_screenshot_private.h"

#include "comp_log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>

/* Premultiplied colors */
static const float k_dim_rgba[4]       = { 0.0f, 0.0f, 0.0f, 0.55f };
static const float k_default_accent[4] = { 1.0f, 0.2f, 0.4f, 1.0f };
static const float k_badge_fg[4]       = { 1.0f, 1.0f, 1.0f, 1.0f };
static const float k_badge_bg[4]       = { 0.066667f, 0.066667f, 0.066667f, 0.92f };
static const float k_badge_border[4]   = { 0.168627f, 0.168627f, 0.168627f, 1.0f };

typedef struct {
    struct CompScreenshot *ss;
    uint8_t               *argb_pixels;
    int                    width;
    int                    height;
    char                  *path_template;
    char                  *home_dir;
    bool                   copy_to_clipboard;
    bool                   show_notification;
} ScreenshotWorkerArg;

typedef struct {
    struct wlr_data_source  base;
    struct wl_event_loop   *loop;
    uint8_t                *png_bytes;
    size_t                  png_len;
} CompPngDataSource;

typedef struct {
    struct wl_event_source *ev_src;
    int                     fd;
    uint8_t                *bytes;
    size_t                  len;
    size_t                  off;
} PngPipeWriter;

/* ── Non-blocking wl_seat clipboard data source for image/png ── */

static int
png_pipe_writable(int fd, uint32_t mask, void *data)
{
    PngPipeWriter *pw = data;
    if ((mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) || pw->off >= pw->len)
        goto done;

    while (pw->off < pw->len) {
        ssize_t n = write(fd, pw->bytes + pw->off, pw->len - pw->off);
        if (n > 0) {
            pw->off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }

    if (pw->off < pw->len)
        return 0;

done:
    if (pw->ev_src)
        wl_event_source_remove(pw->ev_src);
    close(pw->fd);
    free(pw->bytes);
    free(pw);
    return 0;
}

static void
png_data_source_send(struct wlr_data_source *wlr_src, const char *mime_type, int32_t fd)
{
    CompPngDataSource *src = wl_container_of(wlr_src, src, base);
    if (!src->png_bytes || src->png_len == 0 || !src->loop ||
        (strcmp(mime_type, "image/png") != 0 && strcmp(mime_type, "application/octet-stream") != 0)) {
        close(fd);
        return;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    PngPipeWriter *pw = calloc(1, sizeof(*pw));
    if (!pw) {
        close(fd);
        return;
    }
    pw->bytes = malloc(src->png_len);
    if (!pw->bytes) {
        free(pw);
        close(fd);
        return;
    }
    memcpy(pw->bytes, src->png_bytes, src->png_len);
    pw->len = src->png_len;
    pw->off = 0;
    pw->fd  = fd;

    pw->ev_src = wl_event_loop_add_fd(src->loop, fd, WL_EVENT_WRITABLE, png_pipe_writable, pw);
    if (!pw->ev_src) {
        free(pw->bytes);
        free(pw);
        close(fd);
    }
}

static void
png_data_source_destroy(struct wlr_data_source *wlr_src)
{
    CompPngDataSource *src = wl_container_of(wlr_src, src, base);
    free(src->png_bytes);
    free(src);
}

static const struct wlr_data_source_impl k_png_data_source_impl = {
    .send    = png_data_source_send,
    .destroy = png_data_source_destroy,
};

static bool
set_clipboard_png(CompServer *s, const uint8_t *png_bytes, size_t png_len)
{
    if (!s || !s->seat || !s->display || !png_bytes || png_len == 0)
        return false;

    CompPngDataSource *src = calloc(1, sizeof(*src));
    if (!src)
        return false;

    wlr_data_source_init(&src->base, &k_png_data_source_impl);
    src->loop      = s->loop;
    src->png_bytes = malloc(png_len);
    if (!src->png_bytes) {
        free(src);
        return false;
    }
    memcpy(src->png_bytes, png_bytes, png_len);
    src->png_len = png_len;

    char **mime = wl_array_add(&src->base.mime_types, sizeof(char *));
    if (mime)
        *mime = strdup("image/png");

    wlr_seat_set_selection(s->seat, &src->base, wl_display_next_serial(s->display));
    return true;
}

/* ── Off-thread PNG encoding and disk write ── */

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    bool     oom;
} PngMemBuf;

static void
png_mem_write_cb(png_structp png_ptr, png_bytep data, png_size_t length)
{
    PngMemBuf *m = (PngMemBuf *)png_get_io_ptr(png_ptr);
    if (!m || m->oom)
        return;
    if (m->len + length > m->cap) {
        size_t new_cap = m->cap ? m->cap * 2u : 65536u;
        while (new_cap < m->len + length)
            new_cap *= 2u;
        uint8_t *nb = realloc(m->buf, new_cap);
        if (!nb) {
            m->oom = true;
            return;
        }
        m->buf = nb;
        m->cap = new_cap;
    }
    memcpy(m->buf + m->len, data, length);
    m->len += length;
}

static void
png_mem_flush_cb(png_structp png_ptr)
{
    (void)png_ptr;
}

static bool
encode_argb_to_png_mem(const uint8_t *argb, int width, int height,
                       uint8_t **out_bytes, size_t *out_len)
{
    *out_bytes = NULL;
    *out_len   = 0;
    if (!argb || width <= 0 || height <= 0)
        return false;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr)
        return false;

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        return false;
    }

    PngMemBuf mem = {0};
    uint8_t *row_rgba = malloc((size_t)width * 4u);
    if (!row_rgba) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        free(row_rgba);
        free(mem.buf);
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return false;
    }

    png_set_write_fn(png_ptr, &mem, png_mem_write_cb, png_mem_flush_cb);
    png_set_IHDR(png_ptr, info_ptr, (png_uint_32)width, (png_uint_32)height,
                 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    for (int y = 0; y < height; y++) {
        const uint32_t *src = (const uint32_t *)(const void *)(argb + (size_t)y * (size_t)width * 4u);
        for (int x = 0; x < width; x++) {
            uint32_t px = src[x];
            uint8_t a = (uint8_t)((px >> 24) & 0xffu);
            uint8_t r = (uint8_t)((px >> 16) & 0xffu);
            uint8_t g = (uint8_t)((px >> 8) & 0xffu);
            uint8_t b = (uint8_t)(px & 0xffu);
            if (a > 0 && a < 255) {
                unsigned ur = ((unsigned)r * 255u + (unsigned)a / 2u) / (unsigned)a;
                unsigned ug = ((unsigned)g * 255u + (unsigned)a / 2u) / (unsigned)a;
                unsigned ub = ((unsigned)b * 255u + (unsigned)a / 2u) / (unsigned)a;
                r = (uint8_t)(ur > 255u ? 255u : ur);
                g = (uint8_t)(ug > 255u ? 255u : ug);
                b = (uint8_t)(ub > 255u ? 255u : ub);
            }
            row_rgba[(size_t)x * 4u + 0u] = r;
            row_rgba[(size_t)x * 4u + 1u] = g;
            row_rgba[(size_t)x * 4u + 2u] = b;
            row_rgba[(size_t)x * 4u + 3u] = a;
        }
        png_write_row(png_ptr, row_rgba);
    }

    png_write_end(png_ptr, NULL);
    free(row_rgba);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    if (mem.oom || !mem.buf || mem.len == 0) {
        free(mem.buf);
        return false;
    }

    *out_bytes = mem.buf;
    *out_len   = mem.len;
    return true;
}

static void
mkdir_parents(const char *file_path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", file_path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return;
    *slash = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0755);
            *p = '/';
        }
    }
    (void)mkdir(tmp, 0755);
}

static char *
expand_screenshot_path(const char *tmpl, const char *home_dir)
{
    const char *use_tmpl = (tmpl && *tmpl)
                               ? tmpl
                               : "~/Pictures/Screenshots/Screenshot from %Y-%m-%d %H-%M-%S.png";
    char expanded_tilde[PATH_MAX];
    if (use_tmpl[0] == '~' && (use_tmpl[1] == '/' || use_tmpl[1] == '\0')) {
        const char *home = (home_dir && *home_dir) ? home_dir : "/tmp";
        snprintf(expanded_tilde, sizeof(expanded_tilde), "%s%s", home, use_tmpl + 1);
    } else {
        snprintf(expanded_tilde, sizeof(expanded_tilde), "%s", use_tmpl);
    }

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char formatted[PATH_MAX];
    size_t out_pos = 0;
    for (const char *in = expanded_tilde; *in && out_pos + 1 < sizeof(formatted); in++) {
        if (*in != '%') {
            formatted[out_pos++] = *in;
            continue;
        }
        in++;
        if (!*in)
            break;
        char tok[64] = {0};
        switch (*in) {
        case 'Y': (void)strftime(tok, sizeof(tok), "%Y", &tm_now); break;
        case 'y': snprintf(tok, sizeof(tok), "%02d", (tm_now.tm_year + 1900) % 100); break;
        case 'm': (void)strftime(tok, sizeof(tok), "%m", &tm_now); break;
        case 'd': (void)strftime(tok, sizeof(tok), "%d", &tm_now); break;
        case 'H': (void)strftime(tok, sizeof(tok), "%H", &tm_now); break;
        case 'M': (void)strftime(tok, sizeof(tok), "%M", &tm_now); break;
        case 'S': (void)strftime(tok, sizeof(tok), "%S", &tm_now); break;
        case 'F': (void)strftime(tok, sizeof(tok), "%F", &tm_now); break;
        case 'T': (void)strftime(tok, sizeof(tok), "%T", &tm_now); break;
        case 's': (void)strftime(tok, sizeof(tok), "%s", &tm_now); break;
        case '%': tok[0] = '%'; tok[1] = '\0'; break;
        default:
            tok[0] = '%';
            tok[1] = *in;
            tok[2] = '\0';
            break;
        }
        size_t tlen = strlen(tok);
        if (out_pos + tlen >= sizeof(formatted))
            tlen = sizeof(formatted) - 1 - out_pos;
        memcpy(formatted + out_pos, tok, tlen);
        out_pos += tlen;
    }
    formatted[out_pos] = '\0';

    /* Ensure unique path if multiple screenshots occur in the same second */
    if (access(formatted, F_OK) != 0)
        return strdup(formatted);

    const char *dot = strrchr(formatted, '.');
    size_t stem_len = dot ? (size_t)(dot - formatted) : strlen(formatted);
    const char *ext = dot ? dot : "";
    for (int suffix = 1; suffix < 1000; suffix++) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%.*s-%d%s",
                 (int)stem_len, formatted, suffix, ext);
        if (access(candidate, F_OK) != 0)
            return strdup(candidate);
    }
    return strdup(formatted);
}

static bool
write_bytes_to_file(const char *path, const uint8_t *bytes, size_t len)
{
    mkdir_parents(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;

    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, bytes + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

static void *
screenshot_worker_main(void *arg)
{
    ScreenshotWorkerArg *wa = arg;
    CompScreenshot *ss = wa->ss;

    uint8_t *png_bytes = NULL;
    size_t png_len = 0;
    bool ok = encode_argb_to_png_mem(wa->argb_pixels, wa->width, wa->height, &png_bytes, &png_len);
    free(wa->argb_pixels);

    char *final_path = NULL;
    if (ok) {
        final_path = expand_screenshot_path(wa->path_template, wa->home_dir);
        if (!final_path || !write_bytes_to_file(final_path, png_bytes, png_len))
            ok = false;
    }
    free(wa->path_template);
    free(wa->home_dir);

    if (ss) {
        ScreenshotJobResult *res = calloc(1, sizeof(*res));
        pthread_mutex_lock(&ss->res_lock);
        if (!ss->shutting_down && res) {
            res->path              = final_path;
            res->png_bytes         = png_bytes;
            res->png_len           = png_len;
            res->copy_to_clipboard = wa->copy_to_clipboard;
            res->show_notification = wa->show_notification;
            res->ok                = ok;

            if (ss->res_tail)
                ss->res_tail->next = res;
            else
                ss->res_head = res;
            ss->res_tail = res;
            final_path = NULL;
            png_bytes  = NULL;
            res        = NULL;

            if (ss->notify_fd >= 0) {
                uint64_t one = 1;
                ssize_t wr = write(ss->notify_fd, &one, sizeof(one));
                (void)wr;
            }
        }
        if (ss->pending_workers > 0 && --ss->pending_workers == 0)
            pthread_cond_broadcast(&ss->workers_done);
        pthread_mutex_unlock(&ss->res_lock);
        free(res);
    }

    free(final_path);
    free(png_bytes);
    free(wa);
    return NULL;
}

void
comp_screenshot_dispatch_worker(CompServer *s, uint8_t *owned_argb, int width, int height)
{
    if (!s || !s->screenshot || !owned_argb || width <= 0 || height <= 0) {
        free(owned_argb);
        return;
    }
    CompScreenshot *ss = s->screenshot;

    ScreenshotWorkerArg *wa = calloc(1, sizeof(*wa));
    if (!wa) {
        free(owned_argb);
        return;
    }
    const char *env_home = getenv("HOME");
    wa->ss                = ss;
    wa->argb_pixels       = owned_argb;
    wa->width             = width;
    wa->height            = height;
    wa->path_template     = strdup((s->config && s->config->screenshot_path)
                                       ? s->config->screenshot_path
                                       : "~/Pictures/Screenshots/Screenshot from %Y-%m-%d %H-%M-%S.png");
    wa->home_dir          = strdup((env_home && *env_home) ? env_home : "/tmp");
    wa->copy_to_clipboard = s->config ? s->config->screenshot_copy : true;
    wa->show_notification = s->config ? s->config->screenshot_notify : true;

    pthread_mutex_lock(&ss->res_lock);
    if (ss->shutting_down) {
        pthread_mutex_unlock(&ss->res_lock);
        free(wa->path_template);
        free(wa->home_dir);
        free(wa->argb_pixels);
        free(wa);
        return;
    }
    ss->pending_workers++;
    pthread_mutex_unlock(&ss->res_lock);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, screenshot_worker_main, wa) != 0) {
        /* Fallback synchronous execution if thread creation fails */
        pthread_attr_destroy(&attr);
        screenshot_worker_main(wa);
        return;
    }
    pthread_attr_destroy(&attr);
}

int
comp_screenshot_handle_worker_notify_fd(int fd, uint32_t mask, void *data)
{
    (void)mask;
    CompScreenshot *ss = data;
    uint64_t val = 0;
    while (read(fd, &val, sizeof(val)) > 0) {
        /* drain eventfd */
    }

    pthread_mutex_lock(&ss->res_lock);
    ScreenshotJobResult *list = ss->res_head;
    ss->res_head = NULL;
    ss->res_tail = NULL;
    pthread_mutex_unlock(&ss->res_lock);

    while (list) {
        ScreenshotJobResult *next = list->next;
        if (list->ok && list->path) {
            bool copied = false;
            if (list->copy_to_clipboard && list->png_bytes && list->png_len > 0)
                copied = set_clipboard_png(ss->server, list->png_bytes, list->png_len);
            wlr_log(WLR_INFO, "screenshot saved to %s (copied=%d)", list->path, (int)copied);
            if (list->show_notification)
                comp_ipc_emit_screenshot_saved(ss->server, list->path, copied);
        } else {
            wlr_log(WLR_ERROR, "failed to save screenshot");
        }
        free(list->path);
        free(list->png_bytes);
        free(list);
        list = next;
    }
    return 0;
}

/* ── Interactive Area Selection UI (spec §4.2.1) ── */

void
comp_screenshot_update_selection_scene(CompScreenshot *ss)
{
    if (!ss || !ss->active || !ss->tree)
        return;

    comp_screenshot_clamp_selection(ss);

    int ow = ss->out_box.width;
    int oh = ss->out_box.height;
    int sx = ss->sel_x;
    int sy = ss->sel_y;
    int sw = ss->sel_w;
    int sh = ss->sel_h;

    /* 4 dimming rects outside the selection rectangle */
    wlr_scene_node_set_position(&ss->dim[0]->node, 0, 0);
    wlr_scene_rect_set_size(ss->dim[0], ow, sy > 0 ? sy : 0);
    wlr_scene_node_set_enabled(&ss->dim[0]->node, sy > 0);

    int bot_y = sy + sh;
    int bot_h = oh - bot_y;
    wlr_scene_node_set_position(&ss->dim[1]->node, 0, bot_y);
    wlr_scene_rect_set_size(ss->dim[1], ow, bot_h > 0 ? bot_h : 0);
    wlr_scene_node_set_enabled(&ss->dim[1]->node, bot_h > 0);

    wlr_scene_node_set_position(&ss->dim[2]->node, 0, sy);
    wlr_scene_rect_set_size(ss->dim[2], sx > 0 ? sx : 0, sh);
    wlr_scene_node_set_enabled(&ss->dim[2]->node, sx > 0);

    int r_x = sx + sw;
    int r_w = ow - r_x;
    wlr_scene_node_set_position(&ss->dim[3]->node, r_x, sy);
    wlr_scene_rect_set_size(ss->dim[3], r_w > 0 ? r_w : 0, sh);
    wlr_scene_node_set_enabled(&ss->dim[3]->node, r_w > 0);

    /* 1px border immediately outside the selection rectangle (spec §4.2.1) */
    wlr_scene_node_set_position(&ss->border[0]->node, sx - 1, sy - 1);
    wlr_scene_rect_set_size(ss->border[0], sw + 2, 1);

    wlr_scene_node_set_position(&ss->border[1]->node, sx - 1, sy + sh);
    wlr_scene_rect_set_size(ss->border[1], sw + 2, 1);

    wlr_scene_node_set_position(&ss->border[2]->node, sx - 1, sy);
    wlr_scene_rect_set_size(ss->border[2], 1, sh);

    wlr_scene_node_set_position(&ss->border[3]->node, sx + sw, sy);
    wlr_scene_rect_set_size(ss->border[3], 1, sh);

    /* 8 6x6 resize handles at corners and edge midpoints */
    int hs = HANDLE_SIZE_PX;
    int hh = hs / 2;
    int mx = sx + sw / 2 - hh;
    int my = sy + sh / 2 - hh;
    const int hx[8] = {
        sx - hh,      mx,           sx + sw - hh,
        sx - hh,                    sx + sw - hh,
        sx - hh,      mx,           sx + sw - hh,
    };
    const int hy[8] = {
        sy - hh,      sy - hh,      sy - hh,
        my,                         my,
        sy + sh - hh, sy + sh - hh, sy + sh - hh,
    };
    for (int i = 0; i < 8; i++) {
        if (ss->handles[i]) {
            wlr_scene_node_set_position(&ss->handles[i]->node, hx[i], hy[i]);
            wlr_scene_rect_set_size(ss->handles[i], hs, hs);
        }
    }

    /* Dimension badge: physical W x H, 6px above top-left or 6px inside when within 32px of top */
    int phys_sel_w = (int)lround((double)sw * ss->scale);
    int phys_sel_h = (int)lround((double)sh * ss->scale);
    if (phys_sel_w < 1) phys_sel_w = 1;
    if (phys_sel_h < 1) phys_sel_h = 1;

    char badge_str[64];
    snprintf(badge_str, sizeof(badge_str), "%d × %d", phys_sel_w, phys_sel_h);

    int bw = 0, bh = 0;
    struct wlr_buffer *bbuf = comp_text_render_badge(
        badge_str, ss->scale, k_badge_fg, k_badge_bg, k_badge_border, 8, 6, &bw, &bh);
    if (bbuf && ss->badge_buf) {
        comp_scene_buffer_set_owned_buffer(ss->badge_buf, bbuf);
        wlr_buffer_drop(bbuf);
        wlr_scene_buffer_set_dest_size(ss->badge_buf, bw, bh);

        int bx = (sy < BADGE_FLIP_TOP) ? (sx + BADGE_GAP_PX) : sx;
        int by = (sy < BADGE_FLIP_TOP) ? (sy + BADGE_GAP_PX) : (sy - bh - BADGE_GAP_PX);
        if (bx + bw > ow)
            bx = ow - bw;
        if (bx < 0)
            bx = 0;
        wlr_scene_node_set_position(&ss->badge_buf->node, bx, by);
    }

    comp_server_schedule_frames(ss->server);
}

bool
comp_screenshot_setup_overlay(CompScreenshot *ss, const struct wlr_box *obox,
                              uint8_t *pixels, int phys_w, int phys_h, double scale)
{
    CompServer *s = ss->server;
    size_t byte_len = (size_t)phys_w * (size_t)phys_h * 4u;
    uint8_t *display_copy = malloc(byte_len);
    if (!display_copy)
        return false;
    memcpy(display_copy, pixels, byte_len);

    struct wlr_buffer *fbuf = comp_pixel_buffer_create(display_copy, phys_w, phys_h, (size_t)phys_w * 4u);
    if (!fbuf) {
        free(display_copy);
        return false;
    }

    ss->tree = wlr_scene_tree_create(s->tree_overlay);
    if (!ss->tree) {
        wlr_buffer_drop(fbuf);
        return false;
    }
    wlr_scene_node_set_position(&ss->tree->node, obox->x, obox->y);
    wlr_scene_node_raise_to_top(&ss->tree->node);

    ss->frozen_buf = wlr_scene_buffer_create(ss->tree, NULL);
    if (ss->frozen_buf) {
        comp_scene_buffer_set_owned_buffer(ss->frozen_buf, fbuf);
        wlr_scene_buffer_set_dest_size(ss->frozen_buf, obox->width, obox->height);
    }
    wlr_buffer_drop(fbuf);

    const float *accent = s->config ? s->config->switcher_active_rgba : k_default_accent;
    for (int i = 0; i < 4; i++)
        ss->dim[i] = wlr_scene_rect_create(ss->tree, 0, 0, k_dim_rgba);
    for (int i = 0; i < 4; i++)
        ss->border[i] = wlr_scene_rect_create(ss->tree, 0, 0, accent);
    for (int i = 0; i < 8; i++)
        ss->handles[i] = wlr_scene_rect_create(ss->tree, HANDLE_SIZE_PX, HANDLE_SIZE_PX, accent);
    ss->badge_buf = wlr_scene_buffer_create(ss->tree, NULL);

    ss->frozen_pixels = pixels;
    ss->phys_w        = phys_w;
    ss->phys_h        = phys_h;
    ss->scale         = scale;
    ss->out_box       = *obox;
    ss->active        = true;
    ss->drag_mode     = DRAG_NONE;

    /* Default selection: centered 50% of the output */
    ss->sel_w = obox->width / 2;
    ss->sel_h = obox->height / 2;
    ss->sel_x = (obox->width - ss->sel_w) / 2;
    ss->sel_y = (obox->height - ss->sel_h) / 2;

    if (s->cursor && s->cursor_mgr)
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "crosshair");

    comp_screenshot_update_selection_scene(ss);
    return true;
}

void
comp_screenshot_close_overlay(CompScreenshot *ss)
{
    if (!ss)
        return;
    ss->active = false;
    ss->drag_mode = DRAG_NONE;
    free(ss->frozen_pixels);
    ss->frozen_pixels = NULL;
    if (ss->tree) {
        wlr_scene_node_destroy(&ss->tree->node);
        ss->tree = NULL;
    }
    ss->frozen_buf = NULL;
    ss->badge_buf  = NULL;
    memset(ss->dim, 0, sizeof(ss->dim));
    memset(ss->border, 0, sizeof(ss->border));
    memset(ss->handles, 0, sizeof(ss->handles));

    if (ss->server) {
        if (ss->server->cursor && ss->server->cursor_mgr)
            wlr_cursor_set_xcursor(ss->server->cursor, ss->server->cursor_mgr, "default");
        comp_server_schedule_frames(ss->server);
    }
}
