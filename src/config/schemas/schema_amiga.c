#include "../kajo_config_schema.h"

static const KajoEnumMap k_amiga_video_map[] = {
    { "pal",  KAJO_AMIGA_VIDEO_PAL },
    { "ntsc", KAJO_AMIGA_VIDEO_NTSC },
    { NULL, 0 }
};

static const KajoEnumMap k_amiga_boot_map[] = {
    { "dh0", KAJO_AMIGA_BOOT_DH0 },
    { "adf", KAJO_AMIGA_BOOT_ADF },
    { NULL, 0 }
};

static const KajoEnumMap k_amiga_tc_map[] = {
    { "elf2hunk",   KAJO_AMIGA_TC_ELF2HUNK },
    { "hunk_vlink", KAJO_AMIGA_TC_HUNK_VLINK },
    { "single_asm", KAJO_AMIGA_TC_SINGLE_ASM },
    { NULL, 0 }
};

static const KajoEnumMap k_amiga_entry_map[] = {
    { "entry_break", KAJO_AMIGA_ENTRY_BREAK },
    { "run_to_bp",   KAJO_AMIGA_RUN_TO_BP },
    { NULL, 0 }
};

static const KajoEnumMap k_amiga_hex_map[] = {
    { "8b",  KAJO_AMIGA_HEX_8B },
    { "16b", KAJO_AMIGA_HEX_16B },
    { "32b", KAJO_AMIGA_HEX_32B },
    { NULL, 0 }
};

const gchar *
kajo_amiga_video_to_string(KajoAmigaVideoMode mode)
{
    return (mode == KAJO_AMIGA_VIDEO_NTSC) ? "ntsc" : "pal";
}

const gchar *
kajo_amiga_boot_mode_to_string(KajoAmigaBootMode mode)
{
    return (mode == KAJO_AMIGA_BOOT_ADF) ? "adf" : "dh0";
}

const gchar *
kajo_amiga_toolchain_to_string(KajoAmigaToolchain tc)
{
    switch (tc) {
    case KAJO_AMIGA_TC_HUNK_VLINK: return "hunk_vlink";
    case KAJO_AMIGA_TC_SINGLE_ASM: return "single_asm";
    default:                       return "elf2hunk";
    }
}

