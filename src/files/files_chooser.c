#include "files_chooser.h"
#include "files_model.h"
#include "files_network.h"
#include "files_ops.h"
#include "files_sidebar.h"
#include "files_quicklook.h"
#include <gdk/wayland/gdkwayland.h>
#include <string.h>

typedef struct {
    char      *id;
    gboolean   is_checkbox;
    GtkWidget *widget;
    GPtrArray *option_ids; /* char* for dropdowns */
} ChooserChoiceBinding;

static void
chooser_choice_binding_free(gpointer data)
{
    ChooserChoiceBinding *b = data;
    if (!b) return;
    g_free(b->id);
    g_clear_pointer(&b->option_ids, g_ptr_array_unref);
    g_free(b);
}

struct _KajoFilesChooser {
    GtkWindow parent_instance;

    KajoPortalActiveRequest  *active_req;     /* non-owning; cleared once finished */
    KajoPortalChooserRequest *owned_preview;  /* non-NULL only in preview mode */
    const KajoPortalChooserRequest *spec;
    gboolean responded;

    /* Layout widgets */
    GtkWidget *root_overlay;
    GtkWidget *btn_back;
    GtkWidget *btn_forward;
    GtkWidget *btn_up;
    GtkWidget *btn_reload;
    GtkWidget *location_stack;
    GtkWidget *breadcrumb_box;
    GtkWidget *uri_entry;
    GtkWidget *search_entry;
    GtkWidget *btn_view_grid;
    GtkWidget *btn_view_list;
    GtkWidget *btn_hidden;
    GtkWidget *btn_mkdir;

    GtkWidget *sidebar_list;
    GtkWidget *auth_revealer;
    GtkWidget *connect_revealer;
    GtkWidget *connect_entry;
    GtkWidget *mkdir_revealer;
    GtkWidget *mkdir_entry;
    GtkWidget *overwrite_revealer;
    GtkWidget *overwrite_lbl;

    GtkWidget *view_stack;
    GtkWidget *grid_view;
    GtkWidget *list_view;
    GtkWidget *empty_lbl;
    GtkWidget *status_lbl;

    GtkWidget *filename_entry;
    GtkWidget *filter_dropdown;
    GtkWidget *btn_accept;
    GtkWidget *btn_cancel;

    KajoFilesSidebar *sidebar;
    KajoQuickLook    *quicklook;

    GFile             *current_dir;
    GCancellable      *enum_cancel;
    GCancellable      *ops_cancel;
    char              *last_mount_attempt_uri;
    GPtrArray         *all_items;
    GPtrArray         *visible_items;
    GListStore        *visible_store;
    GtkSelectionModel *selection_model;
    KajoFileItem      *selected_item;
    GPtrArray         *history;
    int                history_pos;
    gboolean           navigating_history;
    gboolean           suppress_selection_sync;
    KajoFileViewMode   view_mode;
    gboolean           show_hidden;
    gint               active_filter_idx; /* -1 = All Files */
    GPtrArray         *choice_bindings;   /* ChooserChoiceBinding* */
    char              *pending_overwrite_path;
};

G_DEFINE_TYPE(KajoFilesChooser, kajo_files_chooser, GTK_TYPE_WINDOW)

static void chooser_navigate_uri(KajoFilesChooser *self, const char *uri_or_path);
static void chooser_reload_dir(KajoFilesChooser *self);
static void chooser_rebuild_filtered_view(KajoFilesChooser *self);
static void chooser_try_accept(KajoFilesChooser *self, gboolean confirmed_overwrite);

static void
chooser_set_status(KajoFilesChooser *self, const char *msg, gboolean is_err)
{
    if (!self || !self->status_lbl) return;
    gtk_label_set_text(GTK_LABEL(self->status_lbl), msg ? msg : "");
    if (is_err)
        gtk_widget_add_css_class(self->status_lbl, "files-status-error");
    else
        gtk_widget_remove_css_class(self->status_lbl, "files-status-error");
}

static void
chooser_complete(KajoFilesChooser *self, guint32 response, KajoPortalChooserResult *result)
{
    if (!self || self->responded) {
        kajo_portal_chooser_result_free(result);
        return;
    }
    self->responded = TRUE;

    KajoPortalActiveRequest *req = self->active_req;
    self->active_req = NULL;
    if (req) {
        kajo_portal_active_request_set_close_cb(req, NULL, NULL);
        kajo_portal_active_request_finish(req, response, result);
    } else {
        kajo_portal_chooser_result_free(result);
    }
    gtk_window_close(GTK_WINDOW(self));
}

static GPtrArray *
chooser_get_selected_items(KajoFilesChooser *self)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);
    if (!self || !self->visible_items || !self->selection_model)
        return arr;

    g_autoptr(GtkBitset) bs = gtk_selection_model_get_selection(self->selection_model);
    if (bs && !gtk_bitset_is_empty(bs)) {
        GtkBitsetIter iter;
        guint idx = 0;
        if (gtk_bitset_iter_init_first(&iter, bs, &idx)) {
            do {
                if (idx < self->visible_items->len) {
                    KajoFileItem *it = g_ptr_array_index(self->visible_items, idx);
                    if (it)
                        g_ptr_array_add(arr, g_object_ref(it));
                }
            } while (gtk_bitset_iter_next(&iter, &idx));
        }
    }
    return arr;
}

static void
chooser_collect_choices(KajoFilesChooser *self, KajoPortalChooserResult *res)
{
    if (!self || !res || !self->choice_bindings)
        return;

    for (guint i = 0; i < self->choice_bindings->len; i++) {
        const ChooserChoiceBinding *b = g_ptr_array_index(self->choice_bindings, i);
        if (!b || !b->id || !b->widget)
            continue;
        g_ptr_array_add(res->choice_ids, g_strdup(b->id));
        if (b->is_checkbox) {
            gboolean on = gtk_check_button_get_active(GTK_CHECK_BUTTON(b->widget));
            g_ptr_array_add(res->choice_values, g_strdup(on ? "true" : "false"));
        } else {
            guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(b->widget));
            const char *val = (b->option_ids && sel < b->option_ids->len)
                ? g_ptr_array_index(b->option_ids, sel) : "";
            g_ptr_array_add(res->choice_values, g_strdup(val ? val : ""));
        }
    }
}

