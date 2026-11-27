#include "amiga_project.h"
#include "amiga_adf.h"
#include "../vendor/tomlc17/tomlc17.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

static void
append_toml_quoted(GString *out, const char *s)
{
  g_string_append_c(out, '"');
  if (s) {
    for (const char *p = s; *p; p++) {
      unsigned char c = (unsigned char)*p;
      if (c == '"') g_string_append(out, "\\\"");
      else if (c == '\\') g_string_append(out, "\\\\");
      else if (c == '\n') g_string_append(out, "\\n");
      else if (c == '\r') g_string_append(out, "\\r");
      else if (c == '\t') g_string_append(out, "\\t");
      else g_string_append_c(out, (char)c);
    }
  }
  g_string_append_c(out, '"');
}

static void
append_toml_str_array(GString *out, const char *key, const GPtrArray *arr)
{
  g_string_append_printf(out, "%s = [", key);
  if (arr) {
    for (guint i = 0; i < arr->len; i++) {
      if (i > 0) g_string_append(out, ", ");
      append_toml_quoted(out, g_ptr_array_index(arr, i));
    }
  }
  g_string_append(out, "]\n");
}

static void
read_toml_str_array(toml_datum_t tab, const char *key, GPtrArray *dst)
{
  toml_datum_t d = toml_get(tab, key);
  if (d.type != TOML_ARRAY) return;
  for (int32_t i = 0; i < d.u.arr.size; i++) {
    toml_datum_t el = d.u.arr.elem[i];
    if (el.type == TOML_STRING && el.u.s != NULL && el.u.s[0] != '\0') {
      g_ptr_array_add(dst, g_strdup(el.u.s));
    }
  }
}

const char *
amiga_target_kind_to_str(AmigaTargetKind k)
{
  switch (k) {
    case AMIGA_TARGET_EXE_ADF:  return "exe_adf";
    case AMIGA_TARGET_FLAT_BIN: return "flat_bin";
    case AMIGA_TARGET_EXE_DH0:
    default:
      return "exe_dh0";
  }
}

AmigaTargetKind
amiga_target_kind_from_str(const char *s)
{
  if (!s) return AMIGA_TARGET_EXE_DH0;
  if (g_strcmp0(s, "exe_adf") == 0 || g_strcmp0(s, "adf_dos") == 0) return AMIGA_TARGET_EXE_ADF;
  if (g_strcmp0(s, "flat_bin") == 0 || g_strcmp0(s, "raw_bin") == 0 ||
      g_strcmp0(s, "adf_raw") == 0) {
    return AMIGA_TARGET_FLAT_BIN;
  }
  return AMIGA_TARGET_EXE_DH0;
}

const char *
amiga_toolchain_kind_to_str(AmigaToolchainKind k)
{
  switch (k) {
    case AMIGA_TOOLCHAIN_HUNK_VLINK: return "hunk_vlink";
    case AMIGA_TOOLCHAIN_SINGLE_ASM: return "single_asm";
    case AMIGA_TOOLCHAIN_ELF2BIN:    return "elf2bin";
    case AMIGA_TOOLCHAIN_VASM_BIN:   return "vasm_bin";
    case AMIGA_TOOLCHAIN_ELF2HUNK:
    default:
      return "elf2hunk";
  }
}

AmigaToolchainKind
amiga_toolchain_kind_from_str(const char *s)
{
  if (!s) return AMIGA_TOOLCHAIN_ELF2HUNK;
  if (g_strcmp0(s, "hunk_vlink") == 0) return AMIGA_TOOLCHAIN_HUNK_VLINK;
  if (g_strcmp0(s, "single_asm") == 0) return AMIGA_TOOLCHAIN_SINGLE_ASM;
  if (g_strcmp0(s, "elf2bin") == 0) return AMIGA_TOOLCHAIN_ELF2BIN;
  if (g_strcmp0(s, "vasm_bin") == 0) return AMIGA_TOOLCHAIN_VASM_BIN;
  return AMIGA_TOOLCHAIN_ELF2HUNK;
}

AmigaTarget *
amiga_target_new(const char *name)
{
  AmigaTarget *t = g_new0(AmigaTarget, 1);
  t->name = g_strdup(name ? name : "main");
  t->kind = AMIGA_TARGET_EXE_DH0;
  t->cpu = g_strdup("68000");
  t->toolchain = AMIGA_TOOLCHAIN_ELF2HUNK;
  t->sources = g_ptr_array_new_with_free_func(g_free);
  t->includes = g_ptr_array_new_with_free_func(g_free);
  t->c_includes = g_ptr_array_new_with_free_func(g_free);
  t->asm_includes = g_ptr_array_new_with_free_func(g_free);
  t->defines = g_ptr_array_new_with_free_func(g_free);
  return t;
}

void
amiga_target_free(AmigaTarget *t)
{
  if (!t) return;
  g_free(t->name);
  g_free(t->cpu);
  if (t->sources) g_ptr_array_unref(t->sources);
  if (t->includes) g_ptr_array_unref(t->includes);
  if (t->c_includes) g_ptr_array_unref(t->c_includes);
  if (t->asm_includes) g_ptr_array_unref(t->asm_includes);
  if (t->defines) g_ptr_array_unref(t->defines);
  g_free(t->startup_sequence);
  g_free(t->copper_symbol);
  g_free(t->bitplane_symbol);
  g_free(t->entry_symbol);
  g_free(t->exe_path);
  g_free(t->elf_path);
  g_free(t->bin_path);
  g_free(t);
}

