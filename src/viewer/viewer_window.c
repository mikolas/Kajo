#define _DEFAULT_SOURCE
#include "viewer_window.h"
#include "viewer_model.h"
#include "viewer_sidebar.h"
#include "viewer_pdf.h"
#include "viewer_markdown.h"
#include "viewer_image.h"
#include "../compositor/compositor.h"

#include <cairo-pdf.h>
#include <string.h>

struct _KajoViewerWindow {
    GtkApplicationWindow parent_instance;

    ViewerDocKind        mode;
    ViewerHistory       *history;
    GFile               *current_file;
    char                *current_path;
    guint64              current_file_size;
    gboolean             sidebar_visible;
    gboolean             find_visible;
    gboolean             find_case;
    gboolean             navigating_history;
    gboolean             updating_ui;

    /* Chrome widgets */
    GtkWidget           *hdr_filename_lbl;
    GtkWidget           *hdr_badge_lbl;
    GtkWidget           *btn_back;
    GtkWidget           *btn_forward;
    GtkWidget           *btn_sidebar;

    /* Mode ribbons */
    GtkWidget           *ribbon_stack;
    GtkWidget           *pdf_page_entry;
    GtkWidget           *pdf_total_lbl;
    GtkWidget           *pdf_zoom_lbl;
    GtkWidget           *pdf_btn_fitw;
    GtkWidget           *pdf_btn_fitp;
    GtkWidget           *pdf_btn_100;
    GtkWidget           *pdf_btn_1p;
    GtkWidget           *pdf_btn_2p;
    GtkWidget           *pdf_rot_btn;
    GtkWidget           *pdf_btn_paper;
    GtkWidget           *pdf_btn_sepia;
    GtkWidget           *pdf_btn_dark;

    GtkWidget           *md_btn_narrow;
    GtkWidget           *md_btn_comfort;
    GtkWidget           *md_btn_full;
    GtkWidget           *md_font_lbl;
    GtkWidget           *md_raw_btn;

    GtkWidget           *img_idx_lbl;
    GtkWidget           *img_zoom_lbl;
    GtkWidget           *img_btn_fit;
    GtkWidget           *img_btn_100;
    GtkWidget           *img_btn_fill;
    GtkWidget           *img_btn_flip;
    GtkWidget           *img_btn_checker;
    GtkWidget           *img_btn_strip;
    GtkWidget           *img_btn_anim;

    /* Inline opt-in Find & Lock bars */
    GtkWidget           *find_bar;
    GtkWidget           *find_entry;
    GtkWidget           *find_case_btn;
    GtkWidget           *find_count_lbl;
    GtkWidget           *lock_bar;
    GtkWidget           *lock_entry;

    /* Workspace body */
    ViewerSidebar       *sidebar;
    GtkWidget           *content_stack;
    ViewerPdfView       *pdf_view;
    ViewerMarkdownView  *md_view;
    ViewerImageView     *img_view;

    /* Bottom telemetry strip */
    GtkWidget           *status_primary_lbl;
    GtkWidget           *status_secondary_lbl;
    GtkWidget           *status_path_lbl;
};

G_DEFINE_FINAL_TYPE(KajoViewerWindow, kajo_viewer_window, GTK_TYPE_APPLICATION_WINDOW)

static void open_file_internal(KajoViewerWindow *self, GFile *file, gboolean push_hist, int page_hint, double scroll_hint);
static void sync_all_ui(KajoViewerWindow *self);

static void
set_active_css(GtkWidget *w, gboolean active)
{
    if (!w)
        return;
    if (active)
        gtk_widget_add_css_class(w, "active");
    else
        gtk_widget_remove_css_class(w, "active");
}

static GtkWidget *
make_tool_btn(const char *label, GCallback cb, gpointer data)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "viewer-tool-btn");
    gtk_widget_set_can_focus(btn, FALSE);
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    if (cb)
        g_signal_connect(btn, "clicked", cb, data);
    return btn;
}

static GtkWidget *
make_seg_btn(const char *label, GCallback cb, gpointer data)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "viewer-seg-btn");
    gtk_widget_set_can_focus(btn, FALSE);
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    if (cb)
        g_signal_connect(btn, "clicked", cb, data);
    return btn;
}

