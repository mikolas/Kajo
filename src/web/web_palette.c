#include "web_palette.h"
#include <string.h>

typedef enum {
    PAL_KIND_TAB = 0,
    PAL_KIND_URI,
    PAL_KIND_CMD,
} PalKind;

typedef struct {
    PalKind  kind;
    gchar   *tag;
    gchar   *label;
    gchar   *sub;
    guint64  tab_id;
    gchar   *payload;
} PalItem;

typedef struct {
    GtkWidget              *root;     /* Full-viewport dim scrim */
    GtkWidget              *card;
    GtkWidget              *entry;
    GtkWidget              *scroll;
    GtkWidget              *listbox;
    KajoWebPaletteCallbacks cbs;
    gpointer                ud;
    GPtrArray              *all_items; /* PalItem* static/open items */
    GPtrArray              *dyn_items; /* PalItem* query-time SQLite/fallback items */
    const KajoWebModel     *model;
    KajoWebStorage         *storage;
} PalCtx;

static void
pal_item_free(gpointer ptr)
{
    PalItem *it = ptr;
    if (!it)
        return;
    g_free(it->tag);
    g_free(it->label);
    g_free(it->sub);
    g_free(it->payload);
    g_free(it);
}

static void
pal_ctx_free(gpointer ptr)
{
    PalCtx *ctx = ptr;
    if (!ctx)
        return;
    g_clear_pointer(&ctx->all_items, g_ptr_array_unref);
    g_clear_pointer(&ctx->dyn_items, g_ptr_array_unref);
    g_free(ctx);
}

static void
add_item(PalCtx *ctx, PalKind kind, const gchar *tag, const gchar *label,
         const gchar *sub, guint64 tab_id, const gchar *payload)
{
    PalItem *it = g_new0(PalItem, 1);
    it->kind = kind;
    it->tag = g_strdup(tag);
    it->label = g_strdup(label);
    it->sub = g_strdup(sub ? sub : "");
    it->tab_id = tab_id;
    it->payload = g_strdup(payload ? payload : "");
    g_ptr_array_add(ctx->all_items, it);
}

static void
activate_item(PalCtx *ctx, const PalItem *it)
{
    if (!it)
        return;
    gtk_widget_set_visible(ctx->root, FALSE);
    if (it->kind == PAL_KIND_TAB && ctx->cbs.on_select_tab)
        ctx->cbs.on_select_tab(it->tab_id, ctx->ud);
    else if (it->kind == PAL_KIND_URI && ctx->cbs.on_open_uri)
        ctx->cbs.on_open_uri(it->payload, ctx->ud);
    else if (it->kind == PAL_KIND_CMD && ctx->cbs.on_run_command)
        ctx->cbs.on_run_command(it->payload, ctx->ud);
}

static void
append_list_row(PalCtx *ctx, const PalItem *it, guint idx)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(row, "web-palette-row");

    GtkWidget *tag = gtk_label_new(it->tag);
    gtk_widget_set_valign(tag, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(tag, "shell-popover-badge");
    gtk_widget_add_css_class(tag, "web-palette-tag");

    GtkWidget *lbl = gtk_label_new(it->label);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);

    GtkWidget *sub = gtk_label_new(it->sub);
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(sub, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(sub, "web-dim-label");

    gtk_box_append(GTK_BOX(row), tag);
    gtk_box_append(GTK_BOX(row), lbl);
    gtk_box_append(GTK_BOX(row), sub);

    GtkWidget *lrow = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(lrow), row);
    g_object_set_data(G_OBJECT(lrow), "pal-item", (gpointer)it);
    gtk_list_box_append(GTK_LIST_BOX(ctx->listbox), lrow);
    if (idx == 0)
        gtk_list_box_select_row(GTK_LIST_BOX(ctx->listbox), GTK_LIST_BOX_ROW(lrow));
}

