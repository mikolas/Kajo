#include "mod_vis.h"
#include "../config.h"
#include "../theme.h"
#include <math.h>
#include <string.h>
#include <pango/pangocairo.h>

typedef struct {
    gboolean has_live_pcm;
    int      n_pcm;
    float    pcm_l[KAJO_MOD_SCOPE_SAMPLES];
    float    pcm_r[KAJO_MOD_SCOPE_SAMPLES];
    gboolean has_live_fft;
    float    fft_bands[KAJO_MOD_FFT_BANDS];
    float    fft_peaks[KAJO_MOD_FFT_BANDS];
} KajoModVisState;

static void
vis_state_free(gpointer data)
{
    g_free(data);
}

static void
draw_vis_header(cairo_t     *cr,
                GtkWidget   *widget,
                int          width,
                const gchar *left_text,
                const gchar *right_text,
                double       right_r,
                double       right_g,
                double       right_b)
{
    g_autofree gchar *font_stack = shell_ide_font_stack("Fira Code");
    g_autoptr(PangoFontDescription) desc = pango_font_description_new();
    pango_font_description_set_family(desc, font_stack);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_size(desc, (int)(9.5 * 0.75 * PANGO_SCALE));

    g_autoptr(PangoLayout) layout = gtk_widget_create_pango_layout(widget, left_text);
    pango_layout_set_font_description(layout, desc);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout, &lw, &lh);

    cairo_set_source_rgb(cr, 0.62, 0.62, 0.62);
    cairo_move_to(cr, 6.0, 3.0);
    pango_cairo_show_layout(cr, layout);

    pango_layout_set_text(layout, right_text, -1);
    int rw = 0, rh = 0;
    pango_layout_get_pixel_size(layout, &rw, &rh);
    if (width - rw - 6 > lw + 12) {
        cairo_set_source_rgb(cr, right_r, right_g, right_b);
        cairo_move_to(cr, (double)(width - rw - 6), 3.0);
        pango_cairo_show_layout(cr, layout);
    }
}

static void
on_draw_scope(GtkDrawingArea *area,
              cairo_t        *cr,
              int             width,
              int             height,
              gpointer        user_data)
{
    KajoModVisState *st = (KajoModVisState *)user_data;
    if (width <= 8 || height <= 16) return;

    /* Background */
    cairo_set_source_rgb(cr, 0.047, 0.047, 0.047);
    cairo_paint(cr);

    draw_vis_header(cr, GTK_WIDGET(area), width,
                    "STEREO OSCILLOSCOPE (L / R)",
                    "512 SMP • 60 FPS",
                    0.0, 0.847, 0.941);

    const double top_pad = 18.0;
    const double plot_h = (double)height - top_pad - 4.0;
    const double mid_y = top_pad + plot_h * 0.5;

    /* Center graticule dashed line */
    const double dashes[] = { 3.0, 3.0 };
    cairo_set_dash(cr, dashes, 2, 0.0);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, 0.14, 0.14, 0.14);
    cairo_move_to(cr, 4.0, mid_y);
    cairo_line_to(cr, (double)(width - 4), mid_y);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0.0);

    const double usable_w = (double)(width - 12);
    const double amp = plot_h * 0.42;

    if (st && st->has_live_pcm && st->n_pcm >= 16) {
        int n_pts = st->n_pcm - 1;
        /* Right Channel Trace (@metro-telemetry #00d8f0) */
        cairo_set_source_rgba(cr, 0.0, 0.847, 0.941, 0.82);
        cairo_set_line_width(cr, 1.35);
        for (int i = 0; i <= n_pts; i++) {
            double t = (double)i / (double)n_pts;
            double x = 6.0 + t * usable_w;
            double v = CLAMP((double)st->pcm_r[i], -1.0, 1.0);
            double y = mid_y + v * amp;
            if (i == 0) cairo_move_to(cr, x, y);
            else        cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);

        /* Left Channel Trace (@metro-accent #0078d4) */
        cairo_set_source_rgb(cr, 0.0, 0.471, 0.831);
        cairo_set_line_width(cr, 1.8);
        for (int i = 0; i <= n_pts; i++) {
            double t = (double)i / (double)n_pts;
            double x = 6.0 + t * usable_w;
            double v = CLAMP((double)st->pcm_l[i], -1.0, 1.0);
            double y = mid_y - v * amp;
            if (i == 0) cairo_move_to(cr, x, y);
            else        cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
        return;
    }

    /* Idle flat baseline trace */
    cairo_set_source_rgb(cr, 0.0, 0.471, 0.831);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, 6.0, mid_y);
    cairo_line_to(cr, 6.0 + usable_w, mid_y);
    cairo_stroke(cr);
}