static void
sync_all_ui(KajoViewerWindow *self)
{
    if (!self)
        return;
    self->updating_ui = TRUE;

    gtk_widget_set_sensitive(self->btn_back, viewer_history_can_back(self->history));
    gtk_widget_set_sensitive(self->btn_forward, viewer_history_can_forward(self->history));
    set_active_css(self->btn_sidebar, self->sidebar_visible);

    gboolean show_sb = self->sidebar_visible && self->mode != VIEWER_KIND_NONE;
    gtk_widget_set_visible(viewer_sidebar_get_widget(self->sidebar), show_sb);

    g_autofree char *base = self->current_path ? g_path_get_basename(self->current_path) : NULL;
    gtk_label_set_text(GTK_LABEL(self->hdr_filename_lbl), base ? base : "No document opened");
    gtk_label_set_text(GTK_LABEL(self->status_path_lbl), self->current_path ? self->current_path : "org.kajo.Viewer (Read-Only)");

    char sz[32];
    viewer_model_format_bytes(self->current_file_size, sz, sizeof(sz));

    if (self->mode == VIEWER_KIND_PDF) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "pdf");
        gtk_stack_set_visible_child_name(GTK_STACK(self->ribbon_stack), "pdf");
        viewer_sidebar_set_mode(self->sidebar, VIEWER_KIND_PDF);

        int cur = viewer_pdf_view_get_current_page(self->pdf_view);
        int tot = viewer_pdf_view_get_num_pages(self->pdf_view);
        viewer_sidebar_set_active_pdf_page(self->sidebar, cur);

        g_autofree char *badge = g_strdup_printf("PDF · %dP · %s", tot, sz);
        gtk_label_set_text(GTK_LABEL(self->hdr_badge_lbl), badge);

        g_autofree char *pstr = g_strdup_printf("%d", tot > 0 ? cur + 1 : 0);
        gtk_editable_set_text(GTK_EDITABLE(self->pdf_page_entry), pstr);
        g_autofree char *tstr = g_strdup_printf("/ %d", tot);
        gtk_label_set_text(GTK_LABEL(self->pdf_total_lbl), tstr);

        int zpct = (int)(viewer_pdf_view_get_zoom(self->pdf_view) * 100.0 + 0.5);
        g_autofree char *zstr = g_strdup_printf("%d%%", zpct);
        gtk_label_set_text(GTK_LABEL(self->pdf_zoom_lbl), zstr);

        ViewerFitMode fit = viewer_pdf_view_get_fit_mode(self->pdf_view);
        set_active_css(self->pdf_btn_fitw, fit == VIEWER_FIT_WIDTH);
        set_active_css(self->pdf_btn_fitp, fit == VIEWER_FIT_PAGE);
        set_active_css(self->pdf_btn_100, fit == VIEWER_FIT_ACTUAL);

        gboolean sp = viewer_pdf_view_get_spread(self->pdf_view);
        set_active_css(self->pdf_btn_1p, !sp);
        set_active_css(self->pdf_btn_2p, sp);

        g_autofree char *rstr = g_strdup_printf("↻ %d°", viewer_pdf_view_get_rotation(self->pdf_view));
        gtk_button_set_label(GTK_BUTTON(self->pdf_rot_btn), rstr);

        KajoPdfPaperTone tone = viewer_pdf_view_get_tone(self->pdf_view);
        set_active_css(self->pdf_btn_paper, tone == KAJO_PDF_TONE_PAPER);
        set_active_css(self->pdf_btn_sepia, tone == KAJO_PDF_TONE_SEPIA);
        set_active_css(self->pdf_btn_dark, tone == KAJO_PDF_TONE_DARK);

        g_autofree char *s1 = g_strdup_printf("PAGE %d / %d", tot > 0 ? cur + 1 : 0, tot);
        g_autofree char *s2 = g_strdup_printf("ZOOM %d%% · %s · READ-ONLY",
                                              zpct, sp ? "2P SPREAD" : "1P CONTINUOUS");
        gtk_label_set_text(GTK_LABEL(self->status_primary_lbl), s1);
        gtk_label_set_text(GTK_LABEL(self->status_secondary_lbl), s2);

        int ftot = viewer_pdf_view_get_search_total(self->pdf_view);
        int fidx = viewer_pdf_view_get_search_index(self->pdf_view);
        g_autofree char *fstr = g_strdup_printf("%d / %d", ftot > 0 ? fidx + 1 : 0, ftot);
        gtk_label_set_text(GTK_LABEL(self->find_count_lbl), fstr);
    } else if (self->mode == VIEWER_KIND_MARKDOWN) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "markdown");
        gtk_stack_set_visible_child_name(GTK_STACK(self->ribbon_stack), "markdown");
        viewer_sidebar_set_mode(self->sidebar, VIEWER_KIND_MARKDOWN);

        gboolean mon = viewer_markdown_view_has_monitor(self->md_view);
        g_autofree char *badge = g_strdup_printf("MD · %s%s", sz, mon ? " · LIVE" : "");
        gtk_label_set_text(GTK_LABEL(self->hdr_badge_lbl), badge);

        ViewerMdWidthMode wm = viewer_markdown_view_get_width_mode(self->md_view);
        set_active_css(self->md_btn_narrow, wm == VIEWER_MD_WIDTH_NARROW);
        set_active_css(self->md_btn_comfort, wm == VIEWER_MD_WIDTH_COMFORT);
        set_active_css(self->md_btn_full, wm == VIEWER_MD_WIDTH_FULL);

        int fpct = viewer_markdown_view_get_font_pct(self->md_view);
        g_autofree char *fstr = g_strdup_printf("%d%%", fpct);
        gtk_label_set_text(GTK_LABEL(self->md_font_lbl), fstr);

        gboolean raw = viewer_markdown_view_get_raw(self->md_view);
        set_active_css(self->md_raw_btn, raw);
        gtk_button_set_label(GTK_BUTTON(self->md_raw_btn), raw ? "RENDERED" : "RAW SOURCE");

        viewer_sidebar_set_markdown_headings(
            self->sidebar,
            viewer_markdown_view_get_headings(self->md_view),
            viewer_markdown_view_get_total_lines(self->md_view),
            mon,
            viewer_markdown_view_count_mermaid(self->md_view),
            0, 0);

        gtk_label_set_text(GTK_LABEL(self->status_primary_lbl), "MARKDOWN + MERMAID");
        g_autofree char *s2 = g_strdup_printf("FONT %d%% · %s · READ-ONLY",
                                              fpct, raw ? "RAW SOURCE" : "RENDERED");
        gtk_label_set_text(GTK_LABEL(self->status_secondary_lbl), s2);

        int ftot = viewer_markdown_view_get_search_total(self->md_view);
        int fidx = viewer_markdown_view_get_search_index(self->md_view);
        g_autofree char *cstr = g_strdup_printf("%d / %d", ftot > 0 ? fidx + 1 : 0, ftot);
        gtk_label_set_text(GTK_LABEL(self->find_count_lbl), cstr);
    } else if (viewer_model_is_image_kind(self->mode)) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "image");
        gtk_stack_set_visible_child_name(GTK_STACK(self->ribbon_stack), "image");
        viewer_sidebar_set_mode(self->sidebar, self->mode);

        const ViewerImageMeta *m = viewer_image_view_get_meta(self->img_view);
        viewer_sidebar_set_image_meta(self->sidebar, m);
        if (m->file_size > 0)
            self->current_file_size = m->file_size;

        viewer_model_format_bytes(self->current_file_size, sz, sizeof(sz));
        g_autofree char *badge = g_strdup_printf("%s · %dx%d · %s",
                                                 m->decoder[0] ? m->decoder : "IMAGE",
                                                 m->width, m->height, sz);
        gtk_label_set_text(GTK_LABEL(self->hdr_badge_lbl), badge);

        int sib_cnt = viewer_image_view_get_sibling_count(self->img_view);
        int sib_idx = viewer_image_view_get_sibling_index(self->img_view);
        g_autofree char *istr = g_strdup_printf("%d / %d",
                                                sib_cnt > 0 ? sib_idx + 1 : 1,
                                                MAX(1, sib_cnt));
        gtk_label_set_text(GTK_LABEL(self->img_idx_lbl), istr);

        int zpct = (int)(viewer_image_view_get_zoom(self->img_view) * 100.0 + 0.5);
        g_autofree char *zstr = g_strdup_printf("%d%%", zpct);
        gtk_label_set_text(GTK_LABEL(self->img_zoom_lbl), zstr);

        ViewerFitMode fit = viewer_image_view_get_fit_mode(self->img_view);
        set_active_css(self->img_btn_fit, fit == VIEWER_FIT_CONTAIN);
        set_active_css(self->img_btn_100, fit == VIEWER_FIT_ACTUAL);
        set_active_css(self->img_btn_fill, fit == VIEWER_FIT_FILL);
        set_active_css(self->img_btn_flip, m->flip_h);
        set_active_css(self->img_btn_checker, viewer_image_view_get_checkerboard(self->img_view));
        set_active_css(self->img_btn_strip, viewer_image_view_get_filmstrip(self->img_view));
        gtk_widget_set_visible(self->img_btn_anim, m->is_animated);
        if (m->is_animated) {
            gtk_button_set_label(GTK_BUTTON(self->img_btn_anim), m->anim_playing ? "⏸ PAUSE" : "▶ PLAY");
        }

        const char *ipath = viewer_image_view_get_current_path(self->img_view);
        if (ipath && g_strcmp0(ipath, self->current_path) != 0) {
            g_free(self->current_path);
            self->current_path = g_strdup(ipath);
            g_autofree char *nb = g_path_get_basename(ipath);
            gtk_label_set_text(GTK_LABEL(self->hdr_filename_lbl), nb);
            gtk_label_set_text(GTK_LABEL(self->status_path_lbl), ipath);
        }

        g_autofree char *s1 = g_strdup_printf("%d × %d px", m->width, m->height);
        g_autofree char *s2 = g_strdup_printf("ZOOM %d%% · ROT %d° · READ-ONLY", zpct, m->rotation_deg);
        gtk_label_set_text(GTK_LABEL(self->status_primary_lbl), s1);
        gtk_label_set_text(GTK_LABEL(self->status_secondary_lbl), s2);
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "empty");
        gtk_stack_set_visible_child_name(GTK_STACK(self->ribbon_stack), "empty");
        gtk_label_set_text(GTK_LABEL(self->hdr_badge_lbl), "READY · READ-ONLY");
        gtk_label_set_text(GTK_LABEL(self->status_primary_lbl), "IDLE · 0.0% CPU");
        gtk_label_set_text(GTK_LABEL(self->status_secondary_lbl), "PDF / MARKDOWN / IMAGE");
    }

    self->updating_ui = FALSE;
}

static void
on_subview_state_changed(gpointer user_data)
{
    sync_all_ui(KAJO_VIEWER_WINDOW(user_data));
}

static void
on_pdf_sidebar_page_selected(int page_index, gpointer user_data)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(user_data);
    viewer_pdf_view_jump_to_page(self->pdf_view, page_index);
}

static void
on_md_sidebar_heading_selected(guint line_1based, guint total_lines G_GNUC_UNUSED, gpointer user_data)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(user_data);
    viewer_markdown_view_scroll_to_line(self->md_view, line_1based);
    viewer_sidebar_set_active_md_line(self->sidebar, line_1based);
}

