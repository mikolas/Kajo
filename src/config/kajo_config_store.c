#include "kajo_config_store.h"
#include "kajo_config_migrate.h"
#include <gio/gio.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <string.h>

/* Reference-counted so that a notification queued on another thread's main
 * context can safely outlive unsubscribe(). `destroy_notify` runs when the last
 * reference is dropped, never while a queued callback can still read user_data. */
typedef struct {
    gint                ref;
    gint                active;   /* atomic: 0 once unsubscribed or the store is destroyed */
    guint               id;
    KajoConfigDomain    domains;
    KajoConfigChangedCb cb;
    gpointer            user_data;
    GDestroyNotify      destroy_notify;
    GMainContext       *context;  /* thread-default context of the subscriber */
} KajoConfigSub;

struct _KajoConfigStore {
    GMutex              lock;
    GMutex              write_lock;
    gchar              *config_dir;

    /* Active immutable refcounted domain snapshots */
    const KajoThemeCfg *theme_snap;
    const KajoShellCfg *shell_snap;
    const KajoIdeCfg   *ide_snap;
    const KajoFilesCfg *files_snap;
    const KajoModCfg   *mod_snap;

    /* Directory monitors & debounce state */
    GFileMonitor       *base_monitor;
    GFileMonitor       *confd_monitor;
    guint               debounce_timer_id;
    KajoConfigDomain    pending_domains;

    /* Subscribers */
    GPtrArray          *subscribers; /* KajoConfigSub* */
    guint               next_sub_id;
};

static KajoConfigSub *
sub_ref(KajoConfigSub *sub)
{
    g_atomic_int_inc(&sub->ref);
    return sub;
}

static void
sub_unref(gpointer data)
{
    KajoConfigSub *sub = (KajoConfigSub *)data;
    if (!sub || !g_atomic_int_dec_and_test(&sub->ref))
        return;
    if (sub->destroy_notify && sub->user_data) {
        sub->destroy_notify(sub->user_data);
    }
    g_clear_pointer(&sub->context, g_main_context_unref);
    g_free(sub);
}

static gint
cmp_strings(gconstpointer a, gconstpointer b)
{
    const gchar *sa = *(const gchar *const *)a;
    const gchar *sb = *(const gchar *const *)b;
    return g_strcmp0(sa, sb);
}

static gboolean
load_base_domain_into(KajoConfigStore *store, const KajoDomainSchema *schema, void *dst)
{
    kajo_cfg_domain_init_defaults(schema, dst);

    g_autofree gchar *def_path = kajo_config_find_default_file(schema->filename);
    if (def_path && g_file_test(def_path, G_FILE_TEST_EXISTS)) {
        kajo_cfg_domain_parse_toml_file(schema, dst, def_path, TRUE);
    }

    g_autofree gchar *user_path = g_build_filename(store->config_dir, schema->filename, NULL);
    if (g_file_test(user_path, G_FILE_TEST_EXISTS)) {
        return kajo_cfg_domain_parse_toml_file(schema, dst, user_path, TRUE);
    }
    return TRUE;
}

static void
apply_confd_overlays(KajoConfigStore *store, const KajoDomainSchema *schema, void *dst)
{
    g_autofree gchar *confd_dir = g_build_filename(store->config_dir, KAJO_CFG_DIR_CONFD, NULL);
    if (!g_file_test(confd_dir, G_FILE_TEST_IS_DIR))
        return;

    GDir *dir = g_dir_open(confd_dir, 0, NULL);
    if (!dir)
        return;

    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_suffix(name, ".toml")) {
            g_ptr_array_add(files, g_strdup(name));
        }
    }
    g_dir_close(dir);

    g_ptr_array_sort(files, cmp_strings);

    for (guint i = 0; i < files->len; i++) {
        const gchar *fname = (const gchar *)g_ptr_array_index(files, i);
        g_autofree gchar *fpath = g_build_filename(confd_dir, fname, NULL);
        g_autofree gchar *contents = NULL;
        gsize len = 0;
        if (!g_file_get_contents(fpath, &contents, &len, NULL) || !contents)
            continue;

        toml_result_t res = toml_parse(contents, (int)len);
        if (!res.ok) {
            g_warning("TOML syntax error in overlay '%s': %s", fpath, res.errmsg);
            toml_free(res);
            continue;
        }

        /* Only apply if the overlay file defines the top-level domain table (e.g. [theme], [shell], [ide]) */
        if (schema->domain_table_name) {
            toml_datum_t sub = toml_get(res.toptab, schema->domain_table_name);
            if (sub.type == TOML_TABLE) {
                kajo_cfg_domain_apply_toml_table(schema, dst, sub, TRUE, fpath);
            }
        }
        toml_free(res);
    }

    g_ptr_array_free(files, TRUE);
}

