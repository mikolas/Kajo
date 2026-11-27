#include "../src/amiga/amiga_project.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>

static void
rm_rf(const char *path)
{
  if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) return;
  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    GDir *d = g_dir_open(path, 0, NULL);
    if (d) {
      const char *name;
      while ((name = g_dir_read_name(d)) != NULL) {
        g_autofree char *child = g_build_filename(path, name, NULL);
        rm_rf(child);
      }
      g_dir_close(d);
    }
    g_rmdir(path);
  } else {
    g_unlink(path);
  }
}

static bool
ptr_array_contains_str(const GPtrArray *arr, const char *needle)
{
  for (guint i = 0; arr && i < arr->len; i++) {
    if (g_strcmp0(g_ptr_array_index(arr, i), needle) == 0) return true;
  }
  return false;
}

static void
test_toml_roundtrip_and_presets(void)
{
  AmigaProject *proj = amiga_project_new();
  proj->name = g_strdup("CopperTest");
  proj->build_mode = AMIGA_BUILD_NATIVE;
  proj->default_target = g_strdup("floppy");
  proj->pre_build_cmd = g_strdup("echo pre");
  proj->post_build_cmd = g_strdup("echo post");
  g_free(proj->default_profile);
  proj->default_profile = g_strdup("a500_ocs");
  proj->trap86_guest_output = true;
  g_ptr_array_add(proj->shared_sources, g_strdup("src/common.c"));
  amiga_project_seed_default_profiles(proj);
  g_assert_cmpuint(proj->profiles->len, ==, 4);

  const AmigaHwProfile *p_ocs = amiga_project_find_profile(proj, "a500_ocs");
  const AmigaHwProfile *p_aga = amiga_project_find_profile(proj, "a1200_aga");
  g_assert_nonnull(p_ocs);
  g_assert_nonnull(p_aga);
  g_autofree char *lbl_ocs = amiga_hw_profile_display_label(p_ocs);
  g_autofree char *lbl_aga = amiga_hw_profile_display_label(p_aga);
  g_assert_cmpstr(lbl_ocs, ==, "A500 OCS");
  g_assert_cmpstr(lbl_aga, ==, "A1200 AGA");

  AmigaTarget *t = amiga_target_new("floppy");
  t->kind = AMIGA_TARGET_EXE_ADF;
  g_free(t->cpu);
  t->cpu = g_strdup("68000");
  t->toolchain = AMIGA_TOOLCHAIN_ELF2HUNK;
  t->entry_symbol = g_strdup("_start");
  t->load_addr = 0x10000u;
  t->exe_path = g_strdup("out/floppy.exe");
  g_ptr_array_add(t->sources, g_strdup("src/main.c"));
  g_ptr_array_add(t->sources, g_strdup("src/irq.s"));
  g_ptr_array_add(t->includes, g_strdup("."));
  g_ptr_array_add(t->c_includes, g_strdup("include/c"));
  g_ptr_array_add(t->asm_includes, g_strdup("include/asm"));
  g_ptr_array_add(t->defines, g_strdup("DEBUG=1"));
  t->startup_sequence = g_strdup("floppy.exe\n");
  t->copper_symbol = g_strdup("_copper_list");
  t->bitplane_symbol = g_strdup("_bpl0");
  g_ptr_array_add(proj->targets, t);

  g_autofree char *toml = amiga_project_serialize_toml(proj);
  g_assert_nonnull(toml);

  g_autoptr(GError) err = NULL;
  AmigaProject *parsed = NULL;
  g_assert_true(amiga_project_parse_toml(toml, "/tmp/copper-test", &parsed, &err));
  g_assert_no_error(err);
  g_assert_nonnull(parsed);
  g_assert_cmpstr(parsed->name, ==, "CopperTest");
  g_assert_cmpint(parsed->build_mode, ==, AMIGA_BUILD_NATIVE);
  g_assert_true(parsed->trap86_guest_output);
  g_assert_cmpuint(parsed->shared_sources->len, ==, 1);
  g_assert_cmpuint(parsed->targets->len, ==, 1);
  g_assert_cmpuint(parsed->profiles->len, ==, 4);

  const AmigaTarget *pt = amiga_project_find_target(parsed, "floppy");
  g_assert_nonnull(pt);
  g_assert_cmpint(pt->kind, ==, AMIGA_TARGET_EXE_ADF);
  g_assert_cmpint(pt->toolchain, ==, AMIGA_TOOLCHAIN_ELF2HUNK);
  g_assert_cmpuint(pt->sources->len, ==, 2);
  g_assert_true(ptr_array_contains_str(pt->c_includes, "include/c"));
  g_assert_true(ptr_array_contains_str(pt->asm_includes, "include/asm"));
  g_assert_cmpuint(pt->load_addr, ==, 0x10000u);
  g_assert_cmpstr(pt->entry_symbol, ==, "_start");
  g_assert_cmpstr(pt->exe_path, ==, "out/floppy.exe");
  g_assert_cmpstr(parsed->pre_build_cmd, ==, "echo pre");
  g_assert_cmpstr(parsed->post_build_cmd, ==, "echo post");
  g_assert_cmpstr(pt->copper_symbol, ==, "_copper_list");

  amiga_project_free(parsed);
  amiga_project_free(proj);

  /* Legacy alias parsing compatibility */
  const char *legacy =
      "[project]\nname = \"Legacy\"\n"
      "[[target]]\nname = \"rom\"\nkind = \"adf_raw\"\nformat = \"raw_bin\"\n";
  AmigaProject *leg_p = NULL;
  g_assert_true(amiga_project_parse_toml(legacy, "/tmp/leg", &leg_p, &err));
  g_assert_no_error(err);
  const AmigaTarget *lt = amiga_project_find_target(leg_p, "rom");
  g_assert_nonnull(lt);
  g_assert_cmpint(lt->kind, ==, AMIGA_TARGET_FLAT_BIN);
  amiga_project_free(leg_p);
}

