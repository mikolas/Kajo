#include "shell.h"
#include "config.h"
#include "theme.h"
#include "icons.h"
#include "panel.h"
#include "widgets/registry.h"
#include "compositor/compositor.h"
#include "osd/osd.h"
#include "notifications/daemon.h"
#include "launcher/launcher_surface.h"
#include "ipc/socket.h"

static void
shell_app_instantiate_widgets(ShellApp *self, GPtrArray *configs, PanelWidgetPosition position)
{
    if (!configs)
        return;

    for (guint i = 0; i < configs->len; i++) {
        ShellWidgetConfig *wc = g_ptr_array_index(configs, i);
        const ShellWidgetClass *klass = shell_widget_registry_find(self->registry, wc->id);
        if (!klass)
            continue;

        ShellWidget *widget = klass->create(self->compositor);
        if (!widget)
            continue;

        widget->klass = klass;
        widget->app   = self;
        widget->mode  = wc->mode;
        klass->enable(widget);

        GtkWidget *gtk_widget = klass->get_widget(widget);
        if (gtk_widget)
            shell_panel_add_widget(self->panel, position, gtk_widget);

        g_ptr_array_add(self->active_widgets, widget);
    }
}

ShellApp *
shell_app_new(GtkApplication *app)
{
    ShellApp *self = g_new0(ShellApp, 1);

    self->gtk_app        = app;
    self->panel          = NULL;
    self->registry       = NULL;
    self->compositor     = NULL;
    self->osd            = NULL;
    self->notifications  = NULL;
    self->active_widgets = NULL;

    /* Load configuration */
    self->config = shell_config_load(NULL);

    /* Load theme */
    self->theme = shell_theme_new(self->config->css_path);

    return self;
}

void
shell_app_activate(ShellApp *self)
{
    if (!self)
        return;

    /* Apply theme to default display */
    GdkDisplay *display = gdk_display_get_default();
    if (display)
        shell_theme_apply(self->theme, display);

    /* Initialize icon system (add bundled symbolic icons to search path) */
    shell_icons_init(self->config->icon_map_path);

    /* Create panel */
    self->panel = shell_panel_new(self);

    /* Create OSD and Notification Daemon */
    self->osd           = shell_osd_new();
    self->notifications = shell_notification_daemon_new();
    shell_notification_daemon_set_app(self->notifications, self);
    shell_notification_daemon_set_toast_timeout(self->notifications, self->config->toast_timeout_ms);

    /* Create widget registry and compositor */
    self->registry       = shell_widget_registry_new();
    self->compositor     = shell_compositor_new();
    self->active_widgets = g_ptr_array_new();

    /* Create Launcher Surface & IPC Socket Listener */
    self->launcher = shell_launcher_surface_new(self->compositor);
    self->ipc      = shell_ipc_socket_new(self);

    /* Instantiate widgets from config layout */
    shell_app_instantiate_widgets(self, self->config->layout_left, PANEL_WIDGET_LEFT);
    shell_app_instantiate_widgets(self, self->config->layout_center, PANEL_WIDGET_CENTER);
    shell_app_instantiate_widgets(self, self->config->layout_right, PANEL_WIDGET_RIGHT);

    /* Set the panel window as application window */
    gtk_application_add_window(self->gtk_app, shell_panel_get_window(self->panel));

    /* Show the panel */
    shell_panel_show(self->panel);
}

