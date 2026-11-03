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
    GtkWidget *btn_terminal;

    GtkWidget *sidebar_vbox;
    GtkListBox *sidebar_list;
    GtkWidget *auth_banner_revealer;
    GtkWidget *connect_banner_revealer;
    GtkWidget *trash_banner_revealer;
    GtkWidget *prompt_banner_revealer;

    GtkStack *view_stack;
    GtkFlowBox *grid_flowbox;
    GtkListBox *details_listbox;
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
    KajoFileItem *selected_item;

    GPtrArray *history;        /* char* URIs */
    int history_pos;
    gboolean navigating_history;

    KajoFileViewMode view_mode;
    KajoFileSortField sort_field;
    gboolean sort_ascending;
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
    gtk_widget_add_css_class(card, "section-card-left");
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
    gtk_widget_add_css_class(btn_ok, "files-accent-btn");
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
    gtk_widget_add_css_class(card, "section-card-center");
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
    gtk_widget_add_css_class(card, "section-card-left");
    gtk_widget_add_css_class(card, "files-context-banner");

    self->prompt_title_lbl = gtk_label_new("ACTION:");
    gtk_widget_add_css_class(self->prompt_title_lbl, "heading");

    self->prompt_entry = gtk_entry_new();
    gtk_widget_set_hexpand(self->prompt_entry, TRUE);
    g_signal_connect(self->prompt_entry, "activate", G_CALLBACK(on_prompt_submit_clicked), self);

    self->prompt_submit_btn = gtk_button_new_with_label("CONFIRM");
    gtk_widget_add_css_class(self->prompt_submit_btn, "files-accent-btn");
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

/* ─── View Builders: Metro Tile Grid & Details List ─── */

static GtkWidget *
create_grid_tile_widget(KajoFilesWindow *self G_GNUC_UNUSED, KajoFileItem *item)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "files-tile-card");
    if (kajo_file_item_is_dir(item))
        gtk_widget_add_css_class(card, "files-tile-folder");

    gtk_widget_set_size_request(card, 136, 114);

    const char *ctype = kajo_file_item_get_content_type(item);
    GFile *file = kajo_file_item_get_file(item);
    gboolean is_local_img = (ctype && g_str_has_prefix(ctype, "image/") &&
                             g_file_is_native(file) &&
                             kajo_file_item_get_size(item) <= (15ULL * 1024ULL * 1024ULL));

    if (is_local_img) {
        GtkWidget *pic = gtk_picture_new_for_file(file);
        gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
        gtk_widget_set_size_request(pic, 48, 48);
        gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(pic, 6);
        gtk_box_append(GTK_BOX(card), pic);
    } else {
        GtkWidget *icon = gtk_image_new_from_icon_name(kajo_file_item_get_icon_name(item));
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 42);
        gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(icon, 8);
        gtk_box_append(GTK_BOX(card), icon);
    }

    GtkWidget *name_lbl = gtk_label_new(kajo_file_item_get_display_name(item));
    gtk_widget_add_css_class(name_lbl, "files-tile-title");
    gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(name_lbl), 16);
    gtk_widget_set_halign(name_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), name_lbl);

    g_autofree char *sz_str = kajo_files_format_size(kajo_file_item_get_size(item), kajo_file_item_is_dir(item));
    GtkWidget *sub_lbl = gtk_label_new(sz_str);
    gtk_widget_add_css_class(sub_lbl, "files-tile-sub");
    gtk_widget_set_halign(sub_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card), sub_lbl);

    GtkWidget *fb_child = gtk_flow_box_child_new();
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(fb_child), card);
    g_object_set_data_full(G_OBJECT(fb_child), "file-item", g_object_ref(item), g_object_unref);

    return fb_child;
}

