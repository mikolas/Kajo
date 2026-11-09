#define _GNU_SOURCE
#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../dbus_helpers.h"
#include <gio/gio.h>
#include <string.h>

/* ─── Programmatic Bento Page Subclass: 02 / 12 Network ─── */

typedef struct _SettingsPageNetwork {
    GtkBox parent_instance;

    GtkWidget *card_radio;
    GtkWidget *sw_wifi;
    GtkWidget *active_conns_box;

    GtkWidget *card_scan;
    GtkWidget *btn_scan;
    GtkWidget *list_scan;

    /* Inline passphrase / status banner (.settings-auth-bar) */
    GtkWidget       *auth_revealer;
    GtkWidget       *auth_title;
    GtkWidget       *auth_status;
    GtkWidget       *auth_entry;
    GtkWidget       *btn_auth_connect;
    GtkWidget       *btn_auth_cancel;
    gchar           *auth_ap_path;
    gchar           *auth_ssid;
    ShellApSecurity  auth_security;

    GDBusConnection *sys_bus;
    GCancellable    *cancellable;
    guint            refresh_timer_id;
    gboolean         syncing_ui;
    gboolean         wifi_enabled;
    gchar           *primary_id;
    gchar           *primary_ipv4;
} SettingsPageNetwork;

typedef struct _SettingsPageNetworkClass {
    GtkBoxClass parent_class;
} SettingsPageNetworkClass;

G_DEFINE_TYPE(SettingsPageNetwork, settings_page_network, GTK_TYPE_BOX)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SettingsPageNetwork, g_object_unref)

static void refresh_network_state(SettingsPageNetwork *self);
static void schedule_net_refresh(SettingsPageNetwork *self, guint delay_ms);

static void
clear_box_children(GtkWidget *box)
{
    if (!box)
        return;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL)
        gtk_box_remove(GTK_BOX(box), child);
}

static void
update_network_telemetry(SettingsPageNetwork *self)
{
    gboolean connected = (self->primary_id && self->primary_id[0] != '\0');

    section_card_set_badge(self->card_radio,
                           connected ? "CONNECTED" : (self->wifi_enabled ? "IDLE" : "OFFLINE"),
                           connected ? "success" : NULL);

    settings_page_set_counter(GTK_WIDGET(self), 0, "BACKEND", "NetworkManager", NULL);
    settings_page_set_counter(GTK_WIDGET(self), 1, "RADIO",
                              self->wifi_enabled ? "WI-FI ON" : "WI-FI OFF",
                              self->wifi_enabled ? "success" : "warn");
    settings_page_set_counter(GTK_WIDGET(self), 2, "ACTIVE",
                              connected ? self->primary_id : "OFFLINE",
                              connected ? "accent" : NULL);
    settings_page_set_counter(GTK_WIDGET(self), 3, "IPV4",
                              (self->primary_ipv4 && self->primary_ipv4[0]) ? self->primary_ipv4 : "--",
                              NULL);

    settings_page_set_nav_badge(GTK_WIDGET(self), "network",
                                connected ? "ON" : (self->wifi_enabled ? "IDLE" : "OFF"),
                                connected ? "success" : NULL);
}

static void
scrub_password_entry(GtkWidget *entry)
{
    if (!entry)
        return;
    /* AGENTS.md §4.E: clear immediately and reset peek state */
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), FALSE);
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
}

static void
net_auth_show(SettingsPageNetwork *self, const char *title, const char *status, gboolean with_entry)
{
    gtk_label_set_text(GTK_LABEL(self->auth_title), title);
    gtk_label_set_text(GTK_LABEL(self->auth_status), status ? status : "WPA2/WPA3 Personal");
    gtk_widget_set_visible(self->auth_entry, with_entry);
    gtk_widget_set_visible(self->btn_auth_connect, with_entry);
    gtk_button_set_label(GTK_BUTTON(self->btn_auth_cancel), with_entry ? "CANCEL" : "CLOSE");
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->auth_revealer), TRUE);
    if (with_entry)
        gtk_widget_grab_focus(self->auth_entry);
}

static void
net_auth_hide(SettingsPageNetwork *self)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->auth_revealer), FALSE);
    scrub_password_entry(self->auth_entry);
}

