#include "files_window_private.h"

/* ─── Interactive Breadcrumb Bar ─── */

static void
sync_breadcrumb_scroll_to_end(KajoFilesWindow *self)
{
    if (!self || !self->breadcrumb_scroll || !self->pending_breadcrumb_scroll_end)
        return;

    GtkWidget *last_child = self->breadcrumb_box ? gtk_widget_get_last_child(self->breadcrumb_box) : NULL;
    if (!last_child || gtk_widget_get_width(last_child) <= 0)
        return;

    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(self->breadcrumb_scroll);
    if (!hadj)
        return;

    double upper = gtk_adjustment_get_upper(hadj);
    double page = gtk_adjustment_get_page_size(hadj);
    if (page > 0.0) {
        if (upper > page) {
            double target = upper - page;
            if (fabs(gtk_adjustment_get_value(hadj) - target) > 0.5) {
                gtk_adjustment_set_value(hadj, target);
            }
        }
        self->pending_breadcrumb_scroll_end = FALSE;
    }
}

static gboolean
on_breadcrumb_scroll_idle(gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->breadcrumb_scroll_idle_id = 0;
    sync_breadcrumb_scroll_to_end(self);
    return G_SOURCE_REMOVE;
}

static void
schedule_breadcrumb_scroll_to_end(KajoFilesWindow *self)
{
    if (!self || !self->pending_breadcrumb_scroll_end)
        return;
    if (self->breadcrumb_scroll_idle_id == 0) {
        self->breadcrumb_scroll_idle_id = g_idle_add(on_breadcrumb_scroll_idle, self);
    }
}

static void
on_breadcrumb_hadj_changed(GtkAdjustment *hadj G_GNUC_UNUSED, gpointer user_data)
{
    schedule_breadcrumb_scroll_to_end(KAJO_FILES_WINDOW(user_data));
}

static void
on_breadcrumb_hadj_notify(GtkAdjustment *hadj G_GNUC_UNUSED,
                          GParamSpec *pspec G_GNUC_UNUSED,
                          gpointer user_data)
{
    schedule_breadcrumb_scroll_to_end(KAJO_FILES_WINDOW(user_data));
}

static gboolean
on_breadcrumb_wheel_scroll(GtkEventControllerScroll *ctrl G_GNUC_UNUSED,
                           double dx,
                           double dy,
                           gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self || !self->breadcrumb_scroll)
        return FALSE;

    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(self->breadcrumb_scroll);
    if (!hadj)
        return FALSE;

    double delta = (fabs(dx) > fabs(dy)) ? dx : dy;
    if (fabs(delta) < 1e-4)
        return FALSE;

    self->pending_breadcrumb_scroll_end = FALSE;
    double val = gtk_adjustment_get_value(hadj);
    double upper = gtk_adjustment_get_upper(hadj);
    double page = gtk_adjustment_get_page_size(hadj);
    double max_val = MAX(0.0, upper - page);

    gtk_adjustment_set_value(hadj, CLAMP(val + delta * 48.0, 0.0, max_val));
    return TRUE;
}

static void
on_bc_pop_subdir_clicked(GtkButton *btn, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *uri = g_object_get_data(G_OBJECT(btn), "target-uri");
    if (self->breadcrumb_popover)
        gtk_popover_popdown(GTK_POPOVER(self->breadcrumb_popover));
    if (uri)
        kajo_files_window_navigate_uri(self, uri);
}

static void
on_bc_pop_newwin_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->breadcrumb_popover)
        gtk_popover_popdown(GTK_POPOVER(self->breadcrumb_popover));
    if (!self->bc_pop_target_uri)
        return;

    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
    GtkWidget *win = kajo_files_window_new(app, self->bc_pop_target_uri);
    gtk_window_present(GTK_WINDOW(win));
}