AmigaHwProfile *
amiga_hw_profile_new(const char *id)
{
  AmigaHwProfile *p = g_new0(AmigaHwProfile, 1);
  p->id = g_strdup(id ? id : "a500_ocs");
  p->model = g_strdup("A500");
  p->cpu = g_strdup("68000");
  p->chipset = g_strdup("OCS");
  p->chip_kb = 512;
  p->slow_kb = 512;
  p->fast_kb = 0;
  p->kickstart = g_strdup("kick13.rom");
  p->cycle_exact = true;
  return p;
}

void
amiga_hw_profile_free(AmigaHwProfile *p)
{
  if (!p) return;
  g_free(p->id);
  g_free(p->model);
  g_free(p->cpu);
  g_free(p->chipset);
  g_free(p->kickstart);
  g_free(p->extra_args);
  g_free(p);
}

char *
amiga_hw_profile_display_label(const AmigaHwProfile *p)
{
  if (!p) return g_strdup("A500 OCS");
  if (g_strcmp0(p->id, "a500_ocs") == 0) return g_strdup("A500 OCS");
  if (g_strcmp0(p->id, "a500p_ecs") == 0) return g_strdup("A500+ ECS");
  if (g_strcmp0(p->id, "a1200_aga") == 0) return g_strdup("A1200 AGA");
  if (g_strcmp0(p->id, "a1200_fast") == 0) return g_strdup("A1200 Fast");
  if (p->model && p->model[0] && p->chipset && p->chipset[0]) {
    return g_strdup_printf("%s %s (%s)", p->model, p->chipset, p->id ? p->id : "custom");
  }
  return g_strdup(p->id ? p->id : "custom");
}

void
amiga_project_seed_default_profiles(AmigaProject *proj)
{
  if (!proj || !proj->profiles) return;
  static const struct {
    const char *id, *model, *cpu, *chipset, *kick;
    uint32_t chip, slow, fast;
  } presets[] = {
    { "a500_ocs",   "A500",  "68000", "OCS", "kick13.rom",  512,  512, 0 },
    { "a500p_ecs",  "A500P", "68000", "ECS", "kick204.rom", 1024, 0,   0 },
    { "a1200_aga",  "A1200", "68020", "AGA", "kick31.rom",  2048, 0,   0 },
    { "a1200_fast", "A1200", "68020", "AGA", "kick31.rom",  2048, 0,   8192 },
  };
  for (size_t i = 0; i < G_N_ELEMENTS(presets); i++) {
    if (amiga_project_find_profile(proj, presets[i].id) != NULL) continue;
    AmigaHwProfile *hp = amiga_hw_profile_new(presets[i].id);
    g_free(hp->model); hp->model = g_strdup(presets[i].model);
    g_free(hp->cpu); hp->cpu = g_strdup(presets[i].cpu);
    g_free(hp->chipset); hp->chipset = g_strdup(presets[i].chipset);
    g_free(hp->kickstart); hp->kickstart = g_strdup(presets[i].kick);
    hp->chip_kb = presets[i].chip;
    hp->slow_kb = presets[i].slow;
    hp->fast_kb = presets[i].fast;
    g_ptr_array_add(proj->profiles, hp);
  }
}

AmigaProject *
amiga_project_new(void)
{
  AmigaProject *proj = g_new0(AmigaProject, 1);
  proj->build_mode = AMIGA_BUILD_NATIVE;
  proj->default_profile = g_strdup("a500_ocs");
  proj->shared_sources = g_ptr_array_new_with_free_func(g_free);
  proj->targets = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_target_free);
  proj->profiles = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_hw_profile_free);
  proj->trap86_guest_output = true;
  amiga_project_seed_default_profiles(proj);
  return proj;
}

void
amiga_project_free(AmigaProject *proj)
{
  if (!proj) return;
  g_free(proj->root_dir);
  g_free(proj->manifest_path);
  g_free(proj->name);
  g_free(proj->build_cmd);
  g_free(proj->clean_cmd);
  g_free(proj->default_target);
  g_free(proj->default_profile);
  if (proj->shared_sources) g_ptr_array_unref(proj->shared_sources);
  if (proj->targets) g_ptr_array_unref(proj->targets);
  if (proj->profiles) g_ptr_array_unref(proj->profiles);
  g_free(proj->hrm_dir);
  g_free(proj->pre_build_cmd);
  g_free(proj->post_build_cmd);
  g_free(proj);
}

const AmigaTarget *
amiga_project_find_target(const AmigaProject *proj, const char *name)
{
  if (!proj || !proj->targets) return NULL;
  if (!name && proj->targets->len > 0) return g_ptr_array_index(proj->targets, 0);
  for (guint i = 0; i < proj->targets->len; i++) {
    const AmigaTarget *t = g_ptr_array_index(proj->targets, i);
    if (g_strcmp0(t->name, name) == 0) return t;
  }
  return NULL;
}

const AmigaHwProfile *
amiga_project_find_profile(const AmigaProject *proj, const char *id)
{
  if (!proj || !proj->profiles || !id) return NULL;
  for (guint i = 0; i < proj->profiles->len; i++) {
    const AmigaHwProfile *p = g_ptr_array_index(proj->profiles, i);
    if (g_strcmp0(p->id, id) == 0) return p;
  }
  return NULL;
}