static const KajoFieldSpec k_amiga_fields[] = {
    { NULL, KAJO_CFG_KEY_AMIGA_AMIBERRY_BIN,      KAJO_FIELD_STRING, offsetof(KajoAmigaCfg, amiberry_bin),             .def.s = "amiberry" },
    { NULL, KAJO_CFG_KEY_AMIGA_DEFAULT_PROFILE,   KAJO_FIELD_STRING, offsetof(KajoAmigaCfg, default_profile),          .def.s = "a500_ocs" },
    { NULL, KAJO_CFG_KEY_AMIGA_DEFAULT_VIDEO,     KAJO_FIELD_ENUM,   offsetof(KajoAmigaCfg, default_video),            .def.e = KAJO_AMIGA_VIDEO_PAL, .enum_map = k_amiga_video_map },
    { NULL, KAJO_CFG_KEY_AMIGA_KICKSTART_A500,    KAJO_FIELD_STRING, offsetof(KajoAmigaCfg, kickstart_a500),           .def.s = "" },
    { NULL, KAJO_CFG_KEY_AMIGA_KICKSTART_A1200,   KAJO_FIELD_STRING, offsetof(KajoAmigaCfg, kickstart_a1200),          .def.s = "" },
    { NULL, KAJO_CFG_KEY_AMIGA_DEFAULT_BOOT_MODE, KAJO_FIELD_ENUM,   offsetof(KajoAmigaCfg, default_boot_mode),        .def.e = KAJO_AMIGA_BOOT_DH0, .enum_map = k_amiga_boot_map },
    { NULL, KAJO_CFG_KEY_AMIGA_GDB_HOST,          KAJO_FIELD_STRING, offsetof(KajoAmigaCfg, gdb_host),                 .def.s = "127.0.0.1" },
    { NULL, KAJO_CFG_KEY_AMIGA_GDB_PORT,          KAJO_FIELD_INT,    offsetof(KajoAmigaCfg, gdb_port),                 .def.i = 2345, .min_int = 1024, .max_int = 65535 },
    { NULL, KAJO_CFG_KEY_AMIGA_ADF_UAEBOARD,      KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, adf_uaeboard_guest_output),.def.b = TRUE },
    { NULL, KAJO_CFG_KEY_AMIGA_DEFAULT_TOOLCHAIN, KAJO_FIELD_ENUM,   offsetof(KajoAmigaCfg, default_toolchain),        .def.e = KAJO_AMIGA_TC_ELF2HUNK, .enum_map = k_amiga_tc_map },
    { NULL, KAJO_CFG_KEY_AMIGA_EXTRA_BIN_PATHS,   KAJO_FIELD_STRING, offsetof(KajoAmigaCfg, extra_bin_paths),          .def.s = "" },
    { NULL, KAJO_CFG_KEY_AMIGA_C_INCLUDE_DIRS,    KAJO_FIELD_STRING, offsetof(KajoAmigaCfg, c_include_dirs),           .def.s = "" },
    { NULL, KAJO_CFG_KEY_AMIGA_ASM_INCLUDE_DIRS,  KAJO_FIELD_STRING, offsetof(KajoAmigaCfg, asm_include_dirs),         .def.s = "" },
    { NULL, KAJO_CFG_KEY_AMIGA_HRM_DIR,           KAJO_FIELD_STRING, offsetof(KajoAmigaCfg, hrm_dir),                  .def.s = "" },
    { NULL, KAJO_CFG_KEY_AMIGA_INHERIT_IDE_PREFS, KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, inherit_ide_editor_prefs), .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_AMIGA_SHOW_CYCLE_GUTTER, KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, show_cycle_gutter),        .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_AMIGA_SHOW_SEL_CYCLES,   KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, show_selection_cycle_sum), .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_AMIGA_HOVER_DECODERS,    KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, enable_hover_decoders),    .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_AMIGA_ENABLE_M68K_LSP,   KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, enable_m68k_lsp),          .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_AMIGA_ENABLE_CLANGD,     KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, enable_clangd),            .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_AMIGA_ENTRY_BREAK_MODE,  KAJO_FIELD_ENUM,   offsetof(KajoAmigaCfg, entry_break_mode),         .def.e = KAJO_AMIGA_ENTRY_BREAK, .enum_map = k_amiga_entry_map },
    { NULL, KAJO_CFG_KEY_AMIGA_CAPTURE_GUEST_OUT, KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, capture_guest_output),     .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_AMIGA_TRACE_HISTORY,     KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, enable_trace_history),     .def.b = FALSE },
    { NULL, KAJO_CFG_KEY_AMIGA_EXC_MASK,          KAJO_FIELD_INT,    offsetof(KajoAmigaCfg, default_exception_mask),   .def.i = 0, .min_int = 0, .max_int = 255 },
    { NULL, KAJO_CFG_KEY_AMIGA_TRACE_BLITS,       KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, trace_blitter_writes),     .def.b = FALSE },
    { NULL, KAJO_CFG_KEY_AMIGA_HEX_GROUPING,      KAJO_FIELD_ENUM,   offsetof(KajoAmigaCfg, default_hex_grouping),     .def.e = KAJO_AMIGA_HEX_16B, .enum_map = k_amiga_hex_map },
    { NULL, KAJO_CFG_KEY_AMIGA_BPL_GRID_16PX,     KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, bitplane_grid_16px),       .def.b = FALSE },
    { NULL, KAJO_CFG_KEY_AMIGA_INSPECTOR_VISIBLE, KAJO_FIELD_BOOL,   offsetof(KajoAmigaCfg, inspector_visible),        .def.b = TRUE },
};

static const KajoDomainSchema k_amiga_schema = {
    .domain            = KAJO_CONFIG_DOMAIN_AMIGA,
    .filename          = KAJO_CFG_FILE_AMIGA,
    .domain_table_name = KAJO_CFG_DOMAIN_AMIGA,
    .struct_size       = sizeof(KajoAmigaCfg),
    .fields            = k_amiga_fields,
    .n_fields          = G_N_ELEMENTS(k_amiga_fields),
};

const KajoDomainSchema *
kajo_schema_get_amiga(void)
{
    return &k_amiga_schema;
}
