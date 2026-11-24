#ifndef KAJO_WEB_SCHEMES_H
#define KAJO_WEB_SCHEMES_H

#include <webkit/webkit.h>
#include "web_storage.h"
#include "web_adblock.h"
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

typedef struct {
    const gchar     *profile_name;
    const gchar     *profile_email;
    const gchar     *profile_accent;
    gboolean         sync_enabled;
    gboolean         is_private;
    gboolean         hw_disabled_override;
    KajoWebStorage  *storage;
    KajoWebAdblock  *adblock;
    gchar            action_token[40];
} KajoWebSchemeContext;

void web_schemes_register(WebKitWebContext *ctx, KajoWebSchemeContext *sctx);
gchar *web_schemes_render_page(const gchar                *uri,
                               const KajoWebSchemeContext *sctx,
                               const KajoWebCfg           *wcfg,
                               const KajoThemeCfg         *tcfg);

G_END_DECLS

#endif /* KAJO_WEB_SCHEMES_H */