static void
chooser_try_accept(KajoFilesChooser *self, gboolean confirmed_overwrite)
{
    if (!self || !self->spec || self->responded)
        return;

    gtk_revealer_set_reveal_child(GTK_REVEALER(self->overwrite_revealer), FALSE);
    g_clear_pointer(&self->pending_overwrite_path, g_free);

    const KajoPortalChooserRequest *spec = self->spec;

    if (spec->mode == KAJO_PORTAL_MODE_SAVE_FILE) {
        if (!self->current_dir)
            return;
        g_autofree char *raw_name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->filename_entry)));
        if (raw_name)
            g_strstrip(raw_name);
        if (!raw_name || raw_name[0] == '\0' || strchr(raw_name, '/') != NULL) {
            chooser_set_status(self, "Enter a valid filename without '/'", TRUE);
            gtk_widget_grab_focus(self->filename_entry);
            return;
        }
        g_autofree char *dir_path = g_file_get_path(self->current_dir);
        if (!dir_path) {
            chooser_set_status(self, "Choose a local or mounted folder to save into", TRUE);
            return;
        }
        g_autofree char *full_path = g_build_filename(dir_path, raw_name, NULL);
        if (!confirmed_overwrite && g_file_test(full_path, G_FILE_TEST_EXISTS)) {
            self->pending_overwrite_path = g_strdup(full_path);
            g_autofree char *warn = g_strdup_printf("⚠ '%s' ALREADY EXISTS — OVERWRITE?", raw_name);
            gtk_label_set_text(GTK_LABEL(self->overwrite_lbl), warn);
            gtk_revealer_set_reveal_child(GTK_REVEALER(self->overwrite_revealer), TRUE);
            return;
        }
        g_autofree char *uri = g_filename_to_uri(full_path, NULL, NULL);
        if (!uri) {
            chooser_set_status(self, "Could not encode destination path", TRUE);
            return;
        }
        KajoPortalChooserResult *res = kajo_portal_chooser_result_new();
        g_ptr_array_add(res->uris, g_steal_pointer(&uri));
        res->selected_filter_index = self->active_filter_idx;
        chooser_collect_choices(self, res);
        chooser_complete(self, 0, res);
        return;
    }

    if (spec->mode == KAJO_PORTAL_MODE_SAVE_FILES) {
        g_autoptr(GFile) target_dir = self->current_dir ? g_object_ref(self->current_dir) : NULL;
        g_autoptr(GPtrArray) sel = chooser_get_selected_items(self);
        if (sel->len == 1) {
            KajoFileItem *it = g_ptr_array_index(sel, 0);
            if (kajo_file_item_is_dir(it))
                g_set_object(&target_dir, kajo_file_item_get_file(it));
        }
        if (!target_dir)
            return;
        g_autoptr(GPtrArray) uris = kajo_portal_build_save_files_uris(target_dir, spec->save_files);
        if (uris->len == 0) {
            chooser_set_status(self, "Choose a local or mounted destination folder", TRUE);
            return;
        }
        KajoPortalChooserResult *res = kajo_portal_chooser_result_new();
        for (guint i = 0; i < uris->len; i++)
            g_ptr_array_add(res->uris, g_strdup(g_ptr_array_index(uris, i)));
        chooser_collect_choices(self, res);
        chooser_complete(self, 0, res);
        return;
    }

    /* OpenFile (files or directories) */
    g_autoptr(GPtrArray) sel = chooser_get_selected_items(self);
    if (spec->directory) {
        KajoPortalChooserResult *res = kajo_portal_chooser_result_new();
        for (guint i = 0; i < sel->len; i++) {
            KajoFileItem *it = g_ptr_array_index(sel, i);
            if (!kajo_file_item_is_dir(it))
                continue;
            char *u = kajo_portal_normalize_file_uri(kajo_file_item_get_file(it));
            if (u)
                g_ptr_array_add(res->uris, u);
        }
        if (res->uris->len == 0 && self->current_dir) {
            char *u = kajo_portal_normalize_file_uri(self->current_dir);
            if (u)
                g_ptr_array_add(res->uris, u);
        }
        if (res->uris->len == 0) {
            kajo_portal_chooser_result_free(res);
            chooser_set_status(self, "Select a local or mounted folder", TRUE);
            return;
        }
        chooser_collect_choices(self, res);
        chooser_complete(self, 0, res);
        return;
    }

    /* OpenFile: if single directory/mountable selected, navigate into it instead of accepting */
    if (sel->len == 1) {
        KajoFileItem *it = g_ptr_array_index(sel, 0);
        if (kajo_file_item_is_dir(it) || kajo_file_item_is_mountable(it)) {
            const char *dest = kajo_file_item_get_target_uri(it);
            if (!dest) dest = kajo_file_item_get_uri(it);
            chooser_navigate_uri(self, dest);
            return;
        }
    }

    KajoPortalChooserResult *res = kajo_portal_chooser_result_new();
    for (guint i = 0; i < sel->len; i++) {
        KajoFileItem *it = g_ptr_array_index(sel, i);
        if (kajo_file_item_is_dir(it) || kajo_file_item_is_mountable(it))
            continue;
        char *u = kajo_portal_normalize_file_uri(kajo_file_item_get_file(it));
        if (u)
            g_ptr_array_add(res->uris, u);
    }
    if (res->uris->len == 0) {
        kajo_portal_chooser_result_free(res);
        chooser_set_status(self, "Select at least one local or mounted file", TRUE);
        return;
    }
    res->selected_filter_index = self->active_filter_idx;
    chooser_collect_choices(self, res);
    chooser_complete(self, 0, res);
}

/* ─── Recycling Grid & List Item Factories ─── */

typedef struct {
    GtkWidget *card;
    GtkWidget *icon;
    GtkWidget *name_lbl;
    GtkWidget *sub_lbl;
} ChooserGridWidgets;

typedef struct {
    GtkWidget *hbox;
    GtkWidget *icon;
    GtkWidget *name_lbl;
    GtkWidget *sz_lbl;
    GtkWidget *type_lbl;
} ChooserListWidgets;

static void
set_icon_for_item(GtkImage *img, const KajoFileItem *item)
{
    GIcon *gi = kajo_file_item_get_gicon(item);
    if (gi)
        gtk_image_set_from_gicon(img, gi);
    else
        gtk_image_set_from_icon_name(img, kajo_file_item_get_icon_name(item));
}

