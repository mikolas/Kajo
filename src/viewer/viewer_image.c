#include "viewer_image.h"

#include <cairo.h>
#include <math.h>
#include <string.h>

#ifdef HAVE_GLYCIN
#include <glycin-gtk4.h>
#endif

#ifdef HAVE_LIBRSVG
#include <librsvg/rsvg.h>
#endif

#ifdef HAVE_LIBEXIF
#include <libexif/exif-data.h>
#endif

struct _ViewerImageView {
    ViewerImageStateCb state_cb;
    gpointer           user_data;

    GtkWidget         *root_box;
    GtkWidget         *canvas;
    GtkWidget         *filmstrip_scroll;
    GtkWidget         *filmstrip_box;

    char              *current_path;
    GPtrArray         *siblings;
    int                sibling_idx;
    GPtrArray         *film_buttons;

    /* Loaded image backing */
    GdkTexture        *texture;
    cairo_surface_t   *cairo_surf;
    cairo_pattern_t   *checker_pattern;
#ifdef HAVE_LIBRSVG
    RsvgHandle        *rsvg;
    double             svg_w;
    double             svg_h;
#endif
#ifdef HAVE_GLYCIN
    GlyImage          *gly_image;
#endif

    double             zoom;
    ViewerFitMode      fit_mode;
    double             pan_x;
    double             pan_y;
    int                rotation_deg;
    gboolean           flip_h;
    gboolean           flip_v;
    gboolean           checkerboard;
    gboolean           filmstrip_visible;

    double             drag_start_pan_x;
    double             drag_start_pan_y;

    guint              anim_timer_id;
    gboolean           anim_playing;
    GCancellable      *load_cancel;

    ViewerImageMeta    meta;
};

static void load_internal_file(ViewerImageView *view, GFile *file, gboolean rescan_siblings);

static void
notify_state(ViewerImageView *view)
{
    if (!view)
        return;
    view->meta.zoom_pct = (int)(view->zoom * 100.0 + 0.5);
    view->meta.rotation_deg = view->rotation_deg;
    view->meta.flip_h = view->flip_h;
    view->meta.flip_v = view->flip_v;
    view->meta.anim_playing = view->anim_playing;
    if (view->state_cb)
        view->state_cb(view->user_data);
}

static cairo_surface_t *
texture_to_cairo_surface(GdkTexture *tex)
{
    if (!tex)
        return NULL;
    int w = gdk_texture_get_width(tex);
    int h = gdk_texture_get_height(tex);
    if (w <= 0 || h <= 0)
        return NULL;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_surface_flush(surf);
    gdk_texture_download(tex, cairo_image_surface_get_data(surf),
                         (gsize)cairo_image_surface_get_stride(surf));
    cairo_surface_mark_dirty(surf);
    return surf;
}

static void
stop_anim_timer(ViewerImageView *view)
{
    if (!view)
        return;
    g_clear_handle_id(&view->anim_timer_id, g_source_remove);
}

#ifdef HAVE_GLYCIN
static void schedule_next_anim_frame(ViewerImageView *view, int delay_ms);

static gboolean
on_anim_frame_tick(gpointer user_data)
{
    ViewerImageView *view = user_data;
    view->anim_timer_id = 0;
    if (!view->gly_image || !view->anim_playing || !gtk_widget_get_mapped(view->canvas))
        return G_SOURCE_REMOVE;

    g_autoptr(GlyFrame) frame = gly_image_next_frame(view->gly_image, NULL);
    if (!frame) {
        view->meta.is_animated = FALSE;
        notify_state(view);
        return G_SOURCE_REMOVE;
    }

    g_autoptr(GdkTexture) tex = gly_gtk_frame_get_texture(frame);
    if (tex) {
        g_set_object(&view->texture, tex);
        g_clear_pointer(&view->cairo_surf, cairo_surface_destroy);
        view->cairo_surf = texture_to_cairo_surface(tex);
        gtk_widget_queue_draw(view->canvas);
    }

    int64_t delay_us = gly_frame_get_delay(frame);
    int delay_ms = (delay_us > 0) ? MAX(16, (int)(delay_us / 1000)) : 80;
    schedule_next_anim_frame(view, delay_ms);
    return G_SOURCE_REMOVE;
}

