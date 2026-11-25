#ifndef KAJO_VIEWER_SIDEBAR_H
#define KAJO_VIEWER_SIDEBAR_H

#include "viewer_model.h"
#include "../ide/ide_markdown.h"
#include <gtk/gtk.h>
#include <poppler.h>

G_BEGIN_DECLS

typedef struct {
    char     decoder[64];
    char     mime_type[64];
    int      width;
    int      height;
    guint64  file_size;
    int      zoom_pct;
    int      rotation_deg;
    gboolean flip_h;
    gboolean flip_v;
    gboolean is_animated;
    gboolean anim_playing;
    char     camera[128];
    char     exposure[128];
    char     colorspace[64];
    char     date_str[64];
} ViewerImageMeta;

typedef struct _ViewerSidebar ViewerSidebar;

typedef void (*ViewerSidebarPdfPageCb)(int page_index, gpointer user_data);
typedef void (*ViewerSidebarMdHeadingCb)(guint line_1based, guint total_lines, gpointer user_data);

ViewerSidebar *viewer_sidebar_new(ViewerSidebarPdfPageCb   pdf_cb,
                                  ViewerSidebarMdHeadingCb md_cb,
                                  gpointer                 user_data);
void           viewer_sidebar_free(ViewerSidebar *sb);

GtkWidget     *viewer_sidebar_get_widget(ViewerSidebar *sb);
void           viewer_sidebar_set_mode(ViewerSidebar *sb, ViewerDocKind kind);
void           viewer_sidebar_toggle_pdf_tab(ViewerSidebar *sb, gboolean show_pages);

void           viewer_sidebar_set_pdf_doc(ViewerSidebar *sb, PopplerDocument *doc);
void           viewer_sidebar_set_active_pdf_page(ViewerSidebar *sb, int page_index);

void           viewer_sidebar_set_markdown_headings(ViewerSidebar *sb,
                                                    GArray        *headings,
                                                    guint          total_lines,
                                                    gboolean       live_watch,
                                                    int            mermaid_count,
                                                    int            tasks_done,
                                                    int            tasks_total);
void           viewer_sidebar_set_active_md_line(ViewerSidebar *sb, guint line_1based);

void           viewer_sidebar_set_image_meta(ViewerSidebar *sb, const ViewerImageMeta *meta);

G_END_DECLS

#endif /* KAJO_VIEWER_SIDEBAR_H */