static gboolean
has_shown_uri(GPtrArray *seen, const gchar *uri)
{
    if (!uri || !*uri)
        return FALSE;
    for (guint i = 0; i < seen->len; i++) {
        if (g_strcmp0(g_ptr_array_index(seen, i), uri) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
rebuild_rows(PalCtx *ctx, const gchar *filter)
{
    GtkWidget *child = gtk_widget_get_first_child(ctx->listbox);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(ctx->listbox), child);
        child = next;
    }
    g_ptr_array_set_size(ctx->dyn_items, 0);

    g_autofree gchar *fq = (filter && *filter) ? g_utf8_strdown(filter, -1) : NULL;
    g_autoptr(GPtrArray) seen_uris = g_ptr_array_new_with_free_func(g_free);
    guint shown = 0;

    for (guint i = 0; i < ctx->all_items->len && shown < 14; i++) {
        PalItem *it = g_ptr_array_index(ctx->all_items, i);
        if (fq) {
            g_autofree gchar *ll = g_utf8_strdown(it->label, -1);
            g_autofree gchar *ls = g_utf8_strdown(it->sub, -1);
            if (!strstr(ll, fq) && !strstr(ls, fq))
                continue;
        }
        append_list_row(ctx, it, shown++);
        if (it->kind == PAL_KIND_URI && it->payload && *it->payload)
            g_ptr_array_add(seen_uris, g_strdup(it->payload));
    }

    if (ctx->storage) {
        g_autoptr(GPtrArray) bms = web_storage_list_bookmarks(ctx->storage, FALSE);
        for (guint i = 0; bms && i < bms->len && shown < 22; i++) {
            const KajoWebBookmark *b = g_ptr_array_index(bms, i);
            if (!b || !b->uri || has_shown_uri(seen_uris, b->uri))
                continue;
            const gchar *t = (b->title && *b->title) ? b->title : b->uri;
            if (fq) {
                g_autofree gchar *lt = g_utf8_strdown(t, -1);
                g_autofree gchar *lu = g_utf8_strdown(b->uri, -1);
                if (!strstr(lt, fq) && !strstr(lu, fq))
                    continue;
            }
            PalItem *it = g_new0(PalItem, 1);
            it->kind = PAL_KIND_URI;
            it->tag = g_strdup("★ BM");
            it->label = g_strdup(t);
            it->sub = g_strdup(b->uri);
            it->payload = g_strdup(b->uri);
            g_ptr_array_add(ctx->dyn_items, it);
            g_ptr_array_add(seen_uris, g_strdup(b->uri));
            append_list_row(ctx, it, shown++);
        }

        g_autoptr(GPtrArray) hist = web_storage_search_history(ctx->storage, filter, 12);
        for (guint i = 0; hist && i < hist->len && shown < 28; i++) {
            const KajoWebHistoryEntry *h = g_ptr_array_index(hist, i);
            if (!h || !h->uri || has_shown_uri(seen_uris, h->uri))
                continue;
            const gchar *t = (h->title && *h->title) ? h->title : h->uri;
            PalItem *it = g_new0(PalItem, 1);
            it->kind = PAL_KIND_URI;
            it->tag = g_strdup("↺ HIST");
            it->label = g_strdup(t);
            it->sub = g_strdup(h->uri);
            it->payload = g_strdup(h->uri);
            g_ptr_array_add(ctx->dyn_items, it);
            g_ptr_array_add(seen_uris, g_strdup(h->uri));
            append_list_row(ctx, it, shown++);
        }
    }

    if (shown == 0 && filter && *filter) {
        g_autofree gchar *trimmed = g_strdup(filter);
        g_strstrip(trimmed);
        if (*trimmed) {
            PalItem *it = g_new0(PalItem, 1);
            it->kind = PAL_KIND_URI;
            it->tag = g_strdup("GO");
            it->label = g_strdup_printf("Open or search: %s", trimmed);
            it->sub = g_strdup("Open URL or search engine");
            it->payload = g_strdup(trimmed);
            g_ptr_array_add(ctx->dyn_items, it);
            append_list_row(ctx, it, shown++);
        }
    }
}

static void
on_entry_changed(GtkEditable *ed, gpointer ud)
{
    PalCtx *ctx = ud;
    rebuild_rows(ctx, gtk_editable_get_text(ed));
}

static void
on_entry_activate(GtkEntry *ed G_GNUC_UNUSED, gpointer ud)
{
    PalCtx *ctx = ud;
    GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(ctx->listbox));
    if (sel)
        activate_item(ctx, g_object_get_data(G_OBJECT(sel), "pal-item"));
}

