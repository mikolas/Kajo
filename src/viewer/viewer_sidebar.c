#include "viewer_sidebar.h"
#include "../common/pdf_render.h"

struct _ViewerSidebar {
    ViewerSidebarPdfPageCb   pdf_cb;
    ViewerSidebarMdHeadingCb md_cb;
    gpointer                 user_data;

    GtkWidget *root_box;
    GtkWidget *stack;

    /* PDF panels */
    GtkWidget *pdf_outline_btn;
    GtkWidget *pdf_pages_btn;
    GtkWidget *pdf_inner_stack;
    GtkWidget *pdf_outline_list;
    GtkWidget *pdf_thumb_list;
    GPtrArray *pdf_outline_rows;  /* GtkWidget* */
    GPtrArray *pdf_thumb_cards;   /* GtkWidget* */
    PopplerDocument *pdf_doc;
    int        active_pdf_page;
    guint      pdf_thumb_idle_id;
    int        pdf_thumb_next;

    /* Markdown outline */
    GtkWidget *md_count_badge;
    GtkWidget *md_outline_list;
    GtkWidget *md_meta_box;
    GPtrArray *md_heading_rows;
    guint      md_total_lines;

    /* Image metadata inspector */
    GtkWidget *img_kind_badge;
    GtkWidget *img_meta_box;
};

static void
clear_box_children(GtkWidget *box)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL)
        gtk_box_remove(GTK_BOX(box), child);
}

static void
on_pdf_outline_row_clicked(GtkButton *btn, gpointer user_data)
{
    ViewerSidebar *sb = user_data;
    int page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pdf-page")) - 1;
    if (sb && sb->pdf_cb && page >= 0) {
        sb->pdf_cb(page, sb->user_data);
    }
}

static void
on_pdf_thumb_clicked(GtkButton *btn, gpointer user_data)
{
    ViewerSidebar *sb = user_data;
    int page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pdf-page")) - 1;
    if (sb && sb->pdf_cb && page >= 0) {
        sb->pdf_cb(page, sb->user_data);
    }
}

static void
on_md_heading_clicked(GtkButton *btn, gpointer user_data)
{
    ViewerSidebar *sb = user_data;
    guint line = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "md-line"));
    if (sb && sb->md_cb && line > 0) {
        sb->md_cb(line, sb->md_total_lines, sb->user_data);
    }
}

static void
populate_outline_iter(ViewerSidebar   *sb,
                      PopplerDocument *doc,
                      PopplerIndexIter *iter,
                      int              depth)
{
    if (!iter)
        return;

    do {
        PopplerAction *act = poppler_index_iter_get_action(iter);
        int page_1based = 0;
        const char *title = NULL;

        if (act) {
            title = act->any.title;
            if (act->type == POPPLER_ACTION_GOTO_DEST && act->goto_dest.dest) {
                PopplerDest *dest = act->goto_dest.dest;
                if (dest->type == POPPLER_DEST_NAMED && dest->named_dest) {
                    PopplerDest *resolved = poppler_document_find_dest(doc, dest->named_dest);
                    if (resolved) {
                        page_1based = resolved->page_num;
                        poppler_dest_free(resolved);
                    }
                } else {
                    page_1based = dest->page_num;
                }
            }
        }

        if (title && *title) {
            GtkWidget *btn = gtk_button_new();
            gtk_widget_add_css_class(btn, "viewer-toc-item");
            gtk_widget_set_can_focus(btn, FALSE);
            if (depth >= 2)
                gtk_widget_add_css_class(btn, "lvl-3");
            else if (depth == 1)
                gtk_widget_add_css_class(btn, "lvl-2");

            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
            gtk_widget_set_margin_start(row, MIN(depth, 4) * 10);

            GtkWidget *lbl = gtk_label_new(title);
            gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
            gtk_widget_set_hexpand(lbl, TRUE);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(row), lbl);

            if (page_1based > 0) {
                g_autofree char *pstr = g_strdup_printf("%d", page_1based);
                GtkWidget *plbl = gtk_label_new(pstr);
                gtk_widget_add_css_class(plbl, "viewer-toc-page");
                gtk_widget_set_valign(plbl, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(row), plbl);
                g_object_set_data(G_OBJECT(btn), "pdf-page", GINT_TO_POINTER(page_1based));
            }

            gtk_button_set_child(GTK_BUTTON(btn), row);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_pdf_outline_row_clicked), sb);
            gtk_box_append(GTK_BOX(sb->pdf_outline_list), btn);
            g_ptr_array_add(sb->pdf_outline_rows, btn);
        }

        if (act)
            poppler_action_free(act);

        PopplerIndexIter *child = poppler_index_iter_get_child(iter);
        if (child) {
            populate_outline_iter(sb, doc, child, depth + 1);
            poppler_index_iter_free(child);
        }
    } while (poppler_index_iter_next(iter));
}

