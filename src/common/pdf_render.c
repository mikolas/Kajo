#include "pdf_render.h"

#include <cairo.h>

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
    if (pw <= 1.0 || ph <= 1.0) {
        g_object_unref(page);
        return NULL;
    }

    double scale = CLAMP((double)target_height / ph, 0.5, 2.5);
    int w = MAX(1, (int)(pw * scale));
    int h = MAX(1, (int)(ph * scale));

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface);

    /* Crisp white paper background */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);

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
    g_object_unref(page);
    return tex;
}
