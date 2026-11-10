#include "../kajo_config_schema.h"

static const KajoFieldSpec k_theme_fields[] = {
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_ACCENT_COLOR,
        .type    = KAJO_FIELD_HEX_COLOR,
        .offset  = offsetof(KajoThemeCfg, accent_color),
        .def.s   = "#0078d4",
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_UI_FONT_SIZE_PX,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoThemeCfg, ui_font_size_px),
        .def.i   = 14,
        .min_int = 11,
        .max_int = 20,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_HEADING_FONT_SIZE_PX,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoThemeCfg, font_heading_size_px),
        .def.i   = 13,
        .min_int = 10,
        .max_int = 18,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_SUB_FONT_SIZE_PX,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoThemeCfg, font_subtitle_size_px),
        .def.i   = 12,
        .min_int = 10,
        .max_int = 18,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_BADGE_FONT_SIZE_PX,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoThemeCfg, font_badge_size_px),
        .def.i   = 11,
        .min_int = 9,
        .max_int = 16,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_CSS_PATH,
        .type    = KAJO_FIELD_STRING,
        .offset  = offsetof(KajoThemeCfg, css_path),
        .def.s   = "",
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_SETTINGS_WIDTH,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoThemeCfg, settings_width),
        .def.i   = 900,
        .min_int = 480,
        .max_int = 3840,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_SETTINGS_HEIGHT,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoThemeCfg, settings_height),
        .def.i   = 640,
        .min_int = 360,
        .max_int = 2160,
    },
};

static const KajoDomainSchema k_theme_schema = {
    .domain            = KAJO_CONFIG_DOMAIN_THEME,
    .filename          = KAJO_CFG_FILE_THEME,
    .domain_table_name = KAJO_CFG_DOMAIN_THEME,
    .struct_size       = sizeof(KajoThemeCfg),
    .fields            = k_theme_fields,
    .n_fields          = G_N_ELEMENTS(k_theme_fields),
};

const KajoDomainSchema *
kajo_schema_get_theme(void)
{
    return &k_theme_schema;
}