static void
on_chooser_grid_setup(GtkSignalListItemFactory *f G_GNUC_UNUSED,
                      GtkListItem              *li,
                      gpointer                  ud G_GNUC_UNUSED)
{
    ChooserGridWidgets *w = g_new0(ChooserGridWidgets, 1);
    w->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(w->card, "files-tile-card");
    gtk_widget_set_size_request(w->card, 128, 102);

    w->icon = gtk_image_new_from_icon_name("text-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 36);
    gtk_widget_set_halign(w->icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(w->icon, 6);

    w->name_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->name_lbl, "files-tile-title");
    gtk_label_set_ellipsize(GTK_LABEL(w->name_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(w->name_lbl), 15);
    gtk_widget_set_halign(w->name_lbl, GTK_ALIGN_CENTER);

    w->sub_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->sub_lbl, "files-tile-sub");
    gtk_widget_set_halign(w->sub_lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(w->card), w->icon);
    gtk_box_append(GTK_BOX(w->card), w->name_lbl);
    gtk_box_append(GTK_BOX(w->card), w->sub_lbl);

    g_object_set_data_full(G_OBJECT(li), "cw", w, g_free);
    gtk_list_item_set_child(li, w->card);
}

static void
on_chooser_grid_bind(GtkSignalListItemFactory *f G_GNUC_UNUSED,
                     GtkListItem              *li,
                     gpointer                  ud G_GNUC_UNUSED)
{
    ChooserGridWidgets *w = g_object_get_data(G_OBJECT(li), "cw");
    KajoFileItem *it = gtk_list_item_get_item(li);
    if (!w || !it) return;

    if (kajo_file_item_is_dir(it) || kajo_file_item_is_mountable(it))
        gtk_widget_add_css_class(w->card, "files-tile-folder");
    else
        gtk_widget_remove_css_class(w->card, "files-tile-folder");

    set_icon_for_item(GTK_IMAGE(w->icon), it);
    gtk_label_set_text(GTK_LABEL(w->name_lbl), kajo_file_item_get_display_name(it));
    g_autofree char *sub = kajo_file_item_format_subtitle(it);
    gtk_label_set_text(GTK_LABEL(w->sub_lbl), sub);
}

static void
on_chooser_list_setup(GtkSignalListItemFactory *f G_GNUC_UNUSED,
                      GtkListItem              *li,
                      gpointer                  ud G_GNUC_UNUSED)
{
    ChooserListWidgets *w = g_new0(ChooserListWidgets, 1);
    w->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(w->hbox, "files-list-row");
    gtk_widget_set_valign(w->hbox, GTK_ALIGN_CENTER);

    w->icon = gtk_image_new_from_icon_name("text-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 18);
    gtk_widget_set_valign(w->icon, GTK_ALIGN_CENTER);

    w->name_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->name_lbl, "files-list-name");
    gtk_widget_set_hexpand(w->name_lbl, TRUE);
    gtk_widget_set_halign(w->name_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(w->name_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(w->name_lbl), PANGO_ELLIPSIZE_END);

    w->sz_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->sz_lbl, "files-list-meta");
    gtk_widget_set_size_request(w->sz_lbl, 92, -1);
    gtk_widget_set_halign(w->sz_lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(w->sz_lbl, GTK_ALIGN_CENTER);

    w->type_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->type_lbl, "files-list-meta");
    gtk_widget_set_size_request(w->type_lbl, 150, -1);
    gtk_widget_set_halign(w->type_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(w->type_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(w->type_lbl), PANGO_ELLIPSIZE_END);

    gtk_box_append(GTK_BOX(w->hbox), w->icon);
    gtk_box_append(GTK_BOX(w->hbox), w->name_lbl);
    gtk_box_append(GTK_BOX(w->hbox), w->sz_lbl);
    gtk_box_append(GTK_BOX(w->hbox), w->type_lbl);

    g_object_set_data_full(G_OBJECT(li), "lw", w, g_free);
    gtk_list_item_set_child(li, w->hbox);
}

static void
on_chooser_list_bind(GtkSignalListItemFactory *f G_GNUC_UNUSED,
                     GtkListItem              *li,
                     gpointer                  ud G_GNUC_UNUSED)
{
    ChooserListWidgets *w = g_object_get_data(G_OBJECT(li), "lw");
    KajoFileItem *it = gtk_list_item_get_item(li);
    if (!w || !it) return;

    set_icon_for_item(GTK_IMAGE(w->icon), it);
    gtk_label_set_text(GTK_LABEL(w->name_lbl), kajo_file_item_get_display_name(it));
    g_autofree char *sub = kajo_file_item_format_subtitle(it);
    gtk_label_set_text(GTK_LABEL(w->sz_lbl), sub);
    gtk_label_set_text(GTK_LABEL(w->type_lbl), kajo_file_item_get_type_description(it));
}

/* ─── Filtering, Navigation & Enumeration ─── */

static gint
chooser_sort_cb(gconstpointer a, gconstpointer b)
{
    const KajoFileItem *ia = *(const KajoFileItem *const *)a;
    const KajoFileItem *ib = *(const KajoFileItem *const *)b;
    return kajo_file_item_compare_full(ia, ib, KAJO_SORT_NAME, TRUE, TRUE);
}

static void
chooser_rebuild_filtered_view(KajoFilesChooser *self)
{
    if (!self) return;

    if (!self->visible_items)
        self->visible_items = g_ptr_array_new_with_free_func(g_object_unref);
    else
        g_ptr_array_set_size(self->visible_items, 0);

    const char *query = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    const KajoPortalFilter *active_f = NULL;
    if (self->spec && self->spec->filters && self->active_filter_idx >= 0 &&
        (guint)self->active_filter_idx < self->spec->filters->len) {
        active_f = g_ptr_array_index(self->spec->filters, (guint)self->active_filter_idx);
    }

    gboolean dirs_only = self->spec && (self->spec->directory || self->spec->mode == KAJO_PORTAL_MODE_SAVE_FILES);

    for (guint i = 0; self->all_items && i < self->all_items->len; i++) {
        KajoFileItem *it = g_ptr_array_index(self->all_items, i);
        if (!kajo_file_item_matches_filter(it, self->show_hidden, query))
            continue;
        if (dirs_only && !kajo_file_item_is_dir(it) && !kajo_file_item_is_mountable(it))
            continue;
        if (active_f && !kajo_portal_filter_matches_item(active_f, it))
            continue;
        g_ptr_array_add(self->visible_items, g_object_ref(it));
    }

    g_ptr_array_sort(self->visible_items, chooser_sort_cb);

    self->suppress_selection_sync = TRUE;
    guint old_n = g_list_model_get_n_items(G_LIST_MODEL(self->visible_store));
    g_list_store_splice(self->visible_store, 0, old_n,
                        self->visible_items->pdata, self->visible_items->len);
    self->suppress_selection_sync = FALSE;

    g_clear_object(&self->selected_item);
    kajo_quicklook_set_item(self->quicklook, NULL, self->current_dir);

    if (self->visible_items->len == 0) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->view_stack), "empty");
    } else {
        gtk_stack_set_visible_child_name(
            GTK_STACK(self->view_stack),
            (self->view_mode == KAJO_VIEW_GRID) ? "grid" : "list");
    }

    g_autofree char *st = g_strdup_printf("%u ITEM(S)", self->visible_items->len);
    chooser_set_status(self, st, FALSE);
}

static void
on_chooser_breadcrumb_clicked(GtkButton *btn, gpointer user_data)
{
    KajoFilesChooser *self = user_data;
    const char *uri = g_object_get_data(G_OBJECT(btn), "seg-uri");
    if (uri)
        chooser_navigate_uri(self, uri);
}

static void
chooser_rebuild_breadcrumbs(KajoFilesChooser *self)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->breadcrumb_box)) != NULL)
        gtk_box_remove(GTK_BOX(self->breadcrumb_box), child);

    if (!self->current_dir)
        return;

    g_autofree char *path = g_file_get_path(self->current_dir);
    g_autofree char *uri = g_file_get_uri(self->current_dir);
    gtk_editable_set_text(GTK_EDITABLE(self->uri_entry), path ? path : (uri ? uri : ""));

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
        g_object_set_data_full(G_OBJECT(btn), "seg-uri", g_strdup(seg->uri), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_chooser_breadcrumb_clicked), self);
        gtk_box_append(GTK_BOX(self->breadcrumb_box), btn);
    }
}

static void
on_chooser_dir_enumerated(GPtrArray *items, GError *error, gpointer user_data)
{
    GWeakRef *wr = user_data;
    g_autoptr(KajoFilesChooser) self = g_weak_ref_get(wr);
    g_weak_ref_clear(wr);
    g_free(wr);
    if (!self) {
        if (items) g_ptr_array_unref(items);
        return;
    }

    if (error) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            return;
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED) && self->current_dir) {
            g_autofree char *cur = g_file_get_uri(self->current_dir);
            if (cur && g_strcmp0(self->last_mount_attempt_uri, cur) != 0) {
                g_free(self->last_mount_attempt_uri);
                self->last_mount_attempt_uri = g_strdup(cur);
                chooser_set_status(self, "Mounting remote location...", FALSE);
                kajo_files_sidebar_mount_uri(self->sidebar, self->current_dir);
                return;
            }
        }
        chooser_set_status(self, error->message, TRUE);
        gtk_label_set_text(GTK_LABEL(self->empty_lbl), error->message);
        gtk_stack_set_visible_child_name(GTK_STACK(self->view_stack), "empty");
        return;
    }

    g_clear_pointer(&self->last_mount_attempt_uri, g_free);
    gtk_label_set_text(GTK_LABEL(self->empty_lbl),
                       g_file_has_uri_scheme(self->current_dir, "network")
                           ? "NO NETWORK SERVERS FOUND"
                           : "NO MATCHING ITEMS");

    g_clear_pointer(&self->all_items, g_ptr_array_unref);
    self->all_items = items ? items : g_ptr_array_new_with_free_func(g_object_unref);
    chooser_rebuild_filtered_view(self);
}

static void
chooser_reload_dir(KajoFilesChooser *self)
{
    if (!self || !self->current_dir)
        return;

    if (self->enum_cancel) {
        g_cancellable_cancel(self->enum_cancel);
        g_clear_object(&self->enum_cancel);
    }
    self->enum_cancel = g_cancellable_new();

    if (g_file_has_uri_scheme(self->current_dir, "network"))
        chooser_set_status(self, "Scanning local network...", FALSE);

    GWeakRef *wr = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr, self);
    kajo_files_enumerate_async(self->current_dir, self->enum_cancel, on_chooser_dir_enumerated, wr);
}

