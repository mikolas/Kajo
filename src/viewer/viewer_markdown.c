#include "viewer_markdown.h"
#include "../common/spawn_detached.h"

#include <string.h>

typedef struct {
    guint line_1based;
    gsize byte_offset;
    gsize byte_len;
} MdSearchMatch;

struct _ViewerMarkdownView {
    ViewerMdStateCb      state_cb;
    ViewerMdOpenTargetCb open_cb;
    gpointer             user_data;

    GtkWidget           *stack;
    IdeMarkdownView     *md_view;
    GtkWidget           *raw_scroll;
    GtkWidget           *raw_view;
    GtkTextTag          *tag_match;
    GtkTextTag          *tag_active;

    GtkCssProvider      *font_css;
    char                *scale_class;

    char                *doc_path;
    char                *raw_text;
    guint                total_lines;
    GArray              *headings;      /* IdeMarkdownHeading */

    ViewerMdWidthMode    width_mode;
    int                  font_pct;
    gboolean             show_raw;

    GFile               *monitored_file;
    GFileMonitor        *monitor;
    GCancellable        *load_cancel;
    guint                reload_timer_id;

    GArray              *matches;       /* MdSearchMatch */
    int                  match_idx;
    char                *search_query;
    gboolean             search_case;
};

static guint g_md_instance_seq = 0;

static void
notify_state(ViewerMarkdownView *view)
{
    if (view && view->state_cb)
        view->state_cb(view->user_data);
}

static void
apply_font_scale_css(ViewerMarkdownView *view)
{
    if (!view || !view->font_css)
        return;

    int pct = view->font_pct;
    int body_px = MAX(10, (13 * pct) / 100);
    int h1_px = MAX(14, (20 * pct) / 100);
    int h2_px = MAX(12, (15 * pct) / 100);
    int h3_px = MAX(11, (13 * pct) / 100);
    g_autofree char *css = g_strdup_printf(
        ".%s .ide-md-h1 { font-size: %dpx; } "
        ".%s .ide-md-h2 { font-size: %dpx; } "
        ".%s .ide-md-h3 { font-size: %dpx; } "
        ".%s textview { font-size: %dpx; } "
        ".%s .ide-markdown-pane label:not(.ide-md-h1):not(.ide-md-h2):not(.ide-md-h3):not(.ide-md-h4):not(.ide-md-h5):not(.shell-popover-badge):not(.ide-tree-header-title) { font-size: %dpx; }",
        view->scale_class, h1_px,
        view->scale_class, h2_px,
        view->scale_class, h3_px,
        view->scale_class, body_px,
        view->scale_class, body_px);
    gtk_css_provider_load_from_string(view->font_css, css);
}

static void
update_doc_width_margins(ViewerMarkdownView *view)
{
    if (!view || !view->md_view)
        return;

    GtkWidget *scroll = ide_markdown_view_get_scroll_window(view->md_view);
    GtkWidget *doc_box = ide_markdown_view_get_doc_box(view->md_view);
    if (!scroll || !doc_box)
        return;

    int vw = gtk_widget_get_width(scroll);
    if (vw <= 40)
        vw = 920;

    int target = vw;
    if (view->width_mode == VIEWER_MD_WIDTH_NARROW)
        target = 680;
    else if (view->width_mode == VIEWER_MD_WIDTH_COMFORT)
        target = 860;

    int margin = (view->width_mode == VIEWER_MD_WIDTH_FULL)
        ? 24
        : MAX(20, (vw - target) / 2);

    gtk_widget_set_margin_start(doc_box, margin);
    gtk_widget_set_margin_end(doc_box, margin);
    gtk_widget_set_margin_top(doc_box, 20);
    gtk_widget_set_margin_bottom(doc_box, 28);
}

static void
on_scroll_width_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    update_doc_width_margins((ViewerMarkdownView *)user_data);
}

static void
on_md_sw_map(GtkWidget *w G_GNUC_UNUSED, gpointer user_data)
{
    update_doc_width_margins((ViewerMarkdownView *)user_data);
}

static void
on_md_jump_cb(guint line_1based, gpointer user_data)
{
    viewer_markdown_view_scroll_to_line((ViewerMarkdownView *)user_data, line_1based);
}

