#include "mod_engine.h"
#include "mod_archive.h"

#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidTuneInfo.h>
#include <sidplayfp/SidInfo.h>
#include <sidplayfp/SidDatabase.h>
#include <sidplayfp/builders/residfp.h>
#include <stilview/stil.h>

#include <gio/gio.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include <string>

struct KajoSidPriv {
    sidplayfp         engine;
    ReSIDfpBuilder   *residfp;
    SidTune          *tune;
    SidConfig         cfg;
    gchar            *file_path;
    int               subsong_cur;
    int               subsong_max;
    int               subsong_def;
    double            duration_sec;
    uint16_t          load_addr;
    uint16_t          init_addr;
    uint16_t          play_addr;
    gboolean          is_pal;
    gboolean          is_rsid;
    gboolean          tune_is_8580;
    double            ch_vu[4];
    uint8_t           last_regs[32];
    std::vector<int16_t> leftover;
    size_t            leftover_pos;

    KajoSidPriv()
        : residfp(nullptr),
          tune(nullptr),
          file_path(nullptr),
          subsong_cur(1),
          subsong_max(1),
          subsong_def(1),
          duration_sec(180.0),
          load_addr(0),
          init_addr(0),
          play_addr(0),
          is_pal(TRUE),
          is_rsid(FALSE),
          tune_is_8580(FALSE),
          leftover_pos(0)
    {
        memset(ch_vu, 0, sizeof(ch_vu));
        memset(last_regs, 0, sizeof(last_regs));
    }

    ~KajoSidPriv()
    {
        engine.load(nullptr);
        delete tune;
        delete residfp;
        g_free(file_path);
    }
};

/* Walk up from the SID file path or check $HVSC_BASE / ~/C64Music for HVSC root */
static gchar *
find_hvsc_root(const gchar *sid_path)
{
    const gchar *env_hvsc = g_getenv("HVSC_BASE");
    if (env_hvsc && g_file_test(env_hvsc, G_FILE_TEST_IS_DIR)) {
        return g_strdup(env_hvsc);
    }

    if (sid_path) {
        g_autofree gchar *cur = g_path_get_dirname(sid_path);
        for (int depth = 0; depth < 6 && cur && *cur && strcmp(cur, "/") != 0; depth++) {
            g_autofree gchar *doc_stil = g_build_filename(cur, "DOCUMENTS", "STIL.txt", NULL);
            g_autofree gchar *doc_md5  = g_build_filename(cur, "DOCUMENTS", "Songlengths.md5", NULL);
            if (g_file_test(doc_stil, G_FILE_TEST_IS_REGULAR) ||
                g_file_test(doc_md5, G_FILE_TEST_IS_REGULAR)) {
                return g_steal_pointer(&cur);
            }
            gchar *parent = g_path_get_dirname(cur);
            g_free(cur);
            cur = parent;
        }
    }

    const gchar *home = g_get_home_dir();
    if (home) {
        const gchar * const candidates[] = { "C64Music", "Music/C64Music", "HVSC", NULL };
        for (int i = 0; candidates[i] != NULL; i++) {
            g_autofree gchar *cand = g_build_filename(home, candidates[i], NULL);
            if (g_file_test(cand, G_FILE_TEST_IS_DIR)) {
                return g_steal_pointer(&cand);
            }
        }
    }
    return NULL;
}

static double
lookup_hvsc_duration_sec(const gchar *hvsc_root, SidTune *tune)
{
    if (!hvsc_root || !tune) return 0.0;
    g_autofree gchar *md5_path = g_build_filename(hvsc_root, "DOCUMENTS", "Songlengths.md5", NULL);
    if (!g_file_test(md5_path, G_FILE_TEST_IS_REGULAR)) {
        g_free(md5_path);
        md5_path = g_build_filename(hvsc_root, "DOCUMENTS", "Songlengths.txt", NULL);
        if (!g_file_test(md5_path, G_FILE_TEST_IS_REGULAR)) return 0.0;
    }

    SidDatabase db;
    if (!db.open(md5_path)) return 0.0;
    int_least32_t ms = db.lengthMs(*tune);
    db.close();
    if (ms > 0) return (double)ms / 1000.0;
    return 0.0;
}