static void
schedule_next_anim_frame(ViewerImageView *view, int delay_ms)
{
    stop_anim_timer(view);
    if (view->gly_image && view->anim_playing && gtk_widget_get_mapped(view->canvas)) {
        view->anim_timer_id = g_timeout_add((guint)delay_ms, on_anim_frame_tick, view);
    }
}
#endif

static void
clear_image_payload(ViewerImageView *view)
{
    stop_anim_timer(view);
    if (view->load_cancel) {
        g_cancellable_cancel(view->load_cancel);
        g_clear_object(&view->load_cancel);
    }
    g_clear_object(&view->texture);
    g_clear_pointer(&view->cairo_surf, cairo_surface_destroy);
#ifdef HAVE_LIBRSVG
    g_clear_object(&view->rsvg);
    view->svg_w = 0.0;
    view->svg_h = 0.0;
#endif
#ifdef HAVE_GLYCIN
    g_clear_object(&view->gly_image);
#endif
    memset(&view->meta, 0, sizeof(view->meta));
    view->anim_playing = FALSE;
}

static void
recompute_fit_zoom(ViewerImageView *view)
{
    if (!view || (view->fit_mode != VIEWER_FIT_CONTAIN &&
                  view->fit_mode != VIEWER_FIT_FILL &&
                  view->fit_mode != VIEWER_FIT_ACTUAL)) {
        return;
    }

    double cw = 0.0, ch = 0.0;
#ifdef HAVE_LIBRSVG
    if (view->rsvg) {
        cw = view->svg_w;
        ch = view->svg_h;
    }
#endif
    if (cw <= 0.0 && view->texture) {
        cw = (double)gdk_texture_get_width(view->texture);
        ch = (double)gdk_texture_get_height(view->texture);
    }
    if (cw <= 0.0 || ch <= 0.0)
        return;

    if (view->rotation_deg == 90 || view->rotation_deg == 270) {
        double tmp = cw;
        cw = ch;
        ch = tmp;
    }

    if (view->fit_mode == VIEWER_FIT_ACTUAL) {
        view->zoom = 1.0;
        view->pan_x = 0.0;
        view->pan_y = 0.0;
        return;
    }

    int vw = gtk_widget_get_width(view->canvas);
    int vh = gtk_widget_get_height(view->canvas);
    if (vw <= 40) vw = 900;
    if (vh <= 40) vh = 600;
    view->zoom = viewer_model_compute_fit_scale(
        cw, ch, MAX(80.0, (double)vw - 24.0), MAX(80.0, (double)vh - 24.0), view->fit_mode);
    view->pan_x = 0.0;
    view->pan_y = 0.0;
}

static void
ensure_checker_pattern(ViewerImageView *view)
{
    if (view->checker_pattern)
        return;
    cairo_surface_t *pat_s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 16, 16);
    cairo_t *cr = cairo_create(pat_s);
    cairo_set_source_rgb(cr, 0.133, 0.133, 0.133);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.188, 0.188, 0.188);
    cairo_rectangle(cr, 0, 0, 8, 8);
    cairo_rectangle(cr, 8, 8, 8, 8);
    cairo_fill(cr);
    cairo_destroy(cr);

    view->checker_pattern = cairo_pattern_create_for_surface(pat_s);
    cairo_pattern_set_extend(view->checker_pattern, CAIRO_EXTEND_REPEAT);
    cairo_surface_destroy(pat_s);
}