static char *
toml_dup_str(toml_datum_t tab, const char *key)
{
  toml_datum_t d = toml_get(tab, key);
  return (d.type == TOML_STRING && d.u.s != NULL) ? g_strdup(d.u.s) : NULL;
}

bool
amiga_project_parse_toml(const char    *toml_str,
                         const char    *root_dir,
                         AmigaProject **out_proj,
                         GError       **error)
{
  if (!toml_str || !out_proj) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid TOML input");
    return false;
  }
  *out_proj = NULL;
  toml_result_t res = toml_parse(toml_str, (int)strlen(toml_str));
  if (!res.ok) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "kajo-amiga.toml parse error: %s", res.errmsg);
    toml_free(res);
    return false;
  }

  AmigaProject *proj = amiga_project_new();
  proj->root_dir = g_strdup(root_dir ? root_dir : ".");
  proj->manifest_path = g_build_filename(proj->root_dir, "kajo-amiga.toml", NULL);

  toml_datum_t pt = toml_get(res.toptab, "project");
  if (pt.type == TOML_TABLE) {
    proj->name = toml_dup_str(pt, "name");
    g_autofree char *bm = toml_dup_str(pt, "build_mode");
    if (g_strcmp0(bm, "external") == 0) proj->build_mode = AMIGA_BUILD_EXTERNAL;
    proj->build_cmd = toml_dup_str(pt, "build_cmd");
    proj->clean_cmd = toml_dup_str(pt, "clean_cmd");
    proj->default_target = toml_dup_str(pt, "default_target");
    g_autofree char *dp = toml_dup_str(pt, "default_profile");
    if (dp && dp[0]) { g_free(proj->default_profile); proj->default_profile = g_steal_pointer(&dp); }
    proj->pre_build_cmd = toml_dup_str(pt, "pre_build_cmd");
    proj->post_build_cmd = toml_dup_str(pt, "post_build_cmd");
    toml_datum_t t86 = toml_get(pt, "trap86_guest_output");
    if (t86.type == TOML_BOOLEAN) proj->trap86_guest_output = t86.u.boolean;
    read_toml_str_array(pt, "shared_sources", proj->shared_sources);
  }
  if (!proj->name) proj->name = g_path_get_basename(proj->root_dir);

  toml_datum_t docs = toml_get(res.toptab, "docs");
  if (docs.type == TOML_TABLE) proj->hrm_dir = toml_dup_str(docs, "hrm_dir");

  toml_datum_t targets = toml_get(res.toptab, "target");
  if (targets.type == TOML_ARRAY) {
    for (int32_t i = 0; i < targets.u.arr.size; i++) {
      toml_datum_t tt = targets.u.arr.elem[i];
      if (tt.type != TOML_TABLE) continue;
      g_autofree char *tname = toml_dup_str(tt, "name");
      AmigaTarget *t = amiga_target_new(tname ? tname : "target");
      g_autofree char *kstr = toml_dup_str(tt, "kind");
      g_autofree char *fstr = toml_dup_str(tt, "format");
      if (kstr) t->kind = amiga_target_kind_from_str(kstr);
      if (fstr && g_strcmp0(fstr, "raw_bin") == 0) t->kind = AMIGA_TARGET_FLAT_BIN;
      g_autofree char *cstr = toml_dup_str(tt, "cpu");
      if (cstr) { g_free(t->cpu); t->cpu = g_steal_pointer(&cstr); }
      g_autofree char *tc = toml_dup_str(tt, "toolchain");
      if (tc) t->toolchain = amiga_toolchain_kind_from_str(tc);
      read_toml_str_array(tt, "sources", t->sources);
      read_toml_str_array(tt, "includes", t->includes);
      read_toml_str_array(tt, "c_includes", t->c_includes);
      read_toml_str_array(tt, "asm_includes", t->asm_includes);
      read_toml_str_array(tt, "defines", t->defines);
      t->startup_sequence = toml_dup_str(tt, "startup_sequence");
      t->copper_symbol = toml_dup_str(tt, "copper_symbol");
      t->bitplane_symbol = toml_dup_str(tt, "bitplane_symbol");
      t->entry_symbol = toml_dup_str(tt, "entry_symbol");
      t->exe_path = toml_dup_str(tt, "exe_path");
      t->elf_path = toml_dup_str(tt, "elf_path");
      t->bin_path = toml_dup_str(tt, "bin_path");
      toml_datum_t la = toml_get(tt, "load_addr");
      if (la.type == TOML_INT64 && la.u.int64 >= 0) t->load_addr = (uint32_t)la.u.int64;
      g_ptr_array_add(proj->targets, t);
    }
  }

  toml_datum_t profiles = toml_get(res.toptab, "profile");
  if (profiles.type == TOML_ARRAY) {
    for (int32_t i = 0; i < profiles.u.arr.size; i++) {
      toml_datum_t ptab = profiles.u.arr.elem[i];
      if (ptab.type != TOML_TABLE) continue;
      g_autofree char *pid = toml_dup_str(ptab, "id");
      if (!pid || !pid[0]) continue;
      AmigaHwProfile *hp = (AmigaHwProfile *)amiga_project_find_profile(proj, pid);
      if (!hp) {
        hp = amiga_hw_profile_new(pid);
        g_ptr_array_add(proj->profiles, hp);
      }
      g_autofree char *m = toml_dup_str(ptab, "model");
      if (m) { g_free(hp->model); hp->model = g_steal_pointer(&m); }
      g_autofree char *c = toml_dup_str(ptab, "cpu");
      if (c) { g_free(hp->cpu); hp->cpu = g_steal_pointer(&c); }
      g_autofree char *cs = toml_dup_str(ptab, "chipset");
      if (cs) { g_free(hp->chipset); hp->chipset = g_steal_pointer(&cs); }
      g_autofree char *k = toml_dup_str(ptab, "kickstart");
      if (k) { g_free(hp->kickstart); hp->kickstart = g_steal_pointer(&k); }
      toml_datum_t d;
      d = toml_get(ptab, "chip_kb"); if (d.type == TOML_INT64 && d.u.int64 > 0) hp->chip_kb = (uint32_t)d.u.int64;
      d = toml_get(ptab, "slow_kb"); if (d.type == TOML_INT64 && d.u.int64 >= 0) hp->slow_kb = (uint32_t)d.u.int64;
      d = toml_get(ptab, "fast_kb"); if (d.type == TOML_INT64 && d.u.int64 >= 0) hp->fast_kb = (uint32_t)d.u.int64;
      d = toml_get(ptab, "ntsc"); if (d.type == TOML_BOOLEAN) hp->ntsc = d.u.boolean;
      d = toml_get(ptab, "cycle_exact"); if (d.type == TOML_BOOLEAN) hp->cycle_exact = d.u.boolean;
      g_free(hp->extra_args);
      hp->extra_args = toml_dup_str(ptab, "extra_args");
    }
  }

  toml_free(res);
  *out_proj = proj;
  return true;
}

