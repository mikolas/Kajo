/* comp_screenshot.c — Screenshot capture orchestration, GPU scene/output copy,
 * mode selection (`screenshot`, `screenshot-window`, `screenshot-screen`), and input.
 */
#include "comp_screenshot_private.h"

#include "comp_log.h"
#include "fx/comp_fx.h"

#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <talloc.h>
#include <unistd.h>
#include <wlr/render/allocator.h>
#include <wlr/render/color.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/interface.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/transform.h>
#include <xkbcommon/xkbcommon-keysyms.h>

/* ── Scene subtree offscreen rendering & pixel readback ── */

static float
transfer_function_luminance_multiplier_to_srgb(enum wlr_color_transfer_function tf)
{
    /* Matches wlroots color_transfer_function_get_default_luminance() against sRGB (ref=80, max=80) */
    float src_max = 80.0f;
    float src_ref = 80.0f;
    switch (tf) {
    case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
        src_max = 10000.0f;
        src_ref = 203.0f;
        break;
    case WLR_COLOR_TRANSFER_FUNCTION_BT1886:
        src_max = 100.0f;
        src_ref = 100.0f;
        break;
    case WLR_COLOR_TRANSFER_FUNCTION_SRGB:
    case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
    case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22:
        break;
    }
    return (80.0f / src_ref) * (src_max / 80.0f);
}

static bool
clip_box_to_pass(int x, int y, int w, int h, int pass_w, int pass_h,
                 struct wlr_box *out_box)
{
    if (w <= 0 || h <= 0 || pass_w <= 0 || pass_h <= 0)
        return false;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > pass_w) ? pass_w : (x + w);
    int y1 = (y + h > pass_h) ? pass_h : (y + h);
    if (x1 <= x0 || y1 <= y0)
        return false;
    out_box->x      = x0;
    out_box->y      = y0;
    out_box->width  = x1 - x0;
    out_box->height = y1 - y0;
    return true;
}

static void
render_node_recursive(CompServer *s, struct wlr_render_pass *pass,
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
            render_node_recursive(s, pass, child, lx, ly,
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
        struct wlr_box cbox;
        if (!clip_box_to_pass(rx, ry, rw, rh, pass_w, pass_h, &cbox))
            return;
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
            .box = cbox,
            .color = {
                .r = rect->color[0],
                .g = rect->color[1],
                .b = rect->color[2],
                .a = rect->color[3],
            },
            .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
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
        struct wlr_box cbox;
        if (!clip_box_to_pass(bx, by, bw, bh, pass_w, pass_h, &cbox))
            return;

        struct wlr_box dst_box = { .x = bx, .y = by, .width = bw, .height = bh };
        if (comp_fx_screenshot_render_node(s, pass, node, &dst_box, &cbox,
                                           origin_lx, origin_ly, scale, pass_w, pass_h))
            return;

        struct wlr_client_buffer *cbuf = wlr_client_buffer_get(sbuf->buffer);
        struct wlr_single_pixel_buffer_v1 *spb =
            wlr_single_pixel_buffer_v1_try_from_buffer(
                (cbuf && cbuf->source) ? cbuf->source : sbuf->buffer);
        if (spb) {
            float op = sbuf->opacity;
            float a = ((float)spb->a / (float)UINT32_MAX) * op;
            wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                .box = cbox,
                .color = {
                    .r = ((float)spb->r / (float)UINT32_MAX) * op,
                    .g = ((float)spb->g / (float)UINT32_MAX) * op,
                    .b = ((float)spb->b / (float)UINT32_MAX) * op,
                    .a = a,
                },
                .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
            });
            return;
        }

        struct wlr_texture *tex = (cbuf && cbuf->texture)
                                      ? cbuf->texture
                                      : wlr_texture_from_buffer(s->renderer, sbuf->buffer);
        if (!tex)
            return;

        struct wlr_fbox src_box = sbuf->src_box;
        if (cbox.x != bx || cbox.y != by || cbox.width != bw || cbox.height != bh) {
            if (wlr_fbox_empty(&src_box)) {
                int tw = (int)tex->width;
                int th = (int)tex->height;
                wlr_output_transform_coords(sbuf->transform, &tw, &th);
                src_box = (struct wlr_fbox){
                    .x = 0.0,
                    .y = 0.0,
                    .width  = (double)(tw > 0 ? tw : sbuf->buffer->width),
                    .height = (double)(th > 0 ? th : sbuf->buffer->height),
                };
            }
            double sx_scale = src_box.width / (double)bw;
            double sy_scale = src_box.height / (double)bh;
            src_box.x      += (double)(cbox.x - bx) * sx_scale;
            src_box.y      += (double)(cbox.y - by) * sy_scale;
            src_box.width   = (double)cbox.width * sx_scale;
            src_box.height  = (double)cbox.height * sy_scale;
        }

        struct wlr_color_primaries primaries_val;
        const struct wlr_color_primaries *primaries = NULL;
        if (sbuf->primaries != 0) {
            wlr_color_primaries_from_named(&primaries_val, sbuf->primaries);
            primaries = &primaries_val;
        }
        float lum_mult = transfer_function_luminance_multiplier_to_srgb(sbuf->transfer_function);
        float alpha = sbuf->opacity;
        wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
            .texture              = tex,
            .src_box              = src_box,
            .dst_box              = cbox,
            .transform            = wlr_output_transform_invert(sbuf->transform),
            .alpha                = &alpha,
            .filter_mode          = sbuf->filter_mode,
            .blend_mode           = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
            .transfer_function    = sbuf->transfer_function,
            .primaries            = primaries,
            .color_encoding       = sbuf->color_encoding,
            .color_range          = sbuf->color_range,
            .luminance_multiplier = &lum_mult,
        });
        if (!cbuf || !cbuf->texture)
            wlr_texture_destroy(tex);
    }
}

