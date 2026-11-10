#include "../kajo_config_schema.h"

static const KajoEnumMap k_mod_filter_map[] = {
    { "a500",   KAJO_MOD_FILTER_A500 },
    { "a1200",  KAJO_MOD_FILTER_A1200 },
    { "near",   KAJO_MOD_FILTER_NEAR },
    { "linear", KAJO_MOD_FILTER_LINEAR },
    { "lin",    KAJO_MOD_FILTER_LINEAR },
    { "sinc",   KAJO_MOD_FILTER_SINC },
    { NULL, 0 }
};

static const KajoEnumMap k_mod_led_map[] = {
    { "auto", KAJO_MOD_LED_AUTO },
    { "on",   KAJO_MOD_LED_ON },
    { "off",  KAJO_MOD_LED_OFF },
    { NULL, 0 }
};

static const KajoEnumMap k_mod_end_map[] = {
    { "next-sub", KAJO_MOD_END_NEXT_SUB },
    { "loop",     KAJO_MOD_END_LOOP },
    { "next-mod", KAJO_MOD_END_NEXT_MOD },
    { NULL, 0 }
};

static const KajoEnumMap k_mod_sid_map[] = {
    { "auto", KAJO_MOD_SID_AUTO },
    { "6581", KAJO_MOD_SID_6581 },
    { "8580", KAJO_MOD_SID_8580 },
    { NULL, 0 }
};

static gint
normalize_uade_timeout(gint val, gint def_val G_GNUC_UNUSED)
{
    if (val == 0)
        return 0;
    return CLAMP(val, 60, 600);
}

static const KajoFieldSpec k_mod_fields[] = {
    {
        .section  = NULL,
        .key      = KAJO_CFG_KEY_MOD_FILTER_MODE,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoModCfg, filter_mode),
        .def.e    = KAJO_MOD_FILTER_A500,
        .enum_map = k_mod_filter_map,
    },
    {
        .section  = NULL,
        .key      = KAJO_CFG_KEY_MOD_LED_MODE,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoModCfg, led_mode),
        .def.e    = KAJO_MOD_LED_AUTO,
        .enum_map = k_mod_led_map,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_MOD_STEREO_SEP_PCT,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoModCfg, stereo_sep_pct),
        .def.i   = 70,
        .min_int = 0,
        .max_int = 100,
    },
    {
        .section  = NULL,
        .key      = KAJO_CFG_KEY_MOD_END_ACTION,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoModCfg, end_action),
        .def.e    = KAJO_MOD_END_NEXT_SUB,
        .enum_map = k_mod_end_map,
    },
    {
        .section  = NULL,
        .key      = KAJO_CFG_KEY_MOD_SID_MODEL,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoModCfg, sid_model),
        .def.e    = KAJO_MOD_SID_AUTO,
        .enum_map = k_mod_sid_map,
    },
    {
        .section       = NULL,
        .key           = KAJO_CFG_KEY_MOD_UADE_TIMEOUT_SEC,
        .type          = KAJO_FIELD_INT,
        .offset        = offsetof(KajoModCfg, uade_timeout_sec),
        .def.i         = 180,
        .normalize_int = normalize_uade_timeout,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_MOD_AUTO_EXTRACT,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoModCfg, auto_extract_archives),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_MOD_HVSC_PATH,
        .type    = KAJO_FIELD_STRING,
        .offset  = offsetof(KajoModCfg, hvsc_path),
        .def.s   = "",
    },
};

static const KajoDomainSchema k_mod_schema = {
    .domain            = KAJO_CONFIG_DOMAIN_MOD,
    .filename          = KAJO_CFG_FILE_MOD,
    .domain_table_name = KAJO_CFG_DOMAIN_MOD,
    .struct_size       = sizeof(KajoModCfg),
    .fields            = k_mod_fields,
    .n_fields          = G_N_ELEMENTS(k_mod_fields),
};

const KajoDomainSchema *
kajo_schema_get_mod(void)
{
    return &k_mod_schema;
}