static void *
build_effective_rc_snapshot(KajoConfigStore        *store,
                            const KajoDomainSchema *schema,
                            gboolean                allow_fallback_on_error)
{
    void *snap = kajo_cfg_domain_new_rc_default(schema);
    if (!load_base_domain_into(store, schema, snap) && !allow_fallback_on_error) {
        kajo_cfg_domain_unref_rc(schema, snap);
        return NULL;
    }
    apply_confd_overlays(store, schema, snap);
    return snap;
}

typedef struct {
    KajoConfigStore *store;
    KajoConfigSub   *sub;       /* owns one reference */
    KajoConfigDomain matched;
} PendingNotify;

static gboolean
pending_notify_run(gpointer data)
{
    PendingNotify *pn = (PendingNotify *)data;
    /* Skip if the subscriber went away while this call was queued */
    if (g_atomic_int_get(&pn->sub->active))
        pn->sub->cb(pn->store, pn->matched, pn->sub->user_data);
    return G_SOURCE_REMOVE;
}

static void
pending_notify_free(gpointer data)
{
    PendingNotify *pn = (PendingNotify *)data;
    sub_unref(pn->sub);
    g_free(pn);
}

/* Delivers each callback on the GMainContext that was the subscriber's
 * thread-default context when it subscribed. When that context is owned by the
 * calling thread the callback runs immediately (so main-thread callers see the
 * same synchronous behaviour as before); otherwise it is queued on that context. */
static void
notify_subscribers(KajoConfigStore *store, KajoConfigDomain changed_mask)
{
    if (!store || changed_mask == 0)
        return;

    GPtrArray *calls = g_ptr_array_new();

    g_mutex_lock(&store->lock);
    if (store->subscribers) {
        for (guint i = 0; i < store->subscribers->len; i++) {
            KajoConfigSub *sub = (KajoConfigSub *)g_ptr_array_index(store->subscribers, i);
            KajoConfigDomain matched = (KajoConfigDomain)(sub->domains & changed_mask);
            if (matched != 0 && sub->cb && g_atomic_int_get(&sub->active)) {
                PendingNotify *pn = g_new0(PendingNotify, 1);
                pn->store   = store;
                pn->sub     = sub_ref(sub);
                pn->matched = matched;
                g_ptr_array_add(calls, pn);
            }
        }
    }
    g_mutex_unlock(&store->lock);

    for (guint i = 0; i < calls->len; i++) {
        PendingNotify *pn = (PendingNotify *)g_ptr_array_index(calls, i);
        g_main_context_invoke_full(pn->sub->context, G_PRIORITY_DEFAULT,
                                   pending_notify_run, pn, pending_notify_free);
    }
    g_ptr_array_free(calls, TRUE);
}

