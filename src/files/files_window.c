#include "files_window.h"
#include "files_model.h"
#include "files_ops.h"
#include "files_sidebar.h"
#include "files_quicklook.h"
#include "../compositor/compositor.h"
#include <string.h>

typedef enum {
    PROMPT_NONE = 0,
    PROMPT_MKDIR,
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

    /* Subsystems */
    KajoFilesSidebar *sidebar;
    KajoQuickLook *quicklook;

    /* State */
    GFile *current_dir;
    GCancellable *enum_cancel;
    GFileMonitor *dir_monitor;
    guint monitor_debounce_id;

    GPtrArray *all_items;      /* KajoFileItem* */
    GPtrArray *visible_items;  /* KajoFileItem* (filtered & sorted) */
    GListStore *visible_store; /* Recycling view model backing GridView & ListView */
    GtkSingleSelection *selection_model;
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

G_DEFINE_TYPE(KajoFilesWindow, kajo_files_window, GTK_TYPE_APPLICATION_WINDOW)

static void reload_current_directory(KajoFilesWindow *self);
static void rebuild_filtered_view(KajoFilesWindow *self);
static void set_selected_item_by_index(KajoFilesWindow *self, int idx);
static void activate_file_item(KajoFilesWindow *self, KajoFileItem *item);

/* ─── Status Bar & Capacity Helpers ─── */

static void
set_status_message(KajoFilesWindow *self, const char *msg, gboolean is_error)
{
    if (!self || !self->status_left_label) return;
    gtk_label_set_text(GTK_LABEL(self->status_left_label), msg ? msg : "");
    if (is_error)
        gtk_widget_add_css_class(self->status_left_label, "files-status-error");
    else
        gtk_widget_remove_css_class(self->status_left_label, "files-status-error");
}

static void
update_status_and_capacity_bar(KajoFilesWindow *self)
{
    if (!self) return;

    guint total_vis = self->visible_items ? self->visible_items->len : 0;
    guint dirs = 0, files = 0;

    for (guint i = 0; i < total_vis; i++) {
        KajoFileItem *it = g_ptr_array_index(self->visible_items, i);
        if (kajo_file_item_is_dir(it))
            dirs++;
        else
            files++;
    }

    if (self->selected_item) {
        g_autofree char *sel_sz = kajo_files_format_size(
            kajo_file_item_get_size(self->selected_item),
            kajo_file_item_is_dir(self->selected_item)
        );
        g_autofree char *left = g_strdup_printf(
            "%u ITEMS (%u FOLDERS, %u FILES)  |  SELECTED: %s (%s)",
            total_vis, dirs, files,
            kajo_file_item_get_display_name(self->selected_item),
            sel_sz
        );
        set_status_message(self, left, FALSE);
    } else {
        g_autofree char *left = g_strdup_printf(
            "%u ITEMS (%u FOLDERS, %u FILES)",
            total_vis, dirs, files
        );
        set_status_message(self, left, FALSE);
    }

    if (self->current_dir) {
        g_autoptr(GFileInfo) fs_info = g_file_query_filesystem_info(
            self->current_dir,
            G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
            NULL,
            NULL
        );
        if (fs_info && g_file_info_has_attribute(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
            guint64 total_b = g_file_info_get_attribute_uint64(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
            guint64 free_b = g_file_info_get_attribute_uint64(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
            if (total_b > 0) {
                g_autofree char *free_s = kajo_files_format_size(free_b, FALSE);
                g_autofree char *tot_s = kajo_files_format_size(total_b, FALSE);
                g_autofree char *right = g_strdup_printf("FREE: %s / %s", free_s, tot_s);
                gtk_label_set_text(GTK_LABEL(self->status_right_label), right);

                double frac = (double)(total_b - free_b) / (double)total_b;
                gtk_level_bar_set_value(GTK_LEVEL_BAR(self->fs_usage_bar), CLAMP(frac, 0.0, 1.0));
                gtk_widget_set_visible(self->fs_usage_bar, TRUE);
                return;
            }
        }
    }

    gtk_label_set_text(GTK_LABEL(self->status_right_label), "GVFS VIRTUAL FS");
    gtk_widget_set_visible(self->fs_usage_bar, FALSE);
}

static void
on_file_op_completed(gboolean success, const char *message, GError *error, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!KAJO_IS_FILES_WINDOW(self)) return;

    if (!success) {
        set_status_message(self, error ? error->message : "Operation failed", TRUE);
    } else if (message) {
        set_status_message(self, message, FALSE);
    }

    reload_current_directory(self);
    if (self->sidebar)
        kajo_files_sidebar_refresh(self->sidebar);
}

/* ─── Contextual Banners (Connect Server / Trash / Inline Prompts) ─── */

static void
on_connect_submit_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *uri = gtk_editable_get_text(GTK_EDITABLE(self->connect_entry));
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->connect_banner_revealer), FALSE);
    if (uri && uri[0] != '\0') {
        kajo_files_window_navigate_uri(self, uri);
    }
}

static void
on_connect_cancel_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->connect_banner_revealer), FALSE);
}

static void
build_connect_banner(KajoFilesWindow *self)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "files-prompt-bar");
    gtk_widget_add_css_class(card, "files-context-banner");

    GtkWidget *lbl = gtk_label_new("🌐 CONNECT TO SERVER (smb://, sftp://, ftp://, davs://, mtp://, onedrive://)");
    gtk_widget_add_css_class(lbl, "heading");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(card), lbl);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->connect_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->connect_entry), "smb://server.local/share or sftp://user@host/path");
    gtk_widget_set_hexpand(self->connect_entry, TRUE);
    g_signal_connect(self->connect_entry, "activate", G_CALLBACK(on_connect_submit_clicked), self);

    GtkWidget *btn_ok = gtk_button_new_with_label("CONNECT");
    gtk_widget_add_css_class(btn_ok, "action-btn-primary");
    g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_connect_submit_clicked), self);

    GtkWidget *btn_cancel = gtk_button_new_with_label("CANCEL");
    gtk_widget_add_css_class(btn_cancel, "flat");
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_connect_cancel_clicked), self);

    gtk_box_append(GTK_BOX(hbox), self->connect_entry);
    gtk_box_append(GTK_BOX(hbox), btn_ok);
    gtk_box_append(GTK_BOX(hbox), btn_cancel);
    gtk_box_append(GTK_BOX(card), hbox);

    gtk_revealer_set_child(GTK_REVEALER(self->connect_banner_revealer), card);
}

static void
on_trash_restore_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self->selected_item) {
        set_status_message(self, "Select an item in Trash to restore", TRUE);
        return;
    }
    kajo_files_op_restore_from_trash_async(self->selected_item, on_file_op_completed, self);
}

static void
on_trash_empty_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    kajo_files_op_empty_trash_async(on_file_op_completed, self);
}

static void
build_trash_banner(KajoFilesWindow *self)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(card, "files-trash-bar");
    gtk_widget_add_css_class(card, "files-context-banner");

    self->trash_summary_lbl = gtk_label_new("🗑 TRASH // RECYCLE BIN");
    gtk_widget_add_css_class(self->trash_summary_lbl, "heading");
    gtk_widget_set_hexpand(self->trash_summary_lbl, TRUE);
    gtk_widget_set_halign(self->trash_summary_lbl, GTK_ALIGN_START);

    GtkWidget *btn_restore = gtk_button_new_with_label("↺ RESTORE SELECTED");
    gtk_widget_add_css_class(btn_restore, "flat");
    g_signal_connect(btn_restore, "clicked", G_CALLBACK(on_trash_restore_clicked), self);

    GtkWidget *btn_empty = gtk_button_new_with_label("🔥 EMPTY TRASH");
    gtk_widget_add_css_class(btn_empty, "flat");
    gtk_widget_add_css_class(btn_empty, "action-btn-danger");
    g_signal_connect(btn_empty, "clicked", G_CALLBACK(on_trash_empty_clicked), self);

    gtk_box_append(GTK_BOX(card), self->trash_summary_lbl);
    gtk_box_append(GTK_BOX(card), btn_restore);
    gtk_box_append(GTK_BOX(card), btn_empty);

    gtk_revealer_set_child(GTK_REVEALER(self->trash_banner_revealer), card);
}

static void
on_prompt_submit_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    InlinePromptKind kind = self->active_prompt;
    self->active_prompt = PROMPT_NONE;
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), FALSE);

    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->prompt_entry));

    if (kind == PROMPT_MKDIR && self->current_dir) {
        kajo_files_op_mkdir_async(self->current_dir, text, on_file_op_completed, self);
    } else if (kind == PROMPT_RENAME && self->selected_item) {
        kajo_files_op_rename_async(kajo_file_item_get_file(self->selected_item), text, on_file_op_completed, self);
    } else if (kind == PROMPT_DELETE_PERM && self->selected_item) {
        g_autoptr(GPtrArray) arr = g_ptr_array_new_with_free_func(g_object_unref);
        g_ptr_array_add(arr, g_object_ref(self->selected_item));
        kajo_files_op_delete_permanent_async(arr, on_file_op_completed, self);
    }
}

static void
on_prompt_cancel_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->active_prompt = PROMPT_NONE;
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), FALSE);
}

static void
build_prompt_banner(KajoFilesWindow *self)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(card, "files-prompt-bar");
    gtk_widget_add_css_class(card, "files-context-banner");

    self->prompt_title_lbl = gtk_label_new("ACTION:");
    gtk_widget_add_css_class(self->prompt_title_lbl, "heading");

    self->prompt_entry = gtk_entry_new();
    gtk_widget_set_hexpand(self->prompt_entry, TRUE);
    g_signal_connect(self->prompt_entry, "activate", G_CALLBACK(on_prompt_submit_clicked), self);

    self->prompt_submit_btn = gtk_button_new_with_label("CONFIRM");
    gtk_widget_add_css_class(self->prompt_submit_btn, "action-btn-primary");
    g_signal_connect(self->prompt_submit_btn, "clicked", G_CALLBACK(on_prompt_submit_clicked), self);

    GtkWidget *btn_cancel = gtk_button_new_with_label("CANCEL");
    gtk_widget_add_css_class(btn_cancel, "flat");
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_prompt_cancel_clicked), self);

    gtk_box_append(GTK_BOX(card), self->prompt_title_lbl);
    gtk_box_append(GTK_BOX(card), self->prompt_entry);
    gtk_box_append(GTK_BOX(card), self->prompt_submit_btn);
    gtk_box_append(GTK_BOX(card), btn_cancel);

    gtk_revealer_set_child(GTK_REVEALER(self->prompt_banner_revealer), card);
}

