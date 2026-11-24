#include "web_omnibar_ac.h"
#include <string.h>

#define KAJO_WEB_OMNIBAR_AC_MAX 8

struct _KajoWebOmnibarAc {
    GtkEntry                *entry;
    GtkWidget               *popover;
    GtkWidget               *listbox;
    KajoWebStorage          *storage;
    const KajoWebModel      *model;
    KajoWebOmnibarAcSelectCb on_select;
    gpointer                 ud;
    guint                    debounce_id;
    gchar                   *pending_query;
    GPtrArray               *uris;       /* gchar* aligned with listbox rows */
    gint                     sel_idx;
};

static gboolean
ac_contains_uri(const KajoWebOmnibarAc *ac, const gchar *uri)
{
    for (guint i = 0; i < ac->uris->len; i++) {
        if (g_strcmp0(g_ptr_array_index(ac->uris, i), uri) == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
str_matches_ci(const gchar *haystack, const gchar *needle_lower)
{
    if (!needle_lower || !*needle_lower)
        return TRUE;
    if (!haystack || !*haystack)
        return FALSE;
    g_autofree gchar *low = g_utf8_strdown(haystack, -1);
    return strstr(low, needle_lower) != NULL;
}

static void
append_ac_row(KajoWebOmnibarAc *ac, const gchar *tag, const gchar *title, const gchar *uri)
{
    if (!uri || !*uri || ac->uris->len >= KAJO_WEB_OMNIBAR_AC_MAX || ac_contains_uri(ac, uri))
        return;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(box, "web-omnibar-ac-row");

    GtkWidget *badge = gtk_label_new(tag);
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(badge, "shell-popover-badge");
    gtk_widget_add_css_class(badge, "web-palette-tag");

    GtkWidget *lbl = gtk_label_new((title && *title) ? title : uri);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(lbl, TRUE);

    GtkWidget *sub = gtk_label_new(uri);
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(sub), 36);
    gtk_widget_set_valign(sub, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(sub, "web-dim-label");

    gtk_box_append(GTK_BOX(box), badge);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), sub);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_can_focus(row, FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(GTK_LIST_BOX(ac->listbox), row);
    g_ptr_array_add(ac->uris, g_strdup(uri));
}

static void
populate_ac(KajoWebOmnibarAc *ac, const gchar *query)
{
    GtkWidget *child = gtk_widget_get_first_child(ac->listbox);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(ac->listbox), child);
        child = next;
    }
    g_ptr_array_set_size(ac->uris, 0);
    ac->sel_idx = -1;

    g_autofree gchar *trimmed = g_strdup(query ? query : "");
    g_strstrip(trimmed);
    if (!*trimmed || !ac->entry || !gtk_widget_get_realized(GTK_WIDGET(ac->entry))) {
        web_omnibar_ac_dismiss(ac);
        return;
    }
    g_autofree gchar *qlow = g_utf8_strdown(trimmed, -1);

    if (ac->model) {
        guint n_tabs = 0;
        for (guint i = 0; i < ac->model->tabs->len && n_tabs < 2; i++) {
            const KajoWebTabMeta *t = g_ptr_array_index(ac->model->tabs, i);
            if (!t || !t->uri || !*t->uri)
                continue;
            if (str_matches_ci(t->title, qlow) || str_matches_ci(t->uri, qlow)) {
                append_ac_row(ac, "TAB", t->title, t->uri);
                n_tabs++;
            }
        }
    }

    if (ac->storage) {
        g_autoptr(GPtrArray) bms = web_storage_list_bookmarks(ac->storage, FALSE);
        guint n_bm = 0;
        for (guint i = 0; bms && i < bms->len && n_bm < 4; i++) {
            const KajoWebBookmark *b = g_ptr_array_index(bms, i);
            if (!b || !b->uri)
                continue;
            if (str_matches_ci(b->title, qlow) || str_matches_ci(b->uri, qlow)) {
                append_ac_row(ac, "★ BM", b->title, b->uri);
                n_bm++;
            }
        }

        g_autoptr(GPtrArray) hist = web_storage_search_history(ac->storage, trimmed, 6);
        for (guint i = 0; hist && i < hist->len; i++) {
            const KajoWebHistoryEntry *h = g_ptr_array_index(hist, i);
            if (h && h->uri)
                append_ac_row(ac, "↺", h->title, h->uri);
        }
    }

    static const struct {
        const gchar *title;
        const gchar *uri;
    } k_schemes[] = {
        { "New Tab / Speed Dial", "kajo://newtab" },
        { "Browsing History",     "kajo://history" },
        { "Bookmarks Manager",    "kajo://bookmarks" },
        { "GPU Telemetry",        "kajo://gpu" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(k_schemes); i++) {
        if (str_matches_ci(k_schemes[i].uri, qlow) || str_matches_ci(k_schemes[i].title, qlow))
            append_ac_row(ac, "INT", k_schemes[i].title, k_schemes[i].uri);
    }

    if (ac->uris->len == 0) {
        gtk_popover_popdown(GTK_POPOVER(ac->popover));
        return;
    }

    gtk_list_box_unselect_all(GTK_LIST_BOX(ac->listbox));
    gint ew = gtk_widget_get_width(GTK_WIDGET(ac->entry));
    gtk_widget_set_size_request(ac->popover, CLAMP(ew, 360, 740), -1);
    gtk_popover_popup(GTK_POPOVER(ac->popover));
}

static gboolean
on_debounce_fire(gpointer user_data)
{
    KajoWebOmnibarAc *ac = user_data;
    ac->debounce_id = 0;
    g_autofree gchar *q = g_steal_pointer(&ac->pending_query);
    populate_ac(ac, q);
    return G_SOURCE_REMOVE;
}

static void
on_row_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
    KajoWebOmnibarAc *ac = user_data;
    if (!row)
        return;
    gint idx = gtk_list_box_row_get_index(row);
    if (idx >= 0 && (guint)idx < ac->uris->len) {
        g_autofree gchar *uri = g_strdup(g_ptr_array_index(ac->uris, (guint)idx));
        web_omnibar_ac_dismiss(ac);
        if (ac->on_select)
            ac->on_select(uri, ac->ud);
    }
}

