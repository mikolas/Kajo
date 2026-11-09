#include "mod_engine.h"
#include "mod_archive.h"
#include <libopenmpt/libopenmpt.h>
#include <libopenmpt/libopenmpt_ext.h>
#include <xmp.h>
#include <gio/gio.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    openmpt_module_ext                       *mod_ext;
    openmpt_module                           *mod;
    openmpt_module_ext_interface_interactive  interactive;
    gboolean                                  has_interactive;
    gchar                                    *file_path;
    int                                       subsong_cur;
    int                                       subsong_max;
    int                                       n_channels;
    int                                       n_orders;
    double                                    duration_sec;
} KajoOpenMptPriv;

static gchar *
dup_openmpt_meta(openmpt_module *mod, const char *key)
{
    const char *val = openmpt_module_get_metadata(mod, key);
    if (!val) return g_strdup("");
    gchar *res = g_strdup(val);
    openmpt_free_string(val);
    return res;
}

static void
apply_openmpt_dsp_internal(KajoOpenMptPriv *priv, const KajoModDspConfig *dsp)
{
    if (!priv || !priv->mod || !dsp) return;

    int interp = 8;
    if (dsp->filter_mode == KAJO_MOD_FILTER_NEAR)        interp = 1;
    else if (dsp->filter_mode == KAJO_MOD_FILTER_LINEAR) interp = 2;
    else if (dsp->filter_mode == KAJO_MOD_FILTER_A500 ||
             dsp->filter_mode == KAJO_MOD_FILTER_A1200)  interp = 4;
    openmpt_module_set_render_param(priv->mod, OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH, interp);

    openmpt_module_set_repeat_count(priv->mod, (dsp->end_action == KAJO_MOD_END_LOOP) ? -1 : 0);

    if (priv->has_interactive) {
        if (priv->interactive.set_tempo_factor) {
            double factor = CLAMP(dsp->tempo_pct / 100.0, 0.5, 2.0);
            priv->interactive.set_tempo_factor(priv->mod_ext, factor);
        }
        if (priv->interactive.set_channel_mute_status) {
            int n_ch = MAX(1, priv->n_channels);
            for (int c = 0; c < n_ch; c++) {
                int group = (n_ch <= 4) ? c : (c * 4) / n_ch;
                group = CLAMP(group, 0, 3);
                priv->interactive.set_channel_mute_status(priv->mod_ext, c, dsp->ch_muted[group] ? 1 : 0);
            }
        }
    }
}

