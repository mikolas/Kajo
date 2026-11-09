#include "mod_engine.h"
#include "mod_archive.h"
#include <uade/uade.h>
#include <gio/gio.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    struct uade_state *state;
    gchar             *file_path;
    int                subsong_min;
    int                subsong_max;
    int                subsong_def;
    int                subsong_cur;
    gboolean           led_on;
    gboolean           has_led_notif;
    double             ch_vu[4];
} KajoUadePriv;

static GMutex             s_probe_mutex;
static struct uade_state *s_probe_state = NULL;

gboolean
kajo_mod_uade_probe_file(const gchar *path, gchar **out_ext)
{
    if (out_ext) *out_ext = NULL;
    if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) return FALSE;

    g_mutex_lock(&s_probe_mutex);
    if (!s_probe_state) {
        struct uade_config *uc = uade_new_config();
        if (uc) {
            uade_config_set_option(uc, UC_FREQUENCY, "48000");
            s_probe_state = uade_new_state(uc);
            free(uc);
        }
    }
    int ours = 0;
    if (s_probe_state) {
        ours = uade_is_our_file(path, s_probe_state);
        if (ours == 1 && out_ext) {
            const struct uade_song_info *info = uade_get_song_info(s_probe_state);
            if (info && info->detectioninfo.ext[0] != '\0') {
                *out_ext = g_ascii_strup(info->detectioninfo.ext, -1);
            }
        }
    }
    g_mutex_unlock(&s_probe_mutex);
    return (ours == 1);
}

static struct uade_config *
build_uade_config(const KajoModDspConfig *dsp)
{
    struct uade_config *uc = uade_new_config();
    if (!uc) return NULL;

    uade_config_set_option(uc, UC_FREQUENCY, "48000");

    int sub_tmo = (int)CLAMP(kajo_mod_get_fallback_duration_sec(), 60.0, 600.0);
    if (!dsp || dsp->end_action != KAJO_MOD_END_LOOP) {
        g_autofree gchar *tmo_str = g_strdup_printf("%d", sub_tmo);
        uade_config_set_option(uc, UC_ONE_SUBSONG, NULL);
        uade_config_set_option(uc, UC_SUBSONG_TIMEOUT_VALUE, tmo_str);
    } else {
        uade_config_set_option(uc, UC_NO_EP_END, NULL);
        uade_config_set_option(uc, UC_DISABLE_TIMEOUTS, NULL);
    }

    if (dsp) {
        if (dsp->filter_mode == KAJO_MOD_FILTER_NEAR) {
            uade_config_set_option(uc, UC_RESAMPLER, "none");
            uade_config_set_option(uc, UC_NO_FILTER, NULL);
        } else if (dsp->filter_mode == KAJO_MOD_FILTER_SINC) {
            uade_config_set_option(uc, UC_RESAMPLER, "sinc");
            uade_config_set_option(uc, UC_FILTER_TYPE, "a500");
        } else if (dsp->filter_mode == KAJO_MOD_FILTER_A1200 ||
                   dsp->filter_mode == KAJO_MOD_FILTER_LINEAR) {
            uade_config_set_option(uc, UC_RESAMPLER, "default");
            uade_config_set_option(uc, UC_FILTER_TYPE, "a1200");
        } else {
            uade_config_set_option(uc, UC_RESAMPLER, "default");
            uade_config_set_option(uc, UC_FILTER_TYPE, "a500");
        }

        if (dsp->led_mode == KAJO_MOD_LED_ON) {
            uade_config_set_option(uc, UC_FORCE_LED, "on");
        } else if (dsp->led_mode == KAJO_MOD_LED_OFF) {
            uade_config_set_option(uc, UC_FORCE_LED, "off");
        }
    }
    return uc;
}

