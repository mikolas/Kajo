#include "../src/config.h"
#include "../src/config/kajo_config_schema.h"
#include "../src/config/kajo_config_migrate.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>

static void
rm_rf(const gchar *path)
{
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return;
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *dir = g_dir_open(path, 0, NULL);
        if (dir) {
            const gchar *name;
            while ((name = g_dir_read_name(dir)) != NULL) {
                g_autofree gchar *child = g_build_filename(path, name, NULL);
                rm_rf(child);
            }
            g_dir_close(dir);
        }
        g_rmdir(path);
    } else {
        g_unlink(path);
    }
}

/* 1. Shipped Default TOML Asset vs Compiled C Schema Parity */
static void
test_defaults_file_parity(void)
{
    const gchar *src_root = g_getenv("KAJO_SOURCE_ROOT");
    if (!src_root || !src_root[0])
        src_root = KAJO_SOURCE_DIR;

    const KajoDomainSchema *domains[] = {
        kajo_schema_get_theme(),
        kajo_schema_get_shell(),
        kajo_schema_get_ide(),
        kajo_schema_get_files(),
        kajo_schema_get_mod(),
    };

    for (size_t i = 0; i < G_N_ELEMENTS(domains); i++) {
        const KajoDomainSchema *d = domains[i];
        g_autofree gchar *toml_path = g_build_filename(src_root, "data", "defaults", d->filename, NULL);
        g_assert_true(g_file_test(toml_path, G_FILE_TEST_EXISTS));

        gpointer c_def = kajo_cfg_domain_new_rc_default(d);
        gpointer parsed = kajo_cfg_domain_new_rc_default(d);
        g_assert_true(kajo_cfg_domain_parse_toml_file(d, parsed, toml_path, FALSE));

        g_assert_true(kajo_cfg_domain_equal(d, c_def, parsed));

        kajo_cfg_domain_unref_rc(d, c_def);
        kajo_cfg_domain_unref_rc(d, parsed);
    }
}

/* 2. Per-Domain Round-Trip, Clamping, Malformed Input & Literal Exec Preservation */
static void
test_domain_theme(void)
{
    const KajoDomainSchema *s_theme = kajo_schema_get_theme();
    KajoThemeCfg *cfg = kajo_theme_cfg_new_default();
    g_assert_cmpstr(cfg->accent_color, ==, "#0078d4");
    g_assert_cmpint(cfg->ui_font_size_px, ==, 14);

    /* Out-of-range and invalid hex fallback */
    const gchar *bad_toml =
        "accent_color = \"not-a-color\"\n"
        "ui_font_size_px = 999\n"
        "font_heading_size_px = 2\n"
        "font_subtitle_size_px = \"wrong-type\"\n";
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*invalid hex color*");
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*expected integer*");
    g_assert_true(kajo_cfg_domain_parse_toml_str(s_theme, cfg, bad_toml, -1, FALSE, "theme.toml"));
    g_test_assert_expected_messages();
    g_assert_cmpstr(cfg->accent_color, ==, "#0078d4");
    g_assert_cmpint(cfg->ui_font_size_px, ==, 20);
    g_assert_cmpint(cfg->font_heading_size_px, ==, 10);
    g_assert_cmpint(cfg->font_subtitle_size_px, ==, 12);

    /* Round-trip serialization */
    g_free(cfg->accent_color);
    cfg->accent_color = g_strdup("#ffb900");
    cfg->ui_font_size_px = 16;

    g_autofree gchar *serialized = kajo_cfg_domain_serialize_toml(s_theme, cfg);
    KajoThemeCfg *reloaded = kajo_theme_cfg_new_default();
    g_assert_true(kajo_cfg_domain_parse_toml_str(s_theme, reloaded, serialized, -1, FALSE, "theme.toml"));
    g_assert_true(kajo_cfg_domain_equal(s_theme, cfg, reloaded));

    kajo_theme_cfg_free(reloaded);
    kajo_theme_cfg_free(cfg);
}

