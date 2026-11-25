#include <gio/gio.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#include "../src/portal/portal_settings.h"
#include "../src/files/files_portal.h"
#include "../src/theme_export.h"
#include "../src/app_init.h"
#include "../src/config/kajo_config_store.h"

/* ─── Helpers ─── */

static gboolean
quit_loop_cb(gpointer data)
{
    GMainLoop *loop = data;
    if (loop && g_main_loop_is_running(loop))
        g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

typedef struct {
    GVariant  *result;
    GError    *error;
    gboolean   done;
    GMainLoop *loop;
} SyncOnLoopCall;

static void
on_sync_on_loop_ready(GObject *src, GAsyncResult *res, gpointer user_data)
{
    SyncOnLoopCall *c = user_data;
    c->result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &c->error);
    c->done = TRUE;
    if (c->loop && g_main_loop_is_running(c->loop))
        g_main_loop_quit(c->loop);
}

static GVariant *
dbus_call_on_mainloop(GDBusConnection *conn,
                      const char      *bus_name,
                      const char      *object_path,
                      const char      *interface_name,
                      const char      *method_name,
                      GVariant        *parameters,
                      const GVariantType *reply_type,
                      GError         **error)
{
    SyncOnLoopCall c = { .loop = g_main_loop_new(NULL, FALSE) };
    g_dbus_connection_call(conn, bus_name, object_path, interface_name, method_name,
                           parameters, reply_type, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                           on_sync_on_loop_ready, &c);
    guint tid = g_timeout_add(5000, quit_loop_cb, c.loop);
    if (!c.done)
        g_main_loop_run(c.loop);
    g_source_remove(tid);
    g_main_loop_unref(c.loop);
    if (c.error)
        g_propagate_error(error, c.error);
    return c.result;
}

static void
drain_main_context(guint iterations)
{
    GMainContext *ctx = g_main_context_default();
    for (guint i = 0; i < iterations; i++) {
        while (g_main_context_iteration(ctx, FALSE)) {}
        g_usleep(5000);
    }
}

/* ─── 1. Settings Portal Backend (org.freedesktop.impl.portal.Settings) ─── */

static void
test_settings_pure_helpers(void)
{
    g_assert_true(kajo_portal_settings_namespace_matches("", "org.freedesktop.appearance"));
    g_assert_true(kajo_portal_settings_namespace_matches("*", "org.freedesktop.appearance"));
    g_assert_true(kajo_portal_settings_namespace_matches("org.freedesktop.*", "org.freedesktop.appearance"));
    g_assert_true(kajo_portal_settings_namespace_matches("org.freedesktop.appearance", "org.freedesktop.appearance"));
    g_assert_false(kajo_portal_settings_namespace_matches("org.gnome.*", "org.freedesktop.appearance"));
    g_assert_false(kajo_portal_settings_namespace_matches("org.freedesktop.appearance.extra", "org.freedesktop.appearance"));

    double r = 0, g = 0, b = 0;
    g_assert_true(kajo_portal_settings_parse_accent_rgb("#0078d4", &r, &g, &b));
    g_assert_cmpfloat_with_epsilon(r, 0.0, 1e-4);
    g_assert_cmpfloat_with_epsilon(g, 0x78 / 255.0, 1e-4);
    g_assert_cmpfloat_with_epsilon(b, 0xd4 / 255.0, 1e-4);

    g_assert_true(kajo_portal_settings_parse_accent_rgb("#f08", &r, &g, &b));
    g_assert_cmpfloat_with_epsilon(r, 1.0, 1e-4);
    g_assert_cmpfloat_with_epsilon(g, 0.0, 1e-4);
    g_assert_cmpfloat_with_epsilon(b, 0x88 / 255.0, 1e-4);

    g_assert_false(kajo_portal_settings_parse_accent_rgb("invalid", &r, &g, &b));
}

typedef struct {
    GPtrArray *changed_keys; /* char* "ns::key" */
    GMainLoop *loop;
} SettingsSignalCollector;

