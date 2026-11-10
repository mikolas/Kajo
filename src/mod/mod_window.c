#include "mod_window.h"
#include "mod_engine.h"
#include "mod_audio.h"
#include "mod_playlist.h"
#include "../config.h"
#include "../compositor/compositor.h"
#include <string.h>

struct _KajoModWindow {
    GtkApplicationWindow parent_instance;

    GtkWidget *root_vbox;
    int        sim_col_width;

    /* Backend Audio, Playlist & DSP State */
    KajoModAudio     *audio;
    KajoModPlaylist  *playlist;
    KajoModDspConfig  dsp;
    int               active_pl_index;
    guint             tick_id;
    gboolean          updating_seek;
    gboolean          solo_ch[4];

    /* Header & Telemetry */
    GtkWidget *hdr_engine_badge;
    GtkWidget *lbl_title;
    GtkWidget *lbl_sub;
    GtkWidget *bdg_engine;
    GtkWidget *bdg_format;
    GtkWidget *bdg_chip;

    /* 6-Column Homogeneous Counter Strip (reflows to 3x2 when <760px) */
    GtkWidget *cnt_grid;
    GtkWidget *cnt_cells[6];
    GtkWidget *cnt_sub;
    GtkWidget *cnt_voices;
    GtkWidget *cnt_timing;
    GtkWidget *cnt_clock;
    GtkWidget *cnt_time;

    /* Transport & Subsong */
    GtkWidget *lbl_t0;
    GtkWidget *lbl_t1;
    GtkWidget *seek_scale;
    GtkWidget *btn_play;
    GtkWidget *lbl_sub_readout;
    GtkWidget *lbl_vol_readout;
    int        sub_cur;
    int        sub_max;
    gboolean   playing;

    /* DSP Strip */
    GtkWidget    *loop_seg;
    GtkWidget    *grp_filter;
    GtkWidget    *grp_led;
    GtkWidget    *grp_sid;
    GtkWidget    *grp_tempo;
    GtkWidget    *scale_sep;
    GtkWidget    *lbl_sep_readout;
    GtkWidget    *lbl_tempo_readout;
    GtkWidget    *scale_tempo;
    guint         cfg_sub_id;

    /* Voice Header & Scope/FFT */
    GtkWidget *ch_grid;
    GtkWidget *ch_pills[4];
    GtkWidget *ch_names[4];
    GtkWidget *ch_vus[4];
    GtkWidget *ch_btn_m[4];
    GtkWidget *ch_btn_s[4];
    GtkWidget *scope_area;
    GtkWidget *fft_area;

    /* Zone 6: Inspector & Controlled Niri Column Reflow (<760px vs >=760px) */
    GtkWidget *bdg_col_mode;
    GtkWidget *box_narrow_tabs;
    GtkWidget *btn_tab_samples;
    GtkWidget *btn_tab_stil;
    GtkWidget *col_samples;
    GtkWidget *col_stil;
    GtkWidget *lbl_sample_hdr_left;
    GtkWidget *lbl_sample_hdr_right;
    GtkWidget *lbl_stil_hdr_right;
    GtkWidget *box_sample_rows;
    GtkTextBuffer *stil_buffer;
    gboolean   is_narrow_column;
    gboolean   narrow_show_stil;

    /* Zone 6B: Docked Playlist Queue */
    GtkWidget *lbl_pl_title;
    GtkWidget *pl_listbox;
    GtkWidget *entry_filter;

    /* Zone 7: Status Bar */
    GtkWidget *lbl_status_engine;
    GtkWidget *bdg_mode;
};

G_DEFINE_TYPE(KajoModWindow, kajo_mod_window, GTK_TYPE_APPLICATION_WINDOW)

typedef struct {
    GWeakRef  win_ref;
    GtkWidget *group_box;
    int       group_kind; /* 0 = loop, 1 = filter, 2 = led, 3 = sid */
    int       item_idx;
} SegBtnCtx;

static void
seg_btn_ctx_free(gpointer data, GClosure *closure G_GNUC_UNUSED)
{
    SegBtnCtx *ctx = (SegBtnCtx *)data;
    if (!ctx) return;
    g_weak_ref_clear(&ctx->win_ref);
    g_free(ctx);
}

static GWeakRef *
weak_ref_new(gpointer obj)
{
    GWeakRef *wr = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr, obj);
    return wr;
}

static void
weak_ref_free(gpointer data)
{
    GWeakRef *wr = (GWeakRef *)data;
    if (!wr) return;
    g_weak_ref_clear(wr);
    g_free(wr);
}

static void
set_seg_box_active_index(GtkWidget *seg_box, int active_idx)
{
    if (!seg_box) return;
    int idx = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(seg_box);
         c != NULL;
         c = gtk_widget_get_next_sibling(c)) {
        if (!GTK_IS_BUTTON(c))
            continue;
        if (idx == active_idx)
            gtk_widget_add_css_class(c, "active");
        else
            gtk_widget_remove_css_class(c, "active");
        idx++;
    }
}

static void
set_dsp_grp_active_index(GtkWidget *dsp_grp, int active_idx)
{
    if (!dsp_grp) return;
    GtkWidget *first = gtk_widget_get_first_child(dsp_grp);
    GtkWidget *seg_box = first ? gtk_widget_get_next_sibling(first) : NULL;
    set_seg_box_active_index(seg_box, active_idx);
}

static void
apply_shell_config_to_mod(KajoModWindow *self, gboolean sync_controls)
{
    if (!self) return;
    g_autoptr(KajoModCfgRef) cfg = kajo_config_store_acquire_mod(NULL);
    if (!cfg) return;

    kajo_mod_set_runtime_prefs(
        (double)CLAMP(cfg->uade_timeout_sec, 60, 600),
        cfg->hvsc_path,
        cfg->auto_extract_archives);

    self->dsp.filter_mode    = cfg->filter_mode;
    self->dsp.led_mode       = cfg->led_mode;
    self->dsp.sid_model      = cfg->sid_model;
    self->dsp.end_action     = cfg->end_action;
    self->dsp.stereo_sep_pct = (double)CLAMP(cfg->stereo_sep_pct, 0, 100);

    if (sync_controls) {
        set_seg_box_active_index(self->loop_seg, (int)self->dsp.end_action);
        set_dsp_grp_active_index(self->grp_filter, (int)self->dsp.filter_mode);
        set_dsp_grp_active_index(self->grp_led, (int)self->dsp.led_mode);
        set_dsp_grp_active_index(self->grp_sid, (int)self->dsp.sid_model);
        if (self->scale_sep)
            gtk_range_set_value(GTK_RANGE(self->scale_sep), self->dsp.stereo_sep_pct);
        if (self->audio)
            kajo_mod_audio_apply_dsp(self->audio, &self->dsp);
    }
}

static void
on_mod_config_changed(KajoConfigStore *store G_GNUC_UNUSED,
                      KajoConfigDomain changed_domains G_GNUC_UNUSED,
                      gpointer         user_data)
{
    GWeakRef *wr = (GWeakRef *)user_data;
    g_autoptr(KajoModWindow) self = g_weak_ref_get(wr);
    if (!self) return;
    apply_shell_config_to_mod(self, TRUE);
}

static void activate_playlist_index(KajoModWindow *self, int idx);
static void rebuild_playlist_rows(KajoModWindow *self);

static void
on_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    gtk_window_close(GTK_WINDOW(user_data));
}

static void
on_float_tile_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    niri_toggle_window_floating();
}

static void
update_subsong_labels(KajoModWindow *self)
{
    g_autofree gchar *cnt = g_strdup_printf("%02d/%02d", self->sub_cur, self->sub_max);
    g_autofree gchar *rd  = g_strdup_printf("SUB %02d / %02d", self->sub_cur, self->sub_max);
    gtk_label_set_text(GTK_LABEL(self->cnt_sub), cnt);
    gtk_label_set_text(GTK_LABEL(self->lbl_sub_readout), rd);
}

static void
append_row_label(GtkWidget *row, const char *text, const char *css, int width, float xalign, gboolean hexpand)
{
    GtkWidget *l = gtk_label_new(text ? text : "");
    if (css) gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    if (hexpand) {
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(l, TRUE);
    }
    if (width > 0) gtk_widget_set_size_request(l, width, -1);
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), l);
}

static GtkWidget *
create_nav_button(const char *label, GCallback cb, gpointer user_data)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "files-nav-btn");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    if (cb) g_signal_connect(btn, "clicked", cb, user_data);
    return btn;
}

static GtkWidget *
create_badge_label(const char *text, const char *extra_css)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "shell-popover-badge");
    if (extra_css) gtk_widget_add_css_class(l, extra_css);
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    return l;
}

