#ifndef KAJO_VIEWER_MARKDOWN_H
#define KAJO_VIEWER_MARKDOWN_H

#include "../ide/ide_markdown.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
    VIEWER_MD_WIDTH_NARROW = 0,  /* 680px */
    VIEWER_MD_WIDTH_COMFORT,     /* 860px */
    VIEWER_MD_WIDTH_FULL,        /* 100% */
} ViewerMdWidthMode;

typedef struct _ViewerMarkdownView ViewerMarkdownView;

typedef void (*ViewerMdStateCb)(gpointer user_data);
typedef void (*ViewerMdOpenTargetCb)(const char *path, gpointer user_data);

ViewerMarkdownView *viewer_markdown_view_new(ViewerMdStateCb      state_cb,
                                             ViewerMdOpenTargetCb open_cb,
                                             gpointer             user_data);
void                viewer_markdown_view_free(ViewerMarkdownView *view);

GtkWidget          *viewer_markdown_view_get_widget(ViewerMarkdownView *view);
gboolean            viewer_markdown_view_load_file(ViewerMarkdownView *view,
                                                   GFile              *file,
                                                   double              initial_scroll_frac);
void                viewer_markdown_view_load_text(ViewerMarkdownView *view,
                                                   const char         *text,
                                                   const char         *virtual_path);

void                viewer_markdown_view_set_width_mode(ViewerMarkdownView *view,
                                                        ViewerMdWidthMode   mode);
ViewerMdWidthMode   viewer_markdown_view_get_width_mode(const ViewerMarkdownView *view);

void                viewer_markdown_view_step_font(ViewerMarkdownView *view, int delta_pct);
void                viewer_markdown_view_reset_font(ViewerMarkdownView *view);
int                 viewer_markdown_view_get_font_pct(const ViewerMarkdownView *view);

void                viewer_markdown_view_set_raw(ViewerMarkdownView *view, gboolean show_raw);
gboolean            viewer_markdown_view_get_raw(const ViewerMarkdownView *view);

void                viewer_markdown_view_set_search(ViewerMarkdownView *view,
                                                    const char         *query,
                                                    gboolean            case_sensitive);
void                viewer_markdown_view_step_search(ViewerMarkdownView *view, int delta);
int                 viewer_markdown_view_get_search_total(const ViewerMarkdownView *view);
int                 viewer_markdown_view_get_search_index(const ViewerMarkdownView *view);

void                viewer_markdown_view_scroll_to_line(ViewerMarkdownView *view,
                                                        guint               line_1based);
GArray             *viewer_markdown_view_get_headings(const ViewerMarkdownView *view);
guint               viewer_markdown_view_get_total_lines(const ViewerMarkdownView *view);
int                 viewer_markdown_view_count_mermaid(const ViewerMarkdownView *view);
gboolean            viewer_markdown_view_has_monitor(const ViewerMarkdownView *view);
void                viewer_markdown_view_open_in_ide(const ViewerMarkdownView *view);

G_END_DECLS

#endif /* KAJO_VIEWER_MARKDOWN_H */