static void
on_md_open_file_cb(const char *path,
                   guint       line_1based G_GNUC_UNUSED,
                   guint       col_1based G_GNUC_UNUSED,
                   gpointer    user_data)
{
    ViewerMarkdownView *view = user_data;
    if (view && view->open_cb && path && *path)
        view->open_cb(path, view->user_data);
}

static guint
count_lines(const char *txt)
{
    if (!txt || !*txt)
        return 1;
    guint n = 1;
    for (const char *p = txt; *p; p++) {
        if (*p == '\n')
            n++;
    }
    return n;
}

static void
refresh_raw_highlights(ViewerMarkdownView *view)
{
    if (!view || !view->raw_view)
        return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view->raw_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gtk_text_buffer_remove_tag(buf, view->tag_match, &start, &end);
    gtk_text_buffer_remove_tag(buf, view->tag_active, &start, &end);

    if (!view->matches || view->matches->len == 0)
        return;

    for (guint i = 0; i < view->matches->len; i++) {
        MdSearchMatch *m = &g_array_index(view->matches, MdSearchMatch, i);
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buf, &a, -1);
        /* Count char offset safely from UTF-8 bytes */
        g_autofree char *prefix = g_strndup(view->raw_text, m->byte_offset);
        glong char_off = g_utf8_strlen(prefix, -1);
        glong char_len = g_utf8_strlen(view->raw_text + m->byte_offset, (gssize)m->byte_len);
        gtk_text_buffer_get_iter_at_offset(buf, &a, (gint)char_off);
        gtk_text_buffer_get_iter_at_offset(buf, &b, (gint)(char_off + char_len));
        gtk_text_buffer_apply_tag(buf,
                                  ((int)i == view->match_idx) ? view->tag_active : view->tag_match,
                                  &a, &b);
        if ((int)i == view->match_idx && view->show_raw) {
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(view->raw_view), &a, 0.25, FALSE, 0.0, 0.0);
        }
    }
}

static void
set_markdown_content(ViewerMarkdownView *view,
                     const char         *text,
                     const char         *path,
                     double              initial_scroll_frac)
{
    g_free(view->raw_text);
    view->raw_text = g_strdup(text ? text : "");
    view->total_lines = count_lines(view->raw_text);

    g_free(view->doc_path);
    view->doc_path = g_strdup(path);

    if (view->headings)
        g_array_unref(view->headings);
    view->headings = ide_markdown_extract_headings(view->raw_text);

    g_autofree char *dir = (path && *path) ? g_path_get_dirname(path) : NULL;
    ide_markdown_view_set_context(view->md_view, path, dir);
    ide_markdown_view_render(view->md_view, view->raw_text);
    update_doc_width_margins(view);

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view->raw_view));
    gtk_text_buffer_set_text(buf, view->raw_text, -1);

    if (initial_scroll_frac > 0.001) {
        ide_markdown_view_sync_scroll(view->md_view, initial_scroll_frac);
    }
    if (view->search_query && *view->search_query) {
        viewer_markdown_view_set_search(view, view->search_query, view->search_case);
    } else {
        notify_state(view);
    }
}

static void
on_file_contents_loaded(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget *stack = GTK_WIDGET(source);
    (void)stack;
    g_autofree char *contents = NULL;
    gsize len = 0;
    g_autoptr(GError) err = NULL;

    /* Note: GFile is the async source object; view_ref is passed via GWeakRef in user_data */
    GWeakRef *ref = user_data;
    GtkWidget *w = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);

    if (!g_file_load_contents_finish(G_FILE(source), res, &contents, &len, NULL, &err)) {
        if (w)
            g_object_unref(w);
        return;
    }
    if (!w)
        return;

    ViewerMarkdownView *view = g_object_get_data(G_OBJECT(w), "viewer-md-self");
    if (view) {
        g_autofree char *path = g_file_get_path(G_FILE(source));
        double frac = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "viewer-md-init-frac")) / 10000.0;
        g_object_set_data(G_OBJECT(w), "viewer-md-init-frac", GINT_TO_POINTER(0));
        set_markdown_content(view, contents, path, frac);
    }
    g_object_unref(w);
}

