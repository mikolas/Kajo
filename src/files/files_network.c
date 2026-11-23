#include "files_network.h"
#include <string.h>

/* Avahi D-Bus constants (from avahi-common/address.h & defs.h) */
#define KAJO_AVAHI_IF_UNSPEC           (-1)
#define KAJO_AVAHI_PROTO_UNSPEC        (-1)
#define KAJO_AVAHI_PROTO_INET          (0)
#define KAJO_AVAHI_PROTO_INET6         (1)
#define KAJO_AVAHI_LOOKUP_RESULT_LOCAL (8u)

#define KAJO_AVAHI_SCAN_TIMEOUT_MS     1500u
#define KAJO_AVAHI_RESOLVE_TIMEOUT_MS  1500

typedef struct {
    const char *dnssd_type;
    const char *scheme;
    guint16     default_port;
    const char *label;
    const char *icon_name;
} AvahiServiceSpec;

static const AvahiServiceSpec k_avahi_services[] = {
    { "_smb._tcp",      "smb",  445,  "SMB Server",           "network-server-symbolic" },
    { "_sftp-ssh._tcp", "sftp", 22,   "SFTP Server",          "network-server-symbolic" },
    { "_webdav._tcp",   "dav",  80,   "WebDAV Server",        "folder-remote-symbolic"  },
    { "_webdavs._tcp",  "davs", 443,  "WebDAV HTTPS Server",  "folder-remote-symbolic"  },
    { "_ftp._tcp",      "ftp",  21,   "FTP Server",           "network-server-symbolic" },
    { "_nfs._tcp",      "nfs",  2049, "NFS Server",           "network-server-symbolic" },
};

static const AvahiServiceSpec *
find_service_spec(const char *service_type)
{
    if (!service_type)
        return NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(k_avahi_services); i++) {
        if (g_strcmp0(k_avahi_services[i].dnssd_type, service_type) == 0)
            return &k_avahi_services[i];
    }
    return NULL;
}

