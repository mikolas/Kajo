#ifndef KAJO_FILES_MODEL_H
#define KAJO_FILES_MODEL_H

#include <gtk/gtk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
    KAJO_SORT_NAME = 0,
    KAJO_SORT_SIZE = 1,
    KAJO_SORT_MODIFIED = 2,
    KAJO_SORT_TYPE = 3,
} KajoFileSortField;

typedef enum {
    KAJO_VIEW_GRID = 0,
    KAJO_VIEW_LIST = 1,
} KajoFileViewMode;

#define KAJO_TYPE_FILE_ITEM (kajo_file_item_get_type())
G_DECLARE_FINAL_TYPE(KajoFileItem, kajo_file_item, KAJO, FILE_ITEM, GObject)

KajoFileItem *kajo_file_item_new_from_info(GFile *parent_dir, GFileInfo *info);

KajoFileItem *kajo_file_item_new_manual(GFile *file,
                                        const char *name,
                                        const char *display_name,
                                        const char *content_type,
                                        guint64 size,
                                        gint64 mtime_sec,
                                        guint32 unix_mode,
                                        gboolean is_dir,
                                        gboolean is_hidden);

GFile      *kajo_file_item_get_file(const KajoFileItem *self);
const char *kajo_file_item_get_uri(const KajoFileItem *self);
const char *kajo_file_item_get_target_uri(const KajoFileItem *self);
const char *kajo_file_item_get_name(const KajoFileItem *self);
const char *kajo_file_item_get_display_name(const KajoFileItem *self);
const char *kajo_file_item_get_content_type(const KajoFileItem *self);
const char *kajo_file_item_get_type_description(const KajoFileItem *self);
const char *kajo_file_item_get_icon_name(const KajoFileItem *self);
GIcon      *kajo_file_item_get_gicon(const KajoFileItem *self);
guint64     kajo_file_item_get_size(const KajoFileItem *self);
gint64      kajo_file_item_get_mtime(const KajoFileItem *self);
guint32     kajo_file_item_get_unix_mode(const KajoFileItem *self);
gboolean    kajo_file_item_is_dir(const KajoFileItem *self);
gboolean    kajo_file_item_is_hidden(const KajoFileItem *self);
gboolean    kajo_file_item_is_symlink(const KajoFileItem *self);
gboolean    kajo_file_item_is_mountable(const KajoFileItem *self);
const char *kajo_file_item_get_symlink_target(const KajoFileItem *self);
const char *kajo_file_item_get_trash_orig_path(const KajoFileItem *self);
const char *kajo_file_item_get_trash_deletion_date(const KajoFileItem *self);

/* Formatting & Comparison Utilities */
char    *kajo_files_format_size(guint64 bytes, gboolean is_dir);
char    *kajo_files_format_permissions(guint32 mode, gboolean is_dir, gboolean is_symlink);
char    *kajo_files_format_mtime(gint64 mtime_sec);
gboolean kajo_files_is_text_or_code(const char *content_type, const char *filename);
gboolean kajo_files_is_archive(const char *content_type, const char *filename);

int      kajo_file_item_compare(const KajoFileItem *a,
                                const KajoFileItem *b,
                                KajoFileSortField field,
                                gboolean ascending);

int      kajo_file_item_compare_full(const KajoFileItem *a,
                                     const KajoFileItem *b,
                                     KajoFileSortField field,
                                     gboolean ascending,
                                     gboolean folders_first);

gboolean kajo_file_sort_field_default_ascending(KajoFileSortField field);
const char       *kajo_file_sort_field_to_string(KajoFileSortField field);
KajoFileSortField kajo_file_sort_field_from_string(const char *str);
const char       *kajo_file_view_mode_to_string(KajoFileViewMode mode);
KajoFileViewMode  kajo_file_view_mode_from_string(const char *str);

gboolean kajo_file_item_matches_filter(const KajoFileItem *item,
                                       gboolean show_hidden,
                                       const char *search_query);

typedef struct {
    char *uri;
    char *label;
    char *tooltip;
} KajoBreadcrumbSegment;

void       kajo_breadcrumb_segment_free(gpointer data);
GPtrArray *kajo_files_build_breadcrumb_chain(GFile *dir);
typedef enum {
    KAJO_REBUILD_SEL_RESTORE = 0,   /* previously selected items still exist: re-select them */
    KAJO_REBUILD_SEL_FIRST,         /* new folder or active search: select the first entry */
    KAJO_REBUILD_SEL_CLEAR          /* anything else: leave the selection cleared */
} KajoRebuildSelectionAction;

KajoRebuildSelectionAction kajo_files_rebuild_selection_action(gboolean has_matches,
                                                              gboolean autoselect_pending,
                                                              gboolean search_active);

GtkBitset *kajo_files_match_selection_uris(GPtrArray *visible_items,
                                           GPtrArray *selected_uris);

/* Async Non-Blocking Directory Enumeration (Local + GVFS) */
typedef void (*KajoDirEnumCallback)(GPtrArray *items, GError *error, gpointer user_data);

void kajo_files_enumerate_async(GFile *dir,
                                GCancellable *cancellable,
                                KajoDirEnumCallback callback,
                                gpointer user_data);

G_END_DECLS

#endif /* KAJO_FILES_MODEL_H */