static void
on_md_open_target(const char *path, gpointer user_data)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(user_data);
    if (!self || !path || !*path)
        return;
    g_autoptr(GFile) f = g_file_new_for_path(path);
    open_file_internal(self, f, TRUE, 0, 0.0);
}

static void
on_pdf_navigate_uri(const char *uri, gpointer user_data)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(user_data);
    (void)self;
    if (!uri || !*uri)
        return;
    g_app_info_launch_default_for_uri(uri, NULL, NULL);
}

static void
open_file_internal(KajoViewerWindow *self, GFile *file, gboolean push_hist, int page_hint, double scroll_hint)
{
    if (!self || !file)
        return;

    g_autofree char *path = g_file_get_path(file);
    g_autoptr(GFileInfo) info = g_file_query_info(
        file, G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    const char *mime = info ? g_file_info_get_content_type(info) : NULL;
    guint64 fsize = info ? (guint64)g_file_info_get_size(info) : 0;

    ViewerDocKind kind = viewer_model_classify_path(path, mime);
    if (kind == VIEWER_KIND_NONE) {
        g_autofree char *uri = g_file_get_uri(file);
        if (uri)
            g_app_info_launch_default_for_uri(uri, NULL, NULL);
        return;
    }

    if (push_hist && self->current_path) {
        viewer_history_update_current(
            self->history,
            self->mode == VIEWER_KIND_PDF ? viewer_pdf_view_get_current_page(self->pdf_view) : 0,
            0.0);
    }

    g_set_object(&self->current_file, file);
    g_free(self->current_path);
    self->current_path = path ? g_strdup(path) : g_file_get_uri(file);
    self->current_file_size = fsize;
    self->mode = kind;

    gtk_widget_set_visible(self->lock_bar, FALSE);
    gtk_widget_set_visible(self->find_bar, FALSE);
    self->find_visible = FALSE;

    if (push_hist && self->current_path) {
        viewer_history_push(self->history, self->current_path, page_hint, scroll_hint);
    }

    if (kind == VIEWER_KIND_PDF) {
        g_autoptr(GError) err = NULL;
        ViewerPdfLoadResult res = viewer_pdf_view_load_file(self->pdf_view, file, NULL, &err);
        if (res == VIEWER_PDF_LOAD_ENCRYPTED) {
            viewer_sidebar_set_pdf_doc(self->sidebar, NULL);
            gtk_widget_set_visible(self->lock_bar, TRUE);
            gtk_widget_grab_focus(self->lock_entry);
        } else if (res == VIEWER_PDF_LOAD_OK) {
            viewer_sidebar_set_pdf_doc(self->sidebar, viewer_pdf_view_get_document(self->pdf_view));
            if (page_hint > 0)
                viewer_pdf_view_jump_to_page(self->pdf_view, page_hint);
        }
    } else if (kind == VIEWER_KIND_MARKDOWN) {
        viewer_markdown_view_load_file(self->md_view, file, scroll_hint);
    } else if (viewer_model_is_image_kind(kind)) {
        viewer_image_view_load_file(self->img_view, file);
    }

    sync_all_ui(self);
}

/* --- Actions & Callbacks --- */

static void
on_open_dialog_finish(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    KajoViewerWindow *self = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);

    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &err);
    if (self && file) {
        kajo_viewer_window_open_file(self, file);
    }
    if (self)
        g_object_unref(self);
}

static void
on_open_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(user_data);
    g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Open Document or Image");

    g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Supported (PDF, Markdown, Images)");
    gtk_file_filter_add_mime_type(filter, "application/pdf");
    gtk_file_filter_add_mime_type(filter, "text/markdown");
    gtk_file_filter_add_mime_type(filter, "text/x-markdown");
    gtk_file_filter_add_mime_type(filter, "image/*");
    g_autoptr(GListStore) store = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(store, filter);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(store));

    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_open_dialog_finish, ref);
}

static void on_float_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d G_GNUC_UNUSED) { niri_toggle_window_floating(); }
static void on_close_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) { gtk_window_close(GTK_WINDOW(d)); }

static void
on_back_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(d);
    const ViewerHistoryEntry *e = viewer_history_back(self->history);
    if (e && e->path) {
        g_autoptr(GFile) f = g_file_new_for_path(e->path);
        open_file_internal(self, f, FALSE, e->page_index, e->scroll_frac);
    }
}

static void
on_forward_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(d);
    const ViewerHistoryEntry *e = viewer_history_forward(self->history);
    if (e && e->path) {
        g_autoptr(GFile) f = g_file_new_for_path(e->path);
        open_file_internal(self, f, FALSE, e->page_index, e->scroll_frac);
    }
}

static void
on_sidebar_toggle(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(d);
    self->sidebar_visible = !self->sidebar_visible;
    sync_all_ui(self);
}

static void
toggle_find_bar(KajoViewerWindow *self, gboolean show)
{
    if (self->mode != VIEWER_KIND_PDF && self->mode != VIEWER_KIND_MARKDOWN)
        show = FALSE;
    self->find_visible = show;
    gtk_widget_set_visible(self->find_bar, show);
    if (show) {
        gtk_widget_grab_focus(self->find_entry);
    } else {
        viewer_pdf_view_set_search(self->pdf_view, NULL, self->find_case);
        viewer_markdown_view_set_search(self->md_view, NULL, self->find_case);
    }
}

static void on_find_btn_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) { toggle_find_bar(KAJO_VIEWER_WINDOW(d), !KAJO_VIEWER_WINDOW(d)->find_visible); }
static void on_find_close_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) { toggle_find_bar(KAJO_VIEWER_WINDOW(d), FALSE); }

static void
run_active_search(KajoViewerWindow *self)
{
    const char *q = gtk_editable_get_text(GTK_EDITABLE(self->find_entry));
    if (self->mode == VIEWER_KIND_PDF) {
        viewer_pdf_view_set_search(self->pdf_view, q, self->find_case);
    } else if (self->mode == VIEWER_KIND_MARKDOWN) {
        viewer_markdown_view_set_search(self->md_view, q, self->find_case);
    }
}

static void on_find_changed(GtkEditable *ed G_GNUC_UNUSED, gpointer d) { run_active_search(KAJO_VIEWER_WINDOW(d)); }
static void on_find_prev(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    KajoViewerWindow *s = KAJO_VIEWER_WINDOW(d);
    if (s->mode == VIEWER_KIND_PDF) viewer_pdf_view_step_search(s->pdf_view, -1);
    else if (s->mode == VIEWER_KIND_MARKDOWN) viewer_markdown_view_step_search(s->md_view, -1);
}
static void on_find_next(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    KajoViewerWindow *s = KAJO_VIEWER_WINDOW(d);
    if (s->mode == VIEWER_KIND_PDF) viewer_pdf_view_step_search(s->pdf_view, 1);
    else if (s->mode == VIEWER_KIND_MARKDOWN) viewer_markdown_view_step_search(s->md_view, 1);
}
static void on_find_case_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    KajoViewerWindow *s = KAJO_VIEWER_WINDOW(d);
    s->find_case = !s->find_case;
    set_active_css(s->find_case_btn, s->find_case);
    run_active_search(s);
}

static void
on_unlock_pdf(GtkWidget *w G_GNUC_UNUSED, gpointer d)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(d);
    if (!self->current_file)
        return;
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(self->lock_entry));
    char *pw = g_strdup(txt ? txt : "");
    size_t pw_len = strlen(pw);
    gtk_editable_set_text(GTK_EDITABLE(self->lock_entry), "");
    if (GTK_IS_PASSWORD_ENTRY(self->lock_entry))
        gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(self->lock_entry), FALSE);

    g_autoptr(GError) err = NULL;
    ViewerPdfLoadResult res = viewer_pdf_view_load_file(self->pdf_view, self->current_file, pw, &err);
    explicit_bzero(pw, pw_len);
    g_free(pw);

    if (res == VIEWER_PDF_LOAD_OK) {
        gtk_widget_set_visible(self->lock_bar, FALSE);
        viewer_sidebar_set_pdf_doc(self->sidebar, viewer_pdf_view_get_document(self->pdf_view));
        sync_all_ui(self);
    } else {
        gtk_widget_grab_focus(self->lock_entry);
    }
}