void
kajo_config_store_reload(KajoConfigStore *store, KajoConfigDomain domains)
{
    if (!store)
        store = kajo_config_store_get_default();
    if (!store || domains == 0)
        return;

    KajoConfigDomain changed_mask = 0;

    if (domains & KAJO_CONFIG_DOMAIN_THEME) {
        const KajoDomainSchema *s = kajo_schema_get_theme();
        const KajoThemeCfg *new_snap = (const KajoThemeCfg *)build_effective_rc_snapshot(store, s, store->theme_snap == NULL);
        const KajoThemeCfg *old_snap = NULL;

        if (new_snap) {
            g_mutex_lock(&store->lock);
            old_snap = store->theme_snap;
            if (!old_snap || !kajo_cfg_domain_equal(s, old_snap, new_snap)) {
                store->theme_snap = new_snap;
                changed_mask |= KAJO_CONFIG_DOMAIN_THEME;
                new_snap = NULL;
            } else {
                old_snap = NULL;
            }
            g_mutex_unlock(&store->lock);

            if (old_snap) kajo_theme_cfg_unref(old_snap);
            if (new_snap) kajo_theme_cfg_unref(new_snap);
        }
    }

    if (domains & KAJO_CONFIG_DOMAIN_SHELL) {
        const KajoDomainSchema *s = kajo_schema_get_shell();
        const KajoShellCfg *new_snap = (const KajoShellCfg *)build_effective_rc_snapshot(store, s, store->shell_snap == NULL);
        const KajoShellCfg *old_snap = NULL;

        if (new_snap) {
            g_mutex_lock(&store->lock);
            old_snap = store->shell_snap;
            if (!old_snap || !kajo_cfg_domain_equal(s, old_snap, new_snap)) {
                store->shell_snap = new_snap;
                changed_mask |= KAJO_CONFIG_DOMAIN_SHELL;
                new_snap = NULL;
            } else {
                old_snap = NULL;
            }
            g_mutex_unlock(&store->lock);

            if (old_snap) kajo_shell_cfg_unref(old_snap);
            if (new_snap) kajo_shell_cfg_unref(new_snap);
        }
    }

    if (domains & KAJO_CONFIG_DOMAIN_IDE) {
        const KajoDomainSchema *s = kajo_schema_get_ide();
        const KajoIdeCfg *new_snap = (const KajoIdeCfg *)build_effective_rc_snapshot(store, s, store->ide_snap == NULL);
        const KajoIdeCfg *old_snap = NULL;

        if (new_snap) {
            g_mutex_lock(&store->lock);
            old_snap = store->ide_snap;
            if (!old_snap || !kajo_cfg_domain_equal(s, old_snap, new_snap)) {
                store->ide_snap = new_snap;
                changed_mask |= KAJO_CONFIG_DOMAIN_IDE;
                new_snap = NULL;
            } else {
                old_snap = NULL;
            }
            g_mutex_unlock(&store->lock);

            if (old_snap) kajo_ide_cfg_unref(old_snap);
            if (new_snap) kajo_ide_cfg_unref(new_snap);
        }
    }

    if (domains & KAJO_CONFIG_DOMAIN_FILES) {
        const KajoDomainSchema *s = kajo_schema_get_files();
        const KajoFilesCfg *new_snap = (const KajoFilesCfg *)build_effective_rc_snapshot(store, s, store->files_snap == NULL);
        const KajoFilesCfg *old_snap = NULL;

        if (new_snap) {
            g_mutex_lock(&store->lock);
            old_snap = store->files_snap;
            if (!old_snap || !kajo_cfg_domain_equal(s, old_snap, new_snap)) {
                store->files_snap = new_snap;
                changed_mask |= KAJO_CONFIG_DOMAIN_FILES;
                new_snap = NULL;
            } else {
                old_snap = NULL;
            }
            g_mutex_unlock(&store->lock);

            if (old_snap) kajo_files_cfg_unref(old_snap);
            if (new_snap) kajo_files_cfg_unref(new_snap);
        }
    }

    if (domains & KAJO_CONFIG_DOMAIN_MOD) {
        const KajoDomainSchema *s = kajo_schema_get_mod();
        const KajoModCfg *new_snap = (const KajoModCfg *)build_effective_rc_snapshot(store, s, store->mod_snap == NULL);
        const KajoModCfg *old_snap = NULL;

        if (new_snap) {
            g_mutex_lock(&store->lock);
            old_snap = store->mod_snap;
            if (!old_snap || !kajo_cfg_domain_equal(s, old_snap, new_snap)) {
                store->mod_snap = new_snap;
                changed_mask |= KAJO_CONFIG_DOMAIN_MOD;
                new_snap = NULL;
            } else {
                old_snap = NULL;
            }
            g_mutex_unlock(&store->lock);

            if (old_snap) kajo_mod_cfg_unref(old_snap);
            if (new_snap) kajo_mod_cfg_unref(new_snap);
        }
    }

    if (changed_mask != 0) {
        notify_subscribers(store, changed_mask);
    }
}

