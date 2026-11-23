#include "files_dnd.h"
#include <string.h>

/* ─── Pure Validation & Action Helpers ─── */

static gboolean
uri_scheme_is(GFile *file, const char *scheme)
{
    if (!file || !scheme)
        return FALSE;
    g_autofree char *s = g_file_get_uri_scheme(file);
    return g_strcmp0(s, scheme) == 0;
}

gboolean
kajo_files_dnd_dest_is_writable(GFile *dest_dir)
{
    if (!dest_dir)
        return FALSE;
    g_autofree char *scheme = g_file_get_uri_scheme(dest_dir);
    if (!scheme || scheme[0] == '\0')
        return FALSE;
    if (g_strcmp0(scheme, "archive") == 0 ||
        g_strcmp0(scheme, "recent") == 0 ||
        g_strcmp0(scheme, "network") == 0)
        return FALSE;
    return TRUE;
}

gboolean
kajo_files_dnd_item_is_draggable(const KajoFileItem *item)
{
    if (!item)
        return FALSE;
    GFile *f = kajo_file_item_get_file(item);
    if (!f)
        return FALSE;
    g_autofree char *scheme = g_file_get_uri_scheme(f);
    if (!scheme || scheme[0] == '\0')
        return FALSE;
    if (g_strcmp0(scheme, "archive") == 0 ||
        g_strcmp0(scheme, "network") == 0 ||
        g_strcmp0(scheme, "trash") == 0 ||
        g_strcmp0(scheme, "recent") == 0)
        return FALSE;
    return TRUE;
}

gboolean
kajo_files_dnd_can_drop_files(GPtrArray *sources,
                              GFile     *dest_dir,
                              gboolean  *out_is_trash)
{
    if (out_is_trash)
        *out_is_trash = FALSE;
    if (!sources || sources->len == 0 || !dest_dir)
        return FALSE;
    if (!kajo_files_dnd_dest_is_writable(dest_dir))
        return FALSE;

    gboolean is_trash = uri_scheme_is(dest_dir, "trash");
    if (out_is_trash)
        *out_is_trash = is_trash;

    for (guint i = 0; i < sources->len; i++) {
        GFile *src = g_ptr_array_index(sources, i);
        if (!G_IS_FILE(src))
            return FALSE;
        if (uri_scheme_is(src, "archive") ||
            uri_scheme_is(src, "network") ||
            uri_scheme_is(src, "trash"))
            return FALSE;
        if (!is_trash) {
            /* Disallow dropping a folder onto itself or inside its own subtree */
            if (g_file_equal(src, dest_dir) || g_file_has_prefix(dest_dir, src))
                return FALSE;
        }
    }
    return TRUE;
}

