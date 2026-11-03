#include "files_sidebar.h"
#include "files_model.h"
#include <string.h>

struct _KajoFilesSidebar {
    GtkListBox *list_box;
    GtkWidget *auth_revealer;
    GtkWidget *auth_title_lbl;
    GtkWidget *auth_user_entry;
    GtkWidget *auth_domain_entry;
    GtkWidget *auth_pass_entry;

    GVolumeMonitor *vol_monitor;
    GMountOperation *active_mount_op;
    GFile *pending_nav_file;

    KajoSidebarNavigateFunc nav_cb;
    KajoSidebarConnectServerFunc connect_cb;
    KajoSidebarStatusFunc status_cb;
    gpointer user_data;

    char *current_uri;
    gboolean suppress_select;
};

static void
emit_status(KajoFilesSidebar *self, const char *msg, gboolean is_err)
{
    if (self && self->status_cb && msg)
        self->status_cb(msg, is_err, self->user_data);
}

/* ─── Inline GVFS / LUKS Authentication Banner ─── */

static void
on_auth_submit_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesSidebar *self = user_data;
    if (!self->active_mount_op) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->auth_revealer), FALSE);
        return;
    }

    const char *user = gtk_editable_get_text(GTK_EDITABLE(self->auth_user_entry));
    const char *domain = gtk_editable_get_text(GTK_EDITABLE(self->auth_domain_entry));
    const char *pass = gtk_editable_get_text(GTK_EDITABLE(self->auth_pass_entry));

    if (gtk_widget_get_visible(self->auth_user_entry) && user && user[0] != '\0')
        g_mount_operation_set_username(self->active_mount_op, user);
    if (gtk_widget_get_visible(self->auth_domain_entry) && domain && domain[0] != '\0')
        g_mount_operation_set_domain(self->active_mount_op, domain);
    if (pass)
        g_mount_operation_set_password(self->active_mount_op, pass);

    gtk_editable_set_text(GTK_EDITABLE(self->auth_pass_entry), "");
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->auth_revealer), FALSE);

    g_mount_operation_reply(self->active_mount_op, G_MOUNT_OPERATION_HANDLED);
}

static void
on_auth_cancel_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesSidebar *self = user_data;
    gtk_editable_set_text(GTK_EDITABLE(self->auth_pass_entry), "");
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->auth_revealer), FALSE);

    if (self->active_mount_op)
        g_mount_operation_reply(self->active_mount_op, G_MOUNT_OPERATION_ABORTED);
}

static void
build_auth_banner_ui(KajoFilesSidebar *self)
{
    if (!self->auth_revealer) return;

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "section-card-left");
    gtk_widget_add_css_class(card, "files-context-banner");

    self->auth_title_lbl = gtk_label_new("🔒 AUTHENTICATION REQUIRED");
    gtk_widget_add_css_class(self->auth_title_lbl, "heading");
    gtk_widget_set_halign(self->auth_title_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(card), self->auth_title_lbl);

    GtkWidget *fields_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    self->auth_user_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->auth_user_entry), "Username");
    gtk_widget_set_hexpand(self->auth_user_entry, TRUE);

    self->auth_domain_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->auth_domain_entry), "Domain (WORKGROUP)");

    self->auth_pass_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(self->auth_pass_entry), TRUE);
    gtk_widget_set_hexpand(self->auth_pass_entry, TRUE);
    g_signal_connect(self->auth_pass_entry, "activate", G_CALLBACK(on_auth_submit_clicked), self);

    GtkWidget *btn_mount = gtk_button_new_with_label("MOUNT / UNLOCK");
    gtk_widget_add_css_class(btn_mount, "files-accent-btn");
    g_signal_connect(btn_mount, "clicked", G_CALLBACK(on_auth_submit_clicked), self);

    GtkWidget *btn_cancel = gtk_button_new_with_label("CANCEL");
    gtk_widget_add_css_class(btn_cancel, "flat");
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_auth_cancel_clicked), self);

    gtk_box_append(GTK_BOX(fields_hbox), self->auth_user_entry);
    gtk_box_append(GTK_BOX(fields_hbox), self->auth_domain_entry);
    gtk_box_append(GTK_BOX(fields_hbox), self->auth_pass_entry);
    gtk_box_append(GTK_BOX(fields_hbox), btn_mount);
    gtk_box_append(GTK_BOX(fields_hbox), btn_cancel);

    gtk_box_append(GTK_BOX(card), fields_hbox);
    gtk_revealer_set_child(GTK_REVEALER(self->auth_revealer), card);
}