static void
on_wifi_prop_set_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    (void)ret;

    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_autoptr(SettingsPageNetwork) self = g_weak_ref_get(ref);
        if (self)
            schedule_net_refresh(self, 800);
    }
    g_weak_ref_clear(ref);
    g_free(ref);
}

static void
on_net_wifi_switch_changed(GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNetwork *self = user_data;
    if (self->syncing_ui)
        return;

    gboolean state = gtk_switch_get_active(GTK_SWITCH(obj));
    self->wifi_enabled = state;
    update_network_telemetry(self);

    if (self->sys_bus) {
        GWeakRef *ref = g_new0(GWeakRef, 1);
        g_weak_ref_init(ref, self);
        g_dbus_connection_call(
            self->sys_bus,
            "org.freedesktop.NetworkManager",
            "/org/freedesktop/NetworkManager",
            "org.freedesktop.DBus.Properties",
            "Set",
            g_variant_new("(ssv)", "org.freedesktop.NetworkManager",
                          "WirelessEnabled", g_variant_new_boolean(state)),
            NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
            self->cancellable, on_wifi_prop_set_done, ref);
    } else {
        shell_dbus_nm_set_wireless_enabled(state);
        schedule_net_refresh(self, 1000);
    }
}

static void
on_deactivate_conn_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    (void)ret;

    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_autoptr(SettingsPageNetwork) self = g_weak_ref_get(ref);
        if (self)
            schedule_net_refresh(self, 700);
    }
    g_weak_ref_clear(ref);
    g_free(ref);
}

static void
on_net_disconnect_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageNetwork *self = user_data;
    const char *conn_path = g_object_get_data(G_OBJECT(btn), "conn-path");
    if (!conn_path || !self->sys_bus)
        return;

    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);
    g_dbus_connection_call(
        self->sys_bus,
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager",
        "DeactivateConnection",
        g_variant_new("(o)", conn_path),
        NULL, G_DBUS_CALL_FLAGS_NONE, 8000,
        self->cancellable, on_deactivate_conn_done, ref);
}

static void
on_net_connect_done(const char *error_message, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(SettingsPageNetwork) self = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);
    if (!self)
        return;

    if (error_message) {
        gboolean needs_pass = (self->auth_security == SHELL_AP_SEC_PSK ||
                               self->auth_security == SHELL_AP_SEC_SAE);
        g_autofree gchar *title = g_strdup_printf(
            "⚠ CONNECTION FAILED — %s",
            self->auth_ssid ? self->auth_ssid : "NETWORK");
        net_auth_show(self, title, error_message,
                      needs_pass && gtk_widget_get_visible(self->auth_entry));
    } else {
        net_auth_hide(self);
    }
    schedule_net_refresh(self, 900);
}

static void
net_start_connect(SettingsPageNetwork *self, const char *passphrase)
{
    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);
    shell_dbus_nm_connect_ap_async(self->auth_ap_path, self->auth_ssid,
                                   self->auth_security, passphrase,
                                   on_net_connect_done, ref);
}

static void
on_net_ap_connect_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageNetwork *self = user_data;
    const char *ap_path = g_object_get_data(G_OBJECT(btn), "ap-path");
    const char *ssid = g_object_get_data(G_OBJECT(btn), "ap-ssid");
    ShellApSecurity sec = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "ap-security"));
    if (!ap_path || !ssid)
        return;

    gchar *dup_path = g_strdup(ap_path);
    gchar *dup_ssid = g_strdup(ssid);
    g_free(self->auth_ap_path);
    g_free(self->auth_ssid);
    self->auth_ap_path = dup_path;
    self->auth_ssid = dup_ssid;
    self->auth_security = sec;

    gboolean needs_pass = (sec == SHELL_AP_SEC_PSK || sec == SHELL_AP_SEC_SAE) &&
                          !shell_dbus_nm_has_saved_connection(ssid);
    if (needs_pass) {
        g_autofree gchar *title = g_strdup_printf("🔒 PASSPHRASE REQUIRED — %s", ssid);
        const char *sec_hint = (sec == SHELL_AP_SEC_SAE)
                                   ? "WPA3-SAE Personal • scrubbed via explicit_bzero()"
                                   : "WPA/WPA2-PSK • scrubbed via explicit_bzero()";
        net_auth_show(self, title, sec_hint, TRUE);
        return;
    }

    net_start_connect(self, NULL);
}