static void
populate_uade_samples_and_meta(KajoUadePriv               *priv,
                               const gchar                *path,
                               const struct uade_song_info *info,
                               const KajoModDspConfig     *dsp,
                               KajoModMetadata            *meta)
{
    g_autofree gchar *base = g_path_get_basename(path);
    g_autofree gchar *dir  = g_path_get_dirname(path);

    g_autofree guint8 *raw_data = NULL;
    gsize raw_len = 0;
    g_file_get_contents(path, (gchar **)&raw_data, &raw_len, NULL);

    g_autofree gchar *pt_title = NULL;
    guint mod_ch = 4;
    gboolean is_pt_like = FALSE;
    if (raw_data && raw_len >= 1084) {
        const gchar *sig = (const gchar *)(raw_data + 1080);
        if (memcmp(sig, "M.K.", 4) == 0 || memcmp(sig, "M!K!", 4) == 0 ||
            memcmp(sig, "FLT4", 4) == 0 || memcmp(sig, "4CHN", 4) == 0 ||
            memcmp(sig, "N.T.", 4) == 0 || memcmp(sig, "FLT8", 4) == 0 ||
            memcmp(sig, "OCTA", 4) == 0) {
            is_pt_like = TRUE;
        }
    }

    g_array_set_size(meta->samples, 0);
    if (is_pt_like) {
        kajo_mod_parse_protracker_samples(raw_data, raw_len, meta->samples, &pt_title);
    }

    const gchar *raw_modname = (info && info->modulename[0]) ? info->modulename : NULL;
    g_autofree gchar *utf8_modname = raw_modname
        ? kajo_mod_text_to_utf8((const guint8 *)raw_modname, strlen(raw_modname))
        : NULL;

    const gchar *chosen_title = (utf8_modname && *utf8_modname) ? utf8_modname
                              : (pt_title && *pt_title)         ? pt_title
                              : base;

    const gchar *player_name = (info && info->playername[0]) ? info->playername : "Eagleplayer";
    const gchar *format_name = (info && info->formatname[0]) ? info->formatname : "";
    const gchar *ext_name    = (info && info->detectioninfo.ext[0]) ? info->detectioninfo.ext : "MOD";

    g_free(meta->file_path);
    meta->file_path = g_strdup(path);
    meta->engine    = KAJO_MOD_ENGINE_UADE;

    g_free(meta->title);
    meta->title = g_ascii_strup(chosen_title, -1);

    g_free(meta->subtitle);
    if (*format_name) {
        meta->subtitle = g_strdup_printf("%s (%s) • %s", player_name, format_name, base);
    } else {
        meta->subtitle = g_strdup_printf("%s • %s", player_name, base);
    }

    g_free(meta->pl_badge);     meta->pl_badge     = g_strdup("UADE 68K");
    g_free(meta->pl_badge_css); meta->pl_badge_css = g_strdup("accent");
    g_free(meta->hdr_badge);    meta->hdr_badge    = g_strdup("UADE 68K • PAULA BLEP");
    g_free(meta->bdg_engine);   meta->bdg_engine   = g_strdup("UADE 3.05");

    g_autofree gchar *up_ext = g_ascii_strup(ext_name, -1);
    g_free(meta->bdg_format);
    meta->bdg_format = g_strdup(*format_name ? format_name : up_ext);

    const char *flt_tag = (dsp && (dsp->filter_mode == KAJO_MOD_FILTER_A1200 ||
                                   dsp->filter_mode == KAJO_MOD_FILTER_LINEAR)) ? "A1200" : "A500";
    g_free(meta->bdg_chip);
    if (dsp && (dsp->led_mode == KAJO_MOD_LED_OFF || dsp->filter_mode == KAJO_MOD_FILTER_NEAR)) {
        meta->bdg_chip = g_strdup("LED: OFF");
    } else if (dsp && dsp->led_mode == KAJO_MOD_LED_ON) {
        meta->bdg_chip = g_strdup_printf("LED: ON (%s)", flt_tag);
    } else {
        meta->bdg_chip = g_strdup_printf("LED: AUTO (%s)", flt_tag);
    }

    int total_subs = MAX(1, priv->subsong_max - priv->subsong_min + 1);
    int cur_rel    = CLAMP(priv->subsong_cur - priv->subsong_min + 1, 1, total_subs);
    meta->subsong_min = 1;
    meta->subsong_max = total_subs;
    meta->subsong_def = CLAMP(priv->subsong_def - priv->subsong_min + 1, 1, total_subs);
    meta->subsong_cur = cur_rel;

    meta->duration_sec = (info && info->duration > 0.5)
        ? info->duration
        : kajo_mod_get_fallback_duration_sec();

    g_free(meta->cnt_voices); meta->cnt_voices = g_strdup_printf("%02u PAULA", mod_ch);
    g_free(meta->cnt_timing); meta->cnt_timing = g_strdup("50Hz PAL");
    g_free(meta->cnt_clock);  meta->cnt_clock  = g_strdup("68000 DMA");

    static const gchar * const k_paula_ch[4] = {
        "PAULA 1L", "PAULA 2R", "PAULA 3R", "PAULA 4L"
    };
    for (int i = 0; i < 4; i++) {
        g_free(meta->ch_names[i]);
        meta->ch_names[i] = g_strdup(k_paula_ch[i]);
    }

    meta->filter_enabled = TRUE;
    meta->led_enabled    = TRUE;
    meta->sid_enabled    = FALSE;
    meta->tempo_enabled  = FALSE;

    if (meta->samples->len == 0) {
        /* Populate Eagleplayer / Custom 68k hardware & subsong table when no PT sample header exists */
        g_free(meta->sample_hdr_left);  meta->sample_hdr_left  = g_strdup("UADE 68000 / PAULA TELEMETRY");
        g_free(meta->sample_hdr_right); meta->sample_hdr_right = g_strdup(up_ext);

        KajoModSampleRow r;
        memset(&r, 0, sizeof(r));
        g_snprintf(r.idx_hex, sizeof(r.idx_hex), "EP");
        g_snprintf(r.ascii_name, sizeof(r.ascii_name), "%-22.22s", player_name);
        g_snprintf(r.length, sizeof(r.length), "%lld", (long long)(info ? info->modulebytes : raw_len));
        g_snprintf(r.loop, sizeof(r.loop), "68000 RAM");
        g_snprintf(r.vol, sizeof(r.vol), "64");
        g_array_append_val(meta->samples, r);

        for (int s = priv->subsong_min; s <= priv->subsong_max && (s - priv->subsong_min) < 14; s++) {
            memset(&r, 0, sizeof(r));
            int rel = s - priv->subsong_min + 1;
            g_snprintf(r.idx_hex, sizeof(r.idx_hex), "S%d", rel);
            g_snprintf(r.ascii_name, sizeof(r.ascii_name), "SUBSONG %02d (%s)",
                       rel, (s == priv->subsong_def) ? "DEFAULT" : "TRACK");
            g_snprintf(r.length, sizeof(r.length), "50Hz PAL");
            g_snprintf(r.loop, sizeof(r.loop), "PAULA 4CH");
            g_snprintf(r.vol, sizeof(r.vol), "64");
            g_array_append_val(meta->samples, r);
        }
    } else {
        g_free(meta->sample_hdr_left);
        meta->sample_hdr_left = g_strdup_printf("SAMPLES / INSTRUMENTS (%u SLOTS)", meta->samples->len);
        g_free(meta->sample_hdr_right);
        meta->sample_hdr_right = g_strdup("PAULA 8-BIT PCM");
    }

    g_free(meta->stil_hdr_right);
    meta->stil_hdr_right = g_strdup(base);

    g_free(meta->status_engine);
    meta->status_engine = g_strdup_printf("PRIO 1: UADE 3.05 (%s • PAULA BLEP)", player_name);

    g_free(meta->stil_text);
    meta->stil_text = g_strdup_printf(
        "[UADE EAGLEPLAYER TELEMETRY]\n"
        "File:       %s (%lld bytes)\n"
        "Player:     %s\n"
        "Format:     %s (ext: %s)\n"
        "Subsongs:   %d (Range %d..%d, Default %d)\n"
        "MD5:        %s\n"
        "Hardware:   Amiga 500 PAL (7.09379 MHz 68000 • 4-Channel Paula DMA)",
        base,
        (long long)(info ? info->modulebytes : raw_len),
        player_name,
        *format_name ? format_name : up_ext,
        ext_name,
        total_subs,
        priv->subsong_min,
        priv->subsong_max,
        priv->subsong_def,
        (info && info->modulemd5[0]) ? info->modulemd5 : "N/A");
}

