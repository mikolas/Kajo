#include "theme.h"
#include <math.h>
#include <string.h>

const ShellAccentPreset shell_accent_presets[SHELL_N_ACCENT_PRESETS] = {
    { "cobalt-blue",  "Cobalt Blue",         "#0078d4", "#1084d8" },
    { "purple",       "Windows Purple",      "#6b69d6", "#7a78e0" },
    { "catppuccin",   "Catppuccin Lavender", "#cba6f7", "#ddb6f2" },
    { "nord-ice",     "Nord Frost Ice",      "#88c0d0", "#99d1e1" },
    { "emerald",      "Emerald Green",       "#107c41", "#138c4a" },
    { "crimson",      "Metro Crimson",       "#d13438", "#e04347" },
    { "amber",        "Amber Gold",          "#c19c00", "#d4ab00" },
    { "cyan",         "Cyberpunk Cyan",      "#008272", "#009684" },
};

const ShellSurfacePreset shell_surface_presets[SHELL_N_SURFACE_PRESETS] = {
    {
        .id                     = "aero-charcoal",
        .name                   = "Aero Charcoal",
        .glass_tint             = "#121212",
        .panel_opacity          = 0.58,
        .glass_opacity          = 0.62,
        .glass_control_contrast = 1.0,
        .widget_chip_opacity    = 0.0,
        .accent_glass_tint      = 0.0,
        .surface_base           = "#141414",
        .surface_contrast       = 1.0,
        .border_opacity         = 1.0,
        .secondary_fg_contrast  = 1.0,
    },
    {
        .id                     = "oled-pitch",
        .name                   = "OLED Pitch",
        .glass_tint             = "#000000",
        .panel_opacity          = 0.72,
        .glass_opacity          = 0.78,
        .glass_control_contrast = 1.20,
        .widget_chip_opacity    = 0.0,
        .accent_glass_tint      = 0.0,
        .surface_base           = "#000000",
        .surface_contrast       = 1.30,
        .border_opacity         = 1.0,
        .secondary_fg_contrast  = 1.05,
    },
    {
        .id                     = "frosted-clear",
        .name                   = "Frosted Clear",
        .glass_tint             = "#10141c",
        .panel_opacity          = 0.38,
        .glass_opacity          = 0.44,
        .glass_control_contrast = 1.25,
        .widget_chip_opacity    = 0.06,
        .accent_glass_tint      = 0.08,
        .surface_base           = "#12161e",
        .surface_contrast       = 1.0,
        .border_opacity         = 0.85,
        .secondary_fg_contrast  = 1.15,
    },
    {
        .id                     = "nord-slate",
        .name                   = "Nord Slate",
        .glass_tint             = "#1b212c",
        .panel_opacity          = 0.64,
        .glass_opacity          = 0.70,
        .glass_control_contrast = 1.05,
        .widget_chip_opacity    = 0.04,
        .accent_glass_tint      = 0.06,
        .surface_base           = "#1c222d",
        .surface_contrast       = 1.0,
        .border_opacity         = 0.95,
        .secondary_fg_contrast  = 1.05,
    },
};

static void
theme_reload_css(ShellTheme *theme)
{
    if (theme->css_path && g_file_test(theme->css_path, G_FILE_TEST_EXISTS)) {
        g_message("Reloading base CSS from '%s'", theme->css_path);
        gtk_css_provider_load_from_path(theme->provider, theme->css_path);
    }
}

static void
theme_reload_override_css(ShellTheme *theme)
{
    if (theme->override_css_path && g_file_test(theme->override_css_path, G_FILE_TEST_EXISTS)) {
        g_message("Reloading accent override CSS from '%s'", theme->override_css_path);
        gtk_css_provider_load_from_path(theme->override_provider, theme->override_css_path);
    }
}

static void
on_css_file_changed(GFileMonitor      *monitor G_GNUC_UNUSED,
                    GFile             *file G_GNUC_UNUSED,
                    GFile             *other_file G_GNUC_UNUSED,
                    GFileMonitorEvent  event_type,
                    gpointer           user_data)
{
    ShellTheme *theme = user_data;

    if (event_type == G_FILE_MONITOR_EVENT_CHANGED ||
        event_type == G_FILE_MONITOR_EVENT_CREATED ||
        event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        event_type == G_FILE_MONITOR_EVENT_MOVED_IN ||
        event_type == G_FILE_MONITOR_EVENT_RENAMED) {
        theme_reload_css(theme);
    }
}