AmigaProject *
amiga_project_load_dir(const char *dir, GError **error)
{
  if (!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Directory not found: '%s'", dir ? dir : "");
    return NULL;
  }

  g_autofree char *primary = g_build_filename(dir, "kajo-amiga.toml", NULL);
  g_autofree char *alias = g_build_filename(dir, ".kajo-amiga.toml", NULL);
  const char *manifest = g_file_test(primary, G_FILE_TEST_IS_REGULAR) ? primary
                       : g_file_test(alias, G_FILE_TEST_IS_REGULAR) ? alias : NULL;
  if (manifest) {
    g_autofree char *contents = NULL;
    if (!g_file_get_contents(manifest, &contents, NULL, error)) return NULL;
    AmigaProject *proj = NULL;
    if (!amiga_project_parse_toml(contents, dir, &proj, error)) return NULL;
    g_free(proj->manifest_path);
    proj->manifest_path = g_strdup(manifest);
    return proj;
  }

  /* Clean external Makefile fallback: single explicit default target, no directory guessing. */
  g_autofree char *makefile = g_build_filename(dir, "Makefile", NULL);
  if (!g_file_test(makefile, G_FILE_TEST_IS_REGULAR)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Neither kajo-amiga.toml nor Makefile found in '%s'", dir);
    return NULL;
  }

  AmigaProject *proj = amiga_project_new();
  proj->root_dir = g_strdup(dir);
  proj->manifest_path = g_strdup(primary);
  proj->name = g_path_get_basename(dir);
  proj->build_mode = AMIGA_BUILD_EXTERNAL;
  proj->build_cmd = g_strdup("make");
  proj->clean_cmd = g_strdup("make clean");

  AmigaTarget *t = amiga_target_new(proj->name ? proj->name : "default");
  t->kind = AMIGA_TARGET_EXE_DH0;
  g_ptr_array_add(t->includes, g_strdup("."));
  g_ptr_array_add(proj->targets, t);
  proj->default_target = g_strdup(t->name);
  return proj;
}