static GtkWidget *
create_scale_widget(double min_v, double max_v, double init_v, const char *css, int min_w, GCallback cb, gpointer user_data)
{
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min_v, max_v, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(s), FALSE);
    gtk_range_set_value(GTK_RANGE(s), init_v);
    if (css) gtk_widget_add_css_class(s, css);
    if (min_w > 0) gtk_widget_set_size_request(s, min_w, -1);
    gtk_widget_set_valign(s, GTK_ALIGN_CENTER);
    if (cb) g_signal_connect(s, "value-changed", cb, user_data);
    return s;
}

static void
populate_inspector_rows_from_array(KajoModWindow          *self,
                                   const KajoModSampleRow *rows,
                                   int                     n_rows)
{
    GtkWidget *child = NULL;
    while ((child = gtk_widget_get_first_child(self->box_sample_rows)) != NULL) {
        gtk_box_remove(GTK_BOX(self->box_sample_rows), child);
    }

    for (int i = 0; i < n_rows; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(row, "mod-sample-row");
        append_row_label(row, rows[i].idx_hex,    NULL, 24, 0.0f, FALSE);
        append_row_label(row, rows[i].ascii_name, NULL, -1, 0.0f, TRUE);
        append_row_label(row, rows[i].length,     NULL, 52, 1.0f, FALSE);
        append_row_label(row, rows[i].loop,       NULL, 74, 1.0f, FALSE);
        append_row_label(row, rows[i].vol,        NULL, 64, 1.0f, FALSE);
        gtk_box_append(GTK_BOX(self->box_sample_rows), row);
    }
}

static void
update_live_inspector_rows(KajoModWindow *self, const KajoModTelemetry *tel)
{
    if (!tel || !tel->has_live_rows || tel->n_live_rows <= 0) return;
    GtkWidget *row = gtk_widget_get_first_child(self->box_sample_rows);
    for (int i = 0; i < tel->n_live_rows && row != NULL; i++) {
        const KajoModSampleRow *r = &tel->live_rows[i];
        const char *vals[5] = { r->idx_hex, r->ascii_name, r->length, r->loop, r->vol };
        GtkWidget *cell = gtk_widget_get_first_child(row);
        for (int c = 0; c < 5 && cell != NULL; c++) {
            if (GTK_IS_LABEL(cell)) gtk_label_set_text(GTK_LABEL(cell), vals[c]);
            cell = gtk_widget_get_next_sibling(cell);
        }
        row = gtk_widget_get_next_sibling(row);
    }
}

static void
reset_ui_to_idle_state(KajoModWindow *self)
{
    if (!self) return;
    gtk_label_set_text(GTK_LABEL(self->hdr_engine_badge), "IDLE • READY");
    gtk_label_set_text(GTK_LABEL(self->lbl_title), "NO MODULE LOADED");
    gtk_label_set_text(GTK_LABEL(self->lbl_sub),
                       "Open an Amiga, C64 SID, Chiptune, or Tracker module / archive (+ ADD / + DIR)");
    gtk_label_set_text(GTK_LABEL(self->bdg_engine), "UADE / SID / GME");
    gtk_label_set_text(GTK_LABEL(self->bdg_format), "--");
    gtk_label_set_text(GTK_LABEL(self->bdg_chip), "48.0 kHz");

    self->sub_cur = 0;
    self->sub_max = 0;
    update_subsong_labels(self);

    gtk_label_set_text(GTK_LABEL(self->cnt_voices), "-- CH");
    gtk_label_set_text(GTK_LABEL(self->cnt_timing), "-- Hz");
    gtk_label_set_text(GTK_LABEL(self->cnt_clock), "IDLE");
    gtk_label_set_text(GTK_LABEL(self->cnt_time), "00:00/00:00");
    gtk_label_set_text(GTK_LABEL(self->lbl_t0), "00:00");
    gtk_label_set_text(GTK_LABEL(self->lbl_t1), "00:00");

    self->updating_seek = TRUE;
    gtk_range_set_range(GTK_RANGE(self->seek_scale), 0.0, 1.0);
    gtk_range_set_value(GTK_RANGE(self->seek_scale), 0.0);
    self->updating_seek = FALSE;

    self->playing = FALSE;
    gtk_button_set_label(GTK_BUTTON(self->btn_play), "▶ PLAY");

    for (int i = 0; i < 4; i++) {
        g_autofree gchar *cname = g_strdup_printf("CH %d", i + 1);
        gtk_label_set_text(GTK_LABEL(self->ch_names[i]), cname);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(self->ch_vus[i]), 0.0);
    }

    kajo_mod_scope_clear(self->scope_area);
    kajo_mod_fft_clear(self->fft_area);

    gtk_label_set_text(GTK_LABEL(self->lbl_sample_hdr_left), "SAMPLES / INSTRUMENTS / REGISTERS");
    gtk_label_set_text(GTK_LABEL(self->lbl_sample_hdr_right), "IDLE");
    gtk_label_set_text(GTK_LABEL(self->lbl_stil_hdr_right), "NONE");
    populate_inspector_rows_from_array(self, NULL, 0);
    gtk_text_buffer_set_text(self->stil_buffer, "", -1);

    gtk_label_set_text(GTK_LABEL(self->lbl_status_engine),
                       "READY — PRIORITY: 1. UADE  2. RESIDFP  3. LIBGME  4. OPENMPT  5. LIBXMP");
    gtk_label_set_text(GTK_LABEL(self->bdg_mode), "IDLE");
    gtk_widget_remove_css_class(self->bdg_mode, "success");
    gtk_widget_remove_css_class(self->bdg_mode, "warning");
}

static void
apply_metadata_to_ui(KajoModWindow *self, const KajoModMetadata *m)
{
    if (!self || !m) return;
    gtk_label_set_text(GTK_LABEL(self->hdr_engine_badge), m->hdr_badge ? m->hdr_badge : "KAJO MOD");
    gtk_label_set_text(GTK_LABEL(self->lbl_title), m->title ? m->title : "UNTITLED");
    gtk_label_set_text(GTK_LABEL(self->lbl_sub), m->subtitle ? m->subtitle : "");
    gtk_label_set_text(GTK_LABEL(self->bdg_engine), m->bdg_engine ? m->bdg_engine : "ENGINE");
    gtk_label_set_text(GTK_LABEL(self->bdg_format), m->bdg_format ? m->bdg_format : "MOD");
    gtk_label_set_text(GTK_LABEL(self->bdg_chip), m->bdg_chip ? m->bdg_chip : "DSP");

    self->sub_cur = MAX(1, m->subsong_cur);
    self->sub_max = MAX(1, m->subsong_max);
    update_subsong_labels(self);

    int dur = (int)MAX(1.0, m->duration_sec);
    g_autofree gchar *t_dur = g_strdup_printf("%02d:%02d", dur / 60, dur % 60);
    g_autofree gchar *t_cnt = g_strdup_printf("00:00/%s", t_dur);
    gtk_label_set_text(GTK_LABEL(self->cnt_voices), m->cnt_voices ? m->cnt_voices : "04 CH");
    gtk_label_set_text(GTK_LABEL(self->cnt_timing), m->cnt_timing ? m->cnt_timing : "50Hz");
    gtk_label_set_text(GTK_LABEL(self->cnt_clock), m->cnt_clock ? m->cnt_clock : "DMA");
    gtk_label_set_text(GTK_LABEL(self->cnt_time), t_cnt);
    gtk_label_set_text(GTK_LABEL(self->lbl_t0), "00:00");
    gtk_label_set_text(GTK_LABEL(self->lbl_t1), t_dur);

    self->updating_seek = TRUE;
    gtk_range_set_range(GTK_RANGE(self->seek_scale), 0.0, MAX(1.0, m->duration_sec));
    gtk_range_set_value(GTK_RANGE(self->seek_scale), 0.0);
    self->updating_seek = FALSE;

    gtk_widget_set_sensitive(self->grp_filter, m->filter_enabled);
    gtk_widget_set_sensitive(self->grp_led, m->led_enabled);
    gtk_widget_set_sensitive(self->grp_sid, m->sid_enabled);
    gtk_widget_set_sensitive(self->grp_tempo, m->tempo_enabled);

    for (int i = 0; i < 4; i++) {
        gtk_label_set_text(GTK_LABEL(self->ch_names[i]),
                           m->ch_names[i] ? m->ch_names[i] : "CH");
    }

    gtk_label_set_text(GTK_LABEL(self->lbl_sample_hdr_left),
                       m->sample_hdr_left ? m->sample_hdr_left : "SAMPLES / INSTRUMENTS");
    gtk_label_set_text(GTK_LABEL(self->lbl_sample_hdr_right),
                       m->sample_hdr_right ? m->sample_hdr_right : "PCM");
    gtk_label_set_text(GTK_LABEL(self->lbl_stil_hdr_right),
                       m->stil_hdr_right ? m->stil_hdr_right : "INFO");

    if (m->samples && m->samples->len > 0) {
        populate_inspector_rows_from_array(self,
                                           (const KajoModSampleRow *)m->samples->data,
                                           (int)m->samples->len);
    } else {
        populate_inspector_rows_from_array(self, NULL, 0);
    }
    gtk_text_buffer_set_text(self->stil_buffer, m->stil_text ? m->stil_text : "", -1);
    gtk_label_set_text(GTK_LABEL(self->lbl_status_engine),
                       m->status_engine ? m->status_engine : "LIVE ENGINE");

    gtk_label_set_text(GTK_LABEL(self->bdg_mode), "LIVE DSP");
    gtk_widget_remove_css_class(self->bdg_mode, "warning");
    gtk_widget_add_css_class(self->bdg_mode, "success");
}