static void
on_image_draw(GtkDrawingArea *area G_GNUC_UNUSED,
              cairo_t        *cr,
              int             width,
              int             height,
              gpointer        user_data)
{
    ViewerImageView *view = user_data;
    if (!view)
        return;

    cairo_set_source_rgb(cr, 0.063, 0.063, 0.063); /* #101010 recessed canvas */
    cairo_paint(cr);

    double iw = 0.0, ih = 0.0;
    gboolean is_svg = FALSE;
#ifdef HAVE_LIBRSVG
    if (view->rsvg) {
        iw = view->svg_w;
        ih = view->svg_h;
        is_svg = TRUE;
    }
#endif
    if (!is_svg && view->cairo_surf) {
        iw = (double)cairo_image_surface_get_width(view->cairo_surf);
        ih = (double)cairo_image_surface_get_height(view->cairo_surf);
    }
    if (iw <= 0.0 || ih <= 0.0)
        return;

    if (view->fit_mode == VIEWER_FIT_CONTAIN || view->fit_mode == VIEWER_FIT_FILL) {
        recompute_fit_zoom(view);
    }

    double draw_w = iw * view->zoom;
    double draw_h = ih * view->zoom;

    cairo_save(cr);
    cairo_translate(cr, (double)width * 0.5 + view->pan_x, (double)height * 0.5 + view->pan_y);
    if (view->rotation_deg != 0)
        cairo_rotate(cr, (double)view->rotation_deg * G_PI / 180.0);
    cairo_scale(cr, view->flip_h ? -1.0 : 1.0, view->flip_v ? -1.0 : 1.0);

    if (view->checkerboard) {
        ensure_checker_pattern(view);
        cairo_save(cr);
        cairo_set_source(cr, view->checker_pattern);
        cairo_rectangle(cr, -draw_w * 0.5, -draw_h * 0.5, draw_w, draw_h);
        cairo_fill(cr);
        cairo_restore(cr);
    }

#ifdef HAVE_LIBRSVG
    if (is_svg && view->rsvg) {
        RsvgRectangle vp = {
            .x = -draw_w * 0.5,
            .y = -draw_h * 0.5,
            .width = draw_w,
            .height = draw_h,
        };
        rsvg_handle_render_document(view->rsvg, cr, &vp, NULL);
        cairo_restore(cr);
        return;
    }
#endif

    if (view->cairo_surf) {
        cairo_save(cr);
        cairo_scale(cr, view->zoom, view->zoom);
        cairo_set_source_surface(cr, view->cairo_surf, -iw * 0.5, -ih * 0.5);
        cairo_pattern_t *pat = cairo_get_source(cr);
        cairo_pattern_set_filter(pat,
                                 view->zoom >= 3.0 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    cairo_restore(cr);
}

#ifdef HAVE_LIBEXIF
static void
exif_read_tag(ExifData *ed, ExifIfd ifd, ExifTag tag, char *buf, size_t len)
{
    ExifEntry *e = exif_content_get_entry(ed->ifd[ifd], tag);
    if (e) {
        exif_entry_get_value(e, buf, (unsigned int)len);
        g_strstrip(buf);
    }
}

static void
populate_exif_metadata(const char *path, ViewerImageMeta *meta)
{
    if (!path || !meta)
        return;
    ExifData *ed = exif_data_new_from_file(path);
    if (!ed)
        return;

    char make[48] = {0}, model[64] = {0};
    exif_read_tag(ed, EXIF_IFD_0, EXIF_TAG_MAKE, make, sizeof(make));
    exif_read_tag(ed, EXIF_IFD_0, EXIF_TAG_MODEL, model, sizeof(model));
    if (make[0] && model[0]) {
        g_snprintf(meta->camera, sizeof(meta->camera), "%s %s", make, model);
    } else if (model[0]) {
        g_strlcpy(meta->camera, model, sizeof(meta->camera));
    }

    char fnum[24] = {0}, exp_t[24] = {0}, iso[24] = {0}, fl[24] = {0};
    exif_read_tag(ed, EXIF_IFD_EXIF, EXIF_TAG_FNUMBER, fnum, sizeof(fnum));
    exif_read_tag(ed, EXIF_IFD_EXIF, EXIF_TAG_EXPOSURE_TIME, exp_t, sizeof(exp_t));
    exif_read_tag(ed, EXIF_IFD_EXIF, EXIF_TAG_ISO_SPEED_RATINGS, iso, sizeof(iso));
    exif_read_tag(ed, EXIF_IFD_EXIF, EXIF_TAG_FOCAL_LENGTH, fl, sizeof(fl));

    GString *ex = g_string_new("");
    if (fl[0]) g_string_append(ex, fl);
    if (fnum[0]) g_string_append_printf(ex, "%s%s", ex->len ? " · " : "", fnum);
    if (exp_t[0]) g_string_append_printf(ex, "%s%s", ex->len ? " · " : "", exp_t);
    if (iso[0]) g_string_append_printf(ex, "%sISO %s", ex->len ? " · " : "", iso);
    if (ex->len > 0)
        g_strlcpy(meta->exposure, ex->str, sizeof(meta->exposure));
    g_string_free(ex, TRUE);

    exif_read_tag(ed, EXIF_IFD_EXIF, EXIF_TAG_DATE_TIME_ORIGINAL, meta->date_str, sizeof(meta->date_str));
    exif_read_tag(ed, EXIF_IFD_EXIF, EXIF_TAG_COLOR_SPACE, meta->colorspace, sizeof(meta->colorspace));
    exif_data_unref(ed);
}
#endif

static void
on_film_thumb_clicked(GtkButton *btn, gpointer user_data)
{
    ViewerImageView *view = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "sib-idx"));
    if (!view || idx < 0 || idx >= (int)view->siblings->len)
        return;
    view->sibling_idx = idx;
    const char *p = g_ptr_array_index(view->siblings, (guint)idx);
    g_autoptr(GFile) f = g_file_new_for_path(p);
    load_internal_file(view, f, FALSE);
}

