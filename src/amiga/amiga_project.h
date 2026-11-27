#ifndef KAJO_AMIGA_PROJECT_H
#define KAJO_AMIGA_PROJECT_H

/* Pure GTK-free kajo-amiga.toml project model:
 * - Workspace + explicit [[target]] / [[profile]] definitions (zero directory heuristics)
 * - Clean 3-option artifact/boot model: exe_dh0, exe_adf, flat_bin
 * - Separate C (.h) vs ASM (.i) system include discovery & compile_commands.json */

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef enum {
  AMIGA_BUILD_NATIVE = 0,
  AMIGA_BUILD_EXTERNAL,
} AmigaBuildMode;

typedef enum {
  AMIGA_TARGET_EXE_DH0 = 0,  /* AmigaDOS Hunk .exe staged under virtual DH0: */
  AMIGA_TARGET_EXE_ADF,      /* AmigaDOS Hunk .exe packaged into bootable .adf via ADFlib */
  AMIGA_TARGET_FLAT_BIN,     /* Flat binary .bin artifact (not directly booted as DOS exe) */
} AmigaTargetKind;

typedef enum {
  AMIGA_TOOLCHAIN_ELF2HUNK = 0, /* gcc/vasm -Felf -dwarf=3 -> elf2hunk */
  AMIGA_TOOLCHAIN_HUNK_VLINK,   /* vasm -Fhunk -linedebug (+ optional vc) -> vlink */
  AMIGA_TOOLCHAIN_SINGLE_ASM,   /* vasmm68k_mot -Fhunkexe -linedebug */
  AMIGA_TOOLCHAIN_ELF2BIN,      /* gcc/vasm -Felf -> ld -Ttext -> objcopy -O binary */
  AMIGA_TOOLCHAIN_VASM_BIN,     /* vasmm68k_mot -Fbin */
} AmigaToolchainKind;

typedef enum {
  AMIGA_TEMPLATE_ASM_OS = 0,
  AMIGA_TEMPLATE_BARE_OCS,
  AMIGA_TEMPLATE_ADF_FLOPPY,
  AMIGA_TEMPLATE_MIXED_C_ASM,
} AmigaTemplateKind;

typedef struct {
  char               *name;
  AmigaTargetKind     kind;
  char               *cpu;            /* "68000", "68010", "68020", ... */
  AmigaToolchainKind  toolchain;
  GPtrArray          *sources;        /* char* */
  GPtrArray          *includes;       /* char* shared project-local include dirs */
  GPtrArray          *c_includes;     /* char* per-target C .h include dirs */
  GPtrArray          *asm_includes;   /* char* per-target ASM .i include dirs */
  GPtrArray          *defines;        /* char* */
  char               *startup_sequence;
  char               *copper_symbol;
  char               *bitplane_symbol;
  char               *entry_symbol;   /* e.g. "_start" */
  uint32_t            load_addr;      /* -Ttext=<load_addr> for flat_bin / elf2bin */
  char               *exe_path;       /* optional override */
  char               *elf_path;       /* optional override */
  char               *bin_path;       /* optional override */
} AmigaTarget;

typedef struct {
  char    *id;
  char    *model;      /* "A500", "A500P", "A1200" */
  char    *cpu;        /* "68000", "68020" */
  char    *chipset;    /* "OCS", "ECS", "AGA" */
  uint32_t chip_kb;
  uint32_t slow_kb;
  uint32_t fast_kb;
  char    *kickstart;
  bool     ntsc;
  bool     cycle_exact;
  char    *extra_args;
} AmigaHwProfile;

typedef struct {
  char           *root_dir;
  char           *manifest_path;
  char           *name;
  AmigaBuildMode  build_mode;
  char           *build_cmd;
  char           *clean_cmd;
  char           *default_target;
  char           *default_profile;
  GPtrArray      *shared_sources; /* char* */
  GPtrArray      *targets;        /* AmigaTarget* */
  GPtrArray      *profiles;       /* AmigaHwProfile* */
  char           *hrm_dir;
  char           *pre_build_cmd;
  char           *post_build_cmd;
  bool            trap86_guest_output;
} AmigaProject;