static void
on_mount_op_ask_password(GMountOperation *op,
                         gchar *message,
                         gchar *default_user,
                         gchar *default_domain,
                         GAskPasswordFlags flags,
                         gpointer user_data)
{
    KajoFilesSidebar *self = user_data;
    g_set_object(&self->active_mount_op, op);

    g_autofree char *title = g_strdup_printf("🔒 %s", message ? message : "AUTHENTICATION / PASSPHRASE REQUIRED");
    gtk_label_set_text(GTK_LABEL(self->auth_title_lbl), title);

    gboolean need_user = (flags & G_ASK_PASSWORD_NEED_USERNAME) != 0;
    gboolean need_domain = (flags & G_ASK_PASSWORD_NEED_DOMAIN) != 0;

    gtk_widget_set_visible(self->auth_user_entry, need_user);
    gtk_widget_set_visible(self->auth_domain_entry, need_domain);

    if (need_user && default_user)
        gtk_editable_set_text(GTK_EDITABLE(self->auth_user_entry), default_user);
    if (need_domain && default_domain)
        gtk_editable_set_text(GTK_EDITABLE(self->auth_domain_entry), default_domain);

    gtk_editable_set_text(GTK_EDITABLE(self->auth_pass_entry), "");
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->auth_revealer), TRUE);
    gtk_widget_grab_focus(self->auth_pass_entry);
}

static GMountOperation *
create_kajo_mount_operation(KajoFilesSidebar *self)
{
    GMountOperation *op = g_mount_operation_new();
    g_signal_connect(op, "ask-password", G_CALLBACK(on_mount_op_ask_password), self);
    return op;
}

/* ─── Mount / Eject / Unmount Callbacks ─── */

static void
on_volume_mount_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    KajoFilesSidebar *self = user_data;
    GVolume *vol = G_VOLUME(source);
    GError *error = NULL;

    if (!g_volume_mount_finish(vol, res, &error)) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
            emit_status(self, error ? error->message : "Mount failed", TRUE);
        g_clear_error(&error);
        return;
    }

    g_autoptr(GMount) mount = g_volume_get_mount(vol);
    if (mount) {
        g_autoptr(GFile) root = g_mount_get_root(mount);
        if (root && self->nav_cb) {
            g_autofree char *uri = g_file_get_uri(root);
            self->nav_cb(uri, self->user_data);
        }
    }
    kajo_files_sidebar_refresh(self);
}

static void
on_enclosing_mount_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    KajoFilesSidebar *self = user_data;
    GFile *file = G_FILE(source);
    GError *error = NULL;

    if (!g_file_mount_enclosing_volume_finish(file, res, &error)) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED) &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
            emit_status(self, error ? error->message : "Failed to connect to remote share", TRUE);
            g_clear_error(&error);
            return;
        }
        g_clear_error(&error);
    }

    g_autofree char *uri = g_file_get_uri(file);
    kajo_files_sidebar_refresh(self);
    if (uri && self->nav_cb)
        self->nav_cb(uri, self->user_data);
}

void
kajo_files_sidebar_mount_uri(KajoFilesSidebar *self, GFile *target_file)
{
    g_return_if_fail(self != NULL && G_IS_FILE(target_file));

    g_autoptr(GMountOperation) op = create_kajo_mount_operation(self);
    g_file_mount_enclosing_volume(
        target_file,
        G_MOUNT_MOUNT_NONE,
        op,
        NULL,
        on_enclosing_mount_ready,
        self
    );
}