static gboolean
on_debounce_reload_timeout(gpointer user_data)
{
    KajoConfigStore *store = (KajoConfigStore *)user_data;
    g_mutex_lock(&store->lock);
    KajoConfigDomain domains = store->pending_domains;
    store->pending_domains = 0;
    store->debounce_timer_id = 0;
    g_mutex_unlock(&store->lock);

    if (domains != 0) {
        kajo_config_store_reload(store, domains);
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_debounced_reload(KajoConfigStore *store, KajoConfigDomain domains)
{
    g_mutex_lock(&store->lock);
    store->pending_domains = (KajoConfigDomain)(store->pending_domains | domains);
    if (store->debounce_timer_id == 0) {
        store->debounce_timer_id = g_timeout_add(50, on_debounce_reload_timeout, store);
    }
    g_mutex_unlock(&store->lock);
}

static KajoConfigDomain
match_base_domain_gfile(GFile *f)
{
    if (!f)
        return 0;
    g_autofree gchar *basename = g_file_get_basename(f);
    if (!basename)
        return 0;

    if (g_strcmp0(basename, KAJO_CFG_FILE_THEME) == 0)
        return KAJO_CONFIG_DOMAIN_THEME;
    if (g_strcmp0(basename, KAJO_CFG_FILE_SHELL) == 0)
        return KAJO_CONFIG_DOMAIN_SHELL;
    if (g_strcmp0(basename, KAJO_CFG_FILE_IDE) == 0)
        return KAJO_CONFIG_DOMAIN_IDE;
    if (g_strcmp0(basename, KAJO_CFG_FILE_FILES) == 0)
        return KAJO_CONFIG_DOMAIN_FILES;
    if (g_strcmp0(basename, KAJO_CFG_FILE_MOD) == 0)
        return KAJO_CONFIG_DOMAIN_MOD;
    return 0;
}

static gboolean
is_toml_gfile(GFile *f)
{
    if (!f)
        return FALSE;
    g_autofree gchar *basename = g_file_get_basename(f);
    return basename && g_str_has_suffix(basename, ".toml");
}

static void
on_base_dir_changed(GFileMonitor     *monitor G_GNUC_UNUSED,
                    GFile            *file,
                    GFile            *other_file,
                    GFileMonitorEvent event_type,
                    gpointer          user_data)
{
    if (event_type != G_FILE_MONITOR_EVENT_CHANGED &&
        event_type != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        event_type != G_FILE_MONITOR_EVENT_CREATED &&
        event_type != G_FILE_MONITOR_EVENT_DELETED &&
        event_type != G_FILE_MONITOR_EVENT_RENAMED &&
        event_type != G_FILE_MONITOR_EVENT_MOVED_IN &&
        event_type != G_FILE_MONITOR_EVENT_MOVED_OUT) {
        return;
    }

    KajoConfigStore *store = (KajoConfigStore *)user_data;
    KajoConfigDomain dom = (KajoConfigDomain)(match_base_domain_gfile(file) |
                                              match_base_domain_gfile(other_file));
    if (dom != 0) {
        schedule_debounced_reload(store, dom);
    }
}

static void
on_confd_dir_changed(GFileMonitor     *monitor G_GNUC_UNUSED,
                     GFile            *file,
                     GFile            *other_file,
                     GFileMonitorEvent event_type,
                     gpointer          user_data)
{
    if (event_type != G_FILE_MONITOR_EVENT_CHANGED &&
        event_type != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        event_type != G_FILE_MONITOR_EVENT_CREATED &&
        event_type != G_FILE_MONITOR_EVENT_DELETED &&
        event_type != G_FILE_MONITOR_EVENT_RENAMED &&
        event_type != G_FILE_MONITOR_EVENT_MOVED_IN &&
        event_type != G_FILE_MONITOR_EVENT_MOVED_OUT) {
        return;
    }

    KajoConfigStore *store = (KajoConfigStore *)user_data;
    if (is_toml_gfile(file) || is_toml_gfile(other_file)) {
        schedule_debounced_reload(store, KAJO_CONFIG_DOMAIN_ALL);
    }
}

KajoConfigStore *
kajo_config_store_new_for_dir(const gchar *config_dir)
{
    KajoConfigStore *store = g_new0(KajoConfigStore, 1);
    g_mutex_init(&store->lock);
    g_mutex_init(&store->write_lock);

    if (config_dir && *config_dir) {
        store->config_dir = g_strdup(config_dir);
    } else {
        store->config_dir = kajo_config_resolve_user_dir();
    }

    store->subscribers = g_ptr_array_new_with_free_func(sub_unref);
    store->next_sub_id = 1;

    /* Run one-time legacy config.json -> *.toml auto-migration if applicable */
    kajo_config_migrate_legacy_json(store->config_dir);

    /* Build initial snapshots */
    store->theme_snap = (const KajoThemeCfg *)build_effective_rc_snapshot(store, kajo_schema_get_theme(), TRUE);
    store->shell_snap = (const KajoShellCfg *)build_effective_rc_snapshot(store, kajo_schema_get_shell(), TRUE);
    store->ide_snap   = (const KajoIdeCfg *)build_effective_rc_snapshot(store, kajo_schema_get_ide(), TRUE);
    store->files_snap = (const KajoFilesCfg *)build_effective_rc_snapshot(store, kajo_schema_get_files(), TRUE);
    store->mod_snap   = (const KajoModCfg *)build_effective_rc_snapshot(store, kajo_schema_get_mod(), TRUE);

    /* Attach GFileMonitors to config_dir and config_dir/conf.d */
    g_autoptr(GFile) base_gfile = g_file_new_for_path(store->config_dir);
    store->base_monitor = g_file_monitor_directory(base_gfile, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (store->base_monitor) {
        g_signal_connect(store->base_monitor, "changed", G_CALLBACK(on_base_dir_changed), store);
    }

    g_autofree gchar *confd_path = g_build_filename(store->config_dir, KAJO_CFG_DIR_CONFD, NULL);
    g_autoptr(GFile) confd_gfile = g_file_new_for_path(confd_path);
    store->confd_monitor = g_file_monitor_directory(confd_gfile, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (store->confd_monitor) {
        g_signal_connect(store->confd_monitor, "changed", G_CALLBACK(on_confd_dir_changed), store);
    }

    return store;
}

void
kajo_config_store_destroy(KajoConfigStore *store)
{
    if (!store)
        return;

    g_mutex_lock(&store->lock);
    if (store->debounce_timer_id > 0) {
        g_source_remove(store->debounce_timer_id);
        store->debounce_timer_id = 0;
    }
    g_mutex_unlock(&store->lock);

    if (store->base_monitor) {
        g_file_monitor_cancel(store->base_monitor);
        g_clear_object(&store->base_monitor);
    }
    if (store->confd_monitor) {
        g_file_monitor_cancel(store->confd_monitor);
        g_clear_object(&store->confd_monitor);
    }

    if (store->subscribers) {
        for (guint i = 0; i < store->subscribers->len; i++) {
            KajoConfigSub *sub = (KajoConfigSub *)g_ptr_array_index(store->subscribers, i);
            g_atomic_int_set(&sub->active, 0);
        }
        g_ptr_array_free(store->subscribers, TRUE);
        store->subscribers = NULL;
    }

    kajo_theme_cfg_unref(store->theme_snap);
    kajo_shell_cfg_unref(store->shell_snap);
    kajo_ide_cfg_unref(store->ide_snap);
    kajo_files_cfg_unref(store->files_snap);
    kajo_mod_cfg_unref(store->mod_snap);

    g_free(store->config_dir);
    g_mutex_clear(&store->write_lock);
    g_mutex_clear(&store->lock);
    g_free(store);
}

const gchar *
kajo_config_store_get_config_dir(KajoConfigStore *store)
{
    return store ? store->config_dir : NULL;
}

static gpointer
create_default_store_once(gpointer data G_GNUC_UNUSED)
{
    return kajo_config_store_new_for_dir(NULL);
}

KajoConfigStore *
kajo_config_store_get_default(void)
{
    static GOnce default_once = G_ONCE_INIT;
    g_once(&default_once, create_default_store_once, NULL);
    return (KajoConfigStore *)default_once.retval;
}

/* ─── Thread-Safe Snapshot Acquisition ─── */

const KajoThemeCfg *
kajo_config_store_acquire_theme(KajoConfigStore *store)
{
    if (!store)
        store = kajo_config_store_get_default();
    g_mutex_lock(&store->lock);
    const KajoThemeCfg *res = kajo_theme_cfg_ref(store->theme_snap);
    g_mutex_unlock(&store->lock);
    return res;
}

const KajoShellCfg *
kajo_config_store_acquire_shell(KajoConfigStore *store)
{
    if (!store)
        store = kajo_config_store_get_default();
    g_mutex_lock(&store->lock);
    const KajoShellCfg *res = kajo_shell_cfg_ref(store->shell_snap);
    g_mutex_unlock(&store->lock);
    return res;
}

const KajoIdeCfg *
kajo_config_store_acquire_ide(KajoConfigStore *store)
{
    if (!store)
        store = kajo_config_store_get_default();
    g_mutex_lock(&store->lock);
    const KajoIdeCfg *res = kajo_ide_cfg_ref(store->ide_snap);
    g_mutex_unlock(&store->lock);
    return res;
}

const KajoFilesCfg *
kajo_config_store_acquire_files(KajoConfigStore *store)
{
    if (!store)
        store = kajo_config_store_get_default();
    g_mutex_lock(&store->lock);
    const KajoFilesCfg *res = kajo_files_cfg_ref(store->files_snap);
    g_mutex_unlock(&store->lock);
    return res;
}

const KajoModCfg *
kajo_config_store_acquire_mod(KajoConfigStore *store)
{
    if (!store)
        store = kajo_config_store_get_default();
    g_mutex_lock(&store->lock);
    const KajoModCfg *res = kajo_mod_cfg_ref(store->mod_snap);
    g_mutex_unlock(&store->lock);
    return res;
}

/* ─── Transactional Base-File Read-Modify-Write Mutators ─── */

static gboolean
update_domain_generic(KajoConfigStore        *store,
                      const KajoDomainSchema *schema,
                      void                  (*mutate_cb)(void *draft, gpointer user_data),
                      gpointer                user_data,
                      GError                **error)
{
    if (!store)
        store = kajo_config_store_get_default();
    if (!schema || !mutate_cb)
        return FALSE;

    g_mutex_lock(&store->write_lock);

    g_mkdir_with_parents(store->config_dir, 0755);
    g_autofree gchar *lock_name = g_strdup_printf(".%s.lock", schema->filename);
    g_autofree gchar *lock_path = g_build_filename(store->config_dir, lock_name, NULL);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_EX);
    }

    g_autofree gchar *filepath = g_build_filename(store->config_dir, schema->filename, NULL);
    g_autofree gchar *existing_toml = NULL;
    gsize existing_len = 0;

    void *old_base = g_malloc0(schema->struct_size);
    kajo_cfg_domain_init_defaults(schema, old_base);

    if (g_file_test(filepath, G_FILE_TEST_EXISTS)) {
        if (!g_file_get_contents(filepath, &existing_toml, &existing_len, error)) {
            kajo_cfg_domain_clear(schema, old_base);
            g_free(old_base);
            if (lock_fd >= 0) {
                flock(lock_fd, LOCK_UN);
                close(lock_fd);
            }
            g_mutex_unlock(&store->write_lock);
            return FALSE;
        }

        if (!kajo_cfg_domain_parse_toml_str(schema, old_base, existing_toml,
                                            (gssize)existing_len, FALSE, filepath)) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                        "Refusing to overwrite '%s' because the existing file contains a TOML syntax error",
                        filepath);
            kajo_cfg_domain_clear(schema, old_base);
            g_free(old_base);
            if (lock_fd >= 0) {
                flock(lock_fd, LOCK_UN);
                close(lock_fd);
            }
            g_mutex_unlock(&store->write_lock);
            return FALSE;
        }
    } else {
        load_base_domain_into(store, schema, old_base);
    }

    void *draft = g_malloc0(schema->struct_size);
    kajo_cfg_domain_copy_into(schema, draft, old_base);

    mutate_cb(draft, user_data);

    gboolean ok = kajo_cfg_domain_save_patched(schema, old_base, draft, existing_toml, filepath, error);

    kajo_cfg_domain_clear(schema, draft);
    g_free(draft);
    kajo_cfg_domain_clear(schema, old_base);
    g_free(old_base);

    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
    g_mutex_unlock(&store->write_lock);

    if (ok) {
        kajo_config_store_reload(store, schema->domain);
    }
    return ok;
}

