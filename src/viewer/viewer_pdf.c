#include "viewer_pdf.h"

#include "../common/spawn_detached.h"
#include <math.h>
#include <string.h>

#define PDF_LRU_CAPACITY 12

typedef struct {
    int        page_index;
    int        scale_pct;
    int        rotation_deg;
    int        tone;
    guint      search_gen;
    guint64    last_use;
    GdkTexture *texture;
} PdfCacheEntry;

typedef struct {
    int                  page_index;
    KajoPdfHighlightRect rect;
} PdfSearchMatch;

typedef struct {
    ViewerPdfView *view;
    int            page_index;
    GtkWidget     *row_slot;
    GtkWidget     *overlay;
    GtkWidget     *picture;
    GtkWidget     *sel_draw;
    double         page_w;
    double         page_h;
    int            last_disp_w;
    int            last_disp_h;
    int            rendered_key_pct;
    int            rendered_rot;
    int            rendered_tone;
    guint          rendered_search_gen;
    gboolean       has_sel;
    double         sel_start_x, sel_start_y;
    double         sel_cur_x, sel_cur_y;
} PdfPageSlot;

static void
pdf_page_slot_free(gpointer p)
{
    PdfPageSlot *slot = p;
    if (!slot)
        return;
    if (slot->overlay) {
        GtkWidget *parent = gtk_widget_get_parent(slot->overlay);
        if (parent && GTK_IS_BOX(parent))
            gtk_box_remove(GTK_BOX(parent), slot->overlay);
        g_object_unref(slot->overlay);
        slot->overlay = NULL;
    }
    g_free(slot);
}

struct _ViewerPdfView {
    ViewerPdfStateCb       state_cb;
    ViewerPdfNavigateUriCb nav_cb;
    gpointer               user_data;

    GtkWidget       *root_scroll;
    GtkWidget       *pages_box;
    PopplerDocument *doc;
    PopplerDocument *render_doc;
    char            *doc_path;

    GPtrArray       *slots;         /* PdfPageSlot* */
    GArray          *lru_cache;     /* PdfCacheEntry */
    guint64          lru_clock;

    int              current_page;
    int              n_pages;
    double           zoom;
    ViewerFitMode    fit_mode;
    gboolean         spread_mode;
    int              rotation_deg;
    KajoPdfPaperTone paper_tone;

    /* Serialized single-worker async rendering */
    GCancellable    *render_cancel;
    gboolean         render_in_flight;
    guint            doc_gen;
    guint            viewport_timer_id;

    /* Search state */
    GArray          *matches;       /* PdfSearchMatch */
    int              match_idx;
    char            *search_query;
    gboolean         search_case;
    guint            search_gen;
};

static void schedule_viewport_render(ViewerPdfView *view);

static void
notify_state(ViewerPdfView *view)
{
    if (view && view->state_cb)
        view->state_cb(view->user_data);
}

static void
clear_lru_cache(ViewerPdfView *view)
{
    if (!view || !view->lru_cache)
        return;
    for (guint i = 0; i < view->lru_cache->len; i++) {
        PdfCacheEntry *e = &g_array_index(view->lru_cache, PdfCacheEntry, i);
        g_clear_object(&e->texture);
    }
    g_array_set_size(view->lru_cache, 0);
}

static GdkTexture *
lru_lookup(ViewerPdfView *view, int page_idx, int scale_pct, int rot, int tone, guint search_gen)
{
    for (guint i = 0; i < view->lru_cache->len; i++) {
        PdfCacheEntry *e = &g_array_index(view->lru_cache, PdfCacheEntry, i);
        if (e->page_index == page_idx &&
            e->scale_pct == scale_pct &&
            e->rotation_deg == rot &&
            e->tone == tone &&
            e->search_gen == search_gen &&
            e->texture) {
            e->last_use = ++view->lru_clock;
            return e->texture;
        }
    }
    return NULL;
}

static void
lru_insert(ViewerPdfView *view, int page_idx, int scale_pct, int rot, int tone, guint search_gen, GdkTexture *tex)
{
    if (!tex)
        return;
    for (guint i = 0; i < view->lru_cache->len; i++) {
        PdfCacheEntry *e = &g_array_index(view->lru_cache, PdfCacheEntry, i);
        if (e->page_index == page_idx) {
            g_set_object(&e->texture, tex);
            e->scale_pct = scale_pct;
            e->rotation_deg = rot;
            e->tone = tone;
            e->search_gen = search_gen;
            e->last_use = ++view->lru_clock;
            return;
        }
    }

    if (view->lru_cache->len >= PDF_LRU_CAPACITY) {
        guint oldest_idx = 0;
        guint64 oldest_tick = G_MAXUINT64;
        for (guint i = 0; i < view->lru_cache->len; i++) {
            PdfCacheEntry *e = &g_array_index(view->lru_cache, PdfCacheEntry, i);
            if (e->last_use < oldest_tick) {
                oldest_tick = e->last_use;
                oldest_idx = i;
            }
        }
        PdfCacheEntry *ev = &g_array_index(view->lru_cache, PdfCacheEntry, oldest_idx);
        g_clear_object(&ev->texture);
        g_array_remove_index_fast(view->lru_cache, oldest_idx);
    }

    PdfCacheEntry entry = {
        .page_index   = page_idx,
        .scale_pct    = scale_pct,
        .rotation_deg = rot,
        .tone         = tone,
        .search_gen   = search_gen,
        .last_use     = ++view->lru_clock,
        .texture      = g_object_ref(tex),
    };
    g_array_append_val(view->lru_cache, entry);
}