static void
test_templates_and_makefile_fallback(void)
{
  g_autoptr(GError) err = NULL;
  g_autofree char *tmp = g_dir_make_tmp("kajo-amiga-tpl-XXXXXX", &err);
  g_assert_no_error(err);

  const AmigaTemplateKind tpls[] = {
    AMIGA_TEMPLATE_ASM_OS,
    AMIGA_TEMPLATE_BARE_OCS,
    AMIGA_TEMPLATE_ADF_FLOPPY,
    AMIGA_TEMPLATE_MIXED_C_ASM,
  };
  const AmigaTargetKind kinds[] = {
    AMIGA_TARGET_EXE_DH0,
    AMIGA_TARGET_EXE_DH0,
    AMIGA_TARGET_EXE_ADF,
    AMIGA_TARGET_FLAT_BIN,
  };
  for (size_t i = 0; i < G_N_ELEMENTS(tpls); i++) {
    g_autofree char *sub = g_strdup_printf("%s/tpl-%zu", tmp, i);
    g_assert_true(amiga_project_init_template(
        sub, "SampleDemo", tpls[i], "a500_ocs", kinds[i], &err));
    g_assert_no_error(err);

    AmigaProject *loaded = amiga_project_load_dir(sub, &err);
    g_assert_no_error(err);
    g_assert_nonnull(loaded);
    g_assert_cmpuint(loaded->targets->len, ==, 1);
    amiga_project_free(loaded);
  }

  /* Mixed C+ASM compile_commands.json must never emit literal $SDKDIR or mix ASM-only paths */
  g_autofree char *mixed_dir = g_strdup_printf("%s/tpl-3", tmp);
  AmigaProject *mixed = amiga_project_load_dir(mixed_dir, &err);
  g_assert_no_error(err);
  AmigaTarget *mt = g_ptr_array_index(mixed->targets, 0);
  g_ptr_array_add(mt->c_includes, g_strdup("/ndk/c_only"));
  g_ptr_array_add(mt->asm_includes, g_strdup("/ndk/asm_only"));
  AmigaToolchainStatus *tc = amiga_project_probe_toolchain(NULL);
  g_assert_nonnull(tc);
  g_assert_nonnull(tc->adflib_version);

  g_autofree char *cc_json = amiga_project_build_compile_commands_json(mixed, tc);
  g_assert_nonnull(cc_json);
  g_assert_null(strstr(cc_json, "$SDKDIR"));
  g_assert_nonnull(strstr(cc_json, "/ndk/c_only"));
  g_assert_null(strstr(cc_json, "/ndk/asm_only"));
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, cc_json, -1, &err));
  g_assert_no_error(err);
  JsonNode *root = json_parser_get_root(parser);
  g_assert_true(JSON_NODE_HOLDS_ARRAY(root));
  g_assert_cmpuint(json_array_get_length(json_node_get_array(root)), >=, 1);

  g_assert_true(amiga_project_write_compile_commands(mixed, tc, &err));
  g_assert_no_error(err);

  g_autofree char *mk = amiga_project_export_makefile(mixed, tc);
  g_assert_nonnull(mk);
  g_assert_nonnull(strstr(mk, "all:"));

  amiga_toolchain_status_free(tc);
  amiga_project_free(mixed);

  /* External Makefile fallback must NOT synthesize build units from directory structure */
  g_autofree char *mk_dir = g_build_filename(tmp, "external-make", NULL);
  g_autofree char *fw_dir = g_build_filename(mk_dir, "framework", NULL);
  g_autofree char *fx_dir = g_build_filename(mk_dir, "effects", NULL);
  g_mkdir_with_parents(fw_dir, 0755);
  g_mkdir_with_parents(fx_dir, 0755);
  g_autofree char *mk_path = g_build_filename(mk_dir, "Makefile", NULL);
  g_autofree char *dh_path = g_build_filename(fw_dir, "devharness.asm", NULL);
  g_autofree char *cube_path = g_build_filename(fx_dir, "cube.asm", NULL);
  g_assert_true(g_file_set_contents(mk_path, "all:\n\techo hi\n", -1, &err));
  g_assert_true(g_file_set_contents(dh_path, "; harness\n", -1, &err));
  g_assert_true(g_file_set_contents(cube_path, "; cube\n", -1, &err));

  AmigaProject *ext = amiga_project_load_dir(mk_dir, &err);
  g_assert_no_error(err);
  g_assert_nonnull(ext);
  g_assert_cmpint(ext->build_mode, ==, AMIGA_BUILD_EXTERNAL);
  g_assert_cmpstr(ext->build_cmd, ==, "make");
  g_assert_cmpuint(ext->shared_sources->len, ==, 0);
  g_assert_cmpuint(ext->targets->len, ==, 1);
  g_assert_null(amiga_project_find_target(ext, "cube"));
  const AmigaTarget *et = amiga_project_find_target(ext, "external-make");
  g_assert_nonnull(et);
  g_assert_cmpint(et->kind, ==, AMIGA_TARGET_EXE_DH0);
  amiga_project_free(ext);

  rm_rf(tmp);
}