gboolean
kajo_files_network_is_remote_uri(const char *uri)
{
    if (!uri || uri[0] == '\0')
        return FALSE;

    g_autofree char *scheme = g_uri_parse_scheme(uri);
    if (!scheme)
        return FALSE;

    static const char *const remote_schemes[] = {
        "smb", "sftp", "ssh", "ftp", "ftps", "dav", "davs", "nfs", "afp", NULL
    };
    for (int i = 0; remote_schemes[i] != NULL; i++) {
        if (g_ascii_strcasecmp(scheme, remote_schemes[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

const char *
kajo_files_network_scheme_label(const char *uri)
{
    if (!uri || uri[0] == '\0')
        return "[ NETWORK ]";

    g_autofree char *scheme = g_uri_parse_scheme(uri);
    if (!scheme)
        return "[ NETWORK ]";

    if (g_ascii_strcasecmp(scheme, "smb") == 0)
        return "[ SMB ]";
    if (g_ascii_strcasecmp(scheme, "sftp") == 0 || g_ascii_strcasecmp(scheme, "ssh") == 0)
        return "[ SFTP ]";
    if (g_ascii_strcasecmp(scheme, "dav") == 0 || g_ascii_strcasecmp(scheme, "davs") == 0)
        return "[ WEBDAV ]";
    if (g_ascii_strcasecmp(scheme, "ftp") == 0 || g_ascii_strcasecmp(scheme, "ftps") == 0)
        return "[ FTP ]";
    if (g_ascii_strcasecmp(scheme, "nfs") == 0)
        return "[ NFS ]";
    if (g_ascii_strcasecmp(scheme, "afp") == 0)
        return "[ AFP ]";
    return "[ NETWORK ]";
}

const char *
kajo_files_network_scheme_description(const char *uri)
{
    if (!uri || uri[0] == '\0')
        return "Network Location";

    g_autofree char *scheme = g_uri_parse_scheme(uri);
    if (!scheme)
        return "Network Location";

    if (g_ascii_strcasecmp(scheme, "smb") == 0)
        return "SMB Network Location";
    if (g_ascii_strcasecmp(scheme, "sftp") == 0 || g_ascii_strcasecmp(scheme, "ssh") == 0)
        return "SFTP Network Location";
    if (g_ascii_strcasecmp(scheme, "dav") == 0 || g_ascii_strcasecmp(scheme, "davs") == 0)
        return "WebDAV Network Location";
    if (g_ascii_strcasecmp(scheme, "ftp") == 0 || g_ascii_strcasecmp(scheme, "ftps") == 0)
        return "FTP Network Location";
    if (g_ascii_strcasecmp(scheme, "nfs") == 0)
        return "NFS Network Location";
    if (g_ascii_strcasecmp(scheme, "afp") == 0)
        return "AFP Network Location";
    return "Network Location";
}

char *
kajo_files_network_canonicalize_uri(const char *uri)
{
    if (!uri || uri[0] == '\0')
        return NULL;

    g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_PARSE_RELAXED, NULL);
    if (!parsed) {
        g_autofree char *copy = g_strdup(uri);
        gsize len = strlen(copy);
        while (len > 1 && copy[len - 1] == '/')
            copy[--len] = '\0';
        return g_ascii_strdown(copy, -1);
    }

    const char *scheme = g_uri_get_scheme(parsed);
    const char *user = g_uri_get_user(parsed);
    const char *host = g_uri_get_host(parsed);
    int port = g_uri_get_port(parsed);
    const char *path = g_uri_get_path(parsed);

    g_autofree char *scheme_lo = scheme ? g_ascii_strdown(scheme, -1) : g_strdup("");
    g_autofree char *host_lo = host ? g_ascii_strdown(host, -1) : g_strdup("");
    g_autofree char *path_norm = g_strdup(path ? path : "");

    gsize plen = strlen(path_norm);
    while (plen > 0 && path_norm[plen - 1] == '/')
        path_norm[--plen] = '\0';

    GString *out = g_string_new(scheme_lo);
    g_string_append(out, "://");
    if (user && user[0] != '\0') {
        g_string_append(out, user);
        g_string_append_c(out, '@');
    }
    if (strchr(host_lo, ':') != NULL) {
        g_string_append_c(out, '[');
        g_string_append(out, host_lo);
        g_string_append_c(out, ']');
    } else {
        g_string_append(out, host_lo);
    }
    if (port > 0)
        g_string_append_printf(out, ":%d", port);
    if (path_norm[0] != '\0') {
        if (path_norm[0] != '/')
            g_string_append_c(out, '/');
        g_string_append(out, path_norm);
    }
    return g_string_free(out, FALSE);
}

char *
kajo_files_network_build_service_uri(const char *service_type,
                                     const char *host,
                                     const char *address,
                                     guint16     port,
                                     const char *path,
                                     const char *user)
{
    const AvahiServiceSpec *spec = find_service_spec(service_type);
    if (!spec)
        return NULL;

    const char *target_host = (address && address[0] != '\0') ? address : host;
    if (!target_host || target_host[0] == '\0')
        return NULL;

    GString *uri = g_string_new(spec->scheme);
    g_string_append(uri, "://");

    if (user && user[0] != '\0') {
        g_autofree char *esc_user = g_uri_escape_string(user, NULL, FALSE);
        g_string_append(uri, esc_user);
        g_string_append_c(uri, '@');
    }

    if (strchr(target_host, ':') != NULL && target_host[0] != '[') {
        g_string_append_c(uri, '[');
        g_string_append(uri, target_host);
        g_string_append_c(uri, ']');
    } else {
        g_string_append(uri, target_host);
    }

    if (port > 0 && port != spec->default_port)
        g_string_append_printf(uri, ":%u", (unsigned)port);

    if (path && path[0] != '\0') {
        if (path[0] != '/')
            g_string_append_c(uri, '/');
        g_string_append(uri, path);
    } else {
        g_string_append_c(uri, '/');
    }

    return g_string_free(uri, FALSE);
}

static gboolean
mark_uri_seen(GHashTable *seen_uris, const char *uri)
{
    if (!seen_uris || !uri || uri[0] == '\0')
        return FALSE;

    char *canon = kajo_files_network_canonicalize_uri(uri);
    if (!canon)
        return FALSE;

    if (g_hash_table_contains(seen_uris, canon)) {
        g_free(canon);
        return FALSE;
    }
    g_hash_table_add(seen_uris, canon);
    return TRUE;
}

static char *
default_server_title_for_uri(const char *uri)
{
    if (!uri || uri[0] == '\0')
        return g_strdup("Network Server");

    g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_PARSE_RELAXED, NULL);
    if (!parsed)
        return g_strdup(uri);

    const char *host = g_uri_get_host(parsed);
    const char *path = g_uri_get_path(parsed);
    if (host && host[0] != '\0') {
        if (path && path[0] != '\0' && g_strcmp0(path, "/") != 0) {
            const char *trimmed = (path[0] == '/') ? path + 1 : path;
            return g_strdup_printf("%s on %s", trimmed, host);
        }
        return g_strdup(host);
    }
    return g_strdup(uri);
}

/* ─── Saved / Recent Servers (~/.config/gtk-4.0/servers) ─── */

void
kajo_files_network_load_saved_servers_from_file(const char *bookmark_path,
                                                GPtrArray  *items,
                                                GHashTable *seen_uris)
{
    if (!bookmark_path || !items || !g_file_test(bookmark_path, G_FILE_TEST_IS_REGULAR))
        return;

    g_autoptr(GBookmarkFile) bf = g_bookmark_file_new();
    if (!g_bookmark_file_load_from_file(bf, bookmark_path, NULL))
        return;

    gsize len = 0;
    g_auto(GStrv) uris = g_bookmark_file_get_uris(bf, &len);
    for (gsize i = 0; i < len && uris && uris[i] != NULL; i++) {
        const char *uri = uris[i];
        if (!kajo_files_network_is_remote_uri(uri))
            continue;
        if (seen_uris && !mark_uri_seen(seen_uris, uri))
            continue;

        g_autofree char *title = g_bookmark_file_get_title(bf, uri, NULL);
        if (!title || title[0] == '\0') {
            g_free(title);
            title = default_server_title_for_uri(uri);
        }

        g_autofree char *desc = g_strdup_printf("Saved %s", kajo_files_network_scheme_description(uri));
        KajoFileItem *item = kajo_file_item_new_network_location(
            title,
            uri,
            desc,
            "network-server-symbolic",
            NULL
        );
        if (item)
            g_ptr_array_add(items, item);
    }
}

gboolean
kajo_files_network_record_server_to_file(const char *bookmark_path,
                                         const char *uri,
                                         const char *title,
                                         GError    **error)
{
    if (!bookmark_path || !uri || !kajo_files_network_is_remote_uri(uri)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid remote server URI");
        return FALSE;
    }

    g_autofree char *parent = g_path_get_dirname(bookmark_path);
    if (parent && parent[0] != '\0')
        g_mkdir_with_parents(parent, 0700);

    g_autoptr(GBookmarkFile) bf = g_bookmark_file_new();
    if (g_file_test(bookmark_path, G_FILE_TEST_IS_REGULAR))
        g_bookmark_file_load_from_file(bf, bookmark_path, NULL);

    g_autofree char *resolved_title = (title && title[0] != '\0')
        ? g_strdup(title)
        : default_server_title_for_uri(uri);

    g_bookmark_file_set_title(bf, uri, resolved_title);
    g_bookmark_file_add_application(bf, uri, "org.kajo.Files", "kajo-files %u");
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();
    if (now)
        g_bookmark_file_set_visited_date_time(bf, uri, now);

    return g_bookmark_file_to_file(bf, bookmark_path, error);
}

void
kajo_files_network_record_server(const char *uri, const char *title)
{
    if (!kajo_files_network_is_remote_uri(uri))
        return;

    const char *cfg_dir = g_get_user_config_dir();
    if (!cfg_dir || cfg_dir[0] == '\0')
        return;

    g_autofree char *path = g_build_filename(cfg_dir, "gtk-4.0", "servers", NULL);
    kajo_files_network_record_server_to_file(path, uri, title, NULL);
}

/* ─── Main-Thread Snapshot of Active Remote GVFS Mounts ─── */

GPtrArray *
kajo_files_network_snapshot_active_mounts(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
    g_autoptr(GVolumeMonitor) vm = g_volume_monitor_get();
    if (!vm)
        return out;

    GList *mounts = g_volume_monitor_get_mounts(vm);
    for (GList *l = mounts; l != NULL; l = l->next) {
        GMount *m = G_MOUNT(l->data);
        g_autoptr(GFile) root = g_mount_get_root(m);
        if (!root || g_file_is_native(root))
            continue;

        g_autofree char *uri = g_file_get_uri(root);
        if (!kajo_files_network_is_remote_uri(uri))
            continue;

        g_autofree char *name = g_mount_get_name(m);
        g_autoptr(GIcon) icon = g_mount_get_icon(m);
        g_autofree char *desc = g_strdup_printf("Mounted %s", kajo_files_network_scheme_description(uri));
        KajoFileItem *item = kajo_file_item_new_network_location(
            (name && name[0] != '\0') ? name : uri,
            uri,
            desc,
            "folder-remote-symbolic",
            icon
        );
        if (item)
            g_ptr_array_add(out, item);
    }
    g_list_free_full(mounts, g_object_unref);
    return out;
}

/* ─── Live Avahi mDNS / DNS-SD Discovery via System D-Bus ─── */

typedef struct {
    gint32 interface;
    gint32 protocol;
    char  *name;
    char  *type;
    char  *domain;
} AvahiDiscoveredSvc;

static void
avahi_discovered_svc_free(gpointer data)
{
    AvahiDiscoveredSvc *s = data;
    if (!s) return;
    g_free(s->name);
    g_free(s->type);
    g_free(s->domain);
    g_free(s);
}

typedef struct {
    GMainLoop  *loop;
    GHashTable *by_key;         /* "type\tdomain\tname" -> AvahiDiscoveredSvc* */
    guint       pending_browsers;
} AvahiScanState;

static void
on_avahi_browser_signal(GDBusConnection *conn G_GNUC_UNUSED,
                        const gchar     *sender_name G_GNUC_UNUSED,
                        const gchar     *object_path G_GNUC_UNUSED,
                        const gchar     *interface_name G_GNUC_UNUSED,
                        const gchar     *signal_name,
                        GVariant        *parameters,
                        gpointer         user_data)
{
    AvahiScanState *st = user_data;
    if (!st) return;

    if (g_strcmp0(signal_name, "ItemNew") == 0) {
        gint32 iface = 0, proto = 0;
        const char *name = NULL, *type = NULL, *domain = NULL;
        guint32 flags = 0;
        g_variant_get(parameters, "(ii&s&s&su)", &iface, &proto, &name, &type, &domain, &flags);

        /* Skip advertisements published by this local host itself */
        if ((flags & KAJO_AVAHI_LOOKUP_RESULT_LOCAL) != 0)
            return;
        if (!name || !type || !domain || name[0] == '\0')
            return;

        g_autofree char *key = g_strdup_printf("%s\t%s\t%s", type, domain, name);
        AvahiDiscoveredSvc *existing = g_hash_table_lookup(st->by_key, key);
        if (existing) {
            /* Prefer IPv4 record over IPv6 when the same service appears on both */
            if (existing->protocol != KAJO_AVAHI_PROTO_INET && proto == KAJO_AVAHI_PROTO_INET) {
                existing->interface = iface;
                existing->protocol = proto;
            }
            return;
        }

        AvahiDiscoveredSvc *svc = g_new0(AvahiDiscoveredSvc, 1);
        svc->interface = iface;
        svc->protocol = proto;
        svc->name = g_strdup(name);
        svc->type = g_strdup(type);
        svc->domain = g_strdup(domain);
        g_hash_table_insert(st->by_key, g_steal_pointer(&key), svc);
    } else if (g_strcmp0(signal_name, "AllForNow") == 0 ||
               g_strcmp0(signal_name, "Failure") == 0) {
        if (st->pending_browsers > 0)
            st->pending_browsers--;
        if (st->pending_browsers == 0 && st->loop)
            g_main_loop_quit(st->loop);
    }
}

static gboolean
on_avahi_scan_timeout(gpointer user_data)
{
    GMainLoop *loop = user_data;
    if (loop)
        g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void
on_avahi_cancelled(GCancellable *cancellable G_GNUC_UNUSED, gpointer user_data)
{
    GMainLoop *loop = user_data;
    if (loop)
        g_main_loop_quit(loop);
}

static void
parse_avahi_txt_records(GVariant *txt_variant, char **out_path, char **out_user)
{
    *out_path = NULL;
    *out_user = NULL;
    if (!txt_variant)
        return;

    GVariantIter iter;
    GVariant *entry = NULL;
    g_variant_iter_init(&iter, txt_variant);
    while ((entry = g_variant_iter_next_value(&iter)) != NULL) {
        gsize n = 0;
        const guint8 *bytes = g_variant_get_fixed_array(entry, &n, 1);
        if (bytes && n > 0) {
            g_autofree char *kv = g_strndup((const char *)bytes, n);
            if (!*out_path && g_str_has_prefix(kv, "path=") && kv[5] != '\0')
                *out_path = g_strdup(kv + 5);
            else if (!*out_user && g_str_has_prefix(kv, "u=") && kv[2] != '\0')
                *out_user = g_strdup(kv + 2);
        }
        g_variant_unref(entry);
    }
}

static gboolean
is_loopback_address(const char *addr)
{
    if (!addr) return FALSE;
    return g_str_has_prefix(addr, "127.") || g_strcmp0(addr, "::1") == 0;
}

static KajoFileItem *
resolve_avahi_discovered_service(GDBusConnection          *bus,
                                 const AvahiDiscoveredSvc *svc,
                                 GHashTable               *seen_uris,
                                 GCancellable             *cancellable)
{
    const AvahiServiceSpec *spec = find_service_spec(svc->type);
    if (!spec)
        return NULL;

    g_autoptr(GVariant) resolved = g_dbus_connection_call_sync(
        bus,
        "org.freedesktop.Avahi",
        "/",
        "org.freedesktop.Avahi.Server2",
        "ResolveService",
        g_variant_new("(iisssiu)",
                      svc->interface,
                      svc->protocol,
                      svc->name,
                      svc->type,
                      svc->domain,
                      KAJO_AVAHI_PROTO_INET,
                      0u),
        G_VARIANT_TYPE("(iissssisqaayu)"),
        G_DBUS_CALL_FLAGS_NONE,
        KAJO_AVAHI_RESOLVE_TIMEOUT_MS,
        cancellable,
        NULL
    );
    if (!resolved && svc->protocol != KAJO_AVAHI_PROTO_INET && !g_cancellable_is_cancelled(cancellable)) {
        resolved = g_dbus_connection_call_sync(
            bus,
            "org.freedesktop.Avahi",
            "/",
            "org.freedesktop.Avahi.Server2",
            "ResolveService",
            g_variant_new("(iisssiu)",
                          svc->interface,
                          svc->protocol,
                          svc->name,
                          svc->type,
                          svc->domain,
                          KAJO_AVAHI_PROTO_UNSPEC,
                          0u),
            G_VARIANT_TYPE("(iissssisqaayu)"),
            G_DBUS_CALL_FLAGS_NONE,
            KAJO_AVAHI_RESOLVE_TIMEOUT_MS,
            cancellable,
            NULL
        );
    }
    if (!resolved)
        return NULL;

    gint32 r_iface = 0, r_proto = 0, r_aproto = 0;
    const char *r_name = NULL, *r_type = NULL, *r_domain = NULL;
    const char *r_host = NULL, *r_addr = NULL;
    guint16 r_port = 0;
    g_autoptr(GVariant) r_txt = NULL;
    guint32 r_flags = 0;

    g_variant_get(resolved, "(ii&s&s&s&si&sq@aayu)",
                  &r_iface, &r_proto, &r_name, &r_type, &r_domain,
                  &r_host, &r_aproto, &r_addr, &r_port, &r_txt, &r_flags);

    if (is_loopback_address(r_addr))
        return NULL;

    g_autofree char *txt_path = NULL;
    g_autofree char *txt_user = NULL;
    parse_avahi_txt_records(r_txt, &txt_path, &txt_user);

    g_autofree char *uri = kajo_files_network_build_service_uri(
        svc->type, r_host, r_addr, r_port, txt_path, txt_user
    );
    if (!uri || !mark_uri_seen(seen_uris, uri))
        return NULL;

    g_autofree char *desc = (r_addr && r_addr[0] != '\0')
        ? g_strdup_printf("%s (%s)", spec->label, r_addr)
        : g_strdup(spec->label);

    return kajo_file_item_new_network_location(
        svc->name,
        uri,
        desc,
        spec->icon_name,
        NULL
    );
}

static void
discover_avahi_services_sync(GPtrArray    *items,
                             GHashTable   *seen_uris,
                             GCancellable *cancellable)
{
    if (g_cancellable_is_cancelled(cancellable))
        return;
    if (g_getenv("KAJO_FILES_DISABLE_AVAHI") != NULL)
        return;

    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, cancellable, NULL);
    if (!bus)
        return;

    GMainContext *ctx = g_main_context_new();
    GMainLoop *loop = g_main_loop_new(ctx, FALSE);
    g_main_context_push_thread_default(ctx);

    AvahiScanState st = {
        .loop = loop,
        .by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, avahi_discovered_svc_free),
        .pending_browsers = 0,
    };

    g_autoptr(GPtrArray) browser_paths = g_ptr_array_new_with_free_func(g_free);
    guint sub_ids[G_N_ELEMENTS(k_avahi_services)];
    guint n_subs = 0;

    for (gsize i = 0; i < G_N_ELEMENTS(k_avahi_services); i++) {
        if (g_cancellable_is_cancelled(cancellable))
            break;

        g_autoptr(GVariant) res = g_dbus_connection_call_sync(
            bus,
            "org.freedesktop.Avahi",
            "/",
            "org.freedesktop.Avahi.Server2",
            "ServiceBrowserPrepare",
            g_variant_new("(iissu)",
                          KAJO_AVAHI_IF_UNSPEC,
                          KAJO_AVAHI_PROTO_UNSPEC,
                          k_avahi_services[i].dnssd_type,
                          "",
                          0u),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            KAJO_AVAHI_RESOLVE_TIMEOUT_MS,
            cancellable,
            NULL
        );
        if (!res)
            continue;

        const char *obj_path = NULL;
        g_variant_get(res, "(&o)", &obj_path);
        if (!obj_path || obj_path[0] == '\0')
            continue;

        guint sub_id = g_dbus_connection_signal_subscribe(
            bus,
            "org.freedesktop.Avahi",
            "org.freedesktop.Avahi.ServiceBrowser",
            NULL,
            obj_path,
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_avahi_browser_signal,
            &st,
            NULL
        );

        g_autoptr(GVariant) start_res = g_dbus_connection_call_sync(
            bus,
            "org.freedesktop.Avahi",
            obj_path,
            "org.freedesktop.Avahi.ServiceBrowser",
            "Start",
            NULL,
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            KAJO_AVAHI_RESOLVE_TIMEOUT_MS,
            cancellable,
            NULL
        );
        if (!start_res) {
            g_dbus_connection_signal_unsubscribe(bus, sub_id);
            g_dbus_connection_call_sync(
                bus, "org.freedesktop.Avahi", obj_path,
                "org.freedesktop.Avahi.ServiceBrowser", "Free",
                NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL
            );
            continue;
        }

        sub_ids[n_subs++] = sub_id;
        g_ptr_array_add(browser_paths, g_strdup(obj_path));
        st.pending_browsers++;
    }

    if (st.pending_browsers > 0 && !g_cancellable_is_cancelled(cancellable)) {
        GSource *timeout_src = g_timeout_source_new(KAJO_AVAHI_SCAN_TIMEOUT_MS);
        g_source_set_callback(timeout_src, on_avahi_scan_timeout, loop, NULL);
        g_source_attach(timeout_src, ctx);

        gulong cancel_hid = 0;
        if (cancellable)
            cancel_hid = g_cancellable_connect(cancellable, G_CALLBACK(on_avahi_cancelled), loop, NULL);

        g_main_loop_run(loop);

        if (cancellable && cancel_hid > 0)
            g_cancellable_disconnect(cancellable, cancel_hid);
        g_source_destroy(timeout_src);
        g_source_unref(timeout_src);
    }

    for (guint i = 0; i < n_subs; i++)
        g_dbus_connection_signal_unsubscribe(bus, sub_ids[i]);
    for (guint i = 0; i < browser_paths->len; i++) {
        const char *obj_path = g_ptr_array_index(browser_paths, i);
        g_dbus_connection_call_sync(
            bus, "org.freedesktop.Avahi", obj_path,
            "org.freedesktop.Avahi.ServiceBrowser", "Free",
            NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL
        );
    }

    g_main_context_pop_thread_default(ctx);
    g_main_loop_unref(loop);
    g_main_context_unref(ctx);

    GHashTableIter hiter;
    gpointer hval = NULL;
    g_hash_table_iter_init(&hiter, st.by_key);
    while (g_hash_table_iter_next(&hiter, NULL, &hval)) {
        if (g_cancellable_is_cancelled(cancellable))
            break;
        KajoFileItem *item = resolve_avahi_discovered_service(bus, hval, seen_uris, cancellable);
        if (item)
            g_ptr_array_add(items, item);
    }

    g_hash_table_unref(st.by_key);
}

/* ─── Unified network:/// Enumeration ─── */

GPtrArray *
kajo_files_network_enumerate_sync(GFile        *network_dir,
                                  GPtrArray    *mounted_snapshot,
                                  GCancellable *cancellable,
                                  GError      **error)
{
    if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return NULL;

    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    g_autoptr(GHashTable) seen_uris = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* 1. Live Avahi mDNS/DNS-SD discovery (SMB, SFTP, WebDAV, FTP, NFS on LAN) */
    discover_avahi_services_sync(items, seen_uris, cancellable);
    if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
        g_ptr_array_unref(items);
        return NULL;
    }

    /* 2. Native GVFS network:/// backend (gvfs-dnssd / gvfs-wsdd / gvfs-smb-browse if installed) */
    if (network_dir) {
        const char *attrs =
            G_FILE_ATTRIBUTE_STANDARD_NAME ","
            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
            G_FILE_ATTRIBUTE_STANDARD_TYPE ","
            G_FILE_ATTRIBUTE_STANDARD_ICON ","
            G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE ","
            G_FILE_ATTRIBUTE_STANDARD_TARGET_URI ","
            G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT;
        g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
            network_dir, attrs, G_FILE_QUERY_INFO_NONE, cancellable, NULL
        );
        if (enumerator) {
            while (!g_cancellable_is_cancelled(cancellable)) {
                g_autoptr(GFileInfo) info = g_file_enumerator_next_file(enumerator, cancellable, NULL);
                if (!info)
                    break;
                KajoFileItem *item = kajo_file_item_new_from_info(network_dir, info);
                if (!item)
                    continue;
                const char *key_uri = kajo_file_item_get_target_uri(item);
                if (!key_uri)
                    key_uri = kajo_file_item_get_uri(item);
                if (mark_uri_seen(seen_uris, key_uri))
                    g_ptr_array_add(items, item);
                else
                    g_object_unref(item);
            }
            g_file_enumerator_close(enumerator, NULL, NULL);
        }
    }

    if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
        g_ptr_array_unref(items);
        return NULL;
    }

    /* 3. Active remote GVFS mounts snapshotted on main thread */
    if (mounted_snapshot) {
        for (guint i = 0; i < mounted_snapshot->len; i++) {
            KajoFileItem *m_item = g_ptr_array_index(mounted_snapshot, i);
            const char *uri = kajo_file_item_get_target_uri(m_item);
            if (!uri)
                uri = kajo_file_item_get_uri(m_item);
            if (mark_uri_seen(seen_uris, uri))
                g_ptr_array_add(items, g_object_ref(m_item));
        }
    }

    /* 4. Saved / recent servers from GTK/Nautilus XBEL files */
    const char *cfg_dir = g_get_user_config_dir();
    if (cfg_dir && cfg_dir[0] != '\0') {
        g_autofree char *gtk4_servers = g_build_filename(cfg_dir, "gtk-4.0", "servers", NULL);
        kajo_files_network_load_saved_servers_from_file(gtk4_servers, items, seen_uris);

        g_autofree char *gtk3_servers = g_build_filename(cfg_dir, "gtk-3.0", "servers", NULL);
        kajo_files_network_load_saved_servers_from_file(gtk3_servers, items, seen_uris);
    }

    return items;
}
