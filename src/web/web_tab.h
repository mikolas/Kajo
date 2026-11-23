#ifndef KAJO_WEB_TAB_H
#define KAJO_WEB_TAB_H

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include "web_model.h"
#include "web_adblock.h"
#include "web_media_sniff_core.h"
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

typedef struct _KajoWebTab KajoWebTab;

typedef struct {
    void (*on_changed)(KajoWebTab *tab, gpointer ud);
    void (*on_progress)(KajoWebTab *tab, gdouble progress, gboolean loading, gpointer ud);
    void (*on_new_tab_requested)(const gchar *uri, gboolean background, gpointer ud);
    void (*on_permission_requested)(KajoWebTab *tab, WebKitPermissionRequest *req, gpointer ud);
    void (*on_download_started)(KajoWebTab *tab, WebKitDownload *dl, gpointer ud);
    void (*on_media_detected)(KajoWebTab *tab, gpointer ud);
    void (*on_visit_committed)(KajoWebTab *tab, const gchar *uri, const gchar *title, gpointer ud);
} KajoWebTabCallbacks;

KajoWebTab         *web_tab_new(guint64                    tab_id,
                                WebKitWebContext          *web_ctx,
                                WebKitNetworkSession      *session,
                                KajoWebAdblock            *adblock,
                                const KajoWebCfg          *cfg,
                                const KajoWebTabCallbacks *cbs,
                                gpointer                   user_data);
void                web_tab_free(KajoWebTab *tab);

guint64             web_tab_get_id(const KajoWebTab *tab);
GtkWidget          *web_tab_get_widget(KajoWebTab *tab);
WebKitWebView      *web_tab_get_view(KajoWebTab *tab);

void                web_tab_load_uri(KajoWebTab *tab, const gchar *uri);
void                web_tab_reload(KajoWebTab *tab, gboolean bypass_cache);
void                web_tab_stop(KajoWebTab *tab);
void                web_tab_go_back(KajoWebTab *tab);
void                web_tab_go_forward(KajoWebTab *tab);
gboolean            web_tab_can_go_back(const KajoWebTab *tab);
gboolean            web_tab_can_go_forward(const KajoWebTab *tab);
gboolean            web_tab_is_loading(const KajoWebTab *tab);
gdouble             web_tab_get_progress(const KajoWebTab *tab);
const gchar        *web_tab_get_uri(const KajoWebTab *tab);
const gchar        *web_tab_get_title(const KajoWebTab *tab);
gboolean            web_tab_is_playing_audio(const KajoWebTab *tab);
gboolean            web_tab_is_muted(const KajoWebTab *tab);
void                web_tab_set_muted(KajoWebTab *tab, gboolean muted);
void                web_tab_set_zoom(KajoWebTab *tab, gdouble zoom);
gdouble             web_tab_get_zoom(const KajoWebTab *tab);
void                web_tab_toggle_inspector(KajoWebTab *tab);
void                web_tab_apply_config(KajoWebTab *tab, const KajoWebCfg *cfg);

/* Idle tab suspension via WebKitWebViewSessionState */
gboolean            web_tab_is_suspended(const KajoWebTab *tab);
void                web_tab_suspend(KajoWebTab *tab);
void                web_tab_wake(KajoWebTab *tab, const KajoWebCfg *cfg);

/* VDH-style sniffed media streams on this tab */
const GPtrArray    *web_tab_get_media_streams(const KajoWebTab *tab);

/* Find in page */
void                web_tab_find_search(KajoWebTab *tab, const gchar *text);
void                web_tab_find_next(KajoWebTab *tab);
void                web_tab_find_prev(KajoWebTab *tab);
void                web_tab_find_clear(KajoWebTab *tab);

G_END_DECLS

#endif /* KAJO_WEB_TAB_H */