static gboolean
files_share_filesystem(GFile *a, GFile *b)
{
    if (!a || !b)
        return FALSE;
    g_autofree char *sa = g_file_get_uri_scheme(a);
    g_autofree char *sb = g_file_get_uri_scheme(b);
    if (g_strcmp0(sa, sb) != 0)
        return FALSE;

    g_autoptr(GFileInfo) ia = g_file_query_info(
        a, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    g_autoptr(GFileInfo) ib = g_file_query_info(
        b, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (ia && ib) {
        const char *fs_a = g_file_info_get_attribute_string(ia, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
        const char *fs_b = g_file_info_get_attribute_string(ib, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
        if (fs_a && fs_b)
            return g_strcmp0(fs_a, fs_b) == 0;
    }
    return g_strcmp0(sa, "file") == 0;
}

static gboolean
all_sources_already_in_dir(GPtrArray *sources, GFile *dest_dir)
{
    if (!sources || sources->len == 0 || !dest_dir)
        return FALSE;
    for (guint i = 0; i < sources->len; i++) {
        GFile *src = g_ptr_array_index(sources, i);
        g_autoptr(GFile) parent = src ? g_file_get_parent(src) : NULL;
        if (!parent || !g_file_equal(parent, dest_dir))
            return FALSE;
    }
    return TRUE;
}

GdkDragAction
kajo_files_dnd_choose_action(GPtrArray     *sources,
                             GFile         *dest_dir,
                             GdkDragAction  preferred_action)
{
    gboolean is_trash = FALSE;
    if (!kajo_files_dnd_can_drop_files(sources, dest_dir, &is_trash))
        return 0;

    if (is_trash)
        return GDK_ACTION_MOVE;

    gboolean same_dir = all_sources_already_in_dir(sources, dest_dir);

    if (preferred_action == GDK_ACTION_COPY)
        return GDK_ACTION_COPY;

    if (preferred_action == GDK_ACTION_MOVE)
        return same_dir ? 0 : GDK_ACTION_MOVE;

    /* Default (unspecified or ASK): MOVE on same filesystem, COPY across filesystems */
    GFile *first = g_ptr_array_index(sources, 0);
    if (files_share_filesystem(first, dest_dir))
        return same_dir ? 0 : GDK_ACTION_MOVE;
    return GDK_ACTION_COPY;
}

GPtrArray *
kajo_files_dnd_extract_files_from_value(const GValue *value)
{
    if (!value)
        return NULL;

    GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *fl = g_value_get_boxed(value);
        GSList *list = fl ? gdk_file_list_get_files(fl) : NULL;
        for (GSList *l = list; l != NULL; l = l->next) {
            if (G_IS_FILE(l->data))
                g_ptr_array_add(files, g_object_ref(l->data));
        }
        g_slist_free(list);
    } else if (G_VALUE_HOLDS_STRING(value)) {
        const char *str = g_value_get_string(value);
        if (str && str[0] != '\0') {
            g_auto(GStrv) lines = g_strsplit_set(str, "\r\n", -1);
            for (int i = 0; lines && lines[i] != NULL; i++) {
                char *line = g_strstrip(lines[i]);
                if (line[0] == '\0' || line[0] == '#')
                    continue;
                GFile *f = strstr(line, "://")
                    ? g_file_new_for_uri(line)
                    : g_file_new_for_path(line);
                if (f)
                    g_ptr_array_add(files, f);
            }
        }
    }

    if (files->len == 0) {
        g_ptr_array_unref(files);
        return NULL;
    }
    return files;
}

/* ─── GTK4 DragSource (Item Outbound Drag) ─── */

typedef struct {
    KajoDndGetDragFilesFunc get_drag_cb;
    KajoDndExecuteDropFunc  drop_cb;
    gpointer                user_data;
} ItemDndCtx;

static GdkContentProvider *
on_item_drag_prepare(GtkDragSource *source,
                     double         x G_GNUC_UNUSED,
                     double         y G_GNUC_UNUSED,
                     gpointer       user_data)
{
    ItemDndCtx *ctx = user_data;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
    KajoFileItem *item = w ? g_object_get_data(G_OBJECT(w), "file-item") : NULL;
    if (!item || !kajo_files_dnd_item_is_draggable(item))
        return NULL;

    g_autoptr(GPtrArray) files = ctx->get_drag_cb
        ? ctx->get_drag_cb(item, ctx->user_data)
        : NULL;

    if (!files || files->len == 0) {
        GFile *f = kajo_file_item_get_file(item);
        if (!f)
            return NULL;
        files = g_ptr_array_new_with_free_func(g_object_unref);
        g_ptr_array_add(files, g_object_ref(f));
    }

    GSList *flist = NULL;
    for (guint i = 0; i < files->len; i++) {
        GFile *f = g_ptr_array_index(files, i);
        if (G_IS_FILE(f))
            flist = g_slist_prepend(flist, f);
    }
    flist = g_slist_reverse(flist);
    if (!flist)
        return NULL;

    GdkFileList *gdk_list = gdk_file_list_new_from_list(flist);
    g_slist_free(flist);

    g_object_set_data(G_OBJECT(source), "dnd-count", GUINT_TO_POINTER(files->len));
    g_object_set_data_full(G_OBJECT(source), "dnd-label",
                           g_strdup(kajo_file_item_get_display_name(item)), g_free);
    g_object_set_data_full(G_OBJECT(source), "dnd-icon",
                           g_strdup(kajo_file_item_get_icon_name(item)), g_free);

    GdkContentProvider *provider = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, gdk_list);
    g_boxed_free(GDK_TYPE_FILE_LIST, gdk_list);
    return provider;
}

static void
on_item_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data G_GNUC_UNUSED)
{
    guint count = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(source), "dnd-count"));
    const char *label = g_object_get_data(G_OBJECT(source), "dnd-label");
    const char *icon_name = g_object_get_data(G_OBJECT(source), "dnd-icon");

    GtkWidget *badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(badge, "files-dnd-badge");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

    GtkWidget *ic = gtk_image_new_from_icon_name(icon_name ? icon_name : "text-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(ic), 18);
    gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);

    g_autofree char *text = (count > 1)
        ? g_strdup_printf("%u ITEMS", count)
        : g_strdup(label ? label : "1 ITEM");
    GtkWidget *lbl = gtk_label_new(text);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 28);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);

    gtk_box_append(GTK_BOX(badge), ic);
    gtk_box_append(GTK_BOX(badge), lbl);

    GtkWidget *drag_icon = gtk_drag_icon_get_for_drag(drag);
    gtk_drag_icon_set_child(GTK_DRAG_ICON(drag_icon), badge);
}