void
shell_app_destroy(ShellApp *self)
{
    if (!self)
        return;

    /* Disable and destroy all active widgets */
    if (self->active_widgets) {
        for (guint i = 0; i < self->active_widgets->len; i++) {
            ShellWidget *widget = g_ptr_array_index(self->active_widgets, i);
            widget->klass->disable(widget);
            widget->klass->destroy(widget);
        }
        g_ptr_array_free(self->active_widgets, TRUE);
        self->active_widgets = NULL;
    }

    /* Destroy compositor */
    if (self->compositor) {
        shell_compositor_destroy(self->compositor);
        self->compositor = NULL;
    }

    /* Destroy registry */
    if (self->registry) {
        shell_widget_registry_destroy(self->registry);
        self->registry = NULL;
    }

    /* Destroy OSD & Notifications & Launcher */
    if (self->osd) {
        shell_osd_destroy(self->osd);
        self->osd = NULL;
    }
    if (self->notifications) {
        shell_notification_daemon_destroy(self->notifications);
        self->notifications = NULL;
    }
    if (self->launcher) {
        shell_launcher_surface_destroy(self->launcher);
        self->launcher = NULL;
    }
    if (self->ipc) {
        shell_ipc_socket_destroy(self->ipc);
        self->ipc = NULL;
    }

    shell_panel_destroy(self->panel);
    self->panel = NULL;

    shell_theme_destroy(self->theme);
    self->theme = NULL;

    shell_config_destroy(self->config);
    self->config = NULL;

    g_free(self);
}

ShellConfig *
shell_app_get_config(ShellApp *self)
{
    return self ? self->config : NULL;
}

ShellNotificationDaemon *
shell_app_get_notification_daemon(ShellApp *self)
{
    return self ? self->notifications : NULL;
}

ShellPanel *
shell_app_get_panel(ShellApp *self)
{
    return self ? self->panel : NULL;
}

ShellCompositor *
shell_app_get_compositor(ShellApp *self)
{
    return self ? self->compositor : NULL;
}

ShellOSD *
shell_app_get_osd(ShellApp *self)
{
    return self ? self->osd : NULL;
}

static gchar *
get_active_session_path(GDBusConnection *bus)
{
    GError *error = NULL;
    GVariant *res = g_dbus_connection_call_sync(
        bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", "org.freedesktop.login1.Seat", "ActiveSession"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        1000,
        NULL,
        &error);

    if (!res) {
        if (error) g_clear_error(&error);
        return g_strdup("/org/freedesktop/login1/session/auto");
    }

    GVariant *v = NULL;
    g_variant_get(res, "(v)", &v);
    g_variant_unref(res);

    if (!v)
        return g_strdup("/org/freedesktop/login1/session/auto");

    const gchar *session_id = NULL;
    const gchar *session_path = NULL;
    g_variant_get(v, "(&s&o)", &session_id, &session_path);

    gchar *path = g_strdup(session_path);
    g_variant_unref(v);

    if (path && *path)
        return path;
    g_free(path);
    return g_strdup("/org/freedesktop/login1/session/auto");
}

void
shell_brightness_change(ShellApp *self, gint delta_percent)
{
    if (!self) return;

    GDir *dir = g_dir_open("/sys/class/backlight", 0, NULL);
    if (!dir) return;

    const gchar *raw_dev_name = g_dir_read_name(dir);
    if (!raw_dev_name) {
        g_dir_close(dir);
        return;
    }
    gchar *device_name = g_strdup(raw_dev_name);
    g_dir_close(dir);

    gchar *backlight_dir = g_build_filename("/sys/class/backlight", device_name, NULL);
    gchar *cur_path = g_build_filename(backlight_dir, "actual_brightness", NULL);
    gchar *max_path = g_build_filename(backlight_dir, "max_brightness", NULL);

    gchar *cur_str = NULL, *max_str = NULL;
    g_file_get_contents(cur_path, &cur_str, NULL, NULL);
    g_file_get_contents(max_path, &max_str, NULL, NULL);

    g_free(backlight_dir);
    g_free(cur_path);
    g_free(max_path);

    if (!cur_str || !max_str) {
        g_free(cur_str);
        g_free(max_str);
        g_free(device_name);
        return;
    }

    gint cur_val = atoi(cur_str);
    gint max_val = atoi(max_str);
    g_free(cur_str);
    g_free(max_str);

    if (max_val <= 0) {
        g_free(device_name);
        return;
    }

    gint step_pct = ABS(delta_percent);
    if (step_pct < 1) step_pct = 5;
    gint step_val = (step_pct * max_val) / 100;
    if (step_val < 1) step_val = 1;

    gint new_val = (delta_percent > 0) ? (cur_val + step_val) : (cur_val - step_val);
    if (new_val < 1) new_val = 1;
    if (new_val > max_val) new_val = max_val;

    gint new_pct = (new_val * 100) / max_val;

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus) {
        gchar *session_path = get_active_session_path(bus);
        g_dbus_connection_call(
            bus,
            "org.freedesktop.login1",
            session_path,
            "org.freedesktop.login1.Session",
            "SetBrightness",
            g_variant_new("(ssu)", "backlight", device_name, (guint32)new_val),
            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        g_free(session_path);
        g_object_unref(bus);
    }
    if (error) g_clear_error(&error);
    g_free(device_name);

    if (self->osd) {
        shell_osd_show_brightness(self->osd, new_pct);
    }
}