static void
compute_slot_display_size(ViewerPdfView *view, const PdfPageSlot *slot, int *out_w, int *out_h)
{
    double pw = slot->page_w;
    double ph = slot->page_h;
    if (view->rotation_deg == 90 || view->rotation_deg == 270) {
        double tmp = pw;
        pw = ph;
        ph = tmp;
    }
    int w = MAX(1, (int)(pw * view->zoom + 0.5));
    int h = MAX(1, (int)(ph * view->zoom + 0.5));
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

static void
recompute_fit_zoom(ViewerPdfView *view)
{
    if (!view || !view->slots || view->slots->len == 0)
        return;
    if (view->fit_mode == VIEWER_FIT_ACTUAL) {
        view->zoom = 1.0;
        return;
    }
    if (view->fit_mode != VIEWER_FIT_WIDTH && view->fit_mode != VIEWER_FIT_PAGE)
        return;

    int vw = gtk_widget_get_width(view->root_scroll);
    int vh = gtk_widget_get_height(view->root_scroll);
    if (vw <= 40)
        vw = 920;
    if (vh <= 40)
        vh = 640;

    int cur = CLAMP(view->current_page, 0, (int)view->slots->len - 1);
    PdfPageSlot *slot = g_ptr_array_index(view->slots, (guint)cur);
    double pw = (view->rotation_deg == 90 || view->rotation_deg == 270) ? slot->page_h : slot->page_w;
    double ph = (view->rotation_deg == 90 || view->rotation_deg == 270) ? slot->page_w : slot->page_h;

    double avail_w = (double)vw - 48.0;
    if (view->spread_mode)
        avail_w = (avail_w - 16.0) * 0.5;
    double avail_h = (double)vh - 36.0;

    view->zoom = viewer_model_compute_fit_scale(pw, ph, MAX(120.0, avail_w), MAX(120.0, avail_h), view->fit_mode);
}

static void
slot_screen_to_page_pts(ViewerPdfView *view, const PdfPageSlot *slot, double sx, double sy, double *out_px, double *out_py)
{
    int dw = MAX(1, slot->last_disp_w);
    int dh = MAX(1, slot->last_disp_h);
    double u = sx / (double)dw;
    double v = sy / (double)dh;
    double px = 0.0, py = 0.0;

    switch (view->rotation_deg) {
    case 90:
        px = v * slot->page_w;
        py = (1.0 - u) * slot->page_h;
        break;
    case 180:
        px = (1.0 - u) * slot->page_w;
        py = (1.0 - v) * slot->page_h;
        break;
    case 270:
        px = (1.0 - v) * slot->page_w;
        py = u * slot->page_h;
        break;
    default:
        px = u * slot->page_w;
        py = v * slot->page_h;
        break;
    }
    *out_px = CLAMP(px, 0.0, slot->page_w);
    *out_py = CLAMP(py, 0.0, slot->page_h);
}

static void
on_slot_draw_selection(GtkDrawingArea *area G_GNUC_UNUSED,
                       cairo_t        *cr,
                       int             width G_GNUC_UNUSED,
                       int             height G_GNUC_UNUSED,
                       gpointer        user_data)
{
    PdfPageSlot *slot = user_data;
    if (!slot || !slot->has_sel)
        return;

    double x = fmin(slot->sel_start_x, slot->sel_cur_x);
    double y = fmin(slot->sel_start_y, slot->sel_cur_y);
    double w = fabs(slot->sel_cur_x - slot->sel_start_x);
    double h = fabs(slot->sel_cur_y - slot->sel_start_y);
    if (w < 2.0 && h < 2.0)
        return;

    cairo_set_source_rgba(cr, 0.0, 0.471, 0.831, 0.25);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.0, 0.847, 0.941, 0.85);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, MAX(1.0, w - 1.0), MAX(1.0, h - 1.0));
    cairo_stroke(cr);
}

static void
clear_other_selections(ViewerPdfView *view, PdfPageSlot *keep)
{
    for (guint i = 0; i < view->slots->len; i++) {
        PdfPageSlot *s = g_ptr_array_index(view->slots, i);
        if (s != keep && s->has_sel) {
            s->has_sel = FALSE;
            gtk_widget_queue_draw(s->sel_draw);
        }
    }
}

