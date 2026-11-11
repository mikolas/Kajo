/* comp_screenshot.c — In-compositor screenshot UI (`screenshot`, `screenshot-window`,
 * `screenshot-screen`), off-thread libpng encoder, wl_seat image/png clipboard
 * data source, and ScreenshotSaved IPC event delivery (Phase 3, spec §4.2).
 */
#include "comp_server.h"

#include "comp_log.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <png.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <talloc.h>
#include <time.h>
#include <unistd.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/transform.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define HANDLE_SIZE_PX   6
#define BADGE_FLIP_TOP  32
#define BADGE_GAP_PX     6

/* Premultiplied colors */
static const float k_dim_rgba[4]       = { 0.0f, 0.0f, 0.0f, 0.55f };
static const float k_default_accent[4] = { 1.0f, 0.2f, 0.4f, 1.0f };
static const float k_badge_fg[4]       = { 1.0f, 1.0f, 1.0f, 1.0f };
static const float k_badge_bg[4]       = { 0.066667f, 0.066667f, 0.066667f, 0.92f };
static const float k_badge_border[4]   = { 0.168627f, 0.168627f, 0.168627f, 1.0f };

typedef enum {
    DRAG_NONE = 0,
    DRAG_NEW,
    DRAG_MOVE,
    DRAG_HANDLE_TL,
    DRAG_HANDLE_TM,
    DRAG_HANDLE_TR,
    DRAG_HANDLE_ML,
    DRAG_HANDLE_MR,
    DRAG_HANDLE_BL,
    DRAG_HANDLE_BM,
    DRAG_HANDLE_BR,
} ScreenshotDragMode;

typedef struct ScreenshotJobResult {
    struct ScreenshotJobResult *next;
    char                       *path;
    uint8_t                    *png_bytes;
    size_t                      png_len;
    bool                        copy_to_clipboard;
    bool                        show_notification;
    bool                        ok;
} ScreenshotJobResult;