void
shell_volume_change(ShellApp *self, gint delta_percent)
{
    if (!self || !self->active_widgets) return;
    for (guint i = 0; i < self->active_widgets->len; i++) {
        ShellWidget *w = g_ptr_array_index(self->active_widgets, i);
        if (w && w->klass && g_strcmp0(w->klass->id, "volume") == 0) {
            extern void volume_widget_change_volume(ShellWidget *widget, gint delta_percent);
            volume_widget_change_volume(w, delta_percent);
            break;
        }
    }
}

void
shell_volume_toggle_mute(ShellApp *self)
{
    if (!self || !self->active_widgets) return;
    for (guint i = 0; i < self->active_widgets->len; i++) {
        ShellWidget *w = g_ptr_array_index(self->active_widgets, i);
        if (w && w->klass && g_strcmp0(w->klass->id, "volume") == 0) {
            extern void volume_widget_toggle_mute(ShellWidget *widget);
            volume_widget_toggle_mute(w);
            break;
        }
    }
}

static void
update_reused_widget_mode_visibility(ShellWidget *w)
{
    if (!w || !w->klass) return;
    if (g_strcmp0(w->klass->id, "notifications") == 0 ||
        g_strcmp0(w->klass->id, "workspaces") == 0 ||
        g_strcmp0(w->klass->id, "tray") == 0) {
        return;
    }

    GtkWidget *root = w->klass->get_widget(w);
    if (!root) return;

    GtkWidget *content = NULL;
    if (GTK_IS_MENU_BUTTON(root)) {
        content = gtk_menu_button_get_child(GTK_MENU_BUTTON(root));
    } else if (GTK_IS_BUTTON(root)) {
        content = gtk_button_get_child(GTK_BUTTON(root));
    }

    if (!content) return;

    if (GTK_IS_LABEL(content)) {
        shell_widget_apply_mode_visibility(w->mode, NULL, content);
    } else if (GTK_IS_IMAGE(content)) {
        shell_widget_apply_mode_visibility(w->mode, content, NULL);
    } else if (GTK_IS_BOX(content)) {
        GtkWidget *icon_img = NULL;
        GtkWidget *label = NULL;
        for (GtkWidget *child = gtk_widget_get_first_child(content);
             child != NULL;
             child = gtk_widget_get_next_sibling(child)) {
            if (!icon_img && GTK_IS_IMAGE(child)) icon_img = child;
            else if (!label && GTK_IS_LABEL(child)) label = child;
        }
        shell_widget_apply_mode_visibility(w->mode, icon_img, label);
    }
}