static void
on_slot_drag_begin(GtkGestureDrag *gesture G_GNUC_UNUSED, double x, double y, gpointer user_data)
{
    PdfPageSlot *slot = user_data;
    if (!slot || !slot->view)
        return;
    clear_other_selections(slot->view, slot);
    slot->has_sel = TRUE;
    slot->sel_start_x = x;
    slot->sel_start_y = y;
    slot->sel_cur_x = x;
    slot->sel_cur_y = y;
    gtk_widget_queue_draw(slot->sel_draw);
}

static void
on_slot_drag_update(GtkGestureDrag *gesture G_GNUC_UNUSED, double dx, double dy, gpointer user_data)
{
    PdfPageSlot *slot = user_data;
    if (!slot)
        return;
    slot->sel_cur_x = slot->sel_start_x + dx;
    slot->sel_cur_y = slot->sel_start_y + dy;
    gtk_widget_queue_draw(slot->sel_draw);
}

static void
on_slot_click_released(GtkGestureClick *gesture G_GNUC_UNUSED,
                       int              n_press G_GNUC_UNUSED,
                       double           x,
                       double           y,
                       gpointer         user_data)
{
    PdfPageSlot *slot = user_data;
    ViewerPdfView *view = slot ? slot->view : NULL;
    if (!view || !view->doc)
        return;

    /* Ignore drag releases */
    if (slot->has_sel && (fabs(slot->sel_cur_x - slot->sel_start_x) > 4.0 ||
                          fabs(slot->sel_cur_y - slot->sel_start_y) > 4.0)) {
        return;
    }

    clear_other_selections(view, NULL);
    view->current_page = slot->page_index;
    notify_state(view);

    PopplerPage *page = poppler_document_get_page(view->doc, slot->page_index);
    if (!page)
        return;

    double px = 0.0, py = 0.0;
    slot_screen_to_page_pts(view, slot, x, y, &px, &py);
    double pdf_y = slot->page_h - py;

    GList *links = poppler_page_get_link_mapping(page);
    for (GList *l = links; l != NULL; l = l->next) {
        PopplerLinkMapping *lm = l->data;
        if (px >= lm->area.x1 && px <= lm->area.x2 &&
            pdf_y >= lm->area.y1 && pdf_y <= lm->area.y2 && lm->action) {
            if (lm->action->type == POPPLER_ACTION_GOTO_DEST && lm->action->goto_dest.dest) {
                PopplerDest *dest = lm->action->goto_dest.dest;
                int target = dest->page_num;
                if (dest->type == POPPLER_DEST_NAMED && dest->named_dest) {
                    PopplerDest *res = poppler_document_find_dest(view->doc, dest->named_dest);
                    if (res) {
                        target = res->page_num;
                        poppler_dest_free(res);
                    }
                }
                if (target > 0)
                    viewer_pdf_view_jump_to_page(view, target - 1);
                break;
            } else if (lm->action->type == POPPLER_ACTION_URI && lm->action->uri.uri) {
                if (view->nav_cb) {
                    view->nav_cb(lm->action->uri.uri, view->user_data);
                } else if (!g_app_info_launch_default_for_uri(lm->action->uri.uri, NULL, NULL)) {
                    const char *argv[] = { "xdg-open", lm->action->uri.uri, NULL };
                    kajo_spawn_detached_argv(NULL, argv, NULL, NULL);
                }
                break;
            }
        }
    }
    poppler_page_free_link_mapping(links);
    g_object_unref(page);
}

static void
rebuild_page_containers(ViewerPdfView *view)
{
    if (view->slots) {
        for (guint i = 0; i < view->slots->len; i++) {
            PdfPageSlot *slot = g_ptr_array_index(view->slots, i);
            if (slot && slot->overlay) {
                GtkWidget *parent = gtk_widget_get_parent(slot->overlay);
                if (parent && GTK_IS_BOX(parent))
                    gtk_box_remove(GTK_BOX(parent), slot->overlay);
            }
        }
    }

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(view->pages_box)) != NULL)
        gtk_box_remove(GTK_BOX(view->pages_box), child);

    if (!view->slots || view->slots->len == 0)
        return;

    recompute_fit_zoom(view);

    for (guint i = 0; i < view->slots->len;) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_widget_set_halign(row, GTK_ALIGN_CENTER);

        int count = view->spread_mode ? 2 : 1;
        for (int c = 0; c < count && i < view->slots->len; c++, i++) {
            PdfPageSlot *slot = g_ptr_array_index(view->slots, i);
            int w = 0, h = 0;
            compute_slot_display_size(view, slot, &w, &h);
            slot->last_disp_w = w;
            slot->last_disp_h = h;
            gtk_widget_set_size_request(slot->overlay, w, h);
            gtk_widget_set_size_request(slot->picture, w, h);
            gtk_widget_set_size_request(slot->sel_draw, w, h);
            gtk_box_append(GTK_BOX(row), slot->overlay);
        }
        gtk_box_append(GTK_BOX(view->pages_box), row);
    }

    schedule_viewport_render(view);
}