typedef struct {
    KajoThemeMutateFn fn;
    gpointer          ud;
} ThemeMutCtx;

static void
theme_mutate_trampoline(void *draft, gpointer ud)
{
    ThemeMutCtx *ctx = (ThemeMutCtx *)ud;
    ctx->fn((KajoThemeCfg *)draft, ctx->ud);
}

gboolean
kajo_config_store_update_theme(KajoConfigStore  *store,
                               KajoThemeMutateFn mutate_fn,
                               gpointer          user_data,
                               GError          **error)
{
    ThemeMutCtx ctx = { mutate_fn, user_data };
    return update_domain_generic(store, kajo_schema_get_theme(), theme_mutate_trampoline, &ctx, error);
}

typedef struct {
    KajoShellMutateFn fn;
    gpointer          ud;
} ShellMutCtx;

static void
shell_mutate_trampoline(void *draft, gpointer ud)
{
    ShellMutCtx *ctx = (ShellMutCtx *)ud;
    ctx->fn((KajoShellCfg *)draft, ctx->ud);
}

gboolean
kajo_config_store_update_shell(KajoConfigStore  *store,
                               KajoShellMutateFn mutate_fn,
                               gpointer          user_data,
                               GError          **error)
{
    ShellMutCtx ctx = { mutate_fn, user_data };
    return update_domain_generic(store, kajo_schema_get_shell(), shell_mutate_trampoline, &ctx, error);
}