static gboolean
on_reload_debounced(gpointer user_data)
{
    ViewerMarkdownView *view = user_data;
    view->reload_timer_id = 0;
    if (!view->monitored_file)
        return G_SOURCE_REMOVE;

    double cur_frac = 0.0;
    GtkWidget *sw = ide_markdown_view_get_scroll_window(view->md_view);
    if (sw) {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
        if (vadj) {
            double upper = gtk_adjustment_get_upper(vadj);
            double page = gtk_adjustment_get_page_size(vadj);
            if (upper > page)
                cur_frac = CLAMP(gtk_adjustment_get_value(vadj) / (upper - page), 0.0, 1.0);
        }
    }

    g_object_set_data(G_OBJECT(view->stack), "viewer-md-init-frac",
                      GINT_TO_POINTER((int)(cur_frac * 10000.0)));
    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, view->stack);
    g_file_load_contents_async(view->monitored_file, view->load_cancel, on_file_contents_loaded, ref);
    return G_SOURCE_REMOVE;
}

static void
on_file_changed(GFileMonitor      *monitor G_GNUC_UNUSED,
                GFile             *file G_GNUC_UNUSED,
                GFile             *other_file G_GNUC_UNUSED,
                GFileMonitorEvent  event_type,
                gpointer           user_data)
{
    ViewerMarkdownView *view = user_data;
    if (!view)
        return;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGED ||
        event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        event_type == G_FILE_MONITOR_EVENT_CREATED) {
        g_clear_handle_id(&view->reload_timer_id, g_source_remove);
        view->reload_timer_id = g_timeout_add(150, on_reload_debounced, view);
    }
}

ViewerMarkdownView *
viewer_markdown_view_new(ViewerMdStateCb      state_cb,
                         ViewerMdOpenTargetCb open_cb,
                         gpointer             user_data)
{
    ViewerMarkdownView *view = g_new0(ViewerMarkdownView, 1);
    view->state_cb = state_cb;
    view->open_cb = open_cb;
    view->user_data = user_data;
    view->width_mode = VIEWER_MD_WIDTH_COMFORT;
    view->font_pct = 100;
    view->matches = g_array_new(FALSE, TRUE, sizeof(MdSearchMatch));
    view->match_idx = -1;

    view->scale_class = g_strdup_printf("viewer-md-scale-%u", ++g_md_instance_seq);
    view->font_css = gtk_css_provider_new();
    GdkDisplay *disp = gdk_display_get_default();
    if (disp) {
        gtk_style_context_add_provider_for_display(
            disp, GTK_STYLE_PROVIDER(view->font_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    }

    view->stack = gtk_stack_new();
    gtk_widget_set_hexpand(view->stack, TRUE);
    gtk_widget_set_vexpand(view->stack, TRUE);
    gtk_widget_add_css_class(view->stack, view->scale_class);
    g_object_set_data(G_OBJECT(view->stack), "viewer-md-self", view);

    view->md_view = ide_markdown_view_new(on_md_jump_cb, NULL, view);
    ide_markdown_view_set_read_only(view->md_view, TRUE);
    ide_markdown_view_set_show_header_bar(view->md_view, FALSE);
    ide_markdown_view_set_open_file_cb(view->md_view, on_md_open_file_cb);

    GtkWidget *md_w = ide_markdown_view_get_widget(view->md_view);
    GtkWidget *md_sw = ide_markdown_view_get_scroll_window(view->md_view);
    if (md_sw) {
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(md_sw));
        if (hadj) {
            g_signal_connect(hadj, "notify::page-size", G_CALLBACK(on_scroll_width_changed), view);
        }
        g_signal_connect(md_sw, "map", G_CALLBACK(on_md_sw_map), view);
    }
    gtk_stack_add_named(GTK_STACK(view->stack), md_w, "rendered");

    view->raw_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view->raw_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    view->raw_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view->raw_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view->raw_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view->raw_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view->raw_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(view->raw_view, "viewer-md-raw");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view->raw_scroll), view->raw_view);

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view->raw_view));
    view->tag_match = gtk_text_buffer_create_tag(buf, "search-hl",
                                                 "background", "#ffb900",
                                                 "foreground", "#000000", NULL);
    view->tag_active = gtk_text_buffer_create_tag(buf, "search-active",
                                                  "background", "#0078d4",
                                                  "foreground", "#ffffff", NULL);

    gtk_stack_add_named(GTK_STACK(view->stack), view->raw_scroll, "raw");
    gtk_stack_set_visible_child_name(GTK_STACK(view->stack), "rendered");
    apply_font_scale_css(view);
    return view;
}