typedef struct {
    GWeakRef         scroll_ref;
    guint            doc_gen;
    PopplerDocument *doc;
    int              page_index;
    double           render_scale;
    int              scale_pct;
    int              rotation_deg;
    KajoPdfPaperTone tone;
    guint            search_gen;
    GArray          *highlights; /* KajoPdfHighlightRect */
} PdfRenderJob;

static void
pdf_render_job_free(gpointer p)
{
    PdfRenderJob *job = p;
    if (!job)
        return;
    g_weak_ref_clear(&job->scroll_ref);
    g_clear_object(&job->doc);
    if (job->highlights)
        g_array_unref(job->highlights);
    g_free(job);
}

static void
pdf_render_worker(GTask        *task,
                  gpointer      source_object G_GNUC_UNUSED,
                  gpointer      task_data,
                  GCancellable *cancellable)
{
    PdfRenderJob *job = task_data;
    if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_boolean(task, FALSE);
        return;
    }

    const KajoPdfHighlightRect *rects = (job->highlights && job->highlights->len > 0)
        ? &g_array_index(job->highlights, KajoPdfHighlightRect, 0)
        : NULL;
    size_t n_rects = job->highlights ? job->highlights->len : 0;

    GdkTexture *tex = kajo_pdf_render_page_scaled(
        job->doc, job->page_index, job->render_scale, job->rotation_deg, job->tone, rects, n_rects);
    if (tex)
        g_task_return_pointer(task, tex, g_object_unref);
    else
        g_task_return_pointer(task, NULL, NULL);
}

static void
on_pdf_render_done(GObject      *source G_GNUC_UNUSED,
                   GAsyncResult *res,
                   gpointer      user_data)
{
    PdfRenderJob *job = user_data;
    GtkWidget *scroll = g_weak_ref_get(&job->scroll_ref);
    if (!scroll)
        return;

    ViewerPdfView *view = g_object_get_data(G_OBJECT(scroll), "viewer-pdf-self");
    if (!view) {
        g_object_unref(scroll);
        return;
    }

    view->render_in_flight = FALSE;
    g_autoptr(GError) err = NULL;
    g_autoptr(GdkTexture) tex = g_task_propagate_pointer(G_TASK(res), &err);

    if (!err &&
        job->doc_gen == view->doc_gen &&
        tex != NULL &&
        job->page_index >= 0 &&
        job->page_index < (int)view->slots->len) {
        lru_insert(view, job->page_index, job->scale_pct, job->rotation_deg,
                   job->tone, job->search_gen, tex);

        int scale_factor = MAX(1, gtk_widget_get_scale_factor(view->root_scroll));
        int cur_pct = (int)(view->zoom * 100.0 * (double)scale_factor + 0.5);
        PdfPageSlot *slot = g_ptr_array_index(view->slots, (guint)job->page_index);
        if (job->scale_pct == cur_pct &&
            job->rotation_deg == view->rotation_deg &&
            job->tone == view->paper_tone &&
            job->search_gen == view->search_gen) {
            gtk_picture_set_paintable(GTK_PICTURE(slot->picture), GDK_PAINTABLE(tex));
            slot->rendered_key_pct = job->scale_pct;
            slot->rendered_rot = job->rotation_deg;
            slot->rendered_tone = job->tone;
            slot->rendered_search_gen = job->search_gen;
        }
    }

    schedule_viewport_render(view);
    g_object_unref(scroll);
}

static void
update_current_page_from_scroll(ViewerPdfView *view, int *out_first_vis, int *out_last_vis)
{
    if (!view || !view->slots || view->slots->len == 0)
        return;

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(view->root_scroll));
    double scroll_y = vadj ? gtk_adjustment_get_value(vadj) : 0.0;
    double view_h = vadj ? gtk_adjustment_get_page_size(vadj) : 700.0;
    if (view_h < 100.0)
        view_h = 700.0;

    int first_vis = -1, last_vis = -1;
    int best_page = view->current_page;
    double best_overlap = -1.0;

    double y_cursor = 20.0;
    for (guint i = 0; i < view->slots->len;) {
        int step = view->spread_mode ? 2 : 1;
        int row_h = 0;
        for (int c = 0; c < step && (i + (guint)c) < view->slots->len; c++) {
            PdfPageSlot *s = g_ptr_array_index(view->slots, i + (guint)c);
            row_h = MAX(row_h, s->last_disp_h);
        }

        double y1 = y_cursor;
        double y2 = y_cursor + (double)row_h;
        double ov = fmin(y2, scroll_y + view_h) - fmax(y1, scroll_y);
        if (ov > 0.0) {
            if (first_vis < 0)
                first_vis = (int)i;
            last_vis = MIN((int)view->slots->len - 1, (int)i + step - 1);
            if (ov > best_overlap) {
                best_overlap = ov;
                best_page = (int)i;
            }
        }
        y_cursor += (double)row_h + 18.0;
        i += (guint)step;
    }

    if (first_vis < 0) {
        first_vis = CLAMP(view->current_page, 0, (int)view->slots->len - 1);
        last_vis = first_vis;
    }
    if (best_page != view->current_page) {
        view->current_page = best_page;
        notify_state(view);
    }
    if (out_first_vis) *out_first_vis = first_vis;
    if (out_last_vis) *out_last_vis = last_vis;
}