static void
populate_openmpt_metadata(KajoOpenMptPriv        *priv,
                          const KajoModDspConfig *dsp,
                          KajoModMetadata        *meta)
{
    g_autofree gchar *base      = g_path_get_basename(priv->file_path);
    g_autofree gchar *m_title   = dup_openmpt_meta(priv->mod, "title");
    g_autofree gchar *m_artist  = dup_openmpt_meta(priv->mod, "artist");
    g_autofree gchar *m_type    = dup_openmpt_meta(priv->mod, "type");
    g_autofree gchar *m_tracker = dup_openmpt_meta(priv->mod, "tracker");
    g_autofree gchar *m_msg     = dup_openmpt_meta(priv->mod, "message");

    g_autofree gchar *up_type = g_ascii_strup(*m_type ? m_type : "MOD", -1);

    meta->engine = KAJO_MOD_ENGINE_OPENMPT;
    g_free(meta->file_path); meta->file_path = g_strdup(priv->file_path);
    g_free(meta->title);     meta->title     = g_ascii_strup(*m_title ? m_title : base, -1);

    g_free(meta->subtitle);
    if (*m_artist && *m_tracker) {
        meta->subtitle = g_strdup_printf("%s (%s) • %s", m_artist, m_tracker, base);
    } else if (*m_artist) {
        meta->subtitle = g_strdup_printf("%s • %s", m_artist, base);
    } else if (*m_tracker) {
        meta->subtitle = g_strdup_printf("%s • %s", m_tracker, base);
    } else {
        meta->subtitle = g_strdup(base);
    }

    g_free(meta->pl_badge);     meta->pl_badge     = g_strdup("OPENMPT");
    g_free(meta->pl_badge_css); meta->pl_badge_css = g_strdup("");
    g_free(meta->hdr_badge);    meta->hdr_badge    = g_strdup("LIBOPENMPT • TRACKER FALLBACK");
    g_free(meta->bdg_engine);   meta->bdg_engine   = g_strdup("OPENMPT 0.8");
    g_free(meta->bdg_format);   meta->bdg_format   = g_strdup_printf("%s (%dCH)", up_type, priv->n_channels);
    g_free(meta->bdg_chip);
    if (dsp && dsp->filter_mode == KAJO_MOD_FILTER_NEAR) {
        meta->bdg_chip = g_strdup("ZOH RAW");
    } else if (dsp && dsp->filter_mode == KAJO_MOD_FILTER_LINEAR) {
        meta->bdg_chip = g_strdup("LINEAR INT");
    } else if (dsp && (dsp->filter_mode == KAJO_MOD_FILTER_A500 ||
                       dsp->filter_mode == KAJO_MOD_FILTER_A1200)) {
        meta->bdg_chip = g_strdup("CUBIC 4-TAP");
    } else {
        meta->bdg_chip = g_strdup("SINC 8-TAP");
    }

    meta->subsong_min  = 1;
    meta->subsong_max  = priv->subsong_max;
    meta->subsong_def  = 1;
    meta->subsong_cur  = priv->subsong_cur;
    meta->duration_sec = priv->duration_sec;

    int spd = openmpt_module_get_current_speed(priv->mod);
    int bpm = (int)openmpt_module_get_current_tempo2(priv->mod);
    int ord = openmpt_module_get_current_order(priv->mod);

    g_free(meta->cnt_voices); meta->cnt_voices = g_strdup_printf("%02d PCM CH", priv->n_channels);
    g_free(meta->cnt_timing); meta->cnt_timing = g_strdup_printf("SPD %02d/%d", spd, bpm);
    g_free(meta->cnt_clock);  meta->cnt_clock  = g_strdup_printf("ORD %02X/%02X",
                                                                 (unsigned)MAX(0, ord),
                                                                 (unsigned)MAX(1, priv->n_orders));

    int n_ch = MAX(4, priv->n_channels);
    for (int i = 0; i < 4; i++) {
        g_free(meta->ch_names[i]);
        if (priv->n_channels <= 4) {
            meta->ch_names[i] = g_strdup_printf("CH %02d", i + 1);
        } else {
            int c0 = (i * n_ch) / 4 + 1;
            int c1 = ((i + 1) * n_ch) / 4;
            meta->ch_names[i] = g_strdup_printf("CH %02d-%02d", c0, c1);
        }
    }

    meta->filter_enabled = TRUE;
    meta->led_enabled    = FALSE;
    meta->sid_enabled    = FALSE;
    meta->tempo_enabled  = TRUE;

    int n_smp = openmpt_module_get_num_samples(priv->mod);
    g_free(meta->sample_hdr_left);
    meta->sample_hdr_left = g_strdup_printf("SAMPLES / INSTRUMENTS (%d SLOTS)", n_smp);
    g_free(meta->sample_hdr_right);
    meta->sample_hdr_right = g_strdup_printf("%s PCM", up_type);

    g_array_set_size(meta->samples, 0);
    for (int s = 0; s < n_smp && meta->samples->len < 32; s++) {
        const char *sname = openmpt_module_get_sample_name(priv->mod, s);
        g_autofree gchar *u_sname = sname
            ? kajo_mod_text_to_utf8((const guint8 *)sname, strlen(sname))
            : g_strdup("");
        if (sname) openmpt_free_string(sname);

        g_strstrip(u_sname);
        if (u_sname[0] == '\0' && n_smp > 16) continue;

        KajoModSampleRow r;
        memset(&r, 0, sizeof(r));
        g_snprintf(r.idx_hex, sizeof(r.idx_hex), "%02X", (unsigned)(s + 1));
        g_snprintf(r.ascii_name, sizeof(r.ascii_name), "%-22.22s",
                   u_sname[0] ? u_sname : "(SAMPLE SLOT)");
        g_snprintf(r.length, sizeof(r.length), "%s", up_type);
        g_snprintf(r.loop, sizeof(r.loop), "%d CH", priv->n_channels);
        g_snprintf(r.vol, sizeof(r.vol), "64");
        g_array_append_val(meta->samples, r);
    }

    g_free(meta->stil_hdr_right); meta->stil_hdr_right = g_strdup("SONG MESSAGE");
    g_free(meta->status_engine);
    meta->status_engine = g_strdup_printf("PRIO 4: LIBOPENMPT (%s • %s)",
                                          up_type, *m_tracker ? m_tracker : "TRACKER");

    g_free(meta->stil_text);
    meta->stil_text = g_strdup_printf(
        "[LIBOPENMPT TRACKER METADATA — %s]\n"
        "Title:    %s\n"
        "Tracker:  %s (%s, %d Channels, %d Orders, %d Samples)%s%s",
        base,
        *m_title ? m_title : base,
        *m_tracker ? m_tracker : "Tracker",
        up_type,
        priv->n_channels,
        priv->n_orders,
        n_smp,
        *m_msg ? "\n\n[MODULE SONG MESSAGE]\n" : "",
        *m_msg ? m_msg : "");
}

