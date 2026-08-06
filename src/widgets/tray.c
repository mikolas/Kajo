#include "widget.h"
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNW_BUS_NAME "org.kde.StatusNotifierWatcher"
#define SNW_OBJECT_PATH "/StatusNotifierWatcher"
#define SNW_INTERFACE "org.kde.StatusNotifierWatcher"
#define SNI_INTERFACE "org.kde.StatusNotifierItem"

extern const ShellWidgetClass tray_widget_class;

typedef struct {
    gchar *service;
    gchar *object_path;
    GDBusProxy *proxy;
    GtkWidget *button;
    GtkWidget *icon_img;
    gchar *icon_name;
    gchar *title;
    gchar *menu_path;
    gpointer signal_pair;
    gulong signal_id;
    guint name_watcher_id;
} TrayItem;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *box;
    GPtrArray *items; /* Array of TrayItem* */

    GDBusConnection *bus;
    guint watcher_owner_id;
    guint watcher_reg_id;
} TrayWidget;

typedef struct {
    TrayWidget *tray;
    TrayItem   *item;
} TrayPair;

static void refresh_tray_item(TrayWidget *tray, TrayItem *item);
static void remove_tray_item(TrayWidget *tray, const gchar *service);
static void on_tray_item_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data);
static void on_sni_signal(GDBusProxy *proxy, gchar *sender_name, gchar *signal_name, GVariant *parameters, gpointer user_data);

/* ─── D-Bus StatusNotifierWatcher Implementation ─── */