/* ─── GTK4 DropTarget (Folder Item Inbound Drop) ─── */

static GFile *
resolve_folder_item_dest(GtkWidget *w)
{
    KajoFileItem *item = w ? g_object_get_data(G_OBJECT(w), "file-item") : NULL;
    if (!item || !kajo_file_item_is_dir(item))
        return NULL;
    GFile *dest = kajo_file_item_get_file(item);
    if (!dest || !kajo_files_dnd_dest_is_writable(dest))
        return NULL;
    return g_object_ref(dest);
}

static GdkDragAction
on_item_drop_enter_motion(GtkDropTarget *target,
                          double         x G_GNUC_UNUSED,
                          double         y G_GNUC_UNUSED,
                          gpointer       user_data G_GNUC_UNUSED)
{
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    g_autoptr(GFile) dest = resolve_folder_item_dest(w);
    if (!dest)
        return 0;

    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    GdkDragAction acts = drop ? gdk_drop_get_actions(drop) : GDK_ACTION_COPY;
    GdkDragAction chosen = (acts & GDK_ACTION_MOVE) ? GDK_ACTION_MOVE : GDK_ACTION_COPY;

    if (w)
        gtk_widget_add_css_class(w, "files-drop-target-hover");
    return chosen;
}

static void
on_widget_drop_leave(GtkDropTarget *target, gpointer user_data G_GNUC_UNUSED)
{
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    if (w)
        gtk_widget_remove_css_class(w, "files-drop-target-hover");
}

static gboolean
on_item_drop_perform(GtkDropTarget *target,
                     const GValue  *value,
                     double         x G_GNUC_UNUSED,
                     double         y G_GNUC_UNUSED,
                     gpointer       user_data)
{
    ItemDndCtx *ctx = user_data;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    if (w)
        gtk_widget_remove_css_class(w, "files-drop-target-hover");

    g_autoptr(GFile) dest = resolve_folder_item_dest(w);
    g_autoptr(GPtrArray) files = kajo_files_dnd_extract_files_from_value(value);
    if (!dest || !files || !ctx->drop_cb)
        return FALSE;

    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    GdkDragAction pref = drop ? gdk_drop_get_actions(drop) : 0;
    if (pref == (GDK_ACTION_COPY | GDK_ACTION_MOVE))
        pref = 0; /* let chooser decide by filesystem */
    GdkDragAction eff = kajo_files_dnd_choose_action(files, dest, pref);
    if (eff == 0)
        return FALSE;

    return ctx->drop_cb(files, dest, eff, ctx->user_data);
}

