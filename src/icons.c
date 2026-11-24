#include "icons.h"
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

static gchar *fallback_icon = NULL;

void
shell_icons_init(void)
{
    if (fallback_icon != NULL)
        g_free(fallback_icon);

    fallback_icon = g_strdup("application-x-executable-symbolic");

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

    /* 1. Development Build Environment */
    gchar *cwd = g_get_current_dir();
    gchar *dev_path = g_build_filename(cwd, "data", "icons", NULL);
    g_free(cwd);
    ADD_ICON_PATHS(dev_path);
    g_free(dev_path);

    /* 2. User Local Install Environment (~/.local/share/kajo/icons) */
    gchar *user_path = g_build_filename(g_get_user_data_dir(), "kajo", "icons", NULL);
    ADD_ICON_PATHS(user_path);
    g_free(user_path);

    /* 3. System Installed Environments (/usr/local/share/kajo/icons & /usr/share/kajo/icons) */
    ADD_ICON_PATHS("/usr/local/share/kajo/icons");
    ADD_ICON_PATHS("/usr/share/kajo/icons");

    #undef ADD_ICON_PATHS
}

void
shell_icons_cleanup(void)
{
    g_free(fallback_icon);
    fallback_icon = NULL;
}

static const gchar *
probe_user_hicolor_icon(const gchar *icon_name, gchar *buf, gsize buf_size)
{
    if (!icon_name || icon_name[0] == '\0' || strchr(icon_name, '/') != NULL)
        return NULL;

    const gchar *udata = g_get_user_data_dir();
    if (!udata || udata[0] == '\0')
        return NULL;

    static const struct {
        const gchar *subdir;
        const gchar *ext;
    } slots[] = {
        { "icons/hicolor/256x256/apps", ".png" },
        { "icons/hicolor/scalable/apps", ".svg" },
        { "icons/hicolor/128x128/apps", ".png" },
        { "icons/hicolor/64x64/apps", ".png" },
        { "icons/hicolor/48x48/apps", ".png" },
    };

    for (gsize i = 0; i < G_N_ELEMENTS(slots); i++) {
        g_autofree gchar *fname = g_strconcat(icon_name, slots[i].ext, NULL);
        g_autofree gchar *path = g_build_filename(udata, slots[i].subdir, fname, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            g_strlcpy(buf, path, buf_size);
            return buf;
        }
    }
    return NULL;
}

static const gchar *
try_resolve_name(GtkIconTheme *theme, const gchar *icon_name, gchar *buf, gsize buf_size)
{
    if (!icon_name || icon_name[0] == '\0')
        return NULL;

    /* 0. Direct absolute path (e.g. from ~/.local/share/applications Desktop Entry Icon=) */
    if (g_path_is_absolute(icon_name)) {
        if (g_file_test(icon_name, G_FILE_TEST_IS_REGULAR)) {
            g_strlcpy(buf, icon_name, buf_size);
            return buf;
        }
        return NULL;
    }

    /* 1. Check exact name */
    if (theme && gtk_icon_theme_has_icon(theme, icon_name)) {
        g_strlcpy(buf, icon_name, buf_size);
        return buf;
    }

    /* 2. Check lowercased name */
    g_autofree gchar *lower = g_ascii_strdown(icon_name, -1);
    if (theme && gtk_icon_theme_has_icon(theme, lower)) {
        g_strlcpy(buf, lower, buf_size);
        return buf;
    }

    /* 3. Probe ~/.local/share/icons/hicolor/ directly so newly installed Web App icons
     * resolve immediately without rebuilding icon caches or restarting kajo. */
    const gchar *user_hit = probe_user_hicolor_icon(icon_name, buf, buf_size);
    if (user_hit) return user_hit;

    user_hit = probe_user_hicolor_icon(lower, buf, buf_size);
    if (user_hit) return user_hit;

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
    const gchar *res = NULL;

    if (g_path_is_absolute(desktop_id)) {
        loaded = g_key_file_load_from_file(kf, desktop_id, G_KEY_FILE_NONE, NULL);
    } else {
        loaded = g_key_file_load_from_data_dirs(kf, desktop_id, NULL, G_KEY_FILE_NONE, NULL);
        if (!loaded) {
            const gchar *udata = g_get_user_data_dir();
            gchar *user_app_path = (udata && udata[0] != '\0')
                                   ? g_build_filename(udata, "applications", desktop_id, NULL)
                                   : NULL;
            const gchar *candidates[] = {
                user_app_path,
                "/usr/local/share/applications",
                "/usr/share/applications",
            };
            if (user_app_path) {
                loaded = g_key_file_load_from_file(kf, user_app_path, G_KEY_FILE_NONE, NULL);
                g_free(user_app_path);
            }
            for (gsize i = 1; !loaded && i < G_N_ELEMENTS(candidates); i++) {
                gchar *full_app_path = g_build_filename(candidates[i], desktop_id, NULL);
                loaded = g_key_file_load_from_file(kf, full_app_path, G_KEY_FILE_NONE, NULL);
                g_free(full_app_path);
            }
        }
    }

    if (loaded) {
        gchar *icon_name = g_key_file_get_string(kf, "Desktop Entry", "Icon", NULL);
        if (icon_name != NULL && icon_name[0] != '\0') {
            res = try_resolve_name(theme, icon_name, buf, buf_size);
        }
        g_free(icon_name);
    }

    g_key_file_free(kf);
    g_free(desktop_id);
    return res;
}