static void
on_bc_pop_copypath_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->breadcrumb_popover)
        gtk_popover_popdown(GTK_POPOVER(self->breadcrumb_popover));
    if (!self->bc_pop_target_uri)
        return;

    g_autoptr(GFile) f = g_file_new_for_uri(self->bc_pop_target_uri);
    g_autofree char *local_path = f ? g_file_get_path(f) : NULL;
    const char *text = local_path ? local_path : self->bc_pop_target_uri;

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GdkClipboard *clipboard = gdk_display_get_clipboard(display);
        gdk_clipboard_set_text(clipboard, text);
    }
    g_autofree char *msg = g_strdup_printf("Copied path: %s", text);
    files_window_set_status_message(self, msg, FALSE);
}

static gint
compare_bc_dir_items(gconstpointer a, gconstpointer b)
{
    const KajoFileItem *ia = *(const KajoFileItem *const *)a;
    const KajoFileItem *ib = *(const KajoFileItem *const *)b;
    return kajo_file_item_compare_full(ia, ib, KAJO_SORT_NAME, TRUE, TRUE);
}

static void
on_breadcrumb_subdirs_ready(GPtrArray *items, GError *error, gpointer user_data)
{
    GWeakRef *wr = user_data;
    g_autoptr(KajoFilesWindow) self = g_weak_ref_get(wr);
    g_weak_ref_clear(wr);
    g_free(wr);

    if (!self || !self->bc_pop_list_box) {
        if (items) g_ptr_array_unref(items);
        return;
    }

    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (items) g_ptr_array_unref(items);
        return;
    }

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->bc_pop_list_box)) != NULL) {
        gtk_box_remove(GTK_BOX(self->bc_pop_list_box), child);
    }

    g_autoptr(GPtrArray) subdirs = g_ptr_array_new_with_free_func(g_object_unref);
    if (items) {
        for (guint i = 0; i < items->len; i++) {
            KajoFileItem *it = g_ptr_array_index(items, i);
            if (!kajo_file_item_is_dir(it))
                continue;
            if (!self->show_hidden && kajo_file_item_is_hidden(it))
                continue;
            g_ptr_array_add(subdirs, g_object_ref(it));
        }
        g_ptr_array_unref(items);
    }

    g_ptr_array_sort(subdirs, compare_bc_dir_items);

    if (subdirs->len == 0) {
        GtkWidget *empty_lbl = gtk_label_new(error ? error->message : "No subdirectories");
        gtk_widget_add_css_class(empty_lbl, "shell-popover-subtitle");
        gtk_widget_set_halign(empty_lbl, GTK_ALIGN_START);
        gtk_widget_set_margin_start(empty_lbl, 8);
        gtk_widget_set_margin_top(empty_lbl, 4);
        gtk_widget_set_margin_bottom(empty_lbl, 4);
        gtk_box_append(GTK_BOX(self->bc_pop_list_box), empty_lbl);
        return;
    }

    guint limit = MIN(subdirs->len, 50U);
    for (guint i = 0; i < limit; i++) {
        KajoFileItem *it = g_ptr_array_index(subdirs, i);
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

        GtkWidget *ic = gtk_image_new_from_icon_name("folder");
        gtk_image_set_pixel_size(GTK_IMAGE(ic), 16);
        gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);

        GtkWidget *lbl = gtk_label_new(kajo_file_item_get_display_name(it));
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);

        GtkWidget *chev = gtk_label_new("›");
        gtk_widget_add_css_class(chev, "sidebar-chevron");
        gtk_widget_set_valign(chev, GTK_ALIGN_CENTER);

        gtk_box_append(GTK_BOX(hbox), ic);
        gtk_box_append(GTK_BOX(hbox), lbl);
        gtk_box_append(GTK_BOX(hbox), chev);

        GtkWidget *row_btn = gtk_button_new();
        gtk_widget_add_css_class(row_btn, "flat");
        gtk_widget_add_css_class(row_btn, "files-menu-row");
        gtk_button_set_child(GTK_BUTTON(row_btn), hbox);
        g_object_set_data_full(G_OBJECT(row_btn), "target-uri", g_strdup(kajo_file_item_get_uri(it)), g_free);
        g_signal_connect(row_btn, "clicked", G_CALLBACK(on_bc_pop_subdir_clicked), self);
        gtk_box_append(GTK_BOX(self->bc_pop_list_box), row_btn);
    }

    if (subdirs->len > limit) {
        g_autofree char *more_txt = g_strdup_printf("… and %u more subfolder(s)", subdirs->len - limit);
        GtkWidget *more_lbl = gtk_label_new(more_txt);
        gtk_widget_add_css_class(more_lbl, "shell-popover-subtitle");
        gtk_widget_set_halign(more_lbl, GTK_ALIGN_START);
        gtk_widget_set_margin_start(more_lbl, 8);
        gtk_widget_set_margin_top(more_lbl, 4);
        gtk_widget_set_margin_bottom(more_lbl, 2);
        gtk_box_append(GTK_BOX(self->bc_pop_list_box), more_lbl);
    }
}