static GArray *
collect_page_highlights(ViewerPdfView *view, int page_index)
{
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(KajoPdfHighlightRect));
    if (!view->matches)
        return arr;
    for (guint i = 0; i < view->matches->len; i++) {
        const PdfSearchMatch *m = &g_array_index(view->matches, PdfSearchMatch, i);
        if (m->page_index == page_index) {
            KajoPdfHighlightRect r = m->rect;
            r.is_active = ((int)i == view->match_idx);
            g_array_append_val(arr, r);
        }
    }
    return arr;
}

static gboolean
on_viewport_render_idle(gpointer user_data)
{
    ViewerPdfView *view = user_data;
    view->viewport_timer_id = 0;
    if (!view->doc || !view->slots || view->slots->len == 0 || view->render_in_flight)
        return G_SOURCE_REMOVE;

    int first_vis = 0, last_vis = 0;
    update_current_page_from_scroll(view, &first_vis, &last_vis);

    int scale_factor = MAX(1, gtk_widget_get_scale_factor(view->root_scroll));
    double render_scale = view->zoom * (double)scale_factor;
    int scale_pct = (int)(render_scale * 100.0 + 0.5);

    int start = MAX(0, first_vis - 2);
    int end = MIN((int)view->slots->len - 1, last_vis + 2);

    int pick = -1;
    for (int i = start; i <= end; i++) {
        PdfPageSlot *slot = g_ptr_array_index(view->slots, (guint)i);
        if (slot->rendered_key_pct == scale_pct &&
            slot->rendered_rot == view->rotation_deg &&
            slot->rendered_tone == (int)view->paper_tone &&
            slot->rendered_search_gen == view->search_gen) {
            continue;
        }
        GdkTexture *cached = lru_lookup(view, i, scale_pct, view->rotation_deg,
                                        view->paper_tone, view->search_gen);
        if (cached) {
            gtk_picture_set_paintable(GTK_PICTURE(slot->picture), GDK_PAINTABLE(cached));
            slot->rendered_key_pct = scale_pct;
            slot->rendered_rot = view->rotation_deg;
            slot->rendered_tone = view->paper_tone;
            slot->rendered_search_gen = view->search_gen;
        } else if (pick < 0 || (i >= first_vis && i <= last_vis && (pick < first_vis || pick > last_vis))) {
            pick = i;
        }
    }

    if (pick < 0)
        return G_SOURCE_REMOVE;

    PdfRenderJob *job = g_new0(PdfRenderJob, 1);
    g_weak_ref_init(&job->scroll_ref, view->root_scroll);
    job->doc_gen = view->doc_gen;
    job->doc = g_object_ref(view->render_doc ? view->render_doc : view->doc);
    job->page_index = pick;
    job->render_scale = render_scale;
    job->scale_pct = scale_pct;
    job->rotation_deg = view->rotation_deg;
    job->tone = view->paper_tone;
    job->search_gen = view->search_gen;
    job->highlights = collect_page_highlights(view, pick);

    view->render_in_flight = TRUE;
    GTask *task = g_task_new(NULL, view->render_cancel, on_pdf_render_done, job);
    g_task_set_task_data(task, job, pdf_render_job_free);
    g_task_run_in_thread(task, pdf_render_worker);
    g_object_unref(task);
    return G_SOURCE_REMOVE;
}

static void
schedule_viewport_render(ViewerPdfView *view)
{
    if (!view || view->viewport_timer_id > 0)
        return;
    view->viewport_timer_id = g_timeout_add(30, on_viewport_render_idle, view);
}

static void
on_scroll_or_size_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    ViewerPdfView *view = user_data;
    if (!view)
        return;
    if (view->fit_mode == VIEWER_FIT_WIDTH || view->fit_mode == VIEWER_FIT_PAGE) {
        double prev = view->zoom;
        recompute_fit_zoom(view);
        if (fabs(view->zoom - prev) > 0.01) {
            rebuild_page_containers(view);
            notify_state(view);
            return;
        }
    }
    schedule_viewport_render(view);
}

static void
on_vadj_value_changed(GtkAdjustment *adj G_GNUC_UNUSED, gpointer user_data)
{
    schedule_viewport_render((ViewerPdfView *)user_data);
}

static void
on_pdf_scroll_map(GtkWidget *w G_GNUC_UNUSED, gpointer user_data)
{
    on_scroll_or_size_changed(NULL, NULL, user_data);
}

static gboolean
on_pdf_scroll_wheel(GtkEventControllerScroll *ctrl,
                    double                    dx G_GNUC_UNUSED,
                    double                    dy,
                    gpointer                  user_data)
{
    ViewerPdfView *view = user_data;
    GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(ctrl));
    if (!(mods & GDK_CONTROL_MASK))
        return FALSE;
    if (dy < 0.0)
        viewer_pdf_view_step_zoom(view, 1);
    else if (dy > 0.0)
        viewer_pdf_view_step_zoom(view, -1);
    return TRUE;
}