static void
on_net_auth_submit(GtkWidget *w G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageNetwork *self = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->auth_entry));
    if (!text || text[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(self->auth_status), "Enter the network passphrase");
        return;
    }

    /* AGENTS.md §4.E: Copy, immediately clear GtkPasswordEntry + reset peek, scrub copy */
    gsize len = strlen(text);
    gchar *pass = g_strdup(text);
    scrub_password_entry(self->auth_entry);

    gtk_label_set_text(GTK_LABEL(self->auth_status), "Authenticating…");
    net_start_connect(self, pass);

    explicit_bzero(pass, len);
    g_free(pass);
}

static void
on_net_auth_cancel(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    net_auth_hide((SettingsPageNetwork *)user_data);
}

/* ─── Async Scan Trigger (Zero UI-Thread Blocking) ─── */

typedef struct {
    GWeakRef self_ref;
    gchar   *dev_path;
} NetDevScanCtx;

static void
net_dev_scan_ctx_free(NetDevScanCtx *ctx)
{
    if (!ctx)
        return;
    g_weak_ref_clear(&ctx->self_ref);
    g_free(ctx->dev_path);
    g_free(ctx);
}

static void
on_request_scan_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    (void)ret;

    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_autoptr(SettingsPageNetwork) self = g_weak_ref_get(ref);
        if (self)
            schedule_net_refresh(self, 1500);
    }
    g_weak_ref_clear(ref);
    g_free(ref);
}

static void
on_dev_type_checked_for_scan(GObject *src, GAsyncResult *res, gpointer user_data)
{
    NetDevScanCtx *ctx = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

    if (ret && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_autoptr(SettingsPageNetwork) self = g_weak_ref_get(&ctx->self_ref);
        g_autoptr(GVariant) inner = NULL;
        g_variant_get(ret, "(v)", &inner);
        if (self && inner && g_variant_get_uint32(inner) == 2 && self->sys_bus) {
            GWeakRef *ref = g_new0(GWeakRef, 1);
            g_weak_ref_init(ref, self);
            GVariantBuilder opts;
            g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
            g_dbus_connection_call(
                self->sys_bus,
                "org.freedesktop.NetworkManager",
                ctx->dev_path,
                "org.freedesktop.NetworkManager.Device.Wireless",
                "RequestScan",
                g_variant_new("(a{sv})", &opts),
                NULL, G_DBUS_CALL_FLAGS_NONE, 8000,
                self->cancellable, on_request_scan_done, ref);
        }
    }
    net_dev_scan_ctx_free(ctx);
}

static void
on_get_devices_for_scan_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

    if (ret && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_autoptr(SettingsPageNetwork) self = g_weak_ref_get(ref);
        if (self && self->sys_bus) {
            g_autoptr(GVariant) arr = g_variant_get_child_value(ret, 0);
            GVariantIter iter;
            g_variant_iter_init(&iter, arr);
            const gchar *dev_path = NULL;
            while (g_variant_iter_loop(&iter, "&o", &dev_path)) {
                NetDevScanCtx *ctx = g_new0(NetDevScanCtx, 1);
                g_weak_ref_init(&ctx->self_ref, self);
                ctx->dev_path = g_strdup(dev_path);
                g_dbus_connection_call(
                    self->sys_bus,
                    "org.freedesktop.NetworkManager",
                    dev_path,
                    "org.freedesktop.DBus.Properties",
                    "Get",
                    g_variant_new("(ss)", "org.freedesktop.NetworkManager.Device", "DeviceType"),
                    G_VARIANT_TYPE("(v)"),
                    G_DBUS_CALL_FLAGS_NONE, 5000,
                    self->cancellable, on_dev_type_checked_for_scan, ctx);
            }
        }
    }
    g_weak_ref_clear(ref);
    g_free(ref);
}