void
files_window_views_init_breadcrumbs(KajoFilesWindow *self)
{
    self->breadcrumb_popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(self->breadcrumb_popover), FALSE);
    gtk_widget_add_css_class(self->breadcrumb_popover, "shell-popover");
    gtk_widget_add_css_class(self->breadcrumb_popover, "files-context-popover");
    gtk_widget_set_parent(self->breadcrumb_popover, GTK_WIDGET(self->location_stack));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(vbox, 260, -1);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hdr, "shell-popover-header");
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

    GtkWidget *hdr_icon = gtk_image_new_from_icon_name("folder");
    gtk_image_set_pixel_size(GTK_IMAGE(hdr_icon), 16);
    gtk_widget_set_valign(hdr_icon, GTK_ALIGN_CENTER);

    self->bc_pop_hdr_lbl = gtk_label_new("FOLDER");
    gtk_widget_add_css_class(self->bc_pop_hdr_lbl, "heading");
    gtk_widget_set_hexpand(self->bc_pop_hdr_lbl, TRUE);
    gtk_widget_set_halign(self->bc_pop_hdr_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(self->bc_pop_hdr_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(self->bc_pop_hdr_lbl), PANGO_ELLIPSIZE_MIDDLE);

    GtkWidget *badge = gtk_label_new("SUBFOLDERS");
    gtk_widget_add_css_class(badge, "shell-popover-badge");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hdr), hdr_icon);
    gtk_box_append(GTK_BOX(hdr), self->bc_pop_hdr_lbl);
    gtk_box_append(GTK_BOX(hdr), badge);
    gtk_box_append(GTK_BOX(vbox), hdr);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(body, "shell-popover-body");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 220);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);

    self->bc_pop_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), self->bc_pop_list_box);
    gtk_box_append(GTK_BOX(body), scroll);

    gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *btn_newwin = gtk_button_new_with_label("🗔  Open in New Window");
    gtk_widget_add_css_class(btn_newwin, "flat");
    gtk_widget_add_css_class(btn_newwin, "files-menu-row");
    g_signal_connect(btn_newwin, "clicked", G_CALLBACK(on_bc_pop_newwin_clicked), self);
    gtk_box_append(GTK_BOX(body), btn_newwin);

    GtkWidget *btn_copypath = gtk_button_new_with_label("🔗  Copy Path / URI");
    gtk_widget_add_css_class(btn_copypath, "flat");
    gtk_widget_add_css_class(btn_copypath, "files-menu-row");
    g_signal_connect(btn_copypath, "clicked", G_CALLBACK(on_bc_pop_copypath_clicked), self);
    gtk_box_append(GTK_BOX(body), btn_copypath);

    gtk_box_append(GTK_BOX(vbox), body);
    gtk_popover_set_child(GTK_POPOVER(self->breadcrumb_popover), vbox);

    if (self->breadcrumb_scroll) {
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(self->breadcrumb_scroll);
        if (hadj) {
            g_signal_connect(hadj, "changed", G_CALLBACK(on_breadcrumb_hadj_changed), self);
            g_signal_connect(hadj, "notify::upper", G_CALLBACK(on_breadcrumb_hadj_notify), self);
            g_signal_connect(hadj, "notify::page-size", G_CALLBACK(on_breadcrumb_hadj_notify), self);
        }
        GtkEventController *bc_scroll_ctrl = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
        g_signal_connect(bc_scroll_ctrl, "scroll", G_CALLBACK(on_breadcrumb_wheel_scroll), self);
        gtk_widget_add_controller(GTK_WIDGET(self->breadcrumb_scroll), bc_scroll_ctrl);
    }
}