static void
chooser_navigate_uri(KajoFilesChooser *self, const char *uri_or_path)
{
    g_autoptr(GFile) target = NULL;
    if (!uri_or_path || uri_or_path[0] == '\0')
        target = g_file_new_for_path(g_get_home_dir());
    else if (strstr(uri_or_path, "://") != NULL)
        target = g_file_new_for_uri(uri_or_path);
    else if (uri_or_path[0] == '~') {
        g_autofree char *exp = g_build_filename(g_get_home_dir(), uri_or_path + 1, NULL);
        target = g_file_new_for_path(exp);
    } else {
        target = g_file_new_for_path(uri_or_path);
    }

    g_clear_pointer(&self->last_mount_attempt_uri, g_free);
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->overwrite_revealer), FALSE);
    g_set_object(&self->current_dir, target);

    g_autofree char *canon = g_file_get_uri(target);
    if (!self->navigating_history && canon) {
        while ((int)self->history->len > self->history_pos + 1)
            g_ptr_array_remove_index(self->history, self->history->len - 1);
        g_ptr_array_add(self->history, g_strdup(canon));
        self->history_pos = (int)self->history->len - 1;
    }

    gtk_widget_set_sensitive(self->btn_back, self->history_pos > 0);
    gtk_widget_set_sensitive(self->btn_forward, self->history_pos + 1 < (int)self->history->len);
    g_autofree char *parent = kajo_files_get_parent_uri(target);
    gtk_widget_set_sensitive(self->btn_up, parent != NULL);
    gtk_stack_set_visible_child_name(GTK_STACK(self->location_stack), "breadcrumbs");

    chooser_rebuild_breadcrumbs(self);
    if (self->sidebar && canon)
        kajo_files_sidebar_sync_active_uri(self->sidebar, canon);
    chooser_reload_dir(self);
}

/* ─── Signal Handlers ─── */

static void
on_chooser_selection_changed(GtkSelectionModel *sel,
                             guint              pos G_GNUC_UNUSED,
                             guint              n G_GNUC_UNUSED,
                             gpointer           user_data)
{
    KajoFilesChooser *self = user_data;
    if (!self || self->suppress_selection_sync || !self->visible_items)
        return;

    g_autoptr(GtkBitset) bs = gtk_selection_model_get_selection(sel);
    KajoFileItem *primary = NULL;
    if (bs && !gtk_bitset_is_empty(bs)) {
        guint min_pos = gtk_bitset_get_minimum(bs);
        if (min_pos < self->visible_items->len)
            primary = g_ptr_array_index(self->visible_items, min_pos);
    }
    g_set_object(&self->selected_item, primary);
    kajo_quicklook_set_item(self->quicklook, primary, self->current_dir);

    if (primary && self->spec && self->spec->mode == KAJO_PORTAL_MODE_SAVE_FILE &&
        !kajo_file_item_is_dir(primary) && !kajo_file_item_is_mountable(primary) &&
        self->filename_entry) {
        gtk_editable_set_text(GTK_EDITABLE(self->filename_entry),
                              kajo_file_item_get_name(primary));
    }
}

static void
on_chooser_item_activated(GtkWidget *view G_GNUC_UNUSED, guint pos, gpointer user_data)
{
    KajoFilesChooser *self = user_data;
    if (!self || !self->visible_items || pos >= self->visible_items->len)
        return;

    KajoFileItem *it = g_ptr_array_index(self->visible_items, pos);
    if (kajo_file_item_is_dir(it) || kajo_file_item_is_mountable(it)) {
        const char *dest = kajo_file_item_get_target_uri(it);
        if (!dest) dest = kajo_file_item_get_uri(it);
        chooser_navigate_uri(self, dest);
        return;
    }

    if (self->spec && self->spec->mode == KAJO_PORTAL_MODE_SAVE_FILE && self->filename_entry)
        gtk_editable_set_text(GTK_EDITABLE(self->filename_entry), kajo_file_item_get_name(it));
    chooser_try_accept(self, FALSE);
}

static void on_chooser_back(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    if (self->history_pos > 0) {
        self->history_pos--;
        self->navigating_history = TRUE;
        chooser_navigate_uri(self, g_ptr_array_index(self->history, (guint)self->history_pos));
        self->navigating_history = FALSE;
    }
}

static void on_chooser_fwd(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    if (self->history_pos + 1 < (int)self->history->len) {
        self->history_pos++;
        self->navigating_history = TRUE;
        chooser_navigate_uri(self, g_ptr_array_index(self->history, (guint)self->history_pos));
        self->navigating_history = FALSE;
    }
}

static void on_chooser_up(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    g_autofree char *p = kajo_files_get_parent_uri(self->current_dir);
    if (p) chooser_navigate_uri(self, p);
}

static void on_chooser_reload(GtkButton *b G_GNUC_UNUSED, gpointer ud) { chooser_reload_dir(ud); }
static void on_chooser_search(GtkSearchEntry *e G_GNUC_UNUSED, gpointer ud) { chooser_rebuild_filtered_view(ud); }
static void on_chooser_cancel(GtkButton *b G_GNUC_UNUSED, gpointer ud) { chooser_complete(ud, 1, NULL); }
static void on_chooser_accept(GtkButton *b G_GNUC_UNUSED, gpointer ud) { chooser_try_accept(ud, FALSE); }
static void on_chooser_overwrite_ok(GtkButton *b G_GNUC_UNUSED, gpointer ud) { chooser_try_accept(ud, TRUE); }
static void on_chooser_overwrite_no(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->overwrite_revealer), FALSE);
    g_clear_pointer(&self->pending_overwrite_path, g_free);
}

static void on_chooser_uri_edit(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    const char *vis = gtk_stack_get_visible_child_name(GTK_STACK(self->location_stack));
    if (g_strcmp0(vis, "entry") == 0) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->location_stack), "breadcrumbs");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(self->location_stack), "entry");
        gtk_widget_grab_focus(self->uri_entry);
        gtk_editable_select_region(GTK_EDITABLE(self->uri_entry), 0, -1);
    }
}

static void on_chooser_uri_activate(GtkEntry *e, gpointer ud)
{
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(e));
    if (txt && txt[0]) chooser_navigate_uri(ud, txt);
}

static void on_chooser_view_grid(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    self->view_mode = KAJO_VIEW_GRID;
    gtk_widget_add_css_class(self->btn_view_grid, "active");
    gtk_widget_remove_css_class(self->btn_view_list, "active");
    if (self->visible_items && self->visible_items->len > 0)
        gtk_stack_set_visible_child_name(GTK_STACK(self->view_stack), "grid");
}

static void on_chooser_view_list(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    self->view_mode = KAJO_VIEW_LIST;
    gtk_widget_add_css_class(self->btn_view_list, "active");
    gtk_widget_remove_css_class(self->btn_view_grid, "active");
    if (self->visible_items && self->visible_items->len > 0)
        gtk_stack_set_visible_child_name(GTK_STACK(self->view_stack), "list");
}

static void on_chooser_hidden(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    self->show_hidden = !self->show_hidden;
    if (self->show_hidden)
        gtk_widget_add_css_class(self->btn_hidden, "active");
    else
        gtk_widget_remove_css_class(self->btn_hidden, "active");
    chooser_rebuild_filtered_view(self);
}

static void on_chooser_filter_changed(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (self->spec && self->spec->filters && sel < self->spec->filters->len)
        self->active_filter_idx = (gint)sel;
    else
        self->active_filter_idx = -1;
    chooser_rebuild_filtered_view(self);
}

static void
on_chooser_mkdir_done(gboolean ok, const char *msg, GError *err, gpointer ud)
{
    GWeakRef *wr = ud;
    g_autoptr(KajoFilesChooser) self = g_weak_ref_get(wr);
    g_weak_ref_clear(wr);
    g_free(wr);
    if (!self) return;
    if (!ok)
        chooser_set_status(self, err ? err->message : "Could not create folder", TRUE);
    else if (msg)
        chooser_set_status(self, msg, FALSE);
    chooser_reload_dir(self);
}