static gboolean
uade_open_impl(KajoModEngine          *eng,
               const gchar            *path,
               int                     subsong_1based,
               const KajoModDspConfig *dsp,
               KajoModMetadata        *meta,
               GError                **error)
{
    struct uade_config *uc = build_uade_config(dsp);
    if (!uc) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to allocate uade_config");
        return FALSE;
    }

    struct uade_state *st = uade_new_state(uc);
    free(uc);
    if (!st) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to initialize uadecore state");
        return FALSE;
    }

    int rc = uade_play(path, -1, st);
    if (rc != 1) {
        uade_cleanup_state(st);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "UADE rejected module: %s", path);
        return FALSE;
    }

    KajoUadePriv *priv = g_new0(KajoUadePriv, 1);
    priv->state     = st;
    priv->file_path = g_strdup(path);
    priv->led_on    = (!dsp || dsp->led_mode != KAJO_MOD_LED_OFF);

    const struct uade_song_info *info = uade_get_song_info(st);
    if (info) {
        priv->subsong_min = info->subsongs.min;
        priv->subsong_max = MAX(info->subsongs.min, info->subsongs.max);
        priv->subsong_def = CLAMP(info->subsongs.def, priv->subsong_min, priv->subsong_max);
        priv->subsong_cur = info->subsongs.cur;
    } else {
        priv->subsong_min = 0;
        priv->subsong_max = 0;
        priv->subsong_def = 0;
        priv->subsong_cur = 0;
    }

    if (subsong_1based > 1) {
        int target = CLAMP(priv->subsong_min + subsong_1based - 1, priv->subsong_min, priv->subsong_max);
        if (target != priv->subsong_cur) {
            uade_seek(UADE_SEEK_SUBSONG_RELATIVE, 0.0, target, st);
            priv->subsong_cur = target;
        }
    }

    eng->priv = priv;
    populate_uade_samples_and_meta(priv, path, info, dsp, meta);
    return TRUE;
}

