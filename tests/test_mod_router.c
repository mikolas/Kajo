#include <glib.h>
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>
#include <string.h>
#include <stdio.h>
#include "../src/mod/mod_engine.h"
#include "../src/mod/mod_archive.h"
#include "../src/mod/mod_playlist.h"

/* Build a minimal valid 4-channel ProTracker M.K. module in memory */
static GBytes *
build_synthetic_amiga_mod(void)
{
    /* 1084-byte header + 1 pattern (1024 bytes) + 64 bytes sample data = 2172 bytes */
    gsize total = 1084 + 1024 + 64;
    guint8 *buf = g_malloc0(total);

    /* Song title (20 bytes) */
    memcpy(buf, "TURRICAN TEST MOD", 17);

    /* Sample 1 at offset 20 (30 bytes) */
    memcpy(buf + 20, "PAULA LEAD SYNTH", 16);
    buf[20 + 22] = 0x00;
    buf[20 + 23] = 32;   /* 32 words = 64 bytes */
    buf[20 + 24] = 0;    /* finetune */
    buf[20 + 25] = 64;   /* volume 64 */
    buf[20 + 26] = 0x00;
    buf[20 + 27] = 4;    /* repeat offset = 4 words = 8 bytes */
    buf[20 + 28] = 0x00;
    buf[20 + 29] = 28;   /* repeat len = 28 words = 56 bytes */

    /* Sample 2 at offset 50 (30 bytes) */
    memcpy(buf + 50, "BASS DRUM PUNCH", 15);
    buf[50 + 22] = 0x00;
    buf[50 + 23] = 0;    /* 0 words (metadata-only slot) */
    buf[50 + 25] = 48;
    buf[50 + 29] = 1;

    /* Song length = 1 order at offset 950, restart = 127 at 951 */
    buf[950] = 1;
    buf[951] = 127;
    buf[952] = 0; /* Order 0 -> Pattern 0 */

    /* Magic "M.K." at offset 1080 */
    memcpy(buf + 1080, "M.K.", 4);

    /* Pattern 0 row 0 channel 0: Sample 1, Period 428 (C-2), Effect 0 */
    guint8 *row0 = buf + 1084;
    row0[0] = 0x10 | ((428 >> 8) & 0x0F);
    row0[1] = 428 & 0xFF;
    row0[2] = 0x10;
    row0[3] = 0x00;

    /* Sample 1 waveform (64 bytes square/saw wave) */
    for (int i = 0; i < 64; i++) {
        buf[1084 + 1024 + i] = (guint8)((i < 32) ? 60 : -60);
    }

    return g_bytes_new_take(buf, total);
}

/* Build a minimal valid C64 PSID v2 file with a real 6502 init/play routine */
static GBytes *
build_synthetic_c64_sid(void)
{
    /* 124-byte PSID v2 header + 2-byte load addr ($1000) + 32 bytes 6502 code */
    gsize total = 124 + 2 + 32;
    guint8 *buf = g_malloc0(total);

    memcpy(buf, "PSID", 4);
    buf[4] = 0x00; buf[5] = 0x02; /* version 2 */
    buf[6] = 0x00; buf[7] = 0x7C; /* data offset 124 */
    buf[8] = 0x00; buf[9] = 0x00; /* load addr 0 (in data) */
    buf[10] = 0x10; buf[11] = 0x00; /* init addr $1000 */
    buf[12] = 0x10; buf[13] = 0x12; /* play addr $1012 */
    buf[14] = 0x00; buf[15] = 0x02; /* 2 subsongs */
    buf[16] = 0x00; buf[17] = 0x01; /* start song 1 */

    strcpy((char *)(buf + 0x16), "Commando Test Tune");
    strcpy((char *)(buf + 0x36), "Rob Hubbard");
    strcpy((char *)(buf + 0x56), "1985 Elite");

    /* Flags at 0x76..0x77: PAL (bit 2 = 1 -> 0x04) + MOS 6581 (bit 4 = 1 -> 0x10) = 0x14 */
    buf[0x76] = 0x00;
    buf[0x77] = 0x14;

    /* Little-endian load address $1000 */
    buf[124] = 0x00;
    buf[125] = 0x10;

    /* 6502 Init routine at $1000:
     * A9 0F       LDA #$0F
     * 8D 18 D4    STA $D418  (Volume 15)
     * A9 1C       LDA #$1C
     * 8D 01 D4    STA $D401  (Voice 1 Freq Hi = $1C)
     * A9 F0       LDA #$F0
     * 8D 06 D4    STA $D406  (Voice 1 SR = $F0)
     * A9 21       LDA #$21
     * 8D 04 D4    STA $D404  (Voice 1 Sawtooth + Gate)
     * 60          RTS
     * Play routine at $1012:
     * EE 00 D4    INC $D400  (Modulate Voice 1 Freq Lo)
     * 60          RTS
     */
    static const guint8 k_6502_code[] = {
        0xA9, 0x0F, 0x8D, 0x18, 0xD4,
        0xA9, 0x1C, 0x8D, 0x01, 0xD4,
        0xA9, 0xF0, 0x8D, 0x06, 0xD4,
        0xA9, 0x21, 0x8D, 0x04, 0xD4,
        0x60, 0xEA,
        /* $1012: */
        0xEE, 0x00, 0xD4, 0x60
    };
    memcpy(buf + 126, k_6502_code, sizeof(k_6502_code));

    return g_bytes_new_take(buf, total);
}