static void
test_domain_shell(void)
{
    const KajoDomainSchema *s_shell = kajo_schema_get_shell();
    KajoShellCfg *cfg = kajo_shell_cfg_new_default();
    g_assert_cmpint(cfg->panel_position, ==, PANEL_POSITION_BOTTOM);
    g_assert_cmpint(cfg->panel_height, ==, 39);
    g_assert_true(cfg->autohide_enabled);
    g_assert_cmpint(cfg->autohide_hide_duration, ==, 200);

    /* Verify single-quoted literal exec string with backslashes and quotes round-trips losslessly */
    const gchar *custom_toml =
        "[panel]\n"
        "position = \"top\"\n"
        "height = 44\n"
        "[autohide]\n"
        "enabled = false\n"
        "hide_duration = 320\n"
        "[[layout.left]]\n"
        "id = \"launcher\"\n"
        "mode = \"icon\"\n"
        "[[layout.right]]\n"
        "id = \"custom-script\"\n"
        "mode = \"label\"\n"
        "interval = 5\n"
        "exec = 'printf \"%s\\n\" $((1+1))'\n"
        "click_left = 'notify-send \"GPU\"'\n";

    g_assert_true(kajo_cfg_domain_parse_toml_str(s_shell, cfg, custom_toml, -1, FALSE, "shell.toml"));
    g_assert_cmpint(cfg->panel_position, ==, PANEL_POSITION_TOP);
    g_assert_cmpint(cfg->panel_height, ==, 44);
    g_assert_false(cfg->autohide_enabled);
    g_assert_cmpint(cfg->autohide_hide_duration, ==, 320);
    g_assert_cmpuint(cfg->layout_left->len, ==, 1);
    g_assert_cmpuint(cfg->layout_right->len, ==, 1);

    ShellWidgetConfig *wp = g_ptr_array_index(cfg->layout_right, 0);
    g_assert_cmpstr(wp->id, ==, "custom-script");
    g_assert_cmpint(wp->mode, ==, WIDGET_MODE_LABEL);
    g_assert_cmpint(wp->interval_sec, ==, 5);
    g_assert_cmpstr(wp->exec_cmd, ==, "printf \"%s\\n\" $((1+1))");
    g_assert_cmpstr(wp->click_left_cmd, ==, "notify-send \"GPU\"");

    g_autofree gchar *serialized = kajo_cfg_domain_serialize_toml(s_shell, cfg);
    KajoShellCfg *reloaded = kajo_shell_cfg_new_default();
    g_assert_true(kajo_cfg_domain_parse_toml_str(s_shell, reloaded, serialized, -1, FALSE, "shell.toml"));
    g_assert_true(kajo_cfg_domain_equal(s_shell, cfg, reloaded));

    kajo_shell_cfg_free(reloaded);
    kajo_shell_cfg_free(cfg);
}

static void
test_domain_ide_files_mod(void)
{
    const KajoDomainSchema *s_ide   = kajo_schema_get_ide();
    const KajoDomainSchema *s_files = kajo_schema_get_files();
    const KajoDomainSchema *s_mod   = kajo_schema_get_mod();

    /* IDE */
    KajoIdeCfg *ide = kajo_ide_cfg_new_default();
    ide->tab_width = 5; /* Not in {2, 4, 8} -> normalized to 4 on parse */
    g_autofree gchar *ide_str = kajo_cfg_domain_serialize_toml(s_ide, ide);
    KajoIdeCfg *ide2 = kajo_ide_cfg_new_default();
    g_assert_true(kajo_cfg_domain_parse_toml_str(s_ide, ide2, ide_str, -1, FALSE, "ide.toml"));
    g_assert_cmpint(ide2->tab_width, ==, 4);
    kajo_ide_cfg_free(ide2);
    kajo_ide_cfg_free(ide);

    /* Files */
    KajoFilesCfg *files = kajo_files_cfg_new_default();
    files->view_mode = KAJO_FILES_VIEW_LIST;
    files->show_hidden = TRUE;
    files->sort_field = KAJO_FILES_SORT_MODIFIED;
    files->sort_ascending = FALSE;
    g_autofree gchar *files_str = kajo_cfg_domain_serialize_toml(s_files, files);
    KajoFilesCfg *files2 = kajo_files_cfg_new_default();
    g_assert_true(kajo_cfg_domain_parse_toml_str(s_files, files2, files_str, -1, FALSE, "files.toml"));
    g_assert_true(kajo_cfg_domain_equal(s_files, files, files2));
    kajo_files_cfg_free(files2);
    kajo_files_cfg_free(files);

    /* Mod */
    KajoModCfg *mod = kajo_mod_cfg_new_default();
    mod->filter_mode = KAJO_MOD_FILTER_A1200;
    mod->sid_model = KAJO_MOD_SID_8580;
    mod->stereo_sep_pct = 50;
    g_autofree gchar *mod_str = kajo_cfg_domain_serialize_toml(s_mod, mod);
    KajoModCfg *mod2 = kajo_mod_cfg_new_default();
    g_assert_true(kajo_cfg_domain_parse_toml_str(s_mod, mod2, mod_str, -1, FALSE, "mod.toml"));
    g_assert_true(kajo_cfg_domain_equal(s_mod, mod, mod2));
    kajo_mod_cfg_free(mod2);
    kajo_mod_cfg_free(mod);
}

