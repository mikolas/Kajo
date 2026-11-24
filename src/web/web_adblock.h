#ifndef KAJO_WEB_ADBLOCK_H
#define KAJO_WEB_ADBLOCK_H

#include <webkit/webkit.h>
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

typedef struct _KajoWebAdblock KajoWebAdblock;

typedef void (*KajoWebAdblockReadyCb)(KajoWebAdblock *ab, gpointer user_data);

/* data_dir may be NULL (e.g. private window -> allowlist is in-memory only). */
KajoWebAdblock *web_adblock_new(const gchar           *cache_dir,
                                const gchar           *data_dir,
                                const KajoWebCfg      *cfg,
                                KajoWebAdblockReadyCb  on_ready,
                                gpointer               user_data);
void            web_adblock_free(KajoWebAdblock *ab);

void            web_adblock_apply_config(KajoWebAdblock *ab, const KajoWebCfg *cfg);

gboolean        web_adblock_is_initial_load_done(const KajoWebAdblock *ab);

/* Shared WebKitUserContentManagers (1 active + 1 empty for paused sites) so all tabs
 * share the exact same mmap'd DFA filters instead of duplicating per-tab managers. */
WebKitUserContentManager *web_adblock_get_shared_ucm(KajoWebAdblock *ab);
WebKitUserContentManager *web_adblock_get_paused_ucm(KajoWebAdblock *ab);

/* Per-site 1-click pause/allowlist (persisted in <data_dir>/adblock-allowlist.txt). */
gboolean        web_adblock_is_host_paused(const KajoWebAdblock *ab, const gchar *host);
gboolean        web_adblock_toggle_host_paused(KajoWebAdblock *ab, const gchar *host);
guint           web_adblock_get_paused_host_count(const KajoWebAdblock *ab);

/* Download, convert (ABP -> WebKit Content Blocker JSON), and compile EasyList +
 * EasyPrivacy + EasyList Finland + Dandelion Sprout's Nordic + Cookie Notices
 * into independent cached WebKit binary filters, evicting superseded hashes. */
void            web_adblock_update_lists_async(KajoWebAdblock *ab);
gboolean        web_adblock_is_updating(const KajoWebAdblock *ab);

gboolean        web_adblock_is_enabled(const KajoWebAdblock *ab);
gboolean        web_adblock_is_ready(const KajoWebAdblock *ab);
gboolean        web_adblock_has_error(const KajoWebAdblock *ab);
guint           web_adblock_get_rule_count(const KajoWebAdblock *ab);
guint           web_adblock_get_loaded_list_count(const KajoWebAdblock *ab);
guint           web_adblock_get_user_script_count(const KajoWebAdblock *ab);
const gchar    *web_adblock_get_status_text(const KajoWebAdblock *ab);

G_END_DECLS

#endif /* KAJO_WEB_ADBLOCK_H */
