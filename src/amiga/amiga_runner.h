#ifndef KAJO_AMIGA_RUNNER_H
#define KAJO_AMIGA_RUNNER_H

/* Pure GTK-free async Amiberry >= 8.5.0 process launcher:
 * - Translates [[profile]] + packaging kind (DH0: dir or .adf) into argv
 * - Enables -G -s debugging_features=segtracker,gdbserver -s gdb_port=... -s cpu_threaded=false
 * - Enables -s uaeboard=1 on floppy boots when Trap 86 guest-output is active
 * - Streams stdout/stderr asynchronously with GCancellable & GWeakRef lifetimes */

#include "amiga_project.h"
#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

#define AMIGA_TYPE_RUNNER (amiga_runner_get_type())
G_DECLARE_FINAL_TYPE(AmigaRunner, amiga_runner, AMIGA, RUNNER, GObject)

typedef struct {
  const char          *amiberry_bin;
  const AmigaHwProfile *profile;
  AmigaTargetKind      target_kind;
  const char          *target_name;
  const char          *dh0_dir;
  const char          *adf_path;
  uint16_t             gdb_port;
  bool                 uaeboard_guest_output;
  const char          *kickstart_override;
} AmigaRunnerLaunchSpec;

typedef void (*AmigaRunnerLogCb)(AmigaRunner *runner,
                                 const char  *line,
                                 bool         is_stderr,
                                 gpointer     user_data);

typedef void (*AmigaRunnerExitCb)(AmigaRunner *runner,
                                  int          exit_status,
                                  gpointer     user_data);

bool amiga_runner_build_argv(const AmigaRunnerLaunchSpec *spec,
                             GPtrArray                  **out_argv,
                             GError                     **error);

AmigaRunner *amiga_runner_new(void);
void         amiga_runner_set_callbacks(AmigaRunner       *runner,
                                        AmigaRunnerLogCb   log_cb,
                                        AmigaRunnerExitCb  exit_cb,
                                        gpointer           user_data);

bool         amiga_runner_start_argv(AmigaRunner        *runner,
                                     const char *const  *argv,
                                     const char         *cwd,
                                     uint16_t            gdb_port,
                                     bool                owns_process,
                                     GError            **error);
bool         amiga_runner_start(AmigaRunner                 *runner,
                                const AmigaRunnerLaunchSpec *spec,
                                const char                  *cwd,
                                GError                     **error);
void         amiga_runner_stop(AmigaRunner *runner);

bool         amiga_runner_is_running(AmigaRunner *runner);
bool         amiga_runner_owns_process(AmigaRunner *runner);
uint16_t     amiga_runner_get_gdb_port(AmigaRunner *runner);

G_END_DECLS

#endif /* KAJO_AMIGA_RUNNER_H */
