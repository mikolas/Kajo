#define _GNU_SOURCE
#include "greeter_users.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

void
greeter_user_free(gpointer data)
{
    GreeterUser *u = data;
    if (!u)
        return;
    g_free(u->username);
    g_free(u->real_name);
    g_free(u->home_dir);
    g_free(u);
}

void
greeter_session_free(gpointer data)
{
    GreeterSession *s = data;
    if (!s)
        return;
    g_free(s->id);
    g_free(s->name);
    g_free(s->comment);
    g_free(s->exec);
    g_free(s->type);
    g_free(s->desktop_names);
    g_strfreev(s->cmd_argv);
    g_strfreev(s->env_vars);
    g_free(s);
}

static gboolean
is_valid_login_shell(const gchar *shell)
{
    if (!shell || shell[0] == '\0')
        return FALSE;
    if (g_str_has_suffix(shell, "/nologin") ||
        g_str_has_suffix(shell, "/false") ||
        g_strcmp0(shell, "/bin/sync") == 0)
        return FALSE;
    return TRUE;
}

static gchar *
extract_gecos_real_name(const gchar *gecos, const gchar *fallback_user)
{
    if (!gecos || gecos[0] == '\0')
        return g_strdup(fallback_user);

    const gchar *comma = strchr(gecos, ',');
    g_autofree gchar *raw = comma ? g_strndup(gecos, comma - gecos) : g_strdup(gecos);
    g_strstrip(raw);
    if (raw[0] == '\0')
        return g_strdup(fallback_user);
    return g_strdup(raw);
}

static gint
compare_users_by_name(gconstpointer a, gconstpointer b)
{
    const GreeterUser *ua = *(const GreeterUser *const *)a;
    const GreeterUser *ub = *(const GreeterUser *const *)b;
    return g_strcmp0(ua->username, ub->username);
}

GPtrArray *
greeter_discover_users(void)
{
    GPtrArray *users = g_ptr_array_new_with_free_func(greeter_user_free);

    setpwent();
    struct passwd *pw = NULL;
    while ((pw = getpwent()) != NULL) {
        if (!pw->pw_name || pw->pw_name[0] == '\0')
            continue;
        if (pw->pw_uid < 1000 || pw->pw_uid >= 60000)
            continue;
        if (g_strcmp0(pw->pw_name, "greeter") == 0 ||
            g_strcmp0(pw->pw_name, "nobody") == 0)
            continue;
        if (!is_valid_login_shell(pw->pw_shell))
            continue;

        /* Avoid duplicates if NSS has multiple sources */
        gboolean exists = FALSE;
        for (guint i = 0; i < users->len; i++) {
            GreeterUser *existing = g_ptr_array_index(users, i);
            if (g_strcmp0(existing->username, pw->pw_name) == 0) {
                exists = TRUE;
                break;
            }
        }
        if (exists)
            continue;

        GreeterUser *u = g_new0(GreeterUser, 1);
        u->username  = g_strdup(pw->pw_name);
        u->real_name = extract_gecos_real_name(pw->pw_gecos, pw->pw_name);
        u->home_dir  = g_strdup(pw->pw_dir ? pw->pw_dir : "/");
        u->uid       = pw->pw_uid;
        g_ptr_array_add(users, u);
    }
    endpwent();

    /* Fallback to current effective user if running in an environment with no UID >= 1000 */
    if (users->len == 0) {
        struct passwd *cur = getpwuid(getuid());
        GreeterUser *u = g_new0(GreeterUser, 1);
        const gchar *uname = (cur && cur->pw_name) ? cur->pw_name : g_get_user_name();
        u->username  = g_strdup(uname ? uname : "user");
        u->real_name = extract_gecos_real_name(cur ? cur->pw_gecos : NULL, u->username);
        u->home_dir  = g_strdup((cur && cur->pw_dir) ? cur->pw_dir : g_get_home_dir());
        u->uid       = getuid();
        g_ptr_array_add(users, u);
    }

    g_ptr_array_sort(users, compare_users_by_name);
    return users;
}