static void
uade_close_impl(KajoModEngine *eng)
{
    KajoUadePriv *priv = (KajoUadePriv *)eng->priv;
    if (!priv) return;
    if (priv->state) {
        uade_stop(priv->state);
        uade_cleanup_state(priv->state);
        priv->state = NULL;
    }
    g_free(priv->file_path);
    g_free(priv);
    eng->priv = NULL;
}

static int
uade_render_impl(KajoModEngine *eng,
                 int16_t       *stereo_frames,
                 int            max_frames,
                 gboolean      *out_song_ended)
{
    KajoUadePriv *priv = (KajoUadePriv *)eng->priv;
    if (!priv || !priv->state || max_frames <= 0) return 0;

    ssize_t nbytes = uade_read(stereo_frames, (size_t)max_frames * UADE_BYTES_PER_FRAME, priv->state);

    struct uade_notification notif;
    while (uade_read_notification(&notif, priv->state)) {
        if (notif.type == UADE_NOTIFICATION_SONG_END) {
            if (out_song_ended) *out_song_ended = TRUE;
        } else if (notif.type == UADE_NOTIFICATION_MESSAGE && notif.msg) {
            if (strstr(notif.msg, "LED is ON")) {
                priv->led_on        = TRUE;
                priv->has_led_notif = TRUE;
            } else if (strstr(notif.msg, "LED is OFF")) {
                priv->led_on        = FALSE;
                priv->has_led_notif = TRUE;
            }
        }
        uade_cleanup_notification(&notif);
    }

    if (nbytes <= 0) {
        if (out_song_ended) *out_song_ended = TRUE;
        return 0;
    }

    int frames = (int)(nbytes / UADE_BYTES_PER_FRAME);

    /* Apply Paula channel mute (1L/4L = left DAC, 2R/3R = right DAC) & compute live VU */
    double l_gain = 1.0;
    if (eng->dsp.ch_muted[0] && eng->dsp.ch_muted[3]) l_gain = 0.0;
    else if (eng->dsp.ch_muted[0] || eng->dsp.ch_muted[3]) l_gain = 0.5;

    double r_gain = 1.0;
    if (eng->dsp.ch_muted[1] && eng->dsp.ch_muted[2]) r_gain = 0.0;
    else if (eng->dsp.ch_muted[1] || eng->dsp.ch_muted[2]) r_gain = 0.5;

    double sum_l_lo = 0.0, sum_l_hi = 0.0;
    double sum_r_lo = 0.0, sum_r_hi = 0.0;
    double prev_l = 0.0, prev_r = 0.0;

    for (int i = 0; i < frames; i++) {
        int32_t sl = (int32_t)((double)stereo_frames[i * 2] * l_gain);
        int32_t sr = (int32_t)((double)stereo_frames[i * 2 + 1] * r_gain);
        stereo_frames[i * 2]     = (int16_t)CLAMP(sl, -32768, 32767);
        stereo_frames[i * 2 + 1] = (int16_t)CLAMP(sr, -32768, 32767);

        double nl = (double)sl / 32768.0;
        double nr = (double)sr / 32768.0;
        double dl = nl - prev_l;
        double dr = nr - prev_r;
        prev_l = nl;
        prev_r = nr;

        sum_l_lo += nl * nl;
        sum_l_hi += dl * dl;
        sum_r_lo += nr * nr;
        sum_r_hi += dr * dr;
    }

    if (frames > 0) {
        double v1 = eng->dsp.ch_muted[0] ? 0.0 : CLAMP(sqrt(sum_l_lo / frames) * 2.6, 0.0, 1.0);
        double v2 = eng->dsp.ch_muted[1] ? 0.0 : CLAMP(sqrt(sum_r_lo / frames) * 2.6, 0.0, 1.0);
        double v3 = eng->dsp.ch_muted[2] ? 0.0 : CLAMP(sqrt(sum_r_hi / frames) * 3.2, 0.0, 1.0);
        double v4 = eng->dsp.ch_muted[3] ? 0.0 : CLAMP(sqrt(sum_l_hi / frames) * 3.2, 0.0, 1.0);
        priv->ch_vu[0] = priv->ch_vu[0] * 0.55 + v1 * 0.45;
        priv->ch_vu[1] = priv->ch_vu[1] * 0.55 + v2 * 0.45;
        priv->ch_vu[2] = priv->ch_vu[2] * 0.55 + v3 * 0.45;
        priv->ch_vu[3] = priv->ch_vu[3] * 0.55 + v4 * 0.45;
    }

    return frames;
}