char *
amiga_project_serialize_toml(const AmigaProject *proj)
{
  if (!proj) return NULL;
  GString *out = g_string_new("[project]\n");
  g_string_append(out, "name = "); append_toml_quoted(out, proj->name ? proj->name : "amiga_app");
  g_string_append_printf(out, "\nbuild_mode = \"%s\"\n",
                         proj->build_mode == AMIGA_BUILD_EXTERNAL ? "external" : "native");
  if (proj->build_cmd) { g_string_append(out, "build_cmd = "); append_toml_quoted(out, proj->build_cmd); g_string_append_c(out, '\n'); }
  if (proj->clean_cmd) { g_string_append(out, "clean_cmd = "); append_toml_quoted(out, proj->clean_cmd); g_string_append_c(out, '\n'); }
  if (proj->pre_build_cmd) { g_string_append(out, "pre_build_cmd = "); append_toml_quoted(out, proj->pre_build_cmd); g_string_append_c(out, '\n'); }
  if (proj->post_build_cmd) { g_string_append(out, "post_build_cmd = "); append_toml_quoted(out, proj->post_build_cmd); g_string_append_c(out, '\n'); }
  if (proj->default_target) { g_string_append(out, "default_target = "); append_toml_quoted(out, proj->default_target); g_string_append_c(out, '\n'); }
  if (proj->default_profile) { g_string_append(out, "default_profile = "); append_toml_quoted(out, proj->default_profile); g_string_append_c(out, '\n'); }
  g_string_append_printf(out, "trap86_guest_output = %s\n", proj->trap86_guest_output ? "true" : "false");
  if (proj->shared_sources && proj->shared_sources->len > 0) {
    append_toml_str_array(out, "shared_sources", proj->shared_sources);
  }
  if (proj->hrm_dir) {
    g_string_append(out, "\n[docs]\nhrm_dir = ");
    append_toml_quoted(out, proj->hrm_dir);
    g_string_append_c(out, '\n');
  }

  for (guint i = 0; proj->targets && i < proj->targets->len; i++) {
    const AmigaTarget *t = g_ptr_array_index(proj->targets, i);
    g_string_append(out, "\n[[target]]\n");
    g_string_append(out, "name = "); append_toml_quoted(out, t->name);
    g_string_append_printf(out, "\nkind = \"%s\"\n", amiga_target_kind_to_str(t->kind));
    g_string_append(out, "cpu = "); append_toml_quoted(out, t->cpu ? t->cpu : "68000");
    g_string_append_printf(out, "\ntoolchain = \"%s\"\n", amiga_toolchain_kind_to_str(t->toolchain));
    append_toml_str_array(out, "sources", t->sources);
    if (t->includes && t->includes->len > 0) append_toml_str_array(out, "includes", t->includes);
    if (t->c_includes && t->c_includes->len > 0) append_toml_str_array(out, "c_includes", t->c_includes);
    if (t->asm_includes && t->asm_includes->len > 0) append_toml_str_array(out, "asm_includes", t->asm_includes);
    if (t->defines && t->defines->len > 0) append_toml_str_array(out, "defines", t->defines);
    if (t->startup_sequence) { g_string_append(out, "startup_sequence = "); append_toml_quoted(out, t->startup_sequence); g_string_append_c(out, '\n'); }
    if (t->copper_symbol) { g_string_append(out, "copper_symbol = "); append_toml_quoted(out, t->copper_symbol); g_string_append_c(out, '\n'); }
    if (t->bitplane_symbol) { g_string_append(out, "bitplane_symbol = "); append_toml_quoted(out, t->bitplane_symbol); g_string_append_c(out, '\n'); }
    if (t->entry_symbol) { g_string_append(out, "entry_symbol = "); append_toml_quoted(out, t->entry_symbol); g_string_append_c(out, '\n'); }
    if (t->exe_path) { g_string_append(out, "exe_path = "); append_toml_quoted(out, t->exe_path); g_string_append_c(out, '\n'); }
    if (t->elf_path) { g_string_append(out, "elf_path = "); append_toml_quoted(out, t->elf_path); g_string_append_c(out, '\n'); }
    if (t->bin_path) { g_string_append(out, "bin_path = "); append_toml_quoted(out, t->bin_path); g_string_append_c(out, '\n'); }
    if (t->load_addr != 0) g_string_append_printf(out, "load_addr = %u\n", t->load_addr);
  }

  return g_string_free(out, FALSE);
}

bool
amiga_project_save(const AmigaProject *proj, GError **error)
{
  if (!proj || !proj->manifest_path) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing project manifest_path");
    return false;
  }
  g_autofree char *dir = g_path_get_dirname(proj->manifest_path);
  g_mkdir_with_parents(dir, 0755);
  g_autofree char *txt = amiga_project_serialize_toml(proj);
  return g_file_set_contents(proj->manifest_path, txt, -1, error);
}

bool
amiga_project_init_template(const char       *dir,
                            const char       *name,
                            AmigaTemplateKind tpl,
                            const char       *default_profile,
                            AmigaTargetKind   pkg_kind,
                            GError          **error)
{
  if (!dir || !name || !name[0]) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing project directory or name");
    return false;
  }
  g_autofree char *src_dir = g_build_filename(dir, "src", NULL);
  g_mkdir_with_parents(src_dir, 0755);

  AmigaProject *proj = amiga_project_new();
  proj->root_dir = g_strdup(dir);
  proj->manifest_path = g_build_filename(dir, "kajo-amiga.toml", NULL);
  proj->name = g_strdup(name);
  proj->default_target = g_strdup(name);
  g_free(proj->default_profile);
  proj->default_profile = g_strdup(default_profile ? default_profile : "a500_ocs");

  AmigaTarget *t = amiga_target_new(name);
  t->kind = pkg_kind;
  g_ptr_array_add(t->includes, g_strdup("."));
  if (pkg_kind == AMIGA_TARGET_FLAT_BIN) {
    t->toolchain = AMIGA_TOOLCHAIN_VASM_BIN;
  }

  g_autofree char *main_file = NULL;
  const char *main_code = NULL;

  if (tpl == AMIGA_TEMPLATE_MIXED_C_ASM) {
    g_ptr_array_add(t->sources, g_strdup("src/main.c"));
    g_ptr_array_add(t->sources, g_strdup("src/copper.asm"));
    main_file = g_build_filename(src_dir, "main.c", NULL);
    main_code =
        "/* Mixed C + 68k ASM starter for kajo-amiga */\n"
        "#include <stdint.h>\n"
        "extern void install_copper(void);\n"
        "int main(void) {\n"
        "    install_copper();\n"
        "    while (1) {}\n"
        "    return 0;\n"
        "}\n";
    g_autofree char *cop_file = g_build_filename(src_dir, "copper.asm", NULL);
    const char *cop_code =
        "    section .text,code\n"
        "    xdef    _install_copper\n"
        "_install_copper:\n"
        "    rts\n";
    if (!g_file_set_contents(cop_file, cop_code, -1, error)) {
      amiga_project_free(proj);
      return false;
    }
  } else {
    g_ptr_array_add(t->sources, g_strdup("src/main.asm"));
    main_file = g_build_filename(src_dir, "main.asm", NULL);
    if (tpl == AMIGA_TEMPLATE_BARE_OCS) {
      t->copper_symbol = g_strdup("copperlist");
      main_code =
          "; Bare-Metal OCS Starter\n"
          "    section .text,code\n"
          "    xdef    _start\n"
          "_start:\n"
          "    lea     $dff000,a5\n"
          "    lea     copperlist,a0\n"
          "    move.l  a0,$080(a5)\n"
          ".loop:\n"
          "    bra.s   .loop\n\n"
          "    section .data_chip,data\n"
          "    xdef    copperlist\n"
          "copperlist:\n"
          "    dc.w    $0180,$0012\n"
          "    dc.w    $ffff,$fffe\n";
    } else {
      main_code =
          "; AmigaDOS 68k ASM Starter\n"
          "    section .text,code\n"
          "    xdef    _start\n"
          "_start:\n"
          "    moveq   #0,d0\n"
          "    rts\n";
    }
  }

  if (!g_file_set_contents(main_file, main_code, -1, error)) {
    amiga_project_free(proj);
    return false;
  }

  g_ptr_array_add(proj->targets, t);
  bool ok = amiga_project_save(proj, error);
  if (ok) {
    g_autofree char *gi_path = g_build_filename(dir, ".gitignore", NULL);
    g_file_set_contents(gi_path, "obj/\nout/\n.kajo-amiga/\n*.adf\n", -1, NULL);
  }
  amiga_project_free(proj);
  return ok;
}

