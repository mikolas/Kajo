#ifndef KAJO_FILES_OPS_H
#define KAJO_FILES_OPS_H

#include <gtk/gtk.h>
#include <gio/gio.h>
#include "files_model.h"

G_BEGIN_DECLS

typedef void (*KajoFileOpCallback)(gboolean success,
                                   const char *message,
                                   GError *error,
                                   gpointer user_data);

void kajo_files_op_mkdir_async(GFile *parent_dir,
                               const char *folder_name,
                               KajoFileOpCallback callback,
                               gpointer user_data);

void kajo_files_op_rename_async(GFile *file,
                                const char *new_name,
                                KajoFileOpCallback callback,
                                gpointer user_data);

void kajo_files_op_trash_async(GPtrArray *files,
                               KajoFileOpCallback callback,
                               gpointer user_data);

void kajo_files_op_restore_from_trash_async(KajoFileItem *item,
                                            KajoFileOpCallback callback,
                                            gpointer user_data);

void kajo_files_op_empty_trash_async(KajoFileOpCallback callback,
                                     gpointer user_data);

void kajo_files_op_delete_permanent_async(GPtrArray *files,
                                          KajoFileOpCallback callback,
                                          gpointer user_data);

void kajo_files_op_transfer_async(GPtrArray *source_files,
                                  GFile *dest_dir,
                                  gboolean is_move,
                                  KajoFileOpCallback callback,
                                  gpointer user_data);

/* Clipboard (Internal + Wayland GdkClipboard) */
void     kajo_files_clipboard_set(GPtrArray *files, gboolean is_cut);
gboolean kajo_files_clipboard_has_files(void);
void     kajo_files_clipboard_paste_async(GFile *dest_dir,
                                          KajoFileOpCallback callback,
                                          gpointer user_data);

/* Desktop Launchers */
void kajo_files_open_item_default(KajoFileItem *item, GtkWindow *parent);
void kajo_files_open_terminal_at(GFile *dir);

G_END_DECLS

#endif /* KAJO_FILES_OPS_H */
