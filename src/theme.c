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
        event_type == G_FILE_MONITOR_EVENT_CREATED) {
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
        event_type == G_FILE_MONITOR_EVENT_CREATED) {
        theme_reload_override_css(theme);
    }
}

static gchar *
resolve_css_path(const gchar *css_path)
{
    if (css_path && g_file_test(css_path, G_FILE_TEST_EXISTS))
        return g_strdup(css_path);

    /* 1. User Config Path (~/.config/kajo/style.css) */
    const gchar *xdg_config = g_get_user_config_dir();
    gchar *user_path = g_build_filename(xdg_config, "kajo", "style.css", NULL);
    if (g_file_test(user_path, G_FILE_TEST_EXISTS))
        return user_path;
    g_free(user_path);

    /* 2. Development / Build Environment (data/defaults/style.css) */
    gchar *cwd = g_get_current_dir();
    gchar *dev_path = g_build_filename(cwd, "data", "defaults", "style.css", NULL);
    g_free(cwd);
    if (g_file_test(dev_path, G_FILE_TEST_EXISTS))
        return dev_path;
    g_free(dev_path);

    /* 3. Installed Environment (/usr/share/kajo/defaults/style.css) */
    const gchar *system_path = "/usr/share/kajo/defaults/style.css";
    if (g_file_test(system_path, G_FILE_TEST_EXISTS))
        return g_strdup(system_path);

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
shell_theme_destroy(ShellTheme *theme)
{
    if (!theme)
        return;

    if (theme->monitor) {
        g_file_monitor_cancel(theme->monitor);
        g_object_unref(theme->monitor);
    }

    if (theme->override_monitor) {
        g_file_monitor_cancel(theme->override_monitor);
        g_object_unref(theme->override_monitor);
    }

    if (theme->provider)
        g_object_unref(theme->provider);

    if (theme->override_provider)
        g_object_unref(theme->override_provider);

    g_free(theme->css_path);
    g_free(theme->override_css_path);
    g_free(theme);
}
