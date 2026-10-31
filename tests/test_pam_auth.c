#include "../src/lock/pam_auth.h"
#include <glib.h>

typedef struct {
    GMainLoop *loop;
    gboolean   finished;
    gboolean   success;
    GError    *error;
} PamTestResult;

static void
on_pam_test_ready(GObject      *source_object G_GNUC_UNUSED,
                  GAsyncResult *res,
                  gpointer      user_data)
{
    PamTestResult *r = user_data;
    r->success = shell_pam_authenticate_finish(res, &r->error);
    r->finished = TRUE;
    g_main_loop_quit(r->loop);
}

static void
test_pam_empty_password_rejected(void)
{
    PamTestResult r = { 0 };
    r.loop = g_main_loop_new(NULL, FALSE);

    shell_pam_authenticate_async("", NULL, on_pam_test_ready, &r);
    g_main_loop_run(r.loop);

    g_assert_true(r.finished);
    g_assert_false(r.success);
    g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);

    g_clear_error(&r.error);
    g_main_loop_unref(r.loop);
}

static void
test_pam_null_password_rejected(void)
{
    PamTestResult r = { 0 };
    r.loop = g_main_loop_new(NULL, FALSE);

    shell_pam_authenticate_async(NULL, NULL, on_pam_test_ready, &r);
    g_main_loop_run(r.loop);

    g_assert_true(r.finished);
    g_assert_false(r.success);
    g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);

    g_clear_error(&r.error);
    g_main_loop_unref(r.loop);
}

static void
test_pam_cancelled_request(void)
{
    PamTestResult r = { 0 };
    r.loop = g_main_loop_new(NULL, FALSE);
    GCancellable *cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);

    shell_pam_authenticate_async("test-password", cancellable, on_pam_test_ready, &r);
    g_main_loop_run(r.loop);

    g_assert_true(r.finished);
    g_assert_false(r.success);
    g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

    g_clear_error(&r.error);
    g_object_unref(cancellable);
    g_main_loop_unref(r.loop);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/pam/empty-password-rejected", test_pam_empty_password_rejected);
    g_test_add_func("/pam/null-password-rejected", test_pam_null_password_rejected);
    g_test_add_func("/pam/cancelled-request", test_pam_cancelled_request);

    return g_test_run();
}