static void on_chooser_mkdir_toggle(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    gboolean show = !gtk_revealer_get_reveal_child(GTK_REVEALER(self->mkdir_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->mkdir_revealer), show);
    if (show) {
        gtk_editable_set_text(GTK_EDITABLE(self->mkdir_entry), "New Folder");
        gtk_widget_grab_focus(self->mkdir_entry);
        gtk_editable_select_region(GTK_EDITABLE(self->mkdir_entry), 0, -1);
    }
}

static void on_chooser_mkdir_submit(GtkWidget *w G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->mkdir_revealer), FALSE);
    const char *name = gtk_editable_get_text(GTK_EDITABLE(self->mkdir_entry));
    if (!name || !name[0] || !self->current_dir) return;
    GWeakRef *wr = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr, self);
    kajo_files_op_mkdir_async(self->current_dir, name, self->ops_cancel, on_chooser_mkdir_done, wr);
}

static void on_chooser_connect_submit(GtkWidget *w G_GNUC_UNUSED, gpointer ud)
{
    KajoFilesChooser *self = ud;
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->connect_revealer), FALSE);
    g_autofree char *uri = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->connect_entry)));
    if (uri) {
        g_strstrip(uri);
        if (uri[0])
            chooser_navigate_uri(self, uri);
    }
}

static void on_chooser_sidebar_nav(const char *uri, gpointer ud) { chooser_navigate_uri(ud, uri); }
static void on_chooser_sidebar_status(const char *msg, gboolean err, gpointer ud) { chooser_set_status(ud, msg, err); }
static void on_chooser_sidebar_connect(gpointer ud)
{
    KajoFilesChooser *self = ud;
    gboolean rev = !gtk_revealer_get_reveal_child(GTK_REVEALER(self->connect_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->connect_revealer), rev);
    if (rev)
        gtk_widget_grab_focus(self->connect_entry);
}

static void
on_chooser_ql_action(KajoQuickLookAction action, KajoFileItem *item, gpointer ud)
{
    KajoFilesChooser *self = ud;
    if (action == KAJO_QL_ACTION_OPEN_DEFAULT && item)
        chooser_try_accept(self, FALSE);
}

static gboolean
on_chooser_key_pressed(GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                       guint                  keyval,
                       guint                  keycode G_GNUC_UNUSED,
                       GdkModifierType        state,
                       gpointer               ud)
{
    KajoFilesChooser *self = ud;
    if (keyval == GDK_KEY_Escape) {
        if (self->quicklook && kajo_quicklook_is_visible(self->quicklook)) {
            kajo_quicklook_hide(self->quicklook);
            return TRUE;
        }
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->overwrite_revealer))) {
            on_chooser_overwrite_no(NULL, self);
            return TRUE;
        }
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->mkdir_revealer))) {
            gtk_revealer_set_reveal_child(GTK_REVEALER(self->mkdir_revealer), FALSE);
            return TRUE;
        }
        chooser_complete(self, 1, NULL);
        return TRUE;
    }

    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(self));
    gboolean in_entry = focus && GTK_IS_EDITABLE(focus);

    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) {
        on_chooser_uri_edit(NULL, self);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
        gtk_widget_grab_focus(self->search_entry);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_h || keyval == GDK_KEY_H)) {
        on_chooser_hidden(NULL, self);
        return TRUE;
    }
    if (!in_entry && keyval == GDK_KEY_space && self->quicklook) {
        kajo_quicklook_toggle(self->quicklook);
        return TRUE;
    }
    if (!in_entry && (keyval == GDK_KEY_BackSpace || ((state & GDK_ALT_MASK) && keyval == GDK_KEY_Up))) {
        on_chooser_up(NULL, self);
        return TRUE;
    }
    return FALSE;
}

static void
on_chooser_map(GtkWidget *widget, gpointer ud G_GNUC_UNUSED)
{
    KajoFilesChooser *self = KAJO_FILES_CHOOSER(widget);
    if (!self->spec || !self->spec->parent_window)
        return;

    const char *pw = self->spec->parent_window;
    if (g_str_has_prefix(pw, "wayland:") && pw[8] != '\0') {
        GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(widget));
        if (surf && GDK_IS_WAYLAND_TOPLEVEL(surf))
            gdk_wayland_toplevel_set_transient_for_exported(GDK_TOPLEVEL(surf), pw + 8);
    }
}

static gboolean
on_chooser_close_request(GtkWindow *win, gpointer ud G_GNUC_UNUSED)
{
    KajoFilesChooser *self = KAJO_FILES_CHOOSER(win);
    if (!self->responded) {
        self->responded = TRUE;
        KajoPortalActiveRequest *req = self->active_req;
        self->active_req = NULL;
        if (req) {
            kajo_portal_active_request_set_close_cb(req, NULL, NULL);
            kajo_portal_active_request_finish(req, 1, NULL);
        }
    }
    return FALSE;
}

static void
on_portal_request_closed(KajoPortalActiveRequest *req G_GNUC_UNUSED, gpointer user_data)
{
    GWeakRef *wr = user_data;
    g_autoptr(KajoFilesChooser) self = g_weak_ref_get(wr);
    if (self && !self->responded) {
        self->responded = TRUE;
        self->active_req = NULL;
        gtk_window_close(GTK_WINDOW(self));
    }
}

/* ─── UI Construction ─── */

static const char *
mode_badge_text(const KajoPortalChooserRequest *spec)
{
    if (!spec) return "OPEN FILE";
    if (spec->mode == KAJO_PORTAL_MODE_SAVE_FILE)  return "SAVE FILE";
    if (spec->mode == KAJO_PORTAL_MODE_SAVE_FILES) return "SAVE FILES";
    if (spec->directory) return spec->multiple ? "SELECT FOLDERS" : "SELECT FOLDER";
    return spec->multiple ? "OPEN MULTIPLE" : "OPEN FILE";
}

static void
build_chooser_header(KajoFilesChooser *self, GtkWidget *root_vbox)
{
    GtkWidget *handle = gtk_window_handle_new();
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hdr, "settings-header-bar");
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

    GtkWidget *badge = gtk_label_new(mode_badge_text(self->spec));
    gtk_widget_add_css_class(badge, "shell-popover-badge");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

    GtkWidget *title = gtk_label_new(self->spec ? self->spec->title : "Select File");
    gtk_widget_add_css_class(title, "settings-header-title");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_halign(title, GTK_ALIGN_FILL);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_set_valign(title, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);

    GtkWidget *btn_close = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(btn_close, "settings-close-btn");
    gtk_widget_set_valign(btn_close, GTK_ALIGN_CENTER);
    g_signal_connect(btn_close, "clicked", G_CALLBACK(on_chooser_cancel), self);

    gtk_box_append(GTK_BOX(hdr), badge);
    gtk_box_append(GTK_BOX(hdr), title);
    gtk_box_append(GTK_BOX(hdr), btn_close);
    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), hdr);
    gtk_box_append(GTK_BOX(root_vbox), handle);
}

