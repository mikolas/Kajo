#define _GNU_SOURCE
#include "../config.h"
#include "../icons.h"
#include "../lock/pam_auth.h"
#include "../theme.h"
#include "greetd_ipc.h"
#include "greeter_users.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

static gchar   *opt_default_cmd  = NULL;
static gchar   *opt_default_user = NULL;
static gboolean opt_test_mode    = FALSE;
static gboolean opt_fullscreen   = FALSE;

static GOptionEntry greeter_cli_entries[] = {
    { "cmd",        'c', 0, G_OPTION_ARG_STRING, &opt_default_cmd,  "Default session command", "CMD" },
    { "user",       'u', 0, G_OPTION_ARG_STRING, &opt_default_user, "Pre-selected username",   "USER" },
    { "test",       't', 0, G_OPTION_ARG_NONE,   &opt_test_mode,    "Run in desktop preview window mode", NULL },
    { "fullscreen", 'f', 0, G_OPTION_ARG_NONE,   &opt_fullscreen,   "Force standard XDG fullscreen window (e.g. cage)", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
};

typedef struct _GreeterApp GreeterApp;

typedef struct {
    GreeterApp *app;
    GtkWidget  *window;
    GtkWidget  *clock_label;
    GtkWidget  *date_label;
    GtkWidget  *card;
    GtkWidget  *realname_label;
    GtkWidget  *username_badge;
    GtkWidget  *combo_user;
    GtkWidget  *password_entry;
    GtkWidget  *submit_btn;
    GtkWidget  *combo_session;
    GtkWidget  *status_label;
    gboolean    is_primary;
    guint       shake_timer_id;
    gint        shake_step;
} GreeterSurface;

struct _GreeterApp {
    GtkApplication  *gtk_app;
    ShellConfig     *config;
    ShellTheme      *theme;
    GDBusConnection *system_bus;
    GCancellable    *cancellable;
    GPtrArray       *users;      /* GreeterUser* */
    GPtrArray       *sessions;   /* GreeterSession* */
    GPtrArray       *surfaces;   /* GreeterSurface* */
    guint            clock_timer_id;
    guint            selected_user_idx;
    guint            selected_session_idx;
    gboolean         auth_in_progress;
    gboolean         auth_succeeded;
    gboolean         use_layer_shell;
};

static void
on_greeter_window_destroy(GtkWidget *widget, gpointer user_data)
{
    GreeterSurface *surf = user_data;
    if (surf && surf->window == widget) {
        surf->window         = NULL;
        surf->clock_label    = NULL;
        surf->date_label     = NULL;
        surf->card           = NULL;
        surf->realname_label = NULL;
        surf->username_badge = NULL;
        surf->combo_user     = NULL;
        surf->password_entry = NULL;
        surf->submit_btn     = NULL;
        surf->combo_session  = NULL;
        surf->status_label   = NULL;
    }
}

static void
greeter_surface_free(gpointer data)
{
    GreeterSurface *surf = data;
    if (!surf)
        return;
    if (surf->shake_timer_id > 0) {
        g_source_remove(surf->shake_timer_id);
        surf->shake_timer_id = 0;
    }
    if (surf->window) {
        g_signal_handlers_disconnect_by_func(surf->window, on_greeter_window_destroy, surf);
        gtk_window_destroy(GTK_WINDOW(surf->window));
        surf->window = NULL;
    }
    g_free(surf);
}

static void
update_greeter_clocks(GreeterApp *app)
{
    if (!app || !app->surfaces)
        return;

    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    if (!now)
        return;

    g_autofree gchar *time_str = g_date_time_format(now, "%H : %M");
    g_autofree gchar *date_str = g_date_time_format(now, "%A, %B %e");
    g_autofree gchar *date_upper = date_str ? g_utf8_strup(date_str, -1) : g_strdup("");

    for (guint i = 0; i < app->surfaces->len; i++) {
        GreeterSurface *surf = g_ptr_array_index(app->surfaces, i);
        if (surf->clock_label && time_str)
            gtk_label_set_text(GTK_LABEL(surf->clock_label), time_str);
        if (surf->date_label && date_upper)
            gtk_label_set_text(GTK_LABEL(surf->date_label), date_upper);
    }
}

static gboolean
on_greeter_clock_tick(gpointer user_data)
{
    GreeterApp *app = user_data;
    update_greeter_clocks(app);
    return G_SOURCE_CONTINUE;
}

static gboolean
on_shake_tick(gpointer user_data)
{
    GreeterSurface *surf = user_data;
    if (!surf || !surf->card)
        return G_SOURCE_REMOVE;

    static const gint offsets[] = { -10, 10, -8, 8, -5, 5, -2, 2, 0 };
    const gint n_steps = (gint)G_N_ELEMENTS(offsets);

    if (surf->shake_step >= n_steps) {
        gtk_widget_set_margin_start(surf->card, 0);
        gtk_widget_set_margin_end(surf->card, 0);
        surf->shake_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    gint off = offsets[surf->shake_step++];
    if (off >= 0) {
        gtk_widget_set_margin_start(surf->card, off * 2);
        gtk_widget_set_margin_end(surf->card, 0);
    } else {
        gtk_widget_set_margin_start(surf->card, 0);
        gtk_widget_set_margin_end(surf->card, (-off) * 2);
    }
    return G_SOURCE_CONTINUE;
}

static void
trigger_error_shake(GreeterSurface *surf)
{
    if (!surf || !surf->card)
        return;
    if (surf->shake_timer_id > 0)
        g_source_remove(surf->shake_timer_id);
    surf->shake_step = 0;
    surf->shake_timer_id = g_timeout_add(32, on_shake_tick, surf);
}

static gboolean
password_entry_has_focus(GreeterSurface *surf)
{
    if (!surf || !surf->window || !surf->password_entry)
        return FALSE;

    if (gtk_widget_get_focus_child(surf->password_entry) != NULL)
        return TRUE;

    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(surf->window));
    return (focus && (focus == surf->password_entry ||
                      gtk_widget_is_ancestor(focus, surf->password_entry)));
}

static void
focus_password_entry_preserve_cursor(GreeterSurface *surf)
{
    if (!surf || !surf->password_entry)
        return;

    if (!password_entry_has_focus(surf)) {
        gtk_widget_grab_focus(surf->password_entry);
        gtk_editable_set_position(GTK_EDITABLE(surf->password_entry), -1);
    }
}

static void
reset_and_clear_password_entry(GreeterSurface *surf, gboolean clear_text)
{
    if (!surf || !surf->password_entry)
        return;

    /* Toggling show-peek-icon off/on forces GtkPasswordEntry's inner GtkText
       back to masked mode (visibility = FALSE) even if the user clicked the eye icon */
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(surf->password_entry), FALSE);
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(surf->password_entry), TRUE);

    if (clear_text) {
        gtk_editable_set_text(GTK_EDITABLE(surf->password_entry), "");
    }
}