static void
on_seg_button_clicked(GtkButton *btn, gpointer user_data)
{
    SegBtnCtx *ctx = (SegBtnCtx *)user_data;
    if (!ctx) return;

    for (GtkWidget *child = gtk_widget_get_first_child(ctx->group_box);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_BUTTON(child)) {
            if (child == GTK_WIDGET(btn))
                gtk_widget_add_css_class(child, "active");
            else
                gtk_widget_remove_css_class(child, "active");
        }
    }

    g_autoptr(KajoModWindow) self = g_weak_ref_get(&ctx->win_ref);
    if (!self) return;

    if (ctx->group_kind == 0) {
        if (ctx->item_idx == 0)      self->dsp.end_action = KAJO_MOD_END_NEXT_SUB;
        else if (ctx->item_idx == 1) self->dsp.end_action = KAJO_MOD_END_LOOP;
        else                         self->dsp.end_action = KAJO_MOD_END_NEXT_MOD;
    } else if (ctx->group_kind == 1) {
        if (ctx->item_idx == 0)      self->dsp.filter_mode = KAJO_MOD_FILTER_A500;
        else if (ctx->item_idx == 1) self->dsp.filter_mode = KAJO_MOD_FILTER_A1200;
        else if (ctx->item_idx == 2) self->dsp.filter_mode = KAJO_MOD_FILTER_NEAR;
        else if (ctx->item_idx == 3) self->dsp.filter_mode = KAJO_MOD_FILTER_LINEAR;
        else                         self->dsp.filter_mode = KAJO_MOD_FILTER_SINC;
    } else if (ctx->group_kind == 2) {
        if (ctx->item_idx == 0)      self->dsp.led_mode = KAJO_MOD_LED_AUTO;
        else if (ctx->item_idx == 1) self->dsp.led_mode = KAJO_MOD_LED_ON;
        else                         self->dsp.led_mode = KAJO_MOD_LED_OFF;
    } else if (ctx->group_kind == 3) {
        if (ctx->item_idx == 0)      self->dsp.sid_model = KAJO_MOD_SID_AUTO;
        else if (ctx->item_idx == 1) self->dsp.sid_model = KAJO_MOD_SID_6581;
        else                         self->dsp.sid_model = KAJO_MOD_SID_8580;
    }
    kajo_mod_audio_apply_dsp(self->audio, &self->dsp);
}

static GtkWidget *
build_segmented_group(KajoModWindow     *self,
                      int                group_kind,
                      const char *const *labels,
                      int                count,
                      int                active_idx)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "segmented-group");
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    for (int i = 0; i < count; i++) {
        GtkWidget *b = gtk_button_new_with_label(labels[i]);
        gtk_widget_add_css_class(b, "files-nav-btn");
        gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        if (i == active_idx)
            gtk_widget_add_css_class(b, "active");

        SegBtnCtx *ctx = g_new0(SegBtnCtx, 1);
        g_weak_ref_init(&ctx->win_ref, self);
        ctx->group_box  = box;
        ctx->group_kind = group_kind;
        ctx->item_idx   = i;
        g_signal_connect_data(b, "clicked", G_CALLBACK(on_seg_button_clicked),
                              ctx, seg_btn_ctx_free, 0);
        gtk_box_append(GTK_BOX(box), b);
    }
    return box;
}

static void
switch_subsong(KajoModWindow *self, int target_sub)
{
    if (target_sub < 1 || target_sub > self->sub_max) return;
    self->sub_cur = target_sub;
    update_subsong_labels(self);

    if (kajo_mod_audio_is_active(self->audio)) {
        KajoModMetadata updated;
        kajo_mod_metadata_init(&updated);
        if (kajo_mod_audio_set_subsong(self->audio, self->sub_cur, &updated)) {
            apply_metadata_to_ui(self, &updated);
        }
        kajo_mod_metadata_clear(&updated);
    }
}

static void on_sub_prev_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    if (self->sub_cur > 1) switch_subsong(self, self->sub_cur - 1);
}

static void on_sub_next_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    if (self->sub_cur < self->sub_max) switch_subsong(self, self->sub_cur + 1);
}

static void on_prev_track_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    if (self->sub_cur > 1) switch_subsong(self, self->sub_cur - 1);
    else if (self->active_pl_index > 0) activate_playlist_index(self, self->active_pl_index - 1);
}

static void on_next_track_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    if (self->sub_cur < self->sub_max) switch_subsong(self, self->sub_cur + 1);
    else if (self->playlist && self->active_pl_index + 1 < (int)self->playlist->items->len)
        activate_playlist_index(self, self->active_pl_index + 1);
}

static void on_play_toggle_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    if (!kajo_mod_audio_is_active(self->audio)) {
        if (self->playlist && self->playlist->items->len > 0) {
            int idx = (self->active_pl_index >= 0) ? self->active_pl_index : 0;
            activate_playlist_index(self, idx);
        }
        return;
    }
    self->playing = !self->playing;
    kajo_mod_audio_set_paused(self->audio, !self->playing);
    gtk_button_set_label(GTK_BUTTON(self->btn_play), self->playing ? "▶ PLAY" : "❚❚ PAUSED");
}

static void on_stop_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    self->playing = FALSE;
    kajo_mod_audio_stop(self->audio);
    gtk_button_set_label(GTK_BUTTON(self->btn_play), "❚❚ STOPPED");
    self->updating_seek = TRUE;
    gtk_range_set_value(GTK_RANGE(self->seek_scale), 0.0);
    self->updating_seek = FALSE;
    gtk_label_set_text(GTK_LABEL(self->lbl_t0), "00:00");
    for (int i = 0; i < 4; i++) {
        gtk_level_bar_set_value(GTK_LEVEL_BAR(self->ch_vus[i]), 0.0);
    }
    kajo_mod_scope_clear(self->scope_area);
    kajo_mod_fft_clear(self->fft_area);
}

static void on_seek_value_changed(GtkRange *range, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    if (!self->updating_seek) kajo_mod_audio_seek(self->audio, gtk_range_get_value(range));
}

static void on_vol_changed(GtkRange *range, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    double v = gtk_range_get_value(range);
    g_autofree gchar *s = g_strdup_printf("%d%%", (int)v);
    gtk_label_set_text(GTK_LABEL(self->lbl_vol_readout), s);
    kajo_mod_audio_set_volume(self->audio, v / 100.0);
}

static void on_sep_changed(GtkRange *range, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    int v = (int)gtk_range_get_value(range);
    const char *tag = (v == 0) ? "(MONO)" : (v <= 70 ? "(AMIGA)" : "(WIDE)");
    g_autofree gchar *s = g_strdup_printf("%d%% %s", v, tag);
    gtk_label_set_text(GTK_LABEL(self->lbl_sep_readout), s);
    self->dsp.stereo_sep_pct = (double)v;
    kajo_mod_audio_apply_dsp(self->audio, &self->dsp);
}

static void on_tempo_changed(GtkRange *range, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    int v = (int)gtk_range_get_value(range);
    g_autofree gchar *s = g_strdup_printf("%d%%", v);
    gtk_label_set_text(GTK_LABEL(self->lbl_tempo_readout), s);
    self->dsp.tempo_pct = (double)v;
    kajo_mod_audio_apply_dsp(self->audio, &self->dsp);
}

static void on_tempo_reset_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) {
    gtk_range_set_value(GTK_RANGE(KAJO_MOD_WINDOW(d)->scale_tempo), 100.0);
}

static void
sync_channel_mute_state(KajoModWindow *self)
{
    gboolean any_solo = FALSE;
    for (int i = 0; i < 4; i++) {
        if (self->solo_ch[i]) any_solo = TRUE;
    }

    for (int i = 0; i < 4; i++) {
        gboolean btn_muted = gtk_widget_has_css_class(self->ch_btn_m[i], "mute-on");
        gboolean eff_muted = any_solo ? !self->solo_ch[i] : btn_muted;
        self->dsp.ch_muted[i] = eff_muted;
        if (eff_muted)
            gtk_widget_add_css_class(self->ch_pills[i], "muted");
        else
            gtk_widget_remove_css_class(self->ch_pills[i], "muted");
    }
    kajo_mod_audio_apply_dsp(self->audio, &self->dsp);
}

static void
on_mute_clicked(GtkButton *btn, gpointer user_data)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(user_data);
    if (gtk_widget_has_css_class(GTK_WIDGET(btn), "mute-on"))
        gtk_widget_remove_css_class(GTK_WIDGET(btn), "mute-on");
    else
        gtk_widget_add_css_class(GTK_WIDGET(btn), "mute-on");
    sync_channel_mute_state(self);
}