static void
build_chooser_toolbar(KajoFilesChooser *self, GtkWidget *root_vbox)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(bar, "files-chooser-toolbar");
    gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);

    self->btn_back    = gtk_button_new_with_label("←");
    self->btn_forward = gtk_button_new_with_label("→");
    self->btn_up      = gtk_button_new_with_label("↑");
    self->btn_reload  = gtk_button_new_with_label("↻");
    GtkWidget *navs[4] = { self->btn_back, self->btn_forward, self->btn_up, self->btn_reload };
    for (int i = 0; i < 4; i++)
        gtk_widget_add_css_class(navs[i], "files-nav-btn");

    g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_chooser_back), self);
    g_signal_connect(self->btn_forward, "clicked", G_CALLBACK(on_chooser_fwd), self);
    g_signal_connect(self->btn_up, "clicked", G_CALLBACK(on_chooser_up), self);
    g_signal_connect(self->btn_reload, "clicked", G_CALLBACK(on_chooser_reload), self);

    self->location_stack = gtk_stack_new();
    gtk_widget_set_hexpand(self->location_stack, TRUE);
    GtkWidget *bc_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bc_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    self->breadcrumb_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(bc_scroll), self->breadcrumb_box);
    gtk_stack_add_named(GTK_STACK(self->location_stack), bc_scroll, "breadcrumbs");

    self->uri_entry = gtk_entry_new();
    g_signal_connect(self->uri_entry, "activate", G_CALLBACK(on_chooser_uri_activate), self);
    gtk_stack_add_named(GTK_STACK(self->location_stack), self->uri_entry, "entry");

    GtkWidget *btn_edit = gtk_button_new_with_label("✎");
    gtk_widget_add_css_class(btn_edit, "files-nav-btn");
    g_signal_connect(btn_edit, "clicked", G_CALLBACK(on_chooser_uri_edit), self);

    self->search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(self->search_entry, 160, -1);
    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_chooser_search), self);

    self->btn_view_grid = gtk_button_new_with_label("⊞");
    self->btn_view_list = gtk_button_new_with_label("☰");
    self->btn_hidden    = gtk_button_new_with_label("👁");
    self->btn_mkdir     = gtk_button_new_with_label("+ 📁");
    gtk_widget_add_css_class(self->btn_view_grid, "files-nav-btn");
    gtk_widget_add_css_class(self->btn_view_list, "files-nav-btn");
    gtk_widget_add_css_class(self->btn_view_list, "active");
    gtk_widget_add_css_class(self->btn_hidden, "files-nav-btn");
    gtk_widget_add_css_class(self->btn_mkdir, "files-nav-btn");
    gtk_widget_set_tooltip_text(self->btn_hidden, "Show Hidden Files (Ctrl+H)");
    gtk_widget_set_tooltip_text(self->btn_mkdir, "Create New Folder");
    g_signal_connect(self->btn_view_grid, "clicked", G_CALLBACK(on_chooser_view_grid), self);
    g_signal_connect(self->btn_view_list, "clicked", G_CALLBACK(on_chooser_view_list), self);
    g_signal_connect(self->btn_hidden, "clicked", G_CALLBACK(on_chooser_hidden), self);
    g_signal_connect(self->btn_mkdir, "clicked", G_CALLBACK(on_chooser_mkdir_toggle), self);

    gboolean show_mkdir = self->spec && (self->spec->mode != KAJO_PORTAL_MODE_OPEN_FILE || self->spec->directory);
    gtk_widget_set_visible(self->btn_mkdir, show_mkdir);

    gtk_box_append(GTK_BOX(bar), self->btn_back);
    gtk_box_append(GTK_BOX(bar), self->btn_forward);
    gtk_box_append(GTK_BOX(bar), self->btn_up);
    gtk_box_append(GTK_BOX(bar), self->btn_reload);
    gtk_box_append(GTK_BOX(bar), self->location_stack);
    gtk_box_append(GTK_BOX(bar), btn_edit);
    gtk_box_append(GTK_BOX(bar), self->search_entry);
    gtk_box_append(GTK_BOX(bar), self->btn_mkdir);
    gtk_box_append(GTK_BOX(bar), self->btn_hidden);
    gtk_box_append(GTK_BOX(bar), self->btn_view_grid);
    gtk_box_append(GTK_BOX(bar), self->btn_view_list);
    gtk_box_append(GTK_BOX(root_vbox), bar);
}

static void
build_chooser_banners(KajoFilesChooser *self, GtkWidget *pane)
{
    self->auth_revealer = gtk_revealer_new();
    gtk_box_append(GTK_BOX(pane), self->auth_revealer);

    /* Connect to Server banner */
    self->connect_revealer = gtk_revealer_new();
    GtkWidget *c_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(c_box, "files-context-banner");
    self->connect_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->connect_entry), "smb://server/share or sftp://host/path");
    gtk_widget_set_hexpand(self->connect_entry, TRUE);
    g_signal_connect(self->connect_entry, "activate", G_CALLBACK(on_chooser_connect_submit), self);
    GtkWidget *c_ok = gtk_button_new_with_label("CONNECT");
    gtk_widget_add_css_class(c_ok, "action-btn-primary");
    g_signal_connect(c_ok, "clicked", G_CALLBACK(on_chooser_connect_submit), self);
    gtk_box_append(GTK_BOX(c_box), gtk_label_new("SERVER:"));
    gtk_box_append(GTK_BOX(c_box), self->connect_entry);
    gtk_box_append(GTK_BOX(c_box), c_ok);
    gtk_revealer_set_child(GTK_REVEALER(self->connect_revealer), c_box);
    gtk_box_append(GTK_BOX(pane), self->connect_revealer);

    /* New Folder inline banner */
    self->mkdir_revealer = gtk_revealer_new();
    GtkWidget *m_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(m_box, "files-context-banner");
    self->mkdir_entry = gtk_entry_new();
    gtk_widget_set_hexpand(self->mkdir_entry, TRUE);
    g_signal_connect(self->mkdir_entry, "activate", G_CALLBACK(on_chooser_mkdir_submit), self);
    GtkWidget *m_ok = gtk_button_new_with_label("CREATE");
    gtk_widget_add_css_class(m_ok, "action-btn-primary");
    g_signal_connect(m_ok, "clicked", G_CALLBACK(on_chooser_mkdir_submit), self);
    gtk_box_append(GTK_BOX(m_box), gtk_label_new("NEW FOLDER:"));
    gtk_box_append(GTK_BOX(m_box), self->mkdir_entry);
    gtk_box_append(GTK_BOX(m_box), m_ok);
    gtk_revealer_set_child(GTK_REVEALER(self->mkdir_revealer), m_box);
    gtk_box_append(GTK_BOX(pane), self->mkdir_revealer);

    /* Overwrite confirmation banner */
    self->overwrite_revealer = gtk_revealer_new();
    GtkWidget *o_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(o_box, "files-context-banner");
    self->overwrite_lbl = gtk_label_new("FILE EXISTS — OVERWRITE?");
    gtk_widget_set_hexpand(self->overwrite_lbl, TRUE);
    gtk_widget_set_halign(self->overwrite_lbl, GTK_ALIGN_START);
    GtkWidget *o_yes = gtk_button_new_with_label("OVERWRITE");
    gtk_widget_add_css_class(o_yes, "action-btn-danger");
    g_signal_connect(o_yes, "clicked", G_CALLBACK(on_chooser_overwrite_ok), self);
    GtkWidget *o_no = gtk_button_new_with_label("CANCEL");
    gtk_widget_add_css_class(o_no, "flat");
    g_signal_connect(o_no, "clicked", G_CALLBACK(on_chooser_overwrite_no), self);
    gtk_box_append(GTK_BOX(o_box), self->overwrite_lbl);
    gtk_box_append(GTK_BOX(o_box), o_yes);
    gtk_box_append(GTK_BOX(o_box), o_no);
    gtk_revealer_set_child(GTK_REVEALER(self->overwrite_revealer), o_box);
    gtk_box_append(GTK_BOX(pane), self->overwrite_revealer);
}