static void
trigger_wifi_scan_async(SettingsPageNetwork *self)
{
    if (!self)
        return;
    settings_page_set_live_status(GTK_WIDGET(self), "🔍 SCANNING",
                                  "Requesting 802.11 access point scan via NetworkManager…");
    if (self->sys_bus) {
        GWeakRef *ref = g_new0(GWeakRef, 1);
        g_weak_ref_init(ref, self);
        g_dbus_connection_call(
            self->sys_bus,
            "org.freedesktop.NetworkManager",
            "/org/freedesktop/NetworkManager",
            "org.freedesktop.NetworkManager",
            "GetDevices",
            NULL, G_VARIANT_TYPE("(ao)"),
            G_DBUS_CALL_FLAGS_NONE, 5000,
            self->cancellable, on_get_devices_for_scan_done, ref);
    }
    schedule_net_refresh(self, 1600);
}

static void
on_net_scan_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    trigger_wifi_scan_async((SettingsPageNetwork *)user_data);
}

static void
on_net_secondary_scan(gpointer user_data)
{
    trigger_wifi_scan_async((SettingsPageNetwork *)user_data);
}

/* ─── Async Active Connection & Access Point Population ─── */

typedef struct {
    GWeakRef self_ref;
    gchar   *conn_path;
    gchar   *conn_id;
    gchar   *conn_type;
} ActiveConnQueryCtx;

static void
active_conn_query_ctx_free(ActiveConnQueryCtx *ctx)
{
    if (!ctx)
        return;
    g_weak_ref_clear(&ctx->self_ref);
    g_free(ctx->conn_path);
    g_free(ctx->conn_id);
    g_free(ctx->conn_type);
    g_free(ctx);
}

static void
append_active_conn_row(SettingsPageNetwork *self,
                       const char          *conn_path,
                       const char          *conn_id,
                       const char          *conn_type,
                       const char          *ip_cidr)
{
    gboolean is_eth = (conn_type && strstr(conn_type, "ethernet") != NULL);
    const char *glyph = is_eth ? "🔌" : "🌐";
    const char *kind = is_eth ? "Ethernet" : "Wi-Fi";

    g_autofree gchar *title = g_strdup_printf("%s (Connected • %s)", conn_id, kind);
    g_autofree gchar *sub = g_strdup_printf("Active Connection • IPv4: %s",
                                            (ip_cidr && ip_cidr[0]) ? ip_cidr : "Obtaining…");

    GtkWidget *btn_disc = create_action_button("DISCONNECT", "conn-path", conn_path,
                                               G_CALLBACK(on_net_disconnect_clicked), self);
    gtk_widget_add_css_class(btn_disc, "files-nav-btn");

    GtkWidget *row = create_boxed_row(glyph, title, sub, btn_disc);
    gtk_widget_add_css_class(row, "active");

    /* Remove placeholder empty-state label if present */
    GtkWidget *first = gtk_widget_get_first_child(self->active_conns_box);
    if (first && GTK_IS_LABEL(first))
        gtk_box_remove(GTK_BOX(self->active_conns_box), first);

    gtk_box_append(GTK_BOX(self->active_conns_box), row);

    if (!self->primary_id) {
        self->primary_id = g_strdup(conn_id);
        g_free(self->primary_ipv4);
        self->primary_ipv4 = g_strdup((ip_cidr && ip_cidr[0]) ? ip_cidr : "--");
        update_network_telemetry(self);
    }
}

static void
on_ip4_address_data_ready(GObject *src, GAsyncResult *res, gpointer user_data)
{
    ActiveConnQueryCtx *ctx = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_autoptr(SettingsPageNetwork) self = g_weak_ref_get(&ctx->self_ref);
        if (self) {
            g_autofree gchar *ip_cidr = NULL;
            if (ret) {
                g_autoptr(GVariant) v_arr = NULL;
                g_variant_get(ret, "(v)", &v_arr);
                if (v_arr && g_variant_n_children(v_arr) > 0) {
                    g_autoptr(GVariant) first = g_variant_get_child_value(v_arr, 0);
                    g_autoptr(GVariant) v_addr = g_variant_lookup_value(first, "address", G_VARIANT_TYPE_STRING);
                    g_autoptr(GVariant) v_pref = g_variant_lookup_value(first, "prefix", G_VARIANT_TYPE_UINT32);
                    if (v_addr) {
                        const char *addr = g_variant_get_string(v_addr, NULL);
                        if (v_pref)
                            ip_cidr = g_strdup_printf("%s/%u", addr, g_variant_get_uint32(v_pref));
                        else
                            ip_cidr = g_strdup(addr);
                    }
                }
            }
            append_active_conn_row(self, ctx->conn_path, ctx->conn_id, ctx->conn_type, ip_cidr);
        }
    }
    active_conn_query_ctx_free(ctx);
}