static void
on_setting_changed_signal(GDBusConnection *conn G_GNUC_UNUSED,
                          const char      *sender G_GNUC_UNUSED,
                          const char      *object_path G_GNUC_UNUSED,
                          const char      *interface_name G_GNUC_UNUSED,
                          const char      *signal_name G_GNUC_UNUSED,
                          GVariant        *parameters,
                          gpointer         user_data)
{
    SettingsSignalCollector *col = user_data;
    const char *ns = NULL;
    const char *key = NULL;
    g_autoptr(GVariant) val = NULL;
    g_variant_get(parameters, "(&s&sv)", &ns, &key, &val);
    g_ptr_array_add(col->changed_keys, g_strdup_printf("%s::%s", ns, key));
    if (col->changed_keys->len >= 2 && col->loop && g_main_loop_is_running(col->loop))
        g_main_loop_quit(col->loop);
}

static void
test_settings_portal_dbus(void)
{
    g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);

    g_autoptr(GError) err = NULL;
    const char *addr = g_test_dbus_get_bus_address(bus);
    g_assert_nonnull(addr);

    g_autoptr(GDBusConnection) server_conn = g_dbus_connection_new_for_address_sync(
        addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &err);
    g_assert_no_error(err);
    g_assert_nonnull(server_conn);

    g_autoptr(GDBusConnection) client_conn = g_dbus_connection_new_for_address_sync(
        addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &err);
    g_assert_no_error(err);
    g_assert_nonnull(client_conn);

    g_autoptr(KajoThemeCfg) theme = kajo_theme_cfg_new_default();
    g_free(theme->accent_color);
    theme->accent_color = g_strdup("#0078d4");
    theme->surface_contrast = 1.0;

    KajoPortalSettings *portal = kajo_portal_settings_new_for_connection(server_conn, theme);
    g_assert_nonnull(portal);

    const char *server_name = g_dbus_connection_get_unique_name(server_conn);
    g_assert_nonnull(server_name);

    /* ReadAll([]) returns org.freedesktop.appearance with all 4 keys */
    const char *empty_ns[] = { NULL };
    g_autoptr(GVariant) res_all = dbus_call_on_mainloop(
        client_conn, server_name,
        KAJO_PORTAL_SETTINGS_OBJECT_PATH,
        KAJO_PORTAL_SETTINGS_INTERFACE,
        "ReadAll",
        g_variant_new("(^as)", empty_ns),
        G_VARIANT_TYPE("(a{sa{sv}})"),
        &err);
    g_assert_no_error(err);
    g_assert_nonnull(res_all);

    g_autoptr(GVariant) dict = g_variant_get_child_value(res_all, 0);
    g_autoptr(GVariant) app_ns = g_variant_lookup_value(dict, "org.freedesktop.appearance", G_VARIANT_TYPE("a{sv}"));
    g_assert_nonnull(app_ns);

    guint32 cs = 99;
    g_assert_true(g_variant_lookup(app_ns, "color-scheme", "u", &cs));
    g_assert_cmpuint(cs, ==, 1);

    guint32 contrast = 99;
    g_assert_true(g_variant_lookup(app_ns, "contrast", "u", &contrast));
    g_assert_cmpuint(contrast, ==, 0);

    double ar = 0, ag = 0, ab = 0;
    g_assert_true(g_variant_lookup(app_ns, "accent-color", "(ddd)", &ar, &ag, &ab));
    g_assert_cmpfloat_with_epsilon(ab, 0xd4 / 255.0, 1e-4);

    /* ReadAll(["org.gnome.*"]) returns empty map */
    const char *gnome_ns[] = { "org.gnome.*", NULL };
    g_autoptr(GVariant) res_gnome = dbus_call_on_mainloop(
        client_conn, server_name,
        KAJO_PORTAL_SETTINGS_OBJECT_PATH,
        KAJO_PORTAL_SETTINGS_INTERFACE,
        "ReadAll",
        g_variant_new("(^as)", gnome_ns),
        G_VARIANT_TYPE("(a{sa{sv}})"),
        &err);
    g_assert_no_error(err);
    g_autoptr(GVariant) gnome_dict = g_variant_get_child_value(res_gnome, 0);
    g_assert_cmpuint(g_variant_n_children(gnome_dict), ==, 0);

    /* Read(single key) */
    g_autoptr(GVariant) res_one = dbus_call_on_mainloop(
        client_conn, server_name,
        KAJO_PORTAL_SETTINGS_OBJECT_PATH,
        KAJO_PORTAL_SETTINGS_INTERFACE,
        "Read",
        g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
        G_VARIANT_TYPE("(v)"),
        &err);
    g_assert_no_error(err);
    g_autoptr(GVariant) boxed = NULL;
    g_variant_get(res_one, "(v)", &boxed);
    g_assert_cmpuint(g_variant_get_uint32(boxed), ==, 1);

    /* Read(missing key) -> org.freedesktop.portal.Error.NotFound */
    g_autoptr(GVariant) res_miss = dbus_call_on_mainloop(
        client_conn, server_name,
        KAJO_PORTAL_SETTINGS_OBJECT_PATH,
        KAJO_PORTAL_SETTINGS_INTERFACE,
        "Read",
        g_variant_new("(ss)", "org.freedesktop.appearance", "nonexistent"),
        G_VARIANT_TYPE("(v)"),
        &err);
    g_assert_null(res_miss);
    g_assert_nonnull(err);
    g_autofree char *remote_err = g_dbus_error_get_remote_error(err);
    g_assert_cmpstr(remote_err, ==, "org.freedesktop.portal.Error.NotFound");
    g_clear_error(&err);

    /* Subscribe to SettingChanged and push accent + high contrast update */
    SettingsSignalCollector col = {
        .changed_keys = g_ptr_array_new_with_free_func(g_free),
        .loop = g_main_loop_new(NULL, FALSE),
    };
    guint sub_id = g_dbus_connection_signal_subscribe(
        client_conn, server_name,
        KAJO_PORTAL_SETTINGS_INTERFACE,
        "SettingChanged",
        KAJO_PORTAL_SETTINGS_OBJECT_PATH,
        NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_setting_changed_signal, &col, NULL);

    g_free(theme->accent_color);
    theme->accent_color = g_strdup("#e81123");
    theme->surface_contrast = 1.5;
    kajo_portal_settings_update_from_theme(portal, theme);

    guint timeout_id = g_timeout_add(3000, quit_loop_cb, col.loop);
    g_main_loop_run(col.loop);
    g_source_remove(timeout_id);
    g_dbus_connection_signal_unsubscribe(client_conn, sub_id);

    g_assert_cmpuint(col.changed_keys->len, ==, 2);
    gboolean saw_accent = FALSE, saw_contrast = FALSE;
    for (guint i = 0; i < col.changed_keys->len; i++) {
        const char *k = g_ptr_array_index(col.changed_keys, i);
        if (g_strcmp0(k, "org.freedesktop.appearance::accent-color") == 0)
            saw_accent = TRUE;
        if (g_strcmp0(k, "org.freedesktop.appearance::contrast") == 0)
            saw_contrast = TRUE;
    }
    g_assert_true(saw_accent);
    g_assert_true(saw_contrast);

    g_ptr_array_unref(col.changed_keys);
    g_main_loop_unref(col.loop);
    kajo_portal_settings_destroy(portal);
    g_test_dbus_down(bus);
}