static void
show_inline_prompt(KajoFilesWindow *self, InlinePromptKind kind)
{
    if (!self) return;
    self->active_prompt = kind;

    if (kind == PROMPT_MKDIR) {
        gtk_label_set_text(GTK_LABEL(self->prompt_title_lbl), "📁 NEW FOLDER NAME:");
        gtk_widget_set_visible(self->prompt_entry, TRUE);
        gtk_editable_set_text(GTK_EDITABLE(self->prompt_entry), "New Folder");
        gtk_button_set_label(GTK_BUTTON(self->prompt_submit_btn), "CREATE");
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), TRUE);
        gtk_widget_grab_focus(self->prompt_entry);
        gtk_editable_select_region(GTK_EDITABLE(self->prompt_entry), 0, -1);
    } else if (kind == PROMPT_RENAME && self->selected_item) {
        gtk_label_set_text(GTK_LABEL(self->prompt_title_lbl), "✏ RENAME TO:");
        gtk_widget_set_visible(self->prompt_entry, TRUE);
        gtk_editable_set_text(GTK_EDITABLE(self->prompt_entry), kajo_file_item_get_display_name(self->selected_item));
        gtk_button_set_label(GTK_BUTTON(self->prompt_submit_btn), "RENAME");
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), TRUE);
        gtk_widget_grab_focus(self->prompt_entry);
        gtk_editable_select_region(GTK_EDITABLE(self->prompt_entry), 0, -1);
    } else if (kind == PROMPT_DELETE_PERM && self->selected_item) {
        g_autofree char *msg = g_strdup_printf(
            "🔥 PERMANENTLY DELETE '%s'? (CANNOT BE UNDONE)",
            kajo_file_item_get_display_name(self->selected_item)
        );
        gtk_label_set_text(GTK_LABEL(self->prompt_title_lbl), msg);
        gtk_widget_set_visible(self->prompt_entry, FALSE);
        gtk_button_set_label(GTK_BUTTON(self->prompt_submit_btn), "DELETE PERMANENTLY");
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), TRUE);
    }
}

/* ─── Interactive Breadcrumb Bar ─── */

static void
on_breadcrumb_segment_clicked(GtkButton *btn, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *uri = g_object_get_data(G_OBJECT(btn), "segment-uri");
    if (uri)
        kajo_files_window_navigate_uri(self, uri);
}

static void
rebuild_breadcrumbs(KajoFilesWindow *self)
{
    if (!self || !self->breadcrumb_box || !self->current_dir)
        return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->breadcrumb_box)) != NULL) {
        gtk_box_remove(GTK_BOX(self->breadcrumb_box), child);
    }

    g_autofree char *uri = g_file_get_uri(self->current_dir);
    g_autofree char *local_path = g_file_get_path(self->current_dir);

    gtk_editable_set_text(GTK_EDITABLE(self->uri_entry), local_path ? local_path : (uri ? uri : ""));

    g_autofree char *badge = g_strdup_printf("LOCATION: %s", local_path ? local_path : (uri ? uri : ""));
    gtk_label_set_text(GTK_LABEL(self->current_uri_badge), badge);

    /* Walk GFile parents up to root */
    GPtrArray *chain = g_ptr_array_new_with_free_func(g_object_unref);
    GFile *cursor = g_object_ref(self->current_dir);
    const char *home_dir = g_get_home_dir();
    g_autoptr(GFile) home_file = home_dir ? g_file_new_for_path(home_dir) : NULL;

    while (cursor != NULL) {
        g_ptr_array_insert(chain, 0, cursor);
        if (home_file && g_file_equal(cursor, home_file))
            break;
        GFile *parent = g_file_get_parent(cursor);
        cursor = parent;
    }

    for (guint i = 0; i < chain->len; i++) {
        GFile *seg = g_ptr_array_index(chain, i);
        g_autofree char *seg_uri = g_file_get_uri(seg);
        g_autofree char *seg_Label = NULL;

        if (home_file && g_file_equal(seg, home_file)) {
            seg_Label = g_strdup("⌂ HOME");
        } else {
            g_autofree char *base = g_file_get_basename(seg);
            if (!base || g_strcmp0(base, "/") == 0 || base[0] == '\0') {
                g_autofree char *scheme = g_file_get_uri_scheme(seg);
                if (g_strcmp0(scheme, "trash") == 0)
                    seg_Label = g_strdup("🗑 TRASH");
                else if (g_strcmp0(scheme, "network") == 0)
                    seg_Label = g_strdup("🌐 NETWORK");
                else if (g_strcmp0(scheme, "recent") == 0)
                    seg_Label = g_strdup("🕒 RECENT");
                else if (g_strcmp0(scheme, "file") == 0)
                    seg_Label = g_strdup("/ ROOT");
                else
                    seg_Label = g_strdup(seg_uri);
            } else {
                seg_Label = g_steal_pointer(&base);
            }
        }

        if (i > 0) {
            GtkWidget *sep = gtk_label_new("▸");
            gtk_widget_add_css_class(sep, "sidebar-chevron");
            gtk_box_append(GTK_BOX(self->breadcrumb_box), sep);
        }

        GtkWidget *btn = gtk_button_new_with_label(seg_Label);
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_add_css_class(btn, "files-breadcrumb-btn");
        if (i + 1 == chain->len)
            gtk_widget_add_css_class(btn, "active");

        g_object_set_data_full(G_OBJECT(btn), "segment-uri", g_strdup(seg_uri), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_breadcrumb_segment_clicked), self);
        gtk_box_append(GTK_BOX(self->breadcrumb_box), btn);
    }

    g_ptr_array_unref(chain);
}

/* ─── Virtualized Recycling View Factories: Metro Tile Grid & Details List ─── */

typedef struct {
    GtkWidget *card;
    GtkWidget *icon;
    GtkWidget *name_lbl;
    GtkWidget *sub_lbl;
    GCancellable *thumb_cancel;
} GridTileWidgets;

static void
grid_tile_widgets_free(gpointer data)
{
    GridTileWidgets *w = data;
    if (!w) return;
    if (w->thumb_cancel) {
        g_cancellable_cancel(w->thumb_cancel);
        g_clear_object(&w->thumb_cancel);
    }
    g_free(w);
}

static void
thumb_load_thread_cb(GTask *task,
                     gpointer source_object,
                     gpointer task_data G_GNUC_UNUSED,
                     GCancellable *cancellable)
{
    GFile *file = G_FILE(source_object);
    GError *error = NULL;
    GdkTexture *tex = gdk_texture_new_from_file(file, &error);
    if (!tex) {
        g_task_return_error(task, error);
        return;
    }
    if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        g_object_unref(tex);
        g_task_return_error(task, error);
        return;
    }
    g_task_return_pointer(task, tex, g_object_unref);
}

static void
on_thumb_load_ready(GObject *source G_GNUC_UNUSED,
                    GAsyncResult *res,
                    gpointer user_data)
{
    GridTileWidgets *w = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GdkTexture) tex = g_task_propagate_pointer(G_TASK(res), &error);
    if (!tex || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    if (w && w->icon && GTK_IS_IMAGE(w->icon)) {
        gtk_image_set_from_paintable(GTK_IMAGE(w->icon), GDK_PAINTABLE(tex));
        gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 48);
    }
}

static void
on_grid_item_setup(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                   GtkListItem *list_item,
                   gpointer user_data G_GNUC_UNUSED)
{
    GridTileWidgets *w = g_new0(GridTileWidgets, 1);

    w->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(w->card, "files-tile-card");
    gtk_widget_set_size_request(w->card, 136, 114);

    w->icon = gtk_image_new_from_icon_name("text-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 42);
    gtk_widget_set_halign(w->icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(w->icon, 8);
    gtk_box_append(GTK_BOX(w->card), w->icon);

    w->name_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->name_lbl, "files-tile-title");
    gtk_label_set_ellipsize(GTK_LABEL(w->name_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(w->name_lbl), 16);
    gtk_widget_set_halign(w->name_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(w->card), w->name_lbl);

    w->sub_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->sub_lbl, "files-tile-sub");
    gtk_widget_set_halign(w->sub_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(w->card), w->sub_lbl);

    g_object_set_data_full(G_OBJECT(list_item), "tile-widgets", w, grid_tile_widgets_free);
    gtk_list_item_set_child(list_item, w->card);
}

static void
on_grid_item_bind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                  GtkListItem *list_item,
                  gpointer user_data G_GNUC_UNUSED)
{
    GridTileWidgets *w = g_object_get_data(G_OBJECT(list_item), "tile-widgets");
    KajoFileItem *item = gtk_list_item_get_item(list_item);
    if (!w || !item) return;

    if (w->thumb_cancel) {
        g_cancellable_cancel(w->thumb_cancel);
        g_clear_object(&w->thumb_cancel);
    }

    if (kajo_file_item_is_dir(item))
        gtk_widget_add_css_class(w->card, "files-tile-folder");
    else
        gtk_widget_remove_css_class(w->card, "files-tile-folder");

    gtk_image_set_from_icon_name(GTK_IMAGE(w->icon), kajo_file_item_get_icon_name(item));
    gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 42);

    gtk_label_set_text(GTK_LABEL(w->name_lbl), kajo_file_item_get_display_name(item));

    g_autofree char *sz_str = kajo_files_format_size(
        kajo_file_item_get_size(item),
        kajo_file_item_is_dir(item)
    );
    gtk_label_set_text(GTK_LABEL(w->sub_lbl), sz_str);

    g_object_set_data_full(G_OBJECT(w->card), "file-item", g_object_ref(item), g_object_unref);

    const char *ctype = kajo_file_item_get_content_type(item);
    GFile *file = kajo_file_item_get_file(item);
    guint64 sz = kajo_file_item_get_size(item);
    if (ctype && g_str_has_prefix(ctype, "image/") &&
        file && g_file_is_native(file) &&
        sz > 0 && sz <= (2ULL * 1024ULL * 1024ULL)) {
        w->thumb_cancel = g_cancellable_new();
        g_autoptr(GTask) task = g_task_new(file, w->thumb_cancel, on_thumb_load_ready, w);
        g_task_run_in_thread(task, thumb_load_thread_cb);
    }
}

static void
on_grid_item_unbind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                    GtkListItem *list_item,
                    gpointer user_data G_GNUC_UNUSED)
{
    GridTileWidgets *w = g_object_get_data(G_OBJECT(list_item), "tile-widgets");
    if (!w) return;

    if (w->thumb_cancel) {
        g_cancellable_cancel(w->thumb_cancel);
        g_clear_object(&w->thumb_cancel);
    }
    g_object_set_data(G_OBJECT(w->card), "file-item", NULL);
}

typedef struct {
    GtkWidget *hbox;
    GtkWidget *icon;
    GtkWidget *name_lbl;
    GtkWidget *sz_lbl;
    GtkWidget *type_lbl;
    GtkWidget *mtime_lbl;
} ListRowWidgets;

