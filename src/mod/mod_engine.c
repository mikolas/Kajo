#include "mod_engine.h"
#include "mod_archive.h"
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>

void
kajo_mod_dsp_config_init(KajoModDspConfig *dsp)
{
    if (!dsp) return;
    memset(dsp, 0, sizeof(*dsp));
    dsp->filter_mode    = KAJO_MOD_FILTER_A500;
    dsp->led_mode       = KAJO_MOD_LED_AUTO;
    dsp->sid_model      = KAJO_MOD_SID_AUTO;
    dsp->end_action     = KAJO_MOD_END_NEXT_SUB;
    dsp->stereo_sep_pct = 65.0;
    dsp->tempo_pct      = 100.0;
}

void
kajo_mod_metadata_init(KajoModMetadata *meta)
{
    if (!meta) return;
    memset(meta, 0, sizeof(*meta));
    meta->engine      = KAJO_MOD_ENGINE_NONE;
    meta->subsong_cur = 1;
    meta->subsong_min = 1;
    meta->subsong_def = 1;
    meta->subsong_max = 1;
    meta->samples     = g_array_new(FALSE, TRUE, sizeof(KajoModSampleRow));
}

void
kajo_mod_metadata_clear(KajoModMetadata *meta)
{
    if (!meta) return;
    g_clear_pointer(&meta->file_path, g_free);
    g_clear_pointer(&meta->title, g_free);
    g_clear_pointer(&meta->subtitle, g_free);
    g_clear_pointer(&meta->pl_badge, g_free);
    g_clear_pointer(&meta->pl_badge_css, g_free);
    g_clear_pointer(&meta->hdr_badge, g_free);
    g_clear_pointer(&meta->bdg_engine, g_free);
    g_clear_pointer(&meta->bdg_format, g_free);
    g_clear_pointer(&meta->bdg_chip, g_free);
    g_clear_pointer(&meta->cnt_voices, g_free);
    g_clear_pointer(&meta->cnt_timing, g_free);
    g_clear_pointer(&meta->cnt_clock, g_free);
    for (int i = 0; i < 4; i++) {
        g_clear_pointer(&meta->ch_names[i], g_free);
    }
    g_clear_pointer(&meta->sample_hdr_left, g_free);
    g_clear_pointer(&meta->sample_hdr_right, g_free);
    g_clear_pointer(&meta->stil_hdr_right, g_free);
    g_clear_pointer(&meta->status_engine, g_free);
    g_clear_pointer(&meta->stil_text, g_free);
    if (meta->samples) {
        g_array_unref(meta->samples);
        meta->samples = NULL;
    }
}

void
kajo_mod_metadata_copy(KajoModMetadata *dst, const KajoModMetadata *src)
{
    if (!dst || !src) return;
    kajo_mod_metadata_clear(dst);
    kajo_mod_metadata_init(dst);

    dst->engine         = src->engine;
    dst->file_path      = g_strdup(src->file_path);
    dst->title          = g_strdup(src->title);
    dst->subtitle       = g_strdup(src->subtitle);
    dst->pl_badge       = g_strdup(src->pl_badge);
    dst->pl_badge_css   = g_strdup(src->pl_badge_css);
    dst->hdr_badge      = g_strdup(src->hdr_badge);
    dst->bdg_engine     = g_strdup(src->bdg_engine);
    dst->bdg_format     = g_strdup(src->bdg_format);
    dst->bdg_chip       = g_strdup(src->bdg_chip);
    dst->subsong_cur    = src->subsong_cur;
    dst->subsong_min    = src->subsong_min;
    dst->subsong_def    = src->subsong_def;
    dst->subsong_max    = src->subsong_max;
    dst->duration_sec   = src->duration_sec;
    dst->cnt_voices     = g_strdup(src->cnt_voices);
    dst->cnt_timing     = g_strdup(src->cnt_timing);
    dst->cnt_clock      = g_strdup(src->cnt_clock);
    for (int i = 0; i < 4; i++) {
        dst->ch_names[i] = g_strdup(src->ch_names[i]);
    }
    dst->filter_enabled   = src->filter_enabled;
    dst->led_enabled      = src->led_enabled;
    dst->sid_enabled      = src->sid_enabled;
    dst->tempo_enabled    = src->tempo_enabled;
    dst->sample_hdr_left  = g_strdup(src->sample_hdr_left);
    dst->sample_hdr_right = g_strdup(src->sample_hdr_right);
    dst->stil_hdr_right   = g_strdup(src->stil_hdr_right);
    dst->status_engine    = g_strdup(src->status_engine);
    dst->stil_text        = g_strdup(src->stil_text);

    if (src->samples && src->samples->len > 0) {
        g_array_append_vals(dst->samples, src->samples->data, src->samples->len);
    }
}