static void
on_entry_destroy(GtkWidget *entry G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebOmnibarAc *ac = user_data;
    if (ac->popover && gtk_widget_get_parent(ac->popover))
        gtk_widget_unparent(ac->popover);
    ac->popover = NULL;
    ac->entry = NULL;
}

KajoWebOmnibarAc *
web_omnibar_ac_new(GtkEntry                *entry,
                   KajoWebStorage          *storage,
                   const KajoWebModel      *model,
                   KajoWebOmnibarAcSelectCb on_select,
                   gpointer                 user_data)
{
    KajoWebOmnibarAc *ac = g_new0(KajoWebOmnibarAc, 1);
    ac->entry = entry;
    ac->storage = storage;
    ac->model = model;
    ac->on_select = on_select;
    ac->ud = user_data;
    ac->uris = g_ptr_array_new_with_free_func(g_free);
    ac->sel_idx = -1;

    ac->popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(ac->popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(ac->popover), GTK_POS_BOTTOM);
    gtk_popover_set_autohide(GTK_POPOVER(ac->popover), FALSE);
    gtk_widget_set_can_focus(ac->popover, FALSE);
    gtk_widget_set_focusable(ac->popover, FALSE);
    gtk_widget_add_css_class(ac->popover, "shell-popover");
    gtk_widget_add_css_class(ac->popover, "web-omnibar-ac");
    gtk_widget_set_parent(ac->popover, GTK_WIDGET(entry));

    ac->listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ac->listbox), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(ac->listbox), TRUE);
    gtk_widget_set_can_focus(ac->listbox, FALSE);
    gtk_widget_set_focusable(ac->listbox, FALSE);
    gtk_widget_add_css_class(ac->listbox, "web-palette-list");
    g_signal_connect(ac->listbox, "row-activated", G_CALLBACK(on_row_activated), ac);

    gtk_popover_set_child(GTK_POPOVER(ac->popover), ac->listbox);
    g_signal_connect(entry, "destroy", G_CALLBACK(on_entry_destroy), ac);
    return ac;
}