static GtkWidget *
create_list_row_widget(KajoFilesWindow *self G_GNUC_UNUSED, KajoFileItem *item)
{
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(hbox, "files-list-row-box");

    GtkWidget *icon = gtk_image_new_from_icon_name(kajo_file_item_get_icon_name(item));
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);

    GtkWidget *name_lbl = gtk_label_new(kajo_file_item_get_display_name(item));
    gtk_widget_add_css_class(name_lbl, "files-list-name");
    gtk_widget_set_hexpand(name_lbl, TRUE);
    gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);

    g_autofree char *sz_str = kajo_files_format_size(kajo_file_item_get_size(item), kajo_file_item_is_dir(item));
    GtkWidget *sz_lbl = gtk_label_new(sz_str);
    gtk_widget_add_css_class(sz_lbl, "files-list-meta");
    gtk_widget_set_size_request(sz_lbl, 100, -1);
    gtk_widget_set_halign(sz_lbl, GTK_ALIGN_END);

    GtkWidget *type_lbl = gtk_label_new(kajo_file_item_get_type_description(item));
    gtk_widget_add_css_class(type_lbl, "files-list-meta");
    gtk_widget_set_size_request(type_lbl, 160, -1);
    gtk_widget_set_halign(type_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(type_lbl), PANGO_ELLIPSIZE_END);

    g_autofree char *mtime_str = kajo_files_format_mtime(kajo_file_item_get_mtime(item));
    GtkWidget *mtime_lbl = gtk_label_new(mtime_str);
    gtk_widget_add_css_class(mtime_lbl, "files-list-meta");
    gtk_widget_set_size_request(mtime_lbl, 140, -1);
    gtk_widget_set_halign(mtime_lbl, GTK_ALIGN_END);

    gtk_box_append(GTK_BOX(hbox), icon);
    gtk_box_append(GTK_BOX(hbox), name_lbl);
    gtk_box_append(GTK_BOX(hbox), sz_lbl);
    gtk_box_append(GTK_BOX(hbox), type_lbl);
    gtk_box_append(GTK_BOX(hbox), mtime_lbl);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "files-list-row");
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    g_object_set_data_full(G_OBJECT(row), "file-item", g_object_ref(item), g_object_unref);

    return row;
}

typedef struct {
    KajoFileSortField field;
    gboolean ascending;
} SortCtx;

static gint
ptr_array_sort_cb(gconstpointer a, gconstpointer b, gpointer user_data)
{
    const KajoFileItem *item_a = *(const KajoFileItem *const *)a;
    const KajoFileItem *item_b = *(const KajoFileItem *const *)b;
    const SortCtx *ctx = user_data;
    return kajo_file_item_compare(item_a, item_b, ctx->field, ctx->ascending);
}

static void
rebuild_filtered_view(KajoFilesWindow *self)
{
    if (!self) return;

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

    SortCtx sctx = { self->sort_field, self->sort_ascending };
    g_ptr_array_sort_with_data(self->visible_items, ptr_array_sort_cb, &sctx);

    /* Clear Grid & List containers */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->grid_flowbox))) != NULL) {
        gtk_flow_box_remove(self->grid_flowbox, child);
    }
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->details_listbox))) != NULL) {
        gtk_list_box_remove(self->details_listbox, child);
    }

    if (self->visible_items->len == 0) {
        gtk_stack_set_visible_child_name(self->view_stack, "empty");
        g_clear_object(&self->selected_item);
        kajo_quicklook_set_item(self->quicklook, NULL, self->current_dir);
        update_status_and_capacity_bar(self);
        return;
    }

    for (guint i = 0; i < self->visible_items->len; i++) {
        KajoFileItem *it = g_ptr_array_index(self->visible_items, i);
        gtk_flow_box_append(self->grid_flowbox, create_grid_tile_widget(self, it));
        gtk_list_box_append(self->details_listbox, create_list_row_widget(self, it));
    }

    gtk_stack_set_visible_child_name(
        self->view_stack,
        (self->view_mode == KAJO_VIEW_GRID) ? "grid" : "list"
    );

    set_selected_item_by_index(self, 0);
}

/* ─── Selection & Activation ─── */

static int
get_selected_item_index(KajoFilesWindow *self)
{
    if (!self || !self->selected_item || !self->visible_items)
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

    GtkFlowBoxChild *fb_child = gtk_flow_box_get_child_at_index(self->grid_flowbox, idx);
    if (fb_child)
        gtk_flow_box_select_child(self->grid_flowbox, fb_child);

    GtkListBoxRow *lb_row = gtk_list_box_get_row_at_index(self->details_listbox, idx);
    if (lb_row)
        gtk_list_box_select_row(self->details_listbox, lb_row);

    kajo_quicklook_set_item(self->quicklook, item, self->current_dir);
    update_status_and_capacity_bar(self);
}