void
kajo_files_dnd_attach_item_controllers(GtkWidget               *item_widget,
                                       KajoDndGetDragFilesFunc  get_drag_cb,
                                       KajoDndExecuteDropFunc   drop_cb,
                                       gpointer                 user_data)
{
    g_return_if_fail(GTK_IS_WIDGET(item_widget));

    ItemDndCtx *ctx = g_new0(ItemDndCtx, 1);
    ctx->get_drag_cb = get_drag_cb;
    ctx->drop_cb = drop_cb;
    ctx->user_data = user_data;
    g_object_set_data_full(G_OBJECT(item_widget), "kajo-item-dnd-ctx", ctx, g_free);

    GtkDragSource *src = gtk_drag_source_new();
    gtk_drag_source_set_actions(src, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(src, "prepare", G_CALLBACK(on_item_drag_prepare), ctx);
    g_signal_connect(src, "drag-begin", G_CALLBACK(on_item_drag_begin), ctx);
    gtk_widget_add_controller(item_widget, GTK_EVENT_CONTROLLER(src));

    GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(dt, "enter", G_CALLBACK(on_item_drop_enter_motion), ctx);
    g_signal_connect(dt, "motion", G_CALLBACK(on_item_drop_enter_motion), ctx);
    g_signal_connect(dt, "leave", G_CALLBACK(on_widget_drop_leave), ctx);
    g_signal_connect(dt, "drop", G_CALLBACK(on_item_drop_perform), ctx);
    gtk_widget_add_controller(item_widget, GTK_EVENT_CONTROLLER(dt));
}

/* ─── GTK4 DropTarget (View Canvas Inbound Drop) ─── */

typedef struct {
    KajoDndGetCurrentDirFunc get_dir_cb;
    KajoDndExecuteDropFunc   drop_cb;
    gpointer                 user_data;
} ViewDndCtx;

static GdkDragAction
on_view_drop_enter_motion(GtkDropTarget *target,
                          double         x G_GNUC_UNUSED,
                          double         y G_GNUC_UNUSED,
                          gpointer       user_data)
{
    ViewDndCtx *ctx = user_data;
    g_autoptr(GFile) dest = ctx->get_dir_cb ? ctx->get_dir_cb(ctx->user_data) : NULL;
    if (!dest || !kajo_files_dnd_dest_is_writable(dest))
        return 0;

    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    if (w)
        gtk_widget_add_css_class(w, "files-drop-target-hover");

    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    GdkDragAction acts = drop ? gdk_drop_get_actions(drop) : GDK_ACTION_COPY;
    return (acts & GDK_ACTION_MOVE) ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
}

static gboolean
on_view_drop_perform(GtkDropTarget *target,
                     const GValue  *value,
                     double         x G_GNUC_UNUSED,
                     double         y G_GNUC_UNUSED,
                     gpointer       user_data)
{
    ViewDndCtx *ctx = user_data;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    if (w)
        gtk_widget_remove_css_class(w, "files-drop-target-hover");

    g_autoptr(GFile) dest = ctx->get_dir_cb ? ctx->get_dir_cb(ctx->user_data) : NULL;
    g_autoptr(GPtrArray) files = kajo_files_dnd_extract_files_from_value(value);
    if (!dest || !files || !ctx->drop_cb)
        return FALSE;

    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    GdkDragAction pref = drop ? gdk_drop_get_actions(drop) : 0;
    if (pref == (GDK_ACTION_COPY | GDK_ACTION_MOVE))
        pref = 0;
    GdkDragAction eff = kajo_files_dnd_choose_action(files, dest, pref);
    if (eff == 0)
        return FALSE;

    return ctx->drop_cb(files, dest, eff, ctx->user_data);
}

void
kajo_files_dnd_attach_view_drop_target(GtkWidget                *view_widget,
                                       KajoDndGetCurrentDirFunc  get_dir_cb,
                                       KajoDndExecuteDropFunc    drop_cb,
                                       gpointer                  user_data)
{
    g_return_if_fail(GTK_IS_WIDGET(view_widget));

    ViewDndCtx *ctx = g_new0(ViewDndCtx, 1);
    ctx->get_dir_cb = get_dir_cb;
    ctx->drop_cb = drop_cb;
    ctx->user_data = user_data;
    g_object_set_data_full(G_OBJECT(view_widget), "kajo-view-dnd-ctx", ctx, g_free);

    GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(dt, "enter", G_CALLBACK(on_view_drop_enter_motion), ctx);
    g_signal_connect(dt, "motion", G_CALLBACK(on_view_drop_enter_motion), ctx);
    g_signal_connect(dt, "leave", G_CALLBACK(on_widget_drop_leave), ctx);
    g_signal_connect(dt, "drop", G_CALLBACK(on_view_drop_perform), ctx);
    gtk_widget_add_controller(view_widget, GTK_EVENT_CONTROLLER(dt));
}

/* ─── GTK4 DropTarget (Fixed URI Target: Breadcrumb / Sidebar Row) ─── */

typedef struct {
    char                   *dest_uri;
    KajoDndExecuteDropFunc  drop_cb;
    gpointer                user_data;
} UriDndCtx;

static void
uri_dnd_ctx_free(gpointer data)
{
    UriDndCtx *ctx = data;
    if (!ctx) return;
    g_free(ctx->dest_uri);
    g_free(ctx);
}

static GdkDragAction
on_uri_drop_enter_motion(GtkDropTarget *target,
                         double         x G_GNUC_UNUSED,
                         double         y G_GNUC_UNUSED,
                         gpointer       user_data)
{
    UriDndCtx *ctx = user_data;
    g_autoptr(GFile) dest = ctx->dest_uri ? g_file_new_for_uri(ctx->dest_uri) : NULL;
    if (!dest || !kajo_files_dnd_dest_is_writable(dest))
        return 0;

    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    if (w)
        gtk_widget_add_css_class(w, "files-drop-target-hover");

    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    GdkDragAction acts = drop ? gdk_drop_get_actions(drop) : GDK_ACTION_COPY;
    return (acts & GDK_ACTION_MOVE) ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
}

static gboolean
on_uri_drop_perform(GtkDropTarget *target,
                    const GValue  *value,
                    double         x G_GNUC_UNUSED,
                    double         y G_GNUC_UNUSED,
                    gpointer       user_data)
{
    UriDndCtx *ctx = user_data;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    if (w)
        gtk_widget_remove_css_class(w, "files-drop-target-hover");

    g_autoptr(GFile) dest = ctx->dest_uri ? g_file_new_for_uri(ctx->dest_uri) : NULL;
    g_autoptr(GPtrArray) files = kajo_files_dnd_extract_files_from_value(value);
    if (!dest || !files || !ctx->drop_cb)
        return FALSE;

    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    GdkDragAction pref = drop ? gdk_drop_get_actions(drop) : 0;
    if (pref == (GDK_ACTION_COPY | GDK_ACTION_MOVE))
        pref = 0;
    GdkDragAction eff = kajo_files_dnd_choose_action(files, dest, pref);
    if (eff == 0)
        return FALSE;

    return ctx->drop_cb(files, dest, eff, ctx->user_data);
}

void
kajo_files_dnd_attach_uri_drop_target(GtkWidget              *target_widget,
                                      const char             *dest_uri,
                                      KajoDndExecuteDropFunc  drop_cb,
                                      gpointer                user_data)
{
    g_return_if_fail(GTK_IS_WIDGET(target_widget));
    if (!dest_uri || !drop_cb)
        return;

    g_autoptr(GFile) test_dest = g_file_new_for_uri(dest_uri);
    if (!kajo_files_dnd_dest_is_writable(test_dest))
        return;

    UriDndCtx *ctx = g_new0(UriDndCtx, 1);
    ctx->dest_uri = g_strdup(dest_uri);
    ctx->drop_cb = drop_cb;
    ctx->user_data = user_data;
    g_object_set_data_full(G_OBJECT(target_widget), "kajo-uri-dnd-ctx", ctx, uri_dnd_ctx_free);

    GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(dt, "enter", G_CALLBACK(on_uri_drop_enter_motion), ctx);
    g_signal_connect(dt, "motion", G_CALLBACK(on_uri_drop_enter_motion), ctx);
    g_signal_connect(dt, "leave", G_CALLBACK(on_widget_drop_leave), ctx);
    g_signal_connect(dt, "drop", G_CALLBACK(on_uri_drop_perform), ctx);
    gtk_widget_add_controller(target_widget, GTK_EVENT_CONTROLLER(dt));
}
