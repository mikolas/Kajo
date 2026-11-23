#ifndef KAJO_FILES_DND_H
#define KAJO_FILES_DND_H

#include <gtk/gtk.h>
#include <gio/gio.h>
#include "files_model.h"

G_BEGIN_DECLS

typedef GPtrArray *(*KajoDndGetDragFilesFunc)(KajoFileItem *anchor_item, gpointer user_data);
typedef gboolean   (*KajoDndExecuteDropFunc)(GPtrArray     *files,
                                             GFile         *dest_dir,
                                             GdkDragAction  action,
                                             gpointer       user_data);
typedef GFile     *(*KajoDndGetCurrentDirFunc)(gpointer user_data);

/* ─── Pure DnD validation & action selection helpers (unit-tested) ─── */

/* Returns TRUE if dest_dir accepts file drops (file://, smb://, sftp://, trash:///;
 * rejects archive://, recent://, network://, and NULL). */
gboolean      kajo_files_dnd_dest_is_writable(GFile *dest_dir);

/* Validates whether every source in @sources can be dropped into @dest_dir.
 * Sets *out_is_trash = TRUE if @dest_dir is trash:///. Rejects self-drops,
 * ancestor-into-descendant drops, archive:// sources, and trash:// sources. */
gboolean      kajo_files_dnd_can_drop_files(GPtrArray *sources,
                                            GFile     *dest_dir,
                                            gboolean  *out_is_trash);

/* Computes the effective GdkDragAction (COPY, MOVE, or 0 for no-op).
 * Same-filesystem defaults to MOVE (unless all sources already reside in dest_dir,
 * where MOVE is a no-op and explicit COPY duplicates). Cross-filesystem defaults to COPY. */
GdkDragAction kajo_files_dnd_choose_action(GPtrArray     *sources,
                                           GFile         *dest_dir,
                                           GdkDragAction  preferred_action);

/* Extracts a GPtrArray of owned GFile* from a GDK_TYPE_FILE_LIST or text/uri-list GValue. */
GPtrArray    *kajo_files_dnd_extract_files_from_value(const GValue *value);

/* Returns TRUE if @item represents a transferable source (not archive/network/recent). */
gboolean      kajo_files_dnd_item_is_draggable(const KajoFileItem *item);

/* ─── GTK4 DragSource / DropTarget attachment helpers ─── */

/* Attaches an outbound GtkDragSource and (for folder items) an inbound GtkDropTarget
 * to a GridView tile or ListView row widget storing "file-item" object data. */
void kajo_files_dnd_attach_item_controllers(GtkWidget               *item_widget,
                                            KajoDndGetDragFilesFunc  get_drag_cb,
                                            KajoDndExecuteDropFunc   drop_cb,
                                            gpointer                 user_data);

/* Attaches an inbound GtkDropTarget to a directory view widget (drops into current_dir). */
void kajo_files_dnd_attach_view_drop_target(GtkWidget                *view_widget,
                                            KajoDndGetCurrentDirFunc  get_dir_cb,
                                            KajoDndExecuteDropFunc    drop_cb,
                                            gpointer                  user_data);

/* Attaches an inbound GtkDropTarget targeting a fixed URI (breadcrumb segment or sidebar row). */
void kajo_files_dnd_attach_uri_drop_target(GtkWidget              *target_widget,
                                           const char             *dest_uri,
                                           KajoDndExecuteDropFunc  drop_cb,
                                           gpointer                user_data);

G_END_DECLS

#endif /* KAJO_FILES_DND_H */
