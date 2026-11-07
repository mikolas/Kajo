#ifndef KAJO_FILES_WINDOW_PRIVATE_H
#define KAJO_FILES_WINDOW_PRIVATE_H

/* Internal to src/files/: the KajoFilesWindow instance struct and the helpers
 * shared between files_window.c and its split-out modules. Not a public API. */

#include "files_window.h"
#include "files_model.h"
#include "files_ops.h"
#include "files_sidebar.h"
#include "files_quicklook.h"
#include "../compositor/compositor.h"
#include <gio/gdesktopappinfo.h>
#include <math.h>
#include <string.h>

G_BEGIN_DECLS

typedef enum {
    PROMPT_NONE = 0,
    PROMPT_MKDIR,
    PROMPT_NEW_FILE,
    PROMPT_RENAME,
    PROMPT_DELETE_PERM,
} InlinePromptKind;

struct _KajoFilesWindow {
    GtkApplicationWindow parent_instance;

    /* Blueprint Template Children */
    GtkOverlay *root_overlay;
    GtkWidget *btn_sidebar_toggle;
    GtkWidget *btn_back;
    GtkWidget *btn_forward;
    GtkWidget *btn_up;
    GtkWidget *btn_reload;
    GtkStack *location_stack;
    GtkScrolledWindow *breadcrumb_scroll;
    GtkWidget *breadcrumb_box;
    GtkWidget *btn_edit_uri;
    GtkWidget *uri_entry;
    GtkWidget *search_entry;
    GtkWidget *btn_view_grid;
    GtkWidget *btn_view_list;
    GtkWidget *btn_inspector;
    GtkWidget *btn_float;
    GtkWidget *btn_close;

    GtkWidget *current_uri_badge;
    GtkWidget *btn_new_folder;
    GtkWidget *btn_new_file;
    GtkWidget *btn_paste;
    GtkWidget *btn_hidden;
    GtkWidget *btn_sort;
    GtkPopover *sort_popover;
    GtkWidget *btn_sort_field_name;
    GtkWidget *btn_sort_field_modified;
    GtkWidget *btn_sort_field_size;
    GtkWidget *btn_sort_field_type;
    GtkWidget *btn_sort_asc;
    GtkWidget *btn_sort_desc;
    GtkWidget *btn_sort_folders_first;
    GtkWidget *btn_terminal;

    GtkWidget *sidebar_vbox;
    GtkListBox *sidebar_list;
    GtkWidget *auth_banner_revealer;
    GtkWidget *connect_banner_revealer;
    GtkWidget *trash_banner_revealer;
    GtkWidget *prompt_banner_revealer;

    GtkStack *view_stack;
    GtkGridView *grid_view;
    GtkWidget *col_btn_name;
    GtkWidget *col_btn_size;
    GtkWidget *col_btn_type;
    GtkWidget *col_btn_modified;
    GtkListView *details_listview;
    GtkWidget *empty_label;
    GtkWidget *inspector_vbox;

    GtkWidget *status_left_label;
    GtkWidget *status_right_label;
    GtkWidget *fs_usage_bar;

    /* Inline Banner Sub-Widgets */
    GtkWidget *connect_entry;
    GtkWidget *trash_summary_lbl;
    GtkWidget *prompt_title_lbl;
    GtkWidget *prompt_entry;
    GtkWidget *prompt_submit_btn;
    InlinePromptKind active_prompt;

    /* Context Menu Popover Sub-Widgets */
    GtkWidget *context_popover;
    GtkWidget *ctx_stack;
    GtkWidget *ctx_hdr_icon;
    GtkWidget *ctx_hdr_title_lbl;
    GtkWidget *ctx_hdr_badge_lbl;
    GtkWidget *ctx_item_box;
    GtkWidget *ctx_bg_box;
    GtkWidget *ctx_btn_open_with;
    GtkWidget *ctx_btn_extract;
    GtkWidget *ctx_btn_item_terminal;
    GtkWidget *ctx_btn_item_newwin;
    GtkWidget *ctx_btn_restore_trash;
    GtkWidget *ctx_btn_move_trash;
    GtkWidget *ctx_btn_bg_paste;
    GtkWidget *ctx_ow_mime_lbl;
    GtkWidget *ctx_ow_list_box;
    GtkWidget *ctx_ow_default_btn;
    gboolean open_with_set_default;