static GreeterSession *
create_session_entry(const gchar *id,
                     const gchar *name,
                     const gchar *comment,
                     const gchar *exec_str,
                     const gchar *session_type,
                     const gchar *desktop_names)
{
    gint argc = 0;
    gchar **raw_argv = NULL;
    if (!g_shell_parse_argv(exec_str, &argc, &raw_argv, NULL) || argc == 0) {
        g_strfreev(raw_argv);
        return NULL;
    }

    /* Strip Desktop Entry % field codes (%f, %F, %u, %U, %i, %c, %k, etc.) */
    GPtrArray *clean_argv = g_ptr_array_new();
    for (gint i = 0; i < argc && raw_argv[i] != NULL; i++) {
        if (raw_argv[i][0] == '%' && raw_argv[i][1] != '\0' && raw_argv[i][2] == '\0')
            continue;
        g_ptr_array_add(clean_argv, g_strdup(raw_argv[i]));
    }
    g_strfreev(raw_argv);

    if (clean_argv->len == 0) {
        g_ptr_array_free(clean_argv, TRUE);
        return NULL;
    }
    g_ptr_array_add(clean_argv, NULL);

    GreeterSession *s = g_new0(GreeterSession, 1);
    s->id            = g_strdup(id);
    s->name          = g_strdup(name);
    s->comment       = g_strdup(comment ? comment : "");
    s->exec          = g_strdup(exec_str);
    s->type          = g_strdup(session_type ? session_type : "wayland");
    s->desktop_names = g_strdup(desktop_names ? desktop_names : id);
    s->cmd_argv      = (gchar **)g_ptr_array_free(clean_argv, FALSE);

    GPtrArray *env = g_ptr_array_new();
    g_ptr_array_add(env, g_strdup_printf("XDG_SESSION_TYPE=%s", s->type));
    g_ptr_array_add(env, g_strdup_printf("XDG_SESSION_DESKTOP=%s", s->id));
    g_ptr_array_add(env, g_strdup_printf("XDG_CURRENT_DESKTOP=%s", s->desktop_names));
    g_ptr_array_add(env, NULL);
    s->env_vars = (gchar **)g_ptr_array_free(env, FALSE);

    return s;
}

static void
scan_session_directory(GPtrArray   *sessions,
                       const gchar *dir_path,
                       const gchar *session_type)
{
    if (!dir_path || !g_file_test(dir_path, G_FILE_TEST_IS_DIR))
        return;

    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir)
        return;

    const gchar *fname = NULL;
    while ((fname = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(fname, ".desktop"))
            continue;

        g_autofree gchar *slug = g_strndup(fname, strlen(fname) - strlen(".desktop"));

        /* Skip if a higher-priority directory already defined this session id */
        gboolean duplicate = FALSE;
        for (guint i = 0; i < sessions->len; i++) {
            GreeterSession *existing = g_ptr_array_index(sessions, i);
            if (g_strcmp0(existing->id, slug) == 0 &&
                g_strcmp0(existing->type, session_type) == 0) {
                duplicate = TRUE;
                break;
            }
        }
        if (duplicate)
            continue;

        g_autofree gchar *full_path = g_build_filename(dir_path, fname, NULL);
        g_autoptr(GKeyFile) kf = g_key_file_new();
        if (!g_key_file_load_from_file(kf, full_path, G_KEY_FILE_NONE, NULL))
            continue;

        if (g_key_file_get_boolean(kf, G_KEY_FILE_DESKTOP_GROUP, "Hidden", NULL) ||
            g_key_file_get_boolean(kf, G_KEY_FILE_DESKTOP_GROUP, "NoDisplay", NULL))
            continue;

        g_autofree gchar *name = g_key_file_get_locale_string(
            kf, G_KEY_FILE_DESKTOP_GROUP, "Name", NULL, NULL);
        g_autofree gchar *exec = g_key_file_get_string(
            kf, G_KEY_FILE_DESKTOP_GROUP, "Exec", NULL);
        g_autofree gchar *comment = g_key_file_get_locale_string(
            kf, G_KEY_FILE_DESKTOP_GROUP, "Comment", NULL, NULL);
        g_autofree gchar *dnames = g_key_file_get_string(
            kf, G_KEY_FILE_DESKTOP_GROUP, "DesktopNames", NULL);

        if (!name || !exec || exec[0] == '\0')
            continue;

        GreeterSession *s = create_session_entry(
            slug, name, comment, exec, session_type, dnames ? dnames : name);
        if (s)
            g_ptr_array_add(sessions, s);
    }

    g_dir_close(dir);
}

static gint
compare_sessions(gconstpointer a, gconstpointer b)
{
    const GreeterSession *sa = *(const GreeterSession *const *)a;
    const GreeterSession *sb = *(const GreeterSession *const *)b;
    /* Prioritize niri first, then alphabetical by name */
    gboolean a_niri = (g_ascii_strcasecmp(sa->id, "niri") == 0);
    gboolean b_niri = (g_ascii_strcasecmp(sb->id, "niri") == 0);
    if (a_niri && !b_niri) return -1;
    if (!a_niri && b_niri) return 1;
    return g_ascii_strcasecmp(sa->name, sb->name);
}