static gchar *
lookup_hvsc_stil_text(const gchar *hvsc_root, const gchar *sid_path, int subsong_1based)
{
    if (!hvsc_root || !sid_path) return NULL;
    STIL stil;
    if (!stil.setBaseDir(hvsc_root)) return NULL;

    const char *entry   = stil.getAbsEntry(sid_path, subsong_1based, STIL::all);
    const char *bug     = stil.getAbsBug(sid_path, subsong_1based);
    const char *glob_c  = stil.getAbsGlobalComment(sid_path);

    if ((!entry || !*entry) && (!bug || !*bug) && (!glob_c || !*glob_c)) {
        return NULL;
    }

    GString *str = g_string_new(NULL);
    if (glob_c && *glob_c) {
        g_string_append(str, glob_c);
        g_string_append_c(str, '\n');
    }
    if (entry && *entry) {
        g_string_append(str, entry);
    }
    if (bug && *bug) {
        if (str->len > 0) g_string_append_c(str, '\n');
        g_string_append(str, "[HVSC BUG ENTRY]\n");
        g_string_append(str, bug);
    }
    g_autofree gchar *raw = g_string_free(str, FALSE);
    return kajo_mod_text_to_utf8((const guint8 *)raw, strlen(raw));
}

static void
format_sid_voice_row(const uint8_t    regs[32],
                     int              v_idx,
                     KajoModSampleRow *out_row)
{
    memset(out_row, 0, sizeof(*out_row));
    const uint8_t *v = regs + v_idx * 7;
    uint16_t freq = (uint16_t)v[0] | ((uint16_t)v[1] << 8);
    uint16_t pw   = (uint16_t)v[2] | (((uint16_t)(v[3] & 0x0F)) << 8);
    uint8_t  ctrl = v[4];
    uint8_t  ad   = v[5];
    uint8_t  sr   = v[6];

    const char *wave = "IDLE";
    if (ctrl & 0x80)      wave = "NOISE";
    else if (ctrl & 0x40) wave = (ctrl & 0x20) ? "PUL+SAW" : "PULSE PWM";
    else if (ctrl & 0x20) wave = (ctrl & 0x04) ? "SAW+RING" : "SAWTOOTH";
    else if (ctrl & 0x10) wave = (ctrl & 0x04) ? "TRI+RING" : "TRIANGLE";

    g_snprintf(out_row->idx_hex, sizeof(out_row->idx_hex), "V%d", v_idx + 1);
    g_snprintf(out_row->ascii_name, sizeof(out_row->ascii_name),
               "SID VOICE %d: %-9s", v_idx + 1, wave);
    g_snprintf(out_row->length, sizeof(out_row->length), "$D4%02X", (unsigned)(v_idx * 7));
    if (ctrl & 0x40) {
        g_snprintf(out_row->loop, sizeof(out_row->loop), "PW   $%03X", (unsigned)pw);
    } else {
        g_snprintf(out_row->loop, sizeof(out_row->loop), "FREQ $%04X", (unsigned)freq);
    }
    g_snprintf(out_row->vol, sizeof(out_row->vol), "ADSR $%02X%02X", (unsigned)ad, (unsigned)sr);
}

static void
format_sid_filter_row(const uint8_t    regs[32],
                      gboolean         is_8580,
                      KajoModSampleRow *out_row)
{
    memset(out_row, 0, sizeof(*out_row));
    uint16_t fc   = (uint16_t)(regs[21] & 0x07) | ((uint16_t)regs[22] << 3);
    uint8_t  res  = (regs[23] >> 4) & 0x0F;
    uint8_t  mode = regs[24];

    char fmode[12] = "";
    if (mode & 0x10) strcat(fmode, "LP");
    if (mode & 0x20) { if (fmode[0]) strcat(fmode, "+"); strcat(fmode, "BP"); }
    if (mode & 0x40) { if (fmode[0]) strcat(fmode, "+"); strcat(fmode, "HP"); }
    if (!fmode[0])   strcpy(fmode, "BYP");

    g_snprintf(out_row->idx_hex, sizeof(out_row->idx_hex), "FL");
    g_snprintf(out_row->ascii_name, sizeof(out_row->ascii_name),
               "RESIDFP %s FILTER  ", is_8580 ? "8580" : "6581");
    g_snprintf(out_row->length, sizeof(out_row->length), "$D415");
    g_snprintf(out_row->loop, sizeof(out_row->loop), "FC   $%04X", (unsigned)fc);
    g_snprintf(out_row->vol, sizeof(out_row->vol), "%-5s RES%X", fmode, (unsigned)res);
}