static void
set_greeter_auth_state(GreeterApp  *app,
                       gboolean     in_progress,
                       const gchar *status_msg,
                       gboolean     is_error,
                       gboolean     clear_password)
{
    if (!app)
        return;

    app->auth_in_progress = in_progress;

    for (guint i = 0; i < app->surfaces->len; i++) {
        GreeterSurface *surf = g_ptr_array_index(app->surfaces, i);
        if (!surf->is_primary)
            continue;

        if (surf->combo_user)
            gtk_widget_set_sensitive(surf->combo_user, !in_progress);
        if (surf->combo_session)
            gtk_widget_set_sensitive(surf->combo_session, !in_progress);
        if (surf->password_entry) {
            gtk_widget_set_sensitive(surf->password_entry, !in_progress);
            reset_and_clear_password_entry(surf, clear_password);
        }
        if (surf->submit_btn) {
            gtk_widget_set_sensitive(surf->submit_btn, !in_progress);
            gtk_button_set_label(GTK_BUTTON(surf->submit_btn), in_progress ? "⟳" : "→");
        }
        if (surf->card) {
            if (is_error) {
                gtk_widget_add_css_class(surf->card, "auth-error");
                trigger_error_shake(surf);
            } else {
                gtk_widget_remove_css_class(surf->card, "auth-error");
            }
        }
        if (surf->status_label) {
            gtk_widget_remove_css_class(surf->status_label, "verifying");
            gtk_widget_remove_css_class(surf->status_label, "error");
            if (in_progress)
                gtk_widget_add_css_class(surf->status_label, "verifying");
            else if (is_error)
                gtk_widget_add_css_class(surf->status_label, "error");

            gtk_label_set_text(GTK_LABEL(surf->status_label),
                               status_msg ? status_msg : "Enter password to sign in");
        }
        if (!in_progress && surf->password_entry) {
            focus_password_entry_preserve_cursor(surf);
        }
    }
}