static gboolean
openmpt_open_impl(KajoModEngine          *eng,
                  const gchar            *path,
                  int                     subsong_1based,
                  const KajoModDspConfig *dsp,
                  KajoModMetadata        *meta,
                  GError                **error)
{
    g_autofree guint8 *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, (gchar **)&data, &len, error)) {
        return FALSE;
    }

    int err_code = 0;
    const char *err_msg = NULL;
    openmpt_module_ext *mod_ext = openmpt_module_ext_create_from_memory(
        data, len, openmpt_log_func_silent, NULL,
        openmpt_error_func_ignore, NULL, &err_code, &err_msg, NULL);
    if (!mod_ext) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "libopenmpt rejected module: %s", err_msg ? err_msg : "invalid module");
        if (err_msg) openmpt_free_string(err_msg);
        return FALSE;
    }
    if (err_msg) openmpt_free_string(err_msg);

    openmpt_module *mod = openmpt_module_ext_get_module(mod_ext);

    KajoOpenMptPriv *priv = g_new0(KajoOpenMptPriv, 1);
    priv->mod_ext   = mod_ext;
    priv->mod       = mod;
    priv->file_path = g_strdup(path);

    memset(&priv->interactive, 0, sizeof(priv->interactive));
    if (openmpt_module_ext_get_interface(mod_ext,
                                         LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
                                         &priv->interactive,
                                         sizeof(priv->interactive)) == 1) {
        priv->has_interactive = TRUE;
    }

    int n_subs = MAX(1, openmpt_module_get_num_subsongs(mod));
    int sel    = CLAMP(subsong_1based > 0 ? subsong_1based : 1, 1, n_subs);
    openmpt_module_select_subsong(mod, sel - 1);

    priv->subsong_cur  = sel;
    priv->subsong_max  = n_subs;
    priv->n_channels   = MAX(1, openmpt_module_get_num_channels(mod));
    priv->n_orders     = MAX(1, openmpt_module_get_num_orders(mod));
    priv->duration_sec = openmpt_module_get_duration_seconds(mod);
    if (priv->duration_sec <= 0.5) priv->duration_sec = 180.0;

    apply_openmpt_dsp_internal(priv, dsp);
    eng->priv = priv;
    populate_openmpt_metadata(priv, dsp, meta);
    return TRUE;
}

static void
openmpt_close_impl(KajoModEngine *eng)
{
    KajoOpenMptPriv *priv = (KajoOpenMptPriv *)eng->priv;
    if (!priv) return;
    if (priv->mod_ext) {
        openmpt_module_ext_destroy(priv->mod_ext);
        priv->mod_ext = NULL;
        priv->mod     = NULL;
    }
    g_free(priv->file_path);
    g_free(priv);
    eng->priv = NULL;
}

static int
openmpt_render_impl(KajoModEngine *eng,
                    int16_t       *stereo_frames,
                    int            max_frames,
                    gboolean      *out_song_ended)
{
    KajoOpenMptPriv *priv = (KajoOpenMptPriv *)eng->priv;
    if (!priv || !priv->mod || max_frames <= 0) return 0;

    size_t got = openmpt_module_read_interleaved_stereo(
        priv->mod, KAJO_MOD_SAMPLE_RATE, (size_t)max_frames, stereo_frames);
    if (got == 0) {
        if (out_song_ended) *out_song_ended = TRUE;
        return 0;
    }
    return (int)got;
}