static gboolean
on_pdf_thumb_idle(gpointer user_data)
{
    ViewerSidebar *sb = user_data;
    if (!sb || !sb->pdf_doc || !sb->pdf_thumb_cards) {
        if (sb)
            sb->pdf_thumb_idle_id = 0;
        return G_SOURCE_REMOVE;
    }

    int batch = 0;
    while (sb->pdf_thumb_next < (int)sb->pdf_thumb_cards->len && batch < 2) {
        GtkWidget *btn = g_ptr_array_index(sb->pdf_thumb_cards, (guint)sb->pdf_thumb_next);
        GtkWidget *pic = g_object_get_data(G_OBJECT(btn), "thumb-pic");
        if (pic) {
            g_autoptr(GdkTexture) tex = kajo_pdf_render_page_texture(sb->pdf_doc, sb->pdf_thumb_next, 140);
            if (tex) {
                gtk_picture_set_paintable(GTK_PICTURE(pic), GDK_PAINTABLE(tex));
            }
        }
        sb->pdf_thumb_next++;
        batch++;
    }

    if (sb->pdf_thumb_next >= (int)sb->pdf_thumb_cards->len) {
        sb->pdf_thumb_idle_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void
on_pdf_tab_clicked(GtkButton *btn, gpointer user_data)
{
    ViewerSidebar *sb = user_data;
    if (!sb)
        return;
    viewer_sidebar_toggle_pdf_tab(sb, (GtkWidget *)btn == sb->pdf_pages_btn);
}

static GtkWidget *
create_meta_section(const char *heading, GtkWidget **out_body)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "viewer-meta-section");

    GtkWidget *hdr = gtk_label_new(heading);
    gtk_widget_add_css_class(hdr, "viewer-meta-head");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
    gtk_box_append(GTK_BOX(box), hdr);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), body);
    if (out_body)
        *out_body = body;
    return box;
}

static void
append_meta_row(GtkWidget *body, const char *key, const char *val)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(row, "viewer-meta-row");
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);

    GtkWidget *klbl = gtk_label_new(key);
    gtk_widget_add_css_class(klbl, "viewer-meta-k");
    gtk_widget_set_valign(klbl, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(klbl), 0.0f);

    GtkWidget *vlbl = gtk_label_new((val && *val) ? val : "—");
    gtk_widget_add_css_class(vlbl, "viewer-meta-v");
    gtk_widget_set_valign(vlbl, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(vlbl, TRUE);
    gtk_label_set_xalign(GTK_LABEL(vlbl), 1.0f);
    gtk_label_set_ellipsize(GTK_LABEL(vlbl), PANGO_ELLIPSIZE_MIDDLE);

    gtk_box_append(GTK_BOX(row), klbl);
    gtk_box_append(GTK_BOX(row), vlbl);
    gtk_box_append(GTK_BOX(body), row);
}