static void
on_active_conn_props_ready(GObject *src, GAsyncResult *res, gpointer user_data)
{
    ActiveConnQueryCtx *ctx = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

    if (!ret || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        active_conn_query_ctx_free(ctx);
        return;
    }

    g_autoptr(SettingsPageNetwork) self = g_weak_ref_get(&ctx->self_ref);
    if (!self || !self->sys_bus) {
        active_conn_query_ctx_free(ctx);
        return;
    }

    g_autoptr(GVariant) dict = g_variant_get_child_value(ret, 0);
    g_autoptr(GVariant) v_id = g_variant_lookup_value(dict, "Id", G_VARIANT_TYPE_STRING);
    g_autoptr(GVariant) v_type = g_variant_lookup_value(dict, "Type", G_VARIANT_TYPE_STRING);
    g_autoptr(GVariant) v_ip4 = g_variant_lookup_value(dict, "Ip4Config", G_VARIANT_TYPE_OBJECT_PATH);

    const char *id_str = v_id ? g_variant_get_string(v_id, NULL) : "Network";
    const char *type_str = v_type ? g_variant_get_string(v_type, NULL) : "";

    /* Skip loopback virtual interface */
    if (g_strcmp0(id_str, "lo") == 0 || g_strcmp0(type_str, "loopback") == 0) {
        active_conn_query_ctx_free(ctx);
        return;
    }

    ctx->conn_id = g_strdup(id_str);
    ctx->conn_type = g_strdup(type_str);

    const char *ip4_path = v_ip4 ? g_variant_get_string(v_ip4, NULL) : "/";
    if (ip4_path && g_strcmp0(ip4_path, "/") != 0) {
        g_dbus_connection_call(
            self->sys_bus,
            "org.freedesktop.NetworkManager",
            ip4_path,
            "org.freedesktop.DBus.Properties",
            "Get",
            g_variant_new("(ss)", "org.freedesktop.NetworkManager.IP4Config", "AddressData"),
            G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, 5000,
            self->cancellable, on_ip4_address_data_ready, ctx);
        return;
    }

    append_active_conn_row(self, ctx->conn_path, ctx->conn_id, ctx->conn_type, "--");
    active_conn_query_ctx_free(ctx);
}

