#ifndef KAJO_IDE_DAP_H
#define KAJO_IDE_DAP_H

#include "ide_buffer.h"
#include "ide_dap_java.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _IdeDapView IdeDapView;

typedef void (*IdeDapActionCb)(const char *dap_cmd, gpointer user_data);

/* Breakpoints of one source file (1-based lines). */
typedef struct {
  char   *path;
  GArray *lines; /* guint */
} IdeDapBreakpointSet;

void ide_dap_breakpoint_set_free(gpointer data);

/* Returns a GPtrArray<IdeDapBreakpointSet*> (free func set) of every open buffer
 * that has a path; the view takes ownership of the array. */
typedef GPtrArray *(*IdeDapBreakpointsFn)(gpointer user_data);

/* Runs a jdtls `workspace/executeCommand` for the Java debugger on the language
 * server attached to `file_path` (arguments ownership transferred). */
typedef gboolean (*IdeDapJavaExecFn)(const char   *file_path,
                                     const char   *command,
                                     JsonNode     *arguments,
                                     IdeJavaExecCb cb,
                                     gpointer      cb_data,
                                     gpointer      user_data);

/* The debuggee stopped in (or the user selected) a frame. path == NULL clears
 * the execution marker. */
typedef void (*IdeDapFrameCb)(const char *path, guint line_1based, gpointer user_data);

/* The session became active (starting / running / paused) or ended. */
typedef void (*IdeDapStateCb)(gboolean active, gpointer user_data);

IdeDapView *ide_dap_view_new(IdeDapActionCb action_cb, gpointer user_data);
void        ide_dap_view_free(IdeDapView *dap);

GtkWidget  *ide_dap_view_get_widget(IdeDapView *dap);
GtkWidget  *ide_dap_view_get_repl_entry(IdeDapView *dap);

void        ide_dap_view_set_context(IdeDapView         *dap,
                                     const char         *workspace_root,
                                     IdeDapBreakpointsFn breakpoints_fn,
                                     IdeDapFrameCb       frame_cb,
                                     IdeDapStateCb       state_cb,
                                     gpointer            user_data);
void        ide_dap_view_set_java_exec(IdeDapView *dap, IdeDapJavaExecFn fn, gpointer user_data);

/* A breakpoint was toggled: re-send them to a live session. */
void        ide_dap_view_breakpoints_changed(IdeDapView *dap);

void        ide_dap_view_start_session(IdeDapView *dap, IdeBuffer *active_buf);
void        ide_dap_view_continue(IdeDapView *dap, IdeBuffer *active_buf);
void        ide_dap_view_step_over(IdeDapView *dap, IdeBuffer *active_buf);
void        ide_dap_view_step_into(IdeDapView *dap, IdeBuffer *active_buf);
void        ide_dap_view_step_out(IdeDapView *dap, IdeBuffer *active_buf);
void        ide_dap_view_pause(IdeDapView *dap);
void        ide_dap_view_stop(IdeDapView *dap, IdeBuffer *active_buf);

gboolean    ide_dap_view_is_running(const IdeDapView *dap);

G_END_DECLS

#endif /* KAJO_IDE_DAP_H */