static void
clear_monitor_and_cancel(ViewerMarkdownView *view)
{
    g_clear_handle_id(&view->reload_timer_id, g_source_remove);
    if (view->monitor) {
        g_file_monitor_cancel(view->monitor);
        g_clear_object(&view->monitor);
    }
    g_clear_object(&view->monitored_file);
    if (view->load_cancel) {
        g_cancellable_cancel(view->load_cancel);
        g_clear_object(&view->load_cancel);
    }
}

void
viewer_markdown_view_free(ViewerMarkdownView *view)
{
    if (!view)
        return;
    if (view->stack) {
        g_object_set_data(G_OBJECT(view->stack), "viewer-md-self", NULL);
    }
    clear_monitor_and_cancel(view);
    GdkDisplay *disp = gdk_display_get_default();
    if (disp && view->font_css) {
        gtk_style_context_remove_provider_for_display(disp, GTK_STYLE_PROVIDER(view->font_css));
    }
    g_clear_object(&view->font_css);
    g_free(view->scale_class);
    g_free(view->doc_path);
    g_free(view->raw_text);
    g_free(view->search_query);
    if (view->headings)
        g_array_unref(view->headings);
    g_array_free(view->matches, TRUE);
    ide_markdown_view_free(view->md_view);
    g_free(view);
}

GtkWidget *
viewer_markdown_view_get_widget(ViewerMarkdownView *view)
{
    return view ? view->stack : NULL;
}

gboolean
viewer_markdown_view_load_file(ViewerMarkdownView *view,
                               GFile              *file,
                               double              initial_scroll_frac)
{
    if (!view || !file)
        return FALSE;

    clear_monitor_and_cancel(view);
    view->load_cancel = g_cancellable_new();
    view->monitored_file = g_object_ref(file);
    view->monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, view->load_cancel, NULL);
    if (view->monitor) {
        g_signal_connect(view->monitor, "changed", G_CALLBACK(on_file_changed), view);
    }

    g_object_set_data(G_OBJECT(view->stack), "viewer-md-init-frac",
                      GINT_TO_POINTER((int)(initial_scroll_frac * 10000.0)));
    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, view->stack);
    g_file_load_contents_async(file, view->load_cancel, on_file_contents_loaded, ref);
    return TRUE;
}

void
viewer_markdown_view_load_text(ViewerMarkdownView *view,
                               const char         *text,
                               const char         *virtual_path)
{
    if (!view)
        return;
    clear_monitor_and_cancel(view);
    set_markdown_content(view, text, virtual_path, 0.0);
}

void
viewer_markdown_view_set_width_mode(ViewerMarkdownView *view, ViewerMdWidthMode mode)
{
    if (!view)
        return;
    view->width_mode = mode;
    update_doc_width_margins(view);
    notify_state(view);
}

ViewerMdWidthMode
viewer_markdown_view_get_width_mode(const ViewerMarkdownView *view)
{
    return view ? view->width_mode : VIEWER_MD_WIDTH_COMFORT;
}

void
viewer_markdown_view_step_font(ViewerMarkdownView *view, int delta_pct)
{
    if (!view)
        return;
    view->font_pct = CLAMP(view->font_pct + delta_pct, 80, 180);
    apply_font_scale_css(view);
    notify_state(view);
}

void
viewer_markdown_view_reset_font(ViewerMarkdownView *view)
{
    if (!view)
        return;
    view->font_pct = 100;
    apply_font_scale_css(view);
    notify_state(view);
}

int
viewer_markdown_view_get_font_pct(const ViewerMarkdownView *view)
{
    return view ? view->font_pct : 100;
}

void
viewer_markdown_view_set_raw(ViewerMarkdownView *view, gboolean show_raw)
{
    if (!view)
        return;
    view->show_raw = show_raw;
    gtk_stack_set_visible_child_name(GTK_STACK(view->stack), show_raw ? "raw" : "rendered");
    refresh_raw_highlights(view);
    notify_state(view);
}