static void
on_mount_eject_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    KajoFilesSidebar *self = user_data;
    GError *error = NULL;
    if (!g_mount_eject_with_operation_finish(G_MOUNT(source), res, &error)) {
        emit_status(self, error ? error->message : "Failed to eject device", TRUE);
        g_clear_error(&error);
    } else {
        emit_status(self, "Safely ejected device", FALSE);
    }
    kajo_files_sidebar_refresh(self);
}

static void
on_mount_unmount_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    KajoFilesSidebar *self = user_data;
    GError *error = NULL;
    if (!g_mount_unmount_with_operation_finish(G_MOUNT(source), res, &error)) {
        emit_status(self, error ? error->message : "Failed to unmount volume", TRUE);
        g_clear_error(&error);
    } else {
        emit_status(self, "Unmounted volume", FALSE);
    }
    kajo_files_sidebar_refresh(self);
}

static void
on_eject_btn_clicked(GtkButton *btn, gpointer user_data)
{
    KajoFilesSidebar *self = user_data;
    GMount *mount = g_object_get_data(G_OBJECT(btn), "gmount");
    if (!G_IS_MOUNT(mount)) return;

    g_autoptr(GMountOperation) op = create_kajo_mount_operation(self);
    if (g_mount_can_eject(mount)) {
        g_mount_eject_with_operation(mount, G_MOUNT_UNMOUNT_NONE, op, NULL, on_mount_eject_ready, self);
    } else {
        g_mount_unmount_with_operation(mount, G_MOUNT_UNMOUNT_NONE, op, NULL, on_mount_unmount_ready, self);
    }
}

/* ─── Sidebar Row Builders (Matching kajo-settings) ─── */

static void
append_category_header(GtkListBox *box, const char *title)
{
    GtkWidget *lbl = gtk_label_new(title);
    gtk_widget_add_css_class(lbl, "sidebar-category-header");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_start(lbl, 12);
    gtk_widget_set_margin_top(lbl, 12);
    gtk_widget_set_margin_bottom(lbl, 4);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "sidebar-header-row");
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
    gtk_list_box_append(box, row);
}

static void
append_place_row(GtkListBox *box,
                 const char *icon_name,
                 const char *title,
                 const char *subtitle,
                 const char *uri,
                 const char *action_tag)
{
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *lbl = gtk_label_new(title);
    gtk_widget_add_css_class(lbl, "sidebar-label");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(vbox), lbl);

    if (subtitle && subtitle[0] != '\0') {
        GtkWidget *sub = gtk_label_new(subtitle);
        gtk_widget_add_css_class(sub, "shell-popover-subtitle");
        gtk_widget_set_halign(sub, GTK_ALIGN_START);
        gtk_widget_set_valign(sub, GTK_ALIGN_CENTER);
        gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(vbox), sub);
    }

    GtkWidget *chevron = gtk_label_new("›");
    gtk_widget_add_css_class(chevron, "sidebar-chevron");
    gtk_widget_set_valign(chevron, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hbox), icon);
    gtk_box_append(GTK_BOX(hbox), vbox);
    gtk_box_append(GTK_BOX(hbox), chevron);

    if (uri)
        g_object_set_data_full(G_OBJECT(hbox), "target-uri", g_strdup(uri), g_free);
    if (action_tag)
        g_object_set_data_full(G_OBJECT(hbox), "action-tag", g_strdup(action_tag), g_free);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    gtk_list_box_append(box, row);
}

