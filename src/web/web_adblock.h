#ifndef KAJO_WEB_ADBLOCK_H
#define KAJO_WEB_ADBLOCK_H

#include <webkit/webkit.h>
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

typedef struct _KajoWebAdblock KajoWebAdblock;

typedef void (*KajoWebAdblockReadyCb)(KajoWebAdblock *ab, gpointer user_data);

KajoWebAdblock *web_adblock_new(const gchar           *cache_dir,
                                KajoWebAdblockReadyCb  on_ready,
                                gpointer               user_data);
void            web_adblock_free(KajoWebAdblock *ab);

void            web_adblock_apply_config(KajoWebAdblock *ab, const KajoWebCfg *cfg);
void            web_adblock_attach_manager(KajoWebAdblock           *ab,
                                           WebKitUserContentManager *ucm);
void            web_adblock_detach_manager(KajoWebAdblock           *ab,
                                           WebKitUserContentManager *ucm);

/* Download & merge Peter Lowe's active ad/tracker hosts list (~3700+ domains)
 * with the built-in WebKit Content Blocker rulepack and recompile live. */
void            web_adblock_update_lists_async(KajoWebAdblock *ab);
gboolean        web_adblock_is_updating(const KajoWebAdblock *ab);

gboolean        web_adblock_is_enabled(const KajoWebAdblock *ab);
gboolean        web_adblock_is_ready(const KajoWebAdblock *ab);
guint           web_adblock_get_rule_count(const KajoWebAdblock *ab);
guint           web_adblock_get_user_script_count(const KajoWebAdblock *ab);
const gchar    *web_adblock_get_status_text(const KajoWebAdblock *ab);

G_END_DECLS

#endif /* KAJO_WEB_ADBLOCK_H */