static void
test_toolchain_probe_and_profile_compat(void)
{
  g_autoptr(GError) err = NULL;
  g_autofree char *tmp = g_dir_make_tmp("kajo-amiga-tc-XXXXXX", &err);
  g_assert_no_error(err);

  g_autofree char *bin_dir = g_build_filename(tmp, "bin", NULL);
  g_autofree char *c_inc = g_build_filename(tmp, "m68k-amiga-elf", "sys-include", NULL);
  g_autofree char *asm_inc = g_build_filename(tmp, "Include_i", NULL);
  g_mkdir_with_parents(bin_dir, 0755);
  g_mkdir_with_parents(c_inc, 0755);
  g_mkdir_with_parents(asm_inc, 0755);

  g_autofree char *ld_path = g_build_filename(bin_dir, "m68k-amiga-elf-ld", NULL);
  g_assert_true(g_file_set_contents(ld_path, "#!/bin/sh\nexit 0\n", -1, &err));
  g_chmod(ld_path, 0755);

  const char *extra[] = { bin_dir, NULL };
  AmigaToolchainStatus *tc = amiga_project_probe_toolchain_ex(extra, NULL, asm_inc);
  g_assert_nonnull(tc);
  g_assert_cmpstr(tc->m68k_elf_ld, ==, ld_path);
  /* C system header dir goes ONLY to c_include_dirs, ASM Include_i goes ONLY to asm_include_dirs */
  g_assert_true(ptr_array_contains_str(tc->c_include_dirs, c_inc));
  g_assert_false(ptr_array_contains_str(tc->asm_include_dirs, c_inc));
  g_assert_true(ptr_array_contains_str(tc->asm_include_dirs, asm_inc));
  g_assert_false(ptr_array_contains_str(tc->c_include_dirs, asm_inc));

  g_autoptr(GPtrArray) resolved_c = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) resolved_asm = g_ptr_array_new_with_free_func(g_free);
  AmigaTarget *t0 = amiga_target_new("demo");
  amiga_project_collect_target_includes(NULL, t0, tc, false, false, resolved_c);
  amiga_project_collect_target_includes(NULL, t0, tc, true, false, resolved_asm);
  g_assert_true(ptr_array_contains_str(resolved_c, c_inc));
  g_assert_false(ptr_array_contains_str(resolved_c, asm_inc));
  g_assert_true(ptr_array_contains_str(resolved_asm, asm_inc));
  g_assert_false(ptr_array_contains_str(resolved_asm, c_inc));
  amiga_target_free(t0);
  amiga_toolchain_status_free(tc);

  /* Profile compat check */
  AmigaTarget *t020 = amiga_target_new("aga_demo");
  g_free(t020->cpu);
  t020->cpu = g_strdup("68020");
  AmigaHwProfile *a500 = amiga_hw_profile_new("a500_ocs");
  g_free(a500->cpu);
  a500->cpu = g_strdup("68000");
  g_free(a500->chipset);
  a500->chipset = g_strdup("OCS");

  g_autofree char *warn = amiga_project_check_profile_compat(t020, a500);
  g_assert_nonnull(warn);

  g_free(t020->cpu);
  t020->cpu = g_strdup("68000");
  g_assert_null(amiga_project_check_profile_compat(t020, a500));

  amiga_hw_profile_free(a500);
  amiga_target_free(t020);
  rm_rf(tmp);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/amiga/project/toml-roundtrip-and-presets", test_toml_roundtrip_and_presets);
  g_test_add_func("/amiga/project/templates-and-makefile-fallback", test_templates_and_makefile_fallback);
  g_test_add_func("/amiga/project/toolchain-probe-and-profile-compat", test_toolchain_probe_and_profile_compat);
  return g_test_run();
}