typedef struct {
    struct CompScreenshot *ss;
    uint8_t               *argb_pixels;
    int                    width;
    int                    height;
    char                  *path_template;
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

typedef struct CompScreenshot {
    CompServer              *server;
    bool                     active;

    /* Frozen output state for interactive `screenshot` */
    uint8_t                 *frozen_pixels; /* owned copy for cropping on confirm */
    int                      phys_w;
    int                      phys_h;
    double                   scale;
    struct wlr_box           out_box;       /* logical output box */

    /* Selection box in output-local logical coordinates (0..out_box.width/height) */
    int                      sel_x;
    int                      sel_y;
    int                      sel_w;
    int                      sel_h;

    ScreenshotDragMode       drag_mode;
    double                   drag_start_lx;
    double                   drag_start_ly;
    int                      drag_orig_x;
    int                      drag_orig_y;
    int                      drag_orig_w;
    int                      drag_orig_h;

    /* Overlay scene nodes in s->tree_overlay */
    struct wlr_scene_tree   *tree;
    struct wlr_scene_buffer *frozen_buf;
    struct wlr_scene_rect   *dim[4];        /* top, bottom, left, right */
    struct wlr_scene_rect   *border[4];     /* 1px outside selection */
    struct wlr_scene_rect   *handles[8];    /* TL, TM, TR, ML, MR, BL, BM, BR */
    struct wlr_scene_buffer *badge_buf;

    /* Background worker completion queue */
    int                       notify_fd;
    struct wl_event_source  *notify_ev;
    pthread_mutex_t          res_lock;
    pthread_cond_t           workers_done;
    int                      pending_workers;
    bool                     shutting_down;
    ScreenshotJobResult     *res_head;
    ScreenshotJobResult     *res_tail;
} CompScreenshot;

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
expand_screenshot_path(const char *tmpl)
{
    const char *use_tmpl = (tmpl && *tmpl)
                               ? tmpl
                               : "~/Pictures/Screenshots/Screenshot from %Y-%m-%d %H-%M-%S.png";
    char expanded_tilde[PATH_MAX];
    if (use_tmpl[0] == '~' && (use_tmpl[1] == '/' || use_tmpl[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home || !*home)
            home = "/tmp";
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
        final_path = expand_screenshot_path(wa->path_template);
        if (!final_path || !write_bytes_to_file(final_path, png_bytes, png_len))
            ok = false;
    }
    free(wa->path_template);

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

static void
dispatch_screenshot_worker(CompServer *s, uint8_t *owned_argb, int width, int height)
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
    wa->ss                = ss;
    wa->argb_pixels       = owned_argb;
    wa->width             = width;
    wa->height            = height;
    wa->path_template     = strdup((s->config && s->config->screenshot_path)
                                       ? s->config->screenshot_path
                                       : "~/Pictures/Screenshots/Screenshot from %Y-%m-%d %H-%M-%S.png");
    wa->copy_to_clipboard = s->config ? s->config->screenshot_copy : true;
    wa->show_notification = s->config ? s->config->screenshot_notify : true;

    pthread_mutex_lock(&ss->res_lock);
    if (ss->shutting_down) {
        pthread_mutex_unlock(&ss->res_lock);
        free(wa->path_template);
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

static int
handle_worker_notify_fd(int fd, uint32_t mask, void *data)
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

/* ── Scene subtree offscreen rendering & pixel readback ── */

static void
render_node_recursive(struct wlr_renderer *renderer, struct wlr_render_pass *pass,
                      struct wlr_scene_node *node, int parent_lx, int parent_ly,
                      int origin_lx, int origin_ly, double scale,
                      int pass_w, int pass_h)
{
    if (!node || !node->enabled)
        return;

    int lx = parent_lx + node->x;
    int ly = parent_ly + node->y;

    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link) {
            render_node_recursive(renderer, pass, child, lx, ly,
                                  origin_lx, origin_ly, scale, pass_w, pass_h);
        }
        return;
    }

    if (node->type == WLR_SCENE_NODE_RECT) {
        struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
        if (rect->width <= 0 || rect->height <= 0 || rect->color[3] <= 0.0f)
            return;
        int rx = (int)lround((double)(lx - origin_lx) * scale);
        int ry = (int)lround((double)(ly - origin_ly) * scale);
        int rw = (int)lround((double)(lx - origin_lx + rect->width) * scale) - rx;
        int rh = (int)lround((double)(ly - origin_ly + rect->height) * scale) - ry;
        if (rw <= 0 || rh <= 0 || rx >= pass_w || ry >= pass_h || rx + rw <= 0 || ry + rh <= 0)
            return;
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
            .box = { .x = rx, .y = ry, .width = rw, .height = rh },
            .color = {
                .r = rect->color[0],
                .g = rect->color[1],
                .b = rect->color[2],
                .a = rect->color[3],
            },
            .blend_mode = rect->color[3] < 1.0f
                              ? WLR_RENDER_BLEND_MODE_PREMULTIPLIED
                              : WLR_RENDER_BLEND_MODE_NONE,
        });
        return;
    }

    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
        if (!sbuf->buffer || sbuf->opacity <= 0.0f)
            return;
        int log_w = sbuf->dst_width > 0 ? sbuf->dst_width : sbuf->buffer->width;
        int log_h = sbuf->dst_height > 0 ? sbuf->dst_height : sbuf->buffer->height;
        if (log_w <= 0 || log_h <= 0)
            return;
        int bx = (int)lround((double)(lx - origin_lx) * scale);
        int by = (int)lround((double)(ly - origin_ly) * scale);
        int bw = (int)lround((double)(lx - origin_lx + log_w) * scale) - bx;
        int bh = (int)lround((double)(ly - origin_ly + log_h) * scale) - by;
        if (bw <= 0 || bh <= 0 || bx >= pass_w || by >= pass_h || bx + bw <= 0 || by + bh <= 0)
            return;

        struct wlr_single_pixel_buffer_v1 *spb =
            wlr_single_pixel_buffer_v1_try_from_buffer(sbuf->buffer);
        if (spb) {
            float op = sbuf->opacity;
            float a = ((float)spb->a / (float)UINT32_MAX) * op;
            wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                .box = { .x = bx, .y = by, .width = bw, .height = bh },
                .color = {
                    .r = ((float)spb->r / (float)UINT32_MAX) * op,
                    .g = ((float)spb->g / (float)UINT32_MAX) * op,
                    .b = ((float)spb->b / (float)UINT32_MAX) * op,
                    .a = a,
                },
                .blend_mode = a < 1.0f
                                  ? WLR_RENDER_BLEND_MODE_PREMULTIPLIED
                                  : WLR_RENDER_BLEND_MODE_NONE,
            });
            return;
        }

        struct wlr_client_buffer *cbuf = wlr_client_buffer_get(sbuf->buffer);
        struct wlr_texture *tex = (cbuf && cbuf->texture)
                                      ? cbuf->texture
                                      : wlr_texture_from_buffer(renderer, sbuf->buffer);
        if (!tex)
            return;
        float alpha = sbuf->opacity;
        wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
            .texture     = tex,
            .src_box     = sbuf->src_box,
            .dst_box     = { .x = bx, .y = by, .width = bw, .height = bh },
            .transform   = wlr_output_transform_invert(sbuf->transform),
            .alpha       = &alpha,
            .filter_mode = sbuf->filter_mode,
            .blend_mode  = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
        });
        if (!cbuf || !cbuf->texture)
            wlr_texture_destroy(tex);
    }
}