static char *
find_tool_in_paths(const char *tool, const char *const *extra_paths)
{
  char *in_path = g_find_program_in_path(tool);
  if (in_path) return in_path;
  if (!extra_paths) return NULL;
  for (size_t i = 0; extra_paths[i] != NULL; i++) {
    if (!extra_paths[i][0]) continue;
    g_autofree char *cand = g_build_filename(extra_paths[i], tool, NULL);
    if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) return g_steal_pointer(&cand);
  }
  return NULL;
}

static void
append_unique_dir(GPtrArray *arr, const char *dir, bool require_exists)
{
  if (!arr || !dir || !dir[0]) return;
  g_autofree char *norm = g_path_is_absolute(dir) ? g_canonicalize_filename(dir, NULL) : g_strdup(dir);
  if (require_exists && !g_file_test(norm, G_FILE_TEST_IS_DIR)) return;
  for (guint i = 0; i < arr->len; i++) {
    if (g_strcmp0(g_ptr_array_index(arr, i), norm) == 0) return;
  }
  g_ptr_array_add(arr, g_steal_pointer(&norm));
}

static void
append_colon_dirs(GPtrArray *arr, const char *colon_list)
{
  if (!arr || !colon_list || !colon_list[0]) return;
  g_auto(GStrv) parts = g_strsplit(colon_list, ":", -1);
  for (int i = 0; parts[i] != NULL; i++) {
    char *s = g_strstrip(parts[i]);
    if (s[0]) append_unique_dir(arr, s, false);
  }
}

static void
probe_rel_dir_from_bin(GPtrArray *arr, const char *bin_path, const char *rel1, const char *rel2, const char *marker)
{
  if (!bin_path || !bin_path[0]) return;
  g_autofree char *bin_dir = g_path_get_dirname(bin_path);
  g_autofree char *cand = g_build_filename(bin_dir, "..", rel1, rel2, NULL);
  g_autofree char *norm = g_canonicalize_filename(cand, NULL);
  if (!g_file_test(norm, G_FILE_TEST_IS_DIR)) return;
  if (marker) {
    g_autofree char *m = g_build_filename(norm, marker, NULL);
    if (!g_file_test(m, G_FILE_TEST_IS_REGULAR)) return;
  }
  append_unique_dir(arr, norm, true);
}

void
amiga_toolchain_status_free(AmigaToolchainStatus *st)
{
  if (!st) return;
  g_free(st->amiberry);
  g_free(st->vasmm68k_mot);
  g_free(st->m68k_elf_gcc);
  g_free(st->m68k_elf_ld);
  g_free(st->m68k_elf_objdump);
  g_free(st->m68k_elf_objcopy);
  g_free(st->elf2hunk);
  g_free(st->vlink);
  g_free(st->vc);
  g_free(st->m68k_lsp_server);
  g_free(st->clangd);
  if (st->c_include_dirs) g_ptr_array_unref(st->c_include_dirs);
  if (st->asm_include_dirs) g_ptr_array_unref(st->asm_include_dirs);
  g_free(st->adflib_version);
  g_free(st);
}