static void
on_list_item_setup(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                   GtkListItem *list_item,
                   gpointer user_data G_GNUC_UNUSED)
{
    ListRowWidgets *w = g_new0(ListRowWidgets, 1);

    w->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(w->hbox, "files-list-row");
    gtk_widget_add_css_class(w->hbox, "files-list-row-box");
    gtk_widget_set_valign(w->hbox, GTK_ALIGN_CENTER);

    w->icon = gtk_image_new_from_icon_name("text-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 20);
    gtk_widget_set_valign(w->icon, GTK_ALIGN_CENTER);

    w->name_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->name_lbl, "files-list-name");
    gtk_widget_set_hexpand(w->name_lbl, TRUE);
    gtk_widget_set_halign(w->name_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(w->name_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(w->name_lbl), PANGO_ELLIPSIZE_END);

    w->sz_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->sz_lbl, "files-list-meta");
    gtk_widget_set_size_request(w->sz_lbl, 100, -1);
    gtk_widget_set_halign(w->sz_lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(w->sz_lbl, GTK_ALIGN_CENTER);

    w->type_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->type_lbl, "files-list-meta");
    gtk_widget_set_size_request(w->type_lbl, 160, -1);
    gtk_widget_set_halign(w->type_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(w->type_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(w->type_lbl), PANGO_ELLIPSIZE_END);

    w->mtime_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->mtime_lbl, "files-list-meta");
    gtk_widget_set_size_request(w->mtime_lbl, 140, -1);
    gtk_widget_set_halign(w->mtime_lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(w->mtime_lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(w->hbox), w->icon);
    gtk_box_append(GTK_BOX(w->hbox), w->name_lbl);
    gtk_box_append(GTK_BOX(w->hbox), w->sz_lbl);
    gtk_box_append(GTK_BOX(w->hbox), w->type_lbl);
    gtk_box_append(GTK_BOX(w->hbox), w->mtime_lbl);

    g_object_set_data_full(G_OBJECT(list_item), "row-widgets", w, g_free);
    gtk_list_item_set_child(list_item, w->hbox);
}

static void
on_list_item_bind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                  GtkListItem *list_item,
                  gpointer user_data G_GNUC_UNUSED)
{
    ListRowWidgets *w = g_object_get_data(G_OBJECT(list_item), "row-widgets");
    KajoFileItem *item = gtk_list_item_get_item(list_item);
    if (!w || !item) return;

    gtk_image_set_from_icon_name(GTK_IMAGE(w->icon), kajo_file_item_get_icon_name(item));
    gtk_label_set_text(GTK_LABEL(w->name_lbl), kajo_file_item_get_display_name(item));

    g_autofree char *sz_str = kajo_files_format_size(
        kajo_file_item_get_size(item),
        kajo_file_item_is_dir(item)
    );
    gtk_label_set_text(GTK_LABEL(w->sz_lbl), sz_str);
    gtk_label_set_text(GTK_LABEL(w->type_lbl), kajo_file_item_get_type_description(item));

    g_autofree char *mtime_str = kajo_files_format_mtime(kajo_file_item_get_mtime(item));
    gtk_label_set_text(GTK_LABEL(w->mtime_lbl), mtime_str);

    g_object_set_data_full(G_OBJECT(w->hbox), "file-item", g_object_ref(item), g_object_unref);
}

static void
on_list_item_unbind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                    GtkListItem *list_item,
                    gpointer user_data G_GNUC_UNUSED)
{
    ListRowWidgets *w = g_object_get_data(G_OBJECT(list_item), "row-widgets");
    if (!w) return;
    g_object_set_data(G_OBJECT(w->hbox), "file-item", NULL);
}

typedef struct {
    KajoFileSortField field;
    gboolean ascending;
    gboolean folders_first;
} SortCtx;

static gint
ptr_array_sort_cb(gconstpointer a, gconstpointer b, gpointer user_data)
{
    const KajoFileItem *item_a = *(const KajoFileItem *const *)a;
    const KajoFileItem *item_b = *(const KajoFileItem *const *)b;
    const SortCtx *ctx = user_data;
    return kajo_file_item_compare_full(item_a, item_b, ctx->field, ctx->ascending, ctx->folders_first);
}

static void
rebuild_filtered_view(KajoFilesWindow *self)
{
    if (!self) return;

    g_autoptr(KajoFileItem) prev_selected = self->selected_item ? g_object_ref(self->selected_item) : NULL;

    if (self->visible_items)
        g_ptr_array_set_size(self->visible_items, 0);
    else
        self->visible_items = g_ptr_array_new_with_free_func(g_object_unref);

    const char *query = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));

    if (self->all_items) {
        for (guint i = 0; i < self->all_items->len; i++) {
            KajoFileItem *it = g_ptr_array_index(self->all_items, i);
            if (kajo_file_item_matches_filter(it, self->show_hidden, query)) {
                g_ptr_array_add(self->visible_items, g_object_ref(it));
            }
        }
    }

    SortCtx sctx = { self->sort_field, self->sort_ascending, self->sort_folders_first };
    g_ptr_array_sort_with_data(self->visible_items, ptr_array_sort_cb, &sctx);

    self->suppress_selection_sync = TRUE;
    if (self->visible_store) {
        guint old_n = g_list_model_get_n_items(G_LIST_MODEL(self->visible_store));
        g_list_store_splice(
            self->visible_store,
            0,
            old_n,
            self->visible_items->pdata,
            self->visible_items->len
        );
    }
    self->suppress_selection_sync = FALSE;

    if (self->visible_items->len == 0) {
        gtk_stack_set_visible_child_name(self->view_stack, "empty");
        g_clear_object(&self->selected_item);
        kajo_quicklook_set_item(self->quicklook, NULL, self->current_dir);
        update_status_and_capacity_bar(self);
        return;
    }

    int restore_idx = 0;
    if (prev_selected) {
        for (guint i = 0; i < self->visible_items->len; i++) {
            if (g_ptr_array_index(self->visible_items, i) == prev_selected) {
                restore_idx = (int)i;
                break;
            }
        }
    }

    gtk_stack_set_visible_child_name(
        self->view_stack,
        (self->view_mode == KAJO_VIEW_GRID) ? "grid" : "list"
    );

    set_selected_item_by_index(self, restore_idx);
}

/* ─── Selection & Activation ─── */

static int
get_selected_item_index(KajoFilesWindow *self)
{
    if (!self || !self->visible_items || self->visible_items->len == 0)
        return -1;
    if (self->selection_model) {
        guint pos = gtk_single_selection_get_selected(self->selection_model);
        if (pos != GTK_INVALID_LIST_POSITION && pos < self->visible_items->len)
            return (int)pos;
    }
    if (!self->selected_item)
        return -1;
    for (guint i = 0; i < self->visible_items->len; i++) {
        KajoFileItem *it = g_ptr_array_index(self->visible_items, i);
        if (it == self->selected_item)
            return (int)i;
    }
    return -1;
}

static void
set_selected_item_by_index(KajoFilesWindow *self, int idx)
{
    if (!self || !self->visible_items || self->visible_items->len == 0)
        return;

    idx = CLAMP(idx, 0, (int)self->visible_items->len - 1);
    KajoFileItem *item = g_ptr_array_index(self->visible_items, (guint)idx);
    g_set_object(&self->selected_item, item);

    if (self->selection_model) {
        self->suppress_selection_sync = TRUE;
        gtk_single_selection_set_selected(self->selection_model, (guint)idx);
        self->suppress_selection_sync = FALSE;
    }

    kajo_quicklook_set_item(self->quicklook, item, self->current_dir);
    update_status_and_capacity_bar(self);
}

static void
on_selection_model_changed(GtkSingleSelection *sel,
                           GParamSpec *pspec G_GNUC_UNUSED,
                           gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self || self->suppress_selection_sync)
        return;

    KajoFileItem *item = gtk_single_selection_get_selected_item(sel);
    if (item && item != self->selected_item) {
        g_set_object(&self->selected_item, item);
        kajo_quicklook_set_item(self->quicklook, item, self->current_dir);
        update_status_and_capacity_bar(self);
    }
}

static void
activate_file_item(KajoFilesWindow *self, KajoFileItem *item)
{
    if (!self || !item) return;

    /* Check if we are in Trash — if so, Quick Look instead of navigating into trashed folder */
    if (self->current_dir) {
        g_autofree char *scheme = g_file_get_uri_scheme(self->current_dir);
        if (g_strcmp0(scheme, "trash") == 0) {
            kajo_quicklook_show(self->quicklook);
            return;
        }
    }

    const char *target_uri = kajo_file_item_get_target_uri(item);
    if (kajo_file_item_is_dir(item) || kajo_file_item_is_mountable(item)) {
        const char *dest = target_uri ? target_uri : kajo_file_item_get_uri(item);
        kajo_files_window_navigate_uri(self, dest);
    } else {
        kajo_files_open_item_default(item, GTK_WINDOW(self));
    }
}

static void
on_view_item_activated(GtkWidget *view G_GNUC_UNUSED,
                       guint position,
                       gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self || !self->visible_items || position >= self->visible_items->len)
        return;

    KajoFileItem *item = g_ptr_array_index(self->visible_items, position);
    if (item)
        activate_file_item(self, item);
}

/* ─── Directory Enumeration & Live GFileMonitor ─── */

static gboolean
on_dir_monitor_debounce_timeout(gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->monitor_debounce_id = 0;
    reload_current_directory(self);
    return G_SOURCE_REMOVE;
}

static void
on_dir_monitor_changed(GFileMonitor *monitor G_GNUC_UNUSED,
                       GFile *file G_GNUC_UNUSED,
                       GFile *other_file G_GNUC_UNUSED,
                       GFileMonitorEvent event_type,
                       gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (event_type == G_FILE_MONITOR_EVENT_CREATED ||
        event_type == G_FILE_MONITOR_EVENT_DELETED ||
        event_type == G_FILE_MONITOR_EVENT_RENAMED ||
        event_type == G_FILE_MONITOR_EVENT_MOVED_IN ||
        event_type == G_FILE_MONITOR_EVENT_MOVED_OUT) {
        if (self->monitor_debounce_id == 0) {
            self->monitor_debounce_id = g_timeout_add(250, on_dir_monitor_debounce_timeout, self);
        }
    }
}

static void
on_directory_enumerated(GPtrArray *items, GError *error, gpointer user_data)
{
    KajoFilesWindow *self = user_data;

    if (error) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            return;

        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED) && self->current_dir) {
            set_status_message(self, "Mounting remote / encrypted volume...", FALSE);
            kajo_files_sidebar_mount_uri(self->sidebar, self->current_dir);
            return;
        }

        set_status_message(self, error->message, TRUE);
        gtk_label_set_text(GTK_LABEL(self->empty_label), error->message);
        gtk_stack_set_visible_child_name(self->view_stack, "empty");
        return;
    }

    gtk_label_set_text(GTK_LABEL(self->empty_label), "DIRECTORY IS EMPTY");

    if (self->all_items)
        g_ptr_array_unref(self->all_items);
    self->all_items = items ? items : g_ptr_array_new_with_free_func(g_object_unref);

    if (self->current_dir) {
        g_autofree char *scheme = g_file_get_uri_scheme(self->current_dir);
        gboolean is_trash = (g_strcmp0(scheme, "trash") == 0);
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->trash_banner_revealer), is_trash);
        if (is_trash) {
            g_autofree char *t_lbl = g_strdup_printf("🗑 TRASH // %u ITEM(S)", self->all_items->len);
            gtk_label_set_text(GTK_LABEL(self->trash_summary_lbl), t_lbl);
        }
    }

    rebuild_filtered_view(self);
}