static void
populate_access_point_rows(SettingsPageNetwork *self)
{
    clear_box_children(self->list_scan);

    GPtrArray *aps = shell_dbus_nm_get_access_points();
    guint ap_count = 0;

    for (guint i = 0; i < aps->len; i++) {
        ShellAccessPoint *ap = g_ptr_array_index(aps, i);
        if (!ap->ssid || ap->ssid[0] == '\0')
            continue;

        ap_count++;
        const char *sec_label = "Open Network";
        switch (ap->security) {
        case SHELL_AP_SEC_PSK:         sec_label = "Secured (WPA/WPA2-PSK)"; break;
        case SHELL_AP_SEC_SAE:         sec_label = "Secured (WPA3-SAE)"; break;
        case SHELL_AP_SEC_UNSUPPORTED: sec_label = "WEP / 802.1X Enterprise"; break;
        default: break;
        }

        g_autofree gchar *sub = g_strdup_printf(
            "%s • Signal: %u%%%s",
            sec_label, ap->strength, ap->active ? " • Active" : "");

        GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_valign(right_box, GTK_ALIGN_CENTER);

        g_autofree gchar *sig_txt = g_strdup_printf("%u%%", ap->strength);
        GtkWidget *sig_badge = gtk_label_new(sig_txt);
        gtk_widget_add_css_class(sig_badge, "shell-popover-badge");
        if (ap->active)
            gtk_widget_add_css_class(sig_badge, "success");
        gtk_widget_set_valign(sig_badge, GTK_ALIGN_CENTER);

        GtkWidget *btn_conn = create_action_button(
            ap->active ? "CONNECTED" : "CONNECT", "ap-path", ap->path,
            G_CALLBACK(on_net_ap_connect_clicked), self);
        gtk_widget_add_css_class(btn_conn, "files-nav-btn");
        g_object_set_data_full(G_OBJECT(btn_conn), "ap-ssid", g_strdup(ap->ssid), g_free);
        g_object_set_data(G_OBJECT(btn_conn), "ap-security", GUINT_TO_POINTER((guint)ap->security));
        if (ap->active)
            gtk_widget_set_sensitive(btn_conn, FALSE);

        gtk_box_append(GTK_BOX(right_box), sig_badge);
        gtk_box_append(GTK_BOX(right_box), btn_conn);

        const char *glyph = (ap->security == SHELL_AP_SEC_OPEN) ? "🔓" : "🔒";
        GtkWidget *row = create_boxed_row(glyph, ap->ssid, sub, right_box);
        if (ap->active)
            gtk_widget_add_css_class(row, "active");

        gtk_box_append(GTK_BOX(self->list_scan), row);
    }
    g_ptr_array_unref(aps);

    if (ap_count == 0) {
        gtk_box_append(GTK_BOX(self->list_scan),
                       create_empty_state_label(
                           self->wifi_enabled
                               ? "No Wi-Fi access points found. Click '🔍 REFRESH SCAN'."
                               : "Wi-Fi radio is disabled. Enable the switch above to scan."));
    }
}

static void
on_nm_props_ready(GObject *src, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

    if (!ret || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_weak_ref_clear(ref);
        g_free(ref);
        return;
    }

    g_autoptr(SettingsPageNetwork) self = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);
    if (!self)
        return;

    g_autoptr(GVariant) dict = g_variant_get_child_value(ret, 0);
    g_autoptr(GVariant) v_wifi = g_variant_lookup_value(dict, "WirelessEnabled", G_VARIANT_TYPE_BOOLEAN);
    g_autoptr(GVariant) v_act = g_variant_lookup_value(dict, "ActiveConnections", G_VARIANT_TYPE_OBJECT_PATH_ARRAY);

    self->wifi_enabled = v_wifi ? g_variant_get_boolean(v_wifi) : FALSE;
    self->syncing_ui = TRUE;
    gtk_switch_set_active(GTK_SWITCH(self->sw_wifi), self->wifi_enabled);
    self->syncing_ui = FALSE;

    g_clear_pointer(&self->primary_id, g_free);
    g_clear_pointer(&self->primary_ipv4, g_free);
    clear_box_children(self->active_conns_box);
    gtk_box_append(GTK_BOX(self->active_conns_box),
                   create_empty_state_label("No active network connection currently established."));

    if (v_act && self->sys_bus) {
        GVariantIter iter;
        g_variant_iter_init(&iter, v_act);
        const gchar *conn_path = NULL;
        while (g_variant_iter_loop(&iter, "&o", &conn_path)) {
            ActiveConnQueryCtx *ctx = g_new0(ActiveConnQueryCtx, 1);
            g_weak_ref_init(&ctx->self_ref, self);
            ctx->conn_path = g_strdup(conn_path);
            g_dbus_connection_call(
                self->sys_bus,
                "org.freedesktop.NetworkManager",
                conn_path,
                "org.freedesktop.DBus.Properties",
                "GetAll",
                g_variant_new("(s)", "org.freedesktop.NetworkManager.Connection.Active"),
                G_VARIANT_TYPE("(a{sv})"),
                G_DBUS_CALL_FLAGS_NONE, 5000,
                self->cancellable, on_active_conn_props_ready, ctx);
        }
    }

    populate_access_point_rows(self);
    update_network_telemetry(self);
    settings_page_set_live_status(GTK_WIDGET(self), "✓ LIVE D-BUS",
                                  "org.freedesktop.NetworkManager • Changes apply immediately");
}