static struct wlr_buffer *
allocate_offscreen_buffer(CompServer *s, int width, int height)
{
    if (!s || !s->allocator || !s->renderer || width <= 0 || height <= 0)
        return NULL;

    const struct wlr_drm_format_set *tex_fmts =
        wlr_renderer_get_texture_formats(s->renderer, s->allocator->buffer_caps);
    if (tex_fmts) {
        static const uint32_t k_try_fmts[] = {
            DRM_FORMAT_ARGB8888,
            DRM_FORMAT_XRGB8888,
            DRM_FORMAT_ABGR8888,
            DRM_FORMAT_XBGR8888,
        };
        for (size_t i = 0; i < sizeof(k_try_fmts) / sizeof(k_try_fmts[0]); i++) {
            const struct wlr_drm_format *fmt = wlr_drm_format_set_get(tex_fmts, k_try_fmts[i]);
            if (fmt) {
                struct wlr_buffer *b = wlr_allocator_create_buffer(s->allocator, width, height, fmt);
                if (b)
                    return b;
            }
        }
    }

    struct wlr_output *out = comp_server_primary_output(s);
    if (out && out->swapchain) {
        return wlr_allocator_create_buffer(s->allocator, width, height, &out->swapchain->format);
    }
    return NULL;
}

static uint8_t *
render_subtree_to_argb8888(CompServer *s, struct wlr_scene_node *root_node,
                           int parent_lx, int parent_ly,
                           int origin_lx, int origin_ly,
                           int logical_w, int logical_h,
                           double scale, bool opaque_bg,
                           int *out_phys_w, int *out_phys_h)
{
    if (!s || !root_node || logical_w <= 0 || logical_h <= 0)
        return NULL;
    if (!(scale > 0.0) || !isfinite(scale))
        scale = 1.0;

    int phys_w = (int)lround((double)logical_w * scale);
    int phys_h = (int)lround((double)logical_h * scale);
    if (phys_w < 1) phys_w = 1;
    if (phys_h < 1) phys_h = 1;

    struct wlr_buffer *gpu_buf = allocate_offscreen_buffer(s, phys_w, phys_h);
    if (!gpu_buf)
        return NULL;

    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(s->renderer, gpu_buf, NULL);
    if (!pass) {
        wlr_buffer_drop(gpu_buf);
        return NULL;
    }

    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box        = { .x = 0, .y = 0, .width = phys_w, .height = phys_h },
        .color      = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = opaque_bg ? 1.0f : 0.0f },
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
    });

    render_node_recursive(s->renderer, pass, root_node, parent_lx, parent_ly,
                          origin_lx, origin_ly, scale, phys_w, phys_h);

    if (!wlr_render_pass_submit(pass)) {
        wlr_buffer_drop(gpu_buf);
        return NULL;
    }

    size_t stride = (size_t)phys_w * 4u;
    uint8_t *pixels = malloc(stride * (size_t)phys_h);
    if (!pixels) {
        wlr_buffer_drop(gpu_buf);
        return NULL;
    }

    bool read_ok = false;
    struct wlr_texture *tex = wlr_texture_from_buffer(s->renderer, gpu_buf);
    if (tex) {
        read_ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
            .data   = pixels,
            .format = DRM_FORMAT_ARGB8888,
            .stride = (uint32_t)stride,
        });
        if (!read_ok) {
            read_ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
                .data   = pixels,
                .format = DRM_FORMAT_XRGB8888,
                .stride = (uint32_t)stride,
            });
            if (read_ok) {
                uint32_t *p32 = (uint32_t *)(void *)pixels;
                size_t total = (size_t)phys_w * (size_t)phys_h;
                for (size_t i = 0; i < total; i++)
                    p32[i] |= 0xff000000u;
            }
        }
        if (!read_ok) {
            read_ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
                .data   = pixels,
                .format = DRM_FORMAT_ABGR8888,
                .stride = (uint32_t)stride,
            });
            if (read_ok) {
                uint32_t *p32 = (uint32_t *)(void *)pixels;
                size_t total = (size_t)phys_w * (size_t)phys_h;
                for (size_t i = 0; i < total; i++) {
                    uint32_t v = p32[i];
                    p32[i] = (v & 0xff00ff00u) | ((v & 0x00ff0000u) >> 16) | ((v & 0x000000ffu) << 16);
                }
            }
        }
        wlr_texture_destroy(tex);
    }
    wlr_buffer_drop(gpu_buf);

    if (!read_ok) {
        free(pixels);
        return NULL;
    }

    if (out_phys_w) *out_phys_w = phys_w;
    if (out_phys_h) *out_phys_h = phys_h;
    return pixels;
}