static void
append_drive_row(KajoFilesSidebar *self,
                 const char *icon_name,
                 const char *title,
                 GFile *mount_root,
                 GMount *mount,
                 GVolume *unmounted_vol)
{
    GtkWidget *vbox_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(vbox_outer, GTK_ALIGN_CENTER);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *lbl = gtk_label_new(title);
    gtk_widget_add_css_class(lbl, "sidebar-label");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(vbox), lbl);

    gtk_box_append(GTK_BOX(hbox), icon);
    gtk_box_append(GTK_BOX(hbox), vbox);

    if (mount && (g_mount_can_unmount(mount) || g_mount_can_eject(mount))) {
        GtkWidget *eject_btn = gtk_button_new_with_label("⏏");
        gtk_widget_add_css_class(eject_btn, "flat");
        gtk_widget_add_css_class(eject_btn, "files-eject-btn");
        gtk_widget_set_valign(eject_btn, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(eject_btn, "Unmount / Safely Eject");
        g_object_set_data_full(G_OBJECT(eject_btn), "gmount", g_object_ref(mount), g_object_unref);
        g_signal_connect(eject_btn, "clicked", G_CALLBACK(on_eject_btn_clicked), self);
        gtk_box_append(GTK_BOX(hbox), eject_btn);
    }

    gtk_box_append(GTK_BOX(vbox_outer), hbox);

    if (mount_root) {
        g_autofree char *uri = g_file_get_uri(mount_root);
        g_object_set_data_full(G_OBJECT(vbox_outer), "target-uri", g_strdup(uri), g_free);

        g_autoptr(GFileInfo) fs_info = g_file_query_filesystem_info(
            mount_root,
            G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
            NULL,
            NULL
        );
        if (fs_info && g_file_info_has_attribute(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
            guint64 total = g_file_info_get_attribute_uint64(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
            guint64 free_b = g_file_info_get_attribute_uint64(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
            if (total > 0) {
                double used_frac = (double)(total - free_b) / (double)total;
                used_frac = CLAMP(used_frac, 0.0, 1.0);

                GtkWidget *bar = gtk_level_bar_new_for_interval(0.0, 1.0);
                gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), used_frac);
                gtk_widget_add_css_class(bar, "files-drive-bar");
                gtk_box_append(GTK_BOX(vbox), bar);

                g_autofree char *free_str = kajo_files_format_size(free_b, FALSE);
                g_autofree char *tot_str = kajo_files_format_size(total, FALSE);
                g_autofree char *cap_txt = g_strdup_printf("%s free / %s", free_str, tot_str);
                GtkWidget *sub = gtk_label_new(cap_txt);
                gtk_widget_add_css_class(sub, "shell-popover-subtitle");
                gtk_widget_set_halign(sub, GTK_ALIGN_START);
                gtk_box_append(GTK_BOX(vbox), sub);
            }
        }
    } else if (unmounted_vol) {
        GtkWidget *sub = gtk_label_new("[ UNMOUNTED — CLICK TO MOUNT ]");
        gtk_widget_add_css_class(sub, "shell-popover-subtitle");
        gtk_widget_set_halign(sub, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(vbox), sub);
        g_object_set_data_full(G_OBJECT(vbox_outer), "gvolume", g_object_ref(unmounted_vol), g_object_unref);
    }

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), vbox_outer);
    gtk_list_box_append(self->list_box, row);
}

static guint
query_trash_item_count(void)
{
    g_autoptr(GFile) trash = g_file_new_for_uri("trash:///");
    g_autoptr(GFileInfo) info = g_file_query_info(
        trash,
        G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT,
        G_FILE_QUERY_INFO_NONE,
        NULL,
        NULL
    );
    if (info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT))
        return g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT);
    return 0;
}