static gboolean
configure_sid_engine(KajoSidPriv *priv, const KajoModDspConfig *dsp, GError **error)
{
    priv->cfg.frequency       = KAJO_MOD_SAMPLE_RATE;
    priv->cfg.sidEmulation    = priv->residfp;
    priv->cfg.defaultC64Model = priv->is_pal ? SidConfig::PAL : SidConfig::NTSC;

    if (dsp && dsp->sid_model == KAJO_MOD_SID_6581) {
        priv->cfg.defaultSidModel = SidConfig::MOS6581;
        priv->cfg.forceSidModel   = true;
    } else if (dsp && dsp->sid_model == KAJO_MOD_SID_8580) {
        priv->cfg.defaultSidModel = SidConfig::MOS8580;
        priv->cfg.forceSidModel   = true;
    } else {
        priv->cfg.defaultSidModel = priv->tune_is_8580 ? SidConfig::MOS8580 : SidConfig::MOS6581;
        priv->cfg.forceSidModel   = false;
    }

    if (dsp && dsp->filter_mode == KAJO_MOD_FILTER_SINC) {
        priv->cfg.samplingMethod = SidConfig::RESAMPLE_INTERPOLATE;
    } else {
        priv->cfg.samplingMethod = SidConfig::INTERPOLATE;
    }

    if (!priv->engine.config(priv->cfg)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "libsidplayfp config failed: %s", priv->engine.error());
        return FALSE;
    }

    priv->engine.initMixer(true);
    priv->engine.filter(0, !dsp || dsp->filter_mode != KAJO_MOD_FILTER_NEAR);

    if (dsp) {
        for (unsigned int v = 0; v < 4; v++) {
            priv->engine.mute(0, v, dsp->ch_muted[v]);
        }
    }
    return TRUE;
}