static void
rebuild_filmstrip(ViewerImageView *view)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(view->filmstrip_box)) != NULL)
        gtk_box_remove(GTK_BOX(view->filmstrip_box), child);
    g_ptr_array_set_size(view->film_buttons, 0);

    if (!view->siblings)
        return;

    for (guint i = 0; i < view->siblings->len; i++) {
        const char *p = g_ptr_array_index(view->siblings, i);
        g_autofree char *base = g_path_get_basename(p);
        GtkWidget *btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "viewer-film-thumb");
        if ((int)i == view->sibling_idx)
            gtk_widget_add_css_class(btn, "active");
        gtk_widget_set_can_focus(btn, FALSE);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        ViewerDocKind k = viewer_model_classify_path(p, NULL);
        GtkWidget *klbl = gtk_label_new(k == VIEWER_KIND_SVG ? "SVG" : "IMG");
        gtk_widget_add_css_class(klbl, "viewer-film-kind");
        gtk_label_set_xalign(GTK_LABEL(klbl), 0.0f);

        GtkWidget *nlbl = gtk_label_new(base);
        gtk_label_set_xalign(GTK_LABEL(nlbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(nlbl), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(nlbl), 14);

        gtk_box_append(GTK_BOX(vbox), klbl);
        gtk_box_append(GTK_BOX(vbox), nlbl);
        gtk_button_set_child(GTK_BUTTON(btn), vbox);
        g_object_set_data(G_OBJECT(btn), "sib-idx", GINT_TO_POINTER((int)i));
        g_signal_connect(btn, "clicked", G_CALLBACK(on_film_thumb_clicked), view);
        gtk_box_append(GTK_BOX(view->filmstrip_box), btn);
        g_ptr_array_add(view->film_buttons, btn);
    }
}

typedef struct {
    GWeakRef    canvas_ref;
    GFile      *file;
    char       *path;
    GdkTexture *texture;
    guint64     file_size;
    char        mime_type[64];
    char        decoder[64];
#ifdef HAVE_GLYCIN
    GlyImage   *gly_image;
    int64_t     first_delay_us;
#endif
} ImageLoadData;

static void
image_load_data_free(gpointer p)
{
    ImageLoadData *d = p;
    if (!d)
        return;
    g_weak_ref_clear(&d->canvas_ref);
    g_clear_object(&d->file);
    g_free(d->path);
    g_clear_object(&d->texture);
#ifdef HAVE_GLYCIN
    g_clear_object(&d->gly_image);
#endif
    g_free(d);
}

static void
image_load_worker(GTask        *task,
                  gpointer      source_object G_GNUC_UNUSED,
                  gpointer      task_data,
                  GCancellable *cancellable)
{
    ImageLoadData *d = task_data;
    if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_boolean(task, FALSE);
        return;
    }

    g_autoptr(GFileInfo) info = g_file_query_info(
        d->file, G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
        G_FILE_QUERY_INFO_NONE, cancellable, NULL);
    if (info) {
        d->file_size = (guint64)g_file_info_get_size(info);
        const char *ct = g_file_info_get_content_type(info);
        if (ct)
            g_strlcpy(d->mime_type, ct, sizeof(d->mime_type));
    }

