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
    GtkWidget              *root;
    GtkWidget              *entry;
    GtkWidget              *listbox;
    KajoWebPaletteCallbacks cbs;
    gpointer                ud;
    GPtrArray              *all_items; /* PalItem* */
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
rebuild_rows(PalCtx *ctx, const gchar *filter)
{
    GtkWidget *child = gtk_widget_get_first_child(ctx->listbox);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(ctx->listbox), child);
        child = next;
    }

    g_autofree gchar *fq = (filter && *filter) ? g_utf8_strdown(filter, -1) : NULL;
    guint shown = 0;

    for (guint i = 0; i < ctx->all_items->len && shown < 24; i++) {
        PalItem *it = g_ptr_array_index(ctx->all_items, i);
        if (fq) {
            g_autofree gchar *ll = g_utf8_strdown(it->label, -1);
            g_autofree gchar *ls = g_utf8_strdown(it->sub, -1);
            if (!strstr(ll, fq) && !strstr(ls, fq))
                continue;
        }
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(row, "web-palette-row");

        GtkWidget *tag = gtk_label_new(it->tag);
        gtk_widget_set_valign(tag, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(tag, "shell-popover-badge");
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
        g_object_set_data(G_OBJECT(lrow), "pal-item", it);
        gtk_list_box_append(GTK_LIST_BOX(ctx->listbox), lrow);
        if (shown == 0)
            gtk_list_box_select_row(GTK_LIST_BOX(ctx->listbox), GTK_LIST_BOX_ROW(lrow));
        shown++;
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

static gboolean
on_pal_key(GtkEventControllerKey *c G_GNUC_UNUSED, guint keyval, guint kc G_GNUC_UNUSED,
           GdkModifierType st G_GNUC_UNUSED, gpointer ud)
{
    PalCtx *ctx = ud;
    if (keyval == GDK_KEY_Escape) {
        gtk_widget_set_visible(ctx->root, FALSE);
        return TRUE;
    }
    return FALSE;
}

GtkWidget *
web_palette_new(const KajoWebPaletteCallbacks *cbs, gpointer user_data)
{
    PalCtx *ctx = g_new0(PalCtx, 1);
    if (cbs)
        ctx->cbs = *cbs;
    ctx->ud = user_data;
    ctx->all_items = g_ptr_array_new_with_free_func(pal_item_free);

    ctx->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(ctx->root, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(ctx->root, GTK_ALIGN_START);
    gtk_widget_set_margin_top(ctx->root, 54);
    gtk_widget_set_size_request(ctx->root, 580, -1);
    gtk_widget_add_css_class(ctx->root, "web-palette-card");
    gtk_widget_set_visible(ctx->root, FALSE);
    g_object_set_data_full(G_OBJECT(ctx->root), "pal-ctx", ctx, pal_ctx_free);

    ctx->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->entry), "⌘ Jump to tab, bookmark, history, or command...");
    gtk_widget_add_css_class(ctx->entry, "web-palette-entry");
    g_signal_connect(ctx->entry, "changed", G_CALLBACK(on_entry_changed), ctx);
    g_signal_connect(ctx->entry, "activate", G_CALLBACK(on_entry_activate), ctx);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_pal_key), ctx);
    gtk_widget_add_controller(ctx->entry, kc);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 300);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);

    ctx->listbox = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(ctx->listbox), TRUE);
    g_signal_connect(ctx->listbox, "row-activated", G_CALLBACK(on_row_activated), ctx);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ctx->listbox);

    gtk_box_append(GTK_BOX(ctx->root), ctx->entry);
    gtk_box_append(GTK_BOX(ctx->root), scroll);
    return ctx->root;
}

void
web_palette_open(GtkWidget *palette, const KajoWebModel *model, KajoWebStorage *storage)
{
    PalCtx *ctx = palette ? g_object_get_data(G_OBJECT(palette), "pal-ctx") : NULL;
    if (!ctx)
        return;

    g_ptr_array_set_size(ctx->all_items, 0);

    if (model) {
        for (guint i = 0; i < model->tabs->len; i++) {
            const KajoWebTabMeta *t = g_ptr_array_index(model->tabs, i);
            add_item(ctx, PAL_KIND_TAB, "TAB", t->title ? t->title : t->uri, t->uri, t->id, NULL);
        }
    }

    add_item(ctx, PAL_KIND_CMD, "CMD", "Toggle Vertical Tab Rail (Ctrl+B)", "rail", 0, "toggle-rail");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Toggle Zen Top Chrome (Ctrl+Shift+U)", "zen", 0, "toggle-zen");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Open Kajo Settings → Kajo Web (Ctrl+,)", "settings", 0, "open-settings");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Open Profile Picker", "profiles", 0, "open-picker");
    add_item(ctx, PAL_KIND_CMD, "CMD", "Toggle WebKit Web Inspector (F12)", "devtools", 0, "toggle-inspector");
    add_item(ctx, PAL_KIND_URI, "INT", "GPU & DMA-BUF Telemetry", "kajo://gpu", 0, "kajo://gpu");
    add_item(ctx, PAL_KIND_URI, "INT", "History", "kajo://history", 0, "kajo://history");
    add_item(ctx, PAL_KIND_URI, "INT", "Bookmarks", "kajo://bookmarks", 0, "kajo://bookmarks");

    if (storage) {
        g_autoptr(GPtrArray) bms = web_storage_list_bookmarks(storage, FALSE);
        for (guint i = 0; bms && i < MIN(bms->len, 15); i++) {
            const KajoWebBookmark *b = g_ptr_array_index(bms, i);
            add_item(ctx, PAL_KIND_URI, "★ BM", b->title && *b->title ? b->title : b->uri, b->uri, 0, b->uri);
        }
        g_autoptr(GPtrArray) hist = web_storage_search_history(storage, NULL, 15);
        for (guint i = 0; hist && i < hist->len; i++) {
            const KajoWebHistoryEntry *h = g_ptr_array_index(hist, i);
            add_item(ctx, PAL_KIND_URI, "↺ HIST", h->title && *h->title ? h->title : h->uri, h->uri, 0, h->uri);
        }
    }

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