static void
on_row_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud)
{
    PalCtx *ctx = ud;
    if (row)
        activate_item(ctx, g_object_get_data(G_OBJECT(row), "pal-item"));
}

static void
move_selection(PalCtx *ctx, gint delta)
{
    GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(ctx->listbox));
    gint idx = cur ? gtk_list_box_row_get_index(cur) : 0;
    GtkListBoxRow *next = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ctx->listbox), MAX(0, idx + delta));
    if (next) {
        gtk_list_box_select_row(GTK_LIST_BOX(ctx->listbox), next);
        graphene_rect_t b;
        if (ctx->scroll && gtk_widget_compute_bounds(GTK_WIDGET(next), ctx->listbox, &b)) {
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ctx->scroll));
            if (vadj) {
                double val = gtk_adjustment_get_value(vadj);
                double page = gtk_adjustment_get_page_size(vadj);
                double top = b.origin.y;
                double bot = b.origin.y + b.size.height;
                if (top < val)
                    gtk_adjustment_set_value(vadj, top);
                else if (bot > val + page)
                    gtk_adjustment_set_value(vadj, bot - page);
            }
        }
    }
}

static gboolean
on_pal_key(GtkEventControllerKey *c G_GNUC_UNUSED, guint keyval, guint kc G_GNUC_UNUSED,
           GdkModifierType st G_GNUC_UNUSED, gpointer ud)
{
    PalCtx *ctx = ud;
    if (keyval == GDK_KEY_Escape) {
        gtk_widget_set_visible(ctx->root, FALSE);
        return TRUE;
    }
    if (keyval == GDK_KEY_Down) {
        move_selection(ctx, 1);
        return TRUE;
    }
    if (keyval == GDK_KEY_Up) {
        move_selection(ctx, -1);
        return TRUE;
    }
    return FALSE;
}

static void
on_scrim_clicked(GtkGestureClick *g G_GNUC_UNUSED, gint n G_GNUC_UNUSED, double x, double y, gpointer ud)
{
    PalCtx *ctx = ud;
    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(ctx->card, ctx->root, &bounds)) {
        graphene_point_t pt = GRAPHENE_POINT_INIT((float)x, (float)y);
        if (!graphene_rect_contains_point(&bounds, &pt))
            gtk_widget_set_visible(ctx->root, FALSE);
    }
}