ViewerPdfView *
viewer_pdf_view_new(ViewerPdfStateCb       state_cb,
                    ViewerPdfNavigateUriCb nav_cb,
                    gpointer               user_data)
{
    ViewerPdfView *view = g_new0(ViewerPdfView, 1);
    view->state_cb = state_cb;
    view->nav_cb = nav_cb;
    view->user_data = user_data;
    view->zoom = 1.0;
    view->fit_mode = VIEWER_FIT_WIDTH;
    view->paper_tone = KAJO_PDF_TONE_PAPER;
    view->slots = g_ptr_array_new_with_free_func(pdf_page_slot_free);
    view->lru_cache = g_array_new(FALSE, TRUE, sizeof(PdfCacheEntry));
    view->matches = g_array_new(FALSE, TRUE, sizeof(PdfSearchMatch));
    view->match_idx = -1;

    view->root_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view->root_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(view->root_scroll, TRUE);
    gtk_widget_set_vexpand(view->root_scroll, TRUE);
    gtk_widget_add_css_class(view->root_scroll, "viewer-canvas-scroll");
    g_object_set_data(G_OBJECT(view->root_scroll), "viewer-pdf-self", view);

    view->pages_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(view->pages_box, 20);
    gtk_widget_set_margin_end(view->pages_box, 20);
    gtk_widget_set_margin_top(view->pages_box, 20);
    gtk_widget_set_margin_bottom(view->pages_box, 24);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view->root_scroll), view->pages_box);

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(view->root_scroll));
    if (vadj) {
        g_signal_connect(vadj, "value-changed", G_CALLBACK(on_vadj_value_changed), view);
        g_signal_connect(vadj, "notify::page-size", G_CALLBACK(on_scroll_or_size_changed), view);
    }
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(view->root_scroll));
    if (hadj) {
        g_signal_connect(hadj, "notify::page-size", G_CALLBACK(on_scroll_or_size_changed), view);
    }
    g_signal_connect(view->root_scroll, "map", G_CALLBACK(on_pdf_scroll_map), view);

    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_pdf_scroll_wheel), view);
    gtk_widget_add_controller(view->root_scroll, scroll);

    return view;
}

static void
clear_current_doc(ViewerPdfView *view)
{
    if (!view)
        return;
    view->doc_gen++;
    if (view->render_cancel) {
        g_cancellable_cancel(view->render_cancel);
        g_clear_object(&view->render_cancel);
    }
    view->render_in_flight = FALSE;
    g_clear_handle_id(&view->viewport_timer_id, g_source_remove);

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(view->pages_box)) != NULL)
        gtk_box_remove(GTK_BOX(view->pages_box), child);

    g_ptr_array_set_size(view->slots, 0);
    clear_lru_cache(view);
    g_array_set_size(view->matches, 0);
    g_clear_pointer(&view->search_query, g_free);
    g_clear_pointer(&view->doc_path, g_free);
    g_clear_object(&view->render_doc);
    g_clear_object(&view->doc);
    view->n_pages = 0;
    view->current_page = 0;
    view->match_idx = -1;
}

void
viewer_pdf_view_free(ViewerPdfView *view)
{
    if (!view)
        return;
    if (view->root_scroll) {
        g_object_set_data(G_OBJECT(view->root_scroll), "viewer-pdf-self", NULL);
    }
    clear_current_doc(view);
    g_ptr_array_free(view->slots, TRUE);
    g_array_free(view->lru_cache, TRUE);
    g_array_free(view->matches, TRUE);
    g_free(view);
}

GtkWidget *
viewer_pdf_view_get_widget(ViewerPdfView *view)
{
    return view ? view->root_scroll : NULL;
}