static void
populate_sid_metadata(KajoSidPriv            *priv,
                      const SidTuneInfo      *tinfo,
                      const KajoModDspConfig *dsp,
                      KajoModMetadata        *meta)
{
    g_autofree gchar *base = g_path_get_basename(priv->file_path);

    const char *raw_title    = (tinfo && tinfo->numberOfInfoStrings() > 0) ? tinfo->infoString(0) : base;
    const char *raw_author   = (tinfo && tinfo->numberOfInfoStrings() > 1) ? tinfo->infoString(1) : "Unknown Composer";
    const char *raw_released = (tinfo && tinfo->numberOfInfoStrings() > 2) ? tinfo->infoString(2) : "";

    g_autofree gchar *u_title    = kajo_mod_text_to_utf8((const guint8 *)raw_title, strlen(raw_title));
    g_autofree gchar *u_author   = kajo_mod_text_to_utf8((const guint8 *)raw_author, strlen(raw_author));
    g_autofree gchar *u_released = kajo_mod_text_to_utf8((const guint8 *)raw_released, strlen(raw_released));
    if (u_title)    g_strstrip(u_title);
    if (u_author)   g_strstrip(u_author);
    if (u_released) g_strstrip(u_released);

    meta->engine = KAJO_MOD_ENGINE_SIDPLAYFP;
    g_free(meta->file_path); meta->file_path = g_strdup(priv->file_path);
    g_free(meta->title);     meta->title     = g_ascii_strup((u_title && *u_title) ? u_title : base, -1);

    g_free(meta->subtitle);
    if (u_released && *u_released) {
        meta->subtitle = g_strdup_printf("%s (%s) • %s", u_author, u_released, base);
    } else {
        meta->subtitle = g_strdup_printf("%s • %s", u_author, base);
    }

    gboolean use_8580 = (dsp && dsp->sid_model == KAJO_MOD_SID_8580) ||
                        ((!dsp || dsp->sid_model == KAJO_MOD_SID_AUTO) && priv->tune_is_8580);

    g_free(meta->pl_badge);     meta->pl_badge     = g_strdup("RESIDFP");
    g_free(meta->pl_badge_css); meta->pl_badge_css = g_strdup("warning");
    g_free(meta->hdr_badge);    meta->hdr_badge    = g_strdup(use_8580 ? "LIBSIDPLAYFP • RESIDFP 8580"
                                                                        : "LIBSIDPLAYFP • RESIDFP 6581");
    g_free(meta->bdg_engine);   meta->bdg_engine   = g_strdup("RESIDFP");
    g_free(meta->bdg_format);   meta->bdg_format   = g_strdup_printf("%s %s",
                                                                      priv->is_rsid ? "RSID" : "PSID",
                                                                      priv->is_pal  ? "PAL"  : "NTSC");
    g_free(meta->bdg_chip);     meta->bdg_chip     = g_strdup(use_8580 ? "MOS 8580 R5" : "MOS 6581 R4AR");

    meta->subsong_min  = 1;
    meta->subsong_max  = priv->subsong_max;
    meta->subsong_def  = priv->subsong_def;
    meta->subsong_cur  = priv->subsong_cur;
    meta->duration_sec = priv->duration_sec;

    g_free(meta->cnt_voices); meta->cnt_voices = g_strdup(priv->is_rsid ? "03 SID + DIGI" : "03 SID V1-3");
    g_free(meta->cnt_timing); meta->cnt_timing = g_strdup(priv->is_pal ? "50Hz CIA1" : "60Hz NTSC");
    g_free(meta->cnt_clock);  meta->cnt_clock  = g_strdup_printf("$%04X/$%04X",
                                                                 (unsigned)priv->init_addr,
                                                                 (unsigned)priv->play_addr);

    static const gchar * const k_sid_ch[4] = {
        "SID V1", "SID V2", "SID V3", "SID DIGI"
    };
    for (int i = 0; i < 4; i++) {
        g_free(meta->ch_names[i]);
        meta->ch_names[i] = g_strdup(k_sid_ch[i]);
    }

    meta->filter_enabled = TRUE;
    meta->led_enabled    = FALSE;
    meta->sid_enabled    = TRUE;
    meta->tempo_enabled  = FALSE;

    g_free(meta->sample_hdr_left);  meta->sample_hdr_left  = g_strdup("C64 SID HARDWARE & SUBSONG TABLE");
    g_free(meta->sample_hdr_right); meta->sample_hdr_right = g_strdup_printf("%s @ $D400",
                                                                              use_8580 ? "MOS 8580" : "MOS 6581");

    g_array_set_size(meta->samples, 0);
    KajoModSampleRow r;
    for (int v = 0; v < 3; v++) {
        format_sid_voice_row(priv->last_regs, v, &r);
        g_array_append_val(meta->samples, r);
    }
    format_sid_filter_row(priv->last_regs, use_8580, &r);
    g_array_append_val(meta->samples, r);

    for (int s = 1; s <= priv->subsong_max && s <= 12; s++) {
        memset(&r, 0, sizeof(r));
        g_snprintf(r.idx_hex, sizeof(r.idx_hex), "S%d", s);
        g_snprintf(r.ascii_name, sizeof(r.ascii_name), "SUBSONG %02d (%s)",
                   s, (s == priv->subsong_def) ? "START SONG" : "SUBTUNE");
        g_snprintf(r.length, sizeof(r.length), "$%04X", (unsigned)priv->init_addr);
        g_snprintf(r.loop, sizeof(r.loop), "%s", priv->is_pal ? "CIA1 50Hz" : "CIA1 60Hz");
        g_snprintf(r.vol, sizeof(r.vol), "%s", priv->is_pal ? "PAL C64" : "NTSC C64");
        g_array_append_val(meta->samples, r);
    }

    g_autofree gchar *hvsc_root = find_hvsc_root(priv->file_path);
    g_autofree gchar *stil_entry = lookup_hvsc_stil_text(hvsc_root, priv->file_path, priv->subsong_cur);

    g_free(meta->stil_hdr_right);
    meta->stil_hdr_right = g_strdup(stil_entry ? "HVSC STIL" : "SID HEADER");

    g_free(meta->status_engine);
    meta->status_engine = g_strdup("PRIO 2: LIBSIDPLAYFP (RESIDFP TRANSISTOR FILTER)");

    g_free(meta->stil_text);
    meta->stil_text = g_strdup_printf(
        "[C64 SID HARDWARE HEADER — %s]\n"
        "NAME:     %s\n"
        "AUTHOR:   %s\n"
        "RELEASED: %s\n"
        "FORMAT:   %s (%s • Load $%04X, Init $%04X, Play $%04X)\n"
        "CHIP:     %s ($D400)%s%s",
        base,
        (u_title && *u_title) ? u_title : base,
        (u_author && *u_author) ? u_author : "Unknown",
        (u_released && *u_released) ? u_released : "N/A",
        tinfo ? tinfo->formatString() : "PSID",
        priv->is_pal ? "PAL 985248 Hz" : "NTSC 1022727 Hz",
        (unsigned)priv->load_addr,
        (unsigned)priv->init_addr,
        (unsigned)priv->play_addr,
        use_8580 ? "MOS 8580 R5" : "MOS 6581 R4AR",
        stil_entry ? "\n\n[HVSC STIL COMMENTARY]\n" : "",
        stil_entry ? stil_entry : "");
}