GtkWidget *
web_palette_new(const KajoWebPaletteCallbacks *cbs, gpointer user_data)
{
    PalCtx *ctx = g_new0(PalCtx, 1);
    if (cbs)
        ctx->cbs = *cbs;
    ctx->ud = user_data;
    ctx->all_items = g_ptr_array_new_with_free_func(pal_item_free);
    ctx->dyn_items = g_ptr_array_new_with_free_func(pal_item_free);

    /* Full-viewport dim backdrop scrim matching mockup */
    ctx->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(ctx->root, TRUE);
    gtk_widget_set_vexpand(ctx->root, TRUE);
    gtk_widget_add_css_class(ctx->root, "web-palette-scrim");
    gtk_widget_set_visible(ctx->root, FALSE);
    g_object_set_data_full(G_OBJECT(ctx->root), "pal-ctx", ctx, pal_ctx_free);

    GtkGesture *scrim_click = gtk_gesture_click_new();
    g_signal_connect(scrim_click, "pressed", G_CALLBACK(on_scrim_clicked), ctx);
    gtk_widget_add_controller(ctx->root, GTK_EVENT_CONTROLLER(scrim_click));

    ctx->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(ctx->card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(ctx->card, GTK_ALIGN_START);
    gtk_widget_set_margin_top(ctx->card, 58);
    gtk_widget_set_size_request(ctx->card, 580, -1);
    gtk_widget_add_css_class(ctx->card, "web-palette-card");

    ctx->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->entry), "⌘ Jump to tab, bookmark, history, or command...");
    gtk_widget_add_css_class(ctx->entry, "web-palette-entry");
    g_signal_connect(ctx->entry, "changed", G_CALLBACK(on_entry_changed), ctx);
    g_signal_connect(ctx->entry, "activate", G_CALLBACK(on_entry_activate), ctx);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_pal_key), ctx);
    gtk_widget_add_controller(ctx->entry, kc);

    ctx->scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctx->scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(ctx->scroll), 320);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(ctx->scroll), TRUE);

    ctx->listbox = gtk_list_box_new();
    gtk_widget_add_css_class(ctx->listbox, "web-palette-list");
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(ctx->listbox), TRUE);
    g_signal_connect(ctx->listbox, "row-activated", G_CALLBACK(on_row_activated), ctx);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ctx->scroll), ctx->listbox);

    gtk_box_append(GTK_BOX(ctx->card), ctx->entry);
    gtk_box_append(GTK_BOX(ctx->card), ctx->scroll);
    gtk_box_append(GTK_BOX(ctx->root), ctx->card);
    return ctx->root;
}

void
web_palette_open(GtkWidget *palette, const KajoWebModel *model, KajoWebStorage *storage)
{
    PalCtx *ctx = palette ? g_object_get_data(G_OBJECT(palette), "pal-ctx") : NULL;
    if (!ctx)
        return;

    ctx->model = model;
    ctx->storage = storage;
    g_ptr_array_set_size(ctx->all_items, 0);

    if (model) {
        for (guint i = 0; i < model->tabs->len; i++) {
            const KajoWebTabMeta *t = g_ptr_array_index(model->tabs, i);
            add_item(ctx, PAL_KIND_TAB, "TAB", t->title ? t->title : t->uri, t->uri, t->id, NULL);
        }
    }

    add_item(ctx, PAL_KIND_CMD, "CMD", "Toggle Vertical Tab Rail (Ctrl+B)", "rail", 0, "toggle-rail");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Toggle Zen Top Chrome (Ctrl+Shift+U)", "zen", 0, "toggle-zen");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Toggle Fullscreen (F11)", "fullscreen", 0, "toggle-fullscreen");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Find in Page (Ctrl+F)", "find", 0, "open-find");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Print Page (Ctrl+P)", "print", 0, "print-page");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Open Kajo Settings → Kajo Web (Ctrl+,)", "settings", 0, "open-settings");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Open Profile Picker", "profiles", 0, "open-picker");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Toggle WebKit Web Inspector (F12)", "devtools", 0, "toggle-inspector");
    add_item(ctx, PAL_KIND_URI, "INT", "New Tab / Speed Dial", "kajo://newtab", 0, "kajo://newtab");
    add_item(ctx, PAL_KIND_URI, "INT", "GPU & DMA-BUF Telemetry", "kajo://gpu", 0, "kajo://gpu");
    add_item(ctx, PAL_KIND_URI, "INT", "History", "kajo://history", 0, "kajo://history");
    add_item(ctx, PAL_KIND_URI, "INT", "Bookmarks", "kajo://bookmarks", 0, "kajo://bookmarks");

    gtk_editable_set_text(GTK_EDITABLE(ctx->entry), "");
    rebuild_rows(ctx, "");
    gtk_widget_set_visible(ctx->root, TRUE);
    gtk_widget_grab_focus(ctx->entry);
}

void
web_palette_close(GtkWidget *palette)
{
    if (palette)
        gtk_widget_set_visible(palette, FALSE);
}

gboolean
web_palette_is_visible(GtkWidget *palette)
{
    return palette && gtk_widget_get_visible(palette);
}
