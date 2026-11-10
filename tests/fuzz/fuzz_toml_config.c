#include "../../src/config.h"
#include "../../src/config/kajo_config_schema.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <glib.h>

static void
silent_log_handler(const gchar   *log_domain G_GNUC_UNUSED,
                   GLogLevelFlags log_level G_GNUC_UNUSED,
                   const gchar   *message G_GNUC_UNUSED,
                   gpointer       user_data G_GNUC_UNUSED)
{
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static gsize log_init = 0;
    if (g_once_init_enter(&log_init)) {
        g_log_set_handler(G_LOG_DOMAIN,
                          (GLogLevelFlags)(G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE),
                          silent_log_handler, NULL);
        g_once_init_leave(&log_init, 1);
    }

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

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int
main(void)
{
    static const char *const k_corpus[] = {
        "",
        "# comment only\n",
        "accent_color = \"#00ff00\"\ncreated_at = 2026-11-10T12:00:00Z\n",
        "[panel\nposition = \"top\"\n",
        "[[layout.left]]\nid = 123\nmode = [1, 2, 3]\n",
        "ui_font_size_px = 999999999999999999999999\n",
        "exec = 'unterminated literal string",
        "[shell.panel]\nheight = -500\n[shell.autohide]\nedge_strip_px = 8\n",
    };

    for (size_t i = 0; i < G_N_ELEMENTS(k_corpus); i++) {
        LLVMFuzzerTestOneInput((const uint8_t *)k_corpus[i], strlen(k_corpus[i]));
    }
    return 0;
}
#endif
