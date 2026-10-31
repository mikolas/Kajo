#ifndef KAJO_GREETER_USERS_H
#define KAJO_GREETER_USERS_H

#include <glib.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gchar *username;
    gchar *real_name;
    gchar *home_dir;
    uid_t  uid;
} GreeterUser;

typedef struct {
    gchar  *id;
    gchar  *name;
    gchar  *comment;
    gchar  *exec;
    gchar  *type;          /* "wayland" or "x11" */
    gchar  *desktop_names;
    gchar **cmd_argv;
    gchar **env_vars;
} GreeterSession;

void       greeter_user_free(gpointer data);
void       greeter_session_free(gpointer data);

/* Returns GPtrArray of GreeterUser* */
GPtrArray *greeter_discover_users(void);

/* Returns GPtrArray of GreeterSession* (optionally scanning custom dirs if custom_dirs != NULL) */
GPtrArray *greeter_discover_sessions_from_dirs(const gchar *const *search_dirs,
                                               const gchar        *default_cmd_override);

GPtrArray *greeter_discover_sessions(const gchar *default_cmd_override);

/* Persists/loads last selected username & session id */
void       greeter_state_load(const gchar *custom_path,
                              gchar      **out_last_user,
                              gchar      **out_last_session);

gboolean   greeter_state_save(const gchar *custom_path,
                              const gchar *username,
                              const gchar *session_id);

#ifdef __cplusplus
}
#endif

#endif /* KAJO_GREETER_USERS_H */
