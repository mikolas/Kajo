#include "../settings_common.h"
#include "../settings_pages.h"
#include <gio/gio.h>
#include <string.h>

typedef struct _SettingsPageNotifications {
    GtkBox parent_instance;

    GtkWidget    *card_toast;
    GtkWidget    *tile_dnd;
    GtkWidget    *scale_tout;
    GtkWidget    *seg_click;

    GtkWidget    *card_hist;
    GtkWidget    *scale_hist;

    GCancellable *cancellable;
    gboolean      syncing_ui;
} SettingsPageNotifications;

typedef struct _SettingsPageNotificationsClass {
    GtkBoxClass parent_class;
} SettingsPageNotificationsClass;

G_DEFINE_TYPE(SettingsPageNotifications, settings_page_notifications, GTK_TYPE_BOX)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SettingsPageNotifications, g_object_unref)

static void
update_notifications_badges_and_telemetry(SettingsPageNotifications *self)
{
    if (!self)
        return;

    gboolean dnd_on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tile_dnd));
    int tout_ms = (int)gtk_range_get_value(GTK_RANGE(self->scale_tout));
    int hist_max = (int)gtk_range_get_value(GTK_RANGE(self->scale_hist));
    gboolean is_dismiss = (segmented_group_get_selected(self->seg_click) == 1);

    toggle_card_set_subtitle(
        self->tile_dnd,
        dnd_on
            ? "Enabled — non-critical popups muted, still logged to history"
            : "Disabled — toast banners shown on top-right overlay");

    if (dnd_on) {
        section_card_set_badge(self->card_toast, "DND MUTED", "peak-warn");
    } else {
        g_autofree gchar *tb = g_strdup_printf("%dMS", tout_ms);
        section_card_set_badge(self->card_toast, tb, "accent");
    }

    g_autofree gchar *hb = g_strdup_printf("%d MAX", hist_max);
    section_card_set_badge(self->card_hist, hb, NULL);

    g_autofree gchar *c1 = g_strdup_printf("%dMS (%dS)", tout_ms, MAX(1, tout_ms / 1000));
    g_autofree gchar *c2 = g_strdup_printf("%d ITEMS", hist_max);

    settings_page_set_counter(
        GTK_WIDGET(self), 0, "DND", dnd_on ? "ACTIVE" : "OFF", dnd_on ? "warn" : "accent");
    settings_page_set_counter(GTK_WIDGET(self), 1, "TIMEOUT", c1, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 2, "HISTORY", c2, NULL);
    settings_page_set_counter(
        GTK_WIDGET(self), 3, "CLICK", is_dismiss ? "DISMISS ONLY" : "FOCUS WINDOW", NULL);

    if (dnd_on) {
        settings_page_set_nav_badge(GTK_WIDGET(self), "notifications", "DND", "peak-warn");
    } else {
        g_autofree gchar *nb = g_strdup_printf("%dS", MAX(1, tout_ms / 1000));
        settings_page_set_nav_badge(GTK_WIDGET(self), "notifications", nb, NULL);
    }
}

static void
load_notifications_from_config(SettingsPageNotifications *self)
{
    if (!self)
        return;

    ShellConfig *config = shell_config_load(NULL);
    if (!config)
        config = shell_config_get_default();

    self->syncing_ui = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->tile_dnd), config->dnd_enabled);
    gtk_range_set_value(
        GTK_RANGE(self->scale_tout),
        config->toast_timeout_ms > 0 ? config->toast_timeout_ms : 8000);
    segmented_group_set_selected(
        self->seg_click,
        (config->toast_click_action && g_strcmp0(config->toast_click_action, "dismiss") == 0) ? 1 : 0);
    gtk_range_set_value(
        GTK_RANGE(self->scale_hist),
        config->max_notification_history > 0 ? config->max_notification_history : 50);
    self->syncing_ui = FALSE;

    shell_config_destroy(config);

    update_notifications_badges_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);
}

static void
mark_notifications_changed(SettingsPageNotifications *self)
{
    if (!self || self->syncing_ui)
        return;
    update_notifications_badges_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), TRUE);
}

static void
on_notif_dnd_toggled(GtkToggleButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    mark_notifications_changed((SettingsPageNotifications *)user_data);
}

static void
on_notif_range_changed(GtkRange *range G_GNUC_UNUSED, gpointer user_data)
{
    mark_notifications_changed((SettingsPageNotifications *)user_data);
}

static void
on_notif_click_seg_changed(GtkWidget *group G_GNUC_UNUSED, guint selected G_GNUC_UNUSED, gpointer user_data)
{
    mark_notifications_changed((SettingsPageNotifications *)user_data);
}

static void
on_test_notify_reply(GObject *source, GAsyncResult *res, gpointer user_data G_GNUC_UNUSED)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
}

static void
on_session_bus_for_test_toast(GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *weak = (GWeakRef *)user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) conn = g_bus_get_finish(res, &err);
    g_autoptr(SettingsPageNotifications) self = g_weak_ref_get(weak);
    g_weak_ref_clear(weak);
    g_free(weak);

    if (!conn || !self || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    GVariantBuilder actions;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));

    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("org.kajo.Settings"));

    gint32 timeout_ms = (gint32)gtk_range_get_value(GTK_RANGE(self->scale_tout));

    g_dbus_connection_call(
        conn,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify",
        g_variant_new("(susssasa{sv}i)",
                      "Kajo Settings",
                      (guint32)0,
                      "preferences-system-notifications-symbolic",
                      "Notification Daemon Test",
                      "Toast timeout and click routing verified.",
                      &actions,
                      &hints,
                      timeout_ms),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        3000,
        self->cancellable,
        on_test_notify_reply,
        NULL);
}

