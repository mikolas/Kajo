#include "pdf_render.h"

#include <cairo.h>

G_LOCK_DEFINE_STATIC(pdf_render_lock);

static void
apply_dark_reading_tone(cairo_surface_t *surface, int w, int h)
{
    cairo_surface_flush(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    if (!data)
        return;

    /* Map white paper (#FFFFFF) -> #17191D dark surface and black ink (#000000) -> #EAEAEA
     * per-channel while preserving hue polarity minimally. */
    for (int y = 0; y < h; y++) {
        guint32 *row = (guint32 *)(data + (size_t)y * (size_t)stride);
        for (int x = 0; x < w; x++) {
            guint32 px = row[x];
            guint32 a = (px >> 24) & 0xFFu;
            guint32 r = (px >> 16) & 0xFFu;
            guint32 g = (px >> 8) & 0xFFu;
            guint32 b = px & 0xFFu;
            guint32 nr = 23u + ((255u - r) * 211u) / 255u;
            guint32 ng = 25u + ((255u - g) * 209u) / 255u;
            guint32 nb = 29u + ((255u - b) * 205u) / 255u;
            row[x] = (a << 24) | (nr << 16) | (ng << 8) | nb;
        }
    }
    cairo_surface_mark_dirty(surface);
}

GdkTexture *
kajo_pdf_render_page_scaled(PopplerDocument            *doc,
                            int                         page_index,
                            double                      scale,
                            int                         rotation_deg,
                            KajoPdfPaperTone            tone,
                            const KajoPdfHighlightRect *highlights,
                            size_t                      n_highlights)
{
    if (!doc || page_index < 0)
        return NULL;

    G_LOCK(pdf_render_lock);
    if (page_index >= poppler_document_get_n_pages(doc)) {
        G_UNLOCK(pdf_render_lock);
        return NULL;
    }

    PopplerPage *page = poppler_document_get_page(doc, page_index);
    if (!page) {
        G_UNLOCK(pdf_render_lock);
        return NULL;
    }

    double pw = 0.0, ph = 0.0;
    poppler_page_get_size(page, &pw, &ph);
    if (pw <= 1.0 || ph <= 1.0) {
        g_object_unref(page);
        G_UNLOCK(pdf_render_lock);
        return NULL;
    }

    double clamped_scale = CLAMP(scale, 0.1, 8.0);
    int rot = ((rotation_deg % 360) + 360) % 360;
    rot = (rot / 90) * 90;

    double out_pw = (rot == 90 || rot == 270) ? ph : pw;
    double out_ph = (rot == 90 || rot == 270) ? pw : ph;
    int w = MAX(1, (int)(out_pw * clamped_scale + 0.5));
    int h = MAX(1, (int)(out_ph * clamped_scale + 0.5));

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface);

    /* Render onto white background first (for DARK mode we tone-invert before drawing highlights). */
    if (tone == KAJO_PDF_TONE_SEPIA) {
        cairo_set_source_rgb(cr, 0.957, 0.925, 0.847); /* #f4ecd8 */
    } else {
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    }
    cairo_paint(cr);

    cairo_save(cr);
    if (rot == 90) {
        cairo_translate(cr, (double)w, 0.0);
        cairo_rotate(cr, G_PI_2);
    } else if (rot == 180) {
        cairo_translate(cr, (double)w, (double)h);
        cairo_rotate(cr, G_PI);
    } else if (rot == 270) {
        cairo_translate(cr, 0.0, (double)h);
        cairo_rotate(cr, -G_PI_2);
    }
    cairo_scale(cr, clamped_scale, clamped_scale);
    poppler_page_render(page, cr);
    cairo_restore(cr);
    g_object_unref(page);
    G_UNLOCK(pdf_render_lock);

    if (tone == KAJO_PDF_TONE_DARK) {
        apply_dark_reading_tone(surface, w, h);
    }

    if (highlights && n_highlights > 0) {
        cairo_save(cr);
        if (rot == 90) {
            cairo_translate(cr, (double)w, 0.0);
            cairo_rotate(cr, G_PI_2);
        } else if (rot == 180) {
            cairo_translate(cr, (double)w, (double)h);
            cairo_rotate(cr, G_PI);
        } else if (rot == 270) {
            cairo_translate(cr, 0.0, (double)h);
            cairo_rotate(cr, -G_PI_2);
        }
        cairo_scale(cr, clamped_scale, clamped_scale);
        for (size_t i = 0; i < n_highlights; i++) {
            double rx = highlights[i].x1;
            double ry = ph - highlights[i].y2;
            double rw = highlights[i].x2 - highlights[i].x1;
            double rh = highlights[i].y2 - highlights[i].y1;
            if (highlights[i].is_active) {
                cairo_set_source_rgba(cr, 0.0, 0.471, 0.831, 0.42);
            } else {
                cairo_set_source_rgba(cr, 1.0, 0.725, 0.0, 0.35);
            }
            cairo_rectangle(cr, rx, ry, rw, rh);
            cairo_fill(cr);
            if (highlights[i].is_active) {
                cairo_set_source_rgba(cr, 0.0, 0.847, 0.941, 0.95);
                cairo_set_line_width(cr, 1.0 / clamped_scale);
                cairo_rectangle(cr, rx, ry, rw, rh);
                cairo_stroke(cr);
            }
        }
        cairo_restore(cr);
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);

    int stride = cairo_image_surface_get_stride(surface);
    const unsigned char *data = cairo_image_surface_get_data(surface);
    GBytes *bytes = g_bytes_new(data, (gsize)stride * (gsize)h);

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    GdkMemoryFormat fmt = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
#else
    GdkMemoryFormat fmt = GDK_MEMORY_A8R8G8B8_PREMULTIPLIED;
#endif

    GdkTexture *tex = gdk_memory_texture_new(w, h, fmt, bytes, (gsize)stride);

    g_bytes_unref(bytes);
    cairo_surface_destroy(surface);
    return tex;
}

GdkTexture *
kajo_pdf_render_page_texture(PopplerDocument *doc, int page_index, int target_height)
{
    if (!doc || page_index < 0 || page_index >= poppler_document_get_n_pages(doc))
        return NULL;

    PopplerPage *page = poppler_document_get_page(doc, page_index);
    if (!page)
        return NULL;

    double pw = 0.0, ph = 0.0;
    poppler_page_get_size(page, &pw, &ph);
    g_object_unref(page);
    if (pw <= 1.0 || ph <= 1.0)
        return NULL;

    double scale = CLAMP((double)target_height / ph, 0.5, 2.5);
    return kajo_pdf_render_page_scaled(doc, page_index, scale, 0, KAJO_PDF_TONE_PAPER, NULL, 0);
}