const gchar *
shell_icons_resolve_app_id(const gchar *app_id)
{
    static gchar buf[512];

    if (app_id == NULL || app_id[0] == '\0')
        return fallback_icon;

    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());

    /* 1. Try exact desktop file parsing (e.g. "Code.desktop", "firefox.desktop") */
    const gchar *res = try_desktop_file_icon(theme, app_id, buf, sizeof(buf));
    if (res) return res;

    /* 2. Try lowercased desktop file parsing (e.g. "Code" -> "code.desktop") */
    gchar *lower = g_ascii_strdown(app_id, -1);

    /* Fast-path for Kajo Suite reverse-DNS app IDs (org.kajo.Ide -> kajo-ide) */
    if (g_str_has_prefix(lower, "org.kajo.")) {
        const char *sub = lower + 9;
        if (g_strcmp0(sub, "shell") == 0) {
            res = try_resolve_name(theme, "kajo", buf, sizeof(buf));
        } else {
            g_autofree gchar *kajo_icon = g_strdup_printf("kajo-%s", sub);
            res = try_resolve_name(theme, kajo_icon, buf, sizeof(buf));
        }
        if (res) {
            g_free(lower);
            return res;
        }
    }

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

void
shell_icons_set_image_for_app_id(GtkImage *image, const gchar *app_id, gint pixel_size)
{
    if (!image) return;
    const gchar *resolved = shell_icons_resolve_app_id(app_id);
    if (resolved && g_path_is_absolute(resolved) && g_file_test(resolved, G_FILE_TEST_IS_REGULAR)) {
        g_autoptr(GFile) file = g_file_new_for_path(resolved);
        g_autoptr(GIcon) gicon = g_file_icon_new(file);
        gtk_image_set_from_gicon(image, gicon);
        if (pixel_size > 0) {
            gtk_image_set_pixel_size(image, pixel_size);
        } else if (gtk_image_get_pixel_size(image) <= 0) {
            gtk_image_set_pixel_size(image, 16);
        }
        return;
    }

    const gchar *name = (resolved && *resolved)
                        ? resolved
                        : (fallback_icon ? fallback_icon : "application-x-executable-symbolic");
    gtk_image_set_from_icon_name(image, name);
    if (pixel_size > 0)
        gtk_image_set_pixel_size(image, pixel_size);
}

GtkWidget *
shell_icons_create_app_icon_widget(const gchar *app_id, gint pixel_size)
{
    GtkWidget *image = gtk_image_new();
    shell_icons_set_image_for_app_id(GTK_IMAGE(image), app_id, pixel_size);
    return image;
}
