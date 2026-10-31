#include "../src/config.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static void
test_config_defaults(void)
{
    ShellConfig *cfg = shell_config_get_default();
    g_assert_nonnull(cfg);

    g_assert_cmpint(cfg->panel_position, ==, PANEL_POSITION_TOP);
    g_assert_cmpint(cfg->panel_height, ==, 32);
    g_assert_cmpstr(cfg->panel_layer, ==, "top");
    g_assert_cmpint(cfg->default_widget_mode, ==, WIDGET_MODE_ICON);
    g_assert_cmpint(cfg->icon_size, ==, 24);

    g_assert_false(cfg->autohide_enabled);
    g_assert_cmpint(cfg->toast_timeout_ms, ==, 8000);
    g_assert_cmpint(cfg->max_notification_history, ==, 50);

    /* Idle & Screen Lock defaults */
    g_assert_cmpstr(cfg->screensaver_mode, ==, "overlay");
    g_assert_cmpint(cfg->screensaver_timeout_sec, ==, 300);
    g_assert_cmpint(cfg->dpms_timeout_sec, ==, 600);
    g_assert_cmpint(cfg->lock_timeout_sec, ==, 900);
    g_assert_true(cfg->lock_on_suspend);

    g_assert_nonnull(cfg->layout_left);
    g_assert_nonnull(cfg->layout_center);
    g_assert_nonnull(cfg->layout_right);

    shell_config_destroy(cfg);
}

static void
test_config_save_and_load_roundtrip(void)
{
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-test-cfg-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    g_autofree gchar *cfg_path = g_build_filename(tmpdir, "config.json", NULL);

    ShellConfig *orig = shell_config_get_default();
    orig->panel_position = PANEL_POSITION_BOTTOM;
    orig->panel_height = 44;
    g_free(orig->panel_layer);
    orig->panel_layer = g_strdup("overlay");
    orig->default_widget_mode = WIDGET_MODE_FULL;
    orig->icon_size = 20;

    orig->autohide_enabled = TRUE;
    orig->autohide_hide_delay = 500;
    orig->autohide_reveal_delay = 120;
    orig->autohide_reveal_duration = 180;

    orig->toast_timeout_ms = 5000;
    orig->max_notification_history = 100;

    g_free(orig->screensaver_mode);
    orig->screensaver_mode = g_strdup("dpms");
    orig->screensaver_timeout_sec = 60;
    orig->dpms_timeout_sec = 300;
    orig->lock_timeout_sec = 600;
    orig->lock_on_suspend = FALSE;

    g_free(orig->accent_color);
    orig->accent_color = g_strdup("#ff8c00");
    orig->ui_font_size_px = 14;

    ShellWidgetConfig *wc_left = g_new0(ShellWidgetConfig, 1);
    wc_left->id = g_strdup("launcher");
    wc_left->mode = WIDGET_MODE_ICON;
    g_ptr_array_add(orig->layout_left, wc_left);

    ShellWidgetConfig *wc_center = g_new0(ShellWidgetConfig, 1);
    wc_center->id = g_strdup("clock");
    wc_center->mode = WIDGET_MODE_FULL;
    g_ptr_array_add(orig->layout_center, wc_center);

    ShellWidgetConfig *wc_right = g_new0(ShellWidgetConfig, 1);
    wc_right->id = g_strdup("custom-script");
    wc_right->mode = WIDGET_MODE_LABEL;
    wc_right->interval_sec = 10;
    wc_right->exec_cmd = g_strdup("echo ok");
    wc_right->click_left_cmd = g_strdup("true");
    wc_right->click_right_cmd = g_strdup("false");
    g_ptr_array_add(orig->layout_right, wc_right);

    g_assert_true(shell_config_save(orig, cfg_path));
    shell_config_destroy(orig);

    ShellConfig *loaded = shell_config_load(cfg_path);
    g_assert_nonnull(loaded);

    g_assert_cmpint(loaded->panel_position, ==, PANEL_POSITION_BOTTOM);
    g_assert_cmpint(loaded->panel_height, ==, 44);
    g_assert_cmpstr(loaded->panel_layer, ==, "overlay");
    g_assert_cmpint(loaded->default_widget_mode, ==, WIDGET_MODE_FULL);
    g_assert_cmpint(loaded->icon_size, ==, 20);

    g_assert_true(loaded->autohide_enabled);
    g_assert_cmpint(loaded->autohide_hide_delay, ==, 500);
    g_assert_cmpint(loaded->autohide_reveal_delay, ==, 120);
    g_assert_cmpint(loaded->autohide_reveal_duration, ==, 180);

    g_assert_cmpint(loaded->toast_timeout_ms, ==, 5000);
    g_assert_cmpint(loaded->max_notification_history, ==, 100);

    g_assert_cmpstr(loaded->screensaver_mode, ==, "dpms");
    g_assert_cmpint(loaded->screensaver_timeout_sec, ==, 60);
    g_assert_cmpint(loaded->dpms_timeout_sec, ==, 300);
    g_assert_cmpint(loaded->lock_timeout_sec, ==, 600);
    g_assert_false(loaded->lock_on_suspend);

    g_assert_cmpstr(loaded->accent_color, ==, "#ff8c00");
    g_assert_cmpint(loaded->ui_font_size_px, ==, 14);

    g_assert_cmpuint(loaded->layout_left->len, ==, 1);
    ShellWidgetConfig *l0 = g_ptr_array_index(loaded->layout_left, 0);
    g_assert_cmpstr(l0->id, ==, "launcher");
    g_assert_cmpint(l0->mode, ==, WIDGET_MODE_ICON);

    g_assert_cmpuint(loaded->layout_center->len, ==, 1);
    ShellWidgetConfig *c0 = g_ptr_array_index(loaded->layout_center, 0);
    g_assert_cmpstr(c0->id, ==, "clock");
    g_assert_cmpint(c0->mode, ==, WIDGET_MODE_FULL);

    g_assert_cmpuint(loaded->layout_right->len, ==, 1);
    ShellWidgetConfig *r0 = g_ptr_array_index(loaded->layout_right, 0);
    g_assert_cmpstr(r0->id, ==, "custom-script");
    g_assert_cmpint(r0->mode, ==, WIDGET_MODE_LABEL);
    g_assert_cmpint(r0->interval_sec, ==, 10);
    g_assert_cmpstr(r0->exec_cmd, ==, "echo ok");
    g_assert_cmpstr(r0->click_left_cmd, ==, "true");
    g_assert_cmpstr(r0->click_right_cmd, ==, "false");

    shell_config_destroy(loaded);
    g_unlink(cfg_path);
    g_rmdir(tmpdir);
}

static void
test_config_legacy_flat_keys_and_invalid_files(void)
{
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-test-legacy-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    g_autofree gchar *legacy_path = g_build_filename(tmpdir, "legacy.json", NULL);
    g_autofree gchar *corrupt_path = g_build_filename(tmpdir, "corrupt.json", NULL);

    const char *legacy_json = "{\n"
                              "  \"position\": \"bottom\",\n"
                              "  \"height\": 48,\n"
                              "  \"autohide\": true\n"
                              "}\n";
    g_assert_true(g_file_set_contents(legacy_path, legacy_json, -1, NULL));

    ShellConfig *cfg = shell_config_load(legacy_path);
    g_assert_nonnull(cfg);
    g_assert_cmpint(cfg->panel_position, ==, PANEL_POSITION_BOTTOM);
    g_assert_cmpint(cfg->panel_height, ==, 48);
    g_assert_true(cfg->autohide_enabled);
    shell_config_destroy(cfg);

    /* Corrupt JSON should emit a warning and gracefully fall back to defaults */
    g_assert_true(g_file_set_contents(corrupt_path, "{ not valid json ", -1, NULL));
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*Failed to parse config*");
    ShellConfig *fallback = shell_config_load(corrupt_path);
    g_test_assert_expected_messages();
    g_assert_nonnull(fallback);
    g_assert_cmpint(fallback->panel_height, ==, 32);
    shell_config_destroy(fallback);

    /* Non-existent path should gracefully fall back to defaults */
    ShellConfig *missing = shell_config_load("/tmp/nonexistent-kajo-config-12345.json");
    g_assert_nonnull(missing);
    g_assert_cmpint(missing->screensaver_timeout_sec, ==, 300);
    shell_config_destroy(missing);

    g_unlink(legacy_path);
    g_unlink(corrupt_path);
    g_rmdir(tmpdir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/config/defaults", test_config_defaults);
    g_test_add_func("/config/save-and-load-roundtrip", test_config_save_and_load_roundtrip);
    g_test_add_func("/config/legacy-and-invalid-files", test_config_legacy_flat_keys_and_invalid_files);

    return g_test_run();
}