static uint8_t *
capture_primary_output_pixels(CompServer *s, struct wlr_box *out_logical_box,
                              double *out_scale, int *out_phys_w, int *out_phys_h)
{
    struct wlr_output *out = comp_server_primary_output(s);
    if (!out || !s->scene)
        return NULL;

    struct wlr_box obox = {0};
    wlr_output_layout_get_box(s->output_layout, out, &obox);
    if (obox.width <= 0 || obox.height <= 0) {
        double vw = 1920.0, vh = 1080.0;
        comp_server_viewport_size(s, &vw, &vh);
        obox.width  = (int)vw;
        obox.height = (int)vh;
    }
    double scale = out->scale > 0.0f ? (double)out->scale : 1.0;

    bool overlay_was_enabled = s->tree_overlay ? s->tree_overlay->node.enabled : false;
    if (s->tree_overlay)
        wlr_scene_node_set_enabled(&s->tree_overlay->node, false);

    uint8_t *pixels = render_subtree_to_argb8888(
        s, &s->scene->tree.node, 0, 0, obox.x, obox.y,
        obox.width, obox.height, scale, true, out_phys_w, out_phys_h);

    if (s->tree_overlay)
        wlr_scene_node_set_enabled(&s->tree_overlay->node, overlay_was_enabled);

    if (out_logical_box) *out_logical_box = obox;
    if (out_scale)       *out_scale       = scale;
    return pixels;
}

/* ── Interactive Area Selection UI (spec §4.2.1) ── */