ViewerPdfLoadResult
viewer_pdf_view_load_file(ViewerPdfView *view,
                          GFile         *file,
                          const char    *password,
                          GError       **error)
{
    if (!view || !file)
        return VIEWER_PDF_LOAD_ERROR;

    g_autoptr(GError) local_err = NULL;
    PopplerDocument *doc = poppler_document_new_from_gfile(file, password, NULL, &local_err);
    if (!doc) {
        if (local_err && g_error_matches(local_err, POPPLER_ERROR, POPPLER_ERROR_ENCRYPTED)) {
            if (error)
                *error = g_steal_pointer(&local_err);
            return VIEWER_PDF_LOAD_ENCRYPTED;
        }
        if (error)
            *error = g_steal_pointer(&local_err);
        return VIEWER_PDF_LOAD_ERROR;
    }

    clear_current_doc(view);
    view->doc = doc;
    view->render_doc = poppler_document_new_from_gfile(file, password, NULL, NULL);
    view->doc_path = g_file_get_path(file);
    view->render_cancel = g_cancellable_new();
    view->n_pages = poppler_document_get_n_pages(doc);
    view->current_page = 0;

    for (int i = 0; i < view->n_pages; i++) {
        PdfPageSlot *slot = g_new0(PdfPageSlot, 1);
        slot->view = view;
        slot->page_index = i;
        slot->page_w = 595.0;
        slot->page_h = 842.0;

        PopplerPage *p = poppler_document_get_page(doc, i);
        if (p) {
            poppler_page_get_size(p, &slot->page_w, &slot->page_h);
            g_object_unref(p);
        }

        slot->overlay = gtk_overlay_new();
        g_object_ref_sink(slot->overlay);
        gtk_widget_add_css_class(slot->overlay, "viewer-pdf-sheet");
        gtk_widget_set_halign(slot->overlay, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(slot->overlay, FALSE);

        slot->picture = gtk_picture_new();
        gtk_picture_set_can_shrink(GTK_PICTURE(slot->picture), TRUE);
        gtk_picture_set_content_fit(GTK_PICTURE(slot->picture), GTK_CONTENT_FIT_FILL);
        gtk_overlay_set_child(GTK_OVERLAY(slot->overlay), slot->picture);

        slot->sel_draw = gtk_drawing_area_new();
        gtk_widget_set_can_target(slot->sel_draw, FALSE);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(slot->sel_draw), on_slot_draw_selection, slot, NULL);
        gtk_overlay_add_overlay(GTK_OVERLAY(slot->overlay), slot->sel_draw);

        GtkGesture *drag = gtk_gesture_drag_new();
        g_signal_connect(drag, "drag-begin", G_CALLBACK(on_slot_drag_begin), slot);
        g_signal_connect(drag, "drag-update", G_CALLBACK(on_slot_drag_update), slot);
        gtk_widget_add_controller(slot->overlay, GTK_EVENT_CONTROLLER(drag));

        GtkGesture *click = gtk_gesture_click_new();
        g_signal_connect(click, "released", G_CALLBACK(on_slot_click_released), slot);
        gtk_widget_add_controller(slot->overlay, GTK_EVENT_CONTROLLER(click));

        g_ptr_array_add(view->slots, slot);
    }

    rebuild_page_containers(view);
    notify_state(view);
    return VIEWER_PDF_LOAD_OK;
}

PopplerDocument *
viewer_pdf_view_get_document(ViewerPdfView *view)
{
    return view ? view->doc : NULL;
}

void
viewer_pdf_view_jump_to_page(ViewerPdfView *view, int page_index)
{
    if (!view || !view->slots || view->slots->len == 0)
        return;

    int target = CLAMP(page_index, 0, (int)view->slots->len - 1);
    view->current_page = target;

    double y = 20.0;
    int step = view->spread_mode ? 2 : 1;
    for (int i = 0; i < target && i < (int)view->slots->len; i += step) {
        if (view->spread_mode && target == i + 1)
            break;
        int row_h = 0;
        for (int c = 0; c < step && (i + c) < (int)view->slots->len; c++) {
            PdfPageSlot *s = g_ptr_array_index(view->slots, (guint)(i + c));
            row_h = MAX(row_h, s->last_disp_h);
        }
        y += (double)row_h + 18.0;
    }

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(view->root_scroll));
    if (vadj) {
        double max_v = MAX(0.0, gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj));
        gtk_adjustment_set_value(vadj, CLAMP(y - 16.0, 0.0, max_v));
    }
    schedule_viewport_render(view);
    notify_state(view);
}

void
viewer_pdf_view_step_page(ViewerPdfView *view, int delta)
{
    if (!view || view->n_pages <= 0)
        return;
    int step = view->spread_mode ? delta * 2 : delta;
    viewer_pdf_view_jump_to_page(view, view->current_page + step);
}

int
viewer_pdf_view_get_current_page(const ViewerPdfView *view)
{
    return view ? view->current_page : 0;
}

int
viewer_pdf_view_get_num_pages(const ViewerPdfView *view)
{
    return view ? view->n_pages : 0;
}

void
viewer_pdf_view_set_zoom_fit(ViewerPdfView *view, ViewerFitMode fit)
{
    if (!view)
        return;
    view->fit_mode = fit;
    recompute_fit_zoom(view);
    rebuild_page_containers(view);
    notify_state(view);
}

void
viewer_pdf_view_step_zoom(ViewerPdfView *view, int direction)
{
    if (!view)
        return;
    view->fit_mode = VIEWER_FIT_CONTAIN;
    view->zoom = viewer_model_step_zoom(view->zoom, direction, 0.25, 4.0);
    rebuild_page_containers(view);
    notify_state(view);
}

double
viewer_pdf_view_get_zoom(const ViewerPdfView *view)
{
    return view ? view->zoom : 1.0;
}

ViewerFitMode
viewer_pdf_view_get_fit_mode(const ViewerPdfView *view)
{
    return view ? view->fit_mode : VIEWER_FIT_WIDTH;
}

void
viewer_pdf_view_set_spread(ViewerPdfView *view, gboolean spread)
{
    if (!view || view->spread_mode == spread)
        return;
    view->spread_mode = spread;
    rebuild_page_containers(view);
    notify_state(view);
}