void
web_omnibar_ac_free(KajoWebOmnibarAc *ac)
{
    if (!ac)
        return;
    if (ac->debounce_id) {
        g_source_remove(ac->debounce_id);
        ac->debounce_id = 0;
    }
    g_clear_pointer(&ac->pending_query, g_free);
    if (ac->entry)
        g_signal_handlers_disconnect_by_data(ac->entry, ac);
    if (ac->popover && gtk_widget_get_parent(ac->popover))
        gtk_widget_unparent(ac->popover);
    ac->popover = NULL;
    ac->entry = NULL;
    g_clear_pointer(&ac->uris, g_ptr_array_unref);
    g_free(ac);
}

void
web_omnibar_ac_schedule(KajoWebOmnibarAc *ac, const gchar *query)
{
    if (!ac)
        return;
    if (ac->debounce_id) {
        g_source_remove(ac->debounce_id);
        ac->debounce_id = 0;
    }
    g_free(ac->pending_query);
    ac->pending_query = g_strdup(query ? query : "");
    if (!*ac->pending_query) {
        web_omnibar_ac_dismiss(ac);
        return;
    }
    ac->debounce_id = g_timeout_add(45, on_debounce_fire, ac);
}

void
web_omnibar_ac_dismiss(KajoWebOmnibarAc *ac)
{
    if (!ac)
        return;
    if (ac->debounce_id) {
        g_source_remove(ac->debounce_id);
        ac->debounce_id = 0;
    }
    g_clear_pointer(&ac->pending_query, g_free);
    ac->sel_idx = -1;
    if (ac->listbox)
        gtk_list_box_unselect_all(GTK_LIST_BOX(ac->listbox));
    if (ac->popover && gtk_widget_get_visible(ac->popover))
        gtk_popover_popdown(GTK_POPOVER(ac->popover));
}

gboolean
web_omnibar_ac_is_visible(const KajoWebOmnibarAc *ac)
{
    return ac && ac->popover && gtk_widget_get_visible(ac->popover);
}

gboolean
web_omnibar_ac_handle_key(KajoWebOmnibarAc *ac, guint keyval)
{
    if (!web_omnibar_ac_is_visible(ac))
        return FALSE;

    if (keyval == GDK_KEY_Escape) {
        web_omnibar_ac_dismiss(ac);
        return TRUE;
    }
    if (keyval == GDK_KEY_Down) {
        if (ac->uris->len == 0)
            return FALSE;
        ac->sel_idx = (ac->sel_idx + 1) % (gint)ac->uris->len;
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ac->listbox), ac->sel_idx);
        if (row)
            gtk_list_box_select_row(GTK_LIST_BOX(ac->listbox), row);
        return TRUE;
    }
    if (keyval == GDK_KEY_Up) {
        if (ac->sel_idx <= 0) {
            ac->sel_idx = -1;
            gtk_list_box_unselect_all(GTK_LIST_BOX(ac->listbox));
        } else {
            ac->sel_idx--;
            GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ac->listbox), ac->sel_idx);
            if (row)
                gtk_list_box_select_row(GTK_LIST_BOX(ac->listbox), row);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (ac->sel_idx >= 0 && (guint)ac->sel_idx < ac->uris->len) {
            g_autofree gchar *uri = g_strdup(g_ptr_array_index(ac->uris, (guint)ac->sel_idx));
            web_omnibar_ac_dismiss(ac);
            if (ac->on_select)
                ac->on_select(uri, ac->ud);
            return TRUE;
        }
        web_omnibar_ac_dismiss(ac);
        return FALSE;
    }
    return FALSE;
}
