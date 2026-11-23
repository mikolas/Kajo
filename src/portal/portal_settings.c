#include "portal_settings.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define APPEARANCE_NS "org.freedesktop.appearance"

static const gchar k_settings_introspection_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Settings'>"
    "    <method name='ReadAll'>"
    "      <arg type='as' name='namespaces' direction='in'/>"
    "      <arg type='a{sa{sv}}' name='value' direction='out'/>"
    "    </method>"
    "    <method name='Read'>"
    "      <arg type='s' name='namespace' direction='in'/>"
    "      <arg type='s' name='key' direction='in'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </method>"
    "    <signal name='SettingChanged'>"
    "      <arg type='s' name='namespace' direction='out'/>"
    "      <arg type='s' name='key' direction='out'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </signal>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

struct _KajoPortalSettings {
    GDBusConnection *connection;
    GDBusNodeInfo   *introspection;
    guint            bus_name_id;
    guint            reg_id;

    guint32          color_scheme;    /* 1 = prefer dark */
    guint32          contrast;        /* 0 = normal, 1 = high */
    guint32          reduced_motion;  /* 0 = no preference */
    double           accent_r;
    double           accent_g;
    double           accent_b;
};

gboolean
kajo_portal_settings_parse_accent_rgb(const char *hex,
                                      double     *out_r,
                                      double     *out_g,
                                      double     *out_b)
{
    if (!hex || hex[0] != '#')
        return FALSE;

    gsize len = strlen(hex);
    unsigned int r = 0, g = 0, b = 0;
    if (len == 7) {
        if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) != 3)
            return FALSE;
    } else if (len == 4) {
        if (sscanf(hex + 1, "%01x%01x%01x", &r, &g, &b) != 3)
            return FALSE;
        r *= 17;
        g *= 17;
        b *= 17;
    } else {
        return FALSE;
    }

    if (out_r) *out_r = (double)r / 255.0;
    if (out_g) *out_g = (double)g / 255.0;
    if (out_b) *out_b = (double)b / 255.0;
    return TRUE;
}

gboolean
kajo_portal_settings_namespace_matches(const char *pattern, const char *ns)
{
    if (!ns)
        return FALSE;
    if (!pattern || pattern[0] == '\0' || g_strcmp0(pattern, "*") == 0)
        return TRUE;
    if (g_strcmp0(pattern, ns) == 0)
        return TRUE;

    gsize plen = strlen(pattern);
    if (plen >= 2 && pattern[plen - 1] == '*' && pattern[plen - 2] == '.') {
        /* Trailing glob like "org.freedesktop.*" matches "org.freedesktop.appearance" */
        return strncmp(ns, pattern, plen - 1) == 0;
    }
    return FALSE;
}

static gboolean
namespaces_match_any(const gchar *const *patterns, const char *ns)
{
    if (!patterns || patterns[0] == NULL)
        return TRUE;
    for (gsize i = 0; patterns[i] != NULL; i++) {
        if (kajo_portal_settings_namespace_matches(patterns[i], ns))
            return TRUE;
    }
    return FALSE;
}

static GVariant *
lookup_setting_value(const KajoPortalSettings *self,
                     const char               *ns,
                     const char               *key)
{
    if (!self || g_strcmp0(ns, APPEARANCE_NS) != 0 || !key)
        return NULL;

    if (g_strcmp0(key, "color-scheme") == 0)
        return g_variant_new_uint32(self->color_scheme);
    if (g_strcmp0(key, "accent-color") == 0)
        return g_variant_new("(ddd)", self->accent_r, self->accent_g, self->accent_b);
    if (g_strcmp0(key, "contrast") == 0)
        return g_variant_new_uint32(self->contrast);
    if (g_strcmp0(key, "reduced-motion") == 0)
        return g_variant_new_uint32(self->reduced_motion);
    return NULL;
}