gboolean
viewer_pdf_view_get_spread(const ViewerPdfView *view)
{
    return view ? view->spread_mode : FALSE;
}

void
viewer_pdf_view_rotate(ViewerPdfView *view, int delta_deg)
{
    if (!view)
        return;
    view->rotation_deg = (((view->rotation_deg + delta_deg) % 360) + 360) % 360;
    rebuild_page_containers(view);
    notify_state(view);
}

int
viewer_pdf_view_get_rotation(const ViewerPdfView *view)
{
    return view ? view->rotation_deg : 0;
}

void
viewer_pdf_view_set_tone(ViewerPdfView *view, KajoPdfPaperTone tone)
{
    if (!view || view->paper_tone == tone)
        return;
    view->paper_tone = tone;
    schedule_viewport_render(view);
    notify_state(view);
}

KajoPdfPaperTone
viewer_pdf_view_get_tone(const ViewerPdfView *view)
{
    return view ? view->paper_tone : KAJO_PDF_TONE_PAPER;
}

void
viewer_pdf_view_set_search(ViewerPdfView *view, const char *query, gboolean case_sensitive)
{
    if (!view)
        return;
    g_array_set_size(view->matches, 0);
    g_free(view->search_query);
    view->search_query = (query && *query) ? g_strdup(query) : NULL;
    view->search_case = case_sensitive;
    view->match_idx = -1;
    view->search_gen++;

    if (view->doc && view->search_query) {
        PopplerFindFlags flags = case_sensitive ? POPPLER_FIND_CASE_SENSITIVE : POPPLER_FIND_DEFAULT;
        for (int i = 0; i < view->n_pages; i++) {
            PopplerPage *page = poppler_document_get_page(view->doc, i);
            if (!page)
                continue;
            GList *rects = poppler_page_find_text_with_options(page, view->search_query, flags);
            for (GList *l = rects; l != NULL; l = l->next) {
                PopplerRectangle *r = l->data;
                PdfSearchMatch m = {
                    .page_index = i,
                    .rect = { r->x1, r->y1, r->x2, r->y2, FALSE },
                };
                g_array_append_val(view->matches, m);
            }
            g_list_free_full(rects, (GDestroyNotify)poppler_rectangle_free);
            g_object_unref(page);
        }
        if (view->matches->len > 0) {
            view->match_idx = 0;
            PdfSearchMatch *first = &g_array_index(view->matches, PdfSearchMatch, 0);
            viewer_pdf_view_jump_to_page(view, first->page_index);
        }
    }
    schedule_viewport_render(view);
    notify_state(view);
}

void
viewer_pdf_view_step_search(ViewerPdfView *view, int delta)
{
    if (!view || !view->matches || view->matches->len == 0)
        return;
    int len = (int)view->matches->len;
    view->match_idx = (((view->match_idx + delta) % len) + len) % len;
    view->search_gen++;
    PdfSearchMatch *m = &g_array_index(view->matches, PdfSearchMatch, (guint)view->match_idx);
    viewer_pdf_view_jump_to_page(view, m->page_index);
}

int
viewer_pdf_view_get_search_total(const ViewerPdfView *view)
{
    return (view && view->matches) ? (int)view->matches->len : 0;
}

int
viewer_pdf_view_get_search_index(const ViewerPdfView *view)
{
    return view ? view->match_idx : -1;
}

gboolean
viewer_pdf_view_copy_selection(ViewerPdfView *view)
{
    if (!view || !view->doc || !view->slots)
        return FALSE;

    g_autofree char *copied = NULL;
    for (guint i = 0; i < view->slots->len; i++) {
        PdfPageSlot *s = g_ptr_array_index(view->slots, i);
        if (!s->has_sel)
            continue;
        PopplerPage *page = poppler_document_get_page(view->doc, s->page_index);
        if (!page)
            continue;
        double x1, y1, x2, y2;
        slot_screen_to_page_pts(view, s, s->sel_start_x, s->sel_start_y, &x1, &y1);
        slot_screen_to_page_pts(view, s, s->sel_cur_x, s->sel_cur_y, &x2, &y2);
        PopplerRectangle sel = {
            .x1 = fmin(x1, x2),
            .y1 = fmin(y1, y2),
            .x2 = fmax(x1, x2),
            .y2 = fmax(y1, y2),
        };
        copied = poppler_page_get_selected_text(page, POPPLER_SELECTION_GLYPH, &sel);
        g_object_unref(page);
        if (copied && *copied)
            break;
        g_clear_pointer(&copied, g_free);
    }

    if (!copied && view->current_page >= 0 && view->current_page < view->n_pages) {
        PopplerPage *page = poppler_document_get_page(view->doc, view->current_page);
        if (page) {
            copied = poppler_page_get_text(page);
            g_object_unref(page);
        }
    }

    if (!copied || !*copied)
        return FALSE;

    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    if (!cb)
        return FALSE;
    gdk_clipboard_set_text(cb, copied);
    return TRUE;
}