static gboolean
sid_open_impl(KajoModEngine          *eng,
              const gchar            *path,
              int                     subsong_1based,
              const KajoModDspConfig *dsp,
              KajoModMetadata        *meta,
              GError                **error)
{
    SidTune *tune = new SidTune(path);
    if (!tune->getStatus()) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "libsidplayfp rejected file: %s", tune->statusString());
        delete tune;
        return FALSE;
    }

    const SidTuneInfo *tinfo = tune->getInfo();
    int max_songs = tinfo ? (int)tinfo->songs() : 1;
    int def_song  = tinfo ? (int)tinfo->startSong() : 1;
    int sel_song  = (subsong_1based >= 1 && subsong_1based <= max_songs) ? subsong_1based : def_song;
    tune->selectSong((unsigned int)sel_song);

    KajoSidPriv *priv = new KajoSidPriv();
    priv->file_path    = g_strdup(path);
    priv->tune         = tune;
    priv->residfp      = new ReSIDfpBuilder("reSIDfp");
    priv->residfp->filter6581Curve(0.5);
    priv->residfp->filter8580Curve(0.5);

    priv->subsong_max  = MAX(1, max_songs);
    priv->subsong_def  = CLAMP(def_song, 1, priv->subsong_max);
    priv->subsong_cur  = CLAMP(sel_song, 1, priv->subsong_max);

    if (tinfo) {
        priv->load_addr    = tinfo->loadAddr();
        priv->init_addr    = tinfo->initAddr();
        priv->play_addr    = tinfo->playAddr();
        priv->is_pal       = (tinfo->clockSpeed() != SidTuneInfo::CLOCK_NTSC);
        priv->is_rsid      = (tinfo->compatibility() == SidTuneInfo::COMPATIBILITY_R64 ||
                              tinfo->compatibility() == SidTuneInfo::COMPATIBILITY_BASIC);
        priv->tune_is_8580 = (tinfo->sidModel(0) == SidTuneInfo::SIDMODEL_8580);
    }

    g_autofree gchar *hvsc_root = find_hvsc_root(path);
    double hvsc_dur = lookup_hvsc_duration_sec(hvsc_root, tune);
    priv->duration_sec = (hvsc_dur > 1.0) ? hvsc_dur : 180.0;

    if (!priv->engine.load(tune)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "libsidplayfp load failed: %s", priv->engine.error());
        delete priv;
        return FALSE;
    }

    if (!configure_sid_engine(priv, dsp, error)) {
        delete priv;
        return FALSE;
    }

    priv->engine.getSidStatus(0, priv->last_regs);
    eng->priv = priv;
    populate_sid_metadata(priv, tinfo, dsp, meta);
    return TRUE;
}