/* ─── 2. FileChooser Portal (org.freedesktop.impl.portal.FileChooser + Request) ─── */

static void
test_filechooser_pure_helpers(void)
{
    g_autofree char *lbl = kajo_portal_strip_mnemonic("_Open File");
    g_assert_cmpstr(lbl, ==, "Open File");
    g_autofree char *lbl2 = kajo_portal_strip_mnemonic("Save__As");
    g_assert_cmpstr(lbl2, ==, "Save_As");

    /* Filter matching (glob case-insensitive, MIME wildcard, directories always pass) */
    g_autoptr(GFile) md_file = g_file_new_for_path("/tmp/README.MD");
    g_autoptr(KajoFileItem) md_item = kajo_file_item_new_manual(
        md_file, "README.MD", "README.MD", "text/markdown", 42, 0, 0644, FALSE, FALSE);

    g_autoptr(GFile) dir_file = g_file_new_for_path("/tmp/sub");
    g_autoptr(KajoFileItem) dir_item = kajo_file_item_new_manual(
        dir_file, "sub", "sub", "inode/directory", 0, 0, 0755, TRUE, FALSE);

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    GVariantBuilder filters;
    g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
    g_variant_builder_open(&filters, G_VARIANT_TYPE("(sa(us))"));
    g_variant_builder_add(&filters, "s", "Markdown");
    g_variant_builder_open(&filters, G_VARIANT_TYPE("a(us)"));
    g_variant_builder_add(&filters, "(us)", (guint32)0, "*.md");
    g_variant_builder_close(&filters);
    g_variant_builder_close(&filters);

    g_variant_builder_open(&filters, G_VARIANT_TYPE("(sa(us))"));
    g_variant_builder_add(&filters, "s", "Images");
    g_variant_builder_open(&filters, G_VARIANT_TYPE("a(us)"));
    g_variant_builder_add(&filters, "(us)", (guint32)1, "image/*");
    g_variant_builder_close(&filters);
    g_variant_builder_close(&filters);
    g_variant_builder_add(&opts, "{sv}", "filters", g_variant_builder_end(&filters));

    g_autoptr(GVariant) vopts = g_variant_ref_sink(g_variant_builder_end(&opts));
    KajoPortalChooserRequest *req = kajo_portal_parse_options(
        KAJO_PORTAL_MODE_OPEN_FILE, "org.example.App", "", "Pick", vopts);
    g_assert_nonnull(req);
    g_assert_cmpuint(req->filters->len, ==, 2);

    const KajoPortalFilter *f_md = g_ptr_array_index(req->filters, 0);
    const KajoPortalFilter *f_img = g_ptr_array_index(req->filters, 1);
    g_assert_true(kajo_portal_filter_matches_item(f_md, md_item));
    g_assert_false(kajo_portal_filter_matches_item(f_img, md_item));
    g_assert_true(kajo_portal_filter_matches_item(f_img, dir_item));
    kajo_portal_chooser_request_free(req);

    /* SaveFiles non-clobbering collision handling */
    g_autofree char *tmp = g_dir_make_tmp("kajo-savefiles-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree char *existing = g_build_filename(tmp, "report.txt", NULL);
    g_assert_true(g_file_set_contents(existing, "old", -1, NULL));

    g_autoptr(GFile) folder = g_file_new_for_path(tmp);
    g_autoptr(GPtrArray) names = g_ptr_array_new();
    g_ptr_array_add(names, (gpointer)"report.txt");
    g_ptr_array_add(names, (gpointer)"notes.md");

    g_autoptr(GPtrArray) uris = kajo_portal_build_save_files_uris(folder, names);
    g_assert_cmpuint(uris->len, ==, 2);
    g_assert_true(g_str_has_suffix(g_ptr_array_index(uris, 0), "/report%20(1).txt") ||
                  g_str_has_suffix(g_ptr_array_index(uris, 0), "/report (1).txt"));
    g_assert_true(g_str_has_suffix(g_ptr_array_index(uris, 1), "/notes.md"));

    g_unlink(existing);
    g_rmdir(tmp);
}

typedef struct {
    gboolean auto_accept;
    gboolean close_cb_invoked;
    KajoPortalActiveRequest *held_req;
    GMainLoop *presented_loop;
} MockChooserPresenter;

static void
mock_presenter_close_cb(KajoPortalActiveRequest *active_req G_GNUC_UNUSED,
                        gpointer user_data)
{
    MockChooserPresenter *p = user_data;
    p->close_cb_invoked = TRUE;
    p->held_req = NULL;
}

static void
mock_presenter_present(KajoPortalActiveRequest *active_req, gpointer user_data)
{
    MockChooserPresenter *p = user_data;
    const KajoPortalChooserRequest *spec = kajo_portal_active_request_get_spec(active_req);
    g_assert_nonnull(spec);

    kajo_portal_active_request_set_close_cb(active_req, mock_presenter_close_cb, p);

    if (p->auto_accept) {
        KajoPortalChooserResult *res = kajo_portal_chooser_result_new();
        g_ptr_array_add(res->uris, g_strdup("file:///tmp/chosen.txt"));
        g_ptr_array_add(res->choice_ids, g_strdup("encoding"));
        g_ptr_array_add(res->choice_values, g_strdup("utf8"));
        res->selected_filter_index = 0;
        res->writable = TRUE;
        kajo_portal_active_request_finish(active_req, 0, res);
    } else {
        p->held_req = active_req;
        if (p->presented_loop && g_main_loop_is_running(p->presented_loop))
            g_main_loop_quit(p->presented_loop);
    }
}

typedef struct {
    gboolean done;
    guint32 response;
    gchar *first_uri;
    gchar *choice_val;
    GMainLoop *loop;
} AsyncCallState;

static void
on_openfile_call_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    AsyncCallState *st = user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GVariant) ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    g_assert_no_error(err);
    g_assert_nonnull(ret);

    g_autoptr(GVariant) results = NULL;
    g_variant_get(ret, "(u@a{sv})", &st->response, &results);

    g_autoptr(GVariant) uris_v = g_variant_lookup_value(results, "uris", G_VARIANT_TYPE_STRING_ARRAY);
    if (uris_v && g_variant_n_children(uris_v) > 0) {
        const char *u = NULL;
        g_variant_get_child(uris_v, 0, "&s", &u);
        st->first_uri = g_strdup(u);
    }

    g_autoptr(GVariant) choices_v = g_variant_lookup_value(results, "choices", G_VARIANT_TYPE("a(ss)"));
    if (choices_v && g_variant_n_children(choices_v) > 0) {
        const char *cid = NULL, *cval = NULL;
        g_variant_get_child(choices_v, 0, "(&s&s)", &cid, &cval);
        if (g_strcmp0(cid, "encoding") == 0)
            st->choice_val = g_strdup(cval);
    }

    st->done = TRUE;
    if (st->loop && g_main_loop_is_running(st->loop))
        g_main_loop_quit(st->loop);
}

