#include "../kajo_config_schema.h"

static gint
normalize_ide_tab_width(gint val, gint def_val)
{
    if (val == 2 || val == 4 || val == 8)
        return val;
    return def_val;
}

static const KajoFieldSpec k_ide_fields[] = {
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_COLOR_SCHEME,
        .type    = KAJO_FIELD_STRING,
        .offset  = offsetof(KajoIdeCfg, color_scheme),
        .def.s   = "kajo-metro-dark",
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_FONT_FAMILY,
        .type    = KAJO_FIELD_STRING,
        .offset  = offsetof(KajoIdeCfg, font_family),
        .def.s   = "Fira Code",
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_FONT_SIZE_PX,
        .type    = KAJO_FIELD_INT,
        .offset  = offsetof(KajoIdeCfg, font_size_px),
        .def.i   = 13,
        .min_int = 9,
        .max_int = 28,
    },
    {
        .section       = NULL,
        .key           = KAJO_CFG_KEY_IDE_TAB_WIDTH,
        .type          = KAJO_FIELD_INT,
        .offset        = offsetof(KajoIdeCfg, tab_width),
        .def.i         = 4,
        .normalize_int = normalize_ide_tab_width,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_INSERT_SPACES,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, insert_spaces),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_WORD_WRAP,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, word_wrap),
        .def.b   = FALSE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_MARGIN_GUIDE,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, show_margin_guide),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_MINIMAP,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, show_minimap),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_LINE_NUMBERS,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, show_line_numbers),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_IDE_CUR_LINE,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoIdeCfg, highlight_current_line),
        .def.b   = TRUE,
    },
};

static const KajoDomainSchema k_ide_schema = {
    .domain            = KAJO_CONFIG_DOMAIN_IDE,
    .filename          = KAJO_CFG_FILE_IDE,
    .domain_table_name = KAJO_CFG_DOMAIN_IDE,
    .struct_size       = sizeof(KajoIdeCfg),
    .fields            = k_ide_fields,
    .n_fields          = G_N_ELEMENTS(k_ide_fields),
};

const KajoDomainSchema *
kajo_schema_get_ide(void)
{
    return &k_ide_schema;
}