void
kajo_files_sidebar_refresh(KajoFilesSidebar *self)
{
    if (!self || !GTK_IS_LIST_BOX(self->list_box))
        return;

    self->suppress_select = TRUE;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != NULL) {
        gtk_list_box_remove(self->list_box, child);
    }

    /* 1. PLACES */
    append_category_header(self->list_box, "PLACES");

    g_autoptr(GFile) home_file = g_file_new_for_path(g_get_home_dir());
    g_autofree char *home_uri = g_file_get_uri(home_file);
    append_place_row(self->list_box, "user-home-symbolic", "Home", NULL, home_uri, NULL);

    struct {
        GUserDirectory dir;
        const char *icon;
        const char *fallback_name;
    } xdg_dirs[] = {
        { G_USER_DIRECTORY_DESKTOP,   "user-desktop-symbolic",     "Desktop"   },
        { G_USER_DIRECTORY_DOCUMENTS, "folder-documents-symbolic", "Documents" },
        { G_USER_DIRECTORY_DOWNLOAD,  "folder-download-symbolic",  "Downloads" },
        { G_USER_DIRECTORY_MUSIC,     "folder-music-symbolic",     "Music"     },
        { G_USER_DIRECTORY_PICTURES,  "folder-pictures-symbolic",  "Pictures"  },
        { G_USER_DIRECTORY_VIDEOS,    "folder-videos-symbolic",    "Videos"    },
    };

    for (guint i = 0; i < G_N_ELEMENTS(xdg_dirs); i++) {
        const char *path = g_get_user_special_dir(xdg_dirs[i].dir);
        if (path && g_file_test(path, G_FILE_TEST_IS_DIR)) {
            g_autoptr(GFile) f = g_file_new_for_path(path);
            g_autofree char *uri = g_file_get_uri(f);
            append_place_row(self->list_box, xdg_dirs[i].icon, xdg_dirs[i].fallback_name, NULL, uri, NULL);
        }
    }

    append_place_row(self->list_box, "document-open-recent-symbolic", "Recent", NULL, "recent:///", NULL);

    guint trash_cnt = query_trash_item_count();
    g_autofree char *trash_lbl = (trash_cnt > 0)
        ? g_strdup_printf("Trash (%u)", trash_cnt)
        : g_strdup("Trash");
    append_place_row(self->list_box, "user-trash-symbolic", trash_lbl, NULL, "trash:///", NULL);

    /* 2. DEVICES & DRIVES */
    append_category_header(self->list_box, "DEVICES & DRIVES");

    g_autoptr(GFile) root_file = g_file_new_for_path("/");
    append_drive_row(self, "drive-harddisk-system-symbolic", "System Root (/)", root_file, NULL, NULL);

    if (self->vol_monitor) {
        GList *mounts = g_volume_monitor_get_mounts(self->vol_monitor);
        for (GList *l = mounts; l != NULL; l = l->next) {
            GMount *m = G_MOUNT(l->data);
            g_autoptr(GFile) m_root = g_mount_get_root(m);
            g_autofree char *m_name = g_mount_get_name(m);
            append_drive_row(
                self,
                "drive-removable-media-symbolic",
                m_name ? m_name : "Mounted Volume",
                m_root,
                m,
                NULL
            );
            g_object_unref(m);
        }
        g_list_free(mounts);

        GList *volumes = g_volume_monitor_get_volumes(self->vol_monitor);
        for (GList *l = volumes; l != NULL; l = l->next) {
            GVolume *v = G_VOLUME(l->data);
            g_autoptr(GMount) m = g_volume_get_mount(v);
            if (!m) {
                g_autofree char *v_name = g_volume_get_name(v);
                append_drive_row(
                    self,
                    "drive-harddisk-symbolic",
                    v_name ? v_name : "Unmounted Volume",
                    NULL,
                    NULL,
                    v
                );
            }
            g_object_unref(v);
        }
        g_list_free(volumes);
    }

    /* 3. NETWORK */
    append_category_header(self->list_box, "NETWORK");
    append_place_row(self->list_box, "network-workgroup-symbolic", "Browse Network", "SMB / SFTP / WSDD", "network:///", NULL);
    append_place_row(self->list_box, "network-server-symbolic", "+ CONNECT TO SERVER", "smb://, sftp://, davs://", NULL, "connect-server");

    self->suppress_select = FALSE;
    if (self->current_uri)
        kajo_files_sidebar_sync_active_uri(self, self->current_uri);
}