static void
on_solo_clicked(GtkButton *btn, gpointer user_data)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(user_data);
    for (int i = 0; i < 4; i++) {
        if (self->ch_btn_s[i] == GTK_WIDGET(btn)) {
            self->solo_ch[i] = !self->solo_ch[i];
            if (self->solo_ch[i])
                gtk_widget_add_css_class(GTK_WIDGET(btn), "solo-on");
            else
                gtk_widget_remove_css_class(GTK_WIDGET(btn), "solo-on");
        }
    }
    sync_channel_mute_state(self);
}

static void
reflow_grid_items(GtkWidget *grid, GtkWidget **items, int count, int cols)
{
    if (!grid || !items[0]) return;
    for (int i = 0; i < count; i++) {
        g_object_ref(items[i]);
        gtk_grid_remove(GTK_GRID(grid), items[i]);
    }
    for (int i = 0; i < count; i++) {
        gtk_grid_attach(GTK_GRID(grid), items[i], i % cols, i / cols, 1, 1);
        g_object_unref(items[i]);
    }
}

static void
apply_inspector_responsive_reflow(KajoModWindow *self, int width)
{
    if (!self || !self->col_samples || !self->col_stil) return;

    int effective_w = (self->sim_col_width >= 480) ? self->sim_col_width : width;
    gboolean narrow = (effective_w > 0 && effective_w < 760);
    self->is_narrow_column = narrow;

    reflow_grid_items(self->cnt_grid, self->cnt_cells, 6, narrow ? 3 : 6);
    reflow_grid_items(self->ch_grid, self->ch_pills, 4, narrow ? 2 : 4);

    gtk_widget_set_visible(self->bdg_col_mode, !narrow);
    gtk_widget_set_visible(self->box_narrow_tabs, narrow);
    gtk_widget_set_visible(self->col_samples, !narrow || !self->narrow_show_stil);
    gtk_widget_set_visible(self->col_stil, !narrow || self->narrow_show_stil);
    if (!narrow)
        gtk_widget_add_css_class(self->col_samples, "mod-inspector-left");
    else
        gtk_widget_remove_css_class(self->col_samples, "mod-inspector-left");
}

static void on_narrow_tab_samples_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    self->narrow_show_stil = FALSE;
    gtk_widget_add_css_class(self->btn_tab_samples, "active");
    gtk_widget_remove_css_class(self->btn_tab_stil, "active");
    apply_inspector_responsive_reflow(self, gtk_widget_get_width(GTK_WIDGET(self)));
}

static void on_narrow_tab_stil_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    self->narrow_show_stil = TRUE;
    gtk_widget_remove_css_class(self->btn_tab_samples, "active");
    gtk_widget_add_css_class(self->btn_tab_stil, "active");
    apply_inspector_responsive_reflow(self, gtk_widget_get_width(GTK_WIDGET(self)));
}

static void on_surface_width_changed(GdkSurface *surface, GParamSpec *p G_GNUC_UNUSED, gpointer d) {
    int w = gdk_surface_get_width(surface);
    if (w > 0) apply_inspector_responsive_reflow(KAJO_MOD_WINDOW(d), w);
}

static void on_window_realize(GtkWidget *widget, gpointer d G_GNUC_UNUSED) {
    KajoModWindow *self = KAJO_MOD_WINDOW(widget);
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (surface) {
        g_signal_connect_object(surface, "notify::width", G_CALLBACK(on_surface_width_changed), self, 0);
        int w = gdk_surface_get_width(surface);
        if (w > 0) apply_inspector_responsive_reflow(self, w);
    }
}

static void on_window_map(GtkWidget *w, gpointer d G_GNUC_UNUSED) {
    kajo_mod_audio_set_vis_mapped(KAJO_MOD_WINDOW(w)->audio, TRUE);
}

static void on_window_unmap(GtkWidget *w, gpointer d G_GNUC_UNUSED) {
    kajo_mod_audio_set_vis_mapped(KAJO_MOD_WINDOW(w)->audio, FALSE);
}

static gboolean
on_ui_tick(gpointer user_data)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(user_data);
    KajoModAudioSnapshot snap;
    if (!kajo_mod_audio_get_snapshot(self->audio, &snap)) {
        return G_SOURCE_CONTINUE;
    }

    int cur_s = (int)MAX(0.0, snap.pos_sec);
    int dur_s = (int)MAX(1.0, snap.duration_sec);
    g_autofree gchar *s_t0  = g_strdup_printf("%02d:%02d", cur_s / 60, cur_s % 60);
    g_autofree gchar *s_t1  = g_strdup_printf("%02d:%02d", dur_s / 60, dur_s % 60);
    g_autofree gchar *s_cnt = g_strdup_printf("%s/%s", s_t0, s_t1);

    gtk_label_set_text(GTK_LABEL(self->lbl_t0), s_t0);
    gtk_label_set_text(GTK_LABEL(self->lbl_t1), s_t1);
    gtk_label_set_text(GTK_LABEL(self->cnt_time), s_cnt);

    self->updating_seek = TRUE;
    gtk_range_set_value(GTK_RANGE(self->seek_scale), CLAMP(snap.pos_sec, 0.0, snap.duration_sec));
    self->updating_seek = FALSE;

    for (int i = 0; i < 4; i++) {
        gtk_level_bar_set_value(GTK_LEVEL_BAR(self->ch_vus[i]),
                                CLAMP(snap.telemetry.ch_vu[i], 0.0, 1.0));
    }

    if (snap.telemetry.cnt_timing[0])
        gtk_label_set_text(GTK_LABEL(self->cnt_timing), snap.telemetry.cnt_timing);
    if (snap.telemetry.cnt_clock[0])
        gtk_label_set_text(GTK_LABEL(self->cnt_clock), snap.telemetry.cnt_clock);
    if (snap.telemetry.bdg_chip[0])
        gtk_label_set_text(GTK_LABEL(self->bdg_chip), snap.telemetry.bdg_chip);
    update_live_inspector_rows(self, &snap.telemetry);

    if (snap.playing) {
        kajo_mod_scope_set_pcm(self->scope_area, snap.scope_l, snap.scope_r, KAJO_MOD_SCOPE_SAMPLES);
        kajo_mod_fft_set_bands(self->fft_area, snap.fft_bands, snap.fft_peaks);
    }

    if (snap.song_ended) {
        if (self->dsp.end_action == KAJO_MOD_END_NEXT_SUB && self->sub_cur < self->sub_max) {
            switch_subsong(self, self->sub_cur + 1);
        } else if (self->playlist && self->active_pl_index + 1 < (int)self->playlist->items->len) {
            activate_playlist_index(self, self->active_pl_index + 1);
        } else {
            self->playing = FALSE;
            gtk_button_set_label(GTK_BUTTON(self->btn_play), "▶ PLAY");
        }
    }
    return G_SOURCE_CONTINUE;
}

static void
rebuild_playlist_rows(KajoModWindow *self)
{
    GtkWidget *child = NULL;
    while ((child = gtk_widget_get_first_child(self->pl_listbox)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(self->pl_listbox), child);
    }

    guint count = self->playlist ? self->playlist->items->len : 0;
    g_autofree gchar *hdr = g_strdup_printf("📂 PLAYLIST & ARCHIVE QUEUE (%u TRACK%s)",
                                            count, count == 1 ? "" : "S");
    gtk_label_set_text(GTK_LABEL(self->lbl_pl_title), hdr);

    const char *q = gtk_editable_get_text(GTK_EDITABLE(self->entry_filter));
    g_autofree gchar *q_low = g_ascii_strdown(q ? q : "", -1);

    for (guint i = 0; i < count; i++) {
        KajoModPlaylistItem *it = g_ptr_array_index(self->playlist->items, i);
        GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        g_autofree gchar *num_str = g_strdup_printf("%02u", i + 1);

        append_row_label(rbox, num_str, "mod-pl-text", 22, 0.0f, FALSE);
        append_row_label(rbox, it->display_title ? it->display_title : "Track", "mod-pl-text", -1, 0.0f, TRUE);

        GtkWidget *l_b = gtk_label_new(it->badge ? it->badge : "MOD");
        gtk_widget_add_css_class(l_b, "shell-popover-badge");
        if (it->badge_css && it->badge_css[0] != '\0')
            gtk_widget_add_css_class(l_b, it->badge_css);
        gtk_widget_set_valign(l_b, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(rbox), l_b);

        append_row_label(rbox, it->sub_text ? it->sub_text : "1 SUB", "mod-pl-text", 40, 0.0f, FALSE);
        append_row_label(rbox, it->dur_text ? it->dur_text : "--:--", "mod-pl-text", 40, 1.0f, FALSE);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), rbox);

        g_autofree gchar *t_low = g_ascii_strdown(it->display_title ? it->display_title : "", -1);
        g_autofree gchar *b_low = g_ascii_strdown(it->badge ? it->badge : "", -1);
        gboolean match = (!q_low[0] || strstr(t_low, q_low) != NULL || strstr(b_low, q_low) != NULL);
        gtk_widget_set_visible(row, match);

        gtk_list_box_append(GTK_LIST_BOX(self->pl_listbox), row);
    }
}

