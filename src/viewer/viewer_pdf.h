#ifndef KAJO_VIEWER_PDF_H
#define KAJO_VIEWER_PDF_H

#include "viewer_model.h"
#include "../common/pdf_render.h"
#include <gtk/gtk.h>
#include <poppler.h>

G_BEGIN_DECLS

typedef enum {
    VIEWER_PDF_LOAD_OK = 0,
    VIEWER_PDF_LOAD_ENCRYPTED,
    VIEWER_PDF_LOAD_ERROR,
} ViewerPdfLoadResult;

typedef struct _ViewerPdfView ViewerPdfView;

typedef void (*ViewerPdfStateCb)(gpointer user_data);
typedef void (*ViewerPdfNavigateUriCb)(const char *uri, gpointer user_data);

ViewerPdfView *viewer_pdf_view_new(ViewerPdfStateCb       state_cb,
                                   ViewerPdfNavigateUriCb nav_cb,
                                   gpointer               user_data);
void           viewer_pdf_view_free(ViewerPdfView *view);

GtkWidget     *viewer_pdf_view_get_widget(ViewerPdfView *view);

ViewerPdfLoadResult viewer_pdf_view_load_file(ViewerPdfView *view,
                                              GFile         *file,
                                              const char    *password,
                                              GError       **error);
PopplerDocument    *viewer_pdf_view_get_document(ViewerPdfView *view);

void           viewer_pdf_view_jump_to_page(ViewerPdfView *view, int page_index);
void           viewer_pdf_view_step_page(ViewerPdfView *view, int delta);
int            viewer_pdf_view_get_current_page(const ViewerPdfView *view);
int            viewer_pdf_view_get_num_pages(const ViewerPdfView *view);

void           viewer_pdf_view_set_zoom_fit(ViewerPdfView *view, ViewerFitMode fit);
void           viewer_pdf_view_step_zoom(ViewerPdfView *view, int direction);
double         viewer_pdf_view_get_zoom(const ViewerPdfView *view);
ViewerFitMode  viewer_pdf_view_get_fit_mode(const ViewerPdfView *view);

void           viewer_pdf_view_set_spread(ViewerPdfView *view, gboolean spread);
gboolean       viewer_pdf_view_get_spread(const ViewerPdfView *view);

void           viewer_pdf_view_rotate(ViewerPdfView *view, int delta_deg);
int            viewer_pdf_view_get_rotation(const ViewerPdfView *view);

void           viewer_pdf_view_set_tone(ViewerPdfView *view, KajoPdfPaperTone tone);
KajoPdfPaperTone viewer_pdf_view_get_tone(const ViewerPdfView *view);

void           viewer_pdf_view_set_search(ViewerPdfView *view,
                                          const char    *query,
                                          gboolean       case_sensitive);
void           viewer_pdf_view_step_search(ViewerPdfView *view, int delta);
int            viewer_pdf_view_get_search_total(const ViewerPdfView *view);
int            viewer_pdf_view_get_search_index(const ViewerPdfView *view);

gboolean       viewer_pdf_view_copy_selection(ViewerPdfView *view);

G_END_DECLS

#endif /* KAJO_VIEWER_PDF_H */