static void
reload_current_directory(KajoFilesWindow *self)
{
    if (!self || !self->current_dir)
        return;

    if (self->enum_cancel) {
        g_cancellable_cancel(self->enum_cancel);
        g_clear_object(&self->enum_cancel);
    }
    self->enum_cancel = g_cancellable_new();

    if (self->dir_monitor) {
        g_file_monitor_cancel(self->dir_monitor);
        g_clear_object(&self->dir_monitor);
    }

    self->dir_monitor = g_file_monitor_directory(
        self->current_dir,
        G_FILE_MONITOR_WATCH_MOVES,
        NULL,
        NULL
    );
    if (self->dir_monitor) {
        g_signal_connect(self->dir_monitor, "changed", G_CALLBACK(on_dir_monitor_changed), self);
    }

    gtk_widget_set_sensitive(self->btn_paste, kajo_files_clipboard_has_files());
    kajo_files_enumerate_async(self->current_dir, self->enum_cancel, on_directory_enumerated, self);
}

void
kajo_files_window_navigate_uri(KajoFilesWindow *self, const char *uri_or_path)
{
    g_return_if_fail(KAJO_IS_FILES_WINDOW(self));

    g_autoptr(GFile) target = NULL;
    if (!uri_or_path || uri_or_path[0] == '\0') {
        target = g_file_new_for_path(g_get_home_dir());
    } else if (strstr(uri_or_path, "://") != NULL) {
        target = g_file_new_for_uri(uri_or_path);
    } else if (uri_or_path[0] == '~') {
        g_autofree char *expanded = g_build_filename(g_get_home_dir(), uri_or_path + 1, NULL);
        target = g_file_new_for_path(expanded);
    } else {
        target = g_file_new_for_commandline_arg(uri_or_path);
    }

    g_set_object(&self->current_dir, target);
    g_autofree char *canonical_uri = g_file_get_uri(target);

    if (!self->navigating_history && canonical_uri) {
        while ((int)self->history->len > self->history_pos + 1) {
            g_ptr_array_remove_index(self->history, self->history->len - 1);
        }
        g_ptr_array_add(self->history, g_strdup(canonical_uri));
        self->history_pos = (int)self->history->len - 1;
    }

    gtk_widget_set_sensitive(self->btn_back, self->history_pos > 0);
    gtk_widget_set_sensitive(self->btn_forward, self->history_pos + 1 < (int)self->history->len);

    g_autoptr(GFile) parent = g_file_get_parent(target);
    gtk_widget_set_sensitive(self->btn_up, parent != NULL);

    gtk_stack_set_visible_child_name(self->location_stack, "breadcrumbs");
    rebuild_breadcrumbs(self);

    if (self->sidebar && canonical_uri)
        kajo_files_sidebar_sync_active_uri(self->sidebar, canonical_uri);

    reload_current_directory(self);
}

/* ─── Header & Ribbon Button Handlers ─── */

static void
on_btn_back_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->history_pos > 0) {
        self->history_pos--;
        self->navigating_history = TRUE;
        const char *uri = g_ptr_array_index(self->history, (guint)self->history_pos);
        kajo_files_window_navigate_uri(self, uri);
        self->navigating_history = FALSE;
    }
}

static void
on_btn_forward_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->history_pos + 1 < (int)self->history->len) {
        self->history_pos++;
        self->navigating_history = TRUE;
        const char *uri = g_ptr_array_index(self->history, (guint)self->history_pos);
        kajo_files_window_navigate_uri(self, uri);
        self->navigating_history = FALSE;
    }
}

static void
on_btn_up_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self->current_dir) return;
    g_autoptr(GFile) parent = g_file_get_parent(self->current_dir);
    if (parent) {
        g_autofree char *uri = g_file_get_uri(parent);
        kajo_files_window_navigate_uri(self, uri);
    }
}

static void
on_btn_reload_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    reload_current_directory(self);
}

static void
on_btn_edit_uri_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *vis = gtk_stack_get_visible_child_name(self->location_stack);
    if (g_strcmp0(vis, "entry") == 0) {
        gtk_stack_set_visible_child_name(self->location_stack, "breadcrumbs");
    } else {
        gtk_stack_set_visible_child_name(self->location_stack, "entry");
        gtk_widget_grab_focus(self->uri_entry);
        gtk_editable_select_region(GTK_EDITABLE(self->uri_entry), 0, -1);
    }
}

static void
on_uri_entry_activated(GtkEntry *entry, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (text && text[0] != '\0')
        kajo_files_window_navigate_uri(self, text);
}

static void
on_search_changed(GtkSearchEntry *entry G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    rebuild_filtered_view(self);
}

static void
set_view_mode(KajoFilesWindow *self, KajoFileViewMode mode)
{
    self->view_mode = mode;
    if (mode == KAJO_VIEW_GRID) {
        gtk_widget_add_css_class(self->btn_view_grid, "active");
        gtk_widget_remove_css_class(self->btn_view_list, "active");
    } else {
        gtk_widget_add_css_class(self->btn_view_list, "active");
        gtk_widget_remove_css_class(self->btn_view_grid, "active");
    }
    if (self->visible_items && self->visible_items->len > 0) {
        gtk_stack_set_visible_child_name(
            self->view_stack,
            (mode == KAJO_VIEW_GRID) ? "grid" : "list"
        );
    }
}

static void
on_btn_view_grid_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    set_view_mode(KAJO_FILES_WINDOW(user_data), KAJO_VIEW_GRID);
}

static void
on_btn_view_list_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    set_view_mode(KAJO_FILES_WINDOW(user_data), KAJO_VIEW_LIST);
}

static void
on_btn_inspector_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->inspector_visible = !self->inspector_visible;
    gtk_widget_set_visible(self->inspector_vbox, self->inspector_visible && !self->is_narrow);
    if (self->inspector_visible)
        gtk_widget_add_css_class(self->btn_inspector, "active");
    else
        gtk_widget_remove_css_class(self->btn_inspector, "active");
}

static void
on_btn_float_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    niri_send_action("{\"Action\":\"ToggleWindowFloating\"}");
}

static void
on_btn_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    gtk_window_close(GTK_WINDOW(user_data));
}

static void
on_btn_new_folder_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    show_inline_prompt(KAJO_FILES_WINDOW(user_data), PROMPT_MKDIR);
}

static void
on_btn_paste_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->current_dir)
        kajo_files_clipboard_paste_async(self->current_dir, on_file_op_completed, self);
}

static void
on_btn_hidden_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->show_hidden = !self->show_hidden;
    if (self->show_hidden)
        gtk_widget_add_css_class(self->btn_hidden, "active");
    else
        gtk_widget_remove_css_class(self->btn_hidden, "active");
    rebuild_filtered_view(self);
}

/* ─── Sorting System (Sort Popover & Clickable Column Headers) ─── */

static void
set_widget_active_class(GtkWidget *w, gboolean active)
{
    if (!w) return;
    if (active)
        gtk_widget_add_css_class(w, "active");
    else
        gtk_widget_remove_css_class(w, "active");
}

static void
sync_sort_ui(KajoFilesWindow *self)
{
    if (!self) return;

    const char *names[] = { "NAME", "SIZE", "MODIFIED", "TYPE" };
    const char *arrow = self->sort_ascending ? "▲" : "▼";

    g_autofree char *btn_lbl = g_strdup_printf(
        "⇅ SORT: %s %s",
        names[self->sort_field],
        arrow
    );
    gtk_menu_button_set_label(GTK_MENU_BUTTON(self->btn_sort), btn_lbl);

    /* Sort Popover Field Rows */
    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_field_name),
        (self->sort_field == KAJO_SORT_NAME) ? "✓  Name (Natural A–Z)" : "   Name (Natural A–Z)"
    );
    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_field_modified),
        (self->sort_field == KAJO_SORT_MODIFIED) ? "✓  Date Modified" : "   Date Modified"
    );
    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_field_size),
        (self->sort_field == KAJO_SORT_SIZE) ? "✓  Size" : "   Size"
    );
    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_field_type),
        (self->sort_field == KAJO_SORT_TYPE) ? "✓  Type / Extension" : "   Type / Extension"
    );

    set_widget_active_class(self->btn_sort_field_name, self->sort_field == KAJO_SORT_NAME);
    set_widget_active_class(self->btn_sort_field_modified, self->sort_field == KAJO_SORT_MODIFIED);
    set_widget_active_class(self->btn_sort_field_size, self->sort_field == KAJO_SORT_SIZE);
    set_widget_active_class(self->btn_sort_field_type, self->sort_field == KAJO_SORT_TYPE);

    /* Sort Popover Direction & Folders First */
    set_widget_active_class(self->btn_sort_asc, self->sort_ascending);
    set_widget_active_class(self->btn_sort_desc, !self->sort_ascending);

    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_folders_first),
        self->sort_folders_first ? "☑  Folders First" : "☐  Folders First"
    );
    set_widget_active_class(self->btn_sort_folders_first, self->sort_folders_first);

    /* Details List Clickable Column Headers */
    g_autofree char *col_name = (self->sort_field == KAJO_SORT_NAME)
        ? g_strdup_printf("NAME %s", arrow) : g_strdup("NAME");
    g_autofree char *col_size = (self->sort_field == KAJO_SORT_SIZE)
        ? g_strdup_printf("SIZE %s", arrow) : g_strdup("SIZE");
    g_autofree char *col_type = (self->sort_field == KAJO_SORT_TYPE)
        ? g_strdup_printf("TYPE %s", arrow) : g_strdup("TYPE");
    g_autofree char *col_mod = (self->sort_field == KAJO_SORT_MODIFIED)
        ? g_strdup_printf("MODIFIED %s", arrow) : g_strdup("MODIFIED");

    gtk_button_set_label(GTK_BUTTON(self->col_btn_name), col_name);
    gtk_button_set_label(GTK_BUTTON(self->col_btn_size), col_size);
    gtk_button_set_label(GTK_BUTTON(self->col_btn_type), col_type);
    gtk_button_set_label(GTK_BUTTON(self->col_btn_modified), col_mod);

    set_widget_active_class(self->col_btn_name, self->sort_field == KAJO_SORT_NAME);
    set_widget_active_class(self->col_btn_size, self->sort_field == KAJO_SORT_SIZE);
    set_widget_active_class(self->col_btn_type, self->sort_field == KAJO_SORT_TYPE);
    set_widget_active_class(self->col_btn_modified, self->sort_field == KAJO_SORT_MODIFIED);
}

static void
select_or_toggle_sort_field(KajoFilesWindow *self, KajoFileSortField field, gboolean close_popover)
{
    if (self->sort_field == field) {
        self->sort_ascending = !self->sort_ascending;
    } else {
        self->sort_field = field;
        self->sort_ascending = kajo_file_sort_field_default_ascending(field);
    }

    if (close_popover && self->sort_popover)
        gtk_popover_popdown(self->sort_popover);

    sync_sort_ui(self);
    rebuild_filtered_view(self);
}

static void
on_sort_field_name_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_NAME, TRUE);
}