ViewerSidebar *
viewer_sidebar_new(ViewerSidebarPdfPageCb   pdf_cb,
                   ViewerSidebarMdHeadingCb md_cb,
                   gpointer                 user_data)
{
    ViewerSidebar *sb = g_new0(ViewerSidebar, 1);
    sb->pdf_cb = pdf_cb;
    sb->md_cb = md_cb;
    sb->user_data = user_data;
    sb->pdf_outline_rows = g_ptr_array_new();
    sb->pdf_thumb_cards = g_ptr_array_new();
    sb->md_heading_rows = g_ptr_array_new();

    sb->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sb->root_box, "viewer-sidebar");
    gtk_widget_set_size_request(sb->root_box, 240, -1);
    gtk_widget_set_hexpand(sb->root_box, FALSE);
    gtk_widget_set_vexpand(sb->root_box, TRUE);

    sb->stack = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(sb->stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(sb->stack), FALSE);
    gtk_widget_set_vexpand(sb->stack, TRUE);
    gtk_box_append(GTK_BOX(sb->root_box), sb->stack);

    /* 1. PDF Sidebar Page */
    GtkWidget *pdf_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *pdf_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(pdf_hdr, "viewer-sidebar-hdr");
    GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(seg, "viewer-seg-group");
    gtk_widget_set_hexpand(seg, TRUE);

    sb->pdf_outline_btn = gtk_button_new_with_label("OUTLINE");
    gtk_widget_add_css_class(sb->pdf_outline_btn, "viewer-seg-btn");
    gtk_widget_add_css_class(sb->pdf_outline_btn, "active");
    gtk_widget_set_can_focus(sb->pdf_outline_btn, FALSE);
    gtk_widget_set_hexpand(sb->pdf_outline_btn, TRUE);
    g_signal_connect(sb->pdf_outline_btn, "clicked", G_CALLBACK(on_pdf_tab_clicked), sb);

    sb->pdf_pages_btn = gtk_button_new_with_label("PAGES");
    gtk_widget_add_css_class(sb->pdf_pages_btn, "viewer-seg-btn");
    gtk_widget_set_can_focus(sb->pdf_pages_btn, FALSE);
    gtk_widget_set_hexpand(sb->pdf_pages_btn, TRUE);
    g_signal_connect(sb->pdf_pages_btn, "clicked", G_CALLBACK(on_pdf_tab_clicked), sb);

    gtk_box_append(GTK_BOX(seg), sb->pdf_outline_btn);
    gtk_box_append(GTK_BOX(seg), sb->pdf_pages_btn);
    gtk_box_append(GTK_BOX(pdf_hdr), seg);
    gtk_box_append(GTK_BOX(pdf_page), pdf_hdr);

    sb->pdf_inner_stack = gtk_stack_new();
    gtk_widget_set_vexpand(sb->pdf_inner_stack, TRUE);

    GtkWidget *out_sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(out_sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(out_sc), FALSE);
    sb->pdf_outline_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(sb->pdf_outline_list, "viewer-sidebar-content");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(out_sc), sb->pdf_outline_list);
    gtk_stack_add_named(GTK_STACK(sb->pdf_inner_stack), out_sc, "outline");

    GtkWidget *thm_sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(thm_sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(thm_sc), FALSE);
    sb->pdf_thumb_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(sb->pdf_thumb_list, "viewer-sidebar-content");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(thm_sc), sb->pdf_thumb_list);
    gtk_stack_add_named(GTK_STACK(sb->pdf_inner_stack), thm_sc, "pages");

    gtk_box_append(GTK_BOX(pdf_page), sb->pdf_inner_stack);
    gtk_stack_add_named(GTK_STACK(sb->stack), pdf_page, "pdf");

    /* 2. Markdown Sidebar Page */
    GtkWidget *md_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *md_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(md_hdr, "viewer-sidebar-hdr");
    gtk_widget_set_valign(md_hdr, GTK_ALIGN_CENTER);

    GtkWidget *md_title = gtk_label_new("HEADING OUTLINE");
    gtk_widget_add_css_class(md_title, "viewer-sidebar-title");
    gtk_widget_set_valign(md_title, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(md_title, TRUE);
    gtk_label_set_xalign(GTK_LABEL(md_title), 0.0f);

    sb->md_count_badge = gtk_label_new("0");
    gtk_widget_add_css_class(sb->md_count_badge, "shell-popover-badge");
    gtk_widget_set_valign(sb->md_count_badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(md_hdr), md_title);
    gtk_box_append(GTK_BOX(md_hdr), sb->md_count_badge);
    gtk_box_append(GTK_BOX(md_page), md_hdr);

    GtkWidget *md_sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(md_sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(md_sc), FALSE);
    gtk_widget_set_vexpand(md_sc, TRUE);
    GtkWidget *md_wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(md_wrap, "viewer-sidebar-content");
    sb->md_outline_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    sb->md_meta_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(md_wrap), sb->md_outline_list);
    gtk_box_append(GTK_BOX(md_wrap), sb->md_meta_box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(md_sc), md_wrap);
    gtk_box_append(GTK_BOX(md_page), md_sc);
    gtk_stack_add_named(GTK_STACK(sb->stack), md_page, "markdown");

    /* 3. Image Sidebar Page */
    GtkWidget *img_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *img_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(img_hdr, "viewer-sidebar-hdr");
    gtk_widget_set_valign(img_hdr, GTK_ALIGN_CENTER);

    GtkWidget *img_title = gtk_label_new("IMAGE INSPECTOR");
    gtk_widget_add_css_class(img_title, "viewer-sidebar-title");
    gtk_widget_set_valign(img_title, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(img_title, TRUE);
    gtk_label_set_xalign(GTK_LABEL(img_title), 0.0f);

    sb->img_kind_badge = gtk_label_new("IMAGE");
    gtk_widget_add_css_class(sb->img_kind_badge, "shell-popover-badge");
    gtk_widget_set_valign(sb->img_kind_badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(img_hdr), img_title);
    gtk_box_append(GTK_BOX(img_hdr), sb->img_kind_badge);
    gtk_box_append(GTK_BOX(img_page), img_hdr);

    GtkWidget *img_sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(img_sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(img_sc), FALSE);
    gtk_widget_set_vexpand(img_sc, TRUE);
    sb->img_meta_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(sb->img_meta_box, "viewer-sidebar-content");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(img_sc), sb->img_meta_box);
    gtk_box_append(GTK_BOX(img_page), img_sc);
    gtk_stack_add_named(GTK_STACK(sb->stack), img_page, "image");

    return sb;
}