#ifdef HAVE_GLYCIN
    GlyLoader *loader = gly_loader_new(d->file);
    if (loader) {
        g_autoptr(GlyImage) gimg = gly_loader_load(loader, NULL);
        g_object_unref(loader);
        if (gimg) {
            g_autoptr(GlyFrame) frame = gly_image_next_frame(gimg, NULL);
            if (frame) {
                d->texture = gly_gtk_frame_get_texture(frame);
                d->first_delay_us = gly_frame_get_delay(frame);
                const char *gm = gly_image_get_mime_type(gimg);
                if (gm && *gm)
                    g_strlcpy(d->mime_type, gm, sizeof(d->mime_type));
                g_strlcpy(d->decoder, "GLYCIN", sizeof(d->decoder));
                if (d->first_delay_us > 0)
                    d->gly_image = g_object_ref(gimg);
            }
        }
    }
#endif

    if (!d->texture) {
        d->texture = gdk_texture_new_from_file(d->file, NULL);
        if (d->texture)
            g_strlcpy(d->decoder, "GTK4", sizeof(d->decoder));
    }

    g_task_return_boolean(task, d->texture != NULL);
}

static void
on_image_load_done(GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    ImageLoadData *d = user_data;
    GtkWidget *canvas = g_weak_ref_get(&d->canvas_ref);
    if (!canvas)
        return;

    ViewerImageView *view = g_object_get_data(G_OBJECT(canvas), "viewer-image-self");
    gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);
    if (!view || !ok || !d->texture) {
        g_object_unref(canvas);
        return;
    }

    if (g_strcmp0(d->path, view->current_path) != 0) {
        g_object_unref(canvas);
        return;
    }

    g_set_object(&view->texture, d->texture);
    g_clear_pointer(&view->cairo_surf, cairo_surface_destroy);
    view->cairo_surf = texture_to_cairo_surface(d->texture);

    g_strlcpy(view->meta.decoder, d->decoder, sizeof(view->meta.decoder));
    g_strlcpy(view->meta.mime_type, d->mime_type, sizeof(view->meta.mime_type));
    view->meta.width = gdk_texture_get_width(d->texture);
    view->meta.height = gdk_texture_get_height(d->texture);
    view->meta.file_size = d->file_size;

#ifdef HAVE_LIBEXIF
    if (d->path)
        populate_exif_metadata(d->path, &view->meta);
#endif

#ifdef HAVE_GLYCIN
    if (d->gly_image && d->first_delay_us > 0) {
        g_set_object(&view->gly_image, d->gly_image);
        view->meta.is_animated = TRUE;
        view->anim_playing = TRUE;
        schedule_next_anim_frame(view, MAX(16, (int)(d->first_delay_us / 1000)));
    }
#endif

    view->fit_mode = VIEWER_FIT_CONTAIN;
    recompute_fit_zoom(view);
    gtk_widget_queue_draw(view->canvas);
    notify_state(view);
    g_object_unref(canvas);
}