static void
on_sort_field_modified_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_MODIFIED, TRUE);
}

static void
on_sort_field_size_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_SIZE, TRUE);
}

static void
on_sort_field_type_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_TYPE, TRUE);
}

static void
on_sort_asc_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->sort_ascending = TRUE;
    sync_sort_ui(self);
    rebuild_filtered_view(self);
}

static void
on_sort_desc_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->sort_ascending = FALSE;
    sync_sort_ui(self);
    rebuild_filtered_view(self);
}

static void
on_sort_folders_first_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->sort_folders_first = !self->sort_folders_first;
    sync_sort_ui(self);
    rebuild_filtered_view(self);
}

static void
on_col_btn_name_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_NAME, FALSE);
}

static void
on_col_btn_size_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_SIZE, FALSE);
}

static void
on_col_btn_type_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_TYPE, FALSE);
}

static void
on_col_btn_modified_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_MODIFIED, FALSE);
}

static void
on_btn_terminal_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->current_dir)
        kajo_files_open_terminal_at(self->current_dir);
}

static void
on_btn_sidebar_toggle_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    gboolean vis = gtk_widget_get_visible(self->sidebar_vbox);
    gtk_widget_set_visible(self->sidebar_vbox, !vis);
}

/* ─── Sidebar & QuickLook Delegate Callbacks ─── */

static void
on_sidebar_navigate(const char *uri, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->is_narrow)
        gtk_widget_set_visible(self->sidebar_vbox, FALSE);
    kajo_files_window_navigate_uri(self, uri);
}

static void
on_sidebar_connect_server(gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    gboolean rev = gtk_revealer_get_reveal_child(GTK_REVEALER(self->connect_banner_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->connect_banner_revealer), !rev);
    if (!rev)
        gtk_widget_grab_focus(self->connect_entry);
}

static void
on_sidebar_status(const char *msg, gboolean is_error, gpointer user_data)
{
    set_status_message(KAJO_FILES_WINDOW(user_data), msg, is_error);
}

static void
trash_selected_item(KajoFilesWindow *self)
{
    if (!self || !self->selected_item) return;

    if (self->current_dir) {
        g_autofree char *scheme = g_file_get_uri_scheme(self->current_dir);
        if (g_strcmp0(scheme, "trash") == 0) {
            show_inline_prompt(self, PROMPT_DELETE_PERM);
            return;
        }
    }

    g_autoptr(GPtrArray) arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(arr, g_object_ref(self->selected_item));
    kajo_files_op_trash_async(arr, on_file_op_completed, self);
}

static void
on_quicklook_action(KajoQuickLookAction action, KajoFileItem *item, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    switch (action) {
    case KAJO_QL_ACTION_OPEN_DEFAULT:
        if (item) activate_file_item(self, item);
        break;
    case KAJO_QL_ACTION_OPEN_TERMINAL:
        if (self->current_dir) kajo_files_open_terminal_at(self->current_dir);
        break;
    case KAJO_QL_ACTION_RENAME:
        show_inline_prompt(self, PROMPT_RENAME);
        break;
    case KAJO_QL_ACTION_TRASH:
        trash_selected_item(self);
        break;
    case KAJO_QL_ACTION_PREV_ITEM: {
        int idx = get_selected_item_index(self);
        if (idx > 0) set_selected_item_by_index(self, idx - 1);
        break;
    }
    case KAJO_QL_ACTION_NEXT_ITEM: {
        int idx = get_selected_item_index(self);
        if (idx >= 0 && idx + 1 < (int)self->visible_items->len)
            set_selected_item_by_index(self, idx + 1);
        break;
    }
    }
}

/* ─── Context Menu Popover (Item Context + Directory Background + Open With) ─── */

static GtkWidget *
create_ctx_action_button(const char *label_text,
                         const char *shortcut_text,
                         gboolean is_danger,
                         GCallback callback,
                         gpointer user_data)
{
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_add_css_class(btn, "files-menu-row");
    if (is_danger)
        gtk_widget_add_css_class(btn, "action-btn-danger");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), lbl);

    if (shortcut_text && shortcut_text[0] != '\0') {
        GtkWidget *sc = gtk_label_new(shortcut_text);
        gtk_widget_add_css_class(sc, "files-ctx-shortcut");
        gtk_widget_set_halign(sc, GTK_ALIGN_END);
        gtk_widget_set_valign(sc, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(hbox), sc);
    }

    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    if (callback)
        g_signal_connect(btn, "clicked", callback, user_data);
    return btn;
}

static void
close_context_menu(KajoFilesWindow *self)
{
    if (self && self->context_popover)
        gtk_popover_popdown(GTK_POPOVER(self->context_popover));
}

static void
on_ctx_open_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (self->selected_item)
        activate_file_item(self, self->selected_item);
}

static void
on_ctx_quicklook_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (self->selected_item)
        kajo_quicklook_show(self->quicklook);
}

static void
on_ctx_extract_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (!self->selected_item || !self->current_dir)
        return;

    GFile *archive_file = kajo_file_item_get_file(self->selected_item);
    set_status_message(self, "Extracting archive...", FALSE);
    kajo_files_op_extract_archive_async(archive_file, self->current_dir, on_file_op_completed, self);
}

static void
on_ctx_item_terminal_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (self->selected_item && kajo_file_item_is_dir(self->selected_item))
        kajo_files_open_terminal_at(kajo_file_item_get_file(self->selected_item));
    else if (self->current_dir)
        kajo_files_open_terminal_at(self->current_dir);
}

static void
on_ctx_item_newwin_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
    const char *uri = NULL;
    g_autofree char *dir_uri = NULL;
    if (self->selected_item && kajo_file_item_is_dir(self->selected_item)) {
        uri = kajo_file_item_get_target_uri(self->selected_item);
        if (!uri) uri = kajo_file_item_get_uri(self->selected_item);
    } else if (self->current_dir) {
        dir_uri = g_file_get_uri(self->current_dir);
        uri = dir_uri;
    }
    GtkWidget *new_win = kajo_files_window_new(app, uri);
    gtk_window_present(GTK_WINDOW(new_win));
}

static void
on_ctx_restore_trash_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    on_trash_restore_clicked(NULL, self);
}

static void
on_ctx_copy_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (!self->selected_item) return;
    g_autoptr(GPtrArray) arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(arr, g_object_ref(self->selected_item));
    kajo_files_clipboard_set(arr, FALSE);
    gtk_widget_set_sensitive(self->btn_paste, TRUE);
    set_status_message(self, "Copied 1 item to clipboard", FALSE);
}

static void
on_ctx_cut_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (!self->selected_item) return;
    g_autoptr(GPtrArray) arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(arr, g_object_ref(self->selected_item));
    kajo_files_clipboard_set(arr, TRUE);
    gtk_widget_set_sensitive(self->btn_paste, TRUE);
    set_status_message(self, "Cut 1 item to clipboard", FALSE);
}

static void
copy_selected_item_path(KajoFilesWindow *self)
{
    if (!self || !self->selected_item) return;
    g_autofree char *copied = kajo_files_copy_path_to_clipboard(self->selected_item);
    if (copied) {
        g_autofree char *msg = g_strdup_printf("Copied path: %s", copied);
        set_status_message(self, msg, FALSE);
    }
}

static void
on_ctx_copy_path_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    copy_selected_item_path(self);
}

static void
on_ctx_rename_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    show_inline_prompt(self, PROMPT_RENAME);
}

static void
on_ctx_move_trash_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    trash_selected_item(self);
}

static void
on_ctx_delete_perm_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    show_inline_prompt(self, PROMPT_DELETE_PERM);
}

static void
on_ctx_inspector_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    on_btn_inspector_clicked(NULL, self);
}

static void
on_ctx_bg_mkdir_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    show_inline_prompt(self, PROMPT_MKDIR);
}

static void
on_ctx_bg_paste_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    on_btn_paste_clicked(NULL, self);
}

static void
on_ctx_bg_terminal_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    on_btn_terminal_clicked(NULL, self);
}

static void
on_ctx_bg_newwin_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
    g_autofree char *uri = self->current_dir ? g_file_get_uri(self->current_dir) : NULL;
    GtkWidget *new_win = kajo_files_window_new(app, uri);
    gtk_window_present(GTK_WINDOW(new_win));
}

static void
on_ctx_bg_hidden_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    on_btn_hidden_clicked(NULL, self);
}

static void
on_ctx_bg_reload_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    reload_current_directory(self);
}

/* ─── Open With Sub-View Handlers ─── */

static void
on_ctx_open_with_app_clicked(GtkButton *btn, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    GAppInfo *app_info = g_object_get_data(G_OBJECT(btn), "app-info");
    close_context_menu(self);

    if (!self->selected_item || !app_info)
        return;

    g_autoptr(GError) err = NULL;
    if (kajo_files_open_item_with_app(self->selected_item, app_info, self->open_with_set_default, &err)) {
        if (self->open_with_set_default) {
            g_autofree char *msg = g_strdup_printf(
                "Opened with %s (saved as default for %s)",
                g_app_info_get_display_name(app_info),
                kajo_file_item_get_content_type(self->selected_item)
            );
            set_status_message(self, msg, FALSE);
        }
    } else if (err) {
        set_status_message(self, err->message, TRUE);
    }
}

static void
on_ctx_ow_default_toggle_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->open_with_set_default = !self->open_with_set_default;
    gtk_button_set_label(
        GTK_BUTTON(self->ctx_ow_default_btn),
        self->open_with_set_default ? "☑  Set as Default for this type" : "☐  Set as Default for this type"
    );
    set_widget_active_class(self->ctx_ow_default_btn, self->open_with_set_default);
}

static void
on_ctx_ow_back_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    gtk_stack_set_visible_child_name(GTK_STACK(self->ctx_stack), "main");
}