static void
test_filechooser_portal_dbus(void)
{
    g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);

    g_autoptr(GError) err = NULL;
    const char *addr = g_test_dbus_get_bus_address(bus);

    g_autoptr(GDBusConnection) server_conn = g_dbus_connection_new_for_address_sync(
        addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &err);
    g_assert_no_error(err);

    g_autoptr(GDBusConnection) client_conn = g_dbus_connection_new_for_address_sync(
        addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &err);
    g_assert_no_error(err);

    MockChooserPresenter presenter = { .auto_accept = TRUE };
    KajoFilesPortal *portal = kajo_files_portal_new_for_connection(
        server_conn, mock_presenter_present, &presenter);
    g_assert_nonnull(portal);

    const char *server_name = g_dbus_connection_get_unique_name(server_conn);

    /* Case A: OpenFile accept round-trip */
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "multiple", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&opts, "{sv}", "accept_label", g_variant_new_string("_Choose"));

    AsyncCallState st = { .loop = g_main_loop_new(NULL, FALSE) };
    g_dbus_connection_call(
        client_conn, server_name,
        KAJO_PORTAL_FILECHOOSER_OBJECT_PATH,
        KAJO_PORTAL_FILECHOOSER_INTERFACE,
        "OpenFile",
        g_variant_new("(osss@a{sv})",
                      "/org/freedesktop/portal/desktop/request/test/req1",
                      "org.example.App",
                      "",
                      "Open Document",
                      g_variant_builder_end(&opts)),
        G_VARIANT_TYPE("(ua{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
        on_openfile_call_done, &st);

    guint tid = g_timeout_add(3000, quit_loop_cb, st.loop);
    g_main_loop_run(st.loop);
    g_source_remove(tid);

    g_assert_true(st.done);
    g_assert_cmpuint(st.response, ==, 0);
    g_assert_cmpstr(st.first_uri, ==, "file:///tmp/chosen.txt");
    g_assert_cmpstr(st.choice_val, ==, "utf8");
    g_free(st.first_uri);
    g_free(st.choice_val);
    g_main_loop_unref(st.loop);

    /* Case B: Request.Close() cancels an active dialog cleanly */
    presenter.auto_accept = FALSE;
    presenter.close_cb_invoked = FALSE;
    presenter.held_req = NULL;
    presenter.presented_loop = g_main_loop_new(NULL, FALSE);

    const char *cancel_handle = "/org/freedesktop/portal/desktop/request/test/req2";
    GVariantBuilder opts2;
    g_variant_builder_init(&opts2, G_VARIANT_TYPE("a{sv}"));

    AsyncCallState st2 = { .loop = g_main_loop_new(NULL, FALSE) };
    g_dbus_connection_call(
        client_conn, server_name,
        KAJO_PORTAL_FILECHOOSER_OBJECT_PATH,
        KAJO_PORTAL_FILECHOOSER_INTERFACE,
        "SaveFile",
        g_variant_new("(osss@a{sv})",
                      cancel_handle,
                      "org.example.App",
                      "",
                      "Save Document",
                      g_variant_builder_end(&opts2)),
        G_VARIANT_TYPE("(ua{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
        on_openfile_call_done, &st2);

    /* Wait until presenter has received the request */
    guint tid_p = g_timeout_add(3000, quit_loop_cb, presenter.presented_loop);
    g_main_loop_run(presenter.presented_loop);
    g_source_remove(tid_p);
    g_assert_nonnull(presenter.held_req);

    /* Call Request.Close() on the request object path */
    g_autoptr(GVariant) close_res = dbus_call_on_mainloop(
        client_conn, server_name,
        cancel_handle,
        KAJO_PORTAL_REQUEST_INTERFACE,
        "Close",
        NULL, NULL,
        &err);
    g_assert_no_error(err);
    g_assert_nonnull(close_res);

    if (!st2.done) {
        guint tid2 = g_timeout_add(3000, quit_loop_cb, st2.loop);
        g_main_loop_run(st2.loop);
        g_source_remove(tid2);
    }

    g_assert_true(presenter.close_cb_invoked);
    g_assert_true(st2.done);
    g_assert_cmpuint(st2.response, ==, 2);
    g_assert_null(st2.first_uri);

    g_main_loop_unref(presenter.presented_loop);
    g_main_loop_unref(st2.loop);
    drain_main_context(3);

    kajo_files_portal_destroy(portal);
    g_test_dbus_down(bus);
}

/* ─── 3. External GTK/Qt Theme Export: Backup, Apply, Partial Disable, Full Revert ─── */

static void
rm_rf_temp(const char *path)
{
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return;
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *d = g_dir_open(path, 0, NULL);
        if (d) {
            const char *name;
            while ((name = g_dir_read_name(d)) != NULL) {
                g_autofree char *child = g_build_filename(path, name, NULL);
                rm_rf_temp(child);
            }
            g_dir_close(d);
        }
        g_rmdir(path);
    } else {
        g_unlink(path);
    }
}