static void
activate_playlist_index(KajoModWindow *self, int idx)
{
    if (!self->playlist || idx < 0 || idx >= (int)self->playlist->items->len) return;
    self->active_pl_index = idx;

    KajoModPlaylistItem *it = g_ptr_array_index(self->playlist->items, idx);

    g_autoptr(GError) err = NULL;
    KajoModEngine *eng = kajo_mod_engine_open(it->file_path, 0, &self->dsp,
                                              it->archive_nfo_name,
                                              it->archive_nfo_text, &err);
    if (!eng) {
        gtk_label_set_text(GTK_LABEL(self->lbl_status_engine),
                           err ? err->message : "Failed to open module");
        return;
    }

    kajo_mod_playlist_item_update_from_meta(it, &eng->meta);
    rebuild_playlist_rows(self);
    apply_metadata_to_ui(self, &eng->meta);
    self->playing = TRUE;
    gtk_button_set_label(GTK_BUTTON(self->btn_play), "▶ PLAY");
    kajo_mod_audio_set_engine(self->audio, eng, TRUE);

    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->pl_listbox), idx);
    if (row && gtk_list_box_get_selected_row(GTK_LIST_BOX(self->pl_listbox)) != row) {
        gtk_list_box_select_row(GTK_LIST_BOX(self->pl_listbox), row);
    }
}

static void on_pl_row_selected(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer d) {
    if (!row) return;
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    int idx = gtk_list_box_row_get_index(row);
    if (idx >= 0 && idx != self->active_pl_index) activate_playlist_index(self, idx);
}

static void on_filter_changed(GtkEditable *editable G_GNUC_UNUSED, gpointer d) {
    rebuild_playlist_rows(KAJO_MOD_WINDOW(d));
}

static void on_add_files_finish(GObject *source, GAsyncResult *res, gpointer user_data) {
    GWeakRef *wr = (GWeakRef *)user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), res, &err);
    g_autoptr(KajoModWindow) self = g_weak_ref_get(wr);
    weak_ref_free(wr);
    if (!self || !files || err) return;

    guint n = g_list_model_get_n_items(files);
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < n; i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        gchar *p = g_file_get_path(f);
        if (p) g_ptr_array_add(paths, p);
    }
    if (paths->len > 0)
        kajo_mod_window_open_paths(self, (const char * const *)paths->pdata, (int)paths->len);
}

static void on_add_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Add Modules or Archives (.lha, .zip, .sid, .mod, ...)");
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL, on_add_files_finish, weak_ref_new(self));
}

static void on_add_dir_finish(GObject *source, GAsyncResult *res, gpointer user_data) {
    GWeakRef *wr = (GWeakRef *)user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, &err);
    g_autoptr(KajoModWindow) self = g_weak_ref_get(wr);
    weak_ref_free(wr);
    if (!self || !folder || err) return;

    g_autofree gchar *p = g_file_get_path(folder);
    if (p) {
        const char *paths[1] = { p };
        kajo_mod_window_open_paths(self, paths, 1);
    }
}

static void on_dir_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Add Folder of Modules / HVSC / Archives");
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(self), NULL, on_add_dir_finish, weak_ref_new(self));
}

static void on_rem_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->pl_listbox));
    if (!row) return;
    int idx = gtk_list_box_row_get_index(row);
    if (idx >= 0) {
        kajo_mod_playlist_remove_at(self->playlist, (guint)idx);
        if (self->playlist->items->len == 0) {
            kajo_mod_audio_set_engine(self->audio, NULL, FALSE);
            self->active_pl_index = -1;
            reset_ui_to_idle_state(self);
        }
        rebuild_playlist_rows(self);
    }
}

static void on_clr_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer d) {
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    kajo_mod_audio_set_engine(self->audio, NULL, FALSE);
    kajo_mod_playlist_clear(self->playlist);
    self->active_pl_index = -1;
    reset_ui_to_idle_state(self);
    rebuild_playlist_rows(self);
}

void
kajo_mod_window_open_paths(KajoModWindow *self, const char * const *paths, int n_paths)
{
    g_return_if_fail(KAJO_IS_MOD_WINDOW(self));
    if (!paths || n_paths <= 0) return;

    int first_new_idx = -1;
    for (int i = 0; i < n_paths; i++) {
        if (!paths[i] || !*paths[i]) continue;
        guint before = self->playlist->items->len;
        if (kajo_mod_playlist_add_path(self->playlist, paths[i], NULL) > 0 && first_new_idx < 0) {
            first_new_idx = (int)before;
        }
    }
    rebuild_playlist_rows(self);
    if (first_new_idx >= 0 && first_new_idx < (int)self->playlist->items->len) {
        activate_playlist_index(self, first_new_idx);
    } else if (self->playlist->items->len > 0) {
        activate_playlist_index(self, 0);
    }
}