static void
reconcile_section_widgets(ShellApp            *self,
                          GPtrArray           *configs,
                          PanelWidgetPosition  position,
                          GPtrArray           *old_widgets,
                          GPtrArray           *new_active)
{
    if (!configs) return;

    for (guint i = 0; i < configs->len; i++) {
        ShellWidgetConfig *wc = g_ptr_array_index(configs, i);
        if (!wc || !wc->id) continue;

        ShellWidget *reused = NULL;
        for (guint j = 0; j < old_widgets->len; j++) {
            ShellWidget *cand = g_ptr_array_index(old_widgets, j);
            if (cand && cand->klass && g_strcmp0(cand->klass->id, wc->id) == 0) {
                reused = cand;
                g_ptr_array_remove_index(old_widgets, j);
                break;
            }
        }

        if (reused) {
            reused->mode = wc->mode;
            update_reused_widget_mode_visibility(reused);
            GtkWidget *gtk_widget = reused->klass->get_widget(reused);
            if (gtk_widget) {
                shell_panel_add_widget(self->panel, position, gtk_widget);
            }
            g_ptr_array_add(new_active, reused);
        } else {
            const ShellWidgetClass *klass = shell_widget_registry_find(self->registry, wc->id);
            if (!klass) continue;

            ShellWidget *widget = klass->create(self->compositor);
            if (!widget) continue;

            widget->klass = klass;
            widget->app   = self;
            widget->mode  = wc->mode;
            klass->enable(widget);

            GtkWidget *gtk_widget = klass->get_widget(widget);
            if (gtk_widget) {
                shell_panel_add_widget(self->panel, position, gtk_widget);
            }
            g_ptr_array_add(new_active, widget);
        }
    }
}

void
shell_app_reload_config(ShellApp *self)
{
    if (!self || !self->panel) return;

    ShellConfig *new_config = shell_config_load(NULL);
    if (!new_config) return;

    ShellConfig *old_config = self->config;
    self->config = new_config;
    shell_config_destroy(old_config);

    shell_panel_apply_config(self->panel, self->config);

    if (self->notifications) {
        shell_notification_daemon_set_toast_timeout(self->notifications, self->config->toast_timeout_ms);
    }

    /* Hold references to all current GTK widget roots while detaching from panel boxes */
    GPtrArray *held_gtk_widgets = g_ptr_array_new_with_free_func(g_object_unref);
    if (self->active_widgets) {
        for (guint i = 0; i < self->active_widgets->len; i++) {
            ShellWidget *w = g_ptr_array_index(self->active_widgets, i);
            GtkWidget *gw = (w && w->klass) ? w->klass->get_widget(w) : NULL;
            if (gw) {
                g_ptr_array_add(held_gtk_widgets, g_object_ref(gw));
                GtkWidget *parent = gtk_widget_get_parent(gw);
                if (parent && GTK_IS_BOX(parent)) {
                    gtk_box_remove(GTK_BOX(parent), gw);
                }
            }
        }
    }

    if (self->panel->widgets) {
        g_ptr_array_set_size(self->panel->widgets, 0);
    }

    GPtrArray *old_widgets = self->active_widgets ? self->active_widgets : g_ptr_array_new();
    GPtrArray *new_active = g_ptr_array_new();

    reconcile_section_widgets(self, self->config->layout_left, PANEL_WIDGET_LEFT, old_widgets, new_active);
    reconcile_section_widgets(self, self->config->layout_center, PANEL_WIDGET_CENTER, old_widgets, new_active);
    reconcile_section_widgets(self, self->config->layout_right, PANEL_WIDGET_RIGHT, old_widgets, new_active);

    /* Destroy any widgets no longer present in the new layout */
    for (guint i = 0; i < old_widgets->len; i++) {
        ShellWidget *w = g_ptr_array_index(old_widgets, i);
        if (w && w->klass) {
            w->klass->disable(w);
            w->klass->destroy(w);
        }
    }
    g_ptr_array_free(old_widgets, TRUE);
    g_ptr_array_unref(held_gtk_widgets);

    self->active_widgets = new_active;
    g_message("Reloaded shell configuration and reconciled %u active panel widgets", new_active->len);
}

void
shell_app_reload_theme(ShellApp *self)
{
    if (!self || !self->theme) return;
    shell_theme_reload(self->theme);
}