static void
on_ctx_open_with_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self->selected_item) return;

    const char *ctype = kajo_file_item_get_content_type(self->selected_item);
    gtk_label_set_text(GTK_LABEL(self->ctx_ow_mime_lbl), ctype ? ctype : "application/octet-stream");

    self->open_with_set_default = FALSE;
    gtk_button_set_label(GTK_BUTTON(self->ctx_ow_default_btn), "☐  Set as Default for this type");
    set_widget_active_class(self->ctx_ow_default_btn, FALSE);

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->ctx_ow_list_box)) != NULL) {
        gtk_box_remove(GTK_BOX(self->ctx_ow_list_box), child);
    }

    g_autoptr(GAppInfo) default_app = ctype ? g_app_info_get_default_for_type(ctype, FALSE) : NULL;
    GList *apps = ctype ? g_app_info_get_all_for_type(ctype) : NULL;

    if (!apps) {
        GtkWidget *empty = gtk_label_new("No registered applications found for this MIME type");
        gtk_widget_add_css_class(empty, "shell-popover-subtitle");
        gtk_widget_set_margin_top(empty, 8);
        gtk_widget_set_margin_bottom(empty, 8);
        gtk_box_append(GTK_BOX(self->ctx_ow_list_box), empty);
    } else {
        for (GList *l = apps; l != NULL; l = l->next) {
            GAppInfo *app = G_APP_INFO(l->data);
            if (!g_app_info_should_show(app) && !(default_app && g_app_info_equal(app, default_app)))
                continue;

            GtkWidget *row_btn = gtk_button_new();
            gtk_widget_add_css_class(row_btn, "flat");
            gtk_widget_add_css_class(row_btn, "files-menu-row");
            if (default_app && g_app_info_equal(app, default_app))
                gtk_widget_add_css_class(row_btn, "active");

            GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

            GIcon *gicon = g_app_info_get_icon(app);
            GtkWidget *ic = gicon
                ? gtk_image_new_from_gicon(gicon)
                : gtk_image_new_from_icon_name("application-x-executable");
            gtk_image_set_pixel_size(GTK_IMAGE(ic), 16);
            gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);

            GtkWidget *nm = gtk_label_new(g_app_info_get_display_name(app));
            gtk_widget_set_hexpand(nm, TRUE);
            gtk_widget_set_halign(nm, GTK_ALIGN_START);
            gtk_widget_set_valign(nm, GTK_ALIGN_CENTER);
            gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_END);

            gtk_box_append(GTK_BOX(hbox), ic);
            gtk_box_append(GTK_BOX(hbox), nm);

            if (default_app && g_app_info_equal(app, default_app)) {
                GtkWidget *def_badge = gtk_label_new("DEFAULT");
                gtk_widget_add_css_class(def_badge, "files-ctx-shortcut");
                gtk_widget_set_valign(def_badge, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(hbox), def_badge);
            }

            gtk_button_set_child(GTK_BUTTON(row_btn), hbox);
            g_object_set_data_full(G_OBJECT(row_btn), "app-info", g_object_ref(app), g_object_unref);
            g_signal_connect(row_btn, "clicked", G_CALLBACK(on_ctx_open_with_app_clicked), self);
            gtk_box_append(GTK_BOX(self->ctx_ow_list_box), row_btn);
        }
        g_list_free_full(apps, g_object_unref);
    }

    gtk_stack_set_visible_child_name(GTK_STACK(self->ctx_stack), "open_with");
}

static void
build_context_menu_popover(KajoFilesWindow *self)
{
    self->context_popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(self->context_popover), FALSE);
    gtk_popover_set_autohide(GTK_POPOVER(self->context_popover), TRUE);
    gtk_widget_add_css_class(self->context_popover, "shell-popover");
    gtk_widget_add_css_class(self->context_popover, "files-context-popover");
    gtk_widget_set_parent(self->context_popover, GTK_WIDGET(self->view_stack));

    self->ctx_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(self->ctx_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_widget_set_size_request(self->ctx_stack, 286, -1);

    /* ── Page 1: Main Context Menu (Item or Directory Background) ── */
    GtkWidget *main_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hdr, "shell-popover-header");
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

    self->ctx_hdr_icon = gtk_image_new_from_icon_name("folder");
    gtk_image_set_pixel_size(GTK_IMAGE(self->ctx_hdr_icon), 16);
    gtk_widget_set_valign(self->ctx_hdr_icon, GTK_ALIGN_CENTER);

    self->ctx_hdr_title_lbl = gtk_label_new("ITEM");
    gtk_widget_add_css_class(self->ctx_hdr_title_lbl, "heading");
    gtk_widget_set_hexpand(self->ctx_hdr_title_lbl, TRUE);
    gtk_widget_set_halign(self->ctx_hdr_title_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(self->ctx_hdr_title_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(self->ctx_hdr_title_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(self->ctx_hdr_title_lbl), 22);

    self->ctx_hdr_badge_lbl = gtk_label_new("");
    gtk_widget_add_css_class(self->ctx_hdr_badge_lbl, "shell-popover-badge");
    gtk_widget_set_halign(self->ctx_hdr_badge_lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(self->ctx_hdr_badge_lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hdr), self->ctx_hdr_icon);
    gtk_box_append(GTK_BOX(hdr), self->ctx_hdr_title_lbl);
    gtk_box_append(GTK_BOX(hdr), self->ctx_hdr_badge_lbl);
    gtk_box_append(GTK_BOX(main_page), hdr);

    /* Item Actions Box */
    self->ctx_item_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(self->ctx_item_box, "shell-popover-body");

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("▶  Open", "Enter", FALSE, G_CALLBACK(on_ctx_open_clicked), self));

    self->ctx_btn_open_with = create_ctx_action_button(
        "⚙  Open With...", "▸", FALSE, G_CALLBACK(on_ctx_open_with_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_open_with);

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("👁  Quick Look", "Space", FALSE, G_CALLBACK(on_ctx_quicklook_clicked), self));

    self->ctx_btn_extract = create_ctx_action_button(
        "📦  Extract Here", "", FALSE, G_CALLBACK(on_ctx_extract_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_extract);

    self->ctx_btn_item_terminal = create_ctx_action_button(
        ">_  Open Terminal Here", "", FALSE, G_CALLBACK(on_ctx_item_terminal_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_item_terminal);

    self->ctx_btn_item_newwin = create_ctx_action_button(
        "🗔  Open in New Window", "Ctrl+N", FALSE, G_CALLBACK(on_ctx_item_newwin_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_item_newwin);

    self->ctx_btn_restore_trash = create_ctx_action_button(
        "↺  Restore from Trash", "", FALSE, G_CALLBACK(on_ctx_restore_trash_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_restore_trash);

    gtk_box_append(GTK_BOX(self->ctx_item_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("📋  Copy", "Ctrl+C", FALSE, G_CALLBACK(on_ctx_copy_clicked), self));
    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("✂  Cut", "Ctrl+X", FALSE, G_CALLBACK(on_ctx_cut_clicked), self));
    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("🔗  Copy Path / URI", "Ctrl+Shift+C", FALSE, G_CALLBACK(on_ctx_copy_path_clicked), self));

    gtk_box_append(GTK_BOX(self->ctx_item_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("✏  Rename...", "F2", FALSE, G_CALLBACK(on_ctx_rename_clicked), self));

    self->ctx_btn_move_trash = create_ctx_action_button(
        "🗑  Move to Trash", "Del", FALSE, G_CALLBACK(on_ctx_move_trash_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_move_trash);

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("⚠  Delete Permanently", "Shift+Del", TRUE, G_CALLBACK(on_ctx_delete_perm_clicked), self));

    gtk_box_append(GTK_BOX(self->ctx_item_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("ⓘ  Toggle Inspector", "Ctrl+I", FALSE, G_CALLBACK(on_ctx_inspector_clicked), self));

    gtk_box_append(GTK_BOX(main_page), self->ctx_item_box);

    /* Directory Background Actions Box */
    self->ctx_bg_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(self->ctx_bg_box, "shell-popover-body");

    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button("+  New Folder...", "Ctrl+Shift+N", FALSE, G_CALLBACK(on_ctx_bg_mkdir_clicked), self));

    self->ctx_btn_bg_paste = create_ctx_action_button(
        "📋  Paste Here", "Ctrl+V", FALSE, G_CALLBACK(on_ctx_bg_paste_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_bg_box), self->ctx_btn_bg_paste);

    gtk_box_append(GTK_BOX(self->ctx_bg_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button(">_  Open Terminal Here", "Ctrl+Shift+T", FALSE, G_CALLBACK(on_ctx_bg_terminal_clicked), self));
    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button("🗔  Open in New Window", "Ctrl+N", FALSE, G_CALLBACK(on_ctx_bg_newwin_clicked), self));

    gtk_box_append(GTK_BOX(self->ctx_bg_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button("👁  Toggle Hidden Files", "Ctrl+H", FALSE, G_CALLBACK(on_ctx_bg_hidden_clicked), self));
    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button("↻  Reload Directory", "Ctrl+R", FALSE, G_CALLBACK(on_ctx_bg_reload_clicked), self));
    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button("ⓘ  Toggle Inspector", "Ctrl+I", FALSE, G_CALLBACK(on_ctx_inspector_clicked), self));

    gtk_box_append(GTK_BOX(main_page), self->ctx_bg_box);
    gtk_stack_add_named(GTK_STACK(self->ctx_stack), main_page, "main");

    /* ── Page 2: Open With / Set Default Sub-View ── */
    GtkWidget *ow_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *ow_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(ow_hdr, "shell-popover-header");
    gtk_widget_set_valign(ow_hdr, GTK_ALIGN_CENTER);

    GtkWidget *btn_back = gtk_button_new_with_label("← BACK");
    gtk_widget_add_css_class(btn_back, "flat");
    gtk_widget_add_css_class(btn_back, "files-nav-btn");
    gtk_widget_set_valign(btn_back, GTK_ALIGN_CENTER);
    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_ctx_ow_back_clicked), self);

    self->ctx_ow_mime_lbl = gtk_label_new("MIME");
    gtk_widget_add_css_class(self->ctx_ow_mime_lbl, "shell-popover-badge");
    gtk_widget_set_hexpand(self->ctx_ow_mime_lbl, TRUE);
    gtk_widget_set_halign(self->ctx_ow_mime_lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(self->ctx_ow_mime_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(self->ctx_ow_mime_lbl), PANGO_ELLIPSIZE_MIDDLE);

    gtk_box_append(GTK_BOX(ow_hdr), btn_back);
    gtk_box_append(GTK_BOX(ow_hdr), self->ctx_ow_mime_lbl);
    gtk_box_append(GTK_BOX(ow_page), ow_hdr);

    GtkWidget *ow_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(ow_body, "shell-popover-body");

    GtkWidget *ow_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ow_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(ow_scroll), 220);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(ow_scroll), TRUE);

    self->ctx_ow_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ow_scroll), self->ctx_ow_list_box);
    gtk_box_append(GTK_BOX(ow_body), ow_scroll);

    gtk_box_append(GTK_BOX(ow_body), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    self->ctx_ow_default_btn = gtk_button_new_with_label("☐  Set as Default for this type");
    gtk_widget_add_css_class(self->ctx_ow_default_btn, "flat");
    gtk_widget_add_css_class(self->ctx_ow_default_btn, "files-menu-row");
    g_signal_connect(self->ctx_ow_default_btn, "clicked", G_CALLBACK(on_ctx_ow_default_toggle_clicked), self);
    gtk_box_append(GTK_BOX(ow_body), self->ctx_ow_default_btn);

    gtk_box_append(GTK_BOX(ow_page), ow_body);
    gtk_stack_add_named(GTK_STACK(self->ctx_stack), ow_page, "open_with");

    gtk_popover_set_child(GTK_POPOVER(self->context_popover), self->ctx_stack);
}