static void
load_internal_file(ViewerImageView *view, GFile *file, gboolean rescan_siblings)
{
    if (!view || !file)
        return;

    clear_image_payload(view);
    g_free(view->current_path);
    view->current_path = g_file_get_path(file);
    view->rotation_deg = 0;
    view->flip_h = FALSE;
    view->flip_v = FALSE;
    view->pan_x = 0.0;
    view->pan_y = 0.0;
    view->fit_mode = VIEWER_FIT_CONTAIN;

    if (rescan_siblings && view->current_path) {
        if (view->siblings)
            g_ptr_array_unref(view->siblings);
        view->siblings = viewer_model_scan_sibling_images(view->current_path, &view->sibling_idx);
        rebuild_filmstrip(view);
    } else if (view->film_buttons) {
        for (guint i = 0; i < view->film_buttons->len; i++) {
            GtkWidget *b = g_ptr_array_index(view->film_buttons, i);
            if ((int)i == view->sibling_idx)
                gtk_widget_add_css_class(b, "active");
            else
                gtk_widget_remove_css_class(b, "active");
        }
    }

    ViewerDocKind kind = viewer_model_classify_path(view->current_path, NULL);
#ifdef HAVE_LIBRSVG
    if (kind == VIEWER_KIND_SVG && view->current_path) {
        RsvgHandle *svg = rsvg_handle_new_from_file(view->current_path, NULL);
        if (svg) {
            view->rsvg = svg;
            double sw = 0.0, sh = 0.0;
            if (!rsvg_handle_get_intrinsic_size_in_pixels(svg, &sw, &sh) || sw <= 1.0 || sh <= 1.0) {
                sw = 800.0;
                sh = 600.0;
            }
            view->svg_w = sw;
            view->svg_h = sh;
            g_strlcpy(view->meta.decoder, "LIBRSVG", sizeof(view->meta.decoder));
            g_strlcpy(view->meta.mime_type, "image/svg+xml", sizeof(view->meta.mime_type));
            g_strlcpy(view->meta.colorspace, "Vector SVG", sizeof(view->meta.colorspace));
            view->meta.width = (int)(sw + 0.5);
            view->meta.height = (int)(sh + 0.5);

            g_autoptr(GFileInfo) fi = g_file_query_info(
                file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (fi)
                view->meta.file_size = (guint64)g_file_info_get_size(fi);

            recompute_fit_zoom(view);
            gtk_widget_queue_draw(view->canvas);
            notify_state(view);
            return;
        }
    }
#else
    (void)kind;
#endif

    view->load_cancel = g_cancellable_new();
    ImageLoadData *d = g_new0(ImageLoadData, 1);
    g_weak_ref_init(&d->canvas_ref, view->canvas);
    d->file = g_object_ref(file);
    d->path = g_strdup(view->current_path);

    GTask *task = g_task_new(NULL, view->load_cancel, on_image_load_done, d);
    g_task_set_task_data(task, d, image_load_data_free);
    g_task_run_in_thread(task, image_load_worker);
    g_object_unref(task);
}

static void
on_drag_begin(GtkGestureDrag *g G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer user_data)
{
    ViewerImageView *view = user_data;
    view->drag_start_pan_x = view->pan_x;
    view->drag_start_pan_y = view->pan_y;
}

static void
on_drag_update(GtkGestureDrag *g G_GNUC_UNUSED, double dx, double dy, gpointer user_data)
{
    ViewerImageView *view = user_data;
    view->fit_mode = VIEWER_FIT_ACTUAL; /* Lock zoom once panned manually */
    view->pan_x = view->drag_start_pan_x + dx;
    view->pan_y = view->drag_start_pan_y + dy;
    gtk_widget_queue_draw(view->canvas);
}

static gboolean
on_canvas_scroll(GtkEventControllerScroll *ctrl G_GNUC_UNUSED,
                 double                    dx G_GNUC_UNUSED,
                 double                    dy,
                 gpointer                  user_data)
{
    ViewerImageView *view = user_data;
    if (dy < 0.0)
        viewer_image_view_step_zoom(view, 1);
    else if (dy > 0.0)
        viewer_image_view_step_zoom(view, -1);
    return TRUE;
}

static void
on_canvas_map(GtkWidget *w G_GNUC_UNUSED, gpointer user_data)
{
#ifdef HAVE_GLYCIN
    ViewerImageView *view = user_data;
    if (view && view->gly_image && view->anim_playing && view->anim_timer_id == 0)
        schedule_next_anim_frame(view, 40);
#else
    (void)user_data;
#endif
}

static void
on_canvas_unmap(GtkWidget *w G_GNUC_UNUSED, gpointer user_data)
{
    stop_anim_timer((ViewerImageView *)user_data);
}

ViewerImageView *
viewer_image_view_new(ViewerImageStateCb state_cb, gpointer user_data)
{
    ViewerImageView *view = g_new0(ViewerImageView, 1);
    view->state_cb = state_cb;
    view->user_data = user_data;
    view->zoom = 1.0;
    view->fit_mode = VIEWER_FIT_CONTAIN;
    view->sibling_idx = -1;
    view->siblings = g_ptr_array_new_with_free_func(g_free);
    view->film_buttons = g_ptr_array_new();

    view->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(view->root_box, TRUE);
    gtk_widget_set_vexpand(view->root_box, TRUE);

    view->canvas = gtk_drawing_area_new();
    gtk_widget_set_hexpand(view->canvas, TRUE);
    gtk_widget_set_vexpand(view->canvas, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(view->canvas), on_image_draw, view, NULL);
    g_object_set_data(G_OBJECT(view->canvas), "viewer-image-self", view);

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), view);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), view);
    gtk_widget_add_controller(view->canvas, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_canvas_scroll), view);
    gtk_widget_add_controller(view->canvas, scroll);

    g_signal_connect(view->canvas, "map", G_CALLBACK(on_canvas_map), view);
    g_signal_connect(view->canvas, "unmap", G_CALLBACK(on_canvas_unmap), view);

    view->filmstrip_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view->filmstrip_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_NEVER);
    gtk_widget_add_css_class(view->filmstrip_scroll, "viewer-filmstrip");
    gtk_widget_set_size_request(view->filmstrip_scroll, -1, 72);
    gtk_widget_set_visible(view->filmstrip_scroll, FALSE);

    view->filmstrip_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(view->filmstrip_box, 10);
    gtk_widget_set_margin_end(view->filmstrip_box, 10);
    gtk_widget_set_margin_top(view->filmstrip_box, 6);
    gtk_widget_set_margin_bottom(view->filmstrip_box, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view->filmstrip_scroll), view->filmstrip_box);

    gtk_box_append(GTK_BOX(view->root_box), view->canvas);
    gtk_box_append(GTK_BOX(view->root_box), view->filmstrip_scroll);
    return view;
}