AmigaToolchainStatus *
amiga_project_probe_toolchain_ex(const char *const *extra_bin_paths,
                                 const char        *c_include_dirs,
                                 const char        *asm_include_dirs)
{
  AmigaToolchainStatus *st = g_new0(AmigaToolchainStatus, 1);
  st->amiberry = find_tool_in_paths("amiberry", extra_bin_paths);
  st->vasmm68k_mot = find_tool_in_paths("vasmm68k_mot", extra_bin_paths);
  st->m68k_elf_gcc = find_tool_in_paths("m68k-amiga-elf-gcc", extra_bin_paths);
  st->m68k_elf_ld = find_tool_in_paths("m68k-amiga-elf-ld", extra_bin_paths);
  st->m68k_elf_objdump = find_tool_in_paths("m68k-amiga-elf-objdump", extra_bin_paths);
  st->m68k_elf_objcopy = find_tool_in_paths("m68k-amiga-elf-objcopy", extra_bin_paths);
  st->elf2hunk = find_tool_in_paths("elf2hunk", extra_bin_paths);
  st->vlink = find_tool_in_paths("vlink", extra_bin_paths);
  st->vc = find_tool_in_paths("vc", extra_bin_paths);
  st->m68k_lsp_server = find_tool_in_paths("m68k-lsp-server", extra_bin_paths);
  st->clangd = find_tool_in_paths("clangd", extra_bin_paths);
  st->adflib_version = g_strdup(amiga_adf_lib_version());

  st->c_include_dirs = g_ptr_array_new_with_free_func(g_free);
  st->asm_include_dirs = g_ptr_array_new_with_free_func(g_free);

  /* 1. Explicit configured C (.h) include dirs, else auto-probe GCC / VBCC layouts */
  append_colon_dirs(st->c_include_dirs, c_include_dirs);
  if (st->c_include_dirs->len == 0) {
    const char *gcc_ref = st->m68k_elf_gcc ? st->m68k_elf_gcc : st->m68k_elf_ld;
    probe_rel_dir_from_bin(st->c_include_dirs, gcc_ref, "m68k-amiga-elf", "sys-include", NULL);
    probe_rel_dir_from_bin(st->c_include_dirs, gcc_ref, "m68k-amigaos", "ndk-include", NULL);
    const char *vbcc = g_getenv("VBCC");
    if (vbcc && vbcc[0]) {
      g_autofree char *vinc = g_build_filename(vbcc, "targets", "m68k-amigaos", "include", NULL);
      append_unique_dir(st->c_include_dirs, vinc, true);
    }
    probe_rel_dir_from_bin(st->c_include_dirs, st->vc, "targets", "m68k-amigaos/include", NULL);
  }

  /* 2. Explicit configured ASM (.i) include dirs, else auto-probe NDK .i layouts */
  append_colon_dirs(st->asm_include_dirs, asm_include_dirs);
  if (st->asm_include_dirs->len == 0) {
    append_unique_dir(st->asm_include_dirs, g_getenv("NDK_INC"), true);
    const char *andk = g_getenv("AMIGA_NDK");
    if (andk && andk[0]) {
      g_autofree char *up = g_build_filename(andk, "Include_i", NULL);
      g_autofree char *lo = g_build_filename(andk, "include_i", NULL);
      append_unique_dir(st->asm_include_dirs, up, true);
      append_unique_dir(st->asm_include_dirs, lo, true);
    }
    const char *asm_ref = st->vasmm68k_mot ? st->vasmm68k_mot
                        : (st->m68k_elf_gcc ? st->m68k_elf_gcc : st->m68k_elf_ld);
    probe_rel_dir_from_bin(st->asm_include_dirs, asm_ref, "Include_i", NULL, NULL);
    probe_rel_dir_from_bin(st->asm_include_dirs, asm_ref, "include_i", NULL, NULL);
    probe_rel_dir_from_bin(st->asm_include_dirs, asm_ref, "asm-include", NULL, NULL);
    probe_rel_dir_from_bin(st->asm_include_dirs, asm_ref, "m68k-amigaos", "ndk-include", "exec/types.i");
  }

  return st;
}

AmigaToolchainStatus *
amiga_project_probe_toolchain(const char *const *extra_bin_paths)
{
  return amiga_project_probe_toolchain_ex(extra_bin_paths, NULL, NULL);
}

static void
append_inc_list(GPtrArray *dst, const GPtrArray *src, const char *root_dir, bool make_abs)
{
  if (!src) return;
  for (guint i = 0; i < src->len; i++) {
    const char *inc = g_ptr_array_index(src, i);
    if (!inc || !inc[0] || g_strcmp0(inc, "$SDKDIR") == 0) continue;
    g_autofree char *resolved = (make_abs && !g_path_is_absolute(inc) && root_dir && root_dir[0])
                                    ? g_canonicalize_filename(inc, root_dir)
                                    : g_strdup(inc);
    bool seen = false;
    for (guint j = 0; j < dst->len; j++) {
      if (g_strcmp0(g_ptr_array_index(dst, j), resolved) == 0) { seen = true; break; }
    }
    if (!seen) g_ptr_array_add(dst, g_steal_pointer(&resolved));
  }
}

bool
amiga_project_collect_target_includes(const AmigaProject         *proj,
                                      const AmigaTarget          *target,
                                      const AmigaToolchainStatus *tc,
                                      bool                        for_asm,
                                      bool                        make_abs,
                                      GPtrArray                  *out_dirs)
{
  if (!out_dirs) return false;
  const char *root = (proj && proj->root_dir) ? proj->root_dir : NULL;
  if (target) {
    append_inc_list(out_dirs, target->includes, root, make_abs);
    append_inc_list(out_dirs, for_asm ? target->asm_includes : target->c_includes, root, make_abs);
  }
  if (tc) {
    append_inc_list(out_dirs, for_asm ? tc->asm_include_dirs : tc->c_include_dirs, root, make_abs);
  }
  if (out_dirs->len == 0) {
    g_ptr_array_add(out_dirs, make_abs && root ? g_strdup(root) : g_strdup("."));
  }
  return true;
}

static bool
is_c_file(const char *path)
{
  return path && (g_str_has_suffix(path, ".c") || g_str_has_suffix(path, ".C"));
}