static gboolean
openmpt_seek_impl(KajoModEngine *eng, double target_sec)
{
    KajoOpenMptPriv *priv = (KajoOpenMptPriv *)eng->priv;
    if (!priv || !priv->mod) return FALSE;
    openmpt_module_set_position_seconds(priv->mod, MAX(0.0, target_sec));
    return TRUE;
}

static gboolean
openmpt_set_subsong_impl(KajoModEngine *eng, int subsong_1based, KajoModMetadata *meta)
{
    KajoOpenMptPriv *priv = (KajoOpenMptPriv *)eng->priv;
    if (!priv || !priv->mod) return FALSE;

    int sel = CLAMP(subsong_1based, 1, priv->subsong_max);
    if (!openmpt_module_select_subsong(priv->mod, sel - 1)) return FALSE;
    priv->subsong_cur  = sel;
    priv->duration_sec = openmpt_module_get_duration_seconds(priv->mod);
    if (priv->duration_sec <= 0.5) priv->duration_sec = 180.0;

    if (meta) {
        meta->subsong_cur  = sel;
        meta->duration_sec = priv->duration_sec;
    }
    return TRUE;
}

static void
openmpt_apply_dsp_impl(KajoModEngine *eng, const KajoModDspConfig *dsp)
{
    KajoOpenMptPriv *priv = (KajoOpenMptPriv *)eng->priv;
    apply_openmpt_dsp_internal(priv, dsp);
}