static void
build_chooser_footer(KajoFilesChooser *self, GtkWidget *root_vbox)
{
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(foot, "files-chooser-footer");

    /* Optional caller choices row */
    if (self->spec && self->spec->choices && self->spec->choices->len > 0) {
        GtkWidget *ch_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        for (guint i = 0; i < self->spec->choices->len; i++) {
            const KajoPortalChoice *ch = g_ptr_array_index(self->spec->choices, i);
            ChooserChoiceBinding *b = g_new0(ChooserChoiceBinding, 1);
            b->id = g_strdup(ch->id);
            if (!ch->options || ch->options->len == 0) {
                b->is_checkbox = TRUE;
                b->widget = gtk_check_button_new_with_label(ch->label);
                gtk_check_button_set_active(GTK_CHECK_BUTTON(b->widget),
                                            g_strcmp0(ch->initial, "true") == 0);
                gtk_box_append(GTK_BOX(ch_row), b->widget);
            } else {
                b->is_checkbox = FALSE;
                b->option_ids = g_ptr_array_new_with_free_func(g_free);
                GtkStringList *sl = gtk_string_list_new(NULL);
                guint init_idx = 0;
                for (guint j = 0; j < ch->options->len; j++) {
                    const KajoPortalChoiceOption *opt = g_ptr_array_index(ch->options, j);
                    g_ptr_array_add(b->option_ids, g_strdup(opt->id));
                    gtk_string_list_append(sl, opt->label);
                    if (g_strcmp0(opt->id, ch->initial) == 0)
                        init_idx = j;
                }
                b->widget = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(b->widget), init_idx);
                GtkWidget *pair = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                gtk_widget_set_valign(pair, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(pair), gtk_label_new(ch->label));
                gtk_box_append(GTK_BOX(pair), b->widget);
                gtk_box_append(GTK_BOX(ch_row), pair);
            }
            g_ptr_array_add(self->choice_bindings, b);
        }
        gtk_box_append(GTK_BOX(foot), ch_row);
    }

    /* Filename entry in SaveFile mode */
    if (self->spec && self->spec->mode == KAJO_PORTAL_MODE_SAVE_FILE) {
        GtkWidget *fn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(fn_row, GTK_ALIGN_CENTER);
        GtkWidget *fn_lbl = gtk_label_new("FILENAME:");
        gtk_widget_add_css_class(fn_lbl, "files-meta-key");
        self->filename_entry = gtk_entry_new();
        gtk_widget_set_hexpand(self->filename_entry, TRUE);
        if (self->spec->current_name)
            gtk_editable_set_text(GTK_EDITABLE(self->filename_entry), self->spec->current_name);
        g_signal_connect(self->filename_entry, "activate", G_CALLBACK(on_chooser_accept), self);
        gtk_box_append(GTK_BOX(fn_row), fn_lbl);
        gtk_box_append(GTK_BOX(fn_row), self->filename_entry);
        gtk_box_append(GTK_BOX(foot), fn_row);
    }

    /* Bottom action row: status + filter dropdown + CANCEL + ACCEPT */
    GtkWidget *act = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(act, GTK_ALIGN_CENTER);

    self->status_lbl = gtk_label_new("");
    gtk_widget_add_css_class(self->status_lbl, "files-status-text");
    gtk_widget_set_hexpand(self->status_lbl, TRUE);
    gtk_widget_set_halign(self->status_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(self->status_lbl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(act), self->status_lbl);

    if (self->spec && self->spec->filters && self->spec->filters->len > 0) {
        GtkStringList *flist = gtk_string_list_new(NULL);
        for (guint i = 0; i < self->spec->filters->len; i++) {
            const KajoPortalFilter *f = g_ptr_array_index(self->spec->filters, i);
            gtk_string_list_append(flist, f->label);
        }
        gtk_string_list_append(flist, "All Files (*)");
        self->filter_dropdown = gtk_drop_down_new(G_LIST_MODEL(flist), NULL);
        guint init_sel = (self->spec->current_filter_index >= 0 &&
                          (guint)self->spec->current_filter_index < self->spec->filters->len)
            ? (guint)self->spec->current_filter_index : self->spec->filters->len;
        self->active_filter_idx = (init_sel < self->spec->filters->len) ? (gint)init_sel : -1;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->filter_dropdown), init_sel);
        g_signal_connect(self->filter_dropdown, "notify::selected",
                         G_CALLBACK(on_chooser_filter_changed), self);
        gtk_box_append(GTK_BOX(act), self->filter_dropdown);
    }

    self->btn_cancel = gtk_button_new_with_label("CANCEL");
    gtk_widget_add_css_class(self->btn_cancel, "files-nav-btn");
    g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_chooser_cancel), self);

    const char *def_acc = (self->spec && self->spec->mode != KAJO_PORTAL_MODE_OPEN_FILE)
        ? "✓ SAVE" : (self->spec && self->spec->directory ? "✓ SELECT" : "✓ OPEN");
    g_autofree char *acc_lbl = (self->spec && self->spec->accept_label && self->spec->accept_label[0])
        ? g_strdup_printf("✓ %s", self->spec->accept_label) : g_strdup(def_acc);
    self->btn_accept = gtk_button_new_with_label(acc_lbl);
    gtk_widget_add_css_class(self->btn_accept, "action-btn-primary");
    g_signal_connect(self->btn_accept, "clicked", G_CALLBACK(on_chooser_accept), self);

    gtk_box_append(GTK_BOX(act), self->btn_cancel);
    gtk_box_append(GTK_BOX(act), self->btn_accept);
    gtk_box_append(GTK_BOX(foot), act);
    gtk_box_append(GTK_BOX(root_vbox), foot);
}

static void
chooser_populate_ui(KajoFilesChooser *self)
{
    gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(self), 880, 580);
    gtk_window_set_title(GTK_WINDOW(self), self->spec ? self->spec->title : "Kajo File Chooser");
    gtk_window_set_modal(GTK_WINDOW(self), self->spec ? self->spec->modal : TRUE);

    gtk_widget_add_css_class(GTK_WIDGET(self), "shell-settings-window");
    gtk_widget_add_css_class(GTK_WIDGET(self), "shell-files-window");
    gtk_widget_add_css_class(GTK_WIDGET(self), "shell-files-chooser");

    self->root_overlay = gtk_overlay_new();
    GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_overlay_set_child(GTK_OVERLAY(self->root_overlay), root_vbox);
    gtk_window_set_child(GTK_WINDOW(self), self->root_overlay);

    build_chooser_header(self, root_vbox);
    build_chooser_toolbar(self, root_vbox);

    /* Split body: Sidebar + File view */
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);

    GtkWidget *sb_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sb_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sb_scroll, 210, -1);
    gtk_widget_add_css_class(sb_scroll, "navigation-sidebar");
    self->sidebar_list = gtk_list_box_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sb_scroll), self->sidebar_list);
    gtk_box_append(GTK_BOX(body), sb_scroll);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(right, TRUE);
    gtk_widget_set_vexpand(right, TRUE);
    build_chooser_banners(self, right);

    self->visible_store = g_list_store_new(KAJO_TYPE_FILE_ITEM);
    if (self->spec && self->spec->multiple) {
        self->selection_model = GTK_SELECTION_MODEL(
            gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(self->visible_store))));
    } else {
        GtkSingleSelection *ss = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->visible_store)));
        gtk_single_selection_set_autoselect(ss, FALSE);
        gtk_single_selection_set_can_unselect(ss, TRUE);
        self->selection_model = GTK_SELECTION_MODEL(ss);
    }
    g_signal_connect(self->selection_model, "selection-changed",
                     G_CALLBACK(on_chooser_selection_changed), self);

    self->view_stack = gtk_stack_new();
    gtk_widget_set_vexpand(self->view_stack, TRUE);
    gtk_widget_set_hexpand(self->view_stack, TRUE);

    GtkListItemFactory *gf = gtk_signal_list_item_factory_new();
    g_signal_connect(gf, "setup", G_CALLBACK(on_chooser_grid_setup), NULL);
    g_signal_connect(gf, "bind", G_CALLBACK(on_chooser_grid_bind), NULL);
    self->grid_view = gtk_grid_view_new(g_object_ref(self->selection_model), gf);
    g_signal_connect(self->grid_view, "activate", G_CALLBACK(on_chooser_item_activated), self);
    GtkWidget *g_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(g_scroll), self->grid_view);
    gtk_stack_add_named(GTK_STACK(self->view_stack), g_scroll, "grid");

    GtkListItemFactory *lf = gtk_signal_list_item_factory_new();
    g_signal_connect(lf, "setup", G_CALLBACK(on_chooser_list_setup), NULL);
    g_signal_connect(lf, "bind", G_CALLBACK(on_chooser_list_bind), NULL);
    self->list_view = gtk_list_view_new(g_object_ref(self->selection_model), lf);
    gtk_widget_add_css_class(self->list_view, "files-details-list");
    g_signal_connect(self->list_view, "activate", G_CALLBACK(on_chooser_item_activated), self);
    GtkWidget *l_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(l_scroll), self->list_view);
    gtk_stack_add_named(GTK_STACK(self->view_stack), l_scroll, "list");

    self->empty_lbl = gtk_label_new("NO MATCHING ITEMS");
    gtk_widget_add_css_class(self->empty_lbl, "empty-state-label");
    gtk_stack_add_named(GTK_STACK(self->view_stack), self->empty_lbl, "empty");
    gtk_stack_set_visible_child_name(GTK_STACK(self->view_stack), "list");

    gtk_box_append(GTK_BOX(right), self->view_stack);
    gtk_box_append(GTK_BOX(body), right);
    gtk_box_append(GTK_BOX(root_vbox), body);

    build_chooser_footer(self, root_vbox);

    self->sidebar = kajo_files_sidebar_new(
        GTK_LIST_BOX(self->sidebar_list),
        self->auth_revealer,
        on_chooser_sidebar_nav,
        on_chooser_sidebar_connect,
        on_chooser_sidebar_status,
        self);

    self->quicklook = kajo_quicklook_new(
        GTK_OVERLAY(self->root_overlay),
        NULL,
        on_chooser_ql_action,
        self);

    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_chooser_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), kc);

    g_signal_connect(self, "map", G_CALLBACK(on_chooser_map), NULL);
    g_signal_connect(self, "close-request", G_CALLBACK(on_chooser_close_request), NULL);

    const char *start_dir = (self->spec && self->spec->current_folder &&
                             g_file_test(self->spec->current_folder, G_FILE_TEST_IS_DIR))
        ? self->spec->current_folder : g_get_home_dir();
    chooser_navigate_uri(self, start_dir);
}

