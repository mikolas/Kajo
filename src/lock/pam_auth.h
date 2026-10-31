#ifndef SHELL_PAM_AUTH_H
#define SHELL_PAM_AUTH_H

#include <gio/gio.h>

#ifdef __cplusplus
extern "C" {
#endif

void     shell_pam_authenticate_user_async(const gchar        *username,
                                           const gchar        *password,
                                           GCancellable       *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer            user_data);

void     shell_pam_authenticate_async(const gchar        *password,
                                      GCancellable       *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer            user_data);

gboolean shell_pam_authenticate_finish(GAsyncResult *result,
                                       GError      **error);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_PAM_AUTH_H */