/* 3. conf.d/ Lexicographical Overlay & Base-Only Save Discipline */
static void
mutate_shell_pos_top(KajoShellCfg *draft, gpointer user_data G_GNUC_UNUSED)
{
    draft->panel_position = PANEL_POSITION_TOP;
    draft->panel_height = 40;
}

static void
test_confd_overlay_and_save(void)
{
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-cfg-confd-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    g_autofree gchar *confd = g_build_filename(tmpdir, "conf.d", NULL);
    g_mkdir_with_parents(confd, 0700);

    /* Write base shell.toml */
    g_autofree gchar *shell_path = g_build_filename(tmpdir, "shell.toml", NULL);
    g_assert_true(g_file_set_contents(shell_path,
        "[panel]\nposition = \"bottom\"\nheight = 39\n", -1, NULL));

    /* Write conf.d/10-first.toml and conf.d/20-second.toml */
    g_autofree gchar *drop1 = g_build_filename(confd, "10-first.toml", NULL);
    g_autofree gchar *drop2 = g_build_filename(confd, "20-second.toml", NULL);
    g_assert_true(g_file_set_contents(drop1,
        "[shell.panel]\nheight = 48\n[shell.autohide]\nreveal_delay = 350\n", -1, NULL));
    g_assert_true(g_file_set_contents(drop2,
        "[shell.panel]\nheight = 52\n", -1, NULL));

    g_autoptr(KajoConfigStore) store = kajo_config_store_new_for_dir(tmpdir);
    g_autoptr(KajoShellCfgRef) snap1 = kajo_config_store_acquire_shell(store);
    g_assert_cmpint(snap1->panel_position, ==, PANEL_POSITION_BOTTOM);
    g_assert_cmpint(snap1->panel_height, ==, 52); /* 20-second wins over 10-first and base */
    g_assert_cmpint(snap1->autohide_reveal_delay, ==, 350); /* from 10-first */

    /* Update via kajo_config_store_update_shell(): writes ONLY to base shell.toml */
    g_assert_true(kajo_config_store_update_shell(store, mutate_shell_pos_top, NULL, NULL));

    /* Base shell.toml on disk has height = 40 and default reveal_delay = 200 (did NOT bake 350 or 52 from conf.d!) */
    g_autoptr(KajoShellCfg) base_parsed = kajo_shell_cfg_new_default();
    g_assert_true(kajo_cfg_domain_parse_toml_file(kajo_schema_get_shell(), base_parsed, shell_path, FALSE));
    g_assert_cmpint(base_parsed->panel_position, ==, PANEL_POSITION_TOP);
    g_assert_cmpint(base_parsed->panel_height, ==, 40);
    g_assert_cmpint(base_parsed->autohide_reveal_delay, ==, 200);

    /* Meanwhile effective snapshot still applies conf.d/ overlays (height = 52, reveal_delay = 350) */
    g_autoptr(KajoShellCfgRef) snap2 = kajo_config_store_acquire_shell(store);
    g_assert_cmpint(snap2->panel_position, ==, PANEL_POSITION_TOP);
    g_assert_cmpint(snap2->panel_height, ==, 52);
    g_assert_cmpint(snap2->autohide_reveal_delay, ==, 350);

    rm_rf(tmpdir);
}