static void
open_context_menu_at(KajoFilesWindow *self, KajoFileItem *item, int x, int y)
{
    if (!self || !self->context_popover)
        return;

    gtk_stack_set_visible_child_name(GTK_STACK(self->ctx_stack), "main");

    gboolean is_trash = FALSE;
    if (self->current_dir) {
        g_autofree char *scheme = g_file_get_uri_scheme(self->current_dir);
        is_trash = (g_strcmp0(scheme, "trash") == 0);
    }

    if (item) {
        gboolean is_dir = kajo_file_item_is_dir(item);
        const char *ctype = kajo_file_item_get_content_type(item);
        const char *name = kajo_file_item_get_name(item);
        g_autofree char *sz_str = kajo_files_format_size(kajo_file_item_get_size(item), is_dir);

        gtk_image_set_from_icon_name(GTK_IMAGE(self->ctx_hdr_icon), kajo_file_item_get_icon_name(item));
        gtk_label_set_text(GTK_LABEL(self->ctx_hdr_title_lbl), kajo_file_item_get_display_name(item));
        gtk_label_set_text(GTK_LABEL(self->ctx_hdr_badge_lbl), sz_str);

        gtk_widget_set_visible(self->ctx_item_box, TRUE);
        gtk_widget_set_visible(self->ctx_bg_box, FALSE);

        gtk_widget_set_visible(self->ctx_btn_open_with, !is_dir);
        gtk_widget_set_visible(self->ctx_btn_extract, !is_dir && kajo_files_is_archive(ctype, name));
        gtk_widget_set_visible(self->ctx_btn_item_terminal, is_dir);
        gtk_widget_set_visible(self->ctx_btn_item_newwin, is_dir);
        gtk_widget_set_visible(self->ctx_btn_restore_trash, is_trash);
        gtk_widget_set_visible(self->ctx_btn_move_trash, !is_trash);
    } else {
        g_autofree char *base = self->current_dir ? g_file_get_basename(self->current_dir) : NULL;
        gtk_image_set_from_icon_name(GTK_IMAGE(self->ctx_hdr_icon), "folder");
        gtk_label_set_text(GTK_LABEL(self->ctx_hdr_title_lbl), base ? base : "CURRENT FOLDER");
        gtk_label_set_text(GTK_LABEL(self->ctx_hdr_badge_lbl), "DIR");

        gtk_widget_set_visible(self->ctx_item_box, FALSE);
        gtk_widget_set_visible(self->ctx_bg_box, TRUE);
        gtk_widget_set_sensitive(self->ctx_btn_bg_paste, kajo_files_clipboard_has_files());
    }

    GdkRectangle rect = { MAX(1, x), MAX(1, y), 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(self->context_popover), &rect);
    gtk_popover_popup(GTK_POPOVER(self->context_popover));
}

static void
open_context_menu_for_selection(KajoFilesWindow *self)
{
    if (!self) return;

    int x = gtk_widget_get_width(GTK_WIDGET(self->view_stack)) / 2;
    int y = gtk_widget_get_height(GTK_WIDGET(self->view_stack)) / 3;

    open_context_menu_at(self, self->selected_item, x, y);
}

static void
on_view_stack_right_click(GtkGestureClick *gesture G_GNUC_UNUSED,
                          int n_press,
                          double x,
                          double y,
                          gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (n_press != 1) return;

    GtkWidget *picked = gtk_widget_pick(GTK_WIDGET(self->view_stack), x, y, GTK_PICK_DEFAULT);
    KajoFileItem *hit_item = NULL;

    if (picked) {
        /* Ignore right-clicks on the Details List column header buttons */
        if (picked == self->col_btn_name || picked == self->col_btn_size ||
            picked == self->col_btn_type || picked == self->col_btn_modified ||
            gtk_widget_get_ancestor(picked, GTK_TYPE_BUTTON) != NULL) {
            return;
        }

        for (GtkWidget *w = picked; w && w != GTK_WIDGET(self->view_stack); w = gtk_widget_get_parent(w)) {
            KajoFileItem *cand = g_object_get_data(G_OBJECT(w), "file-item");
            if (cand) {
                hit_item = cand;
                break;
            }
        }

        if (hit_item && self->visible_items) {
            for (guint i = 0; i < self->visible_items->len; i++) {
                if (g_ptr_array_index(self->visible_items, i) == hit_item) {
                    set_selected_item_by_index(self, (int)i);
                    break;
                }
            }
        }
    }

    open_context_menu_at(self, hit_item, (int)x, (int)y);
}

/* ─── Keyboard Shortcuts Controller (Space Quick Look, Navigation, Clipboard) ─── */

static gboolean
is_text_input_focused(GtkWindow *win)
{
    GtkWidget *focus = gtk_window_get_focus(win);
    if (!focus) return FALSE;
    return (GTK_IS_EDITABLE(focus) ||
            GTK_IS_ENTRY(focus) ||
            GTK_IS_SEARCH_ENTRY(focus) ||
            GTK_IS_PASSWORD_ENTRY(focus) ||
            gtk_widget_get_ancestor(focus, GTK_TYPE_ENTRY) != NULL ||
            gtk_widget_get_ancestor(focus, GTK_TYPE_SEARCH_ENTRY) != NULL ||
            gtk_widget_get_ancestor(focus, GTK_TYPE_PASSWORD_ENTRY) != NULL);
}

static gboolean
on_window_key_pressed(GtkEventControllerKey *controller G_GNUC_UNUSED,
                      guint keyval,
                      guint keycode G_GNUC_UNUSED,
                      GdkModifierType state,
                      gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    gboolean alt = (state & GDK_ALT_MASK) != 0;
    gboolean ql_open = kajo_quicklook_is_visible(self->quicklook);

    /* 1. Escape closes Quick Look, inline banners, or URI entry */
    if (keyval == GDK_KEY_Escape) {
        if (ql_open) {
            kajo_quicklook_hide(self->quicklook);
            return TRUE;
        }
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->prompt_banner_revealer))) {
            on_prompt_cancel_clicked(NULL, self);
            return TRUE;
        }
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->connect_banner_revealer))) {
            on_connect_cancel_clicked(NULL, self);
            return TRUE;
        }
        if (g_strcmp0(gtk_stack_get_visible_child_name(self->location_stack), "entry") == 0) {
            gtk_stack_set_visible_child_name(self->location_stack, "breadcrumbs");
            return TRUE;
        }
    }

    /* 2. When Quick Look is open, Space / Enter / Arrow keys control Quick Look directly */
    if (ql_open) {
        if (keyval == GDK_KEY_space) {
            kajo_quicklook_hide(self->quicklook);
            return TRUE;
        }
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            KajoFileItem *it = self->selected_item;
            kajo_quicklook_hide(self->quicklook);
            if (it) activate_file_item(self, it);
            return TRUE;
        }
        if (keyval == GDK_KEY_Left || keyval == GDK_KEY_Up) {
            int idx = get_selected_item_index(self);
            if (idx > 0) set_selected_item_by_index(self, idx - 1);
            return TRUE;
        }
        if (keyval == GDK_KEY_Right || keyval == GDK_KEY_Down) {
            int idx = get_selected_item_index(self);
            if (idx >= 0 && self->visible_items && idx + 1 < (int)self->visible_items->len)
                set_selected_item_by_index(self, idx + 1);
            return TRUE;
        }
        return FALSE;
    }

    /* 3. Global Ctrl / Alt / Function Shortcuts */
    if (ctrl && !shift && (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) {
        on_btn_edit_uri_clicked(NULL, self);
        return TRUE;
    }
    if (ctrl && !shift && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
        gtk_widget_grab_focus(self->search_entry);
        return TRUE;
    }
    if (ctrl && !shift && (keyval == GDK_KEY_h || keyval == GDK_KEY_H)) {
        on_btn_hidden_clicked(NULL, self);
        return TRUE;
    }
    if (ctrl && !shift && (keyval == GDK_KEY_i || keyval == GDK_KEY_I)) {
        on_btn_inspector_clicked(NULL, self);
        return TRUE;
    }
    if (ctrl && !shift && keyval == GDK_KEY_1) {
        set_view_mode(self, KAJO_VIEW_GRID);
        return TRUE;
    }
    if (ctrl && !shift && keyval == GDK_KEY_2) {
        set_view_mode(self, KAJO_VIEW_LIST);
        return TRUE;
    }
    if (ctrl && !shift && (keyval == GDK_KEY_r || keyval == GDK_KEY_R)) {
        reload_current_directory(self);
        return TRUE;
    }
    if (ctrl && !shift && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
        GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
        g_autofree char *uri = self->current_dir ? g_file_get_uri(self->current_dir) : NULL;
        GtkWidget *new_win = kajo_files_window_new(app, uri);
        gtk_window_present(GTK_WINDOW(new_win));
        return TRUE;
    }
    if (ctrl && shift && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
        show_inline_prompt(self, PROMPT_MKDIR);
        return TRUE;
    }
    if (ctrl && shift && (keyval == GDK_KEY_t || keyval == GDK_KEY_T)) {
        if (self->current_dir) kajo_files_open_terminal_at(self->current_dir);
        return TRUE;
    }
    if (ctrl && shift && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
        copy_selected_item_path(self);
        return TRUE;
    }
    if (keyval == GDK_KEY_Menu || (shift && keyval == GDK_KEY_F10)) {
        open_context_menu_for_selection(self);
        return TRUE;
    }
    if (alt && keyval == GDK_KEY_Left) {
        on_btn_back_clicked(NULL, self);
        return TRUE;
    }
    if (alt && keyval == GDK_KEY_Right) {
        on_btn_forward_clicked(NULL, self);
        return TRUE;
    }
    if (alt && keyval == GDK_KEY_Up) {
        on_btn_up_clicked(NULL, self);
        return TRUE;
    }
    if (keyval == GDK_KEY_F5) {
        reload_current_directory(self);
        return TRUE;
    }
    if (keyval == GDK_KEY_F2) {
        show_inline_prompt(self, PROMPT_RENAME);
        return TRUE;
    }

    /* 4. Shortcuts that only fire when a text entry is NOT focused */
    if (!is_text_input_focused(GTK_WINDOW(self))) {
        if (keyval == GDK_KEY_period && !ctrl && !alt) {
            open_context_menu_for_selection(self);
            return TRUE;
        }
        if (keyval == GDK_KEY_space && !ctrl && !alt) {
            if (self->selected_item) {
                kajo_quicklook_toggle(self->quicklook);
                return TRUE;
            }
        }
        if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && !ctrl && !alt) {
            if (self->selected_item) {
                activate_file_item(self, self->selected_item);
                return TRUE;
            }
        }
        if (keyval == GDK_KEY_BackSpace && !ctrl && !alt) {
            on_btn_up_clicked(NULL, self);
            return TRUE;
        }
        if (keyval == GDK_KEY_Delete) {
            if (shift)
                show_inline_prompt(self, PROMPT_DELETE_PERM);
            else
                trash_selected_item(self);
            return TRUE;
        }
        if (ctrl && !shift && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
            if (self->selected_item) {
                g_autoptr(GPtrArray) arr = g_ptr_array_new_with_free_func(g_object_unref);
                g_ptr_array_add(arr, g_object_ref(self->selected_item));
                kajo_files_clipboard_set(arr, FALSE);
                gtk_widget_set_sensitive(self->btn_paste, TRUE);
                set_status_message(self, "Copied 1 item to clipboard", FALSE);
                return TRUE;
            }
        }
        if (ctrl && !shift && (keyval == GDK_KEY_x || keyval == GDK_KEY_X)) {
            if (self->selected_item) {
                g_autoptr(GPtrArray) arr = g_ptr_array_new_with_free_func(g_object_unref);
                g_ptr_array_add(arr, g_object_ref(self->selected_item));
                kajo_files_clipboard_set(arr, TRUE);
                gtk_widget_set_sensitive(self->btn_paste, TRUE);
                set_status_message(self, "Cut 1 item to clipboard", FALSE);
                return TRUE;
            }
        }
        if (ctrl && !shift && (keyval == GDK_KEY_v || keyval == GDK_KEY_V)) {
            if (self->current_dir) {
                kajo_files_clipboard_paste_async(self->current_dir, on_file_op_completed, self);
                return TRUE;
            }
        }
    }

    return FALSE;
}

