#ifndef KAJO_PDF_RENDER_H
#define KAJO_PDF_RENDER_H

#include <gdk/gdk.h>
#include <poppler.h>
#include <stddef.h>

G_BEGIN_DECLS

typedef enum {
    KAJO_PDF_TONE_PAPER = 0,
    KAJO_PDF_TONE_SEPIA,
    KAJO_PDF_TONE_DARK,
} KajoPdfPaperTone;

/* Highlight rectangle in PDF page coordinates (bottom-left origin, matching poppler_page_find_text). */
typedef struct {
    double   x1;
    double   y1;
    double   x2;
    double   y2;
    gboolean is_active;
} KajoPdfHighlightRect;

/* Renders one PDF page onto a white "paper" background and returns it as a
 * GdkTexture scaled so the page is about `target_height` px tall (scale is
 * clamped to 0.5x..2.5x). Returns NULL for an invalid page index or size.
 * Shared by kajo-files Quick Look and the kajo-ide Markdown PDF cards. */
GdkTexture *kajo_pdf_render_page_texture(PopplerDocument *doc, int page_index, int target_height);

/* Renders one PDF page at an explicit scale, clockwise rotation (0/90/180/270),
 * paper tone, and optional search match highlights. */
GdkTexture *kajo_pdf_render_page_scaled(PopplerDocument           *doc,
                                        int                        page_index,
                                        double                     scale,
                                        int                        rotation_deg,
                                        KajoPdfPaperTone           tone,
                                        const KajoPdfHighlightRect *highlights,
                                        size_t                     n_highlights);

G_END_DECLS

#endif /* KAJO_PDF_RENDER_H */