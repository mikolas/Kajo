#ifndef KAJO_AMIGA_PROJECT_H
#define KAJO_AMIGA_PROJECT_H

/* Pure GTK-free C17 manager for kajo-amiga.toml projects:
 * - Orthogonal [[target]], [[asset]], and [[profile]] tables
 * - Option D build_mode ("native" | "external") + Makefile exporter
 * - Starter project templates (68k ASM, Bare-Metal OCS, Bootable ADF, Mixed C+ASM)
 * - Toolchain probing (extra_bin_paths + adflib + SDKDIR) & compile_commands.json */

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
  AMIGA_TARGET_OS_EXE = 0,
  AMIGA_TARGET_ADF_DOS,
  AMIGA_TARGET_ADF_RAW,
} AmigaTargetKind;

typedef enum {
  AMIGA_TOOLCHAIN_ELF2HUNK = 0,
  AMIGA_TOOLCHAIN_HUNK_VLINK,
  AMIGA_TOOLCHAIN_SINGLE_ASM,
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
  char               *cpu;       /* "68000", "68010", "68020", ... */
  AmigaToolchainKind  toolchain;
  GPtrArray          *sources;     /* char* */
  GPtrArray          *includes;    /* char* */
  GPtrArray          *defines;     /* char* */
  GPtrArray          *extra_files; /* char* */
  char               *startup_sequence;
  char               *copper_symbol;
  char               *bitplane_symbol;
  uint32_t            entry_addr;  /* for adf_raw / OS-less targets */
  char               *exe_path;    /* optional override */
  char               *elf_path;    /* optional override */
} AmigaTarget;

typedef struct {
  char      *name;
  GPtrArray *inputs;  /* char* */
  GPtrArray *outputs; /* char* */
  char      *cmd;
} AmigaAsset;

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
  GPtrArray      *assets;         /* AmigaAsset* */
  GPtrArray      *profiles;       /* AmigaHwProfile* */
  char           *hrm_dir;
  char           *pre_build_cmd;
  char           *post_build_cmd;
  bool            trap86_guest_output;
} AmigaProject;

typedef struct {
  char *amiberry;
  char *vasmm68k_mot;
  char *m68k_elf_gcc;
  char *m68k_elf_ld;
  char *m68k_elf_objdump;
  char *elf2hunk;
  char *vlink;
  char *vc;
  char *m68k_lsp_server;
  char *clangd;
  char *sdk_include_dir;
  char *adflib_version;
} AmigaToolchainStatus;

AmigaProject         *amiga_project_new(void);
void                  amiga_project_free(AmigaProject *proj);

AmigaTarget          *amiga_target_new(const char *name);
void                  amiga_target_free(AmigaTarget *t);

AmigaAsset           *amiga_asset_new(const char *name);
void                  amiga_asset_free(AmigaAsset *a);

AmigaHwProfile       *amiga_hw_profile_new(const char *id);
void                  amiga_hw_profile_free(AmigaHwProfile *p);

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

char                 *amiga_project_discover_sdkdir(const char *ld_bin_path);
void                  amiga_toolchain_status_free(AmigaToolchainStatus *st);
AmigaToolchainStatus *amiga_project_probe_toolchain(const char *const *extra_bin_paths);

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