/* 4. Multi-Threaded Reader / Reload Stress Test */
typedef struct {
    KajoConfigStore *store;
    gint             stop_flag;
} StressCtx;

static void
mutate_mod_stereo(KajoModCfg *draft, gpointer user_data)
{
    draft->stereo_sep_pct = GPOINTER_TO_INT(user_data);
}

static gpointer
reader_thread_fn(gpointer data)
{
    StressCtx *ctx = data;
    while (!g_atomic_int_get(&ctx->stop_flag)) {
        const KajoShellCfg *sh = kajo_config_store_acquire_shell(ctx->store);
        g_assert_nonnull(sh);
        g_assert_cmpint(sh->panel_height, >=, 24);
        g_assert_cmpint(sh->panel_height, <=, 96);
        kajo_shell_cfg_unref(sh);

        const KajoModCfg *md = kajo_config_store_acquire_mod(ctx->store);
        g_assert_nonnull(md);
        g_assert_cmpint(md->stereo_sep_pct, >=, 0);
        g_assert_cmpint(md->stereo_sep_pct, <=, 100);
        kajo_mod_cfg_unref(md);
    }
    return NULL;
}

static void
test_concurrent_readers_and_reload(void)
{
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-cfg-mt-XXXXXX", NULL);
    g_autoptr(KajoConfigStore) store = kajo_config_store_new_for_dir(tmpdir);

    StressCtx ctx = { .store = store, .stop_flag = 0 };
    GThread *readers[4];
    for (int i = 0; i < 4; i++)
        readers[i] = g_thread_new("cfg-reader", reader_thread_fn, &ctx);

    for (int iter = 0; iter < 200; iter++) {
        kajo_config_store_update_mod(store, mutate_mod_stereo, GINT_TO_POINTER(iter % 101), NULL);
        kajo_config_store_reload(store, KAJO_CONFIG_DOMAIN_ALL);
    }

    g_atomic_int_set(&ctx.stop_flag, 1);
    for (int i = 0; i < 4; i++)
        g_thread_join(readers[i]);

    rm_rf(tmpdir);
}

/* 5. Legacy config.json -> *.toml One-Time Auto-Migration */
static void
test_legacy_json_migration(void)
{
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-cfg-mig-XXXXXX", NULL);
    g_autofree gchar *json_path = g_build_filename(tmpdir, "config.json", NULL);

    const gchar *legacy_json =
        "{\n"
        "  \"accent_color\": \"#e81123\",\n"
        "  \"ui_font_size_px\": 16,\n"
        "  \"ide_color_scheme\": \"kajo-nord\",\n"
        "  \"ide_tab_width\": 8,\n"
        "  \"files_view_mode\": \"list\",\n"
        "  \"files_show_hidden\": true,\n"
        "  \"mod_filter_mode\": \"a1200\",\n"
        "  \"mod_stereo_sep_pct\": 45,\n"
        "  \"panel\": {\n"
        "    \"position\": \"top\",\n"
        "    \"height\": 42,\n"
        "    \"touch_gestures\": {\n"
        "      \"enabled\": true,\n"
        "      \"dwell_ms\": 3500,\n"
        "      \"swipe_center\": \"overview\",\n"
        "      \"swipe_right\": \"notifications\",\n"
        "      \"swipe_horizontal\": \"workspace\",\n"
        "      \"osk_auto_show_mode\": \"always\",\n"
        "      \"osk_push_windows\": false,\n"
        "      \"osk_height_px\": 280\n"
        "    }\n"
        "  },\n"
        "  \"autohide\": { \"enabled\": false, \"hide_duration\": 280 },\n"
        "  \"notifications\": {\n"
        "    \"toast_timeout_ms\": 6000,\n"
        "    \"max_notification_history\": 100,\n"
        "    \"toast_click_action\": \"dismiss\",\n"
        "    \"dnd_enabled\": true\n"
        "  },\n"
        "  \"idle\": {\n"
        "    \"screensaver_mode\": \"dpms\",\n"
        "    \"screensaver_timeout_sec\": 120,\n"
        "    \"dpms_timeout_sec\": 240,\n"
        "    \"lock_timeout_sec\": 360,\n"
        "    \"lock_on_suspend\": false\n"
        "  },\n"
        "  \"posture\": {\n"
        "    \"auto_rotate\": false,\n"
        "    \"disable_kbd_in_tablet\": false,\n"
        "    \"disable_touch_in_laptop\": true\n"
        "  },\n"
        "  \"keyboard_backlight\": {\n"
        "    \"level\": 1,\n"
        "    \"timeout_sec\": 30\n"
        "  },\n"
        "  \"layout\": {\n"
        "    \"left\": [ \"launcher\", { \"id\": \"window\", \"mode\": \"full\" } ],\n"
        "    \"center\": [ { \"id\": \"clock\", \"mode\": \"label\" } ],\n"
        "    \"right\": [\n"
        "      { \"id\": \"custom-script\", \"mode\": \"label\", \"interval_sec\": 15, \"exec_cmd\": \"echo ok\", \"click_left_cmd\": \"true\" },\n"
        "      \"power\"\n"
        "    ]\n"
        "  }\n"
        "}\n";
    g_assert_true(g_file_set_contents(json_path, legacy_json, -1, NULL));

    g_autoptr(KajoConfigStore) store = kajo_config_store_new_for_dir(tmpdir);

    g_autofree gchar *bak_path = g_build_filename(tmpdir, "config.json.bak", NULL);
    g_assert_true(g_file_test(bak_path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(json_path, G_FILE_TEST_EXISTS));

    g_autoptr(KajoThemeCfgRef) theme = kajo_config_store_acquire_theme(store);
    g_assert_cmpstr(theme->accent_color, ==, "#e81123");
    g_assert_cmpint(theme->ui_font_size_px, ==, 16);

    g_autoptr(KajoShellCfgRef) shell = kajo_config_store_acquire_shell(store);
    g_assert_cmpint(shell->panel_position, ==, PANEL_POSITION_TOP);
    g_assert_cmpint(shell->panel_height, ==, 42);
    g_assert_false(shell->autohide_enabled);
    g_assert_cmpint(shell->autohide_hide_duration, ==, 280);
    g_assert_cmpint(shell->touch_dwell_ms, ==, 3500);
    g_assert_cmpint(shell->touch_swipe_center, ==, SWIPE_CENTER_OVERVIEW);
    g_assert_cmpint(shell->touch_swipe_right, ==, SWIPE_RIGHT_NOTIFICATIONS);
    g_assert_cmpint(shell->touch_swipe_horizontal, ==, SWIPE_HORIZ_WORKSPACE);
    g_assert_cmpint(shell->osk_auto_show_mode, ==, OSK_AUTO_SHOW_ALWAYS);
    g_assert_false(shell->osk_push_windows);
    g_assert_cmpint(shell->osk_height_px, ==, 280);
    g_assert_cmpint(shell->toast_timeout_ms, ==, 6000);
    g_assert_cmpint(shell->max_notification_history, ==, 100);
    g_assert_cmpint(shell->toast_click_action, ==, TOAST_CLICK_DISMISS);
    g_assert_true(shell->dnd_enabled);
    g_assert_cmpint(shell->screensaver_mode, ==, SCREENSAVER_MODE_DPMS);
    g_assert_cmpint(shell->screensaver_timeout_sec, ==, 120);
    g_assert_cmpint(shell->dpms_timeout_sec, ==, 240);
    g_assert_cmpint(shell->lock_timeout_sec, ==, 360);
    g_assert_false(shell->lock_on_suspend);
    g_assert_false(shell->posture_auto_rotate);
    g_assert_false(shell->posture_disable_kbd_in_tablet);
    g_assert_true(shell->posture_disable_touch_in_laptop);
    g_assert_cmpint(shell->kbd_backlight_level, ==, 1);
    g_assert_cmpint(shell->kbd_backlight_timeout_sec, ==, 30);

    g_assert_cmpuint(shell->layout_left->len, ==, 2);
    ShellWidgetConfig *wl0 = g_ptr_array_index(shell->layout_left, 0);
    ShellWidgetConfig *wl1 = g_ptr_array_index(shell->layout_left, 1);
    g_assert_cmpstr(wl0->id, ==, "launcher");
    g_assert_cmpint(wl0->mode, ==, WIDGET_MODE_ICON);
    g_assert_cmpstr(wl1->id, ==, "window");
    g_assert_cmpint(wl1->mode, ==, WIDGET_MODE_FULL);

    g_assert_cmpuint(shell->layout_right->len, ==, 2);
    ShellWidgetConfig *wr0 = g_ptr_array_index(shell->layout_right, 0);
    g_assert_cmpstr(wr0->id, ==, "custom-script");
    g_assert_cmpint(wr0->mode, ==, WIDGET_MODE_LABEL);
    g_assert_cmpint(wr0->interval_sec, ==, 15);
    g_assert_cmpstr(wr0->exec_cmd, ==, "echo ok");
    g_assert_cmpstr(wr0->click_left_cmd, ==, "true");

    g_autoptr(KajoIdeCfgRef) ide = kajo_config_store_acquire_ide(store);
    g_assert_cmpstr(ide->color_scheme, ==, "kajo-nord");
    g_assert_cmpint(ide->tab_width, ==, 8);

    g_autoptr(KajoFilesCfgRef) files = kajo_config_store_acquire_files(store);
    g_assert_cmpint(files->view_mode, ==, KAJO_FILES_VIEW_LIST);
    g_assert_true(files->show_hidden);

    g_autoptr(KajoModCfgRef) mod = kajo_config_store_acquire_mod(store);
    g_assert_cmpint(mod->filter_mode, ==, KAJO_MOD_FILTER_A1200);
    g_assert_cmpint(mod->stereo_sep_pct, ==, 45);

    rm_rf(tmpdir);
}

/* 6. Syntax Error Preserves Previous Snapshot & Refuses to Clobber Broken File */
static void
mutate_theme_font_18(KajoThemeCfg *draft, gpointer user_data G_GNUC_UNUSED)
{
    draft->ui_font_size_px = 18;
}

static void
test_syntax_error_preservation_and_save_guard(void)
{
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-cfg-synerr-XXXXXX", NULL);
    g_autofree gchar *theme_path = g_build_filename(tmpdir, "theme.toml", NULL);

    g_assert_true(g_file_set_contents(theme_path,
        "accent_color = \"#00ff00\"\nui_font_size_px = 16\n", -1, NULL));

    g_autoptr(KajoConfigStore) store = kajo_config_store_new_for_dir(tmpdir);
    g_autoptr(KajoThemeCfgRef) snap1 = kajo_config_store_acquire_theme(store);
    g_assert_cmpstr(snap1->accent_color, ==, "#00ff00");
    g_assert_cmpint(snap1->ui_font_size_px, ==, 16);

    /* Introduce a syntax error on disk */
    const gchar *broken_toml = "# user edit in progress\naccent_color = [unclosed\n";
    g_assert_true(g_file_set_contents(theme_path, broken_toml, -1, NULL));

    /* Reload must warn and retain the last known-good snapshot (#00ff00 / 16) */
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*TOML syntax error*");
    kajo_config_store_reload(store, KAJO_CONFIG_DOMAIN_THEME);
    g_test_assert_expected_messages();

    g_autoptr(KajoThemeCfgRef) snap2 = kajo_config_store_acquire_theme(store);
    g_assert_cmpstr(snap2->accent_color, ==, "#00ff00");
    g_assert_cmpint(snap2->ui_font_size_px, ==, 16);

    /* A GUI save while the on-disk file is broken must refuse to overwrite it */
    g_autoptr(GError) err = NULL;
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*TOML syntax error*");
    gboolean saved = kajo_config_store_update_theme(store, mutate_theme_font_18, NULL, &err);
    g_test_assert_expected_messages();
    g_assert_false(saved);
    g_assert_nonnull(err);

    g_autofree gchar *disk_after = NULL;
    g_assert_true(g_file_get_contents(theme_path, &disk_after, NULL, NULL));
    g_assert_cmpstr(disk_after, ==, broken_toml);

    rm_rf(tmpdir);
}

/* 7. Surgical Patching Preserves Comments, Unknown Keys, Datetimes & Avoids Freezing Defaults */
static void
mutate_theme_font_16(KajoThemeCfg *draft, gpointer user_data G_GNUC_UNUSED)
{
    draft->ui_font_size_px = 16;
}

static void
mutate_theme_accent_amber(KajoThemeCfg *draft, gpointer user_data G_GNUC_UNUSED)
{
    g_free(draft->accent_color);
    draft->accent_color = g_strdup("#ffb900");
}

static void
test_surgical_patch_preserves_comments_and_unknown_keys(void)
{
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-cfg-patch-XXXXXX", NULL);
    g_autofree gchar *theme_path = g_build_filename(tmpdir, "theme.toml", NULL);

    const gchar *hand_edited =
        "# My custom header comment\n"
        "accent_color = \"#00ff00\" # inline green note\n"
        "my_experimental_key = \"keep-me\"\n"
        "last_edited = 2026-11-10T12:00:00Z\n";
    g_assert_true(g_file_set_contents(theme_path, hand_edited, -1, NULL));

    g_autoptr(KajoConfigStore) store = kajo_config_store_new_for_dir(tmpdir);
    g_autoptr(KajoThemeCfgRef) s0 = kajo_config_store_acquire_theme(store);
    g_assert_cmpstr(s0->accent_color, ==, "#00ff00");
    g_assert_cmpint(s0->ui_font_size_px, ==, 14);

    /* Save a font-size change: must preserve comments, unknown keys, datetime, and NOT freeze defaults */
    g_assert_true(kajo_config_store_update_theme(store, mutate_theme_font_16, NULL, NULL));

    g_autofree gchar *after1 = NULL;
    g_assert_true(g_file_get_contents(theme_path, &after1, NULL, NULL));
    g_assert_nonnull(strstr(after1, "# My custom header comment"));
    g_assert_nonnull(strstr(after1, "# inline green note"));
    g_assert_nonnull(strstr(after1, "my_experimental_key = \"keep-me\""));
    g_assert_nonnull(strstr(after1, "last_edited = 2026-11-10T12:00:00Z"));
    g_assert_nonnull(strstr(after1, "ui_font_size_px"));
    /* Untouched defaults must NOT be frozen into the user's file */
    g_assert_null(strstr(after1, "settings_width"));
    g_assert_null(strstr(after1, "font_badge_size_px"));

    /* Save an existing key (accent_color): must update value in-place and keep inline comment */
    g_assert_true(kajo_config_store_update_theme(store, mutate_theme_accent_amber, NULL, NULL));

    g_autofree gchar *after2 = NULL;
    g_assert_true(g_file_get_contents(theme_path, &after2, NULL, NULL));
    g_assert_nonnull(strstr(after2, "accent_color = \"#ffb900\" # inline green note"));
    g_assert_nonnull(strstr(after2, "my_experimental_key = \"keep-me\""));

    g_autoptr(KajoThemeCfgRef) s2 = kajo_config_store_acquire_theme(store);
    g_assert_cmpstr(s2->accent_color, ==, "#ffb900");
    g_assert_cmpint(s2->ui_font_size_px, ==, 16);

    rm_rf(tmpdir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/config/defaults-file-parity", test_defaults_file_parity);
    g_test_add_func("/config/domain/theme", test_domain_theme);
    g_test_add_func("/config/domain/shell", test_domain_shell);
    g_test_add_func("/config/domain/ide-files-mod", test_domain_ide_files_mod);
    g_test_add_func("/config/store/confd-overlay-and-save", test_confd_overlay_and_save);
    g_test_add_func("/config/store/concurrent-readers-and-reload", test_concurrent_readers_and_reload);
    g_test_add_func("/config/migrate/legacy-json", test_legacy_json_migration);
    g_test_add_func("/config/store/syntax-error-preservation", test_syntax_error_preservation_and_save_guard);
    g_test_add_func("/config/store/surgical-patch-preserves-comments", test_surgical_patch_preserves_comments_and_unknown_keys);

    return g_test_run();
}