void
viewer_image_view_free(ViewerImageView *view)
{
    if (!view)
        return;
    if (view->canvas)
        g_object_set_data(G_OBJECT(view->canvas), "viewer-image-self", NULL);
    clear_image_payload(view);
    g_clear_pointer(&view->checker_pattern, cairo_pattern_destroy);
    g_free(view->current_path);
    if (view->siblings)
        g_ptr_array_free(view->siblings, TRUE);
    g_ptr_array_free(view->film_buttons, TRUE);
    g_free(view);
}

GtkWidget *
viewer_image_view_get_widget(ViewerImageView *view)
{
    return view ? view->root_box : NULL;
}

gboolean
viewer_image_view_load_file(ViewerImageView *view, GFile *file)
{
    if (!view || !file)
        return FALSE;
    load_internal_file(view, file, TRUE);
    return TRUE;
}

void
viewer_image_view_load_texture(ViewerImageView *view,
                               GdkTexture      *texture,
                               const char      *virtual_name,
                               const char      *decoder_label)
{
    if (!view || !texture)
        return;
    clear_image_payload(view);
    g_free(view->current_path);
    view->current_path = g_strdup(virtual_name ? virtual_name : "preview.png");
    g_ptr_array_set_size(view->siblings, 0);
    view->sibling_idx = 0;

    view->texture = g_object_ref(texture);
    view->cairo_surf = texture_to_cairo_surface(texture);
    view->meta.width = gdk_texture_get_width(texture);
    view->meta.height = gdk_texture_get_height(texture);
    view->meta.file_size = (guint64)view->meta.width * (guint64)view->meta.height * 4u;
    g_strlcpy(view->meta.decoder, decoder_label ? decoder_label : "PREVIEW", sizeof(view->meta.decoder));
    g_strlcpy(view->meta.mime_type, "image/png", sizeof(view->meta.mime_type));
    g_strlcpy(view->meta.colorspace, "sRGB RGBA", sizeof(view->meta.colorspace));

    view->fit_mode = VIEWER_FIT_CONTAIN;
    recompute_fit_zoom(view);
    gtk_widget_queue_draw(view->canvas);
    notify_state(view);
}

void
viewer_image_view_set_fit_mode(ViewerImageView *view, ViewerFitMode mode)
{
    if (!view)
        return;
    view->fit_mode = mode;
    recompute_fit_zoom(view);
    gtk_widget_queue_draw(view->canvas);
    notify_state(view);
}

void
viewer_image_view_step_zoom(ViewerImageView *view, int direction)
{
    if (!view)
        return;
    view->fit_mode = VIEWER_FIT_ACTUAL;
    view->zoom = viewer_model_step_zoom(view->zoom, direction, 0.1, 16.0);
    gtk_widget_queue_draw(view->canvas);
    notify_state(view);
}

double
viewer_image_view_get_zoom(const ViewerImageView *view)
{
    return view ? view->zoom : 1.0;
}

ViewerFitMode
viewer_image_view_get_fit_mode(const ViewerImageView *view)
{
    return view ? view->fit_mode : VIEWER_FIT_CONTAIN;
}

void
viewer_image_view_rotate(ViewerImageView *view, int delta_deg)
{
    if (!view)
        return;
    view->rotation_deg = (((view->rotation_deg + delta_deg) % 360) + 360) % 360;
    recompute_fit_zoom(view);
    gtk_widget_queue_draw(view->canvas);
    notify_state(view);
}