static void
refresh_network_state(SettingsPageNetwork *self)
{
    if (!self || !self->sys_bus)
        return;

    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);
    g_dbus_connection_call(
        self->sys_bus,
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", "org.freedesktop.NetworkManager"),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 5000,
        self->cancellable, on_nm_props_ready, ref);
}

static gboolean
on_net_refresh_timeout(gpointer user_data)
{
    SettingsPageNetwork *self = user_data;
    self->refresh_timer_id = 0;
    refresh_network_state(self);
    return G_SOURCE_REMOVE;
}

static void
schedule_net_refresh(SettingsPageNetwork *self, guint delay_ms)
{
    if (!self)
        return;
    if (self->refresh_timer_id != 0)
        g_source_remove(self->refresh_timer_id);
    self->refresh_timer_id = g_timeout_add(delay_ms, on_net_refresh_timeout, self);
}

static void
on_sys_bus_ready(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *ref = user_data;
    g_autoptr(GError) err = NULL;
    GDBusConnection *bus = g_bus_get_finish(res, &err);

    if (!bus || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_object(&bus);
        g_weak_ref_clear(ref);
        g_free(ref);
        return;
    }

    g_autoptr(SettingsPageNetwork) self = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);
    if (!self) {
        g_object_unref(bus);
        return;
    }

    self->sys_bus = bus;
    refresh_network_state(self);
}

static void
settings_page_network_dispose(GObject *object)
{
    SettingsPageNetwork *self = (SettingsPageNetwork *)object;

    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
    if (self->refresh_timer_id != 0) {
        g_source_remove(self->refresh_timer_id);
        self->refresh_timer_id = 0;
    }

    scrub_password_entry(self->auth_entry);
    g_clear_object(&self->sys_bus);
    g_clear_pointer(&self->auth_ap_path, g_free);
    g_clear_pointer(&self->auth_ssid, g_free);
    g_clear_pointer(&self->primary_id, g_free);
    g_clear_pointer(&self->primary_ipv4, g_free);

    G_OBJECT_CLASS(settings_page_network_parent_class)->dispose(object);
}

static void
settings_page_network_init(SettingsPageNetwork *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));

    self->cancellable = g_cancellable_new();
}

static void
settings_page_network_class_init(SettingsPageNetworkClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = settings_page_network_dispose;
}

static GtkWidget *
build_inline_auth_revealer(SettingsPageNetwork *self)
{
    self->auth_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(self->auth_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->auth_revealer), FALSE);

    GtkWidget *auth_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(auth_bar, "settings-auth-bar");

    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(top_row, GTK_ALIGN_CENTER);

    self->auth_title = gtk_label_new("🔒 PASSPHRASE REQUIRED");
    gtk_widget_add_css_class(self->auth_title, "shell-popover-badge");
    gtk_widget_add_css_class(self->auth_title, "accent");
    gtk_widget_set_halign(self->auth_title, GTK_ALIGN_START);
    gtk_widget_set_valign(self->auth_title, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(self->auth_title), PANGO_ELLIPSIZE_END);

    self->auth_status = gtk_label_new("WPA2/WPA3 PSK • scrubbed via explicit_bzero()");
    gtk_widget_add_css_class(self->auth_status, "row-subtitle");
    gtk_widget_set_hexpand(self->auth_status, TRUE);
    gtk_widget_set_halign(self->auth_status, GTK_ALIGN_END);
    gtk_widget_set_valign(self->auth_status, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(self->auth_status), PANGO_ELLIPSIZE_END);

    gtk_box_append(GTK_BOX(top_row), self->auth_title);
    gtk_box_append(GTK_BOX(top_row), self->auth_status);

    GtkWidget *entry_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(entry_row, GTK_ALIGN_CENTER);

    self->auth_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(self->auth_entry), TRUE);
    g_object_set(self->auth_entry, "placeholder-text", "Wi-Fi passphrase", NULL);
    gtk_editable_set_width_chars(GTK_EDITABLE(self->auth_entry), 8);
    gtk_widget_set_hexpand(self->auth_entry, TRUE);
    gtk_widget_set_valign(self->auth_entry, GTK_ALIGN_CENTER);

    self->btn_auth_connect = gtk_button_new_with_label("CONNECT");
    gtk_widget_add_css_class(self->btn_auth_connect, "files-nav-btn");
    gtk_widget_add_css_class(self->btn_auth_connect, "action-btn-primary");
    gtk_widget_set_valign(self->btn_auth_connect, GTK_ALIGN_CENTER);

    self->btn_auth_cancel = gtk_button_new_with_label("CANCEL");
    gtk_widget_add_css_class(self->btn_auth_cancel, "files-nav-btn");
    gtk_widget_set_valign(self->btn_auth_cancel, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(entry_row), self->auth_entry);
    gtk_box_append(GTK_BOX(entry_row), self->btn_auth_connect);
    gtk_box_append(GTK_BOX(entry_row), self->btn_auth_cancel);

    gtk_box_append(GTK_BOX(auth_bar), top_row);
    gtk_box_append(GTK_BOX(auth_bar), entry_row);
    gtk_revealer_set_child(GTK_REVEALER(self->auth_revealer), auth_bar);

    g_signal_connect(self->btn_auth_connect, "clicked", G_CALLBACK(on_net_auth_submit), self);
    g_signal_connect(self->auth_entry, "activate", G_CALLBACK(on_net_auth_submit), self);
    g_signal_connect(self->btn_auth_cancel, "clicked", G_CALLBACK(on_net_auth_cancel), self);

    return self->auth_revealer;
}