static void
on_override_file_changed(GFileMonitor      *monitor G_GNUC_UNUSED,
                         GFile             *file G_GNUC_UNUSED,
                         GFile             *other_file G_GNUC_UNUSED,
                         GFileMonitorEvent  event_type,
                         gpointer           user_data)
{
    ShellTheme *theme = user_data;

    if (event_type == G_FILE_MONITOR_EVENT_CHANGED ||
        event_type == G_FILE_MONITOR_EVENT_CREATED ||
        event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        event_type == G_FILE_MONITOR_EVENT_MOVED_IN ||
        event_type == G_FILE_MONITOR_EVENT_RENAMED) {
        theme_reload_override_css(theme);
    }
}

static gchar *
resolve_css_path(const gchar *css_path)
{
    if (css_path && g_file_test(css_path, G_FILE_TEST_EXISTS))
        return g_strdup(css_path);

    /* 1. Development / Build Environment (data/defaults/style.css) */
    gchar *cwd = g_get_current_dir();
    gchar *dev_path = g_build_filename(cwd, "data", "defaults", "style.css", NULL);
    g_free(cwd);
    if (g_file_test(dev_path, G_FILE_TEST_EXISTS))
        return dev_path;
    g_free(dev_path);

    gchar *exe_path = g_file_read_link("/proc/self/exe", NULL);
    if (exe_path) {
        gchar *exe_dir = g_path_get_dirname(exe_path);
        gchar *exe_dev_path = g_build_filename(exe_dir, "..", "data", "defaults", "style.css", NULL);
        g_free(exe_dir);
        g_free(exe_path);
        if (g_file_test(exe_dev_path, G_FILE_TEST_EXISTS))
            return exe_dev_path;
        g_free(exe_dev_path);
    }

    /* 2. User Local Installed Environment (~/.local/share/kajo/defaults/style.css) */
    gchar *local_share_path = g_build_filename(
        g_get_user_data_dir(), "kajo", "defaults", "style.css", NULL);
    if (g_file_test(local_share_path, G_FILE_TEST_EXISTS))
        return local_share_path;
    g_free(local_share_path);

    /* 3. System Local Installed Environment (/usr/local/share/kajo/defaults/style.css) */
    const gchar *usr_local_path = "/usr/local/share/kajo/defaults/style.css";
    if (g_file_test(usr_local_path, G_FILE_TEST_EXISTS))
        return g_strdup(usr_local_path);

    /* 4. System Installed Environment (/usr/share/kajo/defaults/style.css) */
    const gchar *system_path = "/usr/share/kajo/defaults/style.css";
    if (g_file_test(system_path, G_FILE_TEST_EXISTS))
        return g_strdup(system_path);

    /* 5. System Config Path (/etc/kajo/style.css) */
    const gchar *etc_path = "/etc/kajo/style.css";
    if (g_file_test(etc_path, G_FILE_TEST_EXISTS))
        return g_strdup(etc_path);

    /* 6. User Config Path ($KAJO_CONFIG_DIR/style.css or ~/.config/kajo/style.css) */
    g_autofree gchar *user_cfg_dir = kajo_config_resolve_user_dir();
    gchar *user_path = g_build_filename(user_cfg_dir, "style.css", NULL);
    if (g_file_test(user_path, G_FILE_TEST_EXISTS))
        return user_path;
    g_free(user_path);

    return NULL;
}

static gchar *
resolve_override_css_path(void)
{
    g_autofree gchar *user_cfg_dir = kajo_config_resolve_user_dir();
    gchar *styled_dir = g_build_filename(user_cfg_dir, "style.d", NULL);
    g_mkdir_with_parents(styled_dir, 0755);

    gchar *override_path = g_build_filename(styled_dir, "00-accent.css", NULL);
    g_free(styled_dir);

    return override_path;
}

