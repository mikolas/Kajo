#include "../kajo_config_schema.h"

static const KajoEnumMap k_files_view_map[] = {
    { "grid", KAJO_FILES_VIEW_GRID },
    { "list", KAJO_FILES_VIEW_LIST },
    { NULL, 0 }
};

static const KajoEnumMap k_files_sort_map[] = {
    { "name",     KAJO_FILES_SORT_NAME },
    { "size",     KAJO_FILES_SORT_SIZE },
    { "modified", KAJO_FILES_SORT_MODIFIED },
    { "type",     KAJO_FILES_SORT_TYPE },
    { NULL, 0 }
};

static const KajoFieldSpec k_files_fields[] = {
    {
        .section  = NULL,
        .key      = KAJO_CFG_KEY_FILES_VIEW_MODE,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoFilesCfg, view_mode),
        .def.e    = KAJO_FILES_VIEW_GRID,
        .enum_map = k_files_view_map,
    },
    {
        .section  = NULL,
        .key      = KAJO_CFG_KEY_FILES_SORT_FIELD,
        .type     = KAJO_FIELD_ENUM,
        .offset   = offsetof(KajoFilesCfg, sort_field),
        .def.e    = KAJO_FILES_SORT_NAME,
        .enum_map = k_files_sort_map,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_FILES_SORT_ASC,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoFilesCfg, sort_ascending),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_FILES_FOLDERS_FIRST,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoFilesCfg, sort_folders_first),
        .def.b   = TRUE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_FILES_SHOW_HIDDEN,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoFilesCfg, show_hidden),
        .def.b   = FALSE,
    },
    {
        .section = NULL,
        .key     = KAJO_CFG_KEY_FILES_SHOW_INSPECTOR,
        .type    = KAJO_FIELD_BOOL,
        .offset  = offsetof(KajoFilesCfg, show_inspector),
        .def.b   = TRUE,
    },
};

static const KajoDomainSchema k_files_schema = {
    .domain            = KAJO_CONFIG_DOMAIN_FILES,
    .filename          = KAJO_CFG_FILE_FILES,
    .domain_table_name = KAJO_CFG_DOMAIN_FILES,
    .struct_size       = sizeof(KajoFilesCfg),
    .fields            = k_files_fields,
    .n_fields          = G_N_ELEMENTS(k_files_fields),
};

const KajoDomainSchema *
kajo_schema_get_files(void)
{
    return &k_files_schema;
}
