#ifndef KAJO_IDE_TASKS_H
#define KAJO_IDE_TASKS_H

/* Project tasks (GTK-free): detects the build system that owns a file and
 * produces the shell command for build / test / run / clean. */

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  IDE_TASK_BUILD = 0,
  IDE_TASK_TEST,
  IDE_TASK_RUN,
  IDE_TASK_CLEAN,
} IdeTaskKind;

typedef struct {
  char *system;   /* "maven", "gradle", "meson", "cmake", "cargo", "go", "make", "java", "none" */
  char *label;    /* "Maven: build" */
  char *command;  /* shell command line (run with `sh -c` in `cwd`) */
  char *cwd;      /* project root the command runs in */
} IdeTask;

/* `active_file` (may be NULL) selects the project: the nearest ancestor holding
 * a build file, never above `workspace_root`. Returns NULL when nothing can be
 * done for this combination (e.g. RUN in a Meson project). */
IdeTask *ide_task_resolve(const char *workspace_root, const char *active_file, IdeTaskKind kind);
void     ide_task_free(IdeTask *task);

/* Fully qualified main class of a Java source file ("demo.App"), derived from its
 * `package` statement and file name. NULL if `path` is not a .java file. */
char    *ide_java_main_class(const char *java_path);

G_END_DECLS

#endif
