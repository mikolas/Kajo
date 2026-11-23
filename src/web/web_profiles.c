#include "web_profiles.h"

struct _KajoWebProfileManager {
    gchar                  *data_root;
    gchar                  *cache_root;
    gchar                  *registry_path;
    KajoWebProfileRegistry *registry;
    GHashTable             *sessions; /* gchar* profile_id -> WebKitNetworkSession* (owned) */
};

static void
apply_session_cfg(WebKitNetworkSession *session, const KajoWebCfg *cfg)
{
    if (!session || !cfg)
        return;
    webkit_network_session_set_itp_enabled(session, cfg->itp_enabled);
    webkit_network_session_set_persistent_credential_storage_enabled(
        session, !webkit_network_session_is_ephemeral(session));

    WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(session);
    if (cm) {
        WebKitCookieAcceptPolicy pol = WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY;
        if (cfg->cookie_policy == KAJO_WEB_COOKIES_ALWAYS)
            pol = WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS;
        else if (cfg->cookie_policy == KAJO_WEB_COOKIES_NEVER)
            pol = WEBKIT_COOKIE_POLICY_ACCEPT_NEVER;
        webkit_cookie_manager_set_accept_policy(cm, pol);
    }
}

KajoWebProfileManager *
web_profile_manager_new(void)
{
    KajoWebProfileManager *pm = g_new0(KajoWebProfileManager, 1);
    pm->data_root = g_build_filename(g_get_user_data_dir(), "kajo", "web", NULL);
    pm->cache_root = g_build_filename(g_get_user_cache_dir(), "kajo", "web", NULL);
    pm->registry_path = g_build_filename(pm->data_root, "profiles.toml", NULL);
    g_mkdir_with_parents(pm->data_root, 0700);
    g_mkdir_with_parents(pm->cache_root, 0700);

    pm->registry = web_profile_registry_new();
    web_profile_registry_load(pm->registry, pm->registry_path);
    pm->sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    return pm;
}

void
web_profile_manager_free(KajoWebProfileManager *pm)
{
    if (!pm)
        return;
    g_clear_pointer(&pm->sessions, g_hash_table_unref);
    g_clear_pointer(&pm->registry, web_profile_registry_free);
    g_free(pm->registry_path);
    g_free(pm->cache_root);
    g_free(pm->data_root);
    g_free(pm);
}

KajoWebProfileRegistry *
web_profile_manager_get_registry(KajoWebProfileManager *pm)
{
    return pm ? pm->registry : NULL;
}

gboolean
web_profile_manager_save(KajoWebProfileManager *pm, GError **error)
{
    if (!pm)
        return FALSE;
    return web_profile_registry_save(pm->registry, pm->registry_path, error);
}

const gchar *
web_profile_manager_get_root_dir(KajoWebProfileManager *pm)
{
    return pm ? pm->data_root : NULL;
}

gchar *
web_profile_manager_get_data_dir(KajoWebProfileManager *pm, const gchar *profile_id)
{
    g_autofree gchar *safe = web_profile_sanitize_id(profile_id ? profile_id : "default");
    gchar *dir = g_build_filename(pm->data_root, "profiles", safe, NULL);
    g_mkdir_with_parents(dir, 0700);
    return dir;
}

gchar *
web_profile_manager_get_cache_dir(KajoWebProfileManager *pm, const gchar *profile_id)
{
    g_autofree gchar *safe = web_profile_sanitize_id(profile_id ? profile_id : "default");
    gchar *dir = g_build_filename(pm->cache_root, "profiles", safe, NULL);
    g_mkdir_with_parents(dir, 0700);
    return dir;
}

static void
apply_memory_pressure(const KajoWebCfg *cfg)
{
    if (!cfg)
        return;
    WebKitMemoryPressureSettings *mps = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(mps, (guint)CLAMP(cfg->memory_limit_mb, 512, 16384));
    webkit_memory_pressure_settings_set_strict_threshold(mps, 0.85);
    webkit_memory_pressure_settings_set_conservative_threshold(mps, 0.60);
    webkit_memory_pressure_settings_set_kill_threshold(mps, 1.0);
    webkit_network_session_set_memory_pressure_settings(mps);
    webkit_memory_pressure_settings_free(mps);
}

WebKitNetworkSession *
web_profile_manager_get_session(KajoWebProfileManager *pm,
                                const gchar           *profile_id,
                                gboolean               is_private,
                                const KajoWebCfg      *cfg)
{
    if (!pm)
        return NULL;

    apply_memory_pressure(cfg);

    if (is_private) {
        WebKitNetworkSession *ephem = webkit_network_session_new_ephemeral();
        apply_session_cfg(ephem, cfg);
        return ephem; /* caller owns ref for private window */
    }

    g_autofree gchar *safe = web_profile_sanitize_id(profile_id ? profile_id : "default");
    WebKitNetworkSession *existing = g_hash_table_lookup(pm->sessions, safe);
    if (existing) {
        apply_session_cfg(existing, cfg);
        return g_object_ref(existing);
    }

    g_autofree gchar *data_dir = web_profile_manager_get_data_dir(pm, safe);
    g_autofree gchar *cache_dir = web_profile_manager_get_cache_dir(pm, safe);
    WebKitNetworkSession *session = webkit_network_session_new(data_dir, cache_dir);
    apply_session_cfg(session, cfg);
    g_hash_table_insert(pm->sessions, g_strdup(safe), session);
    return g_object_ref(session);
}

void
web_profile_manager_apply_config(KajoWebProfileManager *pm, const KajoWebCfg *cfg)
{
    if (!pm || !cfg)
        return;
    apply_memory_pressure(cfg);
    GHashTableIter iter;
    gpointer val = NULL;
    g_hash_table_iter_init(&iter, pm->sessions);
    while (g_hash_table_iter_next(&iter, NULL, &val)) {
        apply_session_cfg(WEBKIT_NETWORK_SESSION(val), cfg);
    }
}