void
viewer_sidebar_free(ViewerSidebar *sb)
{
    if (!sb)
        return;
    g_clear_handle_id(&sb->pdf_thumb_idle_id, g_source_remove);
    g_clear_object(&sb->pdf_doc);
    g_ptr_array_free(sb->pdf_outline_rows, TRUE);
    g_ptr_array_free(sb->pdf_thumb_cards, TRUE);
    g_ptr_array_free(sb->md_heading_rows, TRUE);
    g_free(sb);
}

GtkWidget *
viewer_sidebar_get_widget(ViewerSidebar *sb)
{
    return sb ? sb->root_box : NULL;
}

void
viewer_sidebar_set_mode(ViewerSidebar *sb, ViewerDocKind kind)
{
    if (!sb)
        return;
    if (kind == VIEWER_KIND_PDF) {
        gtk_stack_set_visible_child_name(GTK_STACK(sb->stack), "pdf");
    } else if (kind == VIEWER_KIND_MARKDOWN) {
        gtk_stack_set_visible_child_name(GTK_STACK(sb->stack), "markdown");
    } else if (viewer_model_is_image_kind(kind)) {
        gtk_stack_set_visible_child_name(GTK_STACK(sb->stack), "image");
    }
}

void
viewer_sidebar_toggle_pdf_tab(ViewerSidebar *sb, gboolean show_pages)
{
    if (!sb)
        return;
    gtk_widget_remove_css_class(sb->pdf_outline_btn, "active");
    gtk_widget_remove_css_class(sb->pdf_pages_btn, "active");
    if (show_pages) {
        gtk_widget_add_css_class(sb->pdf_pages_btn, "active");
        gtk_stack_set_visible_child_name(GTK_STACK(sb->pdf_inner_stack), "pages");
    } else {
        gtk_widget_add_css_class(sb->pdf_outline_btn, "active");
        gtk_stack_set_visible_child_name(GTK_STACK(sb->pdf_inner_stack), "outline");
    }
}

