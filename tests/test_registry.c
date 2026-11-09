#include "../src/widgets/registry.h"
#include "../src/shell.h"
#include "../src/panel.h"
#include "../src/autohide.h"
#include "../src/config.h"
#include "../src/osd/osd.h"
#include "../src/ipc/socket.h"
#include "../src/notifications/daemon.h"
#include "../src/launcher/launcher_surface.h"
#include "../src/lock/idle_manager.h"
#include "../src/posture/posture_manager.h"
#include "../src/osk/osk.h"
#include <glib.h>

void
shell_osk_toggle(ShellOSK *self G_GNUC_UNUSED)
{
}

/* Stubs for shell_app_* entry points referenced by widget callbacks */
ShellConfig *
shell_app_get_config(ShellApp *self)
{
    return self ? self->config : NULL;
}

ShellCompositor *
shell_app_get_compositor(ShellApp *self G_GNUC_UNUSED)
{
    return NULL;
}

ShellOSD *
shell_app_get_osd(ShellApp *self G_GNUC_UNUSED)
{
    return NULL;
}

void
shell_osd_show_volume(ShellOSD *osd G_GNUC_UNUSED, gint percent G_GNUC_UNUSED, gboolean muted G_GNUC_UNUSED)
{
}

gboolean
shell_ipc_send_command(const gchar *command G_GNUC_UNUSED)
{
    return FALSE;
}

ShellPanel *
shell_app_get_panel(ShellApp *self G_GNUC_UNUSED)
{
    return NULL;
}

ShellAutohide *
shell_panel_get_autohide(ShellPanel *panel G_GNUC_UNUSED)
{
    return NULL;
}

void
shell_autohide_lock(ShellAutohide *self G_GNUC_UNUSED)
{
}

void
shell_autohide_unlock(ShellAutohide *self G_GNUC_UNUSED)
{
}

ShellNotificationDaemon *
shell_app_get_notification_daemon(ShellApp *self G_GNUC_UNUSED)
{
    return NULL;
}

void
shell_app_lock_session(ShellApp *self G_GNUC_UNUSED)
{
}

void
shell_launcher_surface_toggle(ShellLauncherSurface *self G_GNUC_UNUSED)
{
}

void
shell_notification_daemon_clear_history(ShellNotificationDaemon *daemon G_GNUC_UNUSED)
{
}

void
shell_notification_daemon_remove_item(ShellNotificationDaemon *daemon G_GNUC_UNUSED, guint32 id G_GNUC_UNUSED)
{
}

void
shell_notification_daemon_set_dnd(ShellNotificationDaemon *daemon G_GNUC_UNUSED, gboolean dnd G_GNUC_UNUSED)
{
}

gboolean
shell_notification_daemon_get_dnd(ShellNotificationDaemon *daemon G_GNUC_UNUSED)
{
    return FALSE;
}

void
shell_notification_daemon_on_history_changed(ShellNotificationDaemon    *daemon G_GNUC_UNUSED,
                                             NotificationHistoryCallback cb G_GNUC_UNUSED,
                                             gpointer                    user_data G_GNUC_UNUSED)
{
}

void
shell_notification_daemon_remove_callbacks(ShellNotificationDaemon *daemon G_GNUC_UNUSED,
                                           gpointer                 user_data G_GNUC_UNUSED)
{
}

const GPtrArray *
shell_notification_daemon_get_history(ShellNotificationDaemon *daemon G_GNUC_UNUSED)
{
    return NULL;
}

void
shell_notification_daemon_emit_action_invoked(ShellNotificationDaemon *daemon G_GNUC_UNUSED,
                                              guint32                  id G_GNUC_UNUSED,
                                              const gchar             *action_key G_GNUC_UNUSED)
{
}

void
shell_notification_daemon_emit_notification_closed(ShellNotificationDaemon *daemon G_GNUC_UNUSED,
                                                   guint32                  id G_GNUC_UNUSED,
                                                   guint32                  reason G_GNUC_UNUSED)
{
}

