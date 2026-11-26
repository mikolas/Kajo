#ifndef KAJO_AMIGA_BUILD_H
#define KAJO_AMIGA_BUILD_H

/* Pure GTK-free Option D Hybrid Build DAG:
 * - Parses vasmm68k_mot (-dependall=make -depfile) & gcc (-MMD -MF) .d depfiles
 * - Plans ordered incremental compile / link / elf2hunk / DH0: or adflib packaging steps
 * - Parses compiler/assembler/linker diagnostics cleanly
 * - Runs planned subprocesses asynchronously without blocking GTK main loop */

#include "amiga_project.h"
#include <gio/gio.h>
#include <stdbool.h>
#include <stddef.h>

G_BEGIN_DECLS

typedef enum {
  AMIGA_STEP_SHELL_CMD = 0,
  AMIGA_STEP_STAGE_DH0,
  AMIGA_STEP_PACK_ADF_DOS,
  AMIGA_STEP_PACK_ADF_RAW,
} AmigaBuildStepKind;

typedef struct {
  AmigaBuildStepKind  kind;
  char               *label;
  char               *cwd;
  GPtrArray          *argv; /* char* for GSubprocess */
  bool                skipped_up_to_date;
  char               *out_artifact;
} AmigaBuildStep;

typedef enum {
  AMIGA_DIAG_INFO = 0,
  AMIGA_DIAG_WARNING,
  AMIGA_DIAG_ERROR,
} AmigaBuildDiagSeverity;

typedef struct {
  char                   *file;
  guint                   line;
  guint                   col;
  AmigaBuildDiagSeverity  severity;
  char                   *message;
} AmigaBuildDiag;

typedef struct {
  bool       success;
  char      *log_text;
  GPtrArray *diagnostics; /* AmigaBuildDiag* */
  char      *artifact_path;
} AmigaBuildResult;

void amiga_build_step_free(AmigaBuildStep *s);
void amiga_build_diag_free(AmigaBuildDiag *d);
void amiga_build_result_free(AmigaBuildResult *r);

bool amiga_build_parse_depfile(const char  *dep_text,
                               GPtrArray  **out_deps);

bool amiga_build_needs_rebuild(const char        *output_path,
                               const char *const *inputs,
                               size_t             n_inputs);

bool amiga_build_plan_steps(const AmigaProject         *proj,
                            const AmigaTarget          *target,
                            const AmigaToolchainStatus *tc,
                            const char                 *staging_root,
                            GPtrArray                 **out_steps,
                            GError                    **error);

bool amiga_build_parse_diagnostic(const char     *line,
                                  AmigaBuildDiag *out_diag);

bool amiga_build_stage_dh0(const AmigaProject *proj,
                           const AmigaTarget  *target,
                           const char         *exe_path,
                           const char         *dh0_dir,
                           GError            **error);

void amiga_build_run_async(const AmigaProject         *proj,
                           const AmigaTarget          *target,
                           const AmigaToolchainStatus *tc,
                           const char                 *staging_root,
                           GCancellable               *cancellable,
                           GAsyncReadyCallback         callback,
                           gpointer                    user_data);

AmigaBuildResult *amiga_build_run_finish(GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* KAJO_AMIGA_BUILD_H */