static void
clamp_selection(CompScreenshot *ss)
{
    int max_w = ss->out_box.width;
    int max_h = ss->out_box.height;
    if (max_w < 1) max_w = 1;
    if (max_h < 1) max_h = 1;

    if (ss->sel_w < 1) ss->sel_w = 1;
    if (ss->sel_h < 1) ss->sel_h = 1;
    if (ss->sel_w > max_w) ss->sel_w = max_w;
    if (ss->sel_h > max_h) ss->sel_h = max_h;

    if (ss->sel_x < 0) ss->sel_x = 0;
    if (ss->sel_y < 0) ss->sel_y = 0;
    if (ss->sel_x + ss->sel_w > max_w)
        ss->sel_x = max_w - ss->sel_w;
    if (ss->sel_y + ss->sel_h > max_h)
        ss->sel_y = max_h - ss->sel_h;
}

static void
update_selection_scene(CompScreenshot *ss)
{
    if (!ss || !ss->active || !ss->tree)
        return;

    clamp_selection(ss);

    int ow = ss->out_box.width;
    int oh = ss->out_box.height;
    int sx = ss->sel_x;
    int sy = ss->sel_y;
    int sw = ss->sel_w;
    int sh = ss->sel_h;

    /* 4 dimming rects outside the selection rectangle */
    /* top: (0, 0, ow, sy) */
    wlr_scene_node_set_position(&ss->dim[0]->node, 0, 0);
    wlr_scene_rect_set_size(ss->dim[0], ow, sy > 0 ? sy : 0);
    wlr_scene_node_set_enabled(&ss->dim[0]->node, sy > 0);

    /* bottom: (0, sy + sh, ow, oh - (sy + sh)) */
    int bot_y = sy + sh;
    int bot_h = oh - bot_y;
    wlr_scene_node_set_position(&ss->dim[1]->node, 0, bot_y);
    wlr_scene_rect_set_size(ss->dim[1], ow, bot_h > 0 ? bot_h : 0);
    wlr_scene_node_set_enabled(&ss->dim[1]->node, bot_h > 0);

    /* left: (0, sy, sx, sh) */
    wlr_scene_node_set_position(&ss->dim[2]->node, 0, sy);
    wlr_scene_rect_set_size(ss->dim[2], sx > 0 ? sx : 0, sh);
    wlr_scene_node_set_enabled(&ss->dim[2]->node, sx > 0);

    /* right: (sx + sw, sy, ow - (sx + sw), sh) */
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
        wlr_scene_buffer_set_buffer(ss->badge_buf, bbuf);
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

static void
screenshot_close_overlay(CompScreenshot *ss)
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

static int
screenshot_destructor(CompScreenshot *ss)
{
    if (ss->notify_ev) {
        wl_event_source_remove(ss->notify_ev);
        ss->notify_ev = NULL;
    }
    if (ss->server && ss->server->scene)
        screenshot_close_overlay(ss);
    else
        free(ss->frozen_pixels);

    pthread_mutex_lock(&ss->res_lock);
    ss->shutting_down = true;
    while (ss->pending_workers > 0)
        pthread_cond_wait(&ss->workers_done, &ss->res_lock);
    ScreenshotJobResult *cur = ss->res_head;
    ss->res_head = NULL;
    ss->res_tail = NULL;
    pthread_mutex_unlock(&ss->res_lock);
    if (ss->notify_fd >= 0) {
        close(ss->notify_fd);
        ss->notify_fd = -1;
    }
    while (cur) {
        ScreenshotJobResult *next = cur->next;
        free(cur->path);
        free(cur->png_bytes);
        free(cur);
        cur = next;
    }
    pthread_cond_destroy(&ss->workers_done);
    pthread_mutex_destroy(&ss->res_lock);
    return 0;
}

void
comp_screenshot_init(CompServer *s)
{
    if (!s || s->screenshot)
        return;

    CompScreenshot *ss = talloc_zero(s, CompScreenshot);
    COMP_WARN_RETURN_VOID(ss != NULL);
    talloc_set_name_const(ss, "CompScreenshot");
    ss->server    = s;
    ss->notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_mutex_init(&ss->res_lock, NULL);
    pthread_cond_init(&ss->workers_done, NULL);
    talloc_set_destructor(ss, screenshot_destructor);

    if (ss->notify_fd >= 0 && s->loop) {
        ss->notify_ev = wl_event_loop_add_fd(
            s->loop, ss->notify_fd, WL_EVENT_READABLE, handle_worker_notify_fd, ss);
    }
    s->screenshot = ss;
}

bool
comp_screenshot_is_active(const CompServer *s)
{
    return s && s->screenshot && s->screenshot->active;
}

void
comp_screenshot_cancel(CompServer *s)
{
    if (!s || !s->screenshot || !s->screenshot->active)
        return;
    screenshot_close_overlay(s->screenshot);
}

void
comp_screenshot_confirm(CompServer *s)
{
    if (!s || !s->screenshot || !s->screenshot->active)
        return;
    CompScreenshot *ss = s->screenshot;

    clamp_selection(ss);
    int px = (int)lround((double)ss->sel_x * ss->scale);
    int py = (int)lround((double)ss->sel_y * ss->scale);
    int pw = (int)lround((double)ss->sel_w * ss->scale);
    int ph = (int)lround((double)ss->sel_h * ss->scale);

    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    if (px + pw > ss->phys_w) pw = ss->phys_w - px;
    if (py + ph > ss->phys_h) ph = ss->phys_h - py;

    uint8_t *cropped = NULL;
    if (ss->frozen_pixels && pw > 0 && ph > 0) {
        cropped = malloc((size_t)pw * (size_t)ph * 4u);
        if (cropped) {
            for (int row = 0; row < ph; row++) {
                const uint8_t *src_row =
                    ss->frozen_pixels + ((size_t)(py + row) * (size_t)ss->phys_w + (size_t)px) * 4u;
                uint8_t *dst_row = cropped + (size_t)row * (size_t)pw * 4u;
                memcpy(dst_row, src_row, (size_t)pw * 4u);
            }
        }
    }

    screenshot_close_overlay(ss);
    if (cropped)
        dispatch_screenshot_worker(s, cropped, pw, ph);
}

bool
comp_screenshot_interactive(CompServer *s)
{
    if (!s || s->locked)
        return false;
    if (!s->screenshot)
        comp_screenshot_init(s);
    CompScreenshot *ss = s->screenshot;
    if (!ss)
        return false;

    if (comp_switcher_is_active(s))
        comp_switcher_cancel(s);
    if (ss->active)
        screenshot_close_overlay(ss);

    struct wlr_box obox = {0};
    double scale = 1.0;
    int phys_w = 0, phys_h = 0;
    uint8_t *pixels = capture_primary_output_pixels(s, &obox, &scale, &phys_w, &phys_h);
    if (!pixels || phys_w <= 0 || phys_h <= 0) {
        free(pixels);
        return false;
    }

    size_t byte_len = (size_t)phys_w * (size_t)phys_h * 4u;
    uint8_t *display_copy = malloc(byte_len);
    if (!display_copy) {
        free(pixels);
        return false;
    }
    memcpy(display_copy, pixels, byte_len);

    struct wlr_buffer *fbuf = comp_pixel_buffer_create(display_copy, phys_w, phys_h, (size_t)phys_w * 4u);
    if (!fbuf) {
        free(display_copy);
        free(pixels);
        return false;
    }

    ss->tree = wlr_scene_tree_create(s->tree_overlay);
    if (!ss->tree) {
        wlr_buffer_drop(fbuf);
        free(pixels);
        return false;
    }
    wlr_scene_node_set_position(&ss->tree->node, obox.x, obox.y);
    wlr_scene_node_raise_to_top(&ss->tree->node);

    ss->frozen_buf = wlr_scene_buffer_create(ss->tree, fbuf);
    wlr_buffer_drop(fbuf);
    if (ss->frozen_buf)
        wlr_scene_buffer_set_dest_size(ss->frozen_buf, obox.width, obox.height);

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
    ss->out_box       = obox;
    ss->active        = true;
    ss->drag_mode     = DRAG_NONE;

    /* Default selection: centered 50% of the output */
    ss->sel_w = obox.width / 2;
    ss->sel_h = obox.height / 2;
    ss->sel_x = (obox.width - ss->sel_w) / 2;
    ss->sel_y = (obox.height - ss->sel_h) / 2;

    if (s->cursor && s->cursor_mgr)
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "crosshair");

    update_selection_scene(ss);
    return true;
}