static void
on_draw_fft(GtkDrawingArea *area,
            cairo_t        *cr,
            int             width,
            int             height,
            gpointer        user_data)
{
    KajoModVisState *st = (KajoModVisState *)user_data;
    if (width <= 8 || height <= 16) return;

    cairo_set_source_rgb(cr, 0.047, 0.047, 0.047);
    cairo_paint(cr);

    draw_vis_header(cr, GTK_WIDGET(area), width,
                    "16-BAND LOG FFT SPECTRUM",
                    "PEAK HOLD • -60..0 dB",
                    0.063, 0.537, 0.243);

    const double top_pad = 19.0;
    const double bot_pad = 4.0;
    const double max_h = (double)height - top_pad - bot_pad;
    const double usable_w = (double)(width - 12);
    const double gap = 2.0;
    const double bar_w = (usable_w - gap * 15.0) / 16.0;
    if (bar_w < 2.0) return;

    for (int i = 0; i < 16; i++) {
        double level = 0.03;
        double peak  = 0.03;
        if (st && st->has_live_fft) {
            level = CLAMP((double)st->fft_bands[i], 0.03, 1.0);
            peak  = CLAMP((double)st->fft_peaks[i], level, 1.0);
        }
        double bh = fmax(2.0, level * max_h);
        double ph_y = (double)height - bot_pad - fmax(2.0, peak * max_h);
        double bx = 6.0 + (double)i * (bar_w + gap);
        double by = (double)height - bot_pad - bh;

        cairo_pattern_t *pat = cairo_pattern_create_linear(bx, by, bx, by + bh);
        cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.0, 0.847, 0.941);   /* @metro-telemetry */
        cairo_pattern_add_color_stop_rgb(pat, 0.45, 0.0, 0.471, 0.831);  /* @metro-accent */
        cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.063, 0.537, 0.243); /* @metro-success */

        cairo_set_source(cr, pat);
        cairo_rectangle(cr, bx, by, bar_w, bh);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);

        /* Peak-hold cap line */
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_rectangle(cr, bx, ph_y, bar_w, 2.0);
        cairo_fill(cr);
    }
}

GtkWidget *
kajo_mod_scope_new(void)
{
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_add_css_class(area, "mod-vis-card");
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, FALSE);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 78);

    KajoModVisState *st = g_new0(KajoModVisState, 1);
    g_object_set_data_full(G_OBJECT(area), "vis-state", st, vis_state_free);

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), on_draw_scope, st, NULL);
    return area;
}

void
kajo_mod_scope_clear(GtkWidget *scope)
{
    if (!GTK_IS_DRAWING_AREA(scope)) return;
    KajoModVisState *st = g_object_get_data(G_OBJECT(scope), "vis-state");
    if (!st) return;
    memset(st, 0, sizeof(*st));
    gtk_widget_queue_draw(scope);
}

void
kajo_mod_scope_set_pcm(GtkWidget *scope, const float *left, const float *right, int n_samples)
{
    if (!GTK_IS_DRAWING_AREA(scope) || !left || !right || n_samples <= 0) return;
    KajoModVisState *st = g_object_get_data(G_OBJECT(scope), "vis-state");
    if (!st) return;
    int count = MIN(n_samples, KAJO_MOD_SCOPE_SAMPLES);
    memcpy(st->pcm_l, left, (size_t)count * sizeof(float));
    memcpy(st->pcm_r, right, (size_t)count * sizeof(float));
    st->n_pcm        = count;
    st->has_live_pcm = TRUE;
    gtk_widget_queue_draw(scope);
}

GtkWidget *
kajo_mod_fft_new(void)
{
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_add_css_class(area, "mod-vis-card");
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, FALSE);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 78);

    KajoModVisState *st = g_new0(KajoModVisState, 1);
    g_object_set_data_full(G_OBJECT(area), "vis-state", st, vis_state_free);

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), on_draw_fft, st, NULL);
    return area;
}

void
kajo_mod_fft_clear(GtkWidget *fft)
{
    if (!GTK_IS_DRAWING_AREA(fft)) return;
    KajoModVisState *st = g_object_get_data(G_OBJECT(fft), "vis-state");
    if (!st) return;
    memset(st, 0, sizeof(*st));
    gtk_widget_queue_draw(fft);
}

void
kajo_mod_fft_set_bands(GtkWidget *fft, const float bands[KAJO_MOD_FFT_BANDS], const float peaks[KAJO_MOD_FFT_BANDS])
{
    if (!GTK_IS_DRAWING_AREA(fft) || !bands) return;
    KajoModVisState *st = g_object_get_data(G_OBJECT(fft), "vis-state");
    if (!st) return;
    memcpy(st->fft_bands, bands, sizeof(st->fft_bands));
    if (peaks) {
        memcpy(st->fft_peaks, peaks, sizeof(st->fft_peaks));
    } else {
        memcpy(st->fft_peaks, bands, sizeof(st->fft_peaks));
    }
    st->has_live_fft = TRUE;
    gtk_widget_queue_draw(fft);
}