static void
sid_close_impl(KajoModEngine *eng)
{
    KajoSidPriv *priv = static_cast<KajoSidPriv *>(eng->priv);
    if (!priv) return;
    delete priv;
    eng->priv = nullptr;
}

static int
sid_render_impl(KajoModEngine *eng,
                int16_t       *stereo_frames,
                int            max_frames,
                gboolean      *out_song_ended)
{
    KajoSidPriv *priv = static_cast<KajoSidPriv *>(eng->priv);
    if (!priv || max_frames <= 0) return 0;

    int filled = 0;
    while (filled < max_frames) {
        size_t avail_shorts = priv->leftover.size() - priv->leftover_pos;
        if (avail_shorts >= 2) {
            int avail_frames = (int)(avail_shorts / 2);
            int take = MIN(max_frames - filled, avail_frames);
            memcpy(stereo_frames + filled * 2,
                   priv->leftover.data() + priv->leftover_pos,
                   (size_t)(take * 2) * sizeof(int16_t));
            priv->leftover_pos += (size_t)(take * 2);
            filled += take;
            if (priv->leftover_pos >= priv->leftover.size()) {
                priv->leftover.clear();
                priv->leftover_pos = 0;
            }
            continue;
        }

        int samples = priv->engine.play(2000);
        if (samples <= 0) break;

        priv->leftover.resize((size_t)samples * 2);
        priv->leftover_pos = 0;
        unsigned int mixed = priv->engine.mix(priv->leftover.data(), (unsigned int)samples);
        if (mixed == 0) {
            priv->leftover.clear();
            break;
        }
        priv->leftover.resize(mixed);
    }

    if (priv->engine.getSidStatus(0, priv->last_regs)) {
        double master_vol = (double)(priv->last_regs[24] & 0x0F) / 15.0;
        for (int v = 0; v < 3; v++) {
            const uint8_t *vr = priv->last_regs + v * 7;
            uint16_t freq = (uint16_t)vr[0] | ((uint16_t)vr[1] << 8);
            uint8_t  ctrl = vr[4];
            uint8_t  sr   = vr[6];
            double   sus  = (double)((sr >> 4) & 0x0F) / 15.0;
            double   active = ((ctrl & 0xF0) != 0 && freq > 0x0040) ? 1.0 : 0.0;
            double   gate   = (ctrl & 0x01) ? 1.0 : 0.45;
            double   target = eng->dsp.ch_muted[v] ? 0.0 : CLAMP(active * gate * (0.35 + 0.65 * sus) * master_vol, 0.0, 1.0);
            priv->ch_vu[v]  = priv->ch_vu[v] * 0.65 + target * 0.35;
        }
        double flt_active = ((priv->last_regs[23] & 0x07) != 0) ? 0.80 : 0.25;
        priv->ch_vu[3] = eng->dsp.ch_muted[3] ? 0.0 : (priv->ch_vu[3] * 0.65 + flt_active * master_vol * 0.35);
    }

    if (out_song_ended && eng->dsp.end_action != KAJO_MOD_END_LOOP &&
        priv->duration_sec > 0.0 && eng->pos_sec >= priv->duration_sec) {
        *out_song_ended = TRUE;
    }

    return filled;
}

static gboolean
sid_seek_impl(KajoModEngine *eng, double target_sec)
{
    KajoSidPriv *priv = static_cast<KajoSidPriv *>(eng->priv);
    if (!priv || !priv->tune) return FALSE;

    if (target_sec < eng->pos_sec) {
        priv->engine.load(priv->tune);
        priv->leftover.clear();
        priv->leftover_pos = 0;
        eng->pos_sec = 0.0;
    }

    double delta_sec = target_sec - eng->pos_sec;
    if (delta_sec > 0.05 && delta_sec < 600.0) {
        unsigned int total_cycles = (unsigned int)(delta_sec * (priv->is_pal ? 985248.0 : 1022727.0));
        unsigned int done = 0;
        while (done < total_cycles) {
            unsigned int step = MIN(20000U, total_cycles - done);
            if (priv->engine.play(step) < 0) break;
            done += step;
        }
        priv->leftover.clear();
        priv->leftover_pos = 0;
    }
    return TRUE;
}