bool
comp_screenshot_screen(CompServer *s)
{
    if (!s || s->locked)
        return false;
    if (!s->screenshot)
        comp_screenshot_init(s);

    int phys_w = 0, phys_h = 0;
    uint8_t *pixels = capture_primary_output_pixels(s, NULL, NULL, &phys_w, &phys_h);
    if (!pixels)
        return false;

    dispatch_screenshot_worker(s, pixels, phys_w, phys_h);
    return true;
}

bool
comp_screenshot_window(CompServer *s)
{
    if (!s || s->locked || !s->layout)
        return false;
    if (!s->screenshot)
        comp_screenshot_init(s);

    CompWinId fid = comp_layout_focused_window(s->layout);
    struct CompWindow *win = comp_win_from_id(s, fid);
    if (!win)
        return false;

    bool include_border = s->config ? s->config->screenshot_border : true;
    struct wlr_scene_node *root_node = NULL;
    struct wlr_box lbox = {0};
    if (!comp_window_get_capture_box(win, include_border, &root_node, &lbox))
        return false;

    struct wlr_output *out = comp_server_primary_output(s);
    double scale = (out && out->scale > 0.0f) ? (double)out->scale : 1.0;

    int phys_w = 0, phys_h = 0;
    uint8_t *pixels = render_subtree_to_argb8888(
        s, root_node, -root_node->x, -root_node->y,
        lbox.x, lbox.y, lbox.width, lbox.height,
        scale, false, &phys_w, &phys_h);
    if (!pixels)
        return false;

    dispatch_screenshot_worker(s, pixels, phys_w, phys_h);
    return true;
}