static void
openmpt_poll_telemetry_impl(KajoModEngine *eng, KajoModTelemetry *out_tel)
{
    KajoOpenMptPriv *priv = (KajoOpenMptPriv *)eng->priv;
    if (!priv || !priv->mod || !out_tel) return;

    int n_ch = MAX(1, priv->n_channels);
    double group_max[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (int c = 0; c < n_ch; c++) {
        int group = (n_ch <= 4) ? c : (c * 4) / n_ch;
        group = CLAMP(group, 0, 3);
        float vu = openmpt_module_get_current_channel_vu_mono(priv->mod, c);
        if ((double)vu > group_max[group]) group_max[group] = (double)vu;
    }
    for (int i = 0; i < 4; i++) {
        out_tel->ch_vu[i] = eng->dsp.ch_muted[i] ? 0.0 : CLAMP(group_max[i] * 1.15, 0.0, 1.0);
    }

    int spd = openmpt_module_get_current_speed(priv->mod);
    int bpm = (int)openmpt_module_get_current_tempo2(priv->mod);
    int ord = openmpt_module_get_current_order(priv->mod);
    g_snprintf(out_tel->cnt_timing, sizeof(out_tel->cnt_timing), "SPD %02d/%d", spd, bpm);
    g_snprintf(out_tel->cnt_clock, sizeof(out_tel->cnt_clock), "ORD %02X/%02X",
               (unsigned)MAX(0, ord), (unsigned)MAX(1, priv->n_orders));

    if (eng->dsp.filter_mode == KAJO_MOD_FILTER_NEAR) {
        g_snprintf(out_tel->bdg_chip, sizeof(out_tel->bdg_chip), "ZOH RAW");
    } else if (eng->dsp.filter_mode == KAJO_MOD_FILTER_LINEAR) {
        g_snprintf(out_tel->bdg_chip, sizeof(out_tel->bdg_chip), "LINEAR INT");
    } else if (eng->dsp.filter_mode == KAJO_MOD_FILTER_A500 ||
               eng->dsp.filter_mode == KAJO_MOD_FILTER_A1200) {
        g_snprintf(out_tel->bdg_chip, sizeof(out_tel->bdg_chip), "CUBIC 4-TAP");
    } else {
        g_snprintf(out_tel->bdg_chip, sizeof(out_tel->bdg_chip), "SINC 8-TAP");
    }
}

static const KajoModEngineVTable k_openmpt_vtable = {
    .kind           = KAJO_MOD_ENGINE_OPENMPT,
    .name           = "LIBOPENMPT",
    .open           = openmpt_open_impl,
    .close          = openmpt_close_impl,
    .render         = openmpt_render_impl,
    .seek           = openmpt_seek_impl,
    .set_subsong    = openmpt_set_subsong_impl,
    .apply_dsp      = openmpt_apply_dsp_impl,
    .poll_telemetry = openmpt_poll_telemetry_impl,
};

const KajoModEngineVTable *
kajo_mod_backend_openmpt_vtable(void)
{
    return &k_openmpt_vtable;
}

/* =========================================================================
 * Priority 5 Fallback: libxmp (rare tracker formats not handled by 1..4)
 * ========================================================================= */

typedef struct {
    xmp_context ctx;
    gchar      *file_path;
    int         n_channels;
    int         n_orders;
    int         subsong_max;
    double      duration_sec;
} KajoXmpPriv;

static void
apply_xmp_dsp_internal(KajoXmpPriv *priv, const KajoModDspConfig *dsp)
{
    if (!priv || !priv->ctx || !dsp) return;
    int interp = XMP_INTERP_SPLINE;
    if (dsp->filter_mode == KAJO_MOD_FILTER_NEAR)        interp = XMP_INTERP_NEAREST;
    else if (dsp->filter_mode == KAJO_MOD_FILTER_LINEAR) interp = XMP_INTERP_LINEAR;
    xmp_set_player(priv->ctx, XMP_PLAYER_INTERP, interp);

    int n_ch = MAX(1, priv->n_channels);
    for (int c = 0; c < n_ch && c < XMP_MAX_CHANNELS; c++) {
        int group = (n_ch <= 4) ? c : (c * 4) / n_ch;
        group = CLAMP(group, 0, 3);
        xmp_channel_mute(priv->ctx, c, dsp->ch_muted[group] ? 1 : 0);
    }
}

static gboolean
xmp_open_impl(KajoModEngine          *eng,
              const gchar            *path,
              int                     subsong_1based,
              const KajoModDspConfig *dsp,
              KajoModMetadata        *meta,
              GError                **error)
{
    xmp_context ctx = xmp_create_context();
    if (!ctx) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "xmp_create_context failed");
        return FALSE;
    }

    if (xmp_load_module(ctx, (char *)path) != 0) {
        xmp_free_context(ctx);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "libxmp rejected module: %s", path);
        return FALSE;
    }

    if (xmp_start_player(ctx, KAJO_MOD_SAMPLE_RATE, 0) != 0) {
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "xmp_start_player failed");
        return FALSE;
    }

    struct xmp_module_info minfo;
    memset(&minfo, 0, sizeof(minfo));
    xmp_get_module_info(ctx, &minfo);

    int n_seqs = MAX(1, minfo.num_sequences);
    int sel    = CLAMP(subsong_1based > 0 ? subsong_1based : 1, 1, n_seqs);
    if (sel > 1 && minfo.seq_data) {
        xmp_set_position(ctx, minfo.seq_data[sel - 1].entry_point);
    }

    KajoXmpPriv *priv = g_new0(KajoXmpPriv, 1);
    priv->ctx          = ctx;
    priv->file_path    = g_strdup(path);
    priv->n_channels   = (minfo.mod && minfo.mod->chn > 0) ? minfo.mod->chn : 4;
    priv->n_orders     = (minfo.mod && minfo.mod->len > 0) ? minfo.mod->len : 1;
    priv->subsong_max  = n_seqs;
    priv->duration_sec = (minfo.seq_data && minfo.seq_data[sel - 1].duration > 0)
                         ? (double)minfo.seq_data[sel - 1].duration / 1000.0
                         : 180.0;

    apply_xmp_dsp_internal(priv, dsp);
    eng->priv = priv;

    g_autofree gchar *base = g_path_get_basename(path);
    const char *m_name = (minfo.mod && minfo.mod->name[0]) ? minfo.mod->name : base;
    const char *m_type = (minfo.mod && minfo.mod->type[0]) ? minfo.mod->type : "XMP MOD";

    meta->engine = KAJO_MOD_ENGINE_XMP;
    g_free(meta->file_path); meta->file_path = g_strdup(path);
    g_free(meta->title);     meta->title     = g_ascii_strup(m_name, -1);
    g_free(meta->subtitle);  meta->subtitle  = g_strdup_printf("%s • %s", m_type, base);
    g_free(meta->pl_badge);  meta->pl_badge  = g_strdup("LIBXMP");
    g_free(meta->pl_badge_css); meta->pl_badge_css = g_strdup("");
    g_free(meta->hdr_badge); meta->hdr_badge = g_strdup("LIBXMP • TRACKER FALLBACK");
    g_free(meta->bdg_engine); meta->bdg_engine = g_strdup("LIBXMP 4.6");
    g_free(meta->bdg_format); meta->bdg_format = g_strdup(m_type);
    g_free(meta->bdg_chip);   meta->bdg_chip   = g_strdup("SPLINE INT");
    meta->subsong_min  = 1;
    meta->subsong_max  = n_seqs;
    meta->subsong_def  = 1;
    meta->subsong_cur  = sel;
    meta->duration_sec = priv->duration_sec;

    g_free(meta->cnt_voices); meta->cnt_voices = g_strdup_printf("%02d PCM CH", priv->n_channels);
    g_free(meta->cnt_timing); meta->cnt_timing = g_strdup_printf("SPD %02d/%d",
                                                                 minfo.mod ? minfo.mod->spd : 6,
                                                                 minfo.mod ? minfo.mod->bpm : 125);
    g_free(meta->cnt_clock);  meta->cnt_clock  = g_strdup_printf("ORD 00/%02X", (unsigned)priv->n_orders);
    for (int i = 0; i < 4; i++) {
        g_free(meta->ch_names[i]);
        meta->ch_names[i] = g_strdup_printf("CH %02d", i + 1);
    }
    meta->filter_enabled = TRUE;
    meta->led_enabled    = FALSE;
    meta->sid_enabled    = FALSE;
    meta->tempo_enabled  = FALSE;

    int n_smp = (minfo.mod && minfo.mod->smp > 0) ? minfo.mod->smp : 0;
    g_array_set_size(meta->samples, 0);
    if (minfo.mod && minfo.mod->xxs) {
        for (int s = 0; s < n_smp && meta->samples->len < 32; s++) {
            const struct xmp_sample *xs = &minfo.mod->xxs[s];
            if (xs->len <= 0 && xs->name[0] == '\0') continue;
            KajoModSampleRow r;
            memset(&r, 0, sizeof(r));
            g_snprintf(r.idx_hex, sizeof(r.idx_hex), "%02X", (unsigned)(s + 1));
            g_snprintf(r.ascii_name, sizeof(r.ascii_name), "%-22.22s", xs->name[0] ? xs->name : "(SAMPLE)");
            g_snprintf(r.length, sizeof(r.length), "%05d", xs->len);
            if (xs->flg & XMP_SAMPLE_LOOP) {
                g_snprintf(r.loop, sizeof(r.loop), "%04d..%05d", xs->lps, xs->lpe);
            } else {
                g_snprintf(r.loop, sizeof(r.loop), "ONE-SHOT");
            }
            g_snprintf(r.vol, sizeof(r.vol), "PCM");
            g_array_append_val(meta->samples, r);
        }
    }

    g_free(meta->sample_hdr_left);
    meta->sample_hdr_left = g_strdup_printf("SAMPLES / INSTRUMENTS (%d SLOTS)", n_smp);
    g_free(meta->sample_hdr_right); meta->sample_hdr_right = g_strdup("LIBXMP PCM");
    g_free(meta->stil_hdr_right);   meta->stil_hdr_right   = g_strdup(base);
    g_free(meta->status_engine);    meta->status_engine    = g_strdup_printf("PRIO 5: LIBXMP (%s)", m_type);
    g_free(meta->stil_text);
    meta->stil_text = g_strdup_printf("[LIBXMP MODULE — %s]\nFormat: %s\nComment: %s",
                                      base, m_type, minfo.comment ? minfo.comment : "None");
    return TRUE;
}