static const struct wlr_drm_format_set *
renderer_get_render_formats(struct wlr_renderer *r)
{
    if (!r || !r->WLR_PRIVATE.impl || !r->WLR_PRIVATE.impl->get_render_formats)
        return NULL;
    return r->WLR_PRIVATE.impl->get_render_formats(r);
}

static struct wlr_buffer *
allocate_offscreen_buffer(CompServer *s, int width, int height)
{
    if (!s || !s->allocator || !s->renderer || width <= 0 || height <= 0)
        return NULL;

    const struct wlr_drm_format_set *render_fmts =
        renderer_get_render_formats(s->renderer);
    const struct wlr_drm_format_set *tex_fmts =
        wlr_renderer_get_texture_formats(s->renderer, s->allocator->buffer_caps);
    if (render_fmts) {
        struct wlr_drm_format_set usable = {0};
        const struct wlr_drm_format_set *pick_set = render_fmts;
        if (tex_fmts && wlr_drm_format_set_intersect(&usable, render_fmts, tex_fmts))
            pick_set = &usable;

        static const uint32_t k_try_fmts[] = {
            DRM_FORMAT_ARGB8888,
            DRM_FORMAT_ABGR8888,
            DRM_FORMAT_XRGB8888,
            DRM_FORMAT_XBGR8888,
        };
        for (size_t i = 0; i < sizeof(k_try_fmts) / sizeof(k_try_fmts[0]); i++) {
            if (tex_fmts && !wlr_drm_format_set_get(tex_fmts, k_try_fmts[i]))
                continue;
            const struct wlr_drm_format *fmt = wlr_drm_format_set_get(pick_set, k_try_fmts[i]);
            if (fmt) {
                struct wlr_buffer *b = wlr_allocator_create_buffer(s->allocator, width, height, fmt);
                if (b) {
                    wlr_drm_format_set_finish(&usable);
                    return b;
                }
            }
        }
        wlr_drm_format_set_finish(&usable);
    }

    struct wlr_output *out = comp_server_primary_output(s);
    if (out && out->swapchain) {
        return wlr_allocator_create_buffer(s->allocator, width, height, &out->swapchain->format);
    }
    return NULL;
}

static void
convert_pixels_to_argb8888(uint8_t *pixels, int width, int height, uint32_t fmt)
{
    uint32_t *p32 = (uint32_t *)(void *)pixels;
    size_t total = (size_t)width * (size_t)height;
    switch (fmt) {
    case DRM_FORMAT_ARGB8888:
        break;
    case DRM_FORMAT_XRGB8888:
        for (size_t i = 0; i < total; i++)
            p32[i] |= 0xff000000u;
        break;
    case DRM_FORMAT_ABGR8888:
        for (size_t i = 0; i < total; i++) {
            uint32_t v = p32[i];
            p32[i] = (v & 0xff00ff00u) | ((v & 0x00ff0000u) >> 16) | ((v & 0x000000ffu) << 16);
        }
        break;
    case DRM_FORMAT_XBGR8888:
        for (size_t i = 0; i < total; i++) {
            uint32_t v = p32[i];
            p32[i] = 0xff000000u | (v & 0x0000ff00u) | ((v & 0x00ff0000u) >> 16) | ((v & 0x000000ffu) << 16);
        }
        break;
    default:
        break;
    }
}