char *
amiga_project_build_compile_commands_json(const AmigaProject         *proj,
                                          const AmigaToolchainStatus *tc)
{
  if (!proj) return g_strdup("[]");
  g_autoptr(JsonBuilder) jb = json_builder_new();
  json_builder_begin_array(jb);

  const char *gcc = (tc && tc->m68k_elf_gcc) ? tc->m68k_elf_gcc : "m68k-amiga-elf-gcc";
  const char *dir = proj->root_dir ? proj->root_dir : ".";

  for (guint ti = 0; proj->targets && ti < proj->targets->len; ti++) {
    const AmigaTarget *t = g_ptr_array_index(proj->targets, ti);
    g_autoptr(GPtrArray) c_incs = g_ptr_array_new_with_free_func(g_free);
    amiga_project_collect_target_includes(proj, t, tc, false, false, c_incs);

    for (guint si = 0; t->sources && si < t->sources->len; si++) {
      const char *src = g_ptr_array_index(t->sources, si);
      if (!is_c_file(src)) continue;

      g_autoptr(GString) cmd = g_string_new(gcc);
      g_string_append_printf(cmd, " -c -g -MMD -mcpu=%s", t->cpu ? t->cpu : "68000");
      for (guint ii = 0; ii < c_incs->len; ii++) {
        g_string_append_printf(cmd, " -I%s", (const char *)g_ptr_array_index(c_incs, ii));
      }
      for (guint di = 0; t->defines && di < t->defines->len; di++) {
        g_string_append_printf(cmd, " -D%s", (const char *)g_ptr_array_index(t->defines, di));
      }
      g_string_append_printf(cmd, " %s", src);

      json_builder_begin_object(jb);
      json_builder_set_member_name(jb, "directory");
      json_builder_add_string_value(jb, dir);
      json_builder_set_member_name(jb, "command");
      json_builder_add_string_value(jb, cmd->str);
      json_builder_set_member_name(jb, "file");
      json_builder_add_string_value(jb, src);
      json_builder_end_object(jb);
    }
  }

  json_builder_end_array(jb);
  g_autoptr(JsonNode) root = json_builder_get_root(jb);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_root(gen, root);
  return json_generator_to_data(gen, NULL);
}

bool
amiga_project_write_compile_commands(const AmigaProject         *proj,
                                     const AmigaToolchainStatus *tc,
                                     GError                    **error)
{
  if (!proj || !proj->root_dir) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing project root_dir");
    return false;
  }
  g_autofree char *meta_dir = g_build_filename(proj->root_dir, ".kajo-amiga", NULL);
  g_mkdir_with_parents(meta_dir, 0755);
  g_autofree char *out_path = g_build_filename(meta_dir, "compile_commands.json", NULL);
  g_autofree char *json = amiga_project_build_compile_commands_json(proj, tc);
  return g_file_set_contents(out_path, json, -1, error);
}

char *
amiga_project_export_makefile(const AmigaProject         *proj,
                              const AmigaToolchainStatus *tc)
{
  if (!proj) return NULL;
  const AmigaTarget *t = amiga_project_find_target(proj, proj->default_target);
  GString *mf = g_string_new("# Generated by kajo-amiga from kajo-amiga.toml\n");
  g_string_append(mf, "VASM = vasmm68k_mot\nGCC  = m68k-amiga-elf-gcc\nLD   = m68k-amiga-elf-ld\n\n");
  if (!t) return g_string_free(mf, FALSE);

  const char *cpu = t->cpu ? t->cpu : "68000";
  g_autoptr(GPtrArray) asm_incs = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) c_incs = g_ptr_array_new_with_free_func(g_free);
  amiga_project_collect_target_includes(proj, t, tc, true, false, asm_incs);
  amiga_project_collect_target_includes(proj, t, tc, false, false, c_incs);

  g_string_append_printf(mf, "TARGET = %s\nOUT    = out/$(TARGET)\n", t->name);
  g_string_append_printf(mf, "VASMFLAGS = -Felf -m%s -quiet -dwarf=3 -x", cpu);
  for (guint i = 0; i < asm_incs->len; i++) {
    g_string_append_printf(mf, " -I%s", (const char *)g_ptr_array_index(asm_incs, i));
  }
  g_string_append_printf(mf, "\nCFLAGS    = -g -O2 -mcpu=%s", cpu);
  for (guint i = 0; i < c_incs->len; i++) {
    g_string_append_printf(mf, " -I%s", (const char *)g_ptr_array_index(c_incs, i));
  }
  g_string_append(mf, "\n\nall: $(OUT).exe\n\n$(OUT).exe: $(OUT).elf\n\t@mkdir -p out\n\telf2hunk $< $@\n\n");
  g_string_append(mf, "clean:\n\trm -rf obj/* out/*\n");
  return g_string_free(mf, FALSE);
}

char *
amiga_project_check_profile_compat(const AmigaTarget    *target,
                                   const AmigaHwProfile *profile)
{
  if (!target || !profile) return NULL;
  const char *t_cpu = target->cpu ? target->cpu : "68000";
  const char *p_cpu = profile->cpu ? profile->cpu : "68000";
  if (g_strcmp0(t_cpu, "68000") != 0 && g_strcmp0(p_cpu, "68000") == 0) {
    return g_strdup_printf(
        "Target '%s' compiles for %s, but profile '%s' emulates a %s (%s). "
        "Switch profile to A1200/020 or set cpu = \"68000\".",
        target->name, t_cpu, profile->id, p_cpu, profile->model ? profile->model : "A500");
  }
  return NULL;
}