static void
test_theme_export_backup_and_revert(void)
{
    g_assert_cmpstr(kajo_theme_export_nearest_gnome_accent("#0078d4"), ==, "blue");
    g_assert_cmpstr(kajo_theme_export_nearest_gnome_accent("#e81123"), ==, "red");
    g_assert_cmpstr(kajo_theme_export_nearest_gnome_accent("#107c10"), ==, "green");
    g_assert_cmpstr(kajo_theme_export_nearest_gnome_accent("#ffb900"), ==, "yellow");

    g_autofree char *tmp_cfg = g_dir_make_tmp("kajo-theme-export-XXXXXX", NULL);
    g_assert_nonnull(tmp_cfg);

    /* Seed exact pre-Kajo user config layout:
     * - gtk-3.0/settings.ini (existing) + dank-colors.css (unused leftover)
     * - gtk-4.0/gtk.css (existing user customization!) + dank-colors.css (unused leftover)
     * - qt6ct/qt6ct.conf (0-byte empty file) */
    g_autofree char *gtk3_dir = g_build_filename(tmp_cfg, "gtk-3.0", NULL);
    g_autofree char *gtk4_dir = g_build_filename(tmp_cfg, "gtk-4.0", NULL);
    g_autofree char *qt6_dir = g_build_filename(tmp_cfg, "qt6ct", NULL);
    g_assert_cmpint(g_mkdir_with_parents(gtk3_dir, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(gtk4_dir, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(qt6_dir, 0755), ==, 0);

    const char *orig_gtk3_ini =
        "[Settings]\n"
        "gtk-application-prefer-dark-theme=true\n"
        "gtk-cursor-theme-name=capitaine-cursors\n"
        "gtk-font-name=Noto Sans 10\n"
        "gtk-theme-name=adw-gtk3\n";
    const char *orig_gtk4_css = "/* my custom rule */\nwindow { letter-spacing: 0px; }\n";
    const char *orig_leftover = "/* old dank colors */\n";

    g_autofree char *gtk3_ini_path = g_build_filename(gtk3_dir, "settings.ini", NULL);
    g_autofree char *gtk3_css_path = g_build_filename(gtk3_dir, "gtk.css", NULL);
    g_autofree char *gtk3_kajo_css = g_build_filename(gtk3_dir, "kajo-theme.css", NULL);
    g_autofree char *gtk3_dank_path = g_build_filename(gtk3_dir, "dank-colors.css", NULL);

    g_autofree char *gtk4_ini_path = g_build_filename(gtk4_dir, "settings.ini", NULL);
    g_autofree char *gtk4_css_path = g_build_filename(gtk4_dir, "gtk.css", NULL);
    g_autofree char *gtk4_kajo_css = g_build_filename(gtk4_dir, "kajo-theme.css", NULL);
    g_autofree char *gtk4_dank_path = g_build_filename(gtk4_dir, "dank-colors.css", NULL);

    g_autofree char *qt6_conf_path = g_build_filename(qt6_dir, "qt6ct.conf", NULL);
    g_autofree char *qt6_color_path = g_build_filename(qt6_dir, "colors", "kajo-metro.conf", NULL);
    g_autofree char *env_qt_path = g_build_filename(tmp_cfg, "environment.d", "90-kajo-qt.conf", NULL);

    g_assert_true(g_file_set_contents(gtk3_ini_path, orig_gtk3_ini, -1, NULL));
    g_assert_true(g_file_set_contents(gtk3_dank_path, orig_leftover, -1, NULL));
    g_assert_true(g_file_set_contents(gtk4_css_path, orig_gtk4_css, -1, NULL));
    g_assert_true(g_file_set_contents(gtk4_dank_path, orig_leftover, -1, NULL));
    g_assert_true(g_file_set_contents(qt6_conf_path, "", 0, NULL));

    /* Leftover detection finds dank-colors.css */
    g_autofree char *leftovers = kajo_theme_export_detect_leftovers(tmp_cfg);
    g_assert_cmpstr(leftovers, ==, "dank-colors.css");
    g_assert_false(kajo_theme_export_has_backup(tmp_cfg));

    /* Step 1: Enable both GTK and Qt */
    g_autoptr(KajoThemeCfg) cfg = kajo_theme_cfg_new_default();
    g_free(cfg->accent_color);
    cfg->accent_color = g_strdup("#0078d4");
    cfg->theme_external_gtk = TRUE;
    cfg->theme_external_qt = TRUE;

    g_autoptr(GError) err = NULL;
    g_assert_true(kajo_theme_export_sync(cfg, tmp_cfg, FALSE, &err));
    g_assert_no_error(err);
    g_assert_true(kajo_theme_export_has_backup(tmp_cfg));

    /* Verify GTK4 & GTK3 files written and user's custom gtk-4.0/gtk.css preserved */
    g_assert_true(g_file_test(gtk4_kajo_css, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(gtk3_kajo_css, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(gtk3_css_path, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(gtk4_ini_path, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(qt6_color_path, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(env_qt_path, G_FILE_TEST_EXISTS));

    g_autofree char *gtk4_css_after = NULL;
    g_assert_true(g_file_get_contents(gtk4_css_path, &gtk4_css_after, NULL, NULL));
    g_assert_nonnull(strstr(gtk4_css_after, "/* my custom rule */"));
    g_assert_nonnull(strstr(gtk4_css_after, "@import url(\"kajo-theme.css\");"));

    g_autofree char *gtk4_kajo_txt = NULL;
    g_assert_true(g_file_get_contents(gtk4_kajo_css, &gtk4_kajo_txt, NULL, NULL));
    g_assert_nonnull(strstr(gtk4_kajo_txt, "#0078d4"));
    g_assert_nonnull(strstr(gtk4_kajo_txt, "border-radius: 0px"));

    g_autofree char *qt6_color_txt = NULL;
    g_assert_true(g_file_get_contents(qt6_color_path, &qt6_color_txt, NULL, NULL));
    g_assert_nonnull(strstr(qt6_color_txt, "#ff0078d4"));

    /* Step 2: Change accent color -> backup must NOT be overwritten, outputs update */
    g_autofree char *backup_path = g_build_filename(tmp_cfg, "kajo", "external-theme-backup.json", NULL);
    g_autofree char *backup_before = NULL;
    g_assert_true(g_file_get_contents(backup_path, &backup_before, NULL, NULL));

    g_free(cfg->accent_color);
    cfg->accent_color = g_strdup("#e81123");
    g_assert_true(kajo_theme_export_sync(cfg, tmp_cfg, FALSE, &err));
    g_assert_no_error(err);

    g_autofree char *backup_after = NULL;
    g_assert_true(g_file_get_contents(backup_path, &backup_after, NULL, NULL));
    g_assert_cmpstr(backup_before, ==, backup_after);

    g_autofree char *gtk4_kajo_txt2 = NULL;
    g_assert_true(g_file_get_contents(gtk4_kajo_css, &gtk4_kajo_txt2, NULL, NULL));
    g_assert_nonnull(strstr(gtk4_kajo_txt2, "#e81123"));

    /* Step 3: Disable GTK only -> GTK restored bit-for-bit, Qt stays active + backup remains */
    cfg->theme_external_gtk = FALSE;
    g_assert_true(kajo_theme_export_sync(cfg, tmp_cfg, FALSE, &err));
    g_assert_no_error(err);

    g_assert_false(g_file_test(gtk4_kajo_css, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(gtk3_kajo_css, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(gtk3_css_path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(gtk4_ini_path, G_FILE_TEST_EXISTS));

    g_autofree char *restored_gtk3_ini = NULL;
    g_assert_true(g_file_get_contents(gtk3_ini_path, &restored_gtk3_ini, NULL, NULL));
    g_assert_cmpstr(restored_gtk3_ini, ==, orig_gtk3_ini);

    g_autofree char *restored_gtk4_css = NULL;
    g_assert_true(g_file_get_contents(gtk4_css_path, &restored_gtk4_css, NULL, NULL));
    g_assert_cmpstr(restored_gtk4_css, ==, orig_gtk4_css);

    /* Qt still enabled & backup still present */
    g_assert_true(g_file_test(qt6_color_path, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(env_qt_path, G_FILE_TEST_EXISTS));
    g_assert_true(kajo_theme_export_has_backup(tmp_cfg));

    /* Step 4: Explicit Restore Original Defaults -> EVERYTHING back to exact pre-Kajo state */
    g_assert_true(kajo_theme_export_revert_all(tmp_cfg, FALSE, &err));
    g_assert_no_error(err);

    g_assert_false(kajo_theme_export_has_backup(tmp_cfg));
    g_assert_false(g_file_test(qt6_color_path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(env_qt_path, G_FILE_TEST_EXISTS));

    /* qt6ct.conf restored to exact 0-byte original */
    gsize qt6_len = 999;
    g_autofree char *qt6_restored = NULL;
    g_assert_true(g_file_get_contents(qt6_conf_path, &qt6_restored, &qt6_len, NULL));
    g_assert_cmpuint(qt6_len, ==, 0);

    /* dank-colors.css untouched */
    g_autofree char *dank_check = NULL;
    g_assert_true(g_file_get_contents(gtk3_dank_path, &dank_check, NULL, NULL));
    g_assert_cmpstr(dank_check, ==, orig_leftover);

    rm_rf_temp(tmp_cfg);
}

static void
test_disable_portal_client_env(void)
{
    g_autofree gchar *orig_gdk = g_strdup(g_getenv("GDK_DEBUG"));
    g_autofree gchar *orig_gio = g_strdup(g_getenv("GIO_USE_PORTALS"));
    g_autofree gchar *orig_gtk = g_strdup(g_getenv("GTK_USE_PORTAL"));

    g_unsetenv("GDK_DEBUG");
    g_unsetenv("GIO_USE_PORTALS");
    g_unsetenv("GTK_USE_PORTAL");

    kajo_app_disable_portal_client();
    g_assert_cmpstr(g_getenv("GDK_DEBUG"), ==, "no-portals");
    g_assert_cmpstr(g_getenv("GIO_USE_PORTALS"), ==, "0");
    g_assert_cmpstr(g_getenv("GTK_USE_PORTAL"), ==, "0");

    /* Token prefix collision check: "foo-no-portals" must still append ",no-portals" */
    g_setenv("GDK_DEBUG", "opengl,foo-no-portals", TRUE);
    kajo_app_disable_portal_client();
    g_assert_cmpstr(g_getenv("GDK_DEBUG"), ==, "opengl,foo-no-portals,no-portals");

    /* Calling again is idempotent */
    kajo_app_disable_portal_client();
    g_assert_cmpstr(g_getenv("GDK_DEBUG"), ==, "opengl,foo-no-portals,no-portals");

    if (orig_gdk) g_setenv("GDK_DEBUG", orig_gdk, TRUE);
    else g_unsetenv("GDK_DEBUG");
    if (orig_gio) g_setenv("GIO_USE_PORTALS", orig_gio, TRUE);
    else g_unsetenv("GIO_USE_PORTALS");
    if (orig_gtk) g_setenv("GTK_USE_PORTAL", orig_gtk, TRUE);
    else g_unsetenv("GTK_USE_PORTAL");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/portals/settings/pure-helpers", test_settings_pure_helpers);
    g_test_add_func("/portals/settings/dbus", test_settings_portal_dbus);
    g_test_add_func("/portals/filechooser/pure-helpers", test_filechooser_pure_helpers);
    g_test_add_func("/portals/filechooser/dbus", test_filechooser_portal_dbus);
    g_test_add_func("/portals/disable-portal-client-env", test_disable_portal_client_env);
    g_test_add_func("/theme/export/backup-and-revert", test_theme_export_backup_and_revert);
    return g_test_run();
}
