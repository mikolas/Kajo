#ifndef KAJO_CONFIG_MIGRATE_H
#define KAJO_CONFIG_MIGRATE_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Checks if `<config_dir>/config.json` exists and `<config_dir>/shell.toml`
 * does not exist. If so, imports all 5 domains from `config.json`, writes
 * `{theme,shell,ide,files,mod}.toml` into `<config_dir>`, and renames
 * `config.json` to `config.json.bak`.
 *
 * Returns TRUE if a migration was performed, FALSE otherwise.
 */
gboolean kajo_config_migrate_legacy_json(const char *config_dir);

#ifdef __cplusplus
}
#endif

#endif /* KAJO_CONFIG_MIGRATE_H */