    /* Breadcrumb Subfolder Jump Popover */
    GtkWidget *breadcrumb_popover;
    GtkWidget *bc_pop_hdr_lbl;
    GtkWidget *bc_pop_list_box;
    GCancellable *bc_pop_cancel;
    char *bc_pop_target_uri;
    gboolean pending_breadcrumb_scroll_end;
    guint breadcrumb_scroll_idle_id;

    /* Subsystems */
    KajoFilesSidebar *sidebar;
    KajoQuickLook *quicklook;

    /* State */
    GFile *current_dir;
    GCancellable *enum_cancel;
    GCancellable *ops_cancel;
    GFileMonitor *dir_monitor;
    guint monitor_debounce_id;

    GPtrArray *all_items;      /* KajoFileItem* */
    GPtrArray *visible_items;  /* KajoFileItem* (filtered & sorted) */
    GListStore *visible_store; /* Recycling view model backing GridView & ListView */
    GtkMultiSelection *selection_model;
    gboolean suppress_selection_sync;
    KajoFileItem *selected_item;

    GPtrArray *history;        /* char* URIs */
    int history_pos;
    gboolean navigating_history;

    KajoFileViewMode view_mode;
    KajoFileSortField sort_field;
    gboolean sort_ascending;
    gboolean sort_folders_first;
    gboolean show_hidden;
    gboolean inspector_visible;
    gboolean is_narrow;
};


GType kajo_files_window_get_type(void);

/* Shared helpers (defined in files_window.c) */
void files_window_set_status_message(KajoFilesWindow *self, const char *msg, gboolean is_error);
void files_window_reload_directory(KajoFilesWindow *self);
void files_window_rebuild_filtered_view(KajoFilesWindow *self);

/* Helpers shared across the window modules (defined in files_window.c unless noted). */
void     files_window_activate_file_item(KajoFilesWindow *self, KajoFileItem *item);
void     files_window_set_selected_item_by_index(KajoFilesWindow *self, int idx);
void     files_window_show_inline_prompt(KajoFilesWindow *self, InlinePromptKind kind);
void     files_window_trash_selected_item(KajoFilesWindow *self);
void     files_window_set_widget_active_class(GtkWidget *w, gboolean active);
gpointer files_window_op_weak_new(KajoFilesWindow *self);
void     files_window_on_file_op_completed(gboolean success, const char *message, GError *error, gpointer user_data);
void     on_trash_restore_clicked(GtkButton *btn, gpointer user_data);
void     on_btn_inspector_clicked(GtkButton *btn, gpointer user_data);
void     on_btn_paste_clicked(GtkButton *btn, gpointer user_data);
void     on_btn_terminal_clicked(GtkButton *btn, gpointer user_data);
void     on_btn_hidden_clicked(GtkButton *btn, gpointer user_data);

void     on_btn_back_clicked(GtkButton *btn, gpointer user_data);
void     on_btn_forward_clicked(GtkButton *btn, gpointer user_data);
void     on_btn_up_clicked(GtkButton *btn, gpointer user_data);
void     on_btn_edit_uri_clicked(GtkButton *btn, gpointer user_data);
void     on_prompt_cancel_clicked(GtkButton *btn, gpointer user_data);
void     on_connect_cancel_clicked(GtkButton *btn, gpointer user_data);
int        files_window_get_selected_item_index(KajoFilesWindow *self);
GPtrArray *files_window_get_selected_items(KajoFilesWindow *self);
void       files_window_select_all(KajoFilesWindow *self);
void       files_window_set_view_mode(KajoFilesWindow *self, KajoFileViewMode mode);

/* Defined in files_window_keys.c */
gboolean files_window_on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                     GdkModifierType state, gpointer user_data);

/* Defined in files_window_ctxmenu.c */
void files_window_build_context_menu_popover(KajoFilesWindow *self);
void files_window_open_context_menu_for_selection(KajoFilesWindow *self);
void files_window_copy_selected_item_path(KajoFilesWindow *self);
void on_view_stack_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);

G_END_DECLS

#endif /* KAJO_FILES_WINDOW_PRIVATE_H */
