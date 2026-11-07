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
                               GCancellable *cancellable,
                               KajoFileOpCallback callback,
                               gpointer user_data);

void kajo_files_op_create_file_async(GFile *parent_dir,
                                     const char *file_name,
                                     GCancellable *cancellable,
                                     KajoFileOpCallback callback,
                                     gpointer user_data);

void kajo_files_op_rename_async(GFile *file,
                                const char *new_name,
                                GCancellable *cancellable,
                                KajoFileOpCallback callback,
                                gpointer user_data);

void kajo_files_op_trash_async(GPtrArray *files,
                               GCancellable *cancellable,
                               KajoFileOpCallback callback,
                               gpointer user_data);

void kajo_files_op_restore_from_trash_async(GPtrArray *items,
                                            GCancellable *cancellable,
                                            KajoFileOpCallback callback,
                                            gpointer user_data);

void kajo_files_op_empty_trash_async(GCancellable *cancellable,
                                     KajoFileOpCallback callback,
                                     gpointer user_data);

void kajo_files_op_delete_permanent_async(GPtrArray *files,
                                          GCancellable *cancellable,
                                          KajoFileOpCallback callback,
                                          gpointer user_data);

void kajo_files_op_transfer_async(GPtrArray *source_files,
                                  GFile *dest_dir,
                                  gboolean is_move,
                                  GCancellable *cancellable,
                                  KajoFileOpCallback callback,
                                  gpointer user_data);

void kajo_files_op_extract_archive_async(GFile *archive_file,
                                         GFile *dest_parent_dir,
                                         GCancellable *cancellable,
                                         KajoFileOpCallback callback,
                                         gpointer user_data);

/* Clipboard (Wayland GdkClipboard) */
void     kajo_files_clipboard_set(GPtrArray *files, gboolean is_cut);
gboolean kajo_files_clipboard_has_files(void);
gboolean kajo_files_parse_clipboard_data(const char *mime_type,
                                         const char *raw_data,
                                         gsize raw_len,
                                         GPtrArray **out_files,
                                         gboolean *out_is_cut);
void     kajo_files_clipboard_paste_async(GFile *dest_dir,
                                          GCancellable *cancellable,
                                          KajoFileOpCallback callback,
                                          gpointer user_data);
char    *kajo_files_copy_path_to_clipboard(KajoFileItem *item);
char    *kajo_files_copy_items_paths_to_clipboard(GPtrArray *items);
gboolean kajo_files_toggle_executable(const char *local_path,
                                      guint32 *out_new_mode,
                                      GError **error);

/* Desktop Launchers */
void     kajo_files_open_item_default(KajoFileItem *item, GtkWindow *parent);
gboolean kajo_files_open_item_with_app(KajoFileItem *item,
                                       GAppInfo *app_info,
                                       gboolean set_default,
                                       GError **error);
void     kajo_files_open_terminal_at(GFile *dir);

G_END_DECLS

#endif /* KAJO_FILES_OPS_H */