static bool
is_supported_read_format(uint32_t fmt)
{
    return fmt == DRM_FORMAT_ARGB8888 ||
           fmt == DRM_FORMAT_ABGR8888 ||
           fmt == DRM_FORMAT_XRGB8888 ||
           fmt == DRM_FORMAT_XBGR8888;
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

    struct wlr_render_pass *pass = comp_fx_screenshot_begin_pass(s, gpu_buf);
    if (!pass) {
        wlr_buffer_drop(gpu_buf);
        return NULL;
    }

    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box        = { .x = 0, .y = 0, .width = phys_w, .height = phys_h },
        .color      = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = opaque_bg ? 1.0f : 0.0f },
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
    });

    render_node_recursive(s, pass, root_node, parent_lx, parent_ly,
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
        uint32_t pref_fmt = wlr_texture_preferred_read_format(tex);
        if (is_supported_read_format(pref_fmt)) {
            read_ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
                .data   = pixels,
                .format = pref_fmt,
                .stride = (uint32_t)stride,
            });
            if (read_ok)
                convert_pixels_to_argb8888(pixels, phys_w, phys_h, pref_fmt);
        }

        if (!read_ok) {
            static const uint32_t k_fallback_fmts[] = {
                DRM_FORMAT_ARGB8888,
                DRM_FORMAT_ABGR8888,
                DRM_FORMAT_XRGB8888,
                DRM_FORMAT_XBGR8888,
            };
            for (size_t i = 0; i < sizeof(k_fallback_fmts) / sizeof(k_fallback_fmts[0]); i++) {
                uint32_t ffmt = k_fallback_fmts[i];
                if (ffmt == pref_fmt)
                    continue;
                read_ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
                    .data   = pixels,
                    .format = ffmt,
                    .stride = (uint32_t)stride,
                });
                if (read_ok) {
                    wlr_log(WLR_ERROR,
                            "screenshot texture readback used fallback format 0x%08x (preferred=0x%08x)",
                            ffmt, pref_fmt);
                    convert_pixels_to_argb8888(pixels, phys_w, phys_h, ffmt);
                    break;
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
    comp_server_output_box(s, out, &obox);
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

/* ── Lifecycle, Capture Modes & Input ── */

static int
screenshot_destructor(CompScreenshot *ss)
{
    if (ss->notify_ev) {
        wl_event_source_remove(ss->notify_ev);
        ss->notify_ev = NULL;
    }
    if (ss->server && ss->server->scene)
        comp_screenshot_close_overlay(ss);
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
            s->loop, ss->notify_fd, WL_EVENT_READABLE, comp_screenshot_handle_worker_notify_fd, ss);
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
    comp_screenshot_close_overlay(s->screenshot);
}

void
comp_screenshot_clamp_selection(CompScreenshot *ss)
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

void
comp_screenshot_confirm(CompServer *s)
{
    if (!s || !s->screenshot || !s->screenshot->active)
        return;
    CompScreenshot *ss = s->screenshot;

    comp_screenshot_clamp_selection(ss);
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

    comp_screenshot_close_overlay(ss);
    if (cropped)
        comp_screenshot_dispatch_worker(s, cropped, pw, ph);
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
        comp_screenshot_close_overlay(ss);

    struct wlr_box obox = {0};
    double scale = 1.0;
    int phys_w = 0, phys_h = 0;
    uint8_t *pixels = capture_primary_output_pixels(s, &obox, &scale, &phys_w, &phys_h);
    if (!pixels || phys_w <= 0 || phys_h <= 0) {
        free(pixels);
        return false;
    }

    if (!comp_screenshot_setup_overlay(ss, &obox, pixels, phys_w, phys_h, scale)) {
        free(pixels);
        return false;
    }
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

    comp_screenshot_dispatch_worker(s, pixels, phys_w, phys_h);
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

    comp_screenshot_dispatch_worker(s, pixels, phys_w, phys_h);
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
        comp_screenshot_update_selection_scene(ss);
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

    comp_screenshot_update_selection_scene(ss);
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
        comp_screenshot_update_selection_scene(ss);
        return true;

    case XKB_KEY_Right:
        ss->sel_x += step;
        comp_screenshot_update_selection_scene(ss);
        return true;

    case XKB_KEY_Up:
        ss->sel_y -= step;
        comp_screenshot_update_selection_scene(ss);
        return true;

    case XKB_KEY_Down:
        ss->sel_y += step;
        comp_screenshot_update_selection_scene(ss);
        return true;

    default:
        return true;
    }
}