typedef struct {
  char      *amiberry;
  char      *vasmm68k_mot;
  char      *m68k_elf_gcc;
  char      *m68k_elf_ld;
  char      *m68k_elf_objdump;
  char      *m68k_elf_objcopy;
  char      *elf2hunk;
  char      *vlink;
  char      *vc;
  char      *m68k_lsp_server;
  char      *clangd;
  GPtrArray *c_include_dirs;   /* char* NDK / GCC / VBCC C header dirs (.h) */
  GPtrArray *asm_include_dirs; /* char* NDK / 68k ASM include dirs (.i) */
  char      *adflib_version;
} AmigaToolchainStatus;

AmigaProject         *amiga_project_new(void);
void                  amiga_project_free(AmigaProject *proj);

AmigaTarget          *amiga_target_new(const char *name);
void                  amiga_target_free(AmigaTarget *t);

AmigaHwProfile       *amiga_hw_profile_new(const char *id);
void                  amiga_hw_profile_free(AmigaHwProfile *p);
char                 *amiga_hw_profile_display_label(const AmigaHwProfile *p);

/* Seed the 4 built-in Amiberry hardware presets (a500_ocs, a500p_ecs, a1200_aga, a1200_fast) */
void                  amiga_project_seed_default_profiles(AmigaProject *proj);

bool                  amiga_project_parse_toml(const char    *toml_str,
                                               const char    *root_dir,
                                               AmigaProject **out_proj,
                                               GError       **error);
AmigaProject         *amiga_project_load_dir(const char *dir, GError **error);
char                 *amiga_project_serialize_toml(const AmigaProject *proj);
bool                  amiga_project_save(const AmigaProject *proj, GError **error);

const AmigaTarget    *amiga_project_find_target(const AmigaProject *proj, const char *name);
const AmigaHwProfile *amiga_project_find_profile(const AmigaProject *proj, const char *id);

bool                  amiga_project_init_template(const char       *dir,
                                                  const char       *name,
                                                  AmigaTemplateKind tpl,
                                                  const char       *default_profile,
                                                  AmigaTargetKind   pkg_kind,
                                                  GError          **error);

void                  amiga_toolchain_status_free(AmigaToolchainStatus *st);
AmigaToolchainStatus *amiga_project_probe_toolchain(const char *const *extra_bin_paths);
AmigaToolchainStatus *amiga_project_probe_toolchain_ex(const char *const *extra_bin_paths,
                                                       const char        *c_include_dirs,
                                                       const char        *asm_include_dirs);

/* Shared deduped include resolver for GCC/VBCC/clangd (for_asm=false) or vasm/m68k-lsp (for_asm=true). */
bool                  amiga_project_collect_target_includes(const AmigaProject         *proj,
                                                            const AmigaTarget          *target,
                                                            const AmigaToolchainStatus *tc,
                                                            bool                        for_asm,
                                                            bool                        make_abs,
                                                            GPtrArray                  *out_dirs);

char                 *amiga_project_build_compile_commands_json(const AmigaProject         *proj,
                                                                const AmigaToolchainStatus *tc);
bool                  amiga_project_write_compile_commands(const AmigaProject         *proj,
                                                           const AmigaToolchainStatus *tc,
                                                           GError                    **error);
char                 *amiga_project_export_makefile(const AmigaProject         *proj,
                                                    const AmigaToolchainStatus *tc);

/* Returns allocated warning string if target CPU/AGA mismatches profile, else NULL. */
char                 *amiga_project_check_profile_compat(const AmigaTarget    *target,
                                                         const AmigaHwProfile *profile);

const char           *amiga_target_kind_to_str(AmigaTargetKind k);
AmigaTargetKind       amiga_target_kind_from_str(const char *s);
const char           *amiga_toolchain_kind_to_str(AmigaToolchainKind k);
AmigaToolchainKind    amiga_toolchain_kind_from_str(const char *s);

G_END_DECLS

#endif /* KAJO_AMIGA_PROJECT_H */
