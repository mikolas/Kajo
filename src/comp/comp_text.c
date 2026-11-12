/* comp_text.c — Read-only wlr_buffer wrappers for Cairo/pangocairo labels and
 * raw ARGB8888 pixel buffers (Phase 3, spec §4.1 & §4.2).
 */
#include "comp_server.h"

#include "comp_log.h"

#include <cairo.h>
#include <drm_fourcc.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

typedef struct {
    struct wlr_buffer  base;
    cairo_surface_t   *surface;
    uint8_t           *owned_pixels;
    size_t             stride;
    uint32_t           format;
} CompReadonlyBuffer;

static void
readonly_buffer_destroy(struct wlr_buffer *wlr_buf)
{
    CompReadonlyBuffer *buf = wl_container_of(wlr_buf, buf, base);
    wlr_buffer_finish(&buf->base);
    if (buf->surface) {
        cairo_surface_destroy(buf->surface);
        buf->surface = NULL;
    }
    if (buf->owned_pixels) {
        free(buf->owned_pixels);
        buf->owned_pixels = NULL;
    }
    free(buf);
}

static bool
readonly_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buf, uint32_t flags,
                                      void **data, uint32_t *format, size_t *stride)
{
    CompReadonlyBuffer *buf = wl_container_of(wlr_buf, buf, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
        return false;

    if (buf->surface) {
        *data = cairo_image_surface_get_data(buf->surface);
    } else if (buf->owned_pixels) {
        *data = buf->owned_pixels;
    } else {
        return false;
    }
    *format = buf->format;
    *stride = buf->stride;
    return true;
}

static void
readonly_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buf)
{
    (void)wlr_buf;
}

static const struct wlr_buffer_impl k_readonly_buffer_impl = {
    .destroy               = readonly_buffer_destroy,
    .begin_data_ptr_access = readonly_buffer_begin_data_ptr_access,
    .end_data_ptr_access   = readonly_buffer_end_data_ptr_access,
};

struct wlr_buffer *
comp_pixel_buffer_create(uint8_t *owned_argb8888, int width, int height, size_t stride)
{
    if (!owned_argb8888 || width <= 0 || height <= 0 || stride < (size_t)width * 4u)
        return NULL;

    CompReadonlyBuffer *buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    wlr_buffer_init(&buf->base, &k_readonly_buffer_impl, width, height);
    buf->owned_pixels = owned_argb8888;
    buf->stride       = stride;
    buf->format       = DRM_FORMAT_ARGB8888;
    return &buf->base;
}

static struct wlr_buffer *
wrap_cairo_surface(cairo_surface_t *surf)
{
    if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        if (surf)
            cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_surface_flush(surf);
    int w = cairo_image_surface_get_width(surf);
    int h = cairo_image_surface_get_height(surf);
    int s = cairo_image_surface_get_stride(surf);
    if (w <= 0 || h <= 0 || s <= 0) {
        cairo_surface_destroy(surf);
        return NULL;
    }

    CompReadonlyBuffer *buf = calloc(1, sizeof(*buf));
    if (!buf) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    wlr_buffer_init(&buf->base, &k_readonly_buffer_impl, w, h);
    buf->surface = surf;
    buf->stride  = (size_t)s;
    buf->format  = DRM_FORMAT_ARGB8888;
    return &buf->base;
}

struct wlr_buffer *
comp_text_render_label(const char *text, int logical_w, int logical_h, double scale,
                       const float fg_rgba[4], const float bg_rgba[4],
                       const float border_rgba[4], int border_px, int pad_x)
{
    if (logical_w <= 0 || logical_h <= 0)
        return NULL;
    if (!(scale > 0.0) || !isfinite(scale))
        scale = 1.0;

    int phys_w = (int)lround((double)logical_w * scale);
    int phys_h = (int)lround((double)logical_h * scale);
    if (phys_w < 1) phys_w = 1;
    if (phys_h < 1) phys_h = 1;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, phys_w, phys_h);
    if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        if (surf)
            cairo_surface_destroy(surf);
        return NULL;
    }

    cairo_t *cr = cairo_create(surf);
    cairo_scale(cr, scale, scale);

    /* Clear to transparent first */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    if (bg_rgba && bg_rgba[3] > 0.0f) {
        cairo_set_source_rgba(cr, (double)bg_rgba[0], (double)bg_rgba[1],
                              (double)bg_rgba[2], (double)bg_rgba[3]);
        cairo_rectangle(cr, 0.0, 0.0, (double)logical_w, (double)logical_h);
        cairo_fill(cr);
    }

    if (border_rgba && border_px > 0 && border_rgba[3] > 0.0f) {
        double bw = (double)border_px;
        cairo_set_source_rgba(cr, (double)border_rgba[0], (double)border_rgba[1],
                              (double)border_rgba[2], (double)border_rgba[3]);
        cairo_set_line_width(cr, bw);
        cairo_rectangle(cr, bw * 0.5, bw * 0.5,
                        (double)logical_w - bw, (double)logical_h - bw);
        cairo_stroke(cr);
    }

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("Fira Code, monospace 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    int avail_w = logical_w - 2 * pad_x;
    if (avail_w < 1)
        avail_w = 1;
    pango_layout_set_width(layout, avail_w * PANGO_SCALE);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_text(layout, (text && *text) ? text : "(untitled)", -1);

    int text_w = 0, text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    double ty = ((double)logical_h - (double)text_h) * 0.5;
    if (ty < 0.0)
        ty = 0.0;

    if (fg_rgba) {
        cairo_set_source_rgba(cr, (double)fg_rgba[0], (double)fg_rgba[1],
                              (double)fg_rgba[2], (double)fg_rgba[3]);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    }
    cairo_move_to(cr, (double)pad_x, ty);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_destroy(cr);

    return wrap_cairo_surface(surf);
}