static void
quit_greeter_compositor_and_app(GreeterApp *app)
{
    /* Save selected user & session to persistent state cache */
    const gchar *uname = NULL;
    const gchar *sid   = NULL;
    if (app->users && app->selected_user_idx < app->users->len) {
        GreeterUser *u = g_ptr_array_index(app->users, app->selected_user_idx);
        uname = u->username;
    }
    if (app->sessions && app->selected_session_idx < app->sessions->len) {
        GreeterSession *s = g_ptr_array_index(app->sessions, app->selected_session_idx);
        sid = s->id;
    }
    if (uname && sid) {
        greeter_state_save(NULL, uname, sid);
    }

    /* When running inside niri-greeter (or nested niri test with KAJO_GREETER_QUIT_NIRI=1),
       ask the greeter niri instance to exit cleanly so greetd hands the VT to the new session */
    const gchar *niri_sock = g_getenv("NIRI_SOCKET");
    const gchar *quit_niri = g_getenv("KAJO_GREETER_QUIT_NIRI");
    if (niri_sock && niri_sock[0] != '\0' && (!opt_test_mode || g_strcmp0(quit_niri, "1") == 0)) {
        const gchar *argv[] = { "niri", "msg", "action", "quit", "--skip-confirmation", NULL };
        g_spawn_sync(NULL, (gchar **)argv, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, NULL, NULL, NULL, NULL);
    }

    if (app && app->gtk_app) {
        g_application_quit(G_APPLICATION(app->gtk_app));
    }
}

static void
on_test_pam_auth_complete(GObject      *source_object G_GNUC_UNUSED,
                          GAsyncResult *res,
                          gpointer      user_data)
{
    GreeterApp *app = user_data;
    g_autoptr(GError) error = NULL;
    gboolean ok = shell_pam_authenticate_finish(res, &error);

    if (ok) {
        app->auth_succeeded = TRUE;
        GreeterUser *u = (app->users && app->selected_user_idx < app->users->len)
                             ? g_ptr_array_index(app->users, app->selected_user_idx)
                             : NULL;
        GreeterSession *s = (app->sessions && app->selected_session_idx < app->sessions->len)
                                ? g_ptr_array_index(app->sessions, app->selected_session_idx)
                                : NULL;
        g_message("[TEST MODE] Authenticated user '%s' -> starting session '%s' (%s)",
                  u ? u->username : "user",
                  s ? s->name : "Niri",
                  s ? s->exec : "niri-session");
        set_greeter_auth_state(app, FALSE, "✓ Authentication succeeded — starting session...", FALSE, TRUE);
        quit_greeter_compositor_and_app(app);
    } else {
        g_autofree gchar *msg = g_strdup_printf(
            "⚠ %s", (error && error->message) ? error->message : "Authentication failed — try again");
        set_greeter_auth_state(app, FALSE, msg, TRUE, TRUE);
    }
}

static void
on_greetd_auth_complete(GObject      *source_object G_GNUC_UNUSED,
                        GAsyncResult *res,
                        gpointer      user_data)
{
    GreeterApp *app = user_data;
    g_autoptr(GError) error = NULL;
    gboolean ok = greetd_authenticate_and_start_finish(res, &error);

    if (ok) {
        app->auth_succeeded = TRUE;
        set_greeter_auth_state(app, FALSE, "✓ Starting session...", FALSE, TRUE);
        quit_greeter_compositor_and_app(app);
    } else {
        g_autofree gchar *msg = g_strdup_printf(
            "⚠ %s", (error && error->message) ? error->message : "Authentication failed — try again");
        set_greeter_auth_state(app, FALSE, msg, TRUE, TRUE);
    }
}

static void
submit_greeter_login(GreeterSurface *surf)
{
    if (!surf || !surf->app || !surf->password_entry)
        return;

    GreeterApp *app = surf->app;
    if (app->auth_in_progress)
        return;

    const gchar *pw = gtk_editable_get_text(GTK_EDITABLE(surf->password_entry));
    if (!pw || pw[0] == '\0') {
        set_greeter_auth_state(app, FALSE, "⚠ Please enter your password", TRUE, TRUE);
        return;
    }

    /* Copy password into a local buffer and immediately scrub/clear the GTK entry
       and reset its peek-icon visibility BEFORE starting async authentication */
    gsize pw_len = strlen(pw);
    gchar *pw_copy = g_strdup(pw);
    reset_and_clear_password_entry(surf, TRUE);

    GreeterUser *u = (app->users && app->selected_user_idx < app->users->len)
                         ? g_ptr_array_index(app->users, app->selected_user_idx)
                         : NULL;
    GreeterSession *s = (app->sessions && app->selected_session_idx < app->sessions->len)
                            ? g_ptr_array_index(app->sessions, app->selected_session_idx)
                            : NULL;

    const gchar *username = u ? u->username : g_get_user_name();
    set_greeter_auth_state(app, TRUE, "⟳ Authenticating...", FALSE, TRUE);

    const gchar *greetd_sock = g_getenv("GREETD_SOCK");
    if (opt_test_mode && (!greetd_sock || greetd_sock[0] == '\0')) {
        /* Test mode on live desktop: verify against real PAM worker thread */
        shell_pam_authenticate_user_async(username, pw_copy, app->cancellable, on_test_pam_auth_complete, app);
    } else {
        const gchar *fallback_cmd[] = { "niri-session", NULL };
        const gchar *const *cmd = (s && s->cmd_argv) ? (const gchar *const *)s->cmd_argv : fallback_cmd;
        const gchar *const *env = (s && s->env_vars) ? (const gchar *const *)s->env_vars : NULL;

        greetd_authenticate_and_start_async(
            greetd_sock, username, pw_copy, cmd, env,
            app->cancellable, on_greetd_auth_complete, app);
    }

    explicit_bzero(pw_copy, pw_len);
    g_free(pw_copy);
}