static ScreenshotDragMode
hit_test_selection(const CompScreenshot *ss, double rx, double ry)
{
    int sx = ss->sel_x, sy = ss->sel_y, sw = ss->sel_w, sh = ss->sel_h;
    int mx = sx + sw / 2, my = sy + sh / 2;
    const int hx[8] = { sx, mx, sx + sw, sx, sx + sw, sx, mx, sx + sw };
    const int hy[8] = { sy, sy, sy,      my, my,      sy + sh, sy + sh, sy + sh };
    const ScreenshotDragMode modes[8] = {
        DRAG_HANDLE_TL, DRAG_HANDLE_TM, DRAG_HANDLE_TR,
        DRAG_HANDLE_ML,                 DRAG_HANDLE_MR,
        DRAG_HANDLE_BL, DRAG_HANDLE_BM, DRAG_HANDLE_BR,
    };
    for (int i = 0; i < 8; i++) {
        if (fabs(rx - (double)hx[i]) <= 6.0 && fabs(ry - (double)hy[i]) <= 6.0)
            return modes[i];
    }
    if (rx >= (double)sx && rx <= (double)(sx + sw) &&
        ry >= (double)sy && ry <= (double)(sy + sh)) {
        return DRAG_MOVE;
    }
    return DRAG_NEW;
}

bool
comp_screenshot_handle_button(CompServer *s, uint32_t button, bool pressed,
                              double lx, double ly)
{
    if (!s || !s->screenshot || !s->screenshot->active)
        return false;
    CompScreenshot *ss = s->screenshot;

    if (button == BTN_RIGHT && pressed) {
        comp_screenshot_cancel(s);
        return true;
    }
    if (button != BTN_LEFT)
        return true;

    if (!pressed) {
        ss->drag_mode = DRAG_NONE;
        return true;
    }

    double rx = lx - (double)ss->out_box.x;
    double ry = ly - (double)ss->out_box.y;
    ss->drag_mode     = hit_test_selection(ss, rx, ry);
    ss->drag_start_lx = rx;
    ss->drag_start_ly = ry;
    ss->drag_orig_x   = ss->sel_x;
    ss->drag_orig_y   = ss->sel_y;
    ss->drag_orig_w   = ss->sel_w;
    ss->drag_orig_h   = ss->sel_h;

    if (ss->drag_mode == DRAG_NEW) {
        ss->sel_x = (int)lround(rx);
        ss->sel_y = (int)lround(ry);
        ss->sel_w = 1;
        ss->sel_h = 1;
        update_selection_scene(ss);
    }
    return true;
}

