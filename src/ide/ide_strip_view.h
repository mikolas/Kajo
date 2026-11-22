#ifndef KAJO_IDE_STRIP_VIEW_H
#define KAJO_IDE_STRIP_VIEW_H

#include "ide_buffer.h"
#include "ide_lsp.h"
#include "ide_strip_graph.h"
#include "ide_strip_layout.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _IdeStripView IdeStripView;

typedef IdeBuffer *(*IdeStripOpenBufferCb)(const char *abs_path,
                                           guint       line_1based,
                                           gboolean    focus_buffer,
                                           gpointer    user_data);
typedef GPtrArray *(*IdeStripGetBuffersCb)(gpointer user_data);
typedef void (*IdeStripStatusCb)(const char *toast_msg, gpointer user_data);
typedef void (*IdeStripModeExitCb)(gpointer user_data);
typedef void (*IdeStripBpCb)(IdeBuffer *buf, guint line_1based, gboolean enabled, gpointer user_data);
typedef void (*IdeStripCondBpCb)(IdeBuffer *buf, guint line_1based, gpointer user_data);

IdeStripView *ide_strip_view_new(const char           *workspace_root,
                                 IdeLspManager        *lsp,
                                 IdeStripOpenBufferCb  open_buf_cb,
                                 IdeStripGetBuffersCb  get_bufs_cb,
                                 IdeStripStatusCb      status_cb,
                                 IdeStripModeExitCb    exit_cb,
                                 gpointer              user_data);
void          ide_strip_view_free(IdeStripView *sv);

GtkWidget    *ide_strip_view_get_widget(IdeStripView *sv);
GtkWidget    *ide_strip_view_get_d0_source_view(IdeStripView *sv);
GtkWidget    *ide_strip_view_get_filter_entry(IdeStripView *sv);
GtkWidget    *ide_strip_view_get_overview_entry(IdeStripView *sv);

void          ide_strip_view_set_gutter_callbacks(IdeStripView    *sv,
                                                  IdeStripBpCb     bp_cb,
                                                  IdeStripCondBpCb cond_bp_cb,
                                                  gpointer         user_data);

void          ide_strip_view_apply_editor_settings(IdeStripView *sv,
                                                   const char   *font_family,
                                                   int           font_size_px,
                                                   int           tab_width,
                                                   gboolean      insert_spaces,
                                                   gboolean      word_wrap,
                                                   gboolean      show_margin_guide,
                                                   gboolean      highlight_current_line,
                                                   gboolean      show_line_numbers,
                                                   gboolean      relative_line_numbers,
                                                   gboolean      magnetic_follow);

void          ide_strip_view_set_workspace_root(IdeStripView *sv, const char *workspace_root);
void          ide_strip_view_set_active(IdeStripView *sv, gboolean active);
gboolean      ide_strip_view_is_active(const IdeStripView *sv);

void          ide_strip_view_bind_buffer(IdeStripView *sv,
                                         IdeBuffer    *buf,
                                         guint         cursor_line_1based,
                                         gboolean      clear_ancestors);
void          ide_strip_view_notify_buffer_saved(IdeStripView *sv, IdeBuffer *buf);
void          ide_strip_view_notify_buffer_changed(IdeStripView *sv, IdeBuffer *buf);
void          ide_strip_view_notify_buffer_closed(IdeStripView *sv, IdeBuffer *buf);

void          ide_strip_view_set_lens(IdeStripView *sv, IdeStripLens lens);
void          ide_strip_view_toggle_zen(IdeStripView *sv);
void          ide_strip_view_toggle_overview(IdeStripView *sv);
gboolean      ide_strip_view_is_overview(const IdeStripView *sv);
void          ide_strip_view_toggle_full_file(IdeStripView *sv);
void          ide_strip_view_pin_active_d1(IdeStripView *sv);
void          ide_strip_view_unpin(IdeStripView *sv);
void          ide_strip_view_resync_d1(IdeStripView *sv);

void          ide_strip_view_step_d1(IdeStripView *sv, int delta);
void          ide_strip_view_step_d2(IdeStripView *sv, int delta);
void          ide_strip_view_promote_active_d1(IdeStripView *sv);
void          ide_strip_view_promote_active_d2(IdeStripView *sv);
void          ide_strip_view_surface_one(IdeStripView *sv);
void          ide_strip_view_branch_trail(IdeStripView *sv);
void          ide_strip_view_switch_trail(IdeStripView *sv, int delta);
void          ide_strip_view_focus_filter(IdeStripView *sv);
void          ide_strip_view_focus_column(IdeStripView *sv, int delta);

char         *ide_strip_view_get_status_summary(const IdeStripView *sv);

G_END_DECLS

#endif /* KAJO_IDE_STRIP_VIEW_H */
