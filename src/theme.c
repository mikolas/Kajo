#include "theme.h"

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

    /* 6. User Config Path (~/.config/kajo/style.css) */
    const gchar *xdg_config = g_get_user_config_dir();
    gchar *user_path = g_build_filename(xdg_config, "kajo", "style.css", NULL);
    if (g_file_test(user_path, G_FILE_TEST_EXISTS))
        return user_path;
    g_free(user_path);

    return NULL;
}

static gchar *
resolve_override_css_path(void)
{
    const gchar *xdg_config = g_get_user_config_dir();
    gchar *styled_dir = g_build_filename(xdg_config, "kajo", "style.d", NULL);
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

void
shell_theme_write_override_css(const gchar *accent_hex,
                               const gchar *hover_hex,
                               gint         body_px,
                               gint         heading_px,
                               gint         subtitle_px,
                               gint         badge_px)
{
    const gchar *accent = (accent_hex && accent_hex[0] != '\0') ? accent_hex : "#0078d4";
    const gchar *hover  = (hover_hex && hover_hex[0] != '\0') ? hover_hex : "#1084d8";

    if (body_px < 11 || body_px > 20)         body_px = 14;
    if (heading_px < 10 || heading_px > 18)   heading_px = 13;
    if (subtitle_px < 10 || subtitle_px > 18) subtitle_px = 12;
    if (badge_px < 9 || badge_px > 16)        badge_px = 11;

    g_autofree gchar *theme_css_path = resolve_override_css_path();
    if (!theme_css_path)
        return;

    GString *css = g_string_new("/* Auto-generated accent & typography override by kajo */\n");
    g_string_append_printf(css, "@define-color metro-accent %s;\n", accent);
    g_string_append_printf(css, "@define-color metro-accent-hover %s;\n\n", hover);

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
