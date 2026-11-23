#ifndef KAJO_FILES_PORTAL_H
#define KAJO_FILES_PORTAL_H

#include <gio/gio.h>
#include "files_model.h"

G_BEGIN_DECLS

#define KAJO_PORTAL_FILECHOOSER_BUS_NAME    "org.freedesktop.impl.portal.desktop.kajo.Files"
#define KAJO_PORTAL_FILECHOOSER_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define KAJO_PORTAL_FILECHOOSER_INTERFACE   "org.freedesktop.impl.portal.FileChooser"
#define KAJO_PORTAL_REQUEST_INTERFACE       "org.freedesktop.impl.portal.Request"

typedef enum {
    KAJO_PORTAL_MODE_OPEN_FILE = 0,
    KAJO_PORTAL_MODE_SAVE_FILE,
    KAJO_PORTAL_MODE_SAVE_FILES,
} KajoPortalChooserMode;

typedef struct {
    guint32 type;     /* 0 = glob pattern, 1 = MIME type */
    char   *pattern;
} KajoPortalFilterRule;

typedef struct {
    char      *label;
    GPtrArray *rules; /* KajoPortalFilterRule* */
} KajoPortalFilter;

typedef struct {
    char *id;
    char *label;
} KajoPortalChoiceOption;

typedef struct {
    char      *id;
    char      *label;
    char      *initial;
    GPtrArray *options; /* KajoPortalChoiceOption* (empty => boolean checkbox) */
} KajoPortalChoice;

typedef struct {
    KajoPortalChooserMode mode;
    char                 *app_id;
    char                 *parent_window;
    char                 *title;
    char                 *accept_label;
    gboolean              modal;
    gboolean              multiple;
    gboolean              directory;
    char                 *current_folder; /* local path or NULL */
    char                 *current_name;   /* suggested filename or NULL */
    GPtrArray            *save_files;     /* char* filenames for SaveFiles */
    GPtrArray            *filters;        /* KajoPortalFilter* */
    gint                  current_filter_index; /* -1 if none */
    GPtrArray            *choices;        /* KajoPortalChoice* */
} KajoPortalChooserRequest;

typedef struct {
    GPtrArray *uris;          /* char* normalized file:// URIs */
    GPtrArray *choice_ids;    /* char* */
    GPtrArray *choice_values; /* char* */
    gint       selected_filter_index; /* -1 if none */
    gboolean   writable;
} KajoPortalChooserResult;

typedef struct _KajoPortalActiveRequest KajoPortalActiveRequest;
typedef struct _KajoFilesPortal         KajoFilesPortal;

typedef void (*KajoPortalPresentFunc)(KajoPortalActiveRequest *active_req,
                                      gpointer                 user_data);
typedef void (*KajoPortalCloseFunc)(KajoPortalActiveRequest   *active_req,
                                    gpointer                   user_data);

/* Request/result constructors & destructors */
KajoPortalChooserRequest *kajo_portal_parse_options(KajoPortalChooserMode mode,
                                                    const char           *app_id,
                                                    const char           *parent_window,
                                                    const char           *title,
                                                    GVariant             *options);
void                      kajo_portal_chooser_request_free(KajoPortalChooserRequest *req);

KajoPortalChooserResult  *kajo_portal_chooser_result_new(void);
void                      kajo_portal_chooser_result_free(KajoPortalChooserResult *res);

/* Pure protocol helpers (unit-tested) */
char     *kajo_portal_strip_mnemonic(const char *label);
gboolean  kajo_portal_filter_matches_item(const KajoPortalFilter *filter,
                                          const KajoFileItem     *item);
char     *kajo_portal_normalize_file_uri(GFile *file);
GPtrArray *kajo_portal_build_save_files_uris(GFile           *folder,
                                             const GPtrArray *filenames);

/* Active request accessor & completion (response: 0=OK, 1=cancelled by user, 2=other/closed) */
const KajoPortalChooserRequest *kajo_portal_active_request_get_spec(const KajoPortalActiveRequest *req);
void                            kajo_portal_active_request_set_close_cb(KajoPortalActiveRequest *req,
                                                                        KajoPortalCloseFunc      close_cb,
                                                                        gpointer                 user_data);
/* Takes ownership of @result (transfer full) and frees @req. */
void                            kajo_portal_active_request_finish(KajoPortalActiveRequest *req,
                                                                  guint32                  response,
                                                                  KajoPortalChooserResult *result);

/* Portal service lifecycle */
KajoFilesPortal *kajo_files_portal_new(KajoPortalPresentFunc present_cb,
                                       gpointer              user_data);
KajoFilesPortal *kajo_files_portal_new_for_connection(GDBusConnection      *connection,
                                                      KajoPortalPresentFunc present_cb,
                                                      gpointer              user_data);
void             kajo_files_portal_destroy(KajoFilesPortal *self);

G_END_DECLS

#endif /* KAJO_FILES_PORTAL_H */
