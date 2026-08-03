#include "icons.h"
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

static gchar *fallback_icon = NULL;

void
shell_icons_init(const gchar *icon_map_path G_GNUC_UNUSED)
{
    if (fallback_icon != NULL)
        g_free(fallback_icon);

    fallback_icon = g_strdup("application-x-executable-symbolic");

    /* Prefer Papirus-Dark icon theme for rich full-color & symbolic app icons */
    GtkSettings *settings = gtk_settings_get_default();
    if (settings != NULL) {
        g_object_set(settings, "gtk-icon-theme-name", "Papirus-Dark", NULL);
    }

    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());

    #define ADD_ICON_PATHS(root_dir) do { \
        if ((root_dir) && g_file_test((root_dir), G_FILE_TEST_IS_DIR)) { \
            gtk_icon_theme_add_search_path(theme, (root_dir)); \
            gchar *_sub = g_build_filename((root_dir), "hicolor", "scalable", "apps", NULL); \
            if (g_file_test(_sub, G_FILE_TEST_IS_DIR)) \
                gtk_icon_theme_add_search_path(theme, _sub); \
            g_free(_sub); \
        } \
    } while(0)

    /* Standard pixmaps & themes */
    ADD_ICON_PATHS("/usr/share/pixmaps");
    ADD_ICON_PATHS("/usr/share/icons");

    /* 1. Development Build Environment */
    ADD_ICON_PATHS("/home/mikolas/src/desktop/data/icons");

    gchar *cwd = g_get_current_dir();
    gchar *dev_path = g_build_filename(cwd, "data", "icons", NULL);
    g_free(cwd);
    ADD_ICON_PATHS(dev_path);
    g_free(dev_path);

    /* 2. User Local Install Environment (~/.local/share/kajo/icons) */
    gchar *user_path = g_build_filename(g_get_user_data_dir(), "kajo", "icons", NULL);
    ADD_ICON_PATHS(user_path);
    g_free(user_path);

    /* 3. System Installed Environment (/usr/share/kajo/icons) */
    ADD_ICON_PATHS("/usr/share/kajo/icons");

    /* 4. XDG User Icons Directory (~/.local/share/icons) */
    gchar *xdg_user_icons = g_build_filename(g_get_user_data_dir(), "icons", NULL);
    ADD_ICON_PATHS(xdg_user_icons);
    g_free(xdg_user_icons);

    #undef ADD_ICON_PATHS
}

void
shell_icons_cleanup(void)
{
    g_free(fallback_icon);
    fallback_icon = NULL;
}

static const gchar *
try_resolve_name(GtkIconTheme *theme, const gchar *icon_name, gchar *buf, gsize buf_size)
{
    if (!icon_name || icon_name[0] == '\0')
        return NULL;

    /* 1. Check exact name */
    if (gtk_icon_theme_has_icon(theme, icon_name)) {
        g_strlcpy(buf, icon_name, buf_size);
        return buf;
    }

    /* 2. Check lowercased name */
    gchar *lower = g_ascii_strdown(icon_name, -1);
    if (gtk_icon_theme_has_icon(theme, lower)) {
        g_strlcpy(buf, lower, buf_size);
        g_free(lower);
        return buf;
    }
    g_free(lower);

    return NULL;
}

static const gchar *
try_desktop_file_icon(GtkIconTheme *theme, const gchar *desktop_filename, gchar *buf, gsize buf_size)
{
    if (!desktop_filename || desktop_filename[0] == '\0')
        return NULL;

    gchar *desktop_id = g_str_has_suffix(desktop_filename, ".desktop")
                        ? g_strdup(desktop_filename)
                        : g_strconcat(desktop_filename, ".desktop", NULL);

    GKeyFile *kf = g_key_file_new();
    gboolean loaded = FALSE;

    if (g_path_is_absolute(desktop_id)) {
        loaded = g_key_file_load_from_file(kf, desktop_id, G_KEY_FILE_NONE, NULL);
    } else {
        loaded = g_key_file_load_from_data_dirs(kf, desktop_id, NULL, G_KEY_FILE_NONE, NULL);
        if (!loaded) {
            gchar *full_app_path = g_build_filename("/usr/share/applications", desktop_id, NULL);
            loaded = g_key_file_load_from_file(kf, full_app_path, G_KEY_FILE_NONE, NULL);
            g_free(full_app_path);
        }
    }

    if (loaded) {
        gchar *icon_name = g_key_file_get_string(kf, "Desktop Entry", "Icon", NULL);
        if (icon_name != NULL && icon_name[0] != '\0') {
            const gchar *res = try_resolve_name(theme, icon_name, buf, buf_size);
            g_free(icon_name);
            g_key_file_free(kf);
            g_free(desktop_id);
            if (res) return res;
        }
        if (icon_name) g_free(icon_name);
    }

    g_key_file_free(kf);
    g_free(desktop_id);
    return NULL;
}

const gchar *
shell_icons_resolve_app_id(const gchar *app_id)
{
    static gchar buf[256];

    if (app_id == NULL || app_id[0] == '\0')
        return fallback_icon;

    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());

    /* 1. Try exact desktop file parsing (e.g. "Code.desktop", "firefox.desktop") */
    const gchar *res = try_desktop_file_icon(theme, app_id, buf, sizeof(buf));
    if (res) return res;

    /* 2. Try lowercased desktop file parsing (e.g. "Code" -> "code.desktop") */
    gchar *lower = g_ascii_strdown(app_id, -1);
    res = try_desktop_file_icon(theme, lower, buf, sizeof(buf));
    if (res) {
        g_free(lower);
        return res;
    }

    /* 3. Try reverse-domain short name desktop file (e.g. "com.visualstudio.code" -> "code.desktop") */
    const char *last_dot = strrchr(lower, '.');
    if (last_dot && *(last_dot + 1) != '\0') {
        const char *short_name = last_dot + 1;
        res = try_desktop_file_icon(theme, short_name, buf, sizeof(buf));
        if (res) {
            g_free(lower);
            return res;
        }
    }

    /* 4. Direct GTK theme name lookups */
    res = try_resolve_name(theme, app_id, buf, sizeof(buf));
    if (res) {
        g_free(lower);
        return res;
    }

    res = try_resolve_name(theme, lower, buf, sizeof(buf));
    if (res) {
        g_free(lower);
        return res;
    }

    if (last_dot && *(last_dot + 1) != '\0') {
        const char *short_name = last_dot + 1;
        res = try_resolve_name(theme, short_name, buf, sizeof(buf));
        if (res) {
            g_free(lower);
            return res;
        }
    }

    g_free(lower);
    return fallback_icon;
}

GtkWidget *
shell_icons_create_app_icon_widget(const gchar *app_id, gint pixel_size)
{
    const gchar *resolved_icon = shell_icons_resolve_app_id(app_id);
    GtkWidget *image = gtk_image_new_from_icon_name(resolved_icon);
    if (pixel_size > 0)
        gtk_image_set_pixel_size(GTK_IMAGE(image), pixel_size);
    return image;
}
