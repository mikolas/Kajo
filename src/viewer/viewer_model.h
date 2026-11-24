#ifndef KAJO_VIEWER_MODEL_H
#define KAJO_VIEWER_MODEL_H

#include <glib.h>
#include <stddef.h>

G_BEGIN_DECLS

typedef enum {
    VIEWER_KIND_NONE = 0,
    VIEWER_KIND_PDF,
    VIEWER_KIND_MARKDOWN,
    VIEWER_KIND_IMAGE,
    VIEWER_KIND_SVG,
} ViewerDocKind;

typedef enum {
    VIEWER_FIT_WIDTH = 0,
    VIEWER_FIT_PAGE,
    VIEWER_FIT_CONTAIN,
    VIEWER_FIT_FILL,
    VIEWER_FIT_ACTUAL,
} ViewerFitMode;

typedef struct {
    char  *path;
    int    page_index;
    double scroll_frac;
} ViewerHistoryEntry;

typedef struct _ViewerHistory ViewerHistory;

/* Pure file classification (supports extension and optional MIME type fallback) */
ViewerDocKind viewer_model_classify_path(const char *path, const char *mime_type);
gboolean      viewer_model_is_image_kind(ViewerDocKind kind);

/* Natural-sorted list of sibling image paths (GPtrArray of gchar*, caller owns) */
GPtrArray    *viewer_model_scan_sibling_images(const char *file_path, int *out_current_idx);

/* In-window navigation history stack (truncate forward branch on push) */
ViewerHistory            *viewer_history_new(void);
void                      viewer_history_free(ViewerHistory *hist);
void                      viewer_history_push(ViewerHistory *hist,
                                              const char    *path,
                                              int            page_index,
                                              double         scroll_frac);
void                      viewer_history_update_current(ViewerHistory *hist,
                                                        int            page_index,
                                                        double         scroll_frac);
gboolean                  viewer_history_can_back(const ViewerHistory *hist);
gboolean                  viewer_history_can_forward(const ViewerHistory *hist);
const ViewerHistoryEntry *viewer_history_back(ViewerHistory *hist);
const ViewerHistoryEntry *viewer_history_forward(ViewerHistory *hist);
const ViewerHistoryEntry *viewer_history_current(const ViewerHistory *hist);

/* Zoom and formatting helpers */
double viewer_model_step_zoom(double current, int direction, double min_zoom, double max_zoom);
double viewer_model_compute_fit_scale(double        content_w,
                                      double        content_h,
                                      double        view_w,
                                      double        view_h,
                                      ViewerFitMode mode);
void   viewer_model_format_bytes(guint64 bytes, char *out_buf, size_t buf_len);

G_END_DECLS

#endif /* KAJO_VIEWER_MODEL_H */