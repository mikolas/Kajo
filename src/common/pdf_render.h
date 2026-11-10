#ifndef KAJO_PDF_RENDER_H
#define KAJO_PDF_RENDER_H

#include <gdk/gdk.h>
#include <poppler.h>

G_BEGIN_DECLS

/* Renders one PDF page onto a white "paper" background and returns it as a
 * GdkTexture scaled so the page is about `target_height` px tall (scale is
 * clamped to 0.5x..2.5x). Returns NULL for an invalid page index or size.
 * Shared by kajo-files Quick Look and the kajo-ide Markdown PDF cards. */
GdkTexture *kajo_pdf_render_page_texture(PopplerDocument *doc, int page_index, int target_height);

G_END_DECLS

#endif /* KAJO_PDF_RENDER_H */