static void
on_password_activate(GtkWidget *entry G_GNUC_UNUSED, gpointer user_data)
{
    GreeterSurface *surf = user_data;
    submit_greeter_login(surf);
}

static void
on_submit_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GreeterSurface *surf = user_data;
    submit_greeter_login(surf);
}

static void
on_user_dropdown_changed(GObject    * obj,
                         GParamSpec *pspec G_GNUC_UNUSED,
                         gpointer    user_data)
{
    GreeterSurface *surf = user_data;
    if (!surf || !surf->app || !surf->app->users)
        return;

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (sel >= surf->app->users->len)
        return;

    surf->app->selected_user_idx = sel;
    GreeterUser *u = g_ptr_array_index(surf->app->users, sel);

    if (surf->realname_label)
        gtk_label_set_text(GTK_LABEL(surf->realname_label), u->real_name);
    if (surf->username_badge) {
        g_autofree gchar *badge = g_strdup_printf("@%s", u->username);
        gtk_label_set_text(GTK_LABEL(surf->username_badge), badge);
    }
    reset_and_clear_password_entry(surf, TRUE);
    focus_password_entry_preserve_cursor(surf);
}

static void
on_session_dropdown_changed(GObject    *obj,
                            GParamSpec *pspec G_GNUC_UNUSED,
                            gpointer    user_data)
{
    GreeterSurface *surf = user_data;
    if (!surf || !surf->app || !surf->app->sessions)
        return;

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (sel >= surf->app->sessions->len)
        return;

    surf->app->selected_session_idx = sel;
    focus_password_entry_preserve_cursor(surf);
}

static gboolean
on_greeter_key_pressed(GtkEventControllerKey *controller G_GNUC_UNUSED,
                       guint                  keyval,
                       guint                  keycode G_GNUC_UNUSED,
                       GdkModifierType        state G_GNUC_UNUSED,
                       gpointer               user_data)
{
    GreeterSurface *surf = user_data;
    if (!surf || !surf->app)
        return FALSE;

    if (keyval == GDK_KEY_Escape) {
        if (opt_test_mode && (state & GDK_CONTROL_MASK)) {
            g_application_quit(G_APPLICATION(surf->app->gtk_app));
            return TRUE;
        }
        if (surf->password_entry && !surf->app->auth_in_progress) {
            reset_and_clear_password_entry(surf, TRUE);
            set_greeter_auth_state(surf->app, FALSE, "Enter password to sign in", FALSE, TRUE);
            focus_password_entry_preserve_cursor(surf);
        }
        return TRUE;
    }

    /* Allow Tab / navigation keys to move between dropdowns and buttons naturally */
    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab ||
        keyval == GDK_KEY_Up || keyval == GDK_KEY_Down ||
        keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
        keyval == GDK_KEY_space) {
        return FALSE;
    }

    focus_password_entry_preserve_cursor(surf);
    return FALSE;
}

static void
call_logind_power_method(GreeterApp *app, const gchar *method_name)
{
    if (!app)
        return;

    if (opt_test_mode) {
        g_autofree gchar *msg = g_strdup_printf("[TEST MODE] Power action '%s' triggered", method_name);
        set_greeter_auth_state(app, FALSE, msg, FALSE, FALSE);
        return;
    }

    if (!app->system_bus)
        return;

    g_dbus_connection_call(
        app->system_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        method_name,
        g_variant_new("(b)", TRUE),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        NULL,
        NULL);
}

static void
on_suspend_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    call_logind_power_method((GreeterApp *)user_data, "Suspend");
}

static void
on_reboot_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    call_logind_power_method((GreeterApp *)user_data, "Reboot");
}

static void
on_poweroff_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    call_logind_power_method((GreeterApp *)user_data, "PowerOff");
}

static void
on_window_mapped(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    GreeterSurface *surf = user_data;
    if (surf && surf->is_primary && surf->password_entry) {
        gtk_widget_grab_focus(surf->password_entry);
    }
}

static gboolean
on_window_close_request(GtkWindow *window G_GNUC_UNUSED, gpointer user_data)
{
    GreeterSurface *surf = user_data;
    if (!surf || !surf->app)
        return FALSE;

    /* Allow closing only in desktop preview (--test) mode or after successful auth */
    if (opt_test_mode || surf->app->auth_succeeded)
        return FALSE;

    return TRUE;
}