/* PDF Ribbon handlers */
static void on_pdf_prev(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_step_page(KAJO_VIEWER_WINDOW(d)->pdf_view, -1); }
static void on_pdf_next(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_step_page(KAJO_VIEWER_WINDOW(d)->pdf_view, 1); }
static void on_pdf_page_activate(GtkEntry *e, gpointer d)
{
    const char *t = gtk_editable_get_text(GTK_EDITABLE(e));
    int n = (int)g_ascii_strtoll(t, NULL, 10);
    if (n > 0) viewer_pdf_view_jump_to_page(KAJO_VIEWER_WINDOW(d)->pdf_view, n - 1);
}
static void on_pdf_zout(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_step_zoom(KAJO_VIEWER_WINDOW(d)->pdf_view, -1); }
static void on_pdf_zin(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_step_zoom(KAJO_VIEWER_WINDOW(d)->pdf_view, 1); }
static void on_pdf_fitw(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_set_zoom_fit(KAJO_VIEWER_WINDOW(d)->pdf_view, VIEWER_FIT_WIDTH); }
static void on_pdf_fitp(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_set_zoom_fit(KAJO_VIEWER_WINDOW(d)->pdf_view, VIEWER_FIT_PAGE); }
static void on_pdf_100(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_set_zoom_fit(KAJO_VIEWER_WINDOW(d)->pdf_view, VIEWER_FIT_ACTUAL); }
static void on_pdf_1p(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_set_spread(KAJO_VIEWER_WINDOW(d)->pdf_view, FALSE); }
static void on_pdf_2p(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_set_spread(KAJO_VIEWER_WINDOW(d)->pdf_view, TRUE); }
static void on_pdf_rot(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_rotate(KAJO_VIEWER_WINDOW(d)->pdf_view, 90); }
static void on_pdf_paper(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_set_tone(KAJO_VIEWER_WINDOW(d)->pdf_view, KAJO_PDF_TONE_PAPER); }
static void on_pdf_sepia(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_set_tone(KAJO_VIEWER_WINDOW(d)->pdf_view, KAJO_PDF_TONE_SEPIA); }
static void on_pdf_dark(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_pdf_view_set_tone(KAJO_VIEWER_WINDOW(d)->pdf_view, KAJO_PDF_TONE_DARK); }

/* Markdown Ribbon handlers */
static void on_md_narrow(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_markdown_view_set_width_mode(KAJO_VIEWER_WINDOW(d)->md_view, VIEWER_MD_WIDTH_NARROW); }
static void on_md_comfort(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_markdown_view_set_width_mode(KAJO_VIEWER_WINDOW(d)->md_view, VIEWER_MD_WIDTH_COMFORT); }
static void on_md_full(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_markdown_view_set_width_mode(KAJO_VIEWER_WINDOW(d)->md_view, VIEWER_MD_WIDTH_FULL); }
static void on_md_font_down(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_markdown_view_step_font(KAJO_VIEWER_WINDOW(d)->md_view, -10); }
static void on_md_font_up(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_markdown_view_step_font(KAJO_VIEWER_WINDOW(d)->md_view, 10); }
static void on_md_raw(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    ViewerMarkdownView *m = KAJO_VIEWER_WINDOW(d)->md_view;
    viewer_markdown_view_set_raw(m, !viewer_markdown_view_get_raw(m));
}
static void on_md_open_ide(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_markdown_view_open_in_ide(KAJO_VIEWER_WINDOW(d)->md_view); }

/* Image Ribbon handlers */
static void on_img_prev(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_step_sibling(KAJO_VIEWER_WINDOW(d)->img_view, -1); }
static void on_img_next(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_step_sibling(KAJO_VIEWER_WINDOW(d)->img_view, 1); }
static void on_img_zout(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_step_zoom(KAJO_VIEWER_WINDOW(d)->img_view, -1); }
static void on_img_zin(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_step_zoom(KAJO_VIEWER_WINDOW(d)->img_view, 1); }
static void on_img_fit(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_set_fit_mode(KAJO_VIEWER_WINDOW(d)->img_view, VIEWER_FIT_CONTAIN); }
static void on_img_100(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_set_fit_mode(KAJO_VIEWER_WINDOW(d)->img_view, VIEWER_FIT_ACTUAL); }
static void on_img_fill(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_set_fit_mode(KAJO_VIEWER_WINDOW(d)->img_view, VIEWER_FIT_FILL); }
static void on_img_rot(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_rotate(KAJO_VIEWER_WINDOW(d)->img_view, 90); }
static void on_img_flip(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_flip(KAJO_VIEWER_WINDOW(d)->img_view, TRUE); }
static void on_img_checker(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_toggle_checkerboard(KAJO_VIEWER_WINDOW(d)->img_view); }
static void on_img_strip(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_toggle_filmstrip(KAJO_VIEWER_WINDOW(d)->img_view); }
static void on_img_anim(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_toggle_animation(KAJO_VIEWER_WINDOW(d)->img_view); }
static void on_img_copy(GtkButton *b G_GNUC_UNUSED, gpointer d) { viewer_image_view_copy_to_clipboard(KAJO_VIEWER_WINDOW(d)->img_view); }

