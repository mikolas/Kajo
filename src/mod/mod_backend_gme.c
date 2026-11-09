#include "mod_engine.h"
#include "mod_archive.h"
#include <gme/gme.h>
#include <gio/gio.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    Music_Emu *emu;
    gchar     *file_path;
    int        subsong_cur;
    int        subsong_max;
    double     duration_sec;
    double     ch_vu[4];
} KajoGmePriv;

static void
apply_gme_dsp_internal(KajoGmePriv *priv, const KajoModDspConfig *dsp)
{
    if (!priv || !priv->emu || !dsp) return;

    double tempo = CLAMP(dsp->tempo_pct / 100.0, 0.5, 2.0);
    gme_set_tempo(priv->emu, tempo);

    int n_voices = gme_voice_count(priv->emu);
    int mask = 0;
    for (int v = 0; v < n_voices && v < 32; v++) {
        int group = (n_voices <= 4) ? v : (v * 4) / n_voices;
        group = CLAMP(group, 0, 3);
        if (dsp->ch_muted[group]) {
            mask |= (1 << v);
        }
    }
    gme_mute_voices(priv->emu, mask);
}

static void
populate_gme_metadata(KajoGmePriv            *priv,
                      const KajoModDspConfig *dsp,
                      KajoModMetadata        *meta)
{
    g_autofree gchar *base = g_path_get_basename(priv->file_path);
    gme_info_t *info = NULL;
    gme_track_info(priv->emu, &info, priv->subsong_cur - 1);

    const char *raw_song   = (info && info->song && *info->song)     ? info->song   : NULL;
    const char *raw_game   = (info && info->game && *info->game)     ? info->game   : NULL;
    const char *raw_author = (info && info->author && *info->author) ? info->author : "Unknown Artist";
    const char *raw_sys    = (info && info->system && *info->system) ? info->system : "Chiptune Console";
    const char *raw_copy   = (info && info->copyright && *info->copyright) ? info->copyright : "";
    const char *raw_cmt    = (info && info->comment && *info->comment) ? info->comment : "";

    const char *chosen_title = raw_song ? raw_song : (raw_game ? raw_game : base);
    g_autofree gchar *u_title  = kajo_mod_text_to_utf8((const guint8 *)chosen_title, strlen(chosen_title));
    g_autofree gchar *u_author = kajo_mod_text_to_utf8((const guint8 *)raw_author, strlen(raw_author));
    g_autofree gchar *u_game   = raw_game ? kajo_mod_text_to_utf8((const guint8 *)raw_game, strlen(raw_game)) : NULL;
    g_autofree gchar *u_sys    = kajo_mod_text_to_utf8((const guint8 *)raw_sys, strlen(raw_sys));

    if (info && info->length > 0) {
        priv->duration_sec = (double)info->length / 1000.0;
    } else if (info && info->intro_length + info->loop_length > 0) {
        priv->duration_sec = (double)(info->intro_length + info->loop_length * 2) / 1000.0;
    } else {
        priv->duration_sec = kajo_mod_get_fallback_duration_sec();
    }

    int n_voices = gme_voice_count(priv->emu);

    meta->engine = KAJO_MOD_ENGINE_GME;
    g_free(meta->file_path); meta->file_path = g_strdup(priv->file_path);
    g_free(meta->title);     meta->title     = g_ascii_strup((u_title && *u_title) ? u_title : base, -1);

    g_free(meta->subtitle);
    if (u_game && *u_game) {
        meta->subtitle = g_strdup_printf("%s — %s • %s", u_author, u_game, base);
    } else {
        meta->subtitle = g_strdup_printf("%s • %s", u_author, base);
    }

    g_autofree gchar *up_sys = g_ascii_strup(u_sys ? u_sys : "CHIPTUNE", -1);
    g_free(meta->pl_badge);     meta->pl_badge     = g_strdup("LIBGME");
    g_free(meta->pl_badge_css); meta->pl_badge_css = g_strdup("success");
    g_free(meta->hdr_badge);    meta->hdr_badge    = g_strdup_printf("LIBGME • %s", up_sys);
    g_free(meta->bdg_engine);   meta->bdg_engine   = g_strdup("LIBGME 0.6");
    g_free(meta->bdg_format);   meta->bdg_format   = g_strdup(up_sys);
    g_free(meta->bdg_chip);     meta->bdg_chip     = g_strdup_printf("%d-VOICE DSP", n_voices);

    meta->subsong_min  = 1;
    meta->subsong_max  = priv->subsong_max;
    meta->subsong_def  = 1;
    meta->subsong_cur  = priv->subsong_cur;
    meta->duration_sec = priv->duration_sec;

    g_free(meta->cnt_voices); meta->cnt_voices = g_strdup_printf("%02d VOICES", n_voices);
    g_free(meta->cnt_timing); meta->cnt_timing = g_strdup_printf("%.0f%% TEMPO", dsp ? dsp->tempo_pct : 100.0);
    g_free(meta->cnt_clock);  meta->cnt_clock  = g_strdup("CHIP EMU");

    for (int i = 0; i < 4; i++) {
        g_free(meta->ch_names[i]);
        if (n_voices <= 4 && i < n_voices) {
            const char *vn = gme_voice_name(priv->emu, i);
            meta->ch_names[i] = g_ascii_strup(vn ? vn : "VOICE", -1);
        } else {
            int v_start = (i * MAX(4, n_voices)) / 4 + 1;
            int v_end   = ((i + 1) * MAX(4, n_voices)) / 4;
            meta->ch_names[i] = g_strdup_printf("DSP V%d-%d", v_start, v_end);
        }
    }

    meta->filter_enabled = FALSE;
    meta->led_enabled    = FALSE;
    meta->sid_enabled    = FALSE;
    meta->tempo_enabled  = TRUE;

    g_free(meta->sample_hdr_left);  meta->sample_hdr_left  = g_strdup("CHIPTUNE HARDWARE VOICES & TRACKS");
    g_free(meta->sample_hdr_right); meta->sample_hdr_right = g_strdup(up_sys);

    g_array_set_size(meta->samples, 0);
    for (int v = 0; v < n_voices && v < 16; v++) {
        const char *vname = gme_voice_name(priv->emu, v);
        KajoModSampleRow r;
        memset(&r, 0, sizeof(r));
        g_snprintf(r.idx_hex, sizeof(r.idx_hex), "V%d", v + 1);
        g_snprintf(r.ascii_name, sizeof(r.ascii_name), "CHIP VOICE %02d: %-7s",
                   v + 1, vname ? vname : "SYNTH");
        g_snprintf(r.length, sizeof(r.length), "48000Hz");
        g_snprintf(r.loop, sizeof(r.loop), "HW SYNTH");
        g_snprintf(r.vol, sizeof(r.vol), "100%%");
        g_array_append_val(meta->samples, r);
    }

    g_free(meta->stil_hdr_right); meta->stil_hdr_right = g_strdup("GME TAGS");
    g_free(meta->status_engine);  meta->status_engine  = g_strdup_printf("PRIO 3: LIBGME (%s)", up_sys);

    g_free(meta->stil_text);
    meta->stil_text = g_strdup_printf(
        "[LIBGME CHIPTUNE METADATA — %s]\n"
        "System:    %s\n"
        "Game:      %s\n"
        "Track:     %s (%d / %d)\n"
        "Author:    %s\n"
        "Copyright: %s%s%s",
        base,
        u_sys ? u_sys : "Console",
        (u_game && *u_game) ? u_game : "N/A",
        (u_title && *u_title) ? u_title : base,
        priv->subsong_cur,
        priv->subsong_max,
        (u_author && *u_author) ? u_author : "Unknown",
        *raw_copy ? raw_copy : "N/A",
        *raw_cmt ? "\nComment:   " : "",
        *raw_cmt ? raw_cmt : "");

    if (info) gme_free_info(info);
}