void
shell_idle_manager_set_inhibited(ShellIdleManager *self G_GNUC_UNUSED, gboolean inhibited G_GNUC_UNUSED)
{
}

gboolean
shell_idle_manager_get_inhibited(ShellIdleManager *self G_GNUC_UNUSED)
{
    return FALSE;
}

void
shell_idle_manager_add_listener(ShellIdleManager           *self G_GNUC_UNUSED,
                                ShellIdleInhibitChangedFunc callback G_GNUC_UNUSED,
                                gpointer                    user_data G_GNUC_UNUSED)
{
}

void
shell_idle_manager_remove_listener(ShellIdleManager           *self G_GNUC_UNUSED,
                                   ShellIdleInhibitChangedFunc callback G_GNUC_UNUSED,
                                   gpointer                    user_data G_GNUC_UNUSED)
{
}

void
shell_brightness_change(ShellApp *self G_GNUC_UNUSED, gint delta_percent G_GNUC_UNUSED)
{
}

void
shell_posture_manager_reload_config(ShellPostureManager *self G_GNUC_UNUSED)
{
}

static void
test_widget_registry_integrity(void)
{
    ShellWidgetRegistry *reg = shell_widget_registry_new();
    g_assert_nonnull(reg);
    g_assert_nonnull(reg->classes);
    g_assert_cmpuint(reg->classes->len, >=, 20);

    /* Rule #7: Every registered ShellWidgetClass MUST define non-NULL .id, .name, and vtable methods */
    for (guint i = 0; i < reg->classes->len; i++) {
        const ShellWidgetClass *cls = g_ptr_array_index(reg->classes, i);
        g_assert_nonnull(cls);
        g_assert_nonnull(cls->id);
        g_assert_cmpstr(cls->id, !=, "");
        g_assert_nonnull(cls->name);
        g_assert_cmpstr(cls->name, !=, "");
        g_assert_nonnull(cls->create);
        g_assert_nonnull(cls->destroy);
        g_assert_nonnull(cls->get_widget);
        g_assert_nonnull(cls->enable);
        g_assert_nonnull(cls->disable);

        /* Verify lookup by id returns the exact class */
        const ShellWidgetClass *found = shell_widget_registry_find(reg, cls->id);
        g_assert_true(found == cls);
    }

    /* Verify key built-in widgets are registered */
    g_assert_nonnull(shell_widget_registry_find(reg, "launcher"));
    g_assert_nonnull(shell_widget_registry_find(reg, "control-center"));
    g_assert_nonnull(shell_widget_registry_find(reg, "idle-inhibitor"));
    g_assert_nonnull(shell_widget_registry_find(reg, "power"));
    g_assert_nonnull(shell_widget_registry_find(reg, "clock"));
    g_assert_nonnull(shell_widget_registry_find(reg, "network"));
    g_assert_nonnull(shell_widget_registry_find(reg, "bluetooth"));
    g_assert_nonnull(shell_widget_registry_find(reg, "volume"));
    g_assert_nonnull(shell_widget_registry_find(reg, "backlight"));
    g_assert_null(shell_widget_registry_find(reg, "nonexistent-widget-id"));

    shell_widget_registry_destroy(reg);
}

static void
on_css_parse_error(GtkCssProvider *provider G_GNUC_UNUSED,
                   GtkCssSection  *section,
                   const GError   *error,
                   gpointer        user_data)
{
    guint *err_count = user_data;
    (*err_count)++;
    g_printerr("CSS parse error at line %u: %s\n",
               section ? (guint)gtk_css_section_get_start_location(section)->lines + 1 : 0,
               error ? error->message : "unknown");
}

static gboolean
widget_tree_has_css_class(GtkWidget *root, const char *css_class)
{
    if (!root)
        return FALSE;
    if (gtk_widget_has_css_class(root, css_class))
        return TRUE;
    for (GtkWidget *c = gtk_widget_get_first_child(root);
         c != NULL;
         c = gtk_widget_get_next_sibling(c)) {
        if (widget_tree_has_css_class(c, css_class))
            return TRUE;
    }
    return FALSE;
}