static void
on_grid_selected_children_changed(GtkFlowBox *box, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    GList *sel = gtk_flow_box_get_selected_children(box);
    if (!sel) return;

    GtkFlowBoxChild *child = GTK_FLOW_BOX_CHILD(sel->data);
    g_list_free(sel);

    KajoFileItem *item = g_object_get_data(G_OBJECT(child), "file-item");
    if (item && item != self->selected_item) {
        g_set_object(&self->selected_item, item);
        int idx = gtk_flow_box_child_get_index(child);
        GtkListBoxRow *lb_row = gtk_list_box_get_row_at_index(self->details_listbox, idx);
        if (lb_row)
            gtk_list_box_select_row(self->details_listbox, lb_row);
        kajo_quicklook_set_item(self->quicklook, item, self->current_dir);
        update_status_and_capacity_bar(self);
    }
}

static void
on_list_row_selected(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!row) return;

    KajoFileItem *item = g_object_get_data(G_OBJECT(row), "file-item");
    if (item && item != self->selected_item) {
        g_set_object(&self->selected_item, item);
        int idx = gtk_list_box_row_get_index(row);
        GtkFlowBoxChild *fb_child = gtk_flow_box_get_child_at_index(self->grid_flowbox, idx);
        if (fb_child)
            gtk_flow_box_select_child(self->grid_flowbox, fb_child);
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
on_grid_child_activated(GtkFlowBox *box G_GNUC_UNUSED, GtkFlowBoxChild *child, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    KajoFileItem *item = g_object_get_data(G_OBJECT(child), "file-item");
    if (item)
        activate_file_item(self, item);
}

static void
on_list_row_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    KajoFileItem *item = g_object_get_data(G_OBJECT(row), "file-item");
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

static void
on_btn_sort_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->sort_ascending) {
        self->sort_ascending = FALSE;
    } else {
        self->sort_ascending = TRUE;
        self->sort_field = (KajoFileSortField)((self->sort_field + 1) % 4);
    }

    const char *names[] = { "NAME", "SIZE", "MODIFIED", "TYPE" };
    g_autofree char *lbl = g_strdup_printf(
        "⇅ SORT: %s %s",
        names[self->sort_field],
        self->sort_ascending ? "▲" : "▼"
    );
    gtk_button_set_label(GTK_BUTTON(self->btn_sort), lbl);
    rebuild_filtered_view(self);
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

    g_clear_pointer(&self->sidebar, kajo_files_sidebar_free);
    g_clear_pointer(&self->quicklook, kajo_quicklook_free);
    g_clear_object(&self->current_dir);
    g_clear_object(&self->selected_item);

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
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_terminal);

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, sidebar_vbox);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, sidebar_list);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, auth_banner_revealer);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, connect_banner_revealer);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, trash_banner_revealer);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, prompt_banner_revealer);

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, view_stack);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, grid_flowbox);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, details_listbox);
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
    self->show_hidden = FALSE;
    self->inspector_visible = FALSE;

    gtk_widget_add_css_class(self->btn_view_grid, "active");

    build_connect_banner(self);
    build_trash_banner(self);
    build_prompt_banner(self);

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
    g_signal_connect(self->btn_sort, "clicked", G_CALLBACK(on_btn_sort_clicked), self);
    g_signal_connect(self->btn_terminal, "clicked", G_CALLBACK(on_btn_terminal_clicked), self);

    g_signal_connect(self->grid_flowbox, "selected-children-changed", G_CALLBACK(on_grid_selected_children_changed), self);
    g_signal_connect(self->grid_flowbox, "child-activated", G_CALLBACK(on_grid_child_activated), self);
    g_signal_connect(self->details_listbox, "row-selected", G_CALLBACK(on_list_row_selected), self);
    g_signal_connect(self->details_listbox, "row-activated", G_CALLBACK(on_list_row_activated), self);

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