void
kajo_mod_window_set_simulated_column_width(KajoModWindow *self, int col_width)
{
    g_return_if_fail(KAJO_IS_MOD_WINDOW(self));
    self->sim_col_width = col_width;
    if (col_width >= 480 && gtk_window_get_child(GTK_WINDOW(self)) == self->root_vbox) {
        g_object_ref(self->root_vbox);
        gtk_window_set_child(GTK_WINDOW(self), NULL);

        GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_paned_set_resize_start_child(GTK_PANED(hpaned), FALSE);
        gtk_paned_set_shrink_start_child(GTK_PANED(hpaned), TRUE);
        gtk_paned_set_position(GTK_PANED(hpaned), col_width);
        gtk_paned_set_start_child(GTK_PANED(hpaned), self->root_vbox);
        gtk_paned_set_end_child(GTK_PANED(hpaned), gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
        gtk_window_set_child(GTK_WINDOW(self), hpaned);
        g_object_unref(self->root_vbox);

        apply_inspector_responsive_reflow(self, col_width);
    }
}

static GtkWidget *
create_counter_cell(const char *lbl, const char *val, gboolean accent_val, gboolean is_last, GtkWidget **out_val_lbl)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(box, "mod-counter-cell");
    if (is_last) gtk_widget_add_css_class(box, "last");
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *l_k = gtk_label_new(lbl);
    gtk_widget_add_css_class(l_k, "mod-counter-lbl");
    gtk_label_set_ellipsize(GTK_LABEL(l_k), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(l_k, GTK_ALIGN_CENTER);

    GtkWidget *l_v = gtk_label_new(val);
    gtk_widget_add_css_class(l_v, "mod-counter-val");
    if (accent_val) gtk_widget_add_css_class(l_v, "accent");
    gtk_label_set_ellipsize(GTK_LABEL(l_v), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(l_v, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), l_k);
    gtk_box_append(GTK_BOX(box), l_v);
    if (out_val_lbl) *out_val_lbl = l_v;
    return box;
}

static GtkWidget *
create_dsp_seg_group(KajoModWindow *self, const char *title, int kind, const char *const *labels, int count, int active)
{
    GtkWidget *grp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(grp, "mod-dsp-group");
    GtkWidget *lbl = gtk_label_new(title);
    gtk_widget_add_css_class(lbl, "mod-dsp-label");
    gtk_box_append(GTK_BOX(grp), lbl);
    gtk_box_append(GTK_BOX(grp), build_segmented_group(self, kind, labels, count, active));
    return grp;
}

static void
append_telemetry_and_transport(KajoModWindow *self, GtkWidget *top_vbox)
{
    GtkWidget *tl_deck = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(tl_deck, "mod-telemetry-deck");

    GtkWidget *tl_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(title_col, TRUE);
    gtk_widget_set_valign(title_col, GTK_ALIGN_CENTER);

    self->lbl_title = gtk_label_new("");
    gtk_widget_add_css_class(self->lbl_title, "mod-track-title");
    gtk_label_set_xalign(GTK_LABEL(self->lbl_title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_title), PANGO_ELLIPSIZE_END);

    self->lbl_sub = gtk_label_new("");
    gtk_widget_add_css_class(self->lbl_sub, "mod-track-sub");
    gtk_label_set_xalign(GTK_LABEL(self->lbl_sub), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_sub), PANGO_ELLIPSIZE_END);

    gtk_box_append(GTK_BOX(title_col), self->lbl_title);
    gtk_box_append(GTK_BOX(title_col), self->lbl_sub);

    GtkWidget *bdg_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(bdg_box, GTK_ALIGN_CENTER);
    self->bdg_engine = create_badge_label("UADE / SID / GME", "accent");
    self->bdg_format = create_badge_label("--", NULL);
    self->bdg_chip   = create_badge_label("48.0 kHz", "success");
    gtk_box_append(GTK_BOX(bdg_box), self->bdg_engine);
    gtk_box_append(GTK_BOX(bdg_box), self->bdg_format);
    gtk_box_append(GTK_BOX(bdg_box), self->bdg_chip);

    gtk_box_append(GTK_BOX(tl_top), title_col);
    gtk_box_append(GTK_BOX(tl_top), bdg_box);

    GtkWidget *cnt_grid = gtk_grid_new();
    self->cnt_grid = cnt_grid;
    gtk_widget_add_css_class(cnt_grid, "mod-counter-strip");
    gtk_grid_set_column_homogeneous(GTK_GRID(cnt_grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(cnt_grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(cnt_grid), 2);

    GtkWidget *out_lbl = NULL;
    self->cnt_cells[0] = create_counter_cell("SUB", "00/00", TRUE, FALSE, &self->cnt_sub);
    self->cnt_cells[1] = create_counter_cell("VOICES", "-- CH", FALSE, FALSE, &self->cnt_voices);
    self->cnt_cells[2] = create_counter_cell("TIMING", "-- Hz", FALSE, FALSE, &self->cnt_timing);
    self->cnt_cells[3] = create_counter_cell("CLOCK", "IDLE", FALSE, FALSE, &self->cnt_clock);
    self->cnt_cells[4] = create_counter_cell("OUT", "48.0kHz", FALSE, FALSE, &out_lbl);
    self->cnt_cells[5] = create_counter_cell("TIME", "00:00/00:00", TRUE, TRUE, &self->cnt_time);
    for (int i = 0; i < 6; i++)
        gtk_grid_attach(GTK_GRID(cnt_grid), self->cnt_cells[i], i, 0, 1, 1);

    gtk_box_append(GTK_BOX(tl_deck), tl_top);
    gtk_box_append(GTK_BOX(tl_deck), cnt_grid);
    gtk_box_append(GTK_BOX(top_vbox), tl_deck);

    GtkWidget *tr_deck = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(tr_deck, "mod-transport-deck");

    GtkWidget *tr_row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->lbl_t0 = gtk_label_new("00:00");
    gtk_widget_add_css_class(self->lbl_t0, "player-timecode");
    gtk_widget_set_valign(self->lbl_t0, GTK_ALIGN_CENTER);

    self->seek_scale = create_scale_widget(0.0, 1.0, 0.0, "player-seek-scale", -1,
                                           G_CALLBACK(on_seek_value_changed), self);
    gtk_widget_set_hexpand(self->seek_scale, TRUE);

    self->lbl_t1 = gtk_label_new("00:00");
    gtk_widget_add_css_class(self->lbl_t1, "player-timecode");
    gtk_widget_set_valign(self->lbl_t1, GTK_ALIGN_CENTER);

    GtkWidget *sub_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(sub_box, "mod-subsong-box");
    gtk_widget_set_valign(sub_box, GTK_ALIGN_CENTER);
    GtkWidget *btn_sub_prev = gtk_button_new_with_label("◀ SUB");
    self->lbl_sub_readout = gtk_label_new("SUB 00 / 00");
    gtk_widget_add_css_class(self->lbl_sub_readout, "mod-subsong-readout");
    GtkWidget *btn_sub_next = gtk_button_new_with_label("SUB ▶");
    g_signal_connect(btn_sub_prev, "clicked", G_CALLBACK(on_sub_prev_clicked), self);
    g_signal_connect(btn_sub_next, "clicked", G_CALLBACK(on_sub_next_clicked), self);
    gtk_box_append(GTK_BOX(sub_box), btn_sub_prev);
    gtk_box_append(GTK_BOX(sub_box), self->lbl_sub_readout);
    gtk_box_append(GTK_BOX(sub_box), btn_sub_next);

    gtk_box_append(GTK_BOX(tr_row1), self->lbl_t0);
    gtk_box_append(GTK_BOX(tr_row1), self->seek_scale);
    gtk_box_append(GTK_BOX(tr_row1), self->lbl_t1);
    gtk_box_append(GTK_BOX(tr_row1), sub_box);

    GtkWidget *tr_row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *btn_cluster = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_valign(btn_cluster, GTK_ALIGN_CENTER);

    self->btn_play = create_nav_button("▶ PLAY", G_CALLBACK(on_play_toggle_clicked), self);
    gtk_widget_add_css_class(self->btn_play, "action-btn-primary");
    gtk_box_append(GTK_BOX(btn_cluster), create_nav_button("⏮", G_CALLBACK(on_prev_track_clicked), self));
    gtk_box_append(GTK_BOX(btn_cluster), self->btn_play);
    gtk_box_append(GTK_BOX(btn_cluster), create_nav_button("⏸", G_CALLBACK(on_play_toggle_clicked), self));
    gtk_box_append(GTK_BOX(btn_cluster), create_nav_button("⏹", G_CALLBACK(on_stop_clicked), self));
    gtk_box_append(GTK_BOX(btn_cluster), create_nav_button("⏭", G_CALLBACK(on_next_track_clicked), self));

    static const char *const k_loop_labels[] = { "ADVANCE", "LOOP", "FADE" };
    GtkWidget *loop_seg = build_segmented_group(self, 0, k_loop_labels, 3, (int)self->dsp.end_action);
    self->loop_seg = loop_seg;
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);

    GtkWidget *vol_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_valign(vol_box, GTK_ALIGN_CENTER);
    GtkWidget *vol_scale = create_scale_widget(0.0, 100.0, 85.0, "player-volume-scale", 72,
                                               G_CALLBACK(on_vol_changed), self);
    self->lbl_vol_readout = gtk_label_new("85%");
    gtk_widget_add_css_class(self->lbl_vol_readout, "player-timecode");
    gtk_box_append(GTK_BOX(vol_box), gtk_label_new("🔊"));
    gtk_box_append(GTK_BOX(vol_box), vol_scale);
    gtk_box_append(GTK_BOX(vol_box), self->lbl_vol_readout);

    gtk_box_append(GTK_BOX(tr_row2), btn_cluster);
    gtk_box_append(GTK_BOX(tr_row2), loop_seg);
    gtk_box_append(GTK_BOX(tr_row2), spacer);
    gtk_box_append(GTK_BOX(tr_row2), vol_box);

    gtk_box_append(GTK_BOX(tr_deck), tr_row1);
    gtk_box_append(GTK_BOX(tr_deck), tr_row2);
    gtk_box_append(GTK_BOX(top_vbox), tr_deck);
}

static GtkWidget *
build_top_fixed_zones(KajoModWindow *self)
{
    GtkWidget *top_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(top_vbox, 8);
    gtk_widget_set_margin_end(top_vbox, 8);
    gtk_widget_set_margin_top(top_vbox, 6);
    gtk_widget_set_vexpand(top_vbox, FALSE);

    append_telemetry_and_transport(self, top_vbox);

    GtkWidget *dsp_strip = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(dsp_strip, "mod-dsp-strip");

    GtkWidget *dsp_row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    static const char *const k_flt_labels[] = { "A500", "A1200", "NEAR", "LIN", "SINC" };
    static const char *const k_led_labels[] = { "AUTO", "ON", "OFF" };
    static const char *const k_sid_labels[] = { "AUTO", "6581", "8580" };
    self->grp_filter = create_dsp_seg_group(self, "FLT:", 1, k_flt_labels, 5, (int)self->dsp.filter_mode);
    self->grp_led    = create_dsp_seg_group(self, "LED:", 2, k_led_labels, 3, (int)self->dsp.led_mode);
    self->grp_sid    = create_dsp_seg_group(self, "SID:", 3, k_sid_labels, 3, (int)self->dsp.sid_model);
    gtk_box_append(GTK_BOX(dsp_row1), self->grp_filter);
    gtk_box_append(GTK_BOX(dsp_row1), self->grp_led);
    gtk_box_append(GTK_BOX(dsp_row1), self->grp_sid);

    GtkWidget *dsp_row2 = gtk_grid_new();
    gtk_widget_add_css_class(dsp_row2, "mod-dsp-row-2");
    gtk_grid_set_column_homogeneous(GTK_GRID(dsp_row2), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(dsp_row2), 6);

    GtkWidget *box_sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(box_sep, "mod-dsp-group");
    GtkWidget *lbl_sep = gtk_label_new("SEP:");
    gtk_widget_add_css_class(lbl_sep, "mod-dsp-label");
    int init_sep = CLAMP((int)self->dsp.stereo_sep_pct, 0, 100);
    GtkWidget *scale_sep = create_scale_widget(0.0, 100.0, (double)init_sep, "player-volume-scale", 40,
                                               G_CALLBACK(on_sep_changed), self);
    self->scale_sep = scale_sep;
    gtk_widget_set_hexpand(scale_sep, TRUE);
    const char *sep_tag = (init_sep == 0) ? "(MONO)" : (init_sep <= 70 ? "(AMIGA)" : "(WIDE)");
    g_autofree gchar *init_sep_txt = g_strdup_printf("%d%% %s", init_sep, sep_tag);
    self->lbl_sep_readout = gtk_label_new(init_sep_txt);
    gtk_widget_add_css_class(self->lbl_sep_readout, "mod-counter-val");
    gtk_widget_add_css_class(self->lbl_sep_readout, "accent");
    gtk_box_append(GTK_BOX(box_sep), lbl_sep);
    gtk_box_append(GTK_BOX(box_sep), scale_sep);
    gtk_box_append(GTK_BOX(box_sep), self->lbl_sep_readout);

    self->grp_tempo = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(self->grp_tempo, "mod-dsp-group");
    GtkWidget *lbl_tmp = gtk_label_new("TEMPO:");
    gtk_widget_add_css_class(lbl_tmp, "mod-dsp-label");
    self->scale_tempo = create_scale_widget(50.0, 200.0, 100.0, "player-volume-scale", 40,
                                            G_CALLBACK(on_tempo_changed), self);
    gtk_widget_set_hexpand(self->scale_tempo, TRUE);
    self->lbl_tempo_readout = gtk_label_new("100%");
    gtk_widget_add_css_class(self->lbl_tempo_readout, "mod-counter-val");
    gtk_box_append(GTK_BOX(self->grp_tempo), lbl_tmp);
    gtk_box_append(GTK_BOX(self->grp_tempo), self->scale_tempo);
    gtk_box_append(GTK_BOX(self->grp_tempo), self->lbl_tempo_readout);
    gtk_box_append(GTK_BOX(self->grp_tempo), create_nav_button("1:1", G_CALLBACK(on_tempo_reset_clicked), self));

    gtk_grid_attach(GTK_GRID(dsp_row2), box_sep, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(dsp_row2), self->grp_tempo, 1, 0, 1, 1);

    gtk_box_append(GTK_BOX(dsp_strip), dsp_row1);
    gtk_box_append(GTK_BOX(dsp_strip), dsp_row2);
    gtk_box_append(GTK_BOX(top_vbox), dsp_strip);

    GtkWidget *scope_deck = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(scope_deck, "mod-scope-deck");

    GtkWidget *ch_grid = gtk_grid_new();
    self->ch_grid = ch_grid;
    gtk_widget_add_css_class(ch_grid, "mod-channel-header");
    gtk_grid_set_column_homogeneous(GTK_GRID(ch_grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(ch_grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(ch_grid), 3);

    for (int i = 0; i < 4; i++) {
        GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
        gtk_widget_add_css_class(pill, "mod-channel-pill");
        gtk_widget_set_hexpand(pill, TRUE);

        self->ch_names[i] = gtk_label_new("CH");
        gtk_widget_add_css_class(self->ch_names[i], "mod-channel-name");
        gtk_label_set_ellipsize(GTK_LABEL(self->ch_names[i]), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(self->ch_names[i]), 3);
        gtk_label_set_max_width_chars(GTK_LABEL(self->ch_names[i]), 8);
        gtk_widget_set_valign(self->ch_names[i], GTK_ALIGN_CENTER);

        self->ch_vus[i] = gtk_level_bar_new_for_interval(0.0, 1.0);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(self->ch_vus[i]), 0.0);
        gtk_widget_set_size_request(self->ch_vus[i], 12, 5);
        gtk_widget_set_hexpand(self->ch_vus[i], TRUE);
        gtk_widget_set_valign(self->ch_vus[i], GTK_ALIGN_CENTER);

        GtkWidget *btn_m = gtk_button_new_with_label("M");
        gtk_widget_add_css_class(btn_m, "mod-ms-btn");
        gtk_widget_set_valign(btn_m, GTK_ALIGN_CENTER);
        g_signal_connect(btn_m, "clicked", G_CALLBACK(on_mute_clicked), self);

        GtkWidget *btn_s = gtk_button_new_with_label("S");
        gtk_widget_add_css_class(btn_s, "mod-ms-btn");
        gtk_widget_set_valign(btn_s, GTK_ALIGN_CENTER);
        g_signal_connect(btn_s, "clicked", G_CALLBACK(on_solo_clicked), self);

        gtk_box_append(GTK_BOX(pill), self->ch_names[i]);
        gtk_box_append(GTK_BOX(pill), self->ch_vus[i]);
        gtk_box_append(GTK_BOX(pill), btn_m);
        gtk_box_append(GTK_BOX(pill), btn_s);
        self->ch_pills[i] = pill;
        self->ch_btn_m[i] = btn_m;
        self->ch_btn_s[i] = btn_s;
        gtk_grid_attach(GTK_GRID(ch_grid), pill, i, 0, 1, 1);
    }

    GtkWidget *vis_grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(vis_grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(vis_grid), 6);
    gtk_widget_set_margin_start(vis_grid, 6);
    gtk_widget_set_margin_end(vis_grid, 6);
    gtk_widget_set_margin_top(vis_grid, 5);
    gtk_widget_set_margin_bottom(vis_grid, 5);

    self->scope_area = kajo_mod_scope_new();
    self->fft_area   = kajo_mod_fft_new();
    gtk_grid_attach(GTK_GRID(vis_grid), self->scope_area, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(vis_grid), self->fft_area, 1, 0, 1, 1);

    gtk_box_append(GTK_BOX(scope_deck), ch_grid);
    gtk_box_append(GTK_BOX(scope_deck), vis_grid);
    gtk_box_append(GTK_BOX(top_vbox), scope_deck);

    return top_vbox;
}

static GtkWidget *
build_upper_inspector_card(KajoModWindow *self)
{
    GtkWidget *upper_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(upper_card, "mod-pane-card");
    gtk_widget_set_vexpand(upper_card, TRUE);

    GtkWidget *up_tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(up_tb, "mod-pane-toolbar");
    append_row_label(up_tb, "📋 MODULE, HARDWARE & ARCHIVE INSPECTOR", "mod-pane-title", -1, 0.0f, TRUE);
    self->bdg_col_mode = create_badge_label("2-COL SPLIT (≥760px)", NULL);

    self->box_narrow_tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->box_narrow_tabs, "segmented-group");
    self->btn_tab_samples = create_nav_button("SAMPLES / VOICES", G_CALLBACK(on_narrow_tab_samples_clicked), self);
    gtk_widget_add_css_class(self->btn_tab_samples, "active");
    self->btn_tab_stil = create_nav_button("STIL / .NFO", G_CALLBACK(on_narrow_tab_stil_clicked), self);
    gtk_box_append(GTK_BOX(self->box_narrow_tabs), self->btn_tab_samples);
    gtk_box_append(GTK_BOX(self->box_narrow_tabs), self->btn_tab_stil);
    gtk_widget_set_visible(self->box_narrow_tabs, FALSE);

    gtk_box_append(GTK_BOX(up_tb), self->bdg_col_mode);
    gtk_box_append(GTK_BOX(up_tb), self->box_narrow_tabs);

    GtkWidget *insp_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(insp_body, TRUE);
    gtk_widget_set_hexpand(insp_body, TRUE);

    self->col_samples = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(self->col_samples, "mod-inspector-left");
    gtk_widget_set_hexpand(self->col_samples, TRUE);
    gtk_widget_set_vexpand(self->col_samples, TRUE);

    GtkWidget *s_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(s_hdr, "mod-subcol-hdr");
    self->lbl_sample_hdr_left = gtk_label_new("SAMPLES / INSTRUMENTS / REGISTERS");
    gtk_label_set_xalign(GTK_LABEL(self->lbl_sample_hdr_left), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_sample_hdr_left), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(self->lbl_sample_hdr_left, TRUE);
    self->lbl_sample_hdr_right = gtk_label_new("IDLE");
    gtk_widget_add_css_class(self->lbl_sample_hdr_right, "mod-counter-val");
    gtk_widget_add_css_class(self->lbl_sample_hdr_right, "accent");
    gtk_box_append(GTK_BOX(s_hdr), self->lbl_sample_hdr_left);
    gtk_box_append(GTK_BOX(s_hdr), self->lbl_sample_hdr_right);

    GtkWidget *s_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(s_scroll, TRUE);
    self->box_sample_rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s_scroll), self->box_sample_rows);
    gtk_box_append(GTK_BOX(self->col_samples), s_hdr);
    gtk_box_append(GTK_BOX(self->col_samples), s_scroll);

    self->col_stil = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(self->col_stil, TRUE);
    gtk_widget_set_vexpand(self->col_stil, TRUE);

    GtkWidget *m_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(m_hdr, "mod-subcol-hdr");
    append_row_label(m_hdr, "ARCHIVE .NFO / HVSC STIL / SONG MESSAGE", NULL, -1, 0.0f, TRUE);
    self->lbl_stil_hdr_right = create_badge_label("NONE", "warning");
    gtk_box_append(GTK_BOX(m_hdr), self->lbl_stil_hdr_right);

    GtkWidget *m_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(m_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(m_scroll, TRUE);

    GtkWidget *stil_tv = gtk_text_view_new();
    gtk_widget_add_css_class(stil_tv, "mod-stil-view");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(stil_tv), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(stil_tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(stil_tv), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(stil_tv), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(stil_tv), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(stil_tv), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(stil_tv), 6);
    self->stil_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(stil_tv));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(m_scroll), stil_tv);

    gtk_box_append(GTK_BOX(self->col_stil), m_hdr);
    gtk_box_append(GTK_BOX(self->col_stil), m_scroll);

    gtk_box_append(GTK_BOX(insp_body), self->col_samples);
    gtk_box_append(GTK_BOX(insp_body), self->col_stil);
    gtk_box_append(GTK_BOX(upper_card), up_tb);
    gtk_box_append(GTK_BOX(upper_card), insp_body);
    return upper_card;
}

