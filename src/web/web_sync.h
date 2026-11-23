#ifndef KAJO_WEB_SYNC_H
#define KAJO_WEB_SYNC_H

#include <glib.h>
#include "web_storage.h"
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

typedef struct _KajoWebSync KajoWebSync;

typedef void (*KajoWebSyncStatusCb)(const gchar *status_text,
                                    const gchar *email,
                                    gboolean     connected,
                                    gpointer     user_data);

typedef void (*KajoWebSyncOpenUrlCb)(const gchar *auth_url, gpointer user_data);

KajoWebSync *web_sync_new(const gchar         *profile_id,
                          KajoWebStorage      *storage,
                          KajoWebSyncStatusCb  on_status,
                          KajoWebSyncOpenUrlCb on_open_url,
                          gpointer             user_data);
void         web_sync_free(KajoWebSync *sync);

/* Start free Google Drive AppData OAuth 2.0 PKCE loopback sign-in */
gboolean     web_sync_begin_oauth(KajoWebSync *sync, const KajoWebCfg *cfg, GError **error);
void         web_sync_sign_out(KajoWebSync *sync);

/* Trigger an async bookmarks sync with Google Drive appDataFolder */
void         web_sync_trigger_now(KajoWebSync *sync, const KajoWebCfg *cfg, const gchar *passphrase);
void         web_sync_schedule_debounced(KajoWebSync *sync, const KajoWebCfg *cfg);

gboolean     web_sync_is_connected(const KajoWebSync *sync);
const gchar *web_sync_get_status(const KajoWebSync *sync);

G_END_DECLS

#endif /* KAJO_WEB_SYNC_H */