static void
open_breadcrumb_popover_for_segment(KajoFilesWindow *self,
                                    GtkWidget *btn,
                                    const char *uri,
                                    const char *label)
{
    if (!self || !self->breadcrumb_popover || !uri)
        return;

    g_free(self->bc_pop_target_uri);
    self->bc_pop_target_uri = g_strdup(uri);
    gtk_label_set_text(GTK_LABEL(self->bc_pop_hdr_lbl), label ? label : "FOLDER");

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->bc_pop_list_box)) != NULL) {
        gtk_box_remove(GTK_BOX(self->bc_pop_list_box), child);
    }

    GtkWidget *loading = gtk_label_new("Scanning subfolders...");
    gtk_widget_add_css_class(loading, "shell-popover-subtitle");
    gtk_widget_set_halign(loading, GTK_ALIGN_START);
    gtk_widget_set_margin_start(loading, 8);
    gtk_box_append(GTK_BOX(self->bc_pop_list_box), loading);

    if (self->bc_pop_cancel) {
        g_cancellable_cancel(self->bc_pop_cancel);
        g_clear_object(&self->bc_pop_cancel);
    }
    self->bc_pop_cancel = g_cancellable_new();

    g_autoptr(GFile) dir = g_file_new_for_uri(uri);
    kajo_files_enumerate_async(dir, self->bc_pop_cancel, on_breadcrumb_subdirs_ready, files_window_op_weak_new(self));

    graphene_point_t pt = { 0, 0 };
    if (gtk_widget_compute_point(btn, GTK_WIDGET(self->location_stack), &GRAPHENE_POINT_INIT(0, 0), &pt)) {
        GdkRectangle rect = {
            (int)pt.x,
            (int)pt.y,
            MAX(1, gtk_widget_get_width(btn)),
            MAX(1, gtk_widget_get_height(btn))
        };
        gtk_popover_set_pointing_to(GTK_POPOVER(self->breadcrumb_popover), &rect);
    }
    gtk_popover_popup(GTK_POPOVER(self->breadcrumb_popover));
}

static void
on_breadcrumb_btn_pressed(GtkGestureClick *gesture,
                          int n_press,
                          double x G_GNUC_UNUSED,
                          double y G_GNUC_UNUSED,
                          gpointer user_data)
{
    if (n_press != 1)
        return;

    KajoFilesWindow *self = user_data;
    GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    const char *uri = btn ? g_object_get_data(G_OBJECT(btn), "segment-uri") : NULL;
    const char *lbl = btn ? g_object_get_data(G_OBJECT(btn), "segment-label") : NULL;
    if (!uri)
        return;

    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    if (button == GDK_BUTTON_MIDDLE ||
        (button == GDK_BUTTON_PRIMARY && (mods & GDK_CONTROL_MASK) != 0)) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
        GtkWidget *win = kajo_files_window_new(app, uri);
        gtk_window_present(GTK_WINDOW(win));
    } else if (button == GDK_BUTTON_SECONDARY) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        open_breadcrumb_popover_for_segment(self, btn, uri, lbl);
    }
}

static void
on_breadcrumb_segment_clicked(GtkButton *btn, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *uri = g_object_get_data(G_OBJECT(btn), "segment-uri");
    if (uri)
        kajo_files_window_navigate_uri(self, uri);
}