static GtkWidget *
build_expanding_paned(KajoModWindow *self)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_widget_set_margin_start(paned, 8);
    gtk_widget_set_margin_end(paned, 8);
    gtk_widget_set_margin_top(paned, 5);
    gtk_widget_set_margin_bottom(paned, 6);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 195);

    GtkWidget *lower_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(lower_card, "mod-pane-card");
    gtk_widget_set_vexpand(lower_card, TRUE);

    GtkWidget *pl_tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(pl_tb, "mod-pane-toolbar");
    self->lbl_pl_title = gtk_label_new("📂 PLAYLIST & ARCHIVE QUEUE (0 TRACKS)");
    gtk_widget_add_css_class(self->lbl_pl_title, "mod-pane-title");
    gtk_label_set_xalign(GTK_LABEL(self->lbl_pl_title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_pl_title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(self->lbl_pl_title, TRUE);

    self->entry_filter = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_filter), "🔍 Filter...");
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_filter), 5);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(self->entry_filter), 12);
    g_signal_connect(self->entry_filter, "changed", G_CALLBACK(on_filter_changed), self);

    gtk_box_append(GTK_BOX(pl_tb), self->lbl_pl_title);
    gtk_box_append(GTK_BOX(pl_tb), create_nav_button("+ ADD", G_CALLBACK(on_add_clicked), self));
    gtk_box_append(GTK_BOX(pl_tb), create_nav_button("+ DIR", G_CALLBACK(on_dir_clicked), self));
    gtk_box_append(GTK_BOX(pl_tb), create_nav_button("− REM", G_CALLBACK(on_rem_clicked), self));
    gtk_box_append(GTK_BOX(pl_tb), create_nav_button("CLR",   G_CALLBACK(on_clr_clicked), self));
    gtk_box_append(GTK_BOX(pl_tb), self->entry_filter);

    GtkWidget *pl_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pl_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(pl_scroll, TRUE);

    self->pl_listbox = gtk_list_box_new();
    gtk_widget_add_css_class(self->pl_listbox, "mod-pl-listbox");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->pl_listbox), GTK_SELECTION_SINGLE);
    g_signal_connect(self->pl_listbox, "row-selected", G_CALLBACK(on_pl_row_selected), self);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pl_scroll), self->pl_listbox);
    gtk_box_append(GTK_BOX(lower_card), pl_tb);
    gtk_box_append(GTK_BOX(lower_card), pl_scroll);

    gtk_paned_set_start_child(GTK_PANED(paned), build_upper_inspector_card(self));
    gtk_paned_set_end_child(GTK_PANED(paned), lower_card);
    return paned;
}

