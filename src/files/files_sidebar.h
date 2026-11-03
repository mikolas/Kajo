#ifndef KAJO_FILES_SIDEBAR_H
#define KAJO_FILES_SIDEBAR_H

#include <gtk/gtk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef void (*KajoSidebarNavigateFunc)(const char *uri, gpointer user_data);
typedef void (*KajoSidebarConnectServerFunc)(gpointer user_data);
typedef void (*KajoSidebarStatusFunc)(const char *status_msg, gboolean is_error, gpointer user_data);

typedef struct _KajoFilesSidebar KajoFilesSidebar;

KajoFilesSidebar *kajo_files_sidebar_new(GtkListBox *list_box,
                                         GtkWidget *auth_banner_revealer,
                                         KajoSidebarNavigateFunc nav_cb,
                                         KajoSidebarConnectServerFunc connect_cb,
                                         KajoSidebarStatusFunc status_cb,
                                         gpointer user_data);

void kajo_files_sidebar_free(KajoFilesSidebar *self);

void kajo_files_sidebar_refresh(KajoFilesSidebar *self);
void kajo_files_sidebar_sync_active_uri(KajoFilesSidebar *self, const char *active_uri);

/* Mounts an unmounted GVFS URI (smb://, sftp://, etc.) using the inline Auth Banner */
void kajo_files_sidebar_mount_uri(KajoFilesSidebar *self, GFile *target_file);

G_END_DECLS

#endif /* KAJO_FILES_SIDEBAR_H */
