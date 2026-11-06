#ifndef KAJO_IDE_DAP_H
#define KAJO_IDE_DAP_H

#include "ide_buffer.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _IdeDapView IdeDapView;

typedef void (*IdeDapActionCb)(const char *dap_cmd, gpointer user_data);

/* Pure testable DAP adapter binary detector */
const char *ide_dap_detect_adapter_for_file(const char *file_path);

IdeDapView *ide_dap_view_new(IdeDapActionCb action_cb, gpointer user_data);
void        ide_dap_view_free(IdeDapView *dap);

GtkWidget  *ide_dap_view_get_widget(IdeDapView *dap);

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
