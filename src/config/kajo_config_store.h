#ifndef KAJO_CONFIG_STORE_H
#define KAJO_CONFIG_STORE_H

#include "kajo_config_types.h"
#include "kajo_config_keys.h"
#include "kajo_config_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _KajoConfigStore KajoConfigStore;

/* Process-wide singleton store (~/.config/kajo or $KAJO_CONFIG_DIR) */
KajoConfigStore *kajo_config_store_get_default(void);

/* Hermetic store bound to a specific directory (for unit tests) */
KajoConfigStore *kajo_config_store_new_for_dir(const gchar *config_dir);
void             kajo_config_store_destroy(KajoConfigStore *store);
const gchar     *kajo_config_store_get_config_dir(KajoConfigStore *store);

/* Thread-Safe Snapshot Acquisition (usable from ANY thread) */
const KajoThemeCfg *kajo_config_store_acquire_theme(KajoConfigStore *store);
const KajoShellCfg *kajo_config_store_acquire_shell(KajoConfigStore *store);
const KajoIdeCfg   *kajo_config_store_acquire_ide(KajoConfigStore *store);
const KajoFilesCfg *kajo_config_store_acquire_files(KajoConfigStore *store);
const KajoModCfg   *kajo_config_store_acquire_mod(KajoConfigStore *store);

/* Transactional Base-File Read-Modify-Write Mutators */
typedef void (*KajoThemeMutateFn)(KajoThemeCfg *draft, gpointer user_data);
typedef void (*KajoShellMutateFn)(KajoShellCfg *draft, gpointer user_data);
typedef void (*KajoIdeMutateFn)  (KajoIdeCfg   *draft, gpointer user_data);
typedef void (*KajoFilesMutateFn)(KajoFilesCfg *draft, gpointer user_data);
typedef void (*KajoModMutateFn)  (KajoModCfg   *draft, gpointer user_data);

gboolean kajo_config_store_update_theme(KajoConfigStore  *store,
                                        KajoThemeMutateFn mutate_fn,
                                        gpointer          user_data,
                                        GError          **error);

gboolean kajo_config_store_update_shell(KajoConfigStore  *store,
                                        KajoShellMutateFn mutate_fn,
                                        gpointer          user_data,
                                        GError          **error);

gboolean kajo_config_store_update_ide(KajoConfigStore  *store,
                                      KajoIdeMutateFn   mutate_fn,
                                      gpointer          user_data,
                                      GError          **error);

gboolean kajo_config_store_update_files(KajoConfigStore  *store,
                                        KajoFilesMutateFn mutate_fn,
                                        gpointer          user_data,
                                        GError          **error);

gboolean kajo_config_store_update_mod(KajoConfigStore  *store,
                                      KajoModMutateFn   mutate_fn,
                                      gpointer          user_data,
                                      GError          **error);

/* Reload & Pub/Sub Notification */
void kajo_config_store_reload(KajoConfigStore *store, KajoConfigDomain domains);

typedef void (*KajoConfigChangedCb)(KajoConfigStore *store,
                                    KajoConfigDomain changed_domains,
                                    gpointer         user_data);

guint kajo_config_store_subscribe(KajoConfigStore    *store,
                                  KajoConfigDomain    domains,
                                  KajoConfigChangedCb cb,
                                  gpointer            user_data,
                                  GDestroyNotify      destroy_notify);

void  kajo_config_store_unsubscribe(KajoConfigStore *store, guint sub_id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoConfigStore, kajo_config_store_destroy)

#ifdef __cplusplus
}
#endif

#endif /* KAJO_CONFIG_STORE_H */