static void
test_priority_routing_and_disambiguation(void)
{
    gchar *tmp_dir = g_dir_make_tmp("kajo-mod-test-XXXXXX", NULL);
    g_assert_nonnull(tmp_dir);

    /* 1. Companion & NFO classification */
    g_assert_true(kajo_mod_filename_is_companion("smpl.turrican2"));
    g_assert_true(kajo_mod_filename_is_companion("smp.wings_of_death"));
    g_assert_true(kajo_mod_filename_is_companion("ins.song"));
    g_assert_false(kajo_mod_filename_is_companion("mdat.turrican2"));
    g_assert_true(kajo_mod_filename_is_nfo("FILE_ID.DIZ"));
    g_assert_true(kajo_mod_filename_is_nfo("turrican2.readme"));

    /* 2. Amiga ProTracker MOD -> Priority 1 (UADE) */
    g_autoptr(GBytes) mod_bytes = build_synthetic_amiga_mod();
    g_autofree gchar *mod_path = g_build_filename(tmp_dir, "mod.turrican_test", NULL);
    gsize mod_len = 0;
    const guint8 *mod_data = g_bytes_get_data(mod_bytes, &mod_len);
    g_assert_true(g_file_set_contents(mod_path, (const gchar *)mod_data, (gssize)mod_len, NULL));
    g_assert_cmpint(kajo_mod_probe_file(mod_path, NULL), ==, KAJO_MOD_ENGINE_UADE);

    /* 3. C64 PSID .sid -> Priority 2 (libsidplayfp) */
    g_autoptr(GBytes) sid_bytes = build_synthetic_c64_sid();
    g_autofree gchar *sid_path = g_build_filename(tmp_dir, "Commando.sid", NULL);
    gsize sid_len = 0;
    const guint8 *sid_data = g_bytes_get_data(sid_bytes, &sid_len);
    g_assert_true(g_file_set_contents(sid_path, (const gchar *)sid_data, (gssize)sid_len, NULL));
    g_assert_cmpint(kajo_mod_probe_file(sid_path, NULL), ==, KAJO_MOD_ENGINE_SIDPLAYFP);

    /* 4. Amiga SidMon file (sid.monster without PSID/RSID magic) -> Priority 1 (UADE) */
    g_autofree gchar *sidmon_path = g_build_filename(tmp_dir, "sid.monster_beat", NULL);
    const char sidmon_hdr[] = "SIDMON II - THE MIDI VERSION";
    g_assert_true(g_file_set_contents(sidmon_path, sidmon_hdr, sizeof(sidmon_hdr), NULL));
    g_assert_cmpint(kajo_mod_probe_file(sidmon_path, NULL), ==, KAJO_MOD_ENGINE_UADE);

    /* 5. Console Chiptune (SPC700 magic) -> Priority 3 (libgme) */
    g_autofree gchar *spc_path = g_build_filename(tmp_dir, "bramble.spc", NULL);
    const char spc_hdr[] = "SNES-SPC700 Sound File Data v0.30";
    g_assert_true(g_file_set_contents(spc_path, spc_hdr, sizeof(spc_hdr), NULL));
    g_assert_cmpint(kajo_mod_probe_file(spc_path, NULL), ==, KAJO_MOD_ENGINE_GME);

    /* 6. PC FastTracker II (.xm) -> Priority 4 (libopenmpt) */
    g_autofree gchar *xm_path = g_build_filename(tmp_dir, "doskpop.xm", NULL);
    char xm_hdr[64];
    memset(xm_hdr, 0, sizeof(xm_hdr));
    memcpy(xm_hdr, "Extended Module: Doskpop", 24);
    g_assert_true(g_file_set_contents(xm_path, xm_hdr, sizeof(xm_hdr), NULL));
    g_assert_cmpint(kajo_mod_probe_file(xm_path, NULL), ==, KAJO_MOD_ENGINE_OPENMPT);

    g_unlink(mod_path);
    g_unlink(sid_path);
    g_unlink(sidmon_path);
    g_unlink(spc_path);
    g_unlink(xm_path);
    g_rmdir(tmp_dir);
    g_free(tmp_dir);
}