bool
comp_screenshot_handle_motion(CompServer *s, double lx, double ly)
{
    if (!s || !s->screenshot || !s->screenshot->active)
        return false;
    CompScreenshot *ss = s->screenshot;
    if (s->cursor && s->cursor_mgr)
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "crosshair");

    if (ss->drag_mode == DRAG_NONE)
        return true;

    double rx = lx - (double)ss->out_box.x;
    double ry = ly - (double)ss->out_box.y;
    int dx = (int)lround(rx - ss->drag_start_lx);
    int dy = (int)lround(ry - ss->drag_start_ly);

    int x1 = ss->drag_orig_x;
    int y1 = ss->drag_orig_y;
    int x2 = ss->drag_orig_x + ss->drag_orig_w;
    int y2 = ss->drag_orig_y + ss->drag_orig_h;

    switch (ss->drag_mode) {
    case DRAG_NEW: {
        int sx = (int)lround(ss->drag_start_lx);
        int sy = (int)lround(ss->drag_start_ly);
        int ex = (int)lround(rx);
        int ey = (int)lround(ry);
        ss->sel_x = sx < ex ? sx : ex;
        ss->sel_y = sy < ey ? sy : ey;
        ss->sel_w = abs(ex - sx);
        ss->sel_h = abs(ey - sy);
        break;
    }
    case DRAG_MOVE:
        ss->sel_x = ss->drag_orig_x + dx;
        ss->sel_y = ss->drag_orig_y + dy;
        break;
    case DRAG_HANDLE_TL:
        x1 += dx; y1 += dy;
        break;
    case DRAG_HANDLE_TM:
        y1 += dy;
        break;
    case DRAG_HANDLE_TR:
        x2 += dx; y1 += dy;
        break;
    case DRAG_HANDLE_ML:
        x1 += dx;
        break;
    case DRAG_HANDLE_MR:
        x2 += dx;
        break;
    case DRAG_HANDLE_BL:
        x1 += dx; y2 += dy;
        break;
    case DRAG_HANDLE_BM:
        y2 += dy;
        break;
    case DRAG_HANDLE_BR:
        x2 += dx; y2 += dy;
        break;
    case DRAG_NONE:
        break;
    }

    if (ss->drag_mode >= DRAG_HANDLE_TL) {
        ss->sel_x = x1 < x2 ? x1 : x2;
        ss->sel_y = y1 < y2 ? y1 : y2;
        ss->sel_w = abs(x2 - x1);
        ss->sel_h = abs(y2 - y1);
    }

    update_selection_scene(ss);
    return true;
}

bool
comp_screenshot_handle_key(CompServer *s, uint32_t mods, uint32_t keysym)
{
    if (!s || !s->screenshot || !s->screenshot->active)
        return false;
    CompScreenshot *ss = s->screenshot;
    int step = (mods & COMP_MOD_SHIFT) ? 10 : 1;

    switch (keysym) {
    case XKB_KEY_Escape:
        comp_screenshot_cancel(s);
        return true;

    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        comp_screenshot_confirm(s);
        return true;

    case XKB_KEY_Left:
        ss->sel_x -= step;
        update_selection_scene(ss);
        return true;

    case XKB_KEY_Right:
        ss->sel_x += step;
        update_selection_scene(ss);
        return true;

    case XKB_KEY_Up:
        ss->sel_y -= step;
        update_selection_scene(ss);
        return true;

    case XKB_KEY_Down:
        ss->sel_y += step;
        update_selection_scene(ss);
        return true;

    default:
        return true;
    }
}