gboolean
kajo_mod_is_c64_sid_magic(const guint8 *hdr, gsize len)
{
    if (!hdr || len < 4) return FALSE;
    return (memcmp(hdr, "PSID", 4) == 0 || memcmp(hdr, "RSID", 4) == 0);
}

static gboolean
is_gme_magic_or_ext(const guint8 *hdr, gsize len, const gchar *lower_base, gchar **out_hint)
{
    if (hdr && len >= 4) {
        if (len >= 28 && memcmp(hdr, "SNES-SPC700 Sound File Data", 27) == 0) {
            if (out_hint) *out_hint = g_strdup("SNES SPC700");
            return TRUE;
        }
        if (memcmp(hdr, "NESM\x1a", 5) == 0 || memcmp(hdr, "NSFE", 4) == 0) {
            if (out_hint) *out_hint = g_strdup("NES APU (NSF)");
            return TRUE;
        }
        if (memcmp(hdr, "Vgm ", 4) == 0) {
            if (out_hint) *out_hint = g_strdup("SEGA/YM VGM");
            return TRUE;
        }
        if (len >= 8 && memcmp(hdr, "ZXAYEMUL", 8) == 0) {
            if (out_hint) *out_hint = g_strdup("ZX AY-3-8910");
            return TRUE;
        }
        if (len >= 4 && memcmp(hdr, "SAP\r", 4) == 0) {
            if (out_hint) *out_hint = g_strdup("ATARI POKEY SAP");
            return TRUE;
        }
        if (memcmp(hdr, "GBS\x01", 4) == 0) {
            if (out_hint) *out_hint = g_strdup("GAME BOY GBS");
            return TRUE;
        }
        if (memcmp(hdr, "HESM", 4) == 0) {
            if (out_hint) *out_hint = g_strdup("PC ENGINE HES");
            return TRUE;
        }
        if (memcmp(hdr, "KSCC", 4) == 0 || memcmp(hdr, "KSSX", 4) == 0) {
            if (out_hint) *out_hint = g_strdup("MSX KSS");
            return TRUE;
        }
        if (memcmp(hdr, "GYMX", 4) == 0) {
            if (out_hint) *out_hint = g_strdup("GENESIS GYM");
            return TRUE;
        }
    }

    if (!lower_base) return FALSE;
    static const gchar * const k_gme_exts[] = {
        ".spc", ".nsf", ".nsfe", ".vgm", ".vgz",
        ".gbs", ".hes", ".kss", ".sap", ".gym", NULL
    };
    for (int i = 0; k_gme_exts[i] != NULL; i++) {
        if (g_str_has_suffix(lower_base, k_gme_exts[i])) {
            if (out_hint) *out_hint = g_ascii_strup(k_gme_exts[i] + 1, -1);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
is_pc_tracker_magic(const guint8 *hdr, gsize len, const gchar *lower_base, gchar **out_hint)
{
    if (hdr && len >= 48) {
        if (memcmp(hdr, "Extended Module:", 16) == 0) {
            if (out_hint) *out_hint = g_strdup("XM (FT2)");
            return TRUE;
        }
        if (memcmp(hdr, "IMPM", 4) == 0) {
            if (out_hint) *out_hint = g_strdup("IT (IMPULSE)");
            return TRUE;
        }
        if (memcmp(hdr + 44, "SCRM", 4) == 0) {
            if (out_hint) *out_hint = g_strdup("S3M (ST3)");
            return TRUE;
        }
    }

    /* Check if a .mod file has a >4-channel PC tracker signature at offset 1080 */
    if (hdr && len >= 1084) {
        const gchar *sig = (const gchar *)(hdr + 1080);
        if ((sig[0] >= '1' && sig[0] <= '9' && sig[1] >= '0' && sig[1] <= '9' &&
             memcmp(sig + 2, "CH", 2) == 0) ||
            (sig[0] >= '5' && sig[0] <= '9' && memcmp(sig + 1, "CHN", 3) == 0) ||
            memcmp(sig, "TDZ", 3) == 0) {
            /* Note: 6CHN / 8CHN FastTracker I / TakeTracker PC mods:
             * UADE Actually plays some 7/8-voice Amiga formats (StarTrekker FLT8, OctaMED),
             * while 10CH..32CH are strictly PC FastTracker II. */
            if (sig[0] >= '1' && sig[0] <= '9' && sig[1] >= '0' && sig[1] <= '9') {
                if (out_hint) *out_hint = g_strdup("FASTTRACKER MULTICH");
                return TRUE;
            }
        }
    }

    if (!lower_base) return FALSE;
    static const gchar * const k_pc_exts[] = {
        ".xm", ".s3m", ".it", ".mptm", ".stm", ".ult",
        ".far", ".mtm", ".ptm", ".psm", ".amf", ".j2b", ".umx", NULL
    };
    for (int i = 0; k_pc_exts[i] != NULL; i++) {
        if (g_str_has_suffix(lower_base, k_pc_exts[i])) {
            if (out_hint) *out_hint = g_ascii_strup(k_pc_exts[i] + 1, -1);
            return TRUE;
        }
    }
    return FALSE;
}

gboolean
kajo_mod_is_amiga_mod_magic(const guint8 *hdr, gsize len, const gchar *basename, guint *out_channels)
{
    if (out_channels) *out_channels = 4;
    if (hdr && len >= 4) {
        if (memcmp(hdr, "MMD0", 4) == 0 || memcmp(hdr, "MMD1", 4) == 0 ||
            memcmp(hdr, "MMD2", 4) == 0 || memcmp(hdr, "MMD3", 4) == 0) {
            return TRUE;
        }
        if (memcmp(hdr, "THX\x00", 4) == 0 || memcmp(hdr, "THX\x01", 4) == 0) {
            return TRUE;
        }
        if (len >= 8 && (memcmp(hdr, "OKTASONG", 8) == 0 || memcmp(hdr, "TFMX-SONG", 8) == 0 ||
                         memcmp(hdr, "TFMX ", 5) == 0 || memcmp(hdr, "DIGI Boo", 8) == 0)) {
            return TRUE;
        }
    }

    if (hdr && len >= 1084) {
        const gchar *sig = (const gchar *)(hdr + 1080);
        if (memcmp(sig, "M.K.", 4) == 0 || memcmp(sig, "M!K!", 4) == 0 ||
            memcmp(sig, "M&K!", 4) == 0 || memcmp(sig, "FLT4", 4) == 0 ||
            memcmp(sig, "4CHN", 4) == 0 || memcmp(sig, "N.T.", 4) == 0 ||
            memcmp(sig, "FEST", 4) == 0) {
            if (out_channels) *out_channels = 4;
            return TRUE;
        }
        if (memcmp(sig, "FLT8", 4) == 0 || memcmp(sig, "OCTA", 4) == 0 ||
            memcmp(sig, "CD81", 4) == 0 || memcmp(sig, "OKTA", 4) == 0) {
            if (out_channels) *out_channels = 8;
            return TRUE;
        }
    }

    if (!basename) return FALSE;
    g_autofree gchar *lower = g_ascii_strdown(basename, -1);

    /* Classic Amiga prefix.songname and songname.ext conventions */
    static const gchar * const k_amiga_prefixes[] = {
        "mod.", "mdat.", "tfx.", "ahx.", "thx.", "med.", "okt.",
        "hip.", "hip7.", "hipc.", "sog.", "fc.", "fc13.", "fc14.",
        "sid.", "sid1.", "sid2.", "bp.", "bp3.", "sndmon.", "cus.",
        "cust.", "dw.", "np2.", "np3.", "p61.", "pha.", "prt.", "jam.",
        "jmf.", "scumm.", "fred.", "mon.", "rh.", "bd.", "sa.", NULL
    };
    for (int i = 0; k_amiga_prefixes[i] != NULL; i++) {
        if (g_str_has_prefix(lower, k_amiga_prefixes[i])) {
            return TRUE;
        }
    }

    static const gchar * const k_amiga_exts[] = {
        ".mod", ".ahx", ".thx", ".med", ".mmd0", ".mmd1", ".mmd2", ".mmd3",
        ".okt", ".okta", ".digi", ".dbm", ".tfx", ".mdat", ".hip", ".hip7",
        ".hipc", ".fc", ".fc13", ".fc14", ".bp", ".sndmon", ".dw", ".p61",
        ".jam", ".fred", ".mon", ".bd", NULL
    };
    for (int i = 0; k_amiga_exts[i] != NULL; i++) {
        if (g_str_has_suffix(lower, k_amiga_exts[i])) {
            return TRUE;
        }
    }

    return FALSE;
}

void
kajo_mod_parse_protracker_samples(const guint8 *data,
                                  gsize         len,
                                  GArray       *out_samples,
                                  gchar       **out_song_title)
{
    if (!data || len < 600 || !out_samples) return;

    int n_samples = 15;
    if (len >= 1084) {
        const gchar *sig = (const gchar *)(data + 1080);
        gboolean printable = TRUE;
        for (int i = 0; i < 4; i++) {
            if (sig[i] < 0x20 || sig[i] > 0x7e) printable = FALSE;
        }
        if (printable) n_samples = 31;
    }

    if (out_song_title && !*out_song_title) {
        gchar raw_title[21];
        memcpy(raw_title, data, 20);
        raw_title[20] = '\0';
        for (int i = 0; i < 20; i++) {
            if ((guint8)raw_title[i] < 0x20 && raw_title[i] != '\0') raw_title[i] = ' ';
        }
        g_strstrip(raw_title);
        if (raw_title[0] != '\0') {
            *out_song_title = kajo_mod_text_to_utf8((const guint8 *)raw_title, strlen(raw_title));
        }
    }

    for (int i = 0; i < n_samples; i++) {
        gsize off = 20 + (gsize)i * 30;
        if (off + 30 > len) break;

        const guint8 *s = data + off;
        char raw_name[23];
        memcpy(raw_name, s, 22);
        raw_name[22] = '\0';
        for (int c = 0; c < 22; c++) {
            if (raw_name[c] == '\0') break;
            if ((guint8)raw_name[c] < 0x20 || (guint8)raw_name[c] > 0x7e) {
                raw_name[c] = ' ';
            }
        }

        guint32 word_len   = ((guint32)s[22] << 8) | (guint32)s[23];
        guint8  volume     = s[25] > 64 ? 64 : s[25];
        guint32 rep_offset = ((guint32)s[26] << 8) | (guint32)s[27];
        guint32 rep_len    = ((guint32)s[28] << 8) | (guint32)s[29];

        guint32 byte_len   = word_len * 2;
        guint32 loop_start = rep_offset * 2;
        guint32 loop_end   = loop_start + rep_len * 2;

        /* Skip completely empty trailing slots unless they carry a message/title */
        gboolean has_text = (raw_name[0] != '\0');
        if (byte_len == 0 && !has_text) continue;

        KajoModSampleRow row;
        memset(&row, 0, sizeof(row));
        g_snprintf(row.idx_hex, sizeof(row.idx_hex), "%02X", (unsigned)(i + 1));
        g_snprintf(row.ascii_name, sizeof(row.ascii_name), "%-22.22s",
                   has_text ? raw_name : "(UNNAMED SAMPLE)");
        g_snprintf(row.length, sizeof(row.length), "%05u", byte_len);
        if (rep_len > 1 && byte_len > 0) {
            g_snprintf(row.loop, sizeof(row.loop), "%04u..%05u", loop_start, MIN(loop_end, byte_len));
        } else {
            g_snprintf(row.loop, sizeof(row.loop), "NONE");
        }
        g_snprintf(row.vol, sizeof(row.vol), "%02u", volume);
        g_array_append_val(out_samples, row);
    }
}

KajoModEngineKind
kajo_mod_probe_file(const gchar *path, gchar **out_format_hint)
{
    if (out_format_hint) *out_format_hint = NULL;
    if (!path || *path == '\0') return KAJO_MOD_ENGINE_NONE;

    g_autofree gchar *base = g_path_get_basename(path);
    if (kajo_mod_filename_is_companion(base) || kajo_mod_filename_is_nfo(base)) {
        return KAJO_MOD_ENGINE_NONE;
    }

    g_autofree gchar *lower_base = g_ascii_strdown(base, -1);

    /* Read up to 1088 header bytes if the file exists on disk */
    guint8 hdr[1088];
    gsize hdr_len = 0;
    FILE *fp = fopen(path, "rb");
    if (fp) {
        hdr_len = fread(hdr, 1, sizeof(hdr), fp);
        fclose(fp);
    }

    /* 1. Check C64 PSID/RSID magic first so C64 .sid files route to Priority 2
     *    while Amiga SidMon .sid files (without PSID/RSID magic) route to UADE. */
    if (hdr_len >= 4 && kajo_mod_is_c64_sid_magic(hdr, hdr_len)) {
        if (out_format_hint) {
            *out_format_hint = g_strdup(memcmp(hdr, "RSID", 4) == 0 ? "RSID C64" : "PSID C64");
        }
        return KAJO_MOD_ENGINE_SIDPLAYFP;
    }

    /* 2. Check explicit 8/16-bit console chiptune magics/extensions (Priority 3: libgme) */
    gchar *gme_hint = NULL;
    if (is_gme_magic_or_ext(hdr, hdr_len, lower_base, &gme_hint)) {
        if (out_format_hint) *out_format_hint = gme_hint;
        else g_free(gme_hint);
        return KAJO_MOD_ENGINE_GME;
    }

    /* 3. Check explicit PC-only tracker magics/extensions (.xm, .s3m, .it, .mptm, >8ch PC mod) */
    gchar *pc_hint = NULL;
    if (is_pc_tracker_magic(hdr, hdr_len, lower_base, &pc_hint)) {
        if (out_format_hint) *out_format_hint = pc_hint;
        else g_free(pc_hint);
        return KAJO_MOD_ENGINE_OPENMPT;
    }

    /* 4. PRIORITY 1: UADE (Amiga modules, SidMon .sid, TFMX, MED, AHX, ProTracker, Eagleplayers) */
    if (hdr_len > 0) {
        gchar *uade_ext = NULL;
        if (kajo_mod_uade_probe_file(path, &uade_ext)) {
            if (out_format_hint) {
                *out_format_hint = uade_ext ? uade_ext : g_strdup("AMIGA UADE");
            } else {
                g_free(uade_ext);
            }
            return KAJO_MOD_ENGINE_UADE;
        }
    }

    guint mod_ch = 0;
    if (kajo_mod_is_amiga_mod_magic(hdr, hdr_len, base, &mod_ch)) {
        if (out_format_hint) *out_format_hint = g_strdup("AMIGA MOD");
        return KAJO_MOD_ENGINE_UADE;
    }

    /* If extension is .sid and file wasn't on disk yet, inspect prefix: sid.* -> UADE, *.sid -> SIDPLAYFP */
    if (g_str_has_prefix(lower_base, "sid.")) {
        if (out_format_hint) *out_format_hint = g_strdup("SIDMON AMIGA");
        return KAJO_MOD_ENGINE_UADE;
    }
    if (g_str_has_suffix(lower_base, ".sid") || g_str_has_suffix(lower_base, ".mus")) {
        if (out_format_hint) *out_format_hint = g_strdup("C64 SID");
        return KAJO_MOD_ENGINE_SIDPLAYFP;
    }

    /* .ay files that didn't match UADE go to libgme */
    if (g_str_has_suffix(lower_base, ".ay")) {
        if (out_format_hint) *out_format_hint = g_strdup("AY-3-8910");
        return KAJO_MOD_ENGINE_GME;
    }

    /* 5. Priority 4 & 5: libopenmpt / libxmp fallback for remaining tracker formats */
    static const gchar * const k_xmp_exts[] = {
        ".669", ".mdl", ".med", ".okt", ".fnk", ".ims", ".liq", ".mfp", ".rtm", NULL
    };
    for (int i = 0; k_xmp_exts[i] != NULL; i++) {
        if (g_str_has_suffix(lower_base, k_xmp_exts[i])) {
            if (out_format_hint) *out_format_hint = g_strdup("TRACKER");
            return KAJO_MOD_ENGINE_OPENMPT;
        }
    }

    return KAJO_MOD_ENGINE_NONE;
}

static const KajoModEngineVTable *
vtable_for_kind(KajoModEngineKind kind)
{
    switch (kind) {
    case KAJO_MOD_ENGINE_UADE:      return kajo_mod_backend_uade_vtable();
    case KAJO_MOD_ENGINE_SIDPLAYFP: return kajo_mod_backend_sid_vtable();
    case KAJO_MOD_ENGINE_GME:       return kajo_mod_backend_gme_vtable();
    case KAJO_MOD_ENGINE_OPENMPT:   return kajo_mod_backend_openmpt_vtable();
    case KAJO_MOD_ENGINE_XMP:       return kajo_mod_backend_xmp_vtable();
    default:                        return NULL;
    }
}

static void
append_archive_nfo_to_meta(KajoModMetadata *meta,
                           const gchar     *archive_nfo_name,
                           const gchar     *archive_nfo_text)
{
    if (!meta || !archive_nfo_text || *archive_nfo_text == '\0') return;
    g_autofree gchar *prev = meta->stil_text;
    meta->stil_text = g_strdup_printf(
        "%s%s[ARCHIVE INFO — %s]\n%s",
        prev ? prev : "",
        (prev && *prev) ? "\n\n" : "",
        archive_nfo_name ? archive_nfo_name : "README",
        archive_nfo_text);
    if (!meta->stil_hdr_right || *meta->stil_hdr_right == '\0' ||
        g_strcmp0(meta->stil_hdr_right, "SID HEADER") == 0 ||
        g_strcmp0(meta->stil_hdr_right, "MODULE INFO") == 0) {
        g_free(meta->stil_hdr_right);
        meta->stil_hdr_right = g_strdup(archive_nfo_name ? archive_nfo_name : "ARCHIVE NFO");
    }
}

KajoModEngine *
kajo_mod_engine_open(const gchar            *path,
                     int                     subsong_1based,
                     const KajoModDspConfig *dsp,
                     const gchar            *archive_nfo_name,
                     const gchar            *archive_nfo_text,
                     GError                **error)
{
    if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Module file not found: %s", path ? path : "(null)");
        return NULL;
    }

    KajoModDspConfig default_dsp;
    if (!dsp) {
        kajo_mod_dsp_config_init(&default_dsp);
        dsp = &default_dsp;
    }

    KajoModEngineKind primary = kajo_mod_probe_file(path, NULL);

    /* Build ordered candidate list starting with probed primary, then strict priority fallback */
    KajoModEngineKind order[5];
    int n_order = 0;
    if (primary != KAJO_MOD_ENGINE_NONE) {
        order[n_order++] = primary;
    }
    const KajoModEngineKind all_prios[5] = {
        KAJO_MOD_ENGINE_UADE,
        KAJO_MOD_ENGINE_SIDPLAYFP,
        KAJO_MOD_ENGINE_GME,
        KAJO_MOD_ENGINE_OPENMPT,
        KAJO_MOD_ENGINE_XMP
    };
    for (int i = 0; i < 5; i++) {
        gboolean seen = FALSE;
        for (int j = 0; j < n_order; j++) {
            if (order[j] == all_prios[i]) seen = TRUE;
        }
        if (!seen) order[n_order++] = all_prios[i];
    }

    GError *first_err = NULL;
    for (int i = 0; i < n_order; i++) {
        const KajoModEngineVTable *vt = vtable_for_kind(order[i]);
        if (!vt || !vt->open) continue;

        KajoModEngine *eng = g_new0(KajoModEngine, 1);
        eng->vt  = vt;
        eng->dsp = *dsp;
        kajo_mod_metadata_init(&eng->meta);

        GError *sub_err = NULL;
        if (vt->open(eng, path, subsong_1based, dsp, &eng->meta, &sub_err)) {
            g_clear_error(&first_err);
            eng->archive_nfo_name = g_strdup(archive_nfo_name);
            eng->archive_nfo_text = g_strdup(archive_nfo_text);
            append_archive_nfo_to_meta(&eng->meta, eng->archive_nfo_name, eng->archive_nfo_text);
            return eng;
        }

        if (!first_err && sub_err) {
            first_err = sub_err;
        } else {
            g_clear_error(&sub_err);
        }
        kajo_mod_metadata_clear(&eng->meta);
        g_free(eng);
    }

    if (first_err) {
        g_propagate_error(error, first_err);
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "Unsupported module or chiptune format: %s", path);
    }
    return NULL;
}

void
kajo_mod_engine_free(KajoModEngine *eng)
{
    if (!eng) return;
    if (eng->vt && eng->vt->close) {
        eng->vt->close(eng);
    }
    kajo_mod_metadata_clear(&eng->meta);
    g_free(eng->archive_nfo_name);
    g_free(eng->archive_nfo_text);
    g_free(eng);
}

int
kajo_mod_engine_render(KajoModEngine *eng,
                       int16_t       *stereo_frames,
                       int            max_frames,
                       gboolean      *out_song_ended)
{
    if (out_song_ended) *out_song_ended = FALSE;
    if (!eng || !eng->vt || !eng->vt->render || !stereo_frames || max_frames <= 0) return 0;
    int got = eng->vt->render(eng, stereo_frames, max_frames, out_song_ended);
    if (got > 0) {
        eng->pos_sec += (double)got / (double)KAJO_MOD_SAMPLE_RATE;
    }
    return got;
}

gboolean
kajo_mod_engine_seek(KajoModEngine *eng, double target_sec)
{
    if (!eng || !eng->vt || !eng->vt->seek) return FALSE;
    if (target_sec < 0.0) target_sec = 0.0;
    if (eng->vt->seek(eng, target_sec)) {
        eng->pos_sec = target_sec;
        return TRUE;
    }
    return FALSE;
}

gboolean
kajo_mod_engine_set_subsong(KajoModEngine *eng, int subsong_1based)
{
    if (!eng || !eng->vt || !eng->vt->set_subsong) return FALSE;
    if (eng->vt->set_subsong(eng, subsong_1based, &eng->meta)) {
        append_archive_nfo_to_meta(&eng->meta, eng->archive_nfo_name, eng->archive_nfo_text);
        eng->pos_sec = 0.0;
        return TRUE;
    }
    return FALSE;
}

void
kajo_mod_engine_apply_dsp(KajoModEngine *eng, const KajoModDspConfig *dsp)
{
    if (!eng || !dsp) return;
    eng->dsp = *dsp;
    if (eng->vt && eng->vt->apply_dsp) {
        eng->vt->apply_dsp(eng, dsp);
    }
}

void
kajo_mod_engine_poll_telemetry(KajoModEngine *eng, KajoModTelemetry *out_tel)
{
    if (!out_tel) return;
    memset(out_tel, 0, sizeof(*out_tel));
    if (!eng) return;
    out_tel->pos_sec      = eng->pos_sec;
    out_tel->duration_sec = eng->meta.duration_sec;
    if (eng->vt && eng->vt->poll_telemetry) {
        eng->vt->poll_telemetry(eng, out_tel);
    }
}