static GtkWidget *
create_field_label(const gchar *text)
{
    GtkWidget *lbl = gtk_label_new(text);
    gtk_widget_add_css_class(lbl, "greeter-field-label");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    return lbl;
}

static void
build_greeter_surface(GreeterApp *app, GdkMonitor *monitor, gboolean is_primary)
{
    GreeterSurface *surf = g_new0(GreeterSurface, 1);
    surf->app        = app;
    surf->is_primary = is_primary;

    GtkWidget *win = gtk_application_window_new(app->gtk_app);
    surf->window = win;
    gtk_window_set_title(GTK_WINDOW(win), "Kajo Greeter");
    gtk_widget_add_css_class(win, "shell-lock-window");
    gtk_widget_add_css_class(win, "shell-greeter-window");

    if (app->use_layer_shell && monitor != NULL) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_namespace(GTK_WINDOW(win), "kajo-greeter");
        gtk_layer_set_monitor(GTK_WINDOW(win), monitor);
        gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
        gtk_layer_set_keyboard_mode(
            GTK_WINDOW(win),
            is_primary ? GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE
                       : GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    } else if (opt_test_mode && !opt_fullscreen) {
        gtk_window_set_default_size(GTK_WINDOW(win), 1280, 820);
    } else {
        gtk_window_fullscreen(GTK_WINDOW(win));
    }

    g_signal_connect(win, "destroy", G_CALLBACK(on_greeter_window_destroy), surf);
    g_signal_connect(win, "map", G_CALLBACK(on_window_mapped), surf);
    g_signal_connect(win, "close-request", G_CALLBACK(on_window_close_request), surf);

    GtkWidget *root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(root_box, TRUE);
    gtk_widget_set_vexpand(root_box, TRUE);

    /* Top Status Header Bar */
    GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(top_bar, "greeter-top-bar");
    gtk_widget_set_margin_top(top_bar, 20);
    gtk_widget_set_margin_start(top_bar, 32);
    gtk_widget_set_margin_end(top_bar, 32);

    const gchar *hostname = g_get_host_name();
    GtkWidget *host_lbl = gtk_label_new(hostname ? hostname : "localhost");
    gtk_widget_add_css_class(host_lbl, "greeter-Top-meta");
    gtk_widget_add_css_class(host_lbl, "lock-badge-label");
    gtk_widget_set_halign(host_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(host_lbl, TRUE);

    GtkWidget *env_lbl = gtk_label_new(opt_test_mode ? "WAYLAND · KAJO GREETER [PREVIEW]" : "WAYLAND · KAJO GREETER");
    gtk_widget_add_css_class(env_lbl, "lock-badge-label");
    gtk_widget_set_halign(env_lbl, GTK_ALIGN_END);

    gtk_box_append(GTK_BOX(top_bar), host_lbl);
    gtk_box_append(GTK_BOX(top_bar), env_lbl);
    gtk_box_append(GTK_BOX(root_box), top_bar);

    /* Center Column */
    GtkWidget *center_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_hexpand(center_box, TRUE);
    gtk_widget_set_vexpand(center_box, TRUE);
    gtk_widget_set_halign(center_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(center_box, GTK_ALIGN_CENTER);

    /* Hero Clock & Date */
    GtkWidget *clock_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(clock_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(clock_box, 8);

    surf->clock_label = gtk_label_new("00 : 00");
    gtk_widget_add_css_class(surf->clock_label, "lock-clock-label");
    gtk_widget_set_halign(surf->clock_label, GTK_ALIGN_CENTER);

    surf->date_label = gtk_label_new("");
    gtk_widget_add_css_class(surf->date_label, "lock-date-label");
    gtk_widget_set_halign(surf->date_label, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(clock_box), surf->clock_label);
    gtk_box_append(GTK_BOX(clock_box), surf->date_label);
    gtk_box_append(GTK_BOX(center_box), clock_box);

    if (is_primary) {
        GreeterUser *init_user = g_ptr_array_index(app->users, app->selected_user_idx);

        /* Metro Auth Card */
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        surf->card = card;
        gtk_widget_add_css_class(card, "lock-card");
        gtk_widget_add_css_class(card, "greeter-auth-card");
        gtk_widget_set_size_request(card, 400, -1);
        gtk_widget_set_halign(card, GTK_ALIGN_CENTER);

        /* Avatar Ring */
        GtkWidget *avatar_wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(avatar_wrap, "lock-avatar-ring");
        gtk_widget_set_halign(avatar_wrap, GTK_ALIGN_CENTER);

        GtkWidget *user_icon = gtk_image_new_from_icon_name("tb-user-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(user_icon), 32);
        gtk_widget_set_halign(user_icon, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(user_icon, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(user_icon, TRUE);
        gtk_widget_set_vexpand(user_icon, TRUE);
        gtk_box_append(GTK_BOX(avatar_wrap), user_icon);
        gtk_box_append(GTK_BOX(card), avatar_wrap);

        /* Real Name & @username Badge */
        surf->realname_label = gtk_label_new(init_user->real_name);
        gtk_widget_add_css_class(surf->realname_label, "lock-username-label");
        gtk_widget_set_halign(surf->realname_label, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(card), surf->realname_label);

        g_autofree gchar *at_user = g_strdup_printf("@%s", init_user->username);
        surf->username_badge = gtk_label_new(at_user);
        gtk_widget_add_css_class(surf->username_badge, "lock-badge-label");
        gtk_widget_set_halign(surf->username_badge, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_bottom(surf->username_badge, 4);
        gtk_box_append(GTK_BOX(card), surf->username_badge);

        /* USER ACCOUNT Selector */
        gtk_box_append(GTK_BOX(card), create_field_label("USER ACCOUNT"));
        GtkStringList *user_slist = gtk_string_list_new(NULL);
        for (guint i = 0; i < app->users->len; i++) {
            GreeterUser *u = g_ptr_array_index(app->users, i);
            g_autofree gchar *label = (g_strcmp0(u->real_name, u->username) != 0)
                                          ? g_strdup_printf("%s (%s)", u->username, u->real_name)
                                          : g_strdup(u->username);
            gtk_string_list_append(user_slist, label);
        }
        surf->combo_user = gtk_drop_down_new(G_LIST_MODEL(user_slist), NULL);
        gtk_widget_add_css_class(surf->combo_user, "greeter-dropdown");
        gtk_widget_set_hexpand(surf->combo_user, TRUE);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(surf->combo_user), app->selected_user_idx);
        g_signal_connect(surf->combo_user, "notify::selected", G_CALLBACK(on_user_dropdown_changed), surf);
        gtk_box_append(GTK_BOX(card), surf->combo_user);

        /* PASSWORD Input Row */
        gtk_box_append(GTK_BOX(card), create_field_label("PASSWORD"));
        GtkWidget *pw_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        surf->password_entry = gtk_password_entry_new();
        gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(surf->password_entry), TRUE);
        g_object_set(surf->password_entry, "placeholder-text", "Enter password...", NULL);
        gtk_widget_add_css_class(surf->password_entry, "lock-password-entry");
        gtk_widget_set_hexpand(surf->password_entry, TRUE);
        g_signal_connect(surf->password_entry, "activate", G_CALLBACK(on_password_activate), surf);

        surf->submit_btn = gtk_button_new_with_label("→");
        gtk_widget_add_css_class(surf->submit_btn, "lock-submit-btn");
        gtk_widget_set_focus_on_click(surf->submit_btn, FALSE);
        g_signal_connect(surf->submit_btn, "clicked", G_CALLBACK(on_submit_clicked), surf);

        gtk_box_append(GTK_BOX(pw_row), surf->password_entry);
        gtk_box_append(GTK_BOX(pw_row), surf->submit_btn);
        gtk_box_append(GTK_BOX(card), pw_row);

        /* SESSION Selector */
        gtk_box_append(GTK_BOX(card), create_field_label("SESSION"));
        GtkStringList *sess_slist = gtk_string_list_new(NULL);
        for (guint i = 0; i < app->sessions->len; i++) {
            GreeterSession *s = g_ptr_array_index(app->sessions, i);
            g_autofree gchar *type_tag = g_ascii_strcasecmp(s->type, "x11") == 0
                                             ? g_strdup("X11")
                                             : g_strdup("Wayland");
            g_autofree gchar *slabel = g_strdup_printf("%s (%s)", s->name, type_tag);
            gtk_string_list_append(sess_slist, slabel);
        }
        surf->combo_session = gtk_drop_down_new(G_LIST_MODEL(sess_slist), NULL);
        gtk_widget_add_css_class(surf->combo_session, "greeter-dropdown");
        gtk_widget_set_hexpand(surf->combo_session, TRUE);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(surf->combo_session), app->selected_session_idx);
        g_signal_connect(surf->combo_session, "notify::selected", G_CALLBACK(on_session_dropdown_changed), surf);
        gtk_box_append(GTK_BOX(card), surf->combo_session);

        /* Status / Error Label */
        surf->status_label = gtk_label_new("Enter password to sign in");
        gtk_widget_add_css_class(surf->status_label, "lock-status-label");
        gtk_widget_set_halign(surf->status_label, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(surf->status_label, 4);
        gtk_box_append(GTK_BOX(card), surf->status_label);

        gtk_box_append(GTK_BOX(center_box), card);
    }

    gtk_box_append(GTK_BOX(root_box), center_box);

    /* Bottom Footer Bar (Keyboard Hints + Power Actions) */
    if (is_primary) {
        GtkWidget *footer_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_add_css_class(footer_bar, "lock-footer-bar");
        gtk_widget_set_margin_bottom(footer_bar, 24);
        gtk_widget_set_margin_start(footer_bar, 32);
        gtk_widget_set_margin_end(footer_bar, 32);

        const gchar *hint_str = opt_test_mode
                                    ? "[Esc] Clear   [Tab] Switch Field   [Ctrl+Esc] Exit Preview"
                                    : "[Esc] Clear   [Tab] Switch Field   [Enter] Sign In";
        GtkWidget *hint_lbl = gtk_label_new(hint_str);
        gtk_widget_add_css_class(hint_lbl, "lock-status-label");
        gtk_widget_set_halign(hint_lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(hint_lbl, TRUE);
        gtk_box_append(GTK_BOX(footer_bar), hint_lbl);

        GtkWidget *power_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_halign(power_box, GTK_ALIGN_END);

        GtkWidget *btn_suspend = gtk_button_new_with_label("[ 🌙 SUSPEND ]");
        gtk_widget_add_css_class(btn_suspend, "lock-action-btn");
        gtk_widget_set_focus_on_click(btn_suspend, FALSE);
        g_signal_connect(btn_suspend, "clicked", G_CALLBACK(on_suspend_clicked), app);

        GtkWidget *btn_reboot = gtk_button_new_with_label("[ ↻ REBOOT ]");
        gtk_widget_add_css_class(btn_reboot, "lock-action-btn");
        gtk_widget_set_focus_on_click(btn_reboot, FALSE);
        g_signal_connect(btn_reboot, "clicked", G_CALLBACK(on_reboot_clicked), app);

        GtkWidget *btn_poweroff = gtk_button_new_with_label("[ ⏻ POWER OFF ]");
        gtk_widget_add_css_class(btn_poweroff, "lock-action-btn");
        gtk_widget_add_css_class(btn_poweroff, "greeter-poweroff-btn");
        gtk_widget_set_focus_on_click(btn_poweroff, FALSE);
        g_signal_connect(btn_poweroff, "clicked", G_CALLBACK(on_poweroff_clicked), app);

        gtk_box_append(GTK_BOX(power_box), btn_suspend);
        gtk_box_append(GTK_BOX(power_box), btn_reboot);
        gtk_box_append(GTK_BOX(power_box), btn_poweroff);
        gtk_box_append(GTK_BOX(footer_bar), power_box);

        gtk_box_append(GTK_BOX(root_box), footer_bar);
    }

    gtk_window_set_child(GTK_WINDOW(win), root_box);

    if (is_primary) {
        GtkEventController *key_ctrl = gtk_event_controller_key_new();
        g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_greeter_key_pressed), surf);
        gtk_widget_add_controller(win, key_ctrl);
    }

    g_ptr_array_add(app->surfaces, surf);
    gtk_window_present(GTK_WINDOW(win));
}

static void
on_monitors_changed(GListModel *monitors,
                    guint       position,
                    guint       removed,
                    guint       added,
                    gpointer    user_data)
{
    GreeterApp *app = user_data;
    if (!app || !app->use_layer_shell)
        return;

    if (removed > 0 && app->surfaces && position < app->surfaces->len) {
        guint max_rem = app->surfaces->len - position;
        guint n_rem = MIN(removed, max_rem);
        g_ptr_array_remove_range(app->surfaces, position, n_rem);
    }

    for (guint i = 0; i < added; i++) {
        GdkMonitor *mon = g_list_model_get_item(monitors, position + i);
        if (mon) {
            build_greeter_surface(app, mon, (app->surfaces->len == 0));
            g_object_unref(mon);
        }
    }
    update_greeter_clocks(app);
}

static void
on_activate(GtkApplication *gtk_app G_GNUC_UNUSED, gpointer user_data)
{
    GreeterApp *app = user_data;
    GdkDisplay *display = gdk_display_get_default();

    /* Hard-disable GTK4 interactive inspector (Ctrl+Shift+I / Ctrl+Shift+D) */
    gtk_window_set_interactive_debugging(FALSE);

    /* Initialize ShellTheme so style.css and Metro styling apply (Rule #3) */
    app->config = shell_config_load(NULL);
    shell_icons_init(app->config ? app->config->icon_map_path : NULL);
    app->theme = shell_theme_new(app->config ? app->config->css_path : NULL);
    shell_theme_apply(app->theme, display);

    app->use_layer_shell = (!opt_test_mode && !opt_fullscreen && gtk_layer_is_supported());

    if (app->use_layer_shell && display != NULL) {
        GListModel *monitors = gdk_display_get_monitors(display);
        guint n_monitors = monitors ? g_list_model_get_n_items(monitors) : 0;
        if (n_monitors > 0) {
            for (guint i = 0; i < n_monitors; i++) {
                GdkMonitor *mon = g_list_model_get_item(monitors, i);
                build_greeter_surface(app, mon, (i == 0));
                g_object_unref(mon);
            }
        } else {
            build_greeter_surface(app, NULL, TRUE);
        }
        if (monitors) {
            g_signal_connect(monitors, "items-changed", G_CALLBACK(on_monitors_changed), app);
        }
    } else {
        build_greeter_surface(app, NULL, TRUE);
    }

    update_greeter_clocks(app);
    if (app->clock_timer_id == 0) {
        app->clock_timer_id = g_timeout_add_seconds(1, on_greeter_clock_tick, app);
    }
}

int
main(int argc, char *argv[])
{
    /* Prevent core dumps and ptrace attachment to protect password buffers in memory */
    prctl(PR_SET_DUMPABLE, 0);

    /* Strip GTK debug environment variables before GTK initializes */
    g_unsetenv("GTK_DEBUG");
    g_unsetenv("GDK_DEBUG");
    g_unsetenv("G_MESSAGES_DEBUG");

    g_autoptr(GOptionContext) ctx = g_option_context_new("— Kajo Wayland Login Greeter for greetd");
    g_option_context_add_main_entries(ctx, greeter_cli_entries, NULL);

    GError *opt_err = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &opt_err)) {
        g_printerr("kajo-greeter: option parsing failed: %s\n", opt_err->message);
        g_clear_error(&opt_err);
        return 1;
    }

    if (!opt_default_cmd)
        opt_default_cmd = g_strdup(g_getenv("KAJO_GREETER_CMD"));
    if (!opt_default_user)
        opt_default_user = g_strdup(g_getenv("KAJO_GREETER_USER"));
    if (!opt_test_mode && g_strcmp0(g_getenv("KAJO_GREETER_TEST"), "1") == 0)
        opt_test_mode = TRUE;

    /* Security guard: never allow --test mode when running inside a real greetd session */
    const gchar *greetd_sock = g_getenv("GREETD_SOCK");
    if (greetd_sock && greetd_sock[0] != '\0') {
        opt_test_mode = FALSE;
    }

    GreeterApp app = { 0 };
    app.cancellable = g_cancellable_new();
    app.surfaces    = g_ptr_array_new_with_free_func(greeter_surface_free);
    app.users       = greeter_discover_users();
    app.sessions    = greeter_discover_sessions(opt_default_cmd);
    app.system_bus  = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);

    /* Resolve initial selected user & session from CLI or state cache */
    g_autofree gchar *last_user    = NULL;
    g_autofree gchar *last_session = NULL;
    greeter_state_load(NULL, &last_user, &last_session);

    const gchar *target_user = (opt_default_user && opt_default_user[0] != '\0')
                                   ? opt_default_user
                                   : last_user;
    if (target_user) {
        for (guint i = 0; i < app.users->len; i++) {
            GreeterUser *u = g_ptr_array_index(app.users, i);
            if (g_strcmp0(u->username, target_user) == 0) {
                app.selected_user_idx = i;
                break;
            }
        }
    }

    const gchar *target_sess = (opt_default_cmd && opt_default_cmd[0] != '\0')
                                   ? opt_default_cmd
                                   : last_session;
    if (target_sess) {
        for (guint i = 0; i < app.sessions->len; i++) {
            GreeterSession *s = g_ptr_array_index(app.sessions, i);
            if (g_strcmp0(s->id, target_sess) == 0 ||
                g_strcmp0(s->exec, target_sess) == 0) {
                app.selected_session_idx = i;
                break;
            }
        }
    }

    app.gtk_app = gtk_application_new("org.kajo.greeter", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app.gtk_app, "activate", G_CALLBACK(on_activate), &app);

    int status = g_application_run(G_APPLICATION(app.gtk_app), argc, argv);

    if (app.clock_timer_id > 0) {
        g_source_remove(app.clock_timer_id);
        app.clock_timer_id = 0;
    }

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GListModel *monitors = gdk_display_get_monitors(display);
        if (monitors)
            g_signal_handlers_disconnect_by_func(monitors, on_monitors_changed, &app);
    }

    g_cancellable_cancel(app.cancellable);
    g_clear_object(&app.cancellable);
    g_clear_object(&app.system_bus);
    g_ptr_array_free(app.surfaces, TRUE);
    g_clear_object(&app.gtk_app);
    g_ptr_array_free(app.users, TRUE);
    g_ptr_array_free(app.sessions, TRUE);
    shell_theme_destroy(app.theme);
    shell_config_destroy(app.config);
    shell_icons_cleanup();
    g_free(opt_default_cmd);
    g_free(opt_default_user);

    return status;
}