void
files_window_rebuild_breadcrumbs(KajoFilesWindow *self)
{
    if (!self || !self->breadcrumb_box || !self->current_dir)
        return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->breadcrumb_box)) != NULL) {
        gtk_box_remove(GTK_BOX(self->breadcrumb_box), child);
    }

    g_autofree char *uri = g_file_get_uri(self->current_dir);
    g_autofree char *local_path = g_file_get_path(self->current_dir);
    g_autofree char *arch_path = NULL;
    g_autofree char *inner_path = NULL;

    g_autofree char *badge = NULL;
    if (uri && kajo_files_parse_archive_uri(uri, &arch_path, &inner_path)) {
        g_autofree char *canon_uri = kajo_files_build_archive_uri(arch_path, inner_path);
        gtk_editable_set_text(GTK_EDITABLE(self->uri_entry), canon_uri ? canon_uri : uri);
        g_autofree char *arch_base = g_path_get_basename(arch_path);
        if (inner_path && inner_path[0] != '\0')
            badge = g_strdup_printf("ARCHIVE: %s!/%s", arch_base ? arch_base : arch_path, inner_path);
        else
            badge = g_strdup_printf("ARCHIVE: %s!/", arch_base ? arch_base : arch_path);
    } else {
        gtk_editable_set_text(GTK_EDITABLE(self->uri_entry), local_path ? local_path : (uri ? uri : ""));
        badge = g_strdup_printf("LOCATION: %s", local_path ? local_path : (uri ? uri : ""));
    }
    gtk_label_set_text(GTK_LABEL(self->current_uri_badge), badge);

    /* Build full path chain from root to current_dir so '/' and '/home' remain 1 click away */
    g_autoptr(GPtrArray) chain = kajo_files_build_breadcrumb_chain(self->current_dir);

    for (guint i = 0; i < chain->len; i++) {
        const KajoBreadcrumbSegment *seg = g_ptr_array_index(chain, i);

        if (i > 0) {
            GtkWidget *sep = gtk_label_new("▸");
            gtk_widget_add_css_class(sep, "sidebar-chevron");
            gtk_box_append(GTK_BOX(self->breadcrumb_box), sep);
        }

        GtkWidget *btn = gtk_button_new_with_label(seg->label);
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_add_css_class(btn, "files-breadcrumb-btn");
        if (i + 1 == chain->len)
            gtk_widget_add_css_class(btn, "active");

        gtk_widget_set_tooltip_text(btn, seg->tooltip);

        g_object_set_data_full(G_OBJECT(btn), "segment-uri", g_strdup(seg->uri), g_free);
        g_object_set_data_full(G_OBJECT(btn), "segment-label", g_strdup(seg->label), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_breadcrumb_segment_clicked), self);

        GtkGesture *click_gest = gtk_gesture_click_new();
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click_gest), GTK_PHASE_CAPTURE);
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gest), 0);
        g_signal_connect(click_gest, "pressed", G_CALLBACK(on_breadcrumb_btn_pressed), self);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(click_gest));

        gtk_box_append(GTK_BOX(self->breadcrumb_box), btn);
    }

    self->pending_breadcrumb_scroll_end = TRUE;
    schedule_breadcrumb_scroll_to_end(self);
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

    if (kajo_file_item_is_dir(item) || kajo_file_item_is_mountable(item))
        gtk_widget_add_css_class(w->card, "files-tile-folder");
    else
        gtk_widget_remove_css_class(w->card, "files-tile-folder");

    files_window_set_image_for_item(GTK_IMAGE(w->icon), item);
    gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 42);

    gtk_label_set_text(GTK_LABEL(w->name_lbl), kajo_file_item_get_display_name(item));

    g_autofree char *sz_str = kajo_file_item_format_subtitle(item);
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

    files_window_set_image_for_item(GTK_IMAGE(w->icon), item);
    gtk_label_set_text(GTK_LABEL(w->name_lbl), kajo_file_item_get_display_name(item));

    g_autofree char *sz_str = kajo_file_item_format_subtitle(item);
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