static gboolean
on_drop_file(GtkDropTarget *target G_GNUC_UNUSED, const GValue *value, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer d)
{
    if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
        GFile *f = G_FILE(g_value_get_object(value));
        if (f) {
            kajo_viewer_window_open_file(KAJO_VIEWER_WINDOW(d), f);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
on_key_pressed(GtkEventControllerKey *ctrl G_GNUC_UNUSED,
               guint                  keyval,
               guint                  keycode G_GNUC_UNUSED,
               GdkModifierType        state,
               gpointer               user_data)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(user_data);
    gboolean has_ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean has_alt = (state & GDK_ALT_MASK) != 0;
    gboolean has_shift = (state & GDK_SHIFT_MASK) != 0;

    if (has_ctrl && (keyval == GDK_KEY_o || keyval == GDK_KEY_O)) { on_open_clicked(NULL, self); return TRUE; }
    if (has_ctrl && (keyval == GDK_KEY_b || keyval == GDK_KEY_B)) { on_sidebar_toggle(NULL, self); return TRUE; }
    if (has_ctrl && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) { toggle_find_bar(self, !self->find_visible); return TRUE; }
    if (has_ctrl && (keyval == GDK_KEY_q || keyval == GDK_KEY_Q)) { gtk_window_close(GTK_WINDOW(self)); return TRUE; }
    if (keyval == GDK_KEY_Escape && self->find_visible) { toggle_find_bar(self, FALSE); return TRUE; }
    if (keyval == GDK_KEY_F3) {
        if (has_shift) on_find_prev(NULL, self);
        else on_find_next(NULL, self);
        return TRUE;
    }
    if (has_alt && keyval == GDK_KEY_Left) { on_back_clicked(NULL, self); return TRUE; }
    if (has_alt && keyval == GDK_KEY_Right) { on_forward_clicked(NULL, self); return TRUE; }

    if (has_ctrl && (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal)) {
        if (self->mode == VIEWER_KIND_PDF) viewer_pdf_view_step_zoom(self->pdf_view, 1);
        else if (self->mode == VIEWER_KIND_MARKDOWN) viewer_markdown_view_step_font(self->md_view, 10);
        else if (viewer_model_is_image_kind(self->mode)) viewer_image_view_step_zoom(self->img_view, 1);
        return TRUE;
    }
    if (has_ctrl && keyval == GDK_KEY_minus) {
        if (self->mode == VIEWER_KIND_PDF) viewer_pdf_view_step_zoom(self->pdf_view, -1);
        else if (self->mode == VIEWER_KIND_MARKDOWN) viewer_markdown_view_step_font(self->md_view, -10);
        else if (viewer_model_is_image_kind(self->mode)) viewer_image_view_step_zoom(self->img_view, -1);
        return TRUE;
    }
    if (has_ctrl && keyval == GDK_KEY_0) {
        if (self->mode == VIEWER_KIND_PDF) viewer_pdf_view_set_zoom_fit(self->pdf_view, VIEWER_FIT_ACTUAL);
        else if (self->mode == VIEWER_KIND_MARKDOWN) viewer_markdown_view_reset_font(self->md_view);
        else if (viewer_model_is_image_kind(self->mode)) viewer_image_view_set_fit_mode(self->img_view, VIEWER_FIT_CONTAIN);
        return TRUE;
    }

    if (has_ctrl && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
        if (self->mode == VIEWER_KIND_PDF && viewer_pdf_view_copy_selection(self->pdf_view))
            return TRUE;
        if (viewer_model_is_image_kind(self->mode) && viewer_image_view_copy_to_clipboard(self->img_view))
            return TRUE;
    }

    if (!self->find_visible && !gtk_widget_is_visible(self->lock_bar) &&
        !gtk_widget_has_focus(self->pdf_page_entry)) {
        if (self->mode == VIEWER_KIND_PDF) {
            if (has_ctrl && (keyval == GDK_KEY_g || keyval == GDK_KEY_G)) { gtk_widget_grab_focus(self->pdf_page_entry); return TRUE; }
            if (keyval == GDK_KEY_1) { on_pdf_1p(NULL, self); return TRUE; }
            if (keyval == GDK_KEY_2) { on_pdf_2p(NULL, self); return TRUE; }
            if (keyval == GDK_KEY_w || keyval == GDK_KEY_W) { on_pdf_fitw(NULL, self); return TRUE; }
            if (keyval == GDK_KEY_p || keyval == GDK_KEY_P) { on_pdf_fitp(NULL, self); return TRUE; }
            if (keyval == GDK_KEY_r) { viewer_pdf_view_rotate(self->pdf_view, 90); return TRUE; }
            if (keyval == GDK_KEY_R) { viewer_pdf_view_rotate(self->pdf_view, -90); return TRUE; }
            if (keyval == GDK_KEY_d || keyval == GDK_KEY_D) {
                KajoPdfPaperTone next = (viewer_pdf_view_get_tone(self->pdf_view) + 1) % 3;
                viewer_pdf_view_set_tone(self->pdf_view, next);
                return TRUE;
            }
        } else if (self->mode == VIEWER_KIND_MARKDOWN) {
            if ((has_ctrl && (keyval == GDK_KEY_u || keyval == GDK_KEY_U)) || keyval == GDK_KEY_r) {
                on_md_raw(NULL, self);
                return TRUE;
            }
        } else if (viewer_model_is_image_kind(self->mode)) {
            if (keyval == GDK_KEY_Left) { on_img_prev(NULL, self); return TRUE; }
            if (keyval == GDK_KEY_Right) { on_img_next(NULL, self); return TRUE; }
            if (keyval == GDK_KEY_f || keyval == GDK_KEY_F) { on_img_strip(NULL, self); return TRUE; }
            if (keyval == GDK_KEY_r) { viewer_image_view_rotate(self->img_view, 90); return TRUE; }
            if (keyval == GDK_KEY_R) { viewer_image_view_rotate(self->img_view, -90); return TRUE; }
            if (keyval == GDK_KEY_h || keyval == GDK_KEY_H) { viewer_image_view_flip(self->img_view, TRUE); return TRUE; }
            if (keyval == GDK_KEY_v || keyval == GDK_KEY_V) { viewer_image_view_flip(self->img_view, FALSE); return TRUE; }
            if (keyval == GDK_KEY_t || keyval == GDK_KEY_T) { on_img_checker(NULL, self); return TRUE; }
            if (keyval == GDK_KEY_space) {
                const ViewerImageMeta *m = viewer_image_view_get_meta(self->img_view);
                if (m && m->is_animated) viewer_image_view_toggle_animation(self->img_view);
                else on_img_next(NULL, self);
                return TRUE;
            }
        }
    }

    return FALSE;
}

static void
kajo_viewer_window_dispose(GObject *obj)
{
    KajoViewerWindow *self = KAJO_VIEWER_WINDOW(obj);
    g_clear_pointer(&self->history, viewer_history_free);
    g_clear_pointer(&self->sidebar, viewer_sidebar_free);
    g_clear_pointer(&self->pdf_view, viewer_pdf_view_free);
    g_clear_pointer(&self->md_view, viewer_markdown_view_free);
    g_clear_pointer(&self->img_view, viewer_image_view_free);
    g_clear_object(&self->current_file);
    g_clear_pointer(&self->current_path, g_free);
    G_OBJECT_CLASS(kajo_viewer_window_parent_class)->dispose(obj);
}

static void
kajo_viewer_window_class_init(KajoViewerWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = kajo_viewer_window_dispose;
}

static GtkWidget *
make_sep(void)
{
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(s, "viewer-ribbon-sep");
    return s;
}

static void
kajo_viewer_window_init(KajoViewerWindow *self)
{
    self->sidebar_visible = TRUE;
    self->history = viewer_history_new();

    gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(self), 1160, 760);
    gtk_window_set_title(GTK_WINDOW(self), "Kajo Viewer");
    gtk_widget_add_css_class(GTK_WIDGET(self), "shell-settings-window");
    gtk_widget_add_css_class(GTK_WIDGET(self), "shell-viewer-window");

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* 1. Frameless Aero + Metro Header Bar */
    GtkWidget *handle = gtk_window_handle_new();
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hdr, "settings-header-bar");
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

    GtkWidget *brand = gtk_label_new("▤ KAJO VIEWER");
    gtk_widget_add_css_class(brand, "settings-header-title");
    gtk_widget_set_valign(brand, GTK_ALIGN_CENTER);

    self->hdr_filename_lbl = gtk_label_new("No document opened");
    gtk_widget_add_css_class(self->hdr_filename_lbl, "viewer-hdr-filename");
    gtk_widget_set_valign(self->hdr_filename_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(self->hdr_filename_lbl, TRUE);
    gtk_label_set_xalign(GTK_LABEL(self->hdr_filename_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(self->hdr_filename_lbl), PANGO_ELLIPSIZE_MIDDLE);

    self->hdr_badge_lbl = gtk_label_new("READY · READ-ONLY");
    gtk_widget_add_css_class(self->hdr_badge_lbl, "shell-popover-badge");
    gtk_widget_set_valign(self->hdr_badge_lbl, GTK_ALIGN_CENTER);

    GtkWidget *open_btn = make_tool_btn("📂 OPEN", G_CALLBACK(on_open_clicked), self);
    GtkWidget *float_btn = make_tool_btn("🗗 FLOAT / TILE", G_CALLBACK(on_float_clicked), self);
    GtkWidget *close_btn = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(close_btn, "settings-close-btn");
    gtk_widget_set_can_focus(close_btn, FALSE);
    gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), self);

    gtk_box_append(GTK_BOX(hdr), brand);
    gtk_box_append(GTK_BOX(hdr), self->hdr_filename_lbl);
    gtk_box_append(GTK_BOX(hdr), self->hdr_badge_lbl);
    gtk_box_append(GTK_BOX(hdr), open_btn);
    gtk_box_append(GTK_BOX(hdr), float_btn);
    gtk_box_append(GTK_BOX(hdr), close_btn);
    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), hdr);
    gtk_box_append(GTK_BOX(root), handle);

    /* 2. Contextual Mode Ribbon */
    GtkWidget *ribbon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(ribbon, "viewer-mode-ribbon");
    gtk_widget_set_valign(ribbon, GTK_ALIGN_CENTER);

    self->btn_back = make_tool_btn("◀", G_CALLBACK(on_back_clicked), self);
    self->btn_forward = make_tool_btn("▶", G_CALLBACK(on_forward_clicked), self);
    self->btn_sidebar = make_tool_btn("☰ SIDEBAR", G_CALLBACK(on_sidebar_toggle), self);
    gtk_box_append(GTK_BOX(ribbon), self->btn_back);
    gtk_box_append(GTK_BOX(ribbon), self->btn_forward);
    gtk_box_append(GTK_BOX(ribbon), self->btn_sidebar);
    gtk_box_append(GTK_BOX(ribbon), make_sep());

    self->ribbon_stack = gtk_stack_new();
    gtk_widget_set_hexpand(self->ribbon_stack, TRUE);

    GtkWidget *empty_r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *el = gtk_label_new("Drop a PDF, Markdown, or Image file or click 📂 OPEN (Ctrl+O)");
    gtk_widget_add_css_class(el, "viewer-empty-hint");
    gtk_widget_set_valign(el, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(empty_r), el);
    gtk_stack_add_named(GTK_STACK(self->ribbon_stack), empty_r, "empty");

    /* PDF ribbon */
    GtkWidget *pdf_r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(pdf_r), make_tool_btn("◀ PREV", G_CALLBACK(on_pdf_prev), self));
    self->pdf_page_entry = gtk_entry_new();
    gtk_widget_add_css_class(self->pdf_page_entry, "viewer-page-input");
    gtk_widget_set_size_request(self->pdf_page_entry, 44, 24);
    gtk_entry_set_alignment(GTK_ENTRY(self->pdf_page_entry), 0.5f);
    g_signal_connect(self->pdf_page_entry, "activate", G_CALLBACK(on_pdf_page_activate), self);
    gtk_box_append(GTK_BOX(pdf_r), self->pdf_page_entry);
    self->pdf_total_lbl = gtk_label_new("/ 0");
    gtk_box_append(GTK_BOX(pdf_r), self->pdf_total_lbl);
    gtk_box_append(GTK_BOX(pdf_r), make_tool_btn("NEXT ▶", G_CALLBACK(on_pdf_next), self));
    gtk_box_append(GTK_BOX(pdf_r), make_sep());
    gtk_box_append(GTK_BOX(pdf_r), make_tool_btn("−", G_CALLBACK(on_pdf_zout), self));
    self->pdf_zoom_lbl = gtk_label_new("100%");
    gtk_widget_set_size_request(self->pdf_zoom_lbl, 44, -1);
    gtk_box_append(GTK_BOX(pdf_r), self->pdf_zoom_lbl);
    gtk_box_append(GTK_BOX(pdf_r), make_tool_btn("+", G_CALLBACK(on_pdf_zin), self));

    GtkWidget *p_seg_z = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(p_seg_z, "viewer-seg-group");
    self->pdf_btn_fitw = make_seg_btn("FIT W", G_CALLBACK(on_pdf_fitw), self);
    self->pdf_btn_fitp = make_seg_btn("FIT P", G_CALLBACK(on_pdf_fitp), self);
    self->pdf_btn_100 = make_seg_btn("100%", G_CALLBACK(on_pdf_100), self);
    gtk_box_append(GTK_BOX(p_seg_z), self->pdf_btn_fitw);
    gtk_box_append(GTK_BOX(p_seg_z), self->pdf_btn_fitp);
    gtk_box_append(GTK_BOX(p_seg_z), self->pdf_btn_100);
    gtk_box_append(GTK_BOX(pdf_r), p_seg_z);

    GtkWidget *pdf_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(pdf_spacer, TRUE);
    gtk_box_append(GTK_BOX(pdf_r), pdf_spacer);

    GtkWidget *p_seg_sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(p_seg_sp, "viewer-seg-group");
    self->pdf_btn_1p = make_seg_btn("1P", G_CALLBACK(on_pdf_1p), self);
    self->pdf_btn_2p = make_seg_btn("2P SPREAD", G_CALLBACK(on_pdf_2p), self);
    gtk_box_append(GTK_BOX(p_seg_sp), self->pdf_btn_1p);
    gtk_box_append(GTK_BOX(p_seg_sp), self->pdf_btn_2p);
    gtk_box_append(GTK_BOX(pdf_r), p_seg_sp);

    self->pdf_rot_btn = make_tool_btn("↻ 0°", G_CALLBACK(on_pdf_rot), self);
    gtk_box_append(GTK_BOX(pdf_r), self->pdf_rot_btn);

    GtkWidget *p_seg_t = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(p_seg_t, "viewer-seg-group");
    self->pdf_btn_paper = make_seg_btn("PAPER", G_CALLBACK(on_pdf_paper), self);
    self->pdf_btn_sepia = make_seg_btn("SEPIA", G_CALLBACK(on_pdf_sepia), self);
    self->pdf_btn_dark = make_seg_btn("DARK", G_CALLBACK(on_pdf_dark), self);
    gtk_box_append(GTK_BOX(p_seg_t), self->pdf_btn_paper);
    gtk_box_append(GTK_BOX(p_seg_t), self->pdf_btn_sepia);
    gtk_box_append(GTK_BOX(p_seg_t), self->pdf_btn_dark);
    gtk_box_append(GTK_BOX(pdf_r), p_seg_t);
    gtk_box_append(GTK_BOX(pdf_r), make_tool_btn("🔍 FIND", G_CALLBACK(on_find_btn_clicked), self));
    gtk_stack_add_named(GTK_STACK(self->ribbon_stack), pdf_r, "pdf");

    /* Markdown ribbon */
    GtkWidget *md_r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *m_seg_w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(m_seg_w, "viewer-seg-group");
    self->md_btn_narrow = make_seg_btn("NARROW", G_CALLBACK(on_md_narrow), self);
    self->md_btn_comfort = make_seg_btn("COMFORT", G_CALLBACK(on_md_comfort), self);
    self->md_btn_full = make_seg_btn("FULL", G_CALLBACK(on_md_full), self);
    gtk_box_append(GTK_BOX(m_seg_w), self->md_btn_narrow);
    gtk_box_append(GTK_BOX(m_seg_w), self->md_btn_comfort);
    gtk_box_append(GTK_BOX(m_seg_w), self->md_btn_full);
    gtk_box_append(GTK_BOX(md_r), m_seg_w);
    gtk_box_append(GTK_BOX(md_r), make_tool_btn("A-", G_CALLBACK(on_md_font_down), self));
    self->md_font_lbl = gtk_label_new("100%");
    gtk_widget_set_size_request(self->md_font_lbl, 40, -1);
    gtk_box_append(GTK_BOX(md_r), self->md_font_lbl);
    gtk_box_append(GTK_BOX(md_r), make_tool_btn("A+", G_CALLBACK(on_md_font_up), self));

    GtkWidget *md_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(md_spacer, TRUE);
    gtk_box_append(GTK_BOX(md_r), md_spacer);

    self->md_raw_btn = make_tool_btn("RAW SOURCE", G_CALLBACK(on_md_raw), self);
    gtk_box_append(GTK_BOX(md_r), self->md_raw_btn);
    GtkWidget *ro_badge = gtk_label_new("🔒 READ-ONLY");
    gtk_widget_add_css_class(ro_badge, "shell-popover-badge");
    gtk_widget_set_valign(ro_badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(md_r), ro_badge);
    gtk_box_append(GTK_BOX(md_r), make_tool_btn("✎ OPEN IN IDE", G_CALLBACK(on_md_open_ide), self));
    gtk_box_append(GTK_BOX(md_r), make_tool_btn("🔍 FIND", G_CALLBACK(on_find_btn_clicked), self));
    gtk_stack_add_named(GTK_STACK(self->ribbon_stack), md_r, "markdown");

    /* Image ribbon */
    GtkWidget *img_r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(img_r), make_tool_btn("◀ PREV", G_CALLBACK(on_img_prev), self));
    self->img_idx_lbl = gtk_label_new("1 / 1");
    gtk_box_append(GTK_BOX(img_r), self->img_idx_lbl);
    gtk_box_append(GTK_BOX(img_r), make_tool_btn("NEXT ▶", G_CALLBACK(on_img_next), self));
    gtk_box_append(GTK_BOX(img_r), make_sep());
    gtk_box_append(GTK_BOX(img_r), make_tool_btn("−", G_CALLBACK(on_img_zout), self));
    self->img_zoom_lbl = gtk_label_new("100%");
    gtk_widget_set_size_request(self->img_zoom_lbl, 44, -1);
    gtk_box_append(GTK_BOX(img_r), self->img_zoom_lbl);
    gtk_box_append(GTK_BOX(img_r), make_tool_btn("+", G_CALLBACK(on_img_zin), self));

    GtkWidget *i_seg_z = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(i_seg_z, "viewer-seg-group");
    self->img_btn_fit = make_seg_btn("FIT", G_CALLBACK(on_img_fit), self);
    self->img_btn_100 = make_seg_btn("1:1", G_CALLBACK(on_img_100), self);
    self->img_btn_fill = make_seg_btn("FILL", G_CALLBACK(on_img_fill), self);
    gtk_box_append(GTK_BOX(i_seg_z), self->img_btn_fit);
    gtk_box_append(GTK_BOX(i_seg_z), self->img_btn_100);
    gtk_box_append(GTK_BOX(i_seg_z), self->img_btn_fill);
    gtk_box_append(GTK_BOX(img_r), i_seg_z);

    GtkWidget *img_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(img_spacer, TRUE);
    gtk_box_append(GTK_BOX(img_r), img_spacer);

    self->img_btn_anim = make_tool_btn("⏸ PAUSE", G_CALLBACK(on_img_anim), self);
    gtk_widget_set_visible(self->img_btn_anim, FALSE);
    gtk_box_append(GTK_BOX(img_r), self->img_btn_anim);
    gtk_box_append(GTK_BOX(img_r), make_tool_btn("↻ 90°", G_CALLBACK(on_img_rot), self));
    self->img_btn_flip = make_tool_btn("⇄ FLIP", G_CALLBACK(on_img_flip), self);
    gtk_box_append(GTK_BOX(img_r), self->img_btn_flip);
    self->img_btn_checker = make_tool_btn("▦ ALPHA", G_CALLBACK(on_img_checker), self);
    gtk_box_append(GTK_BOX(img_r), self->img_btn_checker);
    self->img_btn_strip = make_tool_btn("▤ STRIP", G_CALLBACK(on_img_strip), self);
    gtk_box_append(GTK_BOX(img_r), self->img_btn_strip);
    gtk_box_append(GTK_BOX(img_r), make_tool_btn("📋 COPY", G_CALLBACK(on_img_copy), self));
    gtk_stack_add_named(GTK_STACK(self->ribbon_stack), img_r, "image");

    gtk_box_append(GTK_BOX(ribbon), self->ribbon_stack);
    gtk_box_append(GTK_BOX(root), ribbon);

    /* 3. Workspace Body (Sidebar + Main Column) */
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);

    self->sidebar = viewer_sidebar_new(
        on_pdf_sidebar_page_selected, on_md_sidebar_heading_selected, self);
    gtk_box_append(GTK_BOX(body), viewer_sidebar_get_widget(self->sidebar));

    GtkWidget *main_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(main_col, TRUE);
    gtk_widget_set_vexpand(main_col, TRUE);

    /* Find Bar (Hidden until Ctrl+F) */
    self->find_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(self->find_bar, "viewer-inline-bar");
    gtk_widget_set_valign(self->find_bar, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(self->find_bar, FALSE);

    GtkWidget *flbl = gtk_label_new("🔍 FIND");
    gtk_widget_add_css_class(flbl, "viewer-find-tag");
    self->find_entry = gtk_entry_new();
    gtk_widget_add_css_class(self->find_entry, "viewer-find-input");
    gtk_widget_set_size_request(self->find_entry, 240, 24);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->find_entry), "Search document...");
    g_signal_connect(self->find_entry, "changed", G_CALLBACK(on_find_changed), self);
    g_signal_connect(self->find_entry, "activate", G_CALLBACK(on_find_next), self);

    self->find_case_btn = make_tool_btn("Aa", G_CALLBACK(on_find_case_clicked), self);
    self->find_count_lbl = gtk_label_new("0 / 0");
    gtk_widget_set_size_request(self->find_count_lbl, 60, -1);
    GtkWidget *f_sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(f_sp, TRUE);

    gtk_box_append(GTK_BOX(self->find_bar), flbl);
    gtk_box_append(GTK_BOX(self->find_bar), self->find_entry);
    gtk_box_append(GTK_BOX(self->find_bar), self->find_case_btn);
    gtk_box_append(GTK_BOX(self->find_bar), self->find_count_lbl);
    gtk_box_append(GTK_BOX(self->find_bar), make_tool_btn("▲", G_CALLBACK(on_find_prev), self));
    gtk_box_append(GTK_BOX(self->find_bar), make_tool_btn("▼", G_CALLBACK(on_find_next), self));
    gtk_box_append(GTK_BOX(self->find_bar), f_sp);
    gtk_box_append(GTK_BOX(self->find_bar), make_tool_btn("✕ CLOSE", G_CALLBACK(on_find_close_clicked), self));
    gtk_box_append(GTK_BOX(main_col), self->find_bar);

    /* Encrypted PDF Lock Bar */
    self->lock_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(self->lock_bar, "viewer-inline-bar");
    gtk_widget_add_css_class(self->lock_bar, "warn");
    gtk_widget_set_visible(self->lock_bar, FALSE);

    GtkWidget *llbl = gtk_label_new("🔒 ENCRYPTED PDF");
    gtk_widget_add_css_class(llbl, "viewer-warn-tag");
    self->lock_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(self->lock_entry), TRUE);
    gtk_widget_set_size_request(self->lock_entry, 240, 24);
    g_signal_connect(self->lock_entry, "activate", G_CALLBACK(on_unlock_pdf), self);
    gtk_box_append(GTK_BOX(self->lock_bar), llbl);
    gtk_box_append(GTK_BOX(self->lock_bar), self->lock_entry);
    gtk_box_append(GTK_BOX(self->lock_bar), make_tool_btn("UNLOCK", G_CALLBACK(on_unlock_pdf), self));
    gtk_box_append(GTK_BOX(main_col), self->lock_bar);

    /* Main Content Stack */
    self->content_stack = gtk_stack_new();
    gtk_widget_set_hexpand(self->content_stack, TRUE);
    gtk_widget_set_vexpand(self->content_stack, TRUE);

    /* Empty Page */
    GtkWidget *empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(empty_box, "viewer-empty-drop");
    gtk_widget_set_halign(empty_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty_box, GTK_ALIGN_CENTER);
    GtkWidget *ei = gtk_label_new("▤");
    gtk_widget_add_css_class(ei, "viewer-empty-icon");
    GtkWidget *et = gtk_label_new("DROP A DOCUMENT OR IMAGE HERE");
    gtk_widget_add_css_class(et, "viewer-empty-title");
    GtkWidget *es = gtk_label_new("Strictly read-only Aero + Metro viewer for PDF, Markdown (+ Mermaid), and Images.");
    gtk_widget_add_css_class(es, "viewer-empty-hint");
    GtkWidget *eb = make_tool_btn("📂 OPEN FILE (Ctrl+O)", G_CALLBACK(on_open_clicked), self);
    gtk_widget_add_css_class(eb, "active");
    gtk_box_append(GTK_BOX(empty_box), ei);
    gtk_box_append(GTK_BOX(empty_box), et);
    gtk_box_append(GTK_BOX(empty_box), es);
    gtk_box_append(GTK_BOX(empty_box), eb);
    gtk_stack_add_named(GTK_STACK(self->content_stack), empty_box, "empty");

    self->pdf_view = viewer_pdf_view_new(on_subview_state_changed, on_pdf_navigate_uri, self);
    gtk_stack_add_named(GTK_STACK(self->content_stack), viewer_pdf_view_get_widget(self->pdf_view), "pdf");

    self->md_view = viewer_markdown_view_new(on_subview_state_changed, on_md_open_target, self);
    gtk_stack_add_named(GTK_STACK(self->content_stack), viewer_markdown_view_get_widget(self->md_view), "markdown");

    self->img_view = viewer_image_view_new(on_subview_state_changed, self);
    gtk_stack_add_named(GTK_STACK(self->content_stack), viewer_image_view_get_widget(self->img_view), "image");

    gtk_box_append(GTK_BOX(main_col), self->content_stack);
    gtk_box_append(GTK_BOX(body), main_col);
    gtk_box_append(GTK_BOX(root), body);

    /* 4. Bottom Telemetry Strip */
    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(status, "viewer-status-strip");
    gtk_widget_set_valign(status, GTK_ALIGN_CENTER);

    self->status_primary_lbl = gtk_label_new("IDLE");
    gtk_widget_add_css_class(self->status_primary_lbl, "viewer-status-accent");
    self->status_secondary_lbl = gtk_label_new("READ-ONLY");
    self->status_path_lbl = gtk_label_new("org.kajo.Viewer");
    gtk_widget_set_hexpand(self->status_path_lbl, TRUE);
    gtk_label_set_xalign(GTK_LABEL(self->status_path_lbl), 1.0f);
    gtk_label_set_ellipsize(GTK_LABEL(self->status_path_lbl), PANGO_ELLIPSIZE_START);

    gtk_box_append(GTK_BOX(status), self->status_primary_lbl);
    gtk_box_append(GTK_BOX(status), self->status_secondary_lbl);
    gtk_box_append(GTK_BOX(status), self->status_path_lbl);
    gtk_box_append(GTK_BOX(root), status);

    gtk_window_set_child(GTK_WINDOW(self), root);

    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop_file), self);
    gtk_widget_add_controller(root, GTK_EVENT_CONTROLLER(drop));

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    sync_all_ui(self);
}

