#ifndef KAJO_PLAYER_WINDOW_PRIVATE_H
#define KAJO_PLAYER_WINDOW_PRIVATE_H

/* Internal to src/player/: KajoPlayerWindow instance struct and helpers
 * shared between player_window.c and player_popovers.c. Not a public API. */

#include "player_window.h"
#include "player_mpv.h"
#include "player_queue.h"
#include "player_mpris.h"
#include "../compositor/compositor.h"
#include <math.h>
#include <string.h>

G_BEGIN_DECLS

struct _KajoPlayerWindow {
    GtkApplicationWindow parent_instance;

    /* Blueprint Template Children */
    GtkOverlay *root_overlay;
    GtkWidget *body_hbox;
    GtkWidget *header_handle;
    GtkWidget *title_label;
    GtkWidget *stream_badge;
    GtkWidget *btn_stats;
    GtkWidget *btn_open;
    GtkWidget *btn_queue;
    GtkWidget *btn_float;
    GtkWidget *btn_close;

    GtkOverlay *video_overlay;
    GtkGLArea *video_gl_area;
    GtkWidget *empty_state_box;
    GtkWidget *btn_empty_open;
    GtkWidget *audio_state_box;
    GtkWidget *audio_title_label;
    GtkWidget *audio_meta_label;

    GtkWidget *stats_osd_box;
    GtkWidget *stats_osd_label;

    GtkWidget *queue_vbox;
    GtkWidget *chapters_card;
    GtkWidget *chapters_count_badge;
    GtkListBox *chapters_listbox;
    GtkWidget *queue_count_badge;
    GtkListBox *queue_listbox;

    GtkWidget *transport_deck;
    GtkWidget *time_elapsed_lbl;
    GtkWidget *seek_scale;
    GtkWidget *time_total_lbl;

    GtkWidget *btn_prev;
    GtkWidget *btn_seek_back;
    GtkWidget *btn_play_pause;
    GtkWidget *btn_seek_fwd;
    GtkWidget *btn_next;
    GtkWidget *btn_loop;
    GtkWidget *transport_status_lbl;

    /* Speed & Frame Step Popover */
    GtkWidget *btn_speed;
    GtkPopover *speed_popover;
    GtkWidget *speed_badge_lbl;
    GtkWidget *btn_spd_05;
    GtkWidget *btn_spd_075;
    GtkWidget *btn_spd_10;
    GtkWidget *btn_spd_125;
    GtkWidget *btn_spd_15;
    GtkWidget *btn_spd_20;
    GtkWidget *btn_frame_prev;
    GtkWidget *btn_frame_next;
    GtkWidget *btn_ab_loop;

    /* Subtitles Popover */
    GtkWidget *btn_subs;
    GtkPopover *subs_popover;
    GtkWidget *subs_badge_lbl;
    GtkListBox *subs_listbox;
    GtkWidget *btn_load_sub;
    GtkWidget *btn_sub_delay_dec;
    GtkWidget *sub_delay_lbl;
    GtkWidget *btn_sub_delay_inc;
    GtkWidget *btn_sub_delay_reset;
    GtkWidget *btn_sub_scale_dec;
    GtkWidget *sub_scale_lbl;
    GtkWidget *btn_sub_scale_inc;

    /* Audio Streams Popover */
    GtkWidget *btn_audio;
    GtkPopover *audio_popover;
    GtkWidget *audio_badge_lbl;
    GtkListBox *audio_listbox;
    GtkWidget *btn_audio_delay_dec;
    GtkWidget *audio_delay_lbl;
    GtkWidget *btn_audio_delay_inc;
    GtkWidget *btn_audio_delay_reset;

    GtkWidget *btn_screenshot;
    GtkWidget *btn_mute;
    GtkWidget *volume_scale;
    GtkWidget *volume_pct_lbl;
    GtkWidget *btn_fullscreen;

    /* Playback Engine & Controllers */
    KajoPlayerMpv *mpv;
    KajoPlayerQueue *queue;
    KajoPlayerMpris *mpris;

    double volume;
    gboolean muted;
    gboolean loop_enabled;
    gboolean queue_visible;
    gboolean stats_visible;
    gboolean is_fullscreen;
    gboolean syncing_fullscreen;
    gboolean last_eof_reached;
    gint64 last_mpris_duration_usec;

    guint last_tracks_hash;
    guint last_chapters_count;
    int last_chapter_idx;

    char *toast_message;
    guint toast_timer_id;

    gboolean suppress_seek_signal;
    gboolean suppress_vol_signal;
    gboolean suppress_track_select;
    gboolean suppress_chapter_select;

    guint hud_hide_timer_id;
    double last_motion_x;
    double last_motion_y;
};

/* Shared helper defined in player_window.c */
void player_window_show_status_toast(KajoPlayerWindow *self, const char *msg);

/* Popovers, chapters & stats HUD (defined in player_popovers.c) */
gboolean player_popovers_any_open(KajoPlayerWindow *self);
void     player_popovers_rebuild_tracks_ui(KajoPlayerWindow *self);
void     player_popovers_rebuild_chapters_ui(KajoPlayerWindow *self);
void     player_popovers_update_stats_osd(KajoPlayerWindow *self);
void     player_popovers_toggle_stats_osd(KajoPlayerWindow *self);
void     player_popovers_connect_signals(KajoPlayerWindow *self);

G_END_DECLS

#endif /* KAJO_PLAYER_WINDOW_PRIVATE_H */
