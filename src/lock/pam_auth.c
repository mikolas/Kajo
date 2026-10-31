#include "pam_auth.h"

#include <pwd.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    gchar *username;
    gchar *password;
} PamAuthData;

static void
secure_zero_memory(void *ptr, size_t len)
{
    if (!ptr || len == 0)
        return;
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

static void
pam_auth_data_free(gpointer data)
{
    PamAuthData *auth = data;
    if (!auth)
        return;
    if (auth->password) {
        secure_zero_memory(auth->password, strlen(auth->password));
        g_free(auth->password);
    }
    g_free(auth->username);
    g_free(auth);
}

static int
pam_conversation_cb(int                        num_msg,
                    const struct pam_message **msg,
                    struct pam_response      **resp,
                    void                      *appdata_ptr)
{
    const char *password = (const char *)appdata_ptr;

    if (num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG || !msg || !resp)
        return PAM_CONV_ERR;

    /* libpam frees the response array and each resp string via stdlib free() */
    struct pam_response *reply = calloc((size_t)num_msg, sizeof(struct pam_response));
    if (!reply)
        return PAM_BUF_ERR;

    for (int i = 0; i < num_msg; i++) {
        if (!msg[i]) {
            for (int j = 0; j < i; j++) {
                if (reply[j].resp) {
                    secure_zero_memory(reply[j].resp, strlen(reply[j].resp));
                    free(reply[j].resp);
                }
            }
            free(reply);
            return PAM_CONV_ERR;
        }

        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON: {
            const char *pw = password ? password : "";
            size_t len = strlen(pw);
            char *copy = malloc(len + 1);
            if (!copy) {
                for (int j = 0; j < i; j++) {
                    if (reply[j].resp) {
                        secure_zero_memory(reply[j].resp, strlen(reply[j].resp));
                        free(reply[j].resp);
                    }
                }
                free(reply);
                return PAM_BUF_ERR;
            }
            memcpy(copy, pw, len + 1);
            reply[i].resp = copy;
            reply[i].resp_retcode = 0;
            break;
        }
        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:
            reply[i].resp = NULL;
            reply[i].resp_retcode = 0;
            break;
        default:
            for (int j = 0; j < i; j++) {
                if (reply[j].resp) {
                    secure_zero_memory(reply[j].resp, strlen(reply[j].resp));
                    free(reply[j].resp);
                }
            }
            free(reply);
            return PAM_CONV_ERR;
        }
    }

    *resp = reply;
    return PAM_SUCCESS;
}

static int
try_pam_service(const char *service_name,
                const char *username,
                const char *password)
{
    struct pam_conv conv = {
        .conv = pam_conversation_cb,
        .appdata_ptr = (void *)password,
    };
    pam_handle_t *pamh = NULL;

    int ret = pam_start(service_name, username, &conv, &pamh);
    if (ret != PAM_SUCCESS || !pamh) {
        if (pamh)
            pam_end(pamh, ret);
        return ret;
    }

    ret = pam_authenticate(pamh, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK);
    if (ret == PAM_SUCCESS) {
        int acct_ret = pam_acct_mgmt(pamh, PAM_SILENT);
        /* Allow unlock even if password expiration warning is flagged so user is not locked out */
        if (acct_ret != PAM_SUCCESS &&
            acct_ret != PAM_NEW_AUTHTOK_REQD &&
            acct_ret != PAM_ACCT_EXPIRED &&
            acct_ret != PAM_IGNORE) {
            /* Some PAM services require root for pam_acct_mgmt; ignore PAM_SYSTEM_ERR/PAM_PERM_DENIED post-auth */
            if (acct_ret == PAM_USER_UNKNOWN)
                ret = acct_ret;
        }
    }

    pam_end(pamh, ret);
    return ret;
}

static void
pam_auth_thread_func(GTask        *task,
                     gpointer      source_object G_GNUC_UNUSED,
                     gpointer      task_data,
                     GCancellable *cancellable G_GNUC_UNUSED)
{
    PamAuthData *auth = task_data;

    if (g_task_return_error_if_cancelled(task))
        return;

    if (!auth || !auth->password || auth->password[0] == '\0') {
        g_task_return_new_error(task,
                                G_IO_ERROR,
                                G_IO_ERROR_PERMISSION_DENIED,
                                "Please enter your password");
        return;
    }

    const char *username = (auth->username && auth->username[0] != '\0') ? auth->username : NULL;
    if (!username) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_name && pw->pw_name[0] != '\0')
            username = pw->pw_name;
        else
            username = g_get_user_name();
    }

    if (!username || username[0] == '\0') {
        g_task_return_new_error(task,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Unable to resolve current session user");
        return;
    }

    static const char *const services[] = {
        "kajo",
        "vlock",
        "system-auth",
        "system-login",
        "login",
        NULL
    };

    int last_ret = PAM_AUTH_ERR;
    for (int i = 0; services[i] != NULL; i++) {
        g_autofree gchar *service_path = g_build_filename("/etc/pam.d", services[i], NULL);
        if (!g_file_test(service_path, G_FILE_TEST_EXISTS))
            continue;

        int ret = try_pam_service(services[i], username, auth->password);
        if (ret == PAM_SUCCESS) {
            g_task_return_boolean(task, TRUE);
            return;
        }

        last_ret = ret;
        /* If pam_unix definitively rejected the password on vlock/system-auth, stop unless it was a system error */
        if (ret == PAM_MAXTRIES)
            break;
    }

    if (last_ret == PAM_MAXTRIES) {
        g_task_return_new_error(task,
                                G_IO_ERROR,
                                G_IO_ERROR_PERMISSION_DENIED,
                                "Too many failed attempts — wait before retrying");
    } else {
        g_task_return_new_error(task,
                                G_IO_ERROR,
                                G_IO_ERROR_PERMISSION_DENIED,
                                "Authentication failed — try again");
    }
}

void
shell_pam_authenticate_user_async(const gchar        *username,
                                  const gchar        *password,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data)
{
    PamAuthData *auth = g_new0(PamAuthData, 1);
    auth->username = (username && username[0] != '\0') ? g_strdup(username) : NULL;
    auth->password = g_strdup(password ? password : "");

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_task_data(task, auth, pam_auth_data_free);
    g_task_run_in_thread(task, pam_auth_thread_func);
    g_object_unref(task);
}

void
shell_pam_authenticate_async(const gchar        *password,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data)
{
    shell_pam_authenticate_user_async(NULL, password, cancellable, callback, user_data);
}

gboolean
shell_pam_authenticate_finish(GAsyncResult *result,
                              GError      **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);
    return g_task_propagate_boolean(G_TASK(result), error);
}