/* ─── Responsive Width Adaptation (< 768px) ─── */

static void
update_responsive_width(KajoFilesWindow *self, int width)
{
    if (!self || width <= 0) return;
    gboolean narrow = (width < 768);
    if (narrow == self->is_narrow) return;

    self->is_narrow = narrow;
    gtk_widget_set_visible(self->btn_sidebar_toggle, narrow);
    gtk_widget_set_visible(self->sidebar_vbox, !narrow);
    gtk_widget_set_visible(self->inspector_vbox, self->inspector_visible && !narrow);
}

static void
on_surface_width_changed(GdkSurface *surface, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    update_responsive_width(self, gdk_surface_get_width(surface));
}

static void
on_window_realize(GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    KajoFilesWindow *self = KAJO_FILES_WINDOW(widget);
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (surface) {
        g_signal_connect_object(surface, "notify::width", G_CALLBACK(on_surface_width_changed), self, 0);
        update_responsive_width(self, gdk_surface_get_width(surface));
    }
}

/* ─── GObject Lifecycle ─── */

static void
kajo_files_window_dispose(GObject *object)
{
    KajoFilesWindow *self = KAJO_FILES_WINDOW(object);

    if (self->monitor_debounce_id > 0) {
        g_source_remove(self->monitor_debounce_id);
        self->monitor_debounce_id = 0;
    }
    if (self->enum_cancel) {
        g_cancellable_cancel(self->enum_cancel);
        g_clear_object(&self->enum_cancel);
    }
    if (self->dir_monitor) {
        g_file_monitor_cancel(self->dir_monitor);
        g_clear_object(&self->dir_monitor);
    }

    g_clear_pointer(&self->context_popover, gtk_widget_unparent);
    g_clear_pointer(&self->sidebar, kajo_files_sidebar_free);
    g_clear_pointer(&self->quicklook, kajo_quicklook_free);
    g_clear_object(&self->current_dir);
    g_clear_object(&self->selected_item);
    g_clear_object(&self->selection_model);
    g_clear_object(&self->visible_store);

    if (self->all_items) {
        g_ptr_array_unref(self->all_items);
        self->all_items = NULL;
    }
    if (self->visible_items) {
        g_ptr_array_unref(self->visible_items);
        self->visible_items = NULL;
    }
    if (self->history) {
        g_ptr_array_unref(self->history);
        self->history = NULL;
    }

    G_OBJECT_CLASS(kajo_files_window_parent_class)->dispose(object);
}

static void
kajo_files_window_class_init(KajoFilesWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = kajo_files_window_dispose;

    gtk_widget_class_set_template_from_resource(widget_class, "/org/kajo/shell/ui/files_window.ui");

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, root_overlay);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sidebar_toggle);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_back);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_forward);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_up);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_reload);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, location_stack);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, breadcrumb_box);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_edit_uri);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, uri_entry);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, search_entry);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_view_grid);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_view_list);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_inspector);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_float);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_close);

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, current_uri_badge);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_new_folder);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_paste);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_hidden);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, sort_popover);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_field_name);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_field_modified);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_field_size);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_field_type);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_asc);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_desc);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_folders_first);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_terminal);

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, sidebar_vbox);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, sidebar_list);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, auth_banner_revealer);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, connect_banner_revealer);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, trash_banner_revealer);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, prompt_banner_revealer);

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, view_stack);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, grid_view);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, col_btn_name);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, col_btn_size);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, col_btn_type);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, col_btn_modified);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, details_listview);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, empty_label);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, inspector_vbox);

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, status_left_label);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, status_right_label);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, fs_usage_bar);
}

static void
kajo_files_window_init(KajoFilesWindow *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->history = g_ptr_array_new_with_free_func(g_free);
    self->history_pos = -1;
    self->view_mode = KAJO_VIEW_GRID;
    self->sort_field = KAJO_SORT_NAME;
    self->sort_ascending = TRUE;
    self->sort_folders_first = TRUE;
    self->show_hidden = FALSE;
    self->inspector_visible = FALSE;

    /* Set up virtualized GListStore + GtkSingleSelection + Factories */
    self->visible_store = g_list_store_new(KAJO_TYPE_FILE_ITEM);
    self->selection_model = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->visible_store)));
    gtk_single_selection_set_autoselect(self->selection_model, TRUE);
    gtk_single_selection_set_can_unselect(self->selection_model, FALSE);
    g_signal_connect(self->selection_model, "notify::selected", G_CALLBACK(on_selection_model_changed), self);

    GtkListItemFactory *grid_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(grid_factory, "setup", G_CALLBACK(on_grid_item_setup), self);
    g_signal_connect(grid_factory, "bind", G_CALLBACK(on_grid_item_bind), self);
    g_signal_connect(grid_factory, "unbind", G_CALLBACK(on_grid_item_unbind), self);
    gtk_grid_view_set_model(self->grid_view, GTK_SELECTION_MODEL(self->selection_model));
    gtk_grid_view_set_factory(self->grid_view, grid_factory);
    g_object_unref(grid_factory);

    GtkListItemFactory *list_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(list_factory, "setup", G_CALLBACK(on_list_item_setup), self);
    g_signal_connect(list_factory, "bind", G_CALLBACK(on_list_item_bind), self);
    g_signal_connect(list_factory, "unbind", G_CALLBACK(on_list_item_unbind), self);
    gtk_list_view_set_model(self->details_listview, GTK_SELECTION_MODEL(self->selection_model));
    gtk_list_view_set_factory(self->details_listview, list_factory);
    g_object_unref(list_factory);

    gtk_widget_add_css_class(self->btn_view_grid, "active");

    build_connect_banner(self);
    build_trash_banner(self);
    build_prompt_banner(self);
    build_context_menu_popover(self);
    sync_sort_ui(self);

    self->quicklook = kajo_quicklook_new(
        self->root_overlay,
        self->inspector_vbox,
        on_quicklook_action,
        self
    );

    self->sidebar = kajo_files_sidebar_new(
        self->sidebar_list,
        self->auth_banner_revealer,
        on_sidebar_navigate,
        on_sidebar_connect_server,
        on_sidebar_status,
        self
    );

    /* Connect Header & Ribbon Signals */
    g_signal_connect(self->btn_sidebar_toggle, "clicked", G_CALLBACK(on_btn_sidebar_toggle_clicked), self);
    g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_btn_back_clicked), self);
    g_signal_connect(self->btn_forward, "clicked", G_CALLBACK(on_btn_forward_clicked), self);
    g_signal_connect(self->btn_up, "clicked", G_CALLBACK(on_btn_up_clicked), self);
    g_signal_connect(self->btn_reload, "clicked", G_CALLBACK(on_btn_reload_clicked), self);
    g_signal_connect(self->btn_edit_uri, "clicked", G_CALLBACK(on_btn_edit_uri_clicked), self);
    g_signal_connect(self->uri_entry, "activate", G_CALLBACK(on_uri_entry_activated), self);
    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);
    g_signal_connect(self->btn_view_grid, "clicked", G_CALLBACK(on_btn_view_grid_clicked), self);
    g_signal_connect(self->btn_view_list, "clicked", G_CALLBACK(on_btn_view_list_clicked), self);
    g_signal_connect(self->btn_inspector, "clicked", G_CALLBACK(on_btn_inspector_clicked), self);
    g_signal_connect(self->btn_float, "clicked", G_CALLBACK(on_btn_float_clicked), self);
    g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_btn_close_clicked), self);

    g_signal_connect(self->btn_new_folder, "clicked", G_CALLBACK(on_btn_new_folder_clicked), self);
    g_signal_connect(self->btn_paste, "clicked", G_CALLBACK(on_btn_paste_clicked), self);
    g_signal_connect(self->btn_hidden, "clicked", G_CALLBACK(on_btn_hidden_clicked), self);
    g_signal_connect(self->btn_terminal, "clicked", G_CALLBACK(on_btn_terminal_clicked), self);

    /* Sort Popover & Column Header Signals */
    g_signal_connect(self->btn_sort_field_name, "clicked", G_CALLBACK(on_sort_field_name_clicked), self);
    g_signal_connect(self->btn_sort_field_modified, "clicked", G_CALLBACK(on_sort_field_modified_clicked), self);
    g_signal_connect(self->btn_sort_field_size, "clicked", G_CALLBACK(on_sort_field_size_clicked), self);
    g_signal_connect(self->btn_sort_field_type, "clicked", G_CALLBACK(on_sort_field_type_clicked), self);
    g_signal_connect(self->btn_sort_asc, "clicked", G_CALLBACK(on_sort_asc_clicked), self);
    g_signal_connect(self->btn_sort_desc, "clicked", G_CALLBACK(on_sort_desc_clicked), self);
    g_signal_connect(self->btn_sort_folders_first, "clicked", G_CALLBACK(on_sort_folders_first_clicked), self);

    g_signal_connect(self->col_btn_name, "clicked", G_CALLBACK(on_col_btn_name_clicked), self);
    g_signal_connect(self->col_btn_size, "clicked", G_CALLBACK(on_col_btn_size_clicked), self);
    g_signal_connect(self->col_btn_type, "clicked", G_CALLBACK(on_col_btn_type_clicked), self);
    g_signal_connect(self->col_btn_modified, "clicked", G_CALLBACK(on_col_btn_modified_clicked), self);

    g_signal_connect(self->grid_view, "activate", G_CALLBACK(on_view_item_activated), self);
    g_signal_connect(self->details_listview, "activate", G_CALLBACK(on_view_item_activated), self);

    /* Secondary Click (Right-Click) Controller for Context Menu */
    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_view_stack_right_click), self);
    gtk_widget_add_controller(GTK_WIDGET(self->view_stack), GTK_EVENT_CONTROLLER(right_click));

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_window_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_ctrl);

    g_signal_connect(self, "realize", G_CALLBACK(on_window_realize), self);
}

GtkWidget *
kajo_files_window_new(GtkApplication *app, const char *initial_uri_or_path)
{
    KajoFilesWindow *win = g_object_new(
        KAJO_TYPE_FILES_WINDOW,
        "application", app,
        NULL
    );
    kajo_files_window_navigate_uri(win, initial_uri_or_path);
    return GTK_WIDGET(win);
}