static gboolean
gme_open_impl(KajoModEngine          *eng,
              const gchar            *path,
              int                     subsong_1based,
              const KajoModDspConfig *dsp,
              KajoModMetadata        *meta,
              GError                **error)
{
    Music_Emu *emu = NULL;
    gme_err_t err = gme_open_file(path, &emu, KAJO_MOD_SAMPLE_RATE);
    if (err || !emu) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "libgme failed to open %s: %s", path, err ? err : "unknown error");
        return FALSE;
    }

    int count = MAX(1, gme_track_count(emu));
    int sel   = CLAMP(subsong_1based > 0 ? subsong_1based : 1, 1, count);

    if (dsp && dsp->end_action == KAJO_MOD_END_LOOP) {
        gme_ignore_silence(emu, 1);
    }

    err = gme_start_track(emu, sel - 1);
    if (err) {
        gme_delete(emu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "libgme failed to start track: %s", err);
        return FALSE;
    }

    KajoGmePriv *priv = g_new0(KajoGmePriv, 1);
    priv->emu         = emu;
    priv->file_path   = g_strdup(path);
    priv->subsong_cur = sel;
    priv->subsong_max = count;

    apply_gme_dsp_internal(priv, dsp);
    eng->priv = priv;
    populate_gme_metadata(priv, dsp, meta);
    return TRUE;
}