static void
xmp_close_impl(KajoModEngine *eng)
{
    KajoXmpPriv *priv = (KajoXmpPriv *)eng->priv;
    if (!priv) return;
    if (priv->ctx) {
        xmp_end_player(priv->ctx);
        xmp_release_module(priv->ctx);
        xmp_free_context(priv->ctx);
        priv->ctx = NULL;
    }
    g_free(priv->file_path);
    g_free(priv);
    eng->priv = NULL;
}

static int
xmp_render_impl(KajoModEngine *eng,
                int16_t       *stereo_frames,
                int            max_frames,
                gboolean      *out_song_ended)
{
    KajoXmpPriv *priv = (KajoXmpPriv *)eng->priv;
    if (!priv || !priv->ctx || max_frames <= 0) return 0;

    int loop = (eng->dsp.end_action == KAJO_MOD_END_LOOP) ? 0 : 1;
    int rc = xmp_play_buffer(priv->ctx, stereo_frames, max_frames * 4, loop);
    if (rc != 0) {
        if (out_song_ended) *out_song_ended = TRUE;
        return 0;
    }
    return max_frames;
}

static gboolean
xmp_seek_impl(KajoModEngine *eng, double target_sec)
{
    KajoXmpPriv *priv = (KajoXmpPriv *)eng->priv;
    if (!priv || !priv->ctx) return FALSE;
    return xmp_seek_time(priv->ctx, (int)(MAX(0.0, target_sec) * 1000.0)) >= 0;
}