KajoViewerWindow *
kajo_viewer_window_new(GtkApplication *app)
{
    return g_object_new(KAJO_TYPE_VIEWER_WINDOW, "application", app, NULL);
}

void
kajo_viewer_window_open_file(KajoViewerWindow *self, GFile *file)
{
    open_file_internal(self, file, TRUE, 0, 0.0);
}

void
kajo_viewer_window_open_preview_mode(KajoViewerWindow *self, const char *mode)
{
    if (!self || !mode)
        return;

    if (g_strcmp0(mode, "pdf") == 0) {
        const char *tmp_pdf = "/tmp/kajo-viewer-preview.pdf";
        cairo_surface_t *surf = cairo_pdf_surface_create(tmp_pdf, 595.0, 842.0);
        cairo_t *cr = cairo_create(surf);
        for (int p = 1; p <= 3; p++) {
            cairo_set_source_rgb(cr, 0.08, 0.09, 0.12);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 20.0);
            cairo_move_to(cr, 56.0, 76.0);
            g_autofree char *h = g_strdup_printf("Kajo Viewer Specimen — Page %d", p);
            cairo_show_text(cr, h);

            cairo_set_source_rgb(cr, 0.0, 0.47, 0.83);
            cairo_rectangle(cr, 56.0, 92.0, 483.0, 3.0);
            cairo_fill(cr);

            cairo_set_source_rgb(cr, 0.18, 0.20, 0.24);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 12.0);
            cairo_move_to(cr, 56.0, 132.0);
            cairo_show_text(cr, "Read-only Aero + Metro Poppler PDF preview with dark paper & highlight tones.");
            cairo_show_page(cr);
        }
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        g_autoptr(GFile) f = g_file_new_for_path(tmp_pdf);
        open_file_internal(self, f, TRUE, 0, 0.0);
    } else if (g_strcmp0(mode, "md") == 0 || g_strcmp0(mode, "markdown") == 0) {
        const char *sample_md =
            "# Kajo Viewer Markdown Preview\n\n"
            "Strictly read-only Markdown viewer sharing the **Kajo** Cairo Mermaid & Pango engine.\n\n"
            "## 1. Architecture Summary\n\n"
            "| Module | Responsibility | Mode |\n"
            "| :--- | :--- | :--- |\n"
            "| `viewer_pdf.c` | Poppler lazy LRU renderer | Read-Only |\n"
            "| `viewer_markdown.c` | Shared MD + Mermaid host | Read-Only |\n"
            "| `viewer_image.c` | Glycin + RSVG + EXIF canvas | Read-Only |\n\n"
            "## 2. Rendering Pipeline\n\n"
            "```mermaid\n"
            "flowchart LR\n"
            "    File[\"File URI\"] --> Model[\"viewer_model\"]\n"
            "    Model --> PDF[\"Poppler PDF\"]\n"
            "    Model --> MD[\"KajoMarkdown + Mermaid\"]\n"
            "    Model --> IMG[\"Glycin / RSVG Canvas\"]\n"
            "```\n\n"
            "## 3. Verification Checklist\n\n"
            "- [x] Zero corner radius Aero + Metro window chrome\n"
            "- [x] Shared Poppler and Mermaid renderers\n"
            "- [ ] Headless screenshot review complete\n";

        self->mode = VIEWER_KIND_MARKDOWN;
        g_free(self->current_path);
        self->current_path = g_strdup("kajo-viewer-preview.md");
        self->current_file_size = strlen(sample_md);
        viewer_markdown_view_load_text(self->md_view, sample_md, self->current_path);
        sync_all_ui(self);
    } else if (g_strcmp0(mode, "image") == 0) {
        const int w = 720, h = 440;
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(s);
        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, w, h);
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.04, 0.11, 0.22);
        cairo_pattern_add_color_stop_rgb(grad, 0.6, 0.06, 0.24, 0.44);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.85, 0.45, 0.05);
        cairo_set_source(cr, grad);
        cairo_paint(cr);
        cairo_pattern_destroy(grad);

        cairo_set_source_rgb(cr, 1.0, 0.72, 0.0);
        cairo_arc(cr, 540.0, 120.0, 44.0, 0.0, 2.0 * G_PI);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.07, 0.09, 0.13);
        cairo_move_to(cr, 0.0, h);
        cairo_line_to(cr, 220.0, 220.0);
        cairo_line_to(cr, 420.0, h);
        cairo_close_path(cr);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.10, 0.13, 0.18);
        cairo_move_to(cr, 260.0, h);
        cairo_line_to(cr, 500.0, 170.0);
        cairo_line_to(cr, w, h);
        cairo_close_path(cr);
        cairo_fill(cr);

        cairo_destroy(cr);
        cairo_surface_flush(s);

        int stride = cairo_image_surface_get_stride(s);
        g_autoptr(GBytes) bytes = g_bytes_new(cairo_image_surface_get_data(s), (gsize)stride * (gsize)h);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
        GdkMemoryFormat fmt = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
#else
        GdkMemoryFormat fmt = GDK_MEMORY_A8R8G8B8_PREMULTIPLIED;
#endif
        g_autoptr(GdkTexture) tex = gdk_memory_texture_new(w, h, fmt, bytes, (gsize)stride);
        cairo_surface_destroy(s);

        self->mode = VIEWER_KIND_IMAGE;
        g_free(self->current_path);
        self->current_path = g_strdup("kajo-viewer-preview.png");
        self->current_file_size = (guint64)(w * h * 4);
        viewer_image_view_load_texture(self->img_view, tex, self->current_path, "PREVIEW");
        sync_all_ui(self);
    }
}