void
viewer_sidebar_set_pdf_doc(ViewerSidebar *sb, PopplerDocument *doc)
{
    if (!sb)
        return;
    g_clear_handle_id(&sb->pdf_thumb_idle_id, g_source_remove);
    g_clear_object(&sb->pdf_doc);
    clear_box_children(sb->pdf_outline_list);
    clear_box_children(sb->pdf_thumb_list);
    g_ptr_array_set_size(sb->pdf_outline_rows, 0);
    g_ptr_array_set_size(sb->pdf_thumb_cards, 0);
    sb->pdf_thumb_next = 0;

    if (!doc)
        return;
    sb->pdf_doc = g_object_ref(doc);

    PopplerIndexIter *root_iter = poppler_index_iter_new(doc);
    if (root_iter) {
        populate_outline_iter(sb, doc, root_iter, 0);
        poppler_index_iter_free(root_iter);
    } else {
        GtkWidget *empty = gtk_label_new("No embedded outline in PDF.\nSwitch to PAGES for thumbnails.");
        gtk_widget_add_css_class(empty, "viewer-empty-hint");
        gtk_label_set_wrap(GTK_LABEL(empty), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(empty), 26);
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0f);
        gtk_box_append(GTK_BOX(sb->pdf_outline_list), empty);
    }

    int n_pages = poppler_document_get_n_pages(doc);
    int limit_thumbs = MIN(n_pages, 120);
    for (int i = 0; i < limit_thumbs; i++) {
        GtkWidget *btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "viewer-thumb-card");
        gtk_widget_set_can_focus(btn, FALSE);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);

        GtkWidget *pic = gtk_picture_new();
        gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
        gtk_widget_set_size_request(pic, 110, 142);
        gtk_widget_add_css_class(pic, "viewer-thumb-preview");

        g_autofree char *lbl_txt = g_strdup_printf("PAGE %d", i + 1);
        GtkWidget *lbl = gtk_label_new(lbl_txt);
        gtk_widget_add_css_class(lbl, "viewer-thumb-label");

        gtk_box_append(GTK_BOX(vbox), pic);
        gtk_box_append(GTK_BOX(vbox), lbl);
        gtk_button_set_child(GTK_BUTTON(btn), vbox);
        g_object_set_data(G_OBJECT(btn), "pdf-page", GINT_TO_POINTER(i + 1));
        g_object_set_data(G_OBJECT(btn), "thumb-pic", pic);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_pdf_thumb_clicked), sb);

        gtk_box_append(GTK_BOX(sb->pdf_thumb_list), btn);
        g_ptr_array_add(sb->pdf_thumb_cards, btn);
    }

    if (n_pages > limit_thumbs) {
        g_autofree char *note = g_strdup_printf("Showing thumbnails 1–%d of %d", limit_thumbs, n_pages);
        GtkWidget *nl = gtk_label_new(note);
        gtk_widget_add_css_class(nl, "viewer-empty-hint");
        gtk_box_append(GTK_BOX(sb->pdf_thumb_list), nl);
    }

    if (sb->pdf_thumb_cards->len > 0) {
        sb->pdf_thumb_idle_id = g_idle_add(on_pdf_thumb_idle, sb);
    }
    viewer_sidebar_set_active_pdf_page(sb, 0);
}

void
viewer_sidebar_set_active_pdf_page(ViewerSidebar *sb, int page_index)
{
    if (!sb)
        return;
    sb->active_pdf_page = page_index;

    for (guint i = 0; i < sb->pdf_thumb_cards->len; i++) {
        GtkWidget *card = g_ptr_array_index(sb->pdf_thumb_cards, i);
        if ((int)i == page_index)
            gtk_widget_add_css_class(card, "active");
        else
            gtk_widget_remove_css_class(card, "active");
    }

    int target_1based = page_index + 1;
    GtkWidget *best_row = NULL;
    for (guint i = 0; i < sb->pdf_outline_rows->len; i++) {
        GtkWidget *row = g_ptr_array_index(sb->pdf_outline_rows, i);
        gtk_widget_remove_css_class(row, "active");
        int p = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "pdf-page"));
        if (p > 0 && p <= target_1based) {
            best_row = row;
        }
    }
    if (best_row)
        gtk_widget_add_css_class(best_row, "active");
}

void
viewer_sidebar_set_markdown_headings(ViewerSidebar *sb,
                                     GArray        *headings,
                                     guint          total_lines,
                                     gboolean       live_watch,
                                     int            mermaid_count,
                                     int            tasks_done,
                                     int            tasks_total)
{
    if (!sb)
        return;
    clear_box_children(sb->md_outline_list);
    clear_box_children(sb->md_meta_box);
    g_ptr_array_set_size(sb->md_heading_rows, 0);
    sb->md_total_lines = total_lines;

    guint count = headings ? headings->len : 0;
    g_autofree char *cstr = g_strdup_printf("%u", count);
    gtk_label_set_text(GTK_LABEL(sb->md_count_badge), cstr);

    if (count == 0) {
        GtkWidget *empty = gtk_label_new("No headings in document.");
        gtk_widget_add_css_class(empty, "viewer-empty-hint");
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0f);
        gtk_box_append(GTK_BOX(sb->md_outline_list), empty);
    } else {
        for (guint i = 0; i < headings->len; i++) {
            const IdeMarkdownHeading *h = &g_array_index(headings, IdeMarkdownHeading, i);
            GtkWidget *btn = gtk_button_new();
            gtk_widget_add_css_class(btn, "viewer-toc-item");
            gtk_widget_set_can_focus(btn, FALSE);
            if (h->level >= 3)
                gtk_widget_add_css_class(btn, "lvl-3");
            else if (h->level == 2)
                gtk_widget_add_css_class(btn, "lvl-2");

            GtkWidget *lbl = gtk_label_new(h->title);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_margin_start(lbl, (int)(MIN(h->level - 1, 3) * 10));
            gtk_button_set_child(GTK_BUTTON(btn), lbl);

            g_object_set_data(G_OBJECT(btn), "md-line", GUINT_TO_POINTER(h->line_1based));
            g_signal_connect(btn, "clicked", G_CALLBACK(on_md_heading_clicked), sb);
            gtk_box_append(GTK_BOX(sb->md_outline_list), btn);
            g_ptr_array_add(sb->md_heading_rows, btn);
        }
    }

    GtkWidget *meta_body = NULL;
    gtk_box_append(GTK_BOX(sb->md_meta_box), create_meta_section("DOCUMENT TELEMETRY", &meta_body));
    append_meta_row(meta_body, "Access Mode", "Read-Only");
    append_meta_row(meta_body, "Live Monitor", live_watch ? "ACTIVE" : "OFF");
    g_autofree char *mstr = g_strdup_printf("%d", mermaid_count);
    append_meta_row(meta_body, "Mermaid Diagrams", mstr);
    if (tasks_total > 0) {
        g_autofree char *tstr = g_strdup_printf("%d / %d", tasks_done, tasks_total);
        append_meta_row(meta_body, "Checklist Items", tstr);
    }
    viewer_sidebar_set_active_md_line(sb, 1);
}

void
viewer_sidebar_set_active_md_line(ViewerSidebar *sb, guint line_1based)
{
    if (!sb || !sb->md_heading_rows)
        return;
    GtkWidget *best = NULL;
    for (guint i = 0; i < sb->md_heading_rows->len; i++) {
        GtkWidget *row = g_ptr_array_index(sb->md_heading_rows, i);
        gtk_widget_remove_css_class(row, "active");
        guint ln = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "md-line"));
        if (ln > 0 && ln <= line_1based)
            best = row;
    }
    if (!best && sb->md_heading_rows->len > 0)
        best = g_ptr_array_index(sb->md_heading_rows, 0);
    if (best)
        gtk_widget_add_css_class(best, "active");
}

void
viewer_sidebar_set_image_meta(ViewerSidebar *sb, const ViewerImageMeta *meta)
{
    if (!sb || !meta)
        return;
    clear_box_children(sb->img_meta_box);

    gtk_label_set_text(GTK_LABEL(sb->img_kind_badge),
                       meta->decoder[0] ? meta->decoder : "IMAGE");

    GtkWidget *dim_body = NULL;
    gtk_box_append(GTK_BOX(sb->img_meta_box), create_meta_section("DECODER & DIMENSIONS", &dim_body));
    append_meta_row(dim_body, "Backend", meta->decoder);
    append_meta_row(dim_body, "MIME", meta->mime_type);
    g_autofree char *res = (meta->width > 0 && meta->height > 0)
        ? g_strdup_printf("%d × %d px", meta->width, meta->height)
        : g_strdup("—");
    append_meta_row(dim_body, "Resolution", res);
    char size_str[32];
    viewer_model_format_bytes(meta->file_size, size_str, sizeof(size_str));
    append_meta_row(dim_body, "File Size", size_str);
    g_autofree char *zstr = g_strdup_printf("%d%%", meta->zoom_pct);
    append_meta_row(dim_body, "Zoom", zstr);
    g_autofree char *tstr = g_strdup_printf("%d°%s%s",
                                            meta->rotation_deg,
                                            meta->flip_h ? " · FlipH" : "",
                                            meta->flip_v ? " · FlipV" : "");
    append_meta_row(dim_body, "Transform", tstr);
    if (meta->is_animated) {
        append_meta_row(dim_body, "Playback", meta->anim_playing ? "PLAYING" : "PAUSED");
    }

    GtkWidget *exif_body = NULL;
    gtk_box_append(GTK_BOX(sb->img_meta_box), create_meta_section("EXIF & COLORIMETRY", &exif_body));
    append_meta_row(exif_body, "Camera / Source", meta->camera);
    append_meta_row(exif_body, "Exposure", meta->exposure);
    append_meta_row(exif_body, "Profile", meta->colorspace);
    append_meta_row(exif_body, "Captured", meta->date_str);
}