#ifndef KAJO_GREETD_IPC_H
#define KAJO_GREETD_IPC_H

#include <gio/gio.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GREETD_RESPONSE_SUCCESS = 0,
    GREETD_RESPONSE_ERROR,
    GREETD_RESPONSE_AUTH_MESSAGE,
} GreetdResponseType;

typedef enum {
    GREETD_AUTH_MESSAGE_VISIBLE = 0,
    GREETD_AUTH_MESSAGE_SECRET,
    GREETD_AUTH_MESSAGE_INFO,
    GREETD_AUTH_MESSAGE_ERROR,
} GreetdAuthMessageType;

typedef enum {
    GREETD_ERROR_AUTH = 0,
    GREETD_ERROR_OTHER,
} GreetdErrorType;

typedef struct {
    GreetdResponseType    type;
    GreetdAuthMessageType auth_message_type;
    gchar                *auth_message;
    GreetdErrorType       error_type;
    gchar                *error_description;
} GreetdResponse;

void            greetd_response_free(GreetdResponse *resp);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GreetdResponse, greetd_response_free)

/* High-level asynchronous authentication + session start workflow */
void            greetd_authenticate_and_start_async(const gchar        *socket_path,
                                                    const gchar        *username,
                                                    const gchar        *password,
                                                    const gchar *const *cmd,
                                                    const gchar *const *env,
                                                    GCancellable       *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer            user_data);

gboolean        greetd_authenticate_and_start_finish(GAsyncResult *result,
                                                     GError      **error);

#ifdef __cplusplus
}
#endif

#endif /* KAJO_GREETD_IPC_H */