void
files_window_rebuild_filtered_view(KajoFilesWindow *self)
{
    if (!self) return;

    g_autoptr(GPtrArray) prev_uris = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) prev_sel_items = files_window_get_selected_items(self);
    if (prev_sel_items) {
        for (guint i = 0; i < prev_sel_items->len; i++) {
            KajoFileItem *it = g_ptr_array_index(prev_sel_items, i);
            const char *u = it ? kajo_file_item_get_uri(it) : NULL;
            if (u)
                g_ptr_array_add(prev_uris, g_strdup(u));
        }
    }

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
        files_window_update_status_and_capacity_bar(self);
        return;
    }

    gtk_stack_set_visible_child_name(
        self->view_stack,
        (self->view_mode == KAJO_VIEW_GRID) ? "grid" : "list"
    );

    g_autoptr(GtkBitset) matched_bitset = kajo_files_match_selection_uris(self->visible_items, prev_uris);
    KajoRebuildSelectionAction sel_action = kajo_files_rebuild_selection_action(
        matched_bitset && !gtk_bitset_is_empty(matched_bitset),
        self->autoselect_pending,
        query && query[0] != '\0');
    if (sel_action == KAJO_REBUILD_SEL_RESTORE) {
        guint first_idx = gtk_bitset_get_minimum(matched_bitset);
        KajoFileItem *primary = g_ptr_array_index(self->visible_items, first_idx);
        g_set_object(&self->selected_item, primary);

        if (self->selection_model) {
            g_autoptr(GtkBitset) mask = gtk_bitset_new_range(0, self->visible_items->len);
            self->suppress_selection_sync = TRUE;
            gtk_selection_model_set_selection(GTK_SELECTION_MODEL(self->selection_model), matched_bitset, mask);
            self->suppress_selection_sync = FALSE;
        }

        kajo_quicklook_set_item(self->quicklook, primary, self->current_dir);
        files_window_update_status_and_capacity_bar(self);
    } else if (sel_action == KAJO_REBUILD_SEL_FIRST) {
        /* Opening a folder, or filtering with the search box: land on the first entry.
         * Any other refresh (file monitor, sort, hidden toggle, operation reload) must
         * not resurrect a selection the user cleared. */
        self->autoselect_pending = FALSE;
        files_window_set_selected_item_by_index(self, 0);
    } else {
        g_clear_object(&self->selected_item);
        if (self->selection_model) {
            self->suppress_selection_sync = TRUE;
            gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(self->selection_model));
            self->suppress_selection_sync = FALSE;
        }
        kajo_quicklook_set_item(self->quicklook, NULL, self->current_dir);
        files_window_update_status_and_capacity_bar(self);
    }
}

/* ─── Selection Helpers & Model Setup ─── */

int
files_window_get_selected_item_index(KajoFilesWindow *self)
{
    if (!self || !self->visible_items || self->visible_items->len == 0)
        return -1;
    if (self->selected_item) {
        for (guint i = 0; i < self->visible_items->len; i++) {
            KajoFileItem *it = g_ptr_array_index(self->visible_items, i);
            if (it == self->selected_item)
                return (int)i;
        }
    }
    if (self->selection_model) {
        g_autoptr(GtkBitset) bitset = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(self->selection_model));
        if (bitset && !gtk_bitset_is_empty(bitset)) {
            guint pos = gtk_bitset_get_minimum(bitset);
            if (pos != GTK_INVALID_LIST_POSITION && pos < self->visible_items->len)
                return (int)pos;
        }
    }
    return -1;
}