void
kajo_files_sidebar_sync_active_uri(KajoFilesSidebar *self, const char *active_uri)
{
    if (!self || !active_uri) return;

    if (g_strcmp0(self->current_uri, active_uri) != 0) {
        g_free(self->current_uri);
        self->current_uri = g_strdup(active_uri);
    }

    self->suppress_select = TRUE;
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box));
    gboolean matched = FALSE;

    while (child) {
        if (GTK_IS_LIST_BOX_ROW(child)) {
            GtkWidget *row_child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));
            if (row_child) {
                const char *row_uri = g_object_get_data(G_OBJECT(row_child), "target-uri");
                if (row_uri && g_strcmp0(row_uri, active_uri) == 0) {
                    gtk_list_box_select_row(self->list_box, GTK_LIST_BOX_ROW(child));
                    matched = TRUE;
                    break;
                }
            }
        }
        child = gtk_widget_get_next_sibling(child);
    }

    if (!matched)
        gtk_list_box_unselect_all(self->list_box);

    self->suppress_select = FALSE;
}

static void
on_sidebar_row_selected(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
    KajoFilesSidebar *self = user_data;
    if (!row || self->suppress_select)
        return;

    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!child) return;

    const char *action_tag = g_object_get_data(G_OBJECT(child), "action-tag");
    if (g_strcmp0(action_tag, "connect-server") == 0) {
        if (self->connect_cb)
            self->connect_cb(self->user_data);
        return;
    }

    GVolume *vol = g_object_get_data(G_OBJECT(child), "gvolume");
    if (G_IS_VOLUME(vol)) {
        emit_status(self, "Mounting volume...", FALSE);
        g_autoptr(GMountOperation) op = create_kajo_mount_operation(self);
        g_volume_mount(vol, G_MOUNT_MOUNT_NONE, op, NULL, on_volume_mount_ready, self);
        return;
    }

    const char *target_uri = g_object_get_data(G_OBJECT(child), "target-uri");
    if (target_uri && self->nav_cb) {
        self->nav_cb(target_uri, self->user_data);
    }
}

static void
on_vol_monitor_changed(GVolumeMonitor *monitor G_GNUC_UNUSED,
                       gpointer item G_GNUC_UNUSED,
                       gpointer user_data)
{
    KajoFilesSidebar *self = user_data;
    kajo_files_sidebar_refresh(self);
}

KajoFilesSidebar *
kajo_files_sidebar_new(GtkListBox *list_box,
                       GtkWidget *auth_banner_revealer,
                       KajoSidebarNavigateFunc nav_cb,
                       KajoSidebarConnectServerFunc connect_cb,
                       KajoSidebarStatusFunc status_cb,
                       gpointer user_data)
{
    KajoFilesSidebar *self = g_new0(KajoFilesSidebar, 1);
    self->list_box = list_box;
    self->auth_revealer = auth_banner_revealer;
    self->nav_cb = nav_cb;
    self->connect_cb = connect_cb;
    self->status_cb = status_cb;
    self->user_data = user_data;

    build_auth_banner_ui(self);

    self->vol_monitor = g_volume_monitor_get();
    if (self->vol_monitor) {
        g_signal_connect(self->vol_monitor, "volume-added", G_CALLBACK(on_vol_monitor_changed), self);
        g_signal_connect(self->vol_monitor, "volume-removed", G_CALLBACK(on_vol_monitor_changed), self);
        g_signal_connect(self->vol_monitor, "volume-changed", G_CALLBACK(on_vol_monitor_changed), self);
        g_signal_connect(self->vol_monitor, "mount-added", G_CALLBACK(on_vol_monitor_changed), self);
        g_signal_connect(self->vol_monitor, "mount-removed", G_CALLBACK(on_vol_monitor_changed), self);
        g_signal_connect(self->vol_monitor, "mount-changed", G_CALLBACK(on_vol_monitor_changed), self);
    }

    g_signal_connect(self->list_box, "row-selected", G_CALLBACK(on_sidebar_row_selected), self);
    kajo_files_sidebar_refresh(self);

    return self;
}

void
kajo_files_sidebar_free(KajoFilesSidebar *self)
{
    if (!self) return;
    if (self->vol_monitor) {
        g_signal_handlers_disconnect_by_data(self->vol_monitor, self);
        g_clear_object(&self->vol_monitor);
    }
    g_clear_object(&self->active_mount_op);
    g_clear_object(&self->pending_nav_file);
    g_free(self->current_uri);
    g_free(self);
}
