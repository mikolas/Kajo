/* comp_config_private.h — internal TOML config parsing helpers (pure C). */
#ifndef KAJO_COMP_CONFIG_PRIVATE_H
#define KAJO_COMP_CONFIG_PRIVATE_H

#include "comp_config.h"
#include "comp_schema_keys.h"
#include "../../vendor/tomlc17/tomlc17.h"

#include <math.h>

static inline bool
comp_config_datum_as_double(toml_datum_t d, double *out)
{
    if (d.type == TOML_FP64 && isfinite(d.u.fp64)) {
        *out = d.u.fp64;
        return true;
    }
    if (d.type == TOML_INT64) {
        *out = (double)d.u.int64;
        return true;
    }
    return false;
}

bool comp_config_parse_hex_rgba(const char *s, float out[4]);

void comp_config_merge_animations_section(CompConfig *cfg, toml_datum_t tab,
                                          const char *src, CompDiagList *diag);
void comp_config_merge_effects_section(CompConfig *cfg, toml_datum_t fx,
                                       const char *src, CompDiagList *diag);

#endif /* KAJO_COMP_CONFIG_PRIVATE_H */