void
viewer_image_view_flip(ViewerImageView *view, gboolean horizontal)
{
    if (!view)
        return;
    if (horizontal)
        view->flip_h = !view->flip_h;
    else
        view->flip_v = !view->flip_v;
    gtk_widget_queue_draw(view->canvas);
    notify_state(view);
}

void
viewer_image_view_toggle_checkerboard(ViewerImageView *view)
{
    if (!view)
        return;
    view->checkerboard = !view->checkerboard;
    gtk_widget_queue_draw(view->canvas);
    notify_state(view);
}

gboolean
viewer_image_view_get_checkerboard(const ViewerImageView *view)
{
    return view ? view->checkerboard : FALSE;
}

void
viewer_image_view_toggle_filmstrip(ViewerImageView *view)
{
    if (!view)
        return;
    view->filmstrip_visible = !view->filmstrip_visible;
    gtk_widget_set_visible(view->filmstrip_scroll, view->filmstrip_visible);
    notify_state(view);
}

gboolean
viewer_image_view_get_filmstrip(const ViewerImageView *view)
{
    return view ? view->filmstrip_visible : FALSE;
}

void
viewer_image_view_toggle_animation(ViewerImageView *view)
{
    if (!view || !view->meta.is_animated)
        return;
    view->anim_playing = !view->anim_playing;
    if (!view->anim_playing) {
        stop_anim_timer(view);
    } else {
#ifdef HAVE_GLYCIN
        schedule_next_anim_frame(view, 40);
#endif
    }
    notify_state(view);
}

void
viewer_image_view_step_sibling(ViewerImageView *view, int delta)
{
    if (!view || !view->siblings || view->siblings->len <= 1)
        return;
    int len = (int)view->siblings->len;
    int cur = view->sibling_idx >= 0 ? view->sibling_idx : 0;
    view->sibling_idx = (((cur + delta) % len) + len) % len;
    const char *p = g_ptr_array_index(view->siblings, (guint)view->sibling_idx);
    g_autoptr(GFile) f = g_file_new_for_path(p);
    load_internal_file(view, f, FALSE);
}

int
viewer_image_view_get_sibling_count(const ViewerImageView *view)
{
    return (view && view->siblings) ? (int)view->siblings->len : 0;
}

int
viewer_image_view_get_sibling_index(const ViewerImageView *view)
{
    return view ? view->sibling_idx : -1;
}

const char *
viewer_image_view_get_current_path(const ViewerImageView *view)
{
    return view ? view->current_path : NULL;
}

gboolean
viewer_image_view_copy_to_clipboard(ViewerImageView *view)
{
    if (!view)
        return FALSE;

    GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
    if (!cb)
        return FALSE;

    if (view->texture) {
        gdk_clipboard_set_texture(cb, view->texture);
        return TRUE;
    }

#ifdef HAVE_LIBRSVG
    if (view->rsvg && view->svg_w > 1.0 && view->svg_h > 1.0) {
        int w = MAX(1, (int)(view->svg_w + 0.5));
        int h = MAX(1, (int)(view->svg_h + 0.5));
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(s);
        RsvgRectangle vp = { 0.0, 0.0, (double)w, (double)h };
        rsvg_handle_render_document(view->rsvg, cr, &vp, NULL);
        cairo_destroy(cr);
        cairo_surface_flush(s);

        int stride = cairo_image_surface_get_stride(s);
        g_autoptr(GBytes) bytes = g_bytes_new(cairo_image_surface_get_data(s),
                                              (gsize)stride * (gsize)h);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
        GdkMemoryFormat fmt = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
#else
        GdkMemoryFormat fmt = GDK_MEMORY_A8R8G8B8_PREMULTIPLIED;
#endif
        g_autoptr(GdkTexture) tex = gdk_memory_texture_new(w, h, fmt, bytes, (gsize)stride);
        cairo_surface_destroy(s);
        if (tex) {
            gdk_clipboard_set_texture(cb, tex);
            return TRUE;
        }
    }
#endif
    return FALSE;
}

const ViewerImageMeta *
viewer_image_view_get_meta(const ViewerImageView *view)
{
    return view ? &view->meta : NULL;
}