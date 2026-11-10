#include "../../src/config.h"
#include "../../src/config/kajo_config_schema.h"
#include <stdint.h>
#include <stddef.h>
#include <glib.h>

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536)
        return 0;

    const KajoDomainSchema *domains[] = {
        kajo_schema_get_theme(),
        kajo_schema_get_shell(),
        kajo_schema_get_ide(),
        kajo_schema_get_files(),
        kajo_schema_get_mod(),
    };

    for (size_t i = 0; i < G_N_ELEMENTS(domains); i++) {
        const KajoDomainSchema *d = domains[i];
        gpointer cfg = kajo_cfg_domain_new_rc_default(d);
        kajo_cfg_domain_parse_toml_str(d, cfg, (const char *)data, (gssize)size, FALSE, d->filename);
        kajo_cfg_domain_parse_toml_str(d, cfg, (const char *)data, (gssize)size, TRUE, d->filename);
        kajo_cfg_domain_unref_rc(d, cfg);
    }

    return 0;
}