static gboolean
on_key_pressed(GtkEventControllerKey *ctrl G_GNUC_UNUSED,
               guint                  keyval,
               guint                  keycode G_GNUC_UNUSED,
               GdkModifierType        state,
               gpointer               user_data)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(user_data);
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(self));
    if (focus && GTK_IS_EDITABLE(focus)) return FALSE;

    if (keyval == GDK_KEY_space) { on_play_toggle_clicked(NULL, self); return TRUE; }
    if ((state & GDK_ALT_MASK) && keyval == GDK_KEY_Left)  { on_sub_prev_clicked(NULL, self); return TRUE; }
    if ((state & GDK_ALT_MASK) && keyval == GDK_KEY_Right) { on_sub_next_clicked(NULL, self); return TRUE; }
    return FALSE;
}

static void
kajo_mod_window_dispose(GObject *object)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(object);
    if (self->tick_id != 0) {
        g_source_remove(self->tick_id);
        self->tick_id = 0;
    }
    if (self->cfg_sub_id != 0) {
        kajo_config_store_unsubscribe(NULL, self->cfg_sub_id);
        self->cfg_sub_id = 0;
    }
    g_clear_pointer(&self->audio, kajo_mod_audio_free);
    g_clear_pointer(&self->playlist, kajo_mod_playlist_free);
    G_OBJECT_CLASS(kajo_mod_window_parent_class)->dispose(object);
}

static void
kajo_mod_window_init(KajoModWindow *self)
{
    kajo_mod_dsp_config_init(&self->dsp);
    apply_shell_config_to_mod(self, FALSE);
    self->audio           = kajo_mod_audio_new();
    self->playlist        = kajo_mod_playlist_new();
    self->active_pl_index = -1;
    self->sub_cur         = 0;
    self->sub_max         = 0;
    self->playing         = FALSE;
    self->sim_col_width   = 0;

    self->cfg_sub_id = kajo_config_store_subscribe(
        NULL, KAJO_CONFIG_DOMAIN_MOD, on_mod_config_changed,
        weak_ref_new(self), weak_ref_free);

    gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
    gtk_window_set_title(GTK_WINDOW(self), "Kajo Mod");
    gtk_widget_add_css_class(GTK_WIDGET(self), "shell-settings-window");
    gtk_widget_add_css_class(GTK_WIDGET(self), "shell-mod-window");

    self->root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(self), self->root_vbox);

    GtkWidget *win_handle = gtk_window_handle_new();
    GtkWidget *hdr_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hdr_bar, "settings-header-bar");

    append_row_label(hdr_bar, "🎹 KAJO MOD", "settings-header-title", -1, 0.0f, FALSE);
    self->hdr_engine_badge = create_badge_label("IDLE • READY", "accent");
    gtk_label_set_ellipsize(GTK_LABEL(self->hdr_engine_badge), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(hdr_bar), self->hdr_engine_badge);

    GtkWidget *hdr_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(hdr_spacer, TRUE);
    gtk_box_append(GTK_BOX(hdr_bar), hdr_spacer);
    gtk_box_append(GTK_BOX(hdr_bar), create_nav_button("🗗 FLOAT / TILE", G_CALLBACK(on_float_tile_clicked), self));

    GtkWidget *btn_close = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(btn_close, "settings-close-btn");
    gtk_widget_set_valign(btn_close, GTK_ALIGN_CENTER);
    g_signal_connect(btn_close, "clicked", G_CALLBACK(on_close_clicked), self);
    gtk_box_append(GTK_BOX(hdr_bar), btn_close);

    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(win_handle), hdr_bar);
    gtk_box_append(GTK_BOX(self->root_vbox), win_handle);
    gtk_box_append(GTK_BOX(self->root_vbox), build_top_fixed_zones(self));
    gtk_box_append(GTK_BOX(self->root_vbox), build_expanding_paned(self));

    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(status_bar, "mod-status-bar");
    append_row_label(status_bar, "● PW 48kHz/S16 │", NULL, -1, 0.0f, FALSE);

    self->lbl_status_engine = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_status_engine), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(self->lbl_status_engine), 0.0f);
    gtk_widget_set_hexpand(self->lbl_status_engine, TRUE);
    gtk_widget_set_valign(self->lbl_status_engine, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(status_bar), self->lbl_status_engine);

    self->bdg_mode = create_badge_label("IDLE", NULL);
    gtk_box_append(GTK_BOX(status_bar), self->bdg_mode);
    append_row_label(status_bar, "SPACE:PLAY  ALT+◀/▶:SUB", NULL, -1, 1.0f, FALSE);
    gtk_box_append(GTK_BOX(self->root_vbox), status_bar);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_ctrl);

    g_signal_connect(self, "realize", G_CALLBACK(on_window_realize), NULL);
    g_signal_connect(self, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(self, "unmap", G_CALLBACK(on_window_unmap), NULL);

    reset_ui_to_idle_state(self);
    rebuild_playlist_rows(self);

    self->tick_id = g_timeout_add(50, on_ui_tick, self);
}

static void
kajo_mod_window_class_init(KajoModWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = kajo_mod_window_dispose;
}

KajoModWindow *
kajo_mod_window_new(GtkApplication *app, int initial_width, int initial_height)
{
    int w = (initial_width >= 480) ? initial_width : 814;
    int h = (initial_height >= 480) ? initial_height : 940;

    KajoModWindow *self = g_object_new(KAJO_TYPE_MOD_WINDOW,
                                       "application", app,
                                       "default-width", w,
                                       "default-height", h,
                                       NULL);
    apply_inspector_responsive_reflow(self, w);
    return self;
}