static void
test_protracker_sample_parser(void)
{
    g_autoptr(GBytes) mod_bytes = build_synthetic_amiga_mod();
    gsize len = 0;
    const guint8 *data = g_bytes_get_data(mod_bytes, &len);

    g_autoptr(GArray) samples = g_array_new(FALSE, TRUE, sizeof(KajoModSampleRow));
    g_autofree gchar *title = NULL;
    kajo_mod_parse_protracker_samples(data, len, samples, &title);

    g_assert_cmpstr(title, ==, "TURRICAN TEST MOD");
    g_assert_cmpuint(samples->len, ==, 2);

    const KajoModSampleRow *r0 = &g_array_index(samples, KajoModSampleRow, 0);
    g_assert_cmpstr(r0->idx_hex, ==, "01");
    g_assert_true(g_str_has_prefix(r0->ascii_name, "PAULA LEAD SYNTH"));
    g_assert_cmpstr(r0->length, ==, "00064");
    g_assert_cmpstr(r0->loop, ==, "0008..00064");
    g_assert_cmpstr(r0->vol, ==, "64");
}

static void
test_uade_and_sid_live_synthesis(void)
{
    gchar *tmp_dir = g_dir_make_tmp("kajo-mod-synth-XXXXXX", NULL);
    g_assert_nonnull(tmp_dir);

    /* Test 1: Open & synthesize Amiga ProTracker module via UADE (Priority 1) */
    g_autoptr(GBytes) mod_bytes = build_synthetic_amiga_mod();
    g_autofree gchar *mod_path = g_build_filename(tmp_dir, "test_amiga.mod", NULL);
    gsize mod_len = 0;
    const guint8 *mod_data = g_bytes_get_data(mod_bytes, &mod_len);
    g_assert_true(g_file_set_contents(mod_path, (const gchar *)mod_data, (gssize)mod_len, NULL));

    KajoModDspConfig dsp;
    kajo_mod_dsp_config_init(&dsp);

    g_autoptr(GError) err = NULL;
    g_autoptr(KajoModEngine) uade_eng = kajo_mod_engine_open(
        mod_path, 1, &dsp, "FILE_ID.DIZ", "Amiga Demo Rip", &err);
    g_assert_no_error(err);
    g_assert_nonnull(uade_eng);
    g_assert_cmpint(uade_eng->meta.engine, ==, KAJO_MOD_ENGINE_UADE);
    g_assert_true(strstr(uade_eng->meta.stil_text, "Amiga Demo Rip") != NULL);

    int16_t pcm[1024 * 2];
    gboolean ended = FALSE;
    int got = kajo_mod_engine_render(uade_eng, pcm, 1024, &ended);
    g_assert_cmpint(got, >, 0);

    /* Test 2: Open & synthesize C64 PSID tune via libsidplayfp + reSIDfp (Priority 2) */
    g_autoptr(GBytes) sid_bytes = build_synthetic_c64_sid();
    g_autofree gchar *sid_path = g_build_filename(tmp_dir, "Commando_Test.sid", NULL);
    gsize sid_len = 0;
    const guint8 *sid_data = g_bytes_get_data(sid_bytes, &sid_len);
    g_assert_true(g_file_set_contents(sid_path, (const gchar *)sid_data, (gssize)sid_len, NULL));

    g_autoptr(KajoModEngine) sid_eng = kajo_mod_engine_open(
        sid_path, 1, &dsp, NULL, NULL, &err);
    g_assert_no_error(err);
    g_assert_nonnull(sid_eng);
    g_assert_cmpint(sid_eng->meta.engine, ==, KAJO_MOD_ENGINE_SIDPLAYFP);
    g_assert_cmpint(sid_eng->meta.subsong_max, ==, 2);

    got = kajo_mod_engine_render(sid_eng, pcm, 1024, &ended);
    g_assert_cmpint(got, ==, 1024);
    gboolean has_nonzero_pcm = FALSE;
    for (int i = 0; i < 1024 * 2; i++) {
        if (pcm[i] != 0) { has_nonzero_pcm = TRUE; break; }
    }
    g_assert_true(has_nonzero_pcm);

    KajoModTelemetry tel;
    kajo_mod_engine_poll_telemetry(sid_eng, &tel);
    g_assert_true(tel.has_live_rows);
    g_assert_cmpint(tel.n_live_rows, ==, 4);
    g_assert_cmpstr(tel.live_rows[0].idx_hex, ==, "V1");

    g_unlink(mod_path);
    g_unlink(sid_path);
    g_rmdir(tmp_dir);
    g_free(tmp_dir);
}