GPtrArray *
files_window_get_selected_items(KajoFilesWindow *self)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);
    if (!self || !self->visible_items)
        return arr;

    if (self->selection_model) {
        g_autoptr(GtkBitset) bitset = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(self->selection_model));
        if (bitset && !gtk_bitset_is_empty(bitset)) {
            GtkBitsetIter iter;
            guint idx = 0;
            if (gtk_bitset_iter_init_first(&iter, bitset, &idx)) {
                do {
                    if (idx < self->visible_items->len) {
                        KajoFileItem *it = g_ptr_array_index(self->visible_items, idx);
                        if (it)
                            g_ptr_array_add(arr, g_object_ref(it));
                    }
                } while (gtk_bitset_iter_next(&iter, &idx));
            }
        }
    }

    if (arr->len == 0 && self->selected_item) {
        g_ptr_array_add(arr, g_object_ref(self->selected_item));
    }
    return arr;
}

void
files_window_select_all(KajoFilesWindow *self)
{
    if (!self || !self->selection_model || !self->visible_items || self->visible_items->len == 0)
        return;

    gtk_selection_model_select_all(GTK_SELECTION_MODEL(self->selection_model));
}

void
files_window_set_selected_item_by_index(KajoFilesWindow *self, int idx)
{
    if (!self || !self->visible_items || self->visible_items->len == 0)
        return;

    idx = CLAMP(idx, 0, (int)self->visible_items->len - 1);
    KajoFileItem *item = g_ptr_array_index(self->visible_items, (guint)idx);
    g_set_object(&self->selected_item, item);

    if (self->selection_model) {
        self->suppress_selection_sync = TRUE;
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(self->selection_model), (guint)idx, TRUE);
        self->suppress_selection_sync = FALSE;
    }

    kajo_quicklook_set_item(self->quicklook, item, self->current_dir);
    files_window_update_status_and_capacity_bar(self);
}

static void
on_selection_model_changed(GtkSelectionModel *sel,
                           guint position G_GNUC_UNUSED,
                           guint n_items G_GNUC_UNUSED,
                           gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self || self->suppress_selection_sync || !self->visible_items)
        return;

    g_autoptr(GtkBitset) bitset = gtk_selection_model_get_selection(sel);
    KajoFileItem *item = NULL;

    if (bitset && !gtk_bitset_is_empty(bitset)) {
        guint min_pos = gtk_bitset_get_minimum(bitset);
        if (min_pos != GTK_INVALID_LIST_POSITION && min_pos < self->visible_items->len)
            item = g_ptr_array_index(self->visible_items, min_pos);
    }

    if (item != self->selected_item) {
        g_set_object(&self->selected_item, item);
        kajo_quicklook_set_item(self->quicklook, item, self->current_dir);
    }
    files_window_update_status_and_capacity_bar(self);
}

void
files_window_views_setup_models(KajoFilesWindow *self)
{
    self->visible_store = g_list_store_new(KAJO_TYPE_FILE_ITEM);
    self->selection_model = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(self->visible_store)));
    g_signal_connect(self->selection_model, "selection-changed", G_CALLBACK(on_selection_model_changed), self);

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
}

/* ─── Header & Ribbon Button Handlers ─── */

void
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

void
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

void
on_btn_up_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self->current_dir) return;
    g_autofree char *parent_uri = kajo_files_get_parent_uri(self->current_dir);
    if (parent_uri) {
        kajo_files_window_navigate_uri(self, parent_uri);
    }
}

static void
on_btn_reload_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    files_window_reload_directory(self);
}

void
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
    files_window_rebuild_filtered_view(self);
}

void
files_window_set_view_mode(KajoFilesWindow *self, KajoFileViewMode mode)
{
    gboolean changed = (self->view_mode != mode);
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
    if (changed)
        files_window_save_prefs(self);
}

static void
on_btn_view_grid_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    files_window_set_view_mode(KAJO_FILES_WINDOW(user_data), KAJO_VIEW_GRID);
}

static void
on_btn_view_list_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    files_window_set_view_mode(KAJO_FILES_WINDOW(user_data), KAJO_VIEW_LIST);
}

