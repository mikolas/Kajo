#ifndef KAJO_SPAWN_DETACHED_H
#define KAJO_SPAWN_DETACHED_H

#include <glib.h>

G_BEGIN_DECLS

/* Spawns argv asynchronously in a new session (setsid, clean signal mask,
 * SIGPIPE = SIG_DFL) and, when a user systemd instance is active (and
 * KAJO_SPAWN_NO_SYSTEMD is unset), isolates it in a transient app.slice scope
 * via systemd-run. This prevents restarting kajo or closing the terminal that
 * launched kajo from terminating spawned apps such as kitty.
 *
 * Returns FALSE immediately with G_SPAWN_ERROR_NOENT if argv[0] is missing or
 * not executable, preserving fallback behavior for callers.
 *
 * When child_pid_out is non-NULL, G_SPAWN_DO_NOT_REAP_CHILD is set and the
 * caller must reap *child_pid_out and call g_spawn_close_pid(). */
gboolean kajo_spawn_detached_argv(const gchar         *working_directory,
                                  const gchar *const  *argv,
                                  GPid                *child_pid_out,
                                  GError             **error);

/* Parses command_line via g_shell_parse_argv() and forwards to
 * kajo_spawn_detached_argv(). */
gboolean kajo_spawn_detached_command_line(const gchar  *command_line,
                                          GError      **error);

G_END_DECLS

#endif /* KAJO_SPAWN_DETACHED_H */