static void
write_archive_entry(struct archive *a, const char *name, const guint8 *data, gsize len)
{
    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, name);
    archive_entry_set_size(entry, (la_int64_t)len);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    g_assert_cmpint(archive_write_header(a, entry), ==, ARCHIVE_OK);
    g_assert_cmpint((int)archive_write_data(a, data, len), ==, (int)len);
    archive_entry_free(entry);
}

static void
test_archive_companion_and_nfo_extraction(void)
{
    gchar *tmp_dir = g_dir_make_tmp("kajo-mod-arch-XXXXXX", NULL);
    g_assert_nonnull(tmp_dir);

    g_autofree gchar *arch_path = g_build_filename(tmp_dir, "turrican2_pack.tar.gz", NULL);
    struct archive *a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    g_assert_cmpint(archive_write_open_filename(a, arch_path), ==, ARCHIVE_OK);

    g_autoptr(GBytes) mod_bytes = build_synthetic_amiga_mod();
    gsize mod_len = 0;
    const guint8 *mod_data = g_bytes_get_data(mod_bytes, &mod_len);

    const char companion_pcm[] = "TFMX SAMPLE DATA BANK";
    const char diz_content[]   = "Turrican II by Chris Huelsbeck (1991 Factor 5)";

    write_archive_entry(a, "turrican2/mdat.turrican2", mod_data, mod_len);
    write_archive_entry(a, "turrican2/smpl.turrican2", (const guint8 *)companion_pcm, sizeof(companion_pcm) - 1);
    write_archive_entry(a, "turrican2/FILE_ID.DIZ", (const guint8 *)diz_content, strlen(diz_content));

    archive_write_close(a);
    archive_write_free(a);

    g_assert_true(kajo_mod_path_is_archive(arch_path));

    g_autoptr(GError) err = NULL;
    g_autoptr(KajoModArchive) extracted = kajo_mod_archive_extract(arch_path, &err);
    g_assert_no_error(err);
    g_assert_nonnull(extracted);

    /* Only mdat.turrican2 is in module_paths; smpl.turrican2 is in companion_paths! */
    g_assert_cmpuint(extracted->module_paths->len, ==, 1);
    g_assert_cmpuint(extracted->companion_paths->len, ==, 1);
    g_assert_cmpstr(extracted->nfo_name, ==, "FILE_ID.DIZ");
    g_assert_cmpstr(extracted->nfo_text, ==, diz_content);

    /* Also verify KajoModPlaylist adds exactly 1 playable item from the archive */
    g_autoptr(KajoModPlaylist) pl = kajo_mod_playlist_new();
    int added = kajo_mod_playlist_add_path(pl, arch_path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(added, ==, 1);
    g_assert_cmpuint(pl->items->len, ==, 1);

    /* Verify runtime preference auto_extract_archives = FALSE skips archives */
    kajo_mod_set_runtime_prefs(240.0, "  /tmp/hvsc-custom  ", FALSE);
    g_assert_cmpfloat_with_epsilon(kajo_mod_get_fallback_duration_sec(), 240.0, 0.01);
    g_autofree gchar *dup_hvsc = kajo_mod_dup_hvsc_path();
    g_assert_cmpstr(dup_hvsc, ==, "/tmp/hvsc-custom");
    g_assert_false(kajo_mod_get_auto_extract_archives());

    g_autoptr(KajoModPlaylist) pl_no_ext = kajo_mod_playlist_new();
    int added_disabled = kajo_mod_playlist_add_path(pl_no_ext, arch_path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(added_disabled, ==, 0);
    g_assert_cmpuint(pl_no_ext->items->len, ==, 0);

    /* Clamping & empty-path clearing */
    kajo_mod_set_runtime_prefs(9999.0, "   ", TRUE);
    g_assert_cmpfloat_with_epsilon(kajo_mod_get_fallback_duration_sec(), 600.0, 0.01);
    g_assert_null(kajo_mod_dup_hvsc_path());
    g_assert_true(kajo_mod_get_auto_extract_archives());

    kajo_mod_set_runtime_prefs(180.0, NULL, TRUE);

    g_unlink(arch_path);
    g_rmdir(tmp_dir);
    g_free(tmp_dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mod/router/priority-and-disambiguation", test_priority_routing_and_disambiguation);
    g_test_add_func("/mod/router/protracker-sample-parser", test_protracker_sample_parser);
    g_test_add_func("/mod/router/uade-and-sid-live-synthesis", test_uade_and_sid_live_synthesis);
    g_test_add_func("/mod/archive/companion-and-nfo-extraction", test_archive_companion_and_nfo_extraction);
    return g_test_run();
}