static void
action_test_toast(gpointer user_data)
{
    SettingsPageNotifications *self = (SettingsPageNotifications *)user_data;
    if (!self)
        return;

    GWeakRef *weak = g_new0(GWeakRef, 1);
    g_weak_ref_init(weak, self);
    g_bus_get(G_BUS_TYPE_SESSION, self->cancellable, on_session_bus_for_test_toast, weak);
}

static void
action_revert_notifications(gpointer user_data)
{
    load_notifications_from_config((SettingsPageNotifications *)user_data);
}

static void
action_save_notifications(gpointer user_data)
{
    SettingsPageNotifications *self = (SettingsPageNotifications *)user_data;
    if (!self)
        return;

    ShellConfig *config = shell_config_load(NULL);
    if (!config)
        config = shell_config_get_default();

    config->dnd_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->tile_dnd));
    config->toast_timeout_ms = (gint)gtk_range_get_value(GTK_RANGE(self->scale_tout));
    config->max_notification_history = (gint)gtk_range_get_value(GTK_RANGE(self->scale_hist));

    const gchar *click_act = (segmented_group_get_selected(self->seg_click) == 1) ? "dismiss" : "focus";
    g_free(config->toast_click_action);
    config->toast_click_action = g_strdup(click_act);

    g_autofree gchar *dir = g_build_filename(g_get_user_config_dir(), "kajo", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_autofree gchar *path = g_build_filename(dir, "config.json", NULL);

    if (shell_config_save(config, path)) {
        settings_send_ipc_command("reload-config");
    }
    shell_config_destroy(config);

    update_notifications_badges_and_telemetry(self);
    settings_page_set_dirty(GTK_WIDGET(self), FALSE);
}

static void
on_notif_page_mapped(GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    update_notifications_badges_and_telemetry((SettingsPageNotifications *)widget);
}

static void
settings_page_notifications_dispose(GObject *object)
{
    SettingsPageNotifications *self = (SettingsPageNotifications *)object;
    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
    G_OBJECT_CLASS(settings_page_notifications_parent_class)->dispose(object);
}

static void
settings_page_notifications_init(SettingsPageNotifications *self)
{
    self->cancellable = g_cancellable_new();
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));
}

static void
settings_page_notifications_class_init(SettingsPageNotificationsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = settings_page_notifications_dispose;
}

GtkWidget *
build_page_notifications(void)
{
    SettingsPageNotifications *self = g_object_new(settings_page_notifications_get_type(), NULL);

    /* ─── Card 1: Freedesktop Toast Popup Behavior ─── */
    self->card_toast = create_section_card(
        SETTINGS_CARD_LEFT,
        "1. FREEDESKTOP TOAST POPUP BEHAVIOR",
        "8000MS",
        "accent",
        NULL);

    self->tile_dnd = create_toggle_card(
        "🔕",
        "Do Not Disturb (Suppress Popups)",
        "Disabled — toast banners shown on top-right overlay",
        FALSE);
    gtk_box_append(GTK_BOX(self->card_toast), self->tile_dnd);

    GtkWidget *w_tout = create_scale_with_badge(
        3000, 30000, 1000, 8000, "%.0fms", &self->scale_tout, NULL);
    gtk_box_append(GTK_BOX(self->card_toast), create_boxed_row(
        "🔔",
        "Toast Display Timeout",
        "Auto-dismiss duration for normal urgency popups",
        w_tout));

    static const char *const click_items[] = { "FOCUS WINDOW", "DISMISS ONLY", NULL };
    self->seg_click = create_segmented_group(click_items, 0);
    gtk_box_append(GTK_BOX(self->card_toast), create_boxed_row(
        "🖱",
        "Toast Click Action",
        "Focus source window via Niri or dismiss banner only",
        self->seg_click));

    /* ─── Card 2: Notification Center & Ring Buffer ─── */
    self->card_hist = create_section_card(
        SETTINGS_CARD_CENTER,
        "2. NOTIFICATION CENTER & RING BUFFER",
        "50 MAX",
        NULL,
        NULL);

    GtkWidget *w_hist = create_scale_with_badge(
        20, 200, 10, 50, "%.0f items", &self->scale_hist, NULL);
    gtk_box_append(GTK_BOX(self->card_hist), create_boxed_row(
        "🗂",
        "Maximum History Items Retained",
        "Ring buffer capacity in Notification Center popover",
        w_hist));

    GtkWidget *spec_badge = gtk_label_new("SPEC 1.2");
    gtk_widget_add_css_class(spec_badge, "shell-popover-badge");
    gtk_widget_set_valign(spec_badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(self->card_hist), create_boxed_row(
        "⚡",
        "Critical Urgency Bypass",
        "Urgency=2 alerts bypass Do Not Disturb per Freedesktop spec",
        spec_badge));

    settings_bento_add_pair(GTK_WIDGET(self), self->card_toast, self->card_hist);

    load_notifications_from_config(self);

    g_signal_connect(self->tile_dnd, "toggled", G_CALLBACK(on_notif_dnd_toggled), self);
    g_signal_connect(self->scale_tout, "value-changed", G_CALLBACK(on_notif_range_changed), self);
    segmented_group_connect(self->seg_click, on_notif_click_seg_changed, self);
    g_signal_connect(self->scale_hist, "value-changed", G_CALLBACK(on_notif_range_changed), self);
    g_signal_connect(self, "map", G_CALLBACK(on_notif_page_mapped), NULL);

    settings_page_bind_actions(
        GTK_WIDGET(self),
        "🔔 TEST TOAST",
        action_test_toast,
        action_revert_notifications,
        action_save_notifications,
        self);

    return GTK_WIDGET(self);
}