typedef struct {
    KajoIdeMutateFn fn;
    gpointer        ud;
} IdeMutCtx;

static void
ide_mutate_trampoline(void *draft, gpointer ud)
{
    IdeMutCtx *ctx = (IdeMutCtx *)ud;
    ctx->fn((KajoIdeCfg *)draft, ctx->ud);
}

gboolean
kajo_config_store_update_ide(KajoConfigStore *store,
                             KajoIdeMutateFn  mutate_fn,
                             gpointer         user_data,
                             GError         **error)
{
    IdeMutCtx ctx = { mutate_fn, user_data };
    return update_domain_generic(store, kajo_schema_get_ide(), ide_mutate_trampoline, &ctx, error);
}

typedef struct {
    KajoFilesMutateFn fn;
    gpointer          ud;
} FilesMutCtx;

static void
files_mutate_trampoline(void *draft, gpointer ud)
{
    FilesMutCtx *ctx = (FilesMutCtx *)ud;
    ctx->fn((KajoFilesCfg *)draft, ctx->ud);
}

gboolean
kajo_config_store_update_files(KajoConfigStore  *store,
                               KajoFilesMutateFn mutate_fn,
                               gpointer          user_data,
                               GError          **error)
{
    FilesMutCtx ctx = { mutate_fn, user_data };
    return update_domain_generic(store, kajo_schema_get_files(), files_mutate_trampoline, &ctx, error);
}