static void
handle_method_call(GDBusConnection       *connection G_GNUC_UNUSED,
                   const gchar           *sender G_GNUC_UNUSED,
                   const gchar           *object_path G_GNUC_UNUSED,
                   const gchar           *interface_name G_GNUC_UNUSED,
                   const gchar           *method_name,
                   GVariant              *parameters,
                   GDBusMethodInvocation *invocation,
                   gpointer               user_data)
{
    KajoPortalSettings *self = user_data;

    if (g_strcmp0(method_name, "ReadAll") == 0) {
        g_autofree const gchar **namespaces = NULL;
        g_variant_get(parameters, "(^a&s)", &namespaces);

        GVariantBuilder outer;
        g_variant_builder_init(&outer, G_VARIANT_TYPE("a{sa{sv}}"));

        if (namespaces_match_any(namespaces, APPEARANCE_NS)) {
            GVariantBuilder inner;
            g_variant_builder_init(&inner, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&inner, "{sv}", "color-scheme",
                                  g_variant_new_uint32(self->color_scheme));
            g_variant_builder_add(&inner, "{sv}", "accent-color",
                                  g_variant_new("(ddd)", self->accent_r, self->accent_g, self->accent_b));
            g_variant_builder_add(&inner, "{sv}", "contrast",
                                  g_variant_new_uint32(self->contrast));
            g_variant_builder_add(&inner, "{sv}", "reduced-motion",
                                  g_variant_new_uint32(self->reduced_motion));
            g_variant_builder_add(&outer, "{sa{sv}}", APPEARANCE_NS, &inner);
        }

        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sa{sv}})", &outer));
        return;
    }

    if (g_strcmp0(method_name, "Read") == 0) {
        const gchar *ns = NULL;
        const gchar *key = NULL;
        g_variant_get(parameters, "(&s&s)", &ns, &key);

        GVariant *val = lookup_setting_value(self, ns, key);
        if (!val) {
            g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.freedesktop.portal.Error.NotFound",
                "Requested setting not found");
            return;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", val));
        return;
    }

    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
        "Unknown method '%s'", method_name);
}

static GVariant *
handle_get_property(GDBusConnection *connection G_GNUC_UNUSED,
                    const gchar     *sender G_GNUC_UNUSED,
                    const gchar     *object_path G_GNUC_UNUSED,
                    const gchar     *interface_name G_GNUC_UNUSED,
                    const gchar     *property_name,
                    GError         **error G_GNUC_UNUSED,
                    gpointer         user_data G_GNUC_UNUSED)
{
    if (g_strcmp0(property_name, "version") == 0)
        return g_variant_new_uint32(1);
    return NULL;
}

static const GDBusInterfaceVTable k_settings_vtable = {
    .method_call  = handle_method_call,
    .get_property = handle_get_property,
    .set_property = NULL,
};

static void
emit_setting_changed(KajoPortalSettings *self, const char *key, GVariant *val)
{
    if (!self || !self->connection || self->reg_id == 0) {
        if (val)
            g_variant_unref(g_variant_ref_sink(val));
        return;
    }

    g_dbus_connection_emit_signal(
        self->connection,
        NULL,
        KAJO_PORTAL_SETTINGS_OBJECT_PATH,
        KAJO_PORTAL_SETTINGS_INTERFACE,
        "SettingChanged",
        g_variant_new("(ssv)", APPEARANCE_NS, key, val),
        NULL);
}

static void
compute_values_from_theme(const KajoThemeCfg *theme,
                          guint32            *out_scheme,
                          guint32            *out_contrast,
                          double             *out_r,
                          double             *out_g,
                          double             *out_b)
{
    *out_scheme = 1; /* Kajo uses dark Aero + Metro surfaces */
    *out_contrast = 0;
    *out_r = 0.0;
    *out_g = 120.0 / 255.0;
    *out_b = 212.0 / 255.0;

    if (!theme)
        return;

    if (theme->accent_color)
        kajo_portal_settings_parse_accent_rgb(theme->accent_color, out_r, out_g, out_b);

    if (theme->surface_contrast >= 1.35 || theme->secondary_fg_contrast >= 1.35)
        *out_contrast = 1;
}

