#ifndef KAJO_WEB_PROFILES_H
#define KAJO_WEB_PROFILES_H

#include <webkit/webkit.h>
#include "web_model.h"
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

typedef struct _KajoWebProfileManager KajoWebProfileManager;

KajoWebProfileManager  *web_profile_manager_new(void);
void                    web_profile_manager_free(KajoWebProfileManager *pm);

KajoWebProfileRegistry *web_profile_manager_get_registry(KajoWebProfileManager *pm);
gboolean                web_profile_manager_save(KajoWebProfileManager *pm, GError **error);
const gchar            *web_profile_manager_get_root_dir(KajoWebProfileManager *pm);

/* Returns per-profile directories (caller frees with g_free) */
gchar                  *web_profile_manager_get_data_dir(KajoWebProfileManager *pm, const gchar *profile_id);
gchar                  *web_profile_manager_get_cache_dir(KajoWebProfileManager *pm, const gchar *profile_id);

/* Acquire cached persistent WebKitNetworkSession for profile_id (or new ephemeral for private) */
WebKitNetworkSession   *web_profile_manager_get_session(KajoWebProfileManager *pm,
                                                        const gchar           *profile_id,
                                                        gboolean               is_private,
                                                        const KajoWebCfg      *cfg);

/* Apply updated network/privacy/memory settings to all live sessions */
void                    web_profile_manager_apply_config(KajoWebProfileManager *pm,
                                                         const KajoWebCfg      *cfg);

G_END_DECLS

#endif /* KAJO_WEB_PROFILES_H */