GPtrArray *
greeter_discover_sessions_from_dirs(const gchar *const *search_dirs,
                                    const gchar        *default_cmd_override)
{
    GPtrArray *sessions = g_ptr_array_new_with_free_func(greeter_session_free);

    if (search_dirs) {
        for (gsize i = 0; search_dirs[i] != NULL; i++) {
            const gchar *stype = (strstr(search_dirs[i], "xsessions") != NULL) ? "x11" : "wayland";
            scan_session_directory(sessions, search_dirs[i], stype);
        }
    }

    if (default_cmd_override && default_cmd_override[0] != '\0') {
        gboolean found = FALSE;
        for (guint i = 0; i < sessions->len; i++) {
            GreeterSession *s = g_ptr_array_index(sessions, i);
            if (g_strcmp0(s->exec, default_cmd_override) == 0 ||
                g_strcmp0(s->id, default_cmd_override) == 0) {
                found = TRUE;
                break;
            }
        }
        if (!found) {
            GreeterSession *custom = create_session_entry(
                default_cmd_override, default_cmd_override,
                "Custom CLI Session", default_cmd_override, "wayland", default_cmd_override);
            if (custom)
                g_ptr_array_insert(sessions, 0, custom);
        }
    }

    if (sessions->len == 0) {
        GreeterSession *fallback = create_session_entry(
            "niri", "Niri", "A scrollable-tiling Wayland compositor",
            "niri-session", "wayland", "niri");
        if (fallback)
            g_ptr_array_add(sessions, fallback);
    }

    g_ptr_array_sort(sessions, compare_sessions);
    return sessions;
}

GPtrArray *
greeter_discover_sessions(const gchar *default_cmd_override)
{
    const gchar *default_dirs[] = {
        "/usr/local/share/wayland-sessions",
        "/usr/share/wayland-sessions",
        "/usr/local/share/xsessions",
        "/usr/share/xsessions",
        NULL
    };
    return greeter_discover_sessions_from_dirs(default_dirs, default_cmd_override);
}

static gchar *
resolve_state_file_path(const gchar *custom_path, gboolean for_write)
{
    if (custom_path && custom_path[0] != '\0')
        return g_strdup(custom_path);

    const gchar *sys_cache = "/var/cache/kajo-greeter";
    if (g_file_test(sys_cache, G_FILE_TEST_IS_DIR) &&
        (!for_write || g_access(sys_cache, W_OK) == 0)) {
        return g_build_filename(sys_cache, "state.json", NULL);
    }

    return g_build_filename(g_get_user_cache_dir(), "kajo", "greeter-state.json", NULL);
}

void
greeter_state_load(const gchar *custom_path,
                   gchar      **out_last_user,
                   gchar      **out_last_session)
{
    if (out_last_user) *out_last_user = NULL;
    if (out_last_session) *out_last_session = NULL;

    g_autofree gchar *path = resolve_state_file_path(custom_path, FALSE);
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return;

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, NULL))
        return;

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return;

    JsonObject *obj = json_node_get_object(root);
    if (out_last_user && json_object_has_member(obj, "last_user")) {
        const gchar *u = json_object_get_string_member(obj, "last_user");
        if (u && u[0] != '\0')
            *out_last_user = g_strdup(u);
    }
    if (out_last_session && json_object_has_member(obj, "last_session")) {
        const gchar *s = json_object_get_string_member(obj, "last_session");
        if (s && s[0] != '\0')
            *out_last_session = g_strdup(s);
    }
}

gboolean
greeter_state_save(const gchar *custom_path,
                   const gchar *username,
                   const gchar *session_id)
{
    g_autofree gchar *path = resolve_state_file_path(custom_path, TRUE);
    if (!path)
        return FALSE;

    g_autofree gchar *dir = g_path_get_dirname(path);
    if (dir && dir[0] != '\0')
        g_mkdir_with_parents(dir, 0755);

    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "last_user");
    json_builder_add_string_value(b, username ? username : "");
    json_builder_set_member_name(b, "last_session");
    json_builder_add_string_value(b, session_id ? session_id : "");
    json_builder_end_object(b);

    g_autoptr(JsonGenerator) gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(gen, root);

    return json_generator_to_file(gen, path, NULL);
}