gboolean
viewer_markdown_view_get_raw(const ViewerMarkdownView *view)
{
    return view ? view->show_raw : FALSE;
}

void
viewer_markdown_view_set_search(ViewerMarkdownView *view,
                                const char         *query,
                                gboolean            case_sensitive)
{
    if (!view)
        return;
    g_array_set_size(view->matches, 0);
    g_free(view->search_query);
    view->search_query = (query && *query) ? g_strdup(query) : NULL;
    view->search_case = case_sensitive;
    view->match_idx = -1;

    if (view->search_query && view->raw_text && *view->raw_text) {
        size_t qlen = strlen(view->search_query);
        g_autofree char *hay_low = case_sensitive ? NULL : g_utf8_strdown(view->raw_text, -1);
        g_autofree char *q_low = case_sensitive ? NULL : g_utf8_strdown(view->search_query, -1);
        const char *hay = hay_low ? hay_low : view->raw_text;
        const char *needle = q_low ? q_low : view->search_query;
        size_t nlen = strlen(needle);
        if (nlen > 0) {
            const char *p = hay;
            guint line = 1;
            const char *line_scan = hay;
            while ((p = strstr(p, needle)) != NULL) {
                while (line_scan < p) {
                    if (*line_scan == '\n')
                        line++;
                    line_scan++;
                }
                MdSearchMatch m = {
                    .line_1based = line,
                    .byte_offset = (gsize)(p - hay),
                    .byte_len = qlen,
                };
                g_array_append_val(view->matches, m);
                p += nlen;
            }
        }
        if (view->matches->len > 0) {
            view->match_idx = 0;
            MdSearchMatch *first = &g_array_index(view->matches, MdSearchMatch, 0);
            if (!view->show_raw)
                viewer_markdown_view_scroll_to_line(view, first->line_1based);
        }
    }

    refresh_raw_highlights(view);
    notify_state(view);
}

void
viewer_markdown_view_step_search(ViewerMarkdownView *view, int delta)
{
    if (!view || !view->matches || view->matches->len == 0)
        return;
    int len = (int)view->matches->len;
    view->match_idx = (((view->match_idx + delta) % len) + len) % len;
    MdSearchMatch *m = &g_array_index(view->matches, MdSearchMatch, (guint)view->match_idx);
    if (!view->show_raw)
        viewer_markdown_view_scroll_to_line(view, m->line_1based);
    refresh_raw_highlights(view);
    notify_state(view);
}

int
viewer_markdown_view_get_search_total(const ViewerMarkdownView *view)
{
    return (view && view->matches) ? (int)view->matches->len : 0;
}

int
viewer_markdown_view_get_search_index(const ViewerMarkdownView *view)
{
    return view ? view->match_idx : -1;
}

void
viewer_markdown_view_scroll_to_line(ViewerMarkdownView *view, guint line_1based)
{
    if (!view)
        return;
    if (view->show_raw && view->raw_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view->raw_view));
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(buf, &it, (gint)(line_1based > 0 ? line_1based - 1 : 0));
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(view->raw_view), &it, 0.15, FALSE, 0.0, 0.0);
    } else {
        ide_markdown_view_scroll_to_line(view->md_view, line_1based, view->total_lines);
    }
}

GArray *
viewer_markdown_view_get_headings(const ViewerMarkdownView *view)
{
    return view ? view->headings : NULL;
}

guint
viewer_markdown_view_get_total_lines(const ViewerMarkdownView *view)
{
    return view ? view->total_lines : 1;
}

int
viewer_markdown_view_count_mermaid(const ViewerMarkdownView *view)
{
    if (!view || !view->raw_text)
        return 0;
    int cnt = 0;
    const char *p = view->raw_text;
    while ((p = strstr(p, "```mermaid")) != NULL) {
        cnt++;
        p += 10;
    }
    return cnt;
}

gboolean
viewer_markdown_view_has_monitor(const ViewerMarkdownView *view)
{
    return view && view->monitor != NULL;
}

void
viewer_markdown_view_open_in_ide(const ViewerMarkdownView *view)
{
    if (!view || !view->doc_path || !*view->doc_path)
        return;
    const char *argv[] = { "kajo-ide", view->doc_path, NULL };
    kajo_spawn_detached_argv(NULL, argv, NULL, NULL);
}