ShellTheme *
shell_theme_new(const gchar *css_path)
{
    ShellTheme *theme = g_new0(ShellTheme, 1);

    theme->provider = gtk_css_provider_new();
    theme->override_provider = gtk_css_provider_new();

    theme->css_path = resolve_css_path(css_path);
    theme->override_css_path = resolve_override_css_path();

    if (theme->css_path) {
        theme_reload_css(theme);

        g_autoptr(GFile) file = g_file_new_for_path(theme->css_path);
        g_autoptr(GError) error = NULL;
        theme->monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, &error);
        if (theme->monitor) {
            g_signal_connect(theme->monitor, "changed", G_CALLBACK(on_css_file_changed), theme);
        }
    } else {
        g_message("Loading embedded fallback CSS from GResource '/org/kajo/shell/defaults/style.css'");
        gtk_css_provider_load_from_resource(theme->provider, "/org/kajo/shell/defaults/style.css");
    }

    if (theme->override_css_path) {
        if (!g_file_test(theme->override_css_path, G_FILE_TEST_EXISTS)) {
            g_autoptr(KajoThemeCfgRef) tcfg = kajo_config_store_acquire_theme(NULL);
            if (tcfg)
                shell_theme_sync_from_theme_cfg(tcfg);
        }
        theme_reload_override_css(theme);

        g_autoptr(GFile) file = g_file_new_for_path(theme->override_css_path);
        g_autoptr(GError) error = NULL;
        theme->override_monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, &error);
        if (theme->override_monitor) {
            g_signal_connect(theme->override_monitor, "changed", G_CALLBACK(on_override_file_changed), theme);
        }
    }

    return theme;
}

void
shell_theme_apply(ShellTheme *theme, GdkDisplay *display)
{
    if (!theme || !display)
        return;

    GtkSettings *gtk_settings = gtk_settings_get_for_display(display);
    if (gtk_settings) {
        g_object_set(gtk_settings, "gtk-application-prefer-dark-theme", TRUE, NULL);
    }

    if (theme->provider) {
        gtk_style_context_add_provider_for_display(display,
            GTK_STYLE_PROVIDER(theme->provider),
            GTK_STYLE_PROVIDER_PRIORITY_USER);
    }

    if (theme->override_provider) {
        gtk_style_context_add_provider_for_display(display,
            GTK_STYLE_PROVIDER(theme->override_provider),
            GTK_STYLE_PROVIDER_PRIORITY_USER + 10);
    }
}

void
shell_theme_reload(ShellTheme *theme)
{
    if (!theme)
        return;

    theme_reload_css(theme);
    theme_reload_override_css(theme);
}

void
shell_theme_destroy(ShellTheme *theme)
{
    if (!theme)
        return;

    if (theme->monitor) {
        g_signal_handlers_disconnect_by_data(theme->monitor, theme);
        g_file_monitor_cancel(theme->monitor);
        g_object_unref(theme->monitor);
    }

    if (theme->override_monitor) {
        g_signal_handlers_disconnect_by_data(theme->override_monitor, theme);
        g_file_monitor_cancel(theme->override_monitor);
        g_object_unref(theme->override_monitor);
    }

    GdkDisplay *display = gdk_display_get_default();
    if (theme->provider) {
        if (display)
            gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(theme->provider));
        g_object_unref(theme->provider);
    }

    if (theme->override_provider) {
        if (display)
            gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(theme->override_provider));
        g_object_unref(theme->override_provider);
    }

    g_free(theme->css_path);
    g_free(theme->override_css_path);
    g_free(theme);
}

static gboolean
parse_hex_rgb(const gchar *hex, int *out_r, int *out_g, int *out_b)
{
    if (!hex || hex[0] != '#' || strlen(hex) != 7)
        return FALSE;
    unsigned int r = 0, g = 0, b = 0;
    if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) != 3)
        return FALSE;
    *out_r = (int)r;
    *out_g = (int)g;
    *out_b = (int)b;
    return TRUE;
}

static int
clamp_u8(int v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return v;
}