struct wlr_buffer *
comp_text_render_badge(const char *text, double scale,
                       const float fg_rgba[4], const float bg_rgba[4],
                       const float border_rgba[4], int pad_x, int pad_y,
                       int *out_logical_w, int *out_logical_h)
{
    if (!(scale > 0.0) || !isfinite(scale))
        scale = 1.0;

    /* Measure text on a 1x1 scratch surface first */
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *tmp_cr = cairo_create(tmp);
    PangoLayout *tmp_layout = pango_cairo_create_layout(tmp_cr);
    PangoFontDescription *desc = pango_font_description_from_string("Fira Code, monospace 10");
    pango_layout_set_font_description(tmp_layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_single_paragraph_mode(tmp_layout, TRUE);
    pango_layout_set_text(tmp_layout, text ? text : "", -1);

    int tw = 0, th = 0;
    pango_layout_get_pixel_size(tmp_layout, &tw, &th);
    g_object_unref(tmp_layout);
    cairo_destroy(tmp_cr);
    cairo_surface_destroy(tmp);

    int logical_w = tw + 2 * pad_x;
    int logical_h = th + 2 * pad_y;
    if (logical_w < 16) logical_w = 16;
    if (logical_h < 16) logical_h = 16;

    if (out_logical_w) *out_logical_w = logical_w;
    if (out_logical_h) *out_logical_h = logical_h;

    return comp_text_render_label(text ? text : "", logical_w, logical_h, scale,
                                  fg_rgba, bg_rgba, border_rgba, 1, pad_x);
}

typedef struct {
    struct wlr_scene_buffer *sbuf;
    struct wlr_buffer       *locked_buf;
    struct wl_listener       node_destroy;
} CompSceneBufPin;

static void
scene_buf_pin_handle_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    CompSceneBufPin *pin = wl_container_of(listener, pin, node_destroy);
    wl_list_remove(&pin->node_destroy.link);
    if (pin->sbuf && pin->sbuf->node.data == pin)
        pin->sbuf->node.data = NULL;
    if (pin->locked_buf) {
        wlr_buffer_unlock(pin->locked_buf);
        pin->locked_buf = NULL;
    }
    free(pin);
}

void
comp_scene_buffer_set_owned_buffer(struct wlr_scene_buffer *sbuf, struct wlr_buffer *buf)
{
    if (!sbuf)
        return;

    CompSceneBufPin *pin = sbuf->node.data;
    if (!pin && buf) {
        pin = calloc(1, sizeof(*pin));
        if (pin) {
            pin->sbuf                = sbuf;
            pin->node_destroy.notify = scene_buf_pin_handle_destroy;
            wl_signal_add(&sbuf->node.events.destroy, &pin->node_destroy);
            sbuf->node.data = pin;
        }
    }

    struct wlr_buffer *new_lock = buf ? wlr_buffer_lock(buf) : NULL;
    wlr_scene_buffer_set_buffer(sbuf, buf);

    if (pin) {
        if (pin->locked_buf)
            wlr_buffer_unlock(pin->locked_buf);
        pin->locked_buf = new_lock;
        if (!pin->locked_buf) {
            wl_list_remove(&pin->node_destroy.link);
            sbuf->node.data = NULL;
            free(pin);
        }
    } else if (new_lock) {
        wlr_buffer_unlock(new_lock);
    }
}