static gboolean
sid_set_subsong_impl(KajoModEngine *eng, int subsong_1based, KajoModMetadata *meta)
{
    KajoSidPriv *priv = static_cast<KajoSidPriv *>(eng->priv);
    if (!priv || !priv->tune) return FALSE;

    int target = CLAMP(subsong_1based, 1, priv->subsong_max);
    priv->tune->selectSong((unsigned int)target);
    if (!priv->engine.load(priv->tune)) return FALSE;

    priv->subsong_cur = target;
    priv->leftover.clear();
    priv->leftover_pos = 0;

    g_autofree gchar *hvsc_root = find_hvsc_root(priv->file_path);
    double hvsc_dur = lookup_hvsc_duration_sec(hvsc_root, priv->tune);
    if (hvsc_dur > 1.0) priv->duration_sec = hvsc_dur;

    if (meta) {
        populate_sid_metadata(priv, priv->tune->getInfo(), &eng->dsp, meta);
    }
    return TRUE;
}

static void
sid_apply_dsp_impl(KajoModEngine *eng, const KajoModDspConfig *dsp)
{
    KajoSidPriv *priv = static_cast<KajoSidPriv *>(eng->priv);
    if (!priv || !dsp) return;

    SidConfig::sid_model_t want_model = priv->tune_is_8580 ? SidConfig::MOS8580 : SidConfig::MOS6581;
    bool want_force = false;
    if (dsp->sid_model == KAJO_MOD_SID_6581) {
        want_model = SidConfig::MOS6581;
        want_force = true;
    } else if (dsp->sid_model == KAJO_MOD_SID_8580) {
        want_model = SidConfig::MOS8580;
        want_force = true;
    }

    if (priv->cfg.defaultSidModel != want_model || priv->cfg.forceSidModel != want_force) {
        configure_sid_engine(priv, dsp, NULL);
    } else {
        priv->engine.filter(0, dsp->filter_mode != KAJO_MOD_FILTER_NEAR);
        for (unsigned int v = 0; v < 4; v++) {
            priv->engine.mute(0, v, dsp->ch_muted[v]);
        }
    }
}

static void
sid_poll_telemetry_impl(KajoModEngine *eng, KajoModTelemetry *out_tel)
{
    KajoSidPriv *priv = static_cast<KajoSidPriv *>(eng->priv);
    if (!priv || !out_tel) return;

    for (int i = 0; i < 4; i++) {
        out_tel->ch_vu[i] = priv->ch_vu[i];
    }

    gboolean use_8580 = (eng->dsp.sid_model == KAJO_MOD_SID_8580) ||
                        (eng->dsp.sid_model == KAJO_MOD_SID_AUTO && priv->tune_is_8580);
    g_snprintf(out_tel->bdg_chip, sizeof(out_tel->bdg_chip),
               "%s", use_8580 ? "MOS 8580 R5" : "MOS 6581 R4AR");

    out_tel->has_live_rows = TRUE;
    out_tel->n_live_rows   = 0;
    for (int v = 0; v < 3; v++) {
        format_sid_voice_row(priv->last_regs, v, &out_tel->live_rows[out_tel->n_live_rows++]);
    }
    format_sid_filter_row(priv->last_regs, use_8580, &out_tel->live_rows[out_tel->n_live_rows++]);
}

static const KajoModEngineVTable k_sid_vtable = {
    KAJO_MOD_ENGINE_SIDPLAYFP,
    "LIBSIDPLAYFP",
    sid_open_impl,
    sid_close_impl,
    sid_render_impl,
    sid_seek_impl,
    sid_set_subsong_impl,
    sid_apply_dsp_impl,
    sid_poll_telemetry_impl,
};

extern "C" const KajoModEngineVTable *
kajo_mod_backend_sid_vtable(void)
{
    return &k_sid_vtable;
}
