#ifndef KAJO_MOD_WINDOW_PRIVATE_H
#define KAJO_MOD_WINDOW_PRIVATE_H

/* Internal to src/mod/: KajoModWindow instance struct and helpers shared
 * between mod_window.c and mod_inspector.c. Not a public API. */

#include "mod_window.h"
#include "mod_engine.h"
#include "mod_audio.h"
#include "mod_playlist.h"
#include "../config.h"
#include "../compositor/compositor.h"
#include <string.h>

G_BEGIN_DECLS

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

/* Shared helpers defined in mod_window.c */
GWeakRef  *mod_window_weak_ref_new(gpointer obj);
void       mod_window_weak_ref_free(gpointer data);
void       mod_window_append_row_label(GtkWidget *row, const char *text, const char *css,
                                       int width, float xalign, gboolean hexpand);
GtkWidget *mod_window_create_nav_button(const char *label, GCallback cb, gpointer user_data);
GtkWidget *mod_window_create_badge_label(const char *text, const char *extra_css);
void       mod_window_reset_ui_to_idle(KajoModWindow *self);
void       mod_window_activate_playlist_index(KajoModWindow *self, int idx);

/* Inspector, SID $D400 / sample / STIL & playlist pane (defined in mod_inspector.c) */
void       mod_inspector_populate_rows(KajoModWindow *self, const KajoModSampleRow *rows, int n_rows);
void       mod_inspector_update_live_rows(KajoModWindow *self, const KajoModTelemetry *tel);
void       mod_inspector_apply_responsive_reflow(KajoModWindow *self, int width);
void       mod_inspector_rebuild_playlist_rows(KajoModWindow *self);
GtkWidget *mod_inspector_build_paned(KajoModWindow *self);

G_END_DECLS

#endif /* KAJO_MOD_WINDOW_PRIVATE_H */