static void
gme_close_impl(KajoModEngine *eng)
{
    KajoGmePriv *priv = (KajoGmePriv *)eng->priv;
    if (!priv) return;
    if (priv->emu) {
        gme_delete(priv->emu);
        priv->emu = NULL;
    }
    g_free(priv->file_path);
    g_free(priv);
    eng->priv = NULL;
}

static int
gme_render_impl(KajoModEngine *eng,
                int16_t       *stereo_frames,
                int            max_frames,
                gboolean      *out_song_ended)
{
    KajoGmePriv *priv = (KajoGmePriv *)eng->priv;
    if (!priv || !priv->emu || max_frames <= 0) return 0;

    gme_err_t err = gme_play(priv->emu, max_frames * 2, stereo_frames);
    if (err) {
        if (out_song_ended) *out_song_ended = TRUE;
        return 0;
    }

    double sum_l = 0.0, sum_r = 0.0, sum_m = 0.0, sum_s = 0.0;
    for (int i = 0; i < max_frames; i++) {
        double nl = (double)stereo_frames[i * 2] / 32768.0;
        double nr = (double)stereo_frames[i * 2 + 1] / 32768.0;
        sum_l += nl * nl;
        sum_r += nr * nr;
        sum_m += (nl + nr) * (nl + nr) * 0.25;
        sum_s += (nl - nr) * (nl - nr) * 0.25;
    }

    double v[4] = {
        CLAMP(sqrt(sum_l / max_frames) * 2.5, 0.0, 1.0),
        CLAMP(sqrt(sum_r / max_frames) * 2.5, 0.0, 1.0),
        CLAMP(sqrt(sum_m / max_frames) * 2.5, 0.0, 1.0),
        CLAMP(sqrt(sum_s / max_frames) * 3.0, 0.0, 1.0),
    };
    for (int i = 0; i < 4; i++) {
        priv->ch_vu[i] = eng->dsp.ch_muted[i] ? 0.0 : (priv->ch_vu[i] * 0.55 + v[i] * 0.45);
    }

    if (out_song_ended && eng->dsp.end_action != KAJO_MOD_END_LOOP) {
        if (gme_track_ended(priv->emu) ||
            (priv->duration_sec > 0.0 && eng->pos_sec >= priv->duration_sec)) {
            *out_song_ended = TRUE;
        }
    }
    return max_frames;
}

static gboolean
gme_seek_impl(KajoModEngine *eng, double target_sec)
{
    KajoGmePriv *priv = (KajoGmePriv *)eng->priv;
    if (!priv || !priv->emu) return FALSE;
    int msec = (int)(MAX(0.0, target_sec) * 1000.0);
    return gme_seek(priv->emu, msec) == NULL;
}

static gboolean
gme_set_subsong_impl(KajoModEngine *eng, int subsong_1based, KajoModMetadata *meta)
{
    KajoGmePriv *priv = (KajoGmePriv *)eng->priv;
    if (!priv || !priv->emu) return FALSE;

    int sel = CLAMP(subsong_1based, 1, priv->subsong_max);
    if (gme_start_track(priv->emu, sel - 1) != NULL) return FALSE;
    priv->subsong_cur = sel;
    apply_gme_dsp_internal(priv, &eng->dsp);
    if (meta) {
        populate_gme_metadata(priv, &eng->dsp, meta);
    }
    return TRUE;
}

static void
gme_apply_dsp_impl(KajoModEngine *eng, const KajoModDspConfig *dsp)
{
    KajoGmePriv *priv = (KajoGmePriv *)eng->priv;
    apply_gme_dsp_internal(priv, dsp);
}

static void
gme_poll_telemetry_impl(KajoModEngine *eng, KajoModTelemetry *out_tel)
{
    KajoGmePriv *priv = (KajoGmePriv *)eng->priv;
    if (!priv || !out_tel) return;
    for (int i = 0; i < 4; i++) {
        out_tel->ch_vu[i] = priv->ch_vu[i];
    }
    g_snprintf(out_tel->cnt_timing, sizeof(out_tel->cnt_timing),
               "%.0f%% TEMPO", eng->dsp.tempo_pct);
}

static const KajoModEngineVTable k_gme_vtable = {
    .kind           = KAJO_MOD_ENGINE_GME,
    .name           = "LIBGME",
    .open           = gme_open_impl,
    .close          = gme_close_impl,
    .render         = gme_render_impl,
    .seek           = gme_seek_impl,
    .set_subsong    = gme_set_subsong_impl,
    .apply_dsp      = gme_apply_dsp_impl,
    .poll_telemetry = gme_poll_telemetry_impl,
};

const KajoModEngineVTable *
kajo_mod_backend_gme_vtable(void)
{
    return &k_gme_vtable;
}