static gboolean
xmp_set_subsong_impl(KajoModEngine *eng, int subsong_1based, KajoModMetadata *meta)
{
    KajoXmpPriv *priv = (KajoXmpPriv *)eng->priv;
    if (!priv || !priv->ctx) return FALSE;

    struct xmp_module_info minfo;
    memset(&minfo, 0, sizeof(minfo));
    xmp_get_module_info(priv->ctx, &minfo);

    int sel = CLAMP(subsong_1based, 1, priv->subsong_max);
    if (minfo.seq_data) {
        xmp_set_position(priv->ctx, minfo.seq_data[sel - 1].entry_point);
        if (minfo.seq_data[sel - 1].duration > 0) {
            priv->duration_sec = (double)minfo.seq_data[sel - 1].duration / 1000.0;
        }
    }
    if (meta) {
        meta->subsong_cur  = sel;
        meta->duration_sec = priv->duration_sec;
    }
    return TRUE;
}

static void
xmp_apply_dsp_impl(KajoModEngine *eng, const KajoModDspConfig *dsp)
{
    KajoXmpPriv *priv = (KajoXmpPriv *)eng->priv;
    apply_xmp_dsp_internal(priv, dsp);
}

static void
xmp_poll_telemetry_impl(KajoModEngine *eng, KajoModTelemetry *out_tel)
{
    KajoXmpPriv *priv = (KajoXmpPriv *)eng->priv;
    if (!priv || !priv->ctx || !out_tel) return;

    struct xmp_frame_info finfo;
    memset(&finfo, 0, sizeof(finfo));
    xmp_get_frame_info(priv->ctx, &finfo);

    int n_ch = MAX(1, priv->n_channels);
    double group_max[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (int c = 0; c < n_ch && c < XMP_MAX_CHANNELS; c++) {
        int group = (n_ch <= 4) ? c : (c * 4) / n_ch;
        group = CLAMP(group, 0, 3);
        double v = (double)finfo.channel_info[c].volume / 64.0;
        if (v > group_max[group]) group_max[group] = v;
    }
    for (int i = 0; i < 4; i++) {
        out_tel->ch_vu[i] = eng->dsp.ch_muted[i] ? 0.0 : CLAMP(group_max[i], 0.0, 1.0);
    }

    g_snprintf(out_tel->cnt_timing, sizeof(out_tel->cnt_timing), "SPD %02d/%d", finfo.speed, finfo.bpm);
    g_snprintf(out_tel->cnt_clock, sizeof(out_tel->cnt_clock), "ORD %02X/%02X",
               (unsigned)MAX(0, finfo.pos), (unsigned)MAX(1, priv->n_orders));
}

static const KajoModEngineVTable k_xmp_vtable = {
    .kind           = KAJO_MOD_ENGINE_XMP,
    .name           = "LIBXMP",
    .open           = xmp_open_impl,
    .close          = xmp_close_impl,
    .render         = xmp_render_impl,
    .seek           = xmp_seek_impl,
    .set_subsong    = xmp_set_subsong_impl,
    .apply_dsp      = xmp_apply_dsp_impl,
    .poll_telemetry = xmp_poll_telemetry_impl,
};

const KajoModEngineVTable *
kajo_mod_backend_xmp_vtable(void)
{
    return &k_xmp_vtable;
}