GtkWidget *
build_page_network(void)
{
    SettingsPageNetwork *self = g_object_new(settings_page_network_get_type(), NULL);

    /* ─── Card 1: Wireless Radio & Active Interfaces ─── */
    self->card_radio = create_section_card(
        SETTINGS_CARD_LEFT, "1. WIRELESS RADIO & ACTIVE INTERFACES",
        "QUERYING", NULL, NULL);

    self->sw_wifi = gtk_switch_new();
    g_signal_connect(self->sw_wifi, "notify::active", G_CALLBACK(on_net_wifi_switch_changed), self);
    gtk_box_append(GTK_BOX(self->card_radio),
                   create_boxed_row("📡", "Wi-Fi Wireless Networking Radio",
                                    "org.freedesktop.NetworkManager WirelessEnabled",
                                    self->sw_wifi));

    self->active_conns_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(self->active_conns_box),
                   create_empty_state_label("Querying NetworkManager active interfaces…"));
    gtk_box_append(GTK_BOX(self->card_radio), self->active_conns_box);

    settings_bento_add_full(GTK_WIDGET(self), self->card_radio);

    /* ─── Card 2: Available Wi-Fi Access Points + Inline Auth ─── */
    self->btn_scan = gtk_button_new_with_label("🔍 REFRESH SCAN");
    gtk_widget_add_css_class(self->btn_scan, "files-nav-btn");
    gtk_widget_set_valign(self->btn_scan, GTK_ALIGN_CENTER);
    g_signal_connect(self->btn_scan, "clicked", G_CALLBACK(on_net_scan_clicked), self);

    self->card_scan = create_section_card(
        SETTINGS_CARD_RIGHT, "2. AVAILABLE WI-FI ACCESS POINTS",
        NULL, NULL, self->btn_scan);
    gtk_widget_set_vexpand(self->card_scan, TRUE);

    gtk_box_append(GTK_BOX(self->card_scan), build_inline_auth_revealer(self));

    self->list_scan = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(self->list_scan),
                   create_empty_state_label("Scanning Wi-Fi access points…"));
    gtk_box_append(GTK_BOX(self->card_scan), self->list_scan);

    settings_bento_add_full(GTK_WIDGET(self), self->card_scan);

    settings_page_bind_actions(
        GTK_WIDGET(self),
        "🔍 REFRESH WI-FI SCAN",
        on_net_secondary_scan,
        NULL,
        NULL,
        self);
    settings_page_set_live_status(GTK_WIDGET(self), "✓ LIVE D-BUS",
                                  "org.freedesktop.NetworkManager • Changes apply immediately");

    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);
    g_bus_get(G_BUS_TYPE_SYSTEM, self->cancellable, on_sys_bus_ready, ref);

    return GTK_WIDGET(self);
}