static gboolean
uade_seek_impl(KajoModEngine *eng, double target_sec)
{
    KajoUadePriv *priv = (KajoUadePriv *)eng->priv;
    if (!priv || !priv->state) return FALSE;
    return uade_seek(UADE_SEEK_SUBSONG_RELATIVE, target_sec, priv->subsong_cur, priv->state) == 0;
}

static gboolean
uade_set_subsong_impl(KajoModEngine *eng, int subsong_1based, KajoModMetadata *meta)
{
    KajoUadePriv *priv = (KajoUadePriv *)eng->priv;
    if (!priv || !priv->state) return FALSE;

    int target = CLAMP(priv->subsong_min + subsong_1based - 1, priv->subsong_min, priv->subsong_max);
    if (uade_seek(UADE_SEEK_SUBSONG_RELATIVE, 0.0, target, priv->state) != 0) {
        /* Fallback: re-run uade_play for eagleplayers that do not support seek-based subsong change */
        uade_stop(priv->state);
        if (uade_play(priv->file_path, target, priv->state) != 1) {
            return FALSE;
        }
    }
    priv->subsong_cur = target;
    if (meta) {
        meta->subsong_cur = CLAMP(target - priv->subsong_min + 1, 1, MAX(1, priv->subsong_max - priv->subsong_min + 1));
    }
    return TRUE;
}

static void
uade_apply_dsp_impl(KajoModEngine *eng, const KajoModDspConfig *dsp)
{
    KajoUadePriv *priv = (KajoUadePriv *)eng->priv;
    if (!priv || !priv->state || !dsp) return;

    if (dsp->filter_mode == KAJO_MOD_FILTER_NEAR || dsp->led_mode == KAJO_MOD_LED_OFF) {
        uade_set_filter_state(priv->state, 0);
        priv->led_on = FALSE;
    } else if (dsp->led_mode == KAJO_MOD_LED_ON) {
        uade_set_filter_state(priv->state, 1);
        priv->led_on = TRUE;
    }
}

static void
uade_poll_telemetry_impl(KajoModEngine *eng, KajoModTelemetry *out_tel)
{
    KajoUadePriv *priv = (KajoUadePriv *)eng->priv;
    if (!priv || !out_tel) return;
    for (int i = 0; i < 4; i++) {
        out_tel->ch_vu[i] = priv->ch_vu[i];
    }
    const char *flt_tag = (eng->dsp.filter_mode == KAJO_MOD_FILTER_A1200 ||
                           eng->dsp.filter_mode == KAJO_MOD_FILTER_LINEAR) ? "A1200" : "A500";
    if (eng->dsp.filter_mode == KAJO_MOD_FILTER_NEAR || eng->dsp.led_mode == KAJO_MOD_LED_OFF) {
        g_snprintf(out_tel->bdg_chip, sizeof(out_tel->bdg_chip), "LED: OFF");
    } else if (eng->dsp.led_mode == KAJO_MOD_LED_ON) {
        g_snprintf(out_tel->bdg_chip, sizeof(out_tel->bdg_chip), "LED: ON (%s)", flt_tag);
    } else if (priv->has_led_notif) {
        g_snprintf(out_tel->bdg_chip, sizeof(out_tel->bdg_chip),
                   "LED: AUTO (%s %s)", flt_tag, priv->led_on ? "ON" : "OFF");
    } else {
        g_snprintf(out_tel->bdg_chip, sizeof(out_tel->bdg_chip), "LED: AUTO (%s)", flt_tag);
    }
}

static const KajoModEngineVTable k_uade_vtable = {
    .kind           = KAJO_MOD_ENGINE_UADE,
    .name           = "UADE 3.05",
    .open           = uade_open_impl,
    .close          = uade_close_impl,
    .render         = uade_render_impl,
    .seek           = uade_seek_impl,
    .set_subsong    = uade_set_subsong_impl,
    .apply_dsp      = uade_apply_dsp_impl,
    .poll_telemetry = uade_poll_telemetry_impl,
};

const KajoModEngineVTable *
kajo_mod_backend_uade_vtable(void)
{
    return &k_uade_vtable;
}