void
on_btn_inspector_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->inspector_visible = !self->inspector_visible;
    gtk_widget_set_visible(self->inspector_vbox, self->inspector_visible && !self->is_narrow);
    if (self->inspector_visible)
        gtk_widget_add_css_class(self->btn_inspector, "active");
    else
        gtk_widget_remove_css_class(self->btn_inspector, "active");
    files_window_save_prefs(self);
}

static void
on_btn_float_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    niri_toggle_window_floating();
}

static void
on_btn_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    gtk_window_close(GTK_WINDOW(user_data));
}

static void
on_btn_new_folder_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    files_window_show_inline_prompt(KAJO_FILES_WINDOW(user_data), PROMPT_MKDIR);
}

static void
on_btn_new_file_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    files_window_show_inline_prompt(KAJO_FILES_WINDOW(user_data), PROMPT_NEW_FILE);
}

void
on_btn_paste_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (files_window_is_read_only_archive(self)) {
        files_window_set_status_message(self, "Archive view is read-only", TRUE);
        return;
    }
    if (self->current_dir) {
        files_window_begin_op(self, "Pasting clipboard items...");
        kajo_files_clipboard_paste_async(self->current_dir, self->ops_cancel, files_window_on_file_op_completed, files_window_op_weak_new(self));
    }
}

void
on_btn_hidden_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->show_hidden = !self->show_hidden;
    if (self->show_hidden)
        gtk_widget_add_css_class(self->btn_hidden, "active");
    else
        gtk_widget_remove_css_class(self->btn_hidden, "active");
    files_window_save_prefs(self);
    files_window_rebuild_filtered_view(self);
}

/* ─── Sorting System (Sort Popover & Clickable Column Headers) ─── */

void
files_window_set_widget_active_class(GtkWidget *w, gboolean active)
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

    files_window_set_widget_active_class(self->btn_sort_field_name, self->sort_field == KAJO_SORT_NAME);
    files_window_set_widget_active_class(self->btn_sort_field_modified, self->sort_field == KAJO_SORT_MODIFIED);
    files_window_set_widget_active_class(self->btn_sort_field_size, self->sort_field == KAJO_SORT_SIZE);
    files_window_set_widget_active_class(self->btn_sort_field_type, self->sort_field == KAJO_SORT_TYPE);

    /* Sort Popover Direction & Folders First */
    files_window_set_widget_active_class(self->btn_sort_asc, self->sort_ascending);
    files_window_set_widget_active_class(self->btn_sort_desc, !self->sort_ascending);

    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_folders_first),
        self->sort_folders_first ? "☑  Folders First" : "☐  Folders First"
    );
    files_window_set_widget_active_class(self->btn_sort_folders_first, self->sort_folders_first);

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

    files_window_set_widget_active_class(self->col_btn_name, self->sort_field == KAJO_SORT_NAME);
    files_window_set_widget_active_class(self->col_btn_size, self->sort_field == KAJO_SORT_SIZE);
    files_window_set_widget_active_class(self->col_btn_type, self->sort_field == KAJO_SORT_TYPE);
    files_window_set_widget_active_class(self->col_btn_modified, self->sort_field == KAJO_SORT_MODIFIED);
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
    files_window_save_prefs(self);
    files_window_rebuild_filtered_view(self);
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
    files_window_save_prefs(self);
    files_window_rebuild_filtered_view(self);
}

static void
on_sort_desc_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->sort_ascending = FALSE;
    sync_sort_ui(self);
    files_window_save_prefs(self);
    files_window_rebuild_filtered_view(self);
}

static void
on_sort_folders_first_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->sort_folders_first = !self->sort_folders_first;
    sync_sort_ui(self);
    files_window_save_prefs(self);
    files_window_rebuild_filtered_view(self);
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

void
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

void
files_window_views_connect_toolbar_signals(KajoFilesWindow *self)
{
    sync_sort_ui(self);

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
    g_signal_connect(self->btn_new_file, "clicked", G_CALLBACK(on_btn_new_file_clicked), self);
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
}