/* ─── GObject Lifecycle ─── */

static void
kajo_files_chooser_dispose(GObject *object)
{
    KajoFilesChooser *self = KAJO_FILES_CHOOSER(object);

    if (!self->responded && self->active_req) {
        self->responded = TRUE;
        KajoPortalActiveRequest *req = self->active_req;
        self->active_req = NULL;
        kajo_portal_active_request_set_close_cb(req, NULL, NULL);
        kajo_portal_active_request_finish(req, 1, NULL);
    }

    if (self->enum_cancel) {
        g_cancellable_cancel(self->enum_cancel);
        g_clear_object(&self->enum_cancel);
    }
    if (self->ops_cancel) {
        g_cancellable_cancel(self->ops_cancel);
        g_clear_object(&self->ops_cancel);
    }

    g_clear_pointer(&self->sidebar, kajo_files_sidebar_free);
    g_clear_pointer(&self->quicklook, kajo_quicklook_free);
    g_clear_object(&self->current_dir);
    g_clear_object(&self->selected_item);
    g_clear_object(&self->selection_model);
    g_clear_object(&self->visible_store);
    g_clear_pointer(&self->all_items, g_ptr_array_unref);
    g_clear_pointer(&self->visible_items, g_ptr_array_unref);
    g_clear_pointer(&self->history, g_ptr_array_unref);
    g_clear_pointer(&self->choice_bindings, g_ptr_array_unref);
    g_clear_pointer(&self->last_mount_attempt_uri, g_free);
    g_clear_pointer(&self->pending_overwrite_path, g_free);
    g_clear_pointer(&self->owned_preview, kajo_portal_chooser_request_free);

    G_OBJECT_CLASS(kajo_files_chooser_parent_class)->dispose(object);
}

static void
kajo_files_chooser_init(KajoFilesChooser *self)
{
    self->ops_cancel = g_cancellable_new();
    self->history = g_ptr_array_new_with_free_func(g_free);
    self->history_pos = -1;
    self->view_mode = KAJO_VIEW_LIST;
    self->active_filter_idx = -1;
    self->choice_bindings = g_ptr_array_new_with_free_func(chooser_choice_binding_free);
}

static void
kajo_files_chooser_class_init(KajoFilesChooserClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = kajo_files_chooser_dispose;
}

static void
weak_ref_destroy_notify(gpointer data)
{
    GWeakRef *wr = data;
    g_weak_ref_clear(wr);
    g_free(wr);
}

GtkWidget *
kajo_files_chooser_new_for_request(GtkApplication          *app,
                                   KajoPortalActiveRequest *active_req)
{
    g_return_val_if_fail(active_req != NULL, NULL);

    KajoFilesChooser *self = g_object_new(KAJO_TYPE_FILES_CHOOSER, "application", app, NULL);
    self->active_req = active_req;
    self->spec = kajo_portal_active_request_get_spec(active_req);
    chooser_populate_ui(self);

    GWeakRef *wr = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr, self);
    g_object_set_data_full(G_OBJECT(self), "portal-close-wr", wr, weak_ref_destroy_notify);
    kajo_portal_active_request_set_close_cb(active_req, on_portal_request_closed, wr);
    return GTK_WIDGET(self);
}

GtkWidget *
kajo_files_chooser_new_preview(GtkApplication *app, const char *initial_dir)
{
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "accept_label", g_variant_new_string("_Save"));
    g_variant_builder_add(&opts, "{sv}", "current_name", g_variant_new_string("untitled.md"));
    if (initial_dir && initial_dir[0]) {
        g_variant_builder_add(&opts, "{sv}", "current_folder",
                              g_variant_new_bytestring(initial_dir));
    }
    GVariantBuilder filters;
    g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
    GVariantBuilder r1;
    g_variant_builder_init(&r1, G_VARIANT_TYPE("a(us)"));
    g_variant_builder_add(&r1, "(us)", 0u, "*.md");
    g_variant_builder_add(&r1, "(us)", 0u, "*.txt");
    g_variant_builder_add(&filters, "(sa(us))", "Markdown & Text (*.md, *.txt)", &r1);
    GVariantBuilder r2;
    g_variant_builder_init(&r2, G_VARIANT_TYPE("a(us)"));
    g_variant_builder_add(&r2, "(us)", 1u, "image/*");
    g_variant_builder_add(&filters, "(sa(us))", "Images", &r2);
    g_variant_builder_add(&opts, "{sv}", "filters", g_variant_builder_end(&filters));

    GVariantBuilder choices;
    g_variant_builder_init(&choices, G_VARIANT_TYPE("a(ssa(ss)s)"));
    GVariantBuilder empty_opts;
    g_variant_builder_init(&empty_opts, G_VARIANT_TYPE("a(ss)"));
    g_variant_builder_add(&choices, "(ssa(ss)s)", "readonly", "Open read-only", &empty_opts, "false");
    g_variant_builder_add(&opts, "{sv}", "choices", g_variant_builder_end(&choices));

    g_autoptr(GVariant) opts_v = g_variant_ref_sink(g_variant_builder_end(&opts));

    KajoFilesChooser *self = g_object_new(KAJO_TYPE_FILES_CHOOSER, "application", app, NULL);
    self->owned_preview = kajo_portal_parse_options(
        KAJO_PORTAL_MODE_SAVE_FILE, "org.kajo.Preview", "", "Save Document — Kajo Chooser", opts_v);
    self->spec = self->owned_preview;
    chooser_populate_ui(self);
    return GTK_WIDGET(self);
}