static void
test_style_css_and_widget_ui_contract(void)
{
    if (!gtk_init_check()) {
        g_test_skip("No display available for GTK4 widget/CSS contract check");
        return;
    }

    /* 1. Master data/defaults/style.css MUST parse with zero GTK4 CSS errors */
    g_autofree gchar *css_path = g_build_filename(G_STRINGIFY(KAJO_SOURCE_DIR),
                                                  "data", "defaults", "style.css", NULL);
    if (!g_file_test(css_path, G_FILE_TEST_EXISTS)) {
        g_free(css_path);
        css_path = g_strdup("../data/defaults/style.css");
    }
    if (g_file_test(css_path, G_FILE_TEST_EXISTS)) {
        GtkCssProvider *provider = gtk_css_provider_new();
        guint parse_errors = 0;
        g_signal_connect(provider, "parsing-error", G_CALLBACK(on_css_parse_error), &parse_errors);
        gtk_css_provider_load_from_path(provider, css_path);
        g_assert_cmpuint(parse_errors, ==, 0);
        g_object_unref(provider);
    }

    /* 2. shell_widget_apply_mode_visibility() MUST never hide the sole child */
    GtkWidget *only_label = g_object_ref_sink(gtk_label_new("23:35"));
    shell_widget_apply_mode_visibility(WIDGET_MODE_ICON, NULL, only_label);
    g_assert_true(gtk_widget_get_visible(only_label));
    g_object_unref(only_label);

    GtkWidget *only_icon = g_object_ref_sink(gtk_image_new());
    shell_widget_apply_mode_visibility(WIDGET_MODE_LABEL, only_icon, NULL);
    g_assert_true(gtk_widget_get_visible(only_icon));
    g_object_unref(only_icon);

    /* 3. Verify 3-zone popover architecture & GTK_ALIGN_CENTER bar boxes on pure-GTK widgets */
    static const char *const checked_ids[] = {
        "clock", "window", "keyboard-layout", "idle-inhibitor", "dnd", "notifications"
    };
    ShellWidgetRegistry *reg = shell_widget_registry_new();
    for (guint i = 0; i < G_N_ELEMENTS(checked_ids); i++) {
        const ShellWidgetClass *cls = shell_widget_registry_find(reg, checked_ids[i]);
        g_assert_nonnull(cls);

        ShellWidget *w = cls->create(NULL);
        g_assert_nonnull(w);
        w->klass = cls;

        GtkWidget *btn = cls->get_widget(w);
        g_assert_nonnull(btn);
        g_object_ref_sink(btn);
        g_assert_true(gtk_widget_has_css_class(btn, "shell-widget"));
        g_assert_true(GTK_IS_MENU_BUTTON(btn));

        GtkPopover *pop = gtk_menu_button_get_popover(GTK_MENU_BUTTON(btn));
        g_assert_nonnull(pop);
        g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(pop), "shell-popover"));
        g_assert_true(widget_tree_has_css_class(GTK_WIDGET(pop), "shell-popover-header"));
        g_assert_true(widget_tree_has_css_class(GTK_WIDGET(pop), "shell-popover-body"));
        g_assert_true(widget_tree_has_css_class(GTK_WIDGET(pop), "shell-popover-footer"));
        g_assert_true(widget_tree_has_css_class(GTK_WIDGET(pop), "popover-compact") ||
                      widget_tree_has_css_class(GTK_WIDGET(pop), "popover-medium") ||
                      widget_tree_has_css_class(GTK_WIDGET(pop), "popover-expanded"));

        for (int m = WIDGET_MODE_ICON; m <= WIDGET_MODE_FULL; m++) {
            w->mode = (ShellWidgetMode)m;
            cls->enable(w);
            cls->disable(w);
        }

        cls->destroy(w);
        g_object_unref(btn);
    }
    shell_widget_registry_destroy(reg);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/registry/widget-classes-integrity", test_widget_registry_integrity);
    g_test_add_func("/registry/style-css-and-widget-ui-contract", test_style_css_and_widget_ui_contract);

    return g_test_run();
}