static gchar *
sanitize_css_font_family(const gchar *family)
{
    if (!family || !*family)
        return NULL;
    GString *out = g_string_new(NULL);
    for (const char *p = family; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '"' || c == '\'' || c == '\\' || c == ';' || c == '{' || c == '}')
            continue;
        g_string_append_c(out, (char)c);
    }
    g_strstrip(out->str);
    if (out->str[0] == '\0') {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

static void
append_override_css_tokens_from_cfg(GString *css, const KajoThemeCfg *cfg,
                                    const gchar *accent, const gchar *hover)
{
    int ar = 0x00, ag = 0x78, ab = 0xd4;
    parse_hex_rgb(accent, &ar, &ag, &ab);

    int gr = 0x12, gg = 0x12, gb = 0x12;
    if (cfg && cfg->glass_tint)
        parse_hex_rgb(cfg->glass_tint, &gr, &gg, &gb);

    int sr = 0x14, sg = 0x14, sb = 0x14;
    if (cfg && cfg->surface_base)
        parse_hex_rgb(cfg->surface_base, &sr, &sg, &sb);

    double panel_op   = cfg ? CLAMP(cfg->panel_opacity, 0.0, 1.0)          : 0.58;
    double glass_op   = cfg ? CLAMP(cfg->glass_opacity, 0.0, 1.0)          : 0.62;
    double glass_ctrl = cfg ? CLAMP(cfg->glass_control_contrast, 0.0, 2.0) : 1.0;
    double chip_op    = cfg ? CLAMP(cfg->widget_chip_opacity, 0.0, 0.25)   : 0.0;
    double acc_tint   = cfg ? CLAMP(cfg->accent_glass_tint, 0.0, 0.25)     : 0.0;
    double surf_c     = cfg ? CLAMP(cfg->surface_contrast, 0.0, 2.0)       : 1.0;
    double border_op  = cfg ? CLAMP(cfg->border_opacity, 0.0, 1.0)         : 1.0;
    double fg_sec_c   = cfg ? CLAMP(cfg->secondary_fg_contrast, 0.5, 1.5)  : 1.0;

    int gr_eff = clamp_u8((int)lround((double)gr * (1.0 - acc_tint) + (double)ar * acc_tint));
    int gg_eff = clamp_u8((int)lround((double)gg * (1.0 - acc_tint) + (double)ag * acc_tint));
    int gb_eff = clamp_u8((int)lround((double)gb * (1.0 - acc_tint) + (double)ab * acc_tint));

    double well_a   = CLAMP(0.24  * glass_ctrl, 0.0, 0.65);
    double hdr_a    = CLAMP(0.035 * glass_ctrl, 0.0, 0.16);
    double ctrl_a   = CLAMP(0.065 * glass_ctrl, 0.0, 0.26);
    double hov_a    = CLAMP(0.13  * glass_ctrl, 0.0, 0.36);
    double gbdr_a   = CLAMP(0.10  * glass_ctrl * border_op, 0.0, 0.35);

    int s0_r = clamp_u8(sr - (int)lround(4.0 * surf_c));
    int s0_g = clamp_u8(sg - (int)lround(4.0 * surf_c));
    int s0_b = clamp_u8(sb - (int)lround(4.0 * surf_c));
    int s2_r = clamp_u8(sr + (int)lround(2.0 * surf_c));
    int s2_g = clamp_u8(sg + (int)lround(2.0 * surf_c));
    int s2_b = clamp_u8(sb + (int)lround(2.0 * surf_c));
    int s3_r = clamp_u8(sr + (int)lround(4.0 * surf_c));
    int s3_g = clamp_u8(sg + (int)lround(4.0 * surf_c));
    int s3_b = clamp_u8(sb + (int)lround(4.0 * surf_c));
    int s4_r = clamp_u8(sr + (int)lround(6.0 * surf_c));
    int s4_g = clamp_u8(sg + (int)lround(6.0 * surf_c));
    int s4_b = clamp_u8(sb + (int)lround(6.0 * surf_c));
    int sh_r = clamp_u8(sr + (int)lround(14.0 * surf_c));
    int sh_g = clamp_u8(sg + (int)lround(14.0 * surf_c));
    int sh_b = clamp_u8(sb + (int)lround(14.0 * surf_c));
    int bd_r = clamp_u8(sr + (int)lround(23.0 * surf_c));
    int bd_g = clamp_u8(sg + (int)lround(23.0 * surf_c));
    int bd_b = clamp_u8(sb + (int)lround(23.0 * surf_c));

    /* Auto-boost secondary text slightly when glass is very sheer (< 0.50). */
    double sheer_boost = (glass_op < 0.50) ? ((0.50 - glass_op) * 48.0) : 0.0;
    int sec_v = CLAMP((int)lround(160.0 * fg_sec_c + sheer_boost), 96, 235);

    char f_panel[32], f_glass[32], f_well[32], f_hdr[32], f_ctrl[32], f_hov[32], f_gbdr[32], f_bdr[32];
    g_ascii_formatd(f_panel, sizeof(f_panel), "%.3f", panel_op);
    g_ascii_formatd(f_glass, sizeof(f_glass), "%.3f", glass_op);
    g_ascii_formatd(f_well,  sizeof(f_well),  "%.3f", well_a);
    g_ascii_formatd(f_hdr,   sizeof(f_hdr),   "%.3f", hdr_a);
    g_ascii_formatd(f_ctrl,  sizeof(f_ctrl),  "%.3f", ctrl_a);
    g_ascii_formatd(f_hov,   sizeof(f_hov),   "%.3f", hov_a);
    g_ascii_formatd(f_gbdr,  sizeof(f_gbdr),  "%.3f", gbdr_a);
    g_ascii_formatd(f_bdr,   sizeof(f_bdr),   "%.3f", border_op);

    g_string_append_printf(css, "@define-color metro-accent %s;\n", accent);
    g_string_append_printf(css, "@define-color metro-accent-hover %s;\n", hover);
    g_string_append_printf(css, "@define-color metro-panel-bg rgba(%d, %d, %d, %s);\n", gr_eff, gg_eff, gb_eff, f_panel);
    g_string_append_printf(css, "@define-color metro-glass-bg rgba(%d, %d, %d, %s);\n", gr_eff, gg_eff, gb_eff, f_glass);
    g_string_append_printf(css, "@define-color metro-glass-well rgba(0, 0, 0, %s);\n", f_well);
    g_string_append_printf(css, "@define-color metro-glass-header rgba(255, 255, 255, %s);\n", f_hdr);
    g_string_append_printf(css, "@define-color metro-glass-control rgba(255, 255, 255, %s);\n", f_ctrl);
    g_string_append_printf(css, "@define-color metro-glass-hover rgba(255, 255, 255, %s);\n", f_hov);
    g_string_append_printf(css, "@define-color metro-glass-border rgba(255, 255, 255, %s);\n", f_gbdr);
    g_string_append_printf(css, "@define-color metro-surface-0 #%02x%02x%02x;\n", s0_r, s0_g, s0_b);
    g_string_append_printf(css, "@define-color metro-surface-bg #%02x%02x%02x;\n", sr, sg, sb);
    g_string_append_printf(css, "@define-color metro-surface-2 #%02x%02x%02x;\n", s2_r, s2_g, s2_b);
    g_string_append_printf(css, "@define-color metro-surface-3 #%02x%02x%02x;\n", s3_r, s3_g, s3_b);
    g_string_append_printf(css, "@define-color metro-surface-4 #%02x%02x%02x;\n", s4_r, s4_g, s4_b);
    g_string_append_printf(css, "@define-color metro-surface-hover #%02x%02x%02x;\n", sh_r, sh_g, sh_b);
    if (border_op >= 0.995) {
        g_string_append_printf(css, "@define-color metro-border #%02x%02x%02x;\n", bd_r, bd_g, bd_b);
    } else {
        g_string_append_printf(css, "@define-color metro-border rgba(%d, %d, %d, %s);\n", bd_r, bd_g, bd_b, f_bdr);
    }
    g_string_append_printf(css, "@define-color metro-fg-secondary #%02x%02x%02x;\n\n", sec_v, sec_v, sec_v);

    g_autofree gchar *safe_font = cfg ? sanitize_css_font_family(cfg->ui_font_family) : NULL;
    if (safe_font && g_ascii_strcasecmp(safe_font, "Fira Code") != 0) {
        g_string_append_printf(css,
            "* {\n"
            "    font-family: \"%s\", \"Fira Code\", \"FiraCode Nerd Font\", monospace;\n"
            "}\n\n",
            safe_font);
    }

    if (chip_op > 0.001) {
        char f_chip[32];
        g_ascii_formatd(f_chip, sizeof(f_chip), "%.3f", chip_op);
        g_string_append_printf(css,
            "#panel-container .shell-widget > button,\n"
            "#panel-container menubutton.shell-widget > button {\n"
            "    background-color: rgba(255, 255, 255, %s);\n"
            "}\n\n",
            f_chip);
    }
}

gchar *
shell_theme_build_token_css(const KajoThemeCfg *config,
                            const gchar        *accent_hex,
                            const gchar        *hover_hex)
{
    const gchar *accent = (accent_hex && accent_hex[0] != '\0') ? accent_hex : "#0078d4";
    const gchar *hover  = (hover_hex && hover_hex[0] != '\0') ? hover_hex : accent;
    GString *css = g_string_new(NULL);
    append_override_css_tokens_from_cfg(css, config, accent, hover);
    return g_string_free(css, FALSE);
}

void
shell_theme_compute_border_hex(const KajoThemeCfg *config, gchar out_hex[8])
{
    int sr = 0x14, sg = 0x14, sb = 0x14;
    if (config && config->surface_base)
        parse_hex_rgb(config->surface_base, &sr, &sg, &sb);
    double surf_c = config ? CLAMP(config->surface_contrast, 0.0, 2.0) : 1.0;
    int bd_r = clamp_u8(sr + (int)lround(23.0 * surf_c));
    int bd_g = clamp_u8(sg + (int)lround(23.0 * surf_c));
    int bd_b = clamp_u8(sb + (int)lround(23.0 * surf_c));
    g_snprintf(out_hex, 8, "#%02x%02x%02x", bd_r, bd_g, bd_b);
}

static void
shell_theme_write_override_css_internal(const KajoThemeCfg *cfg,
                                        const gchar        *accent_hex,
                                        const gchar        *hover_hex,
                                        gint                body_px,
                                        gint                heading_px,
                                        gint                subtitle_px,
                                        gint                badge_px)
{
    const gchar *accent = (accent_hex && accent_hex[0] != '\0') ? accent_hex : "#0078d4";
    const gchar *hover  = (hover_hex && hover_hex[0] != '\0') ? hover_hex : accent;

    if (body_px < 11 || body_px > 20)         body_px = 14;
    if (heading_px < 10 || heading_px > 18)   heading_px = 13;
    if (subtitle_px < 10 || subtitle_px > 18) subtitle_px = 12;
    if (badge_px < 9 || badge_px > 16)        badge_px = 11;

    g_autofree gchar *theme_css_path = resolve_override_css_path();
    if (!theme_css_path)
        return;

    GString *css = g_string_new("/* Auto-generated accent, surface & typography override by kajo */\n");
    append_override_css_tokens_from_cfg(css, cfg, accent, hover);

    /* 1. Primary Body, Controls & Row Titles */
    g_string_append_printf(css,
        "window.shell-settings-window,\n"
        "window.shell-panel,\n"
        ".shell-popover,\n"
        ".row-title,\n"
        "window.shell-settings-window .sidebar-label,\n"
        "window.shell-settings-window .navigation-sidebar listview row,\n"
        "window.shell-settings-window .navigation-sidebar row,\n"
        "window.shell-files-window .files-list-name,\n"
        "window.shell-files-window .files-tile-title,\n"
        ".tile-title,\n"
        "window.shell-settings-window entry,\n"
        "window.shell-settings-window passwordentry,\n"
        "window.shell-settings-window searchentry,\n"
        "popover entry,\n"
        "popover passwordentry,\n"
        "popover searchentry,\n"
        ".shell-launcher-input,\n"
        "dropdown > button,\n"
        "window.shell-settings-window dropdown > button,\n"
        "popover dropdown > button,\n"
        "dropdown listview,\n"
        "dropdown listview row,\n"
        "window.shell-files-window button.files-menu-row,\n"
        ".shell-launcher-row label,\n"
        ".shell-widget,\n"
        ".shell-widget > button,\n"
        "menubutton.shell-widget > button,\n"
        ".shell-osd-label {\n"
        "    font-size: %dpx;\n"
        "}\n\n",
        body_px);

    /* 2. Section & Window Headers */
    g_string_append_printf(css,
        "window.shell-settings-window .settings-header-title,\n"
        ".shell-popover-title,\n"
        ".heading,\n"
        ".shell-popover-section-title,\n"
        ".settings-section-title,\n"
        "window.shell-settings-window .sidebar-category-header,\n"
        "window.shell-lock-window .lock-date-label,\n"
        "window.shell-screensaver-window .screensaver-date-label {\n"
        "    font-size: %dpx;\n"
        "}\n\n",
        heading_px);

    /* 3. Subtitles, Buttons & Metadata */
    g_string_append_printf(css,
        ".row-subtitle,\n"
        ".dim-label,\n"
        ".shell-popover-subtitle,\n"
        ".empty-state-label,\n"
        ".shell-device-placeholder,\n"
        "popover button,\n"
        "window.shell-settings-window button,\n"
        "popover button.files-nav-btn,\n"
        "popover button.player-transport-btn,\n"
        "window.shell-settings-window button.files-nav-btn,\n"
        "window.shell-settings-window button.player-transport-btn,\n"
        "window.shell-settings-window menubutton.files-nav-btn > button,\n"
        "window.shell-files-window .files-breadcrumb-btn,\n"
        "window.shell-files-window .files-list-meta,\n"
        "window.shell-files-window .files-meta-key,\n"
        "window.shell-files-window .files-meta-val,\n"
        "window.shell-files-window .files-status-text,\n"
        "window.shell-files-window .files-quicklook-textview,\n"
        "window.shell-files-window .files-quicklook-textview text,\n"
        "popover checkbutton,\n"
        "window.shell-settings-window checkbutton {\n"
        "    font-size: %dpx;\n"
        "}\n\n",
        subtitle_px);

    /* 4. Technical Badges, Shortcuts & Micro Labels */
    g_string_append_printf(css,
        ".shell-popover-badge,\n"
        "window.shell-files-window .files-ctx-shortcut,\n"
        "window.shell-files-window .files-col-header,\n"
        "window.shell-files-window button.files-col-header-btn,\n"
        ".tile-subtitle,\n"
        "window.shell-files-window .files-tile-sub,\n"
        "window.shell-player-window .player-timecode,\n"
        "window.shell-player-window .player-stats-text,\n"
        ".code-preview-box,\n"
        ".process-row,\n"
        "window.shell-lock-window .lock-badge-label,\n"
        "window.shell-lock-window .greeter-field-label {\n"
        "    font-size: %dpx;\n"
        "}\n",
        badge_px);

    g_file_set_contents(theme_css_path, css->str, css->len, NULL);
    g_string_free(css, TRUE);
}

void
shell_theme_write_override_css(const gchar *accent_hex,
                               const gchar *hover_hex,
                               gint         body_px,
                               gint         heading_px,
                               gint         subtitle_px,
                               gint         badge_px)
{
    g_autoptr(KajoThemeCfgRef) cur = kajo_config_store_acquire_theme(NULL);
    shell_theme_write_override_css_internal(cur, accent_hex, hover_hex,
                                            body_px, heading_px, subtitle_px, badge_px);
}

gchar *
shell_ide_font_stack(const gchar *family)
{
    static const gchar *const fallbacks[] = { "Fira Code", "FiraCode Nerd Font", "monospace", NULL };

    GString *stack = g_string_new(NULL);
    const gchar *first = (family && family[0] != '\0') ? family : NULL;
    if (first)
        g_string_append(stack, first);

    for (gsize i = 0; fallbacks[i]; i++) {
        if (first && g_ascii_strcasecmp(first, fallbacks[i]) == 0)
            continue; /* already the first entry */
        if (stack->len > 0)
            g_string_append(stack, ", ");
        g_string_append(stack, fallbacks[i]);
    }
    return g_string_free(stack, FALSE);
}

void
shell_theme_sync_from_theme_cfg(const KajoThemeCfg *config)
{
    if (!config)
        return;

    const gchar *accent = config->accent_color ? config->accent_color : "#0078d4";
    const gchar *hover  = accent;
    for (size_t i = 0; i < SHELL_N_ACCENT_PRESETS; i++) {
        if (g_ascii_strcasecmp(accent, shell_accent_presets[i].accent) == 0) {
            hover = shell_accent_presets[i].hover;
            break;
        }
    }

    shell_theme_write_override_css_internal(config,
                                            accent,
                                            hover,
                                            config->ui_font_size_px,
                                            config->font_heading_size_px,
                                            config->font_subtitle_size_px,
                                            config->font_badge_size_px);
}
