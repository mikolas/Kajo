#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include "web_window.h"
#include "web_picker.h"
#include "web_profiles.h"
#include "../config/kajo_config_store.h"
#include "../app_init.h"

typedef struct {
    GtkApplication        *app;
    ShellTheme            *theme;
    KajoWebProfileManager *pm;
    gchar                 *opt_profile;
    gboolean               opt_picker;
    gboolean               opt_private;
    gboolean               opt_no_hw_accel;
    GPtrArray             *initial_urls;
} KajoWebApp;

static KajoWebApp g_web_app = { 0 };

static void open_profile_window(const gchar *profile_id, gboolean is_private, const gchar *url, gpointer ud);
static void open_picker_window(gpointer ud);

static void
ensure_bootstrapped(KajoWebApp *wa)
{
    if (!wa->theme)
        wa->theme = kajo_app_init_theme();
    if (!wa->pm)
        wa->pm = web_profile_manager_new();
}

static GtkWindow *
find_window_for_profile(GtkApplication *app, const gchar *profile_id, gboolean is_private)
{
    if (is_private)
        return NULL;
    for (GList *l = gtk_application_get_windows(app); l; l = l->next) {
        GtkWindow *w = GTK_WINDOW(l->data);
        if (!kajo_web_window_is_private(w) &&
            g_strcmp0(kajo_web_window_get_profile_id(w), profile_id) == 0) {
            return w;
        }
    }
    return NULL;
}

static void
open_profile_window(const gchar *profile_id, gboolean is_private, const gchar *url, gpointer ud)
{
    KajoWebApp *wa = ud;
    ensure_bootstrapped(wa);

    g_autofree gchar *safe_id = web_profile_sanitize_id(profile_id ? profile_id : "default");
    if (!is_private) {
        KajoWebProfileRegistry *reg = web_profile_manager_get_registry(wa->pm);
        if (reg) {
            g_free(reg->last_profile_id);
            reg->last_profile_id = g_strdup(safe_id);
            web_profile_manager_save(wa->pm, NULL);
        }
    }

    GtkWindow *existing = find_window_for_profile(wa->app, safe_id, is_private);
    if (existing) {
        if (url && *url)
            kajo_web_window_open_uri(existing, url, TRUE);
        gtk_window_present(existing);
        return;
    }

    KajoWebWindowAppCallbacks cbs = {
        .on_open_profile = open_profile_window,
        .on_open_picker = open_picker_window,
    };
    const gchar *urls[1] = { url };
    GtkWindow *win = kajo_web_window_new(wa->app, wa->pm, safe_id, is_private,
                                         wa->opt_no_hw_accel,
                                         (url && *url) ? urls : NULL,
                                         (url && *url) ? 1 : 0,
                                         &cbs, wa);
    gtk_window_present(win);
}

static void
on_picker_selected(const gchar *profile_id, gboolean is_private, gpointer ud)
{
    open_profile_window(profile_id, is_private, NULL, ud);
}

static void
open_picker_window(gpointer ud)
{
    KajoWebApp *wa = ud;
    ensure_bootstrapped(wa);
    GtkWindow *picker = web_picker_window_new(wa->app, wa->pm, on_picker_selected, wa);
    gtk_window_present(picker);
}

static void
on_app_activate(GtkApplication *app, gpointer user_data)
{
    KajoWebApp *wa = user_data;
    wa->app = app;
    ensure_bootstrapped(wa);

    g_autoptr(KajoWebCfgRef) cfg = kajo_config_store_acquire_web(NULL);
    KajoWebProfileRegistry *reg = web_profile_manager_get_registry(wa->pm);

    if (wa->opt_picker) {
        open_picker_window(wa);
        return;
    }

    if (!wa->opt_profile && !wa->opt_private &&
        (!wa->initial_urls || wa->initial_urls->len == 0) &&
        reg && reg->profiles->len > 1 && (!cfg || cfg->show_picker_on_startup)) {
        open_picker_window(wa);
        return;
    }

    const gchar *pid = wa->opt_profile ? wa->opt_profile
                     : (reg && reg->last_profile_id ? reg->last_profile_id : "default");

    KajoWebWindowAppCallbacks cbs = {
        .on_open_profile = open_profile_window,
        .on_open_picker = open_picker_window,
    };
    GtkWindow *win = kajo_web_window_new(
        app, wa->pm, pid, wa->opt_private, wa->opt_no_hw_accel,
        (wa->initial_urls && wa->initial_urls->len > 0) ? (const gchar *const *)wa->initial_urls->pdata : NULL,
        wa->initial_urls ? (gint)wa->initial_urls->len : 0,
        &cbs, wa);
    gtk_window_present(win);
}

static void
on_app_shutdown(GtkApplication *app G_GNUC_UNUSED, gpointer user_data)
{
    KajoWebApp *wa = user_data;
    g_clear_pointer(&wa->pm, web_profile_manager_free);
    kajo_app_shutdown_theme(wa->theme);
    wa->theme = NULL;
}

int
main(int argc, char *argv[])
{
    g_web_app.initial_urls = g_ptr_array_new_with_free_func(g_free);

    int f_argc = 1;
    char **f_argv = g_new0(char *, argc + 1);
    f_argv[0] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--profile") == 0 && i + 1 < argc) {
            g_free(g_web_app.opt_profile);
            g_web_app.opt_profile = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--profile-picker") == 0) {
            g_web_app.opt_picker = TRUE;
        } else if (g_strcmp0(argv[i], "--private") == 0) {
            g_web_app.opt_private = TRUE;
        } else if (g_strcmp0(argv[i], "--no-hw-accel") == 0) {
            g_web_app.opt_no_hw_accel = TRUE;
        } else if (argv[i][0] != '-') {
            g_ptr_array_add(g_web_app.initial_urls, g_strdup(argv[i]));
        } else {
            f_argv[f_argc++] = argv[i];
        }
    }

    /* Check configured hardware_acceleration BEFORE WebKit initializes DMA-BUF */
    g_autoptr(KajoWebCfgRef) early_cfg = kajo_config_store_acquire_web(NULL);
    if (g_web_app.opt_no_hw_accel || (early_cfg && early_cfg->hardware_acceleration == KAJO_WEB_HW_NEVER)) {
        g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", TRUE);
    }

    g_set_prgname("org.kajo.Web");
    g_set_application_name("Kajo Web");

    g_autoptr(GtkApplication) app = gtk_application_new("org.kajo.Web", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), &g_web_app);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), &g_web_app);

    int status = g_application_run(G_APPLICATION(app), f_argc, f_argv);

    g_ptr_array_unref(g_web_app.initial_urls);
    g_free(g_web_app.opt_profile);
    g_free(f_argv);
    return status;
}