typedef struct {
    KajoModMutateFn fn;
    gpointer        ud;
} ModMutCtx;

static void
mod_mutate_trampoline(void *draft, gpointer ud)
{
    ModMutCtx *ctx = (ModMutCtx *)ud;
    ctx->fn((KajoModCfg *)draft, ctx->ud);
}

gboolean
kajo_config_store_update_mod(KajoConfigStore *store,
                             KajoModMutateFn  mutate_fn,
                             gpointer         user_data,
                             GError         **error)
{
    ModMutCtx ctx = { mutate_fn, user_data };
    return update_domain_generic(store, kajo_schema_get_mod(), mod_mutate_trampoline, &ctx, error);
}

/* ─── Pub/Sub Subscription Management ─── */

guint
kajo_config_store_subscribe(KajoConfigStore    *store,
                            KajoConfigDomain    domains,
                            KajoConfigChangedCb cb,
                            gpointer            user_data,
                            GDestroyNotify      destroy_notify)
{
    if (!store)
        store = kajo_config_store_get_default();
    if (!cb || domains == 0)
        return 0;

    g_mutex_lock(&store->lock);
    guint id = store->next_sub_id++;
    KajoConfigSub *sub = g_new0(KajoConfigSub, 1);
    sub->ref            = 1;
    sub->active         = 1;
    sub->context        = g_main_context_ref_thread_default();
    sub->id             = id;
    sub->domains        = domains;
    sub->cb             = cb;
    sub->user_data      = user_data;
    sub->destroy_notify = destroy_notify;
    g_ptr_array_add(store->subscribers, sub);
    g_mutex_unlock(&store->lock);

    return id;
}

void
kajo_config_store_unsubscribe(KajoConfigStore *store, guint sub_id)
{
    if (!store)
        store = kajo_config_store_get_default();
    if (sub_id == 0)
        return;

    g_mutex_lock(&store->lock);
    if (store->subscribers) {
        for (guint i = 0; i < store->subscribers->len; i++) {
            KajoConfigSub *sub = (KajoConfigSub *)g_ptr_array_index(store->subscribers, i);
            if (sub->id == sub_id) {
                g_atomic_int_set(&sub->active, 0);
                g_ptr_array_remove_index(store->subscribers, i);
                break;
            }
        }
    }
    g_mutex_unlock(&store->lock);
}