static const gchar watcher_introspection_xml[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierWatcher'>"
    "    <method name='RegisterStatusNotifierItem'>"
    "      <arg type='s' name='service' direction='in'/>"
    "    </method>"
    "    <method name='RegisterStatusNotifierHost'>"
    "      <arg type='s' name='service' direction='in'/>"
    "    </method>"
    "    <property type='as' name='RegisteredStatusNotifierItems' access='read'/>"
    "    <property type='b' name='IsStatusNotifierHostRegistered' access='read'/>"
    "    <property type='i' name='ProtocolVersion' access='read'/>"
    "    <signal name='StatusNotifierItemRegistered'>"
    "      <arg type='s' name='service'/>"
    "    </signal>"
    "    <signal name='StatusNotifierItemUnregistered'>"
    "      <arg type='s' name='service'/>"
    "    </signal>"
    "    <signal name='StatusNotifierHostRegistered'/>"
    "  </interface>"
    "</node>";

static void
on_tray_item_clicked(GtkButton *btn, gpointer user_data)
{
    TrayItem *item = user_data;
    if (item && item->proxy) {
        g_dbus_proxy_call(item->proxy, "Activate",
                          g_variant_new("(ii)", 0, 0),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
add_tray_item(TrayWidget *tray, const gchar *service)
{
    if (!service || !*service)
        return;

    /* Check if already added */
    for (guint i = 0; i < tray->items->len; i++) {
        TrayItem *it = g_ptr_array_index(tray->items, i);
        if (g_strcmp0(it->service, service) == 0)
            return;
    }

    TrayItem *item = g_new0(TrayItem, 1);
    item->service = g_strdup(service);

    /* Determine service name and object path */
    if (service[0] == '/') {
        item->object_path = g_strdup(service);
    } else if (strstr(service, "/") != NULL) {
        gchar **parts = g_strsplit(service, "/", 2);
        g_free(item->service);
        item->service = g_strdup(parts[0]);
        item->object_path = g_strconcat("/", parts[1], NULL);
        g_strfreev(parts);
    } else {
        item->object_path = g_strdup("/StatusNotifierItem");
    }

    GError *error = NULL;
    item->proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
        item->service, item->object_path, SNI_INTERFACE, NULL, &error);

    if (error != NULL) {
        g_clear_error(&error);
        g_free(item->service);
        g_free(item->object_path);
        g_free(item);
        return;
    }

    /* Build UI Button */
    item->button = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(item->button), FALSE);
    gtk_widget_add_css_class(item->button, "shell-widget");
    gtk_widget_add_css_class(item->button, "tray-item-button");

    item->icon_img = gtk_image_new_from_icon_name("application-x-executable-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(item->icon_img), 16);
    gtk_button_set_child(GTK_BUTTON(item->button), item->icon_img);

    g_signal_connect(item->button, "clicked", G_CALLBACK(on_tray_item_clicked), item);

    gtk_box_append(GTK_BOX(tray->box), item->button);
    g_ptr_array_add(tray->items, item);

    TrayPair *pair = g_new0(TrayPair, 1);
    pair->tray = tray;
    pair->item = item;
    item->signal_pair = pair;
    item->signal_id = g_signal_connect(item->proxy, "g-signal", G_CALLBACK(on_sni_signal), pair);

    if (tray->bus && item->service) {
        item->name_watcher_id = g_bus_watch_name_on_connection(
            tray->bus, item->service, G_BUS_NAME_WATCHER_FLAGS_NONE,
            NULL, on_tray_item_vanished, pair, g_free);
    } else {
        /* If no bus watcher created, free pair manually */
        g_free(pair);
        item->signal_pair = NULL;
    }

    refresh_tray_item(tray, item);
}

static void
on_tray_item_vanished(GDBusConnection *connection G_GNUC_UNUSED,
                      const gchar *name G_GNUC_UNUSED,
                      gpointer user_data)
{
    TrayPair *pair = user_data;
    if (pair && pair->tray && pair->item) {
        remove_tray_item(pair->tray, pair->item->service);
    }
}

static void
remove_tray_item(TrayWidget *tray, const gchar *service)
{
    if (!tray || !service)
        return;

    for (guint i = 0; i < tray->items->len; i++) {
        TrayItem *it = g_ptr_array_index(tray->items, i);
        if (g_strcmp0(it->service, service) == 0) {
            if (it->button) {
                gtk_box_remove(GTK_BOX(tray->box), it->button);
            }
            if (it->name_watcher_id > 0) {
                g_bus_unwatch_name(it->name_watcher_id);
                it->name_watcher_id = 0;
            }
            if (it->signal_id > 0 && it->proxy) {
                g_signal_handler_disconnect(it->proxy, it->signal_id);
                it->signal_id = 0;
            }
            if (it->proxy) {
                g_object_unref(it->proxy);
                it->proxy = NULL;
            }
            g_free(it->service);
            g_free(it->object_path);
            g_free(it->icon_name);
            g_free(it->title);
            g_free(it->menu_path);
            g_free(it);
            g_ptr_array_remove_index(tray->items, i);
            break;
        }
    }
}

static gboolean
decode_and_set_pixmap(GtkWidget *icon_img, GVariant *pixmap_var)
{
    if (!pixmap_var || !g_variant_is_of_type(pixmap_var, G_VARIANT_TYPE("a(iiay)")))
        return FALSE;

    GVariantIter iter;
    g_variant_iter_init(&iter, pixmap_var);

    gint best_w = 0, best_h = 0;
    GVariant *best_bytes_var = NULL;

    gint w, h;
    GVariant *bytes_var;
    while (g_variant_iter_next(&iter, "(ii@ay)", &w, &h, &bytes_var)) {
        if (w > 0 && h > 0 && w <= 256 && h <= 256 && w > best_w) {
            if (best_bytes_var) g_variant_unref(best_bytes_var);
            best_w = w;
            best_h = h;
            best_bytes_var = bytes_var;
        } else {
            g_variant_unref(bytes_var);
        }
    }

    if (!best_bytes_var || best_w <= 0 || best_h <= 0)
        return FALSE;

    gsize data_len = 0;
    const guchar *raw_data = (const guchar *)g_variant_get_fixed_array(best_bytes_var, &data_len, sizeof(guchar));
    if (!raw_data || data_len < (gsize)(best_w * best_h * 4)) {
        g_variant_unref(best_bytes_var);
        return FALSE;
    }

    guchar *rgba = g_malloc(best_w * best_h * 4);
    for (int i = 0; i < best_w * best_h; i++) {
        guint32 pixel = GUINT32_FROM_BE(((const guint32 *)raw_data)[i]);
        rgba[i * 4 + 0] = (pixel >> 16) & 0xFF; /* R */
        rgba[i * 4 + 1] = (pixel >> 8) & 0xFF;  /* G */
        rgba[i * 4 + 2] = (pixel & 0xFF);         /* B */
        rgba[i * 4 + 3] = (pixel >> 24) & 0xFF; /* A */
    }

    g_variant_unref(best_bytes_var);

    GBytes *gbytes = g_bytes_new_take(rgba, best_w * best_h * 4);
    GdkTexture *texture = gdk_memory_texture_new(best_w, best_h, GDK_MEMORY_R8G8B8A8, gbytes, best_w * 4);
    gtk_image_set_from_paintable(GTK_IMAGE(icon_img), GDK_PAINTABLE(texture));
    g_bytes_unref(gbytes);
    g_object_unref(texture);

    return TRUE;
}

static void
refresh_tray_item(TrayWidget *tray G_GNUC_UNUSED, TrayItem *item)
{
    if (!item || !item->proxy)
        return;

    /* Tooltip Title */
    GVariant *title_var = g_dbus_proxy_get_cached_property(item->proxy, "Title");
    if (title_var != NULL) {
        g_free(item->title);
        item->title = g_variant_dup_string(title_var, NULL);
        gtk_widget_set_tooltip_text(item->button, item->title);
        g_variant_unref(title_var);
    }

    /* Check IconThemePath */
    GVariant *theme_path_var = g_dbus_proxy_get_cached_property(item->proxy, "IconThemePath");
    if (theme_path_var != NULL) {
        const gchar *theme_path = g_variant_get_string(theme_path_var, NULL);
        if (theme_path && *theme_path && g_file_test(theme_path, G_FILE_TEST_IS_DIR)) {
            GdkDisplay *display = gdk_display_get_default();
            if (display) {
                GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
                gtk_icon_theme_add_search_path(icon_theme, theme_path);
            }
        }
        g_variant_unref(theme_path_var);
    }

    gboolean icon_resolved = FALSE;

    /* Tier 1 & 2: IconName */
    GVariant *icon_var = g_dbus_proxy_get_cached_property(item->proxy, "IconName");
    if (icon_var != NULL) {
        const gchar *icon_str = g_variant_get_string(icon_var, NULL);
        if (icon_str && *icon_str) {
            g_free(item->icon_name);
            item->icon_name = g_strdup(icon_str);

            /* Check if icon_str is an absolute file path */
            if (icon_str[0] == '/' && g_file_test(icon_str, G_FILE_TEST_EXISTS)) {
                GFile *file = g_file_new_for_path(icon_str);
                GIcon *gicon = g_file_icon_new(file);
                gtk_image_set_from_gicon(GTK_IMAGE(item->icon_img), gicon);
                g_object_unref(gicon);
                g_object_unref(file);
                icon_resolved = TRUE;
            } else {
                gtk_image_set_from_icon_name(GTK_IMAGE(item->icon_img), item->icon_name);
                icon_resolved = TRUE;
            }
        }
        g_variant_unref(icon_var);
    }

    /* Tier 3: IconPixmap ARGB32 bitmap decoding */
    if (!icon_resolved) {
        GVariant *pixmap_var = g_dbus_proxy_get_cached_property(item->proxy, "IconPixmap");
        if (pixmap_var != NULL) {
            icon_resolved = decode_and_set_pixmap(item->icon_img, pixmap_var);
            g_variant_unref(pixmap_var);
        }
    }

    /* Tier 4: Fallback Service / App-ID Icon Lookup */
    if (!icon_resolved && item->service) {
        gchar *lower_service = g_ascii_strdown(item->service, -1);
        if (strstr(lower_service, "discord")) {
            gtk_image_set_from_icon_name(GTK_IMAGE(item->icon_img), "discord");
        } else if (strstr(lower_service, "steam")) {
            gtk_image_set_from_icon_name(GTK_IMAGE(item->icon_img), "steam");
        } else if (strstr(lower_service, "slack")) {
            gtk_image_set_from_icon_name(GTK_IMAGE(item->icon_img), "slack");
        } else if (strstr(lower_service, "telegram")) {
            gtk_image_set_from_icon_name(GTK_IMAGE(item->icon_img), "telegram");
        } else if (strstr(lower_service, "obsidian")) {
            gtk_image_set_from_icon_name(GTK_IMAGE(item->icon_img), "obsidian");
        } else if (strstr(lower_service, "dropbox")) {
            gtk_image_set_from_icon_name(GTK_IMAGE(item->icon_img), "dropbox");
        } else {
            gtk_image_set_from_icon_name(GTK_IMAGE(item->icon_img), "application-x-executable-symbolic");
        }
        g_free(lower_service);
    }
}

static void
on_sni_signal(GDBusProxy *proxy G_GNUC_UNUSED,
              gchar *sender_name G_GNUC_UNUSED,
              gchar *signal_name,
              GVariant *parameters G_GNUC_UNUSED,
              gpointer user_data)
{
    TrayPair *pair = user_data;
    if (pair && pair->tray && pair->item) {
        if (g_strcmp0(signal_name, "NewIcon") == 0 ||
            g_strcmp0(signal_name, "NewAttentionIcon") == 0 ||
            g_strcmp0(signal_name, "NewStatus") == 0 ||
            g_strcmp0(signal_name, "NewTitle") == 0) {
            refresh_tray_item(pair->tray, pair->item);
        }
    }
}

static void
handle_watcher_method_call(GDBusConnection *connection,
                           const gchar *sender,
                           const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *method_name,
                           GVariant *parameters,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data)
{
    TrayWidget *tray = user_data;

    if (g_strcmp0(method_name, "RegisterStatusNotifierItem") == 0) {
        const gchar *service;
        g_variant_get(parameters, "(&s)", &service);

        gchar *full_service = NULL;
        if (service[0] == '/') {
            full_service = g_strconcat(sender, service, NULL);
        } else {
            full_service = g_strdup(service);
        }

        add_tray_item(tray, full_service);
        g_free(full_service);

        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (g_strcmp0(method_name, "RegisterStatusNotifierHost") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (g_strcmp0(method_name, "UnregisterStatusNotifierItem") == 0) {
        const gchar *service;
        g_variant_get(parameters, "(&s)", &service);
        remove_tray_item(tray, service);
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static GVariant *
handle_watcher_get_property(GDBusConnection *connection,
                            const gchar *sender,
                            const gchar *object_path,
                            const gchar *interface_name,
                            const gchar *property_name,
                            GError **error,
                            gpointer user_data)
{
    TrayWidget *tray = user_data;

    if (g_strcmp0(property_name, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
        for (guint i = 0; i < tray->items->len; i++) {
            TrayItem *it = g_ptr_array_index(tray->items, i);
            g_variant_builder_add(&builder, "s", it->service);
        }
        return g_variant_builder_end(&builder);
    } else if (g_strcmp0(property_name, "IsStatusNotifierHostRegistered") == 0) {
        return g_variant_new_boolean(TRUE);
    } else if (g_strcmp0(property_name, "ProtocolVersion") == 0) {
        return g_variant_new_int32(0);
    }
    return NULL;
}

static const GDBusInterfaceVTable watcher_vtable = {
    .method_call = handle_watcher_method_call,
    .get_property = handle_watcher_get_property,
    .set_property = NULL,
};

static void
on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    TrayWidget *tray = user_data;
    if (tray->bus != connection) {
        if (tray->bus) g_object_unref(tray->bus);
        tray->bus = g_object_ref(connection);
    }

    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(watcher_introspection_xml, NULL);
    if (node_info && node_info->interfaces) {
        tray->watcher_reg_id = g_dbus_connection_register_object(
            connection, SNW_OBJECT_PATH, node_info->interfaces[0],
            &watcher_vtable, tray, NULL, NULL);
    }
    if (node_info) g_dbus_node_info_unref(node_info);
}

/* ─── ShellWidget Interface ─── */

static ShellWidget *
tray_widget_create(ShellCompositor *compositor)
{
    TrayWidget *tray = g_new0(TrayWidget, 1);
    tray->base.klass = &tray_widget_class;
    tray->compositor = compositor;
    tray->items = g_ptr_array_new();

    tray->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(tray->box, "shell-widget");
    gtk_widget_add_css_class(tray->box, "shell-widget-tray");

    /* Register D-Bus StatusNotifierWatcher service */
    tray->watcher_owner_id = g_bus_own_name(
        G_BUS_TYPE_SESSION, SNW_BUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired, NULL, NULL, tray, NULL);

    return (ShellWidget *)tray;
}

static void
tray_widget_destroy(ShellWidget *widget)
{
    TrayWidget *tray = (TrayWidget *)widget;
    if (tray == NULL)
        return;

    if (tray->watcher_owner_id != 0) {
        g_bus_unown_name(tray->watcher_owner_id);
        tray->watcher_owner_id = 0;
    }

    if (tray->watcher_reg_id > 0 && tray->bus) {
        g_dbus_connection_unregister_object(tray->bus, tray->watcher_reg_id);
        tray->watcher_reg_id = 0;
    }

    if (tray->bus) {
        g_object_unref(tray->bus);
        tray->bus = NULL;
    }

    if (tray->items) {
        for (guint i = 0; i < tray->items->len; i++) {
            TrayItem *it = g_ptr_array_index(tray->items, i);
            if (it->signal_id > 0 && it->proxy) {
                g_signal_handler_disconnect(it->proxy, it->signal_id);
                it->signal_id = 0;
            }
            if (it->proxy) g_object_unref(it->proxy);
            g_free(it->signal_pair);
            g_free(it->service);
            g_free(it->object_path);
            g_free(it->icon_name);
            g_free(it->title);
            g_free(it->menu_path);
            g_free(it);
        }
        g_ptr_array_free(tray->items, TRUE);
    }

    g_free(tray);
}

static GtkWidget *
tray_widget_get_widget(ShellWidget *widget)
{
    TrayWidget *tray = (TrayWidget *)widget;
    return tray->box;
}

static void
tray_widget_enable(ShellWidget *widget)
{
    (void)widget;
}

static void
tray_widget_disable(ShellWidget *widget)
{
    (void)widget;
}

const ShellWidgetClass tray_widget_class = {
    .id = "tray",
    .name = "System Tray (StatusNotifier)",
    .create = tray_widget_create,
    .destroy = tray_widget_destroy,
    .get_widget = tray_widget_get_widget,
    .enable = tray_widget_enable,
    .disable = tray_widget_disable,
};
