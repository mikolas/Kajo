#ifndef KAJO_IDE_DAP_LAUNCH_H
#define KAJO_IDE_DAP_LAUNCH_H

/* Decides which debug adapter to start and how to launch the debuggee for the
 * active file (GTK-free, testable). */

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct {
  char      *adapter_label;   /* "gdb -i=dap" */
  GStrv      adapter_argv;    /* process to spawn */
  char      *request;         /* "launch" */
  JsonNode  *arguments;       /* body of the launch request (owned) */
  char      *program;         /* debuggee executable / package, for display */
  char      *cwd;
  char      *error;           /* non-NULL: user-facing reason nothing can be launched */
  gboolean   java;            /* debug through jdtls (see ide_dap_java.h); no adapter process */
  gboolean   wants_stop_at_main; /* caller sets stopAtBeginningOfMainSubprogram when no breakpoints */
} IdeDapLaunch;

/* Resolution order: <workspace>/.kajo/debug.json, then per-language discovery
 * (Meson introspection / build dirs for C and C++, Cargo for Rust, Delve for
 * Go). `has_breakpoints` makes C/C++/Rust programs stop at main otherwise. */
IdeDapLaunch *ide_dap_launch_resolve(const char *workspace_root,
                                     const char *active_file,
                                     gboolean    has_breakpoints);
void          ide_dap_launch_free(IdeDapLaunch *launch);

/* Debug adapter chosen for a file ("gdb -i=dap", "dlv dap", ...), for the UI badge
 * and tests. */
const char   *ide_dap_detect_adapter_for_file(const char *file_path);

G_END_DECLS

#endif