static void
register_object_on_connection(KajoPortalSettings *self, GDBusConnection *conn)
{
    if (!self || !conn || self->reg_id != 0)
        return;

    g_set_object(&self->connection, conn);

    g_autoptr(GError) err = NULL;
    self->reg_id = g_dbus_connection_register_object(
        conn,
        KAJO_PORTAL_SETTINGS_OBJECT_PATH,
        self->introspection->interfaces[0],
        &k_settings_vtable,
        self,
        NULL,
        &err);
    if (self->reg_id == 0 && err)
        g_warning("portal-settings: failed to register object: %s", err->message);
}

static void
on_bus_acquired(GDBusConnection *connection,
                const gchar     *name G_GNUC_UNUSED,
                gpointer         user_data)
{
    register_object_on_connection((KajoPortalSettings *)user_data, connection);
}

static KajoPortalSettings *
portal_settings_alloc(const KajoThemeCfg *initial_theme)
{
    KajoPortalSettings *self = g_new0(KajoPortalSettings, 1);
    self->introspection = g_dbus_node_info_new_for_xml(k_settings_introspection_xml, NULL);
    self->reduced_motion = 0;
    compute_values_from_theme(initial_theme,
                              &self->color_scheme,
                              &self->contrast,
                              &self->accent_r,
                              &self->accent_g,
                              &self->accent_b);
    return self;
}

KajoPortalSettings *
kajo_portal_settings_new(const KajoThemeCfg *initial_theme)
{
    KajoPortalSettings *self = portal_settings_alloc(initial_theme);
    self->bus_name_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        KAJO_PORTAL_SETTINGS_BUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
        on_bus_acquired,
        NULL,
        NULL,
        self,
        NULL);
    return self;
}

KajoPortalSettings *
kajo_portal_settings_new_for_connection(GDBusConnection    *connection,
                                        const KajoThemeCfg *initial_theme)
{
    g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection), NULL);

    KajoPortalSettings *self = portal_settings_alloc(initial_theme);
    register_object_on_connection(self, connection);
    return self;
}

void
kajo_portal_settings_update_from_theme(KajoPortalSettings *self,
                                       const KajoThemeCfg *theme)
{
    if (!self)
        return;

    guint32 new_scheme = 1, new_contrast = 0;
    double new_r = 0.0, new_g = 0.0, new_b = 0.0;
    compute_values_from_theme(theme, &new_scheme, &new_contrast, &new_r, &new_g, &new_b);

    if (new_scheme != self->color_scheme) {
        self->color_scheme = new_scheme;
        emit_setting_changed(self, "color-scheme", g_variant_new_uint32(self->color_scheme));
    }
    if (fabs(new_r - self->accent_r) > 1e-6 ||
        fabs(new_g - self->accent_g) > 1e-6 ||
        fabs(new_b - self->accent_b) > 1e-6) {
        self->accent_r = new_r;
        self->accent_g = new_g;
        self->accent_b = new_b;
        emit_setting_changed(self, "accent-color",
                             g_variant_new("(ddd)", self->accent_r, self->accent_g, self->accent_b));
    }
    if (new_contrast != self->contrast) {
        self->contrast = new_contrast;
        emit_setting_changed(self, "contrast", g_variant_new_uint32(self->contrast));
    }
}

void
kajo_portal_settings_destroy(KajoPortalSettings *self)
{
    if (!self)
        return;

    if (self->connection && self->reg_id != 0) {
        g_dbus_connection_unregister_object(self->connection, self->reg_id);
        self->reg_id = 0;
    }
    if (self->bus_name_id != 0) {
        g_bus_unown_name(self->bus_name_id);
        self->bus_name_id = 0;
    }
    g_clear_object(&self->connection);
    g_clear_pointer(&self->introspection, g_dbus_node_info_unref);
    g_free(self);
}
