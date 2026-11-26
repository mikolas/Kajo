#include "../src/amiga/amiga_project.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>

static void
rm_rf(const char *path)
{
  if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
    return;
  }
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

static void
test_toml_roundtrip_and_presets(void)
{
  AmigaProject *proj = amiga_project_new();
  proj->name = g_strdup("CopperTest");
  proj->build_mode = AMIGA_BUILD_NATIVE;
  proj->default_target = g_strdup("floppy");
  g_free(proj->default_profile);
  proj->default_profile = g_strdup("a500_ocs");
  proj->trap86_guest_output = true;
  g_ptr_array_add(proj->shared_sources, g_strdup("src/common.c"));
  amiga_project_seed_default_profiles(proj);
  g_assert_cmpuint(proj->profiles->len, ==, 4);
  g_assert_nonnull(amiga_project_find_profile(proj, "a500_ocs"));
  g_assert_nonnull(amiga_project_find_profile(proj, "a1200_aga"));

  AmigaTarget *t = amiga_target_new("floppy");
  t->kind = AMIGA_TARGET_ADF_DOS;
  g_free(t->cpu);
  t->cpu = g_strdup("68000");
  t->toolchain = AMIGA_TOOLCHAIN_ELF2HUNK;
  g_ptr_array_add(t->sources, g_strdup("src/main.c"));
  g_ptr_array_add(t->sources, g_strdup("src/irq.s"));
  g_ptr_array_add(t->includes, g_strdup("include"));
  g_ptr_array_add(t->defines, g_strdup("DEBUG=1"));
  g_ptr_array_add(t->extra_files, g_strdup("assets/mod.song"));
  t->startup_sequence = g_strdup("floppy.exe");
  t->copper_symbol = g_strdup("_copper_list");
  t->bitplane_symbol = g_strdup("_bpl0");
  t->entry_addr = 0x20000u;
  g_ptr_array_add(proj->targets, t);

  AmigaAsset *a = amiga_asset_new("logo");
  g_ptr_array_add(a->inputs, g_strdup("assets/logo.png"));
  g_ptr_array_add(a->outputs, g_strdup("build/logo.bpl"));
  a->cmd = g_strdup("echo converting logo");
  g_ptr_array_add(proj->assets, a);

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
  g_assert_cmpuint(parsed->assets->len, ==, 1);
  g_assert_cmpuint(parsed->profiles->len, ==, 4);

  const AmigaTarget *pt = amiga_project_find_target(parsed, "floppy");
  g_assert_nonnull(pt);
  g_assert_cmpint(pt->kind, ==, AMIGA_TARGET_ADF_DOS);
  g_assert_cmpint(pt->toolchain, ==, AMIGA_TOOLCHAIN_ELF2HUNK);
  g_assert_cmpuint(pt->sources->len, ==, 2);
  g_assert_cmpuint(pt->entry_addr, ==, 0x20000u);
  g_assert_cmpstr(pt->copper_symbol, ==, "_copper_list");

  amiga_project_free(parsed);
  amiga_project_free(proj);
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
  for (size_t i = 0; i < G_N_ELEMENTS(tpls); i++) {
    g_autofree char *sub = g_strdup_printf("%s/tpl-%zu", tmp, i);
    g_assert_true(amiga_project_init_template(
        sub, "SampleDemo", tpls[i], "a500_ocs", AMIGA_TARGET_ADF_DOS, &err));
    g_assert_no_error(err);

    g_autofree char *manifest = g_build_filename(sub, "kajo-amiga.toml", NULL);
    g_assert_true(g_file_test(manifest, G_FILE_TEST_EXISTS));

    AmigaProject *loaded = amiga_project_load_dir(sub, &err);
    g_assert_no_error(err);
    g_assert_nonnull(loaded);
    g_assert_cmpuint(loaded->targets->len, >=, 1);
    amiga_project_free(loaded);
  }

  /* Mixed C+ASM compile_commands.json + Makefile export check */
  g_autofree char *mixed_dir = g_strdup_printf("%s/tpl-3", tmp);
  AmigaProject *mixed = amiga_project_load_dir(mixed_dir, &err);
  g_assert_no_error(err);
  AmigaToolchainStatus *tc = amiga_project_probe_toolchain(NULL);
  g_assert_nonnull(tc);
  g_assert_nonnull(tc->adflib_version);

  g_autofree char *cc_json = amiga_project_build_compile_commands_json(mixed, tc);
  g_assert_nonnull(cc_json);
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

  /* External Makefile fallback on directory without kajo-amiga.toml */
  g_autofree char *mk_dir = g_build_filename(tmp, "external-make", NULL);
  g_mkdir_with_parents(mk_dir, 0755);
  g_autofree char *mk_path = g_build_filename(mk_dir, "Makefile", NULL);
  g_assert_true(g_file_set_contents(mk_path, "all:\n\techo hi\n", -1, &err));

  AmigaProject *ext = amiga_project_load_dir(mk_dir, &err);
  g_assert_no_error(err);
  g_assert_nonnull(ext);
  g_assert_cmpint(ext->build_mode, ==, AMIGA_BUILD_EXTERNAL);
  g_assert_cmpuint(ext->targets->len, >=, 1);
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
  g_autofree char *sdk_inc = g_build_filename(tmp, "m68k-amiga-elf", "sys-include", NULL);
  g_mkdir_with_parents(bin_dir, 0755);
  g_mkdir_with_parents(sdk_inc, 0755);

  g_autofree char *ld_path = g_build_filename(bin_dir, "m68k-amiga-elf-ld", NULL);
  g_assert_true(g_file_set_contents(ld_path, "#!/bin/sh\nexit 0\n", -1, &err));
  g_chmod(ld_path, 0755);

  const char *extra[] = { bin_dir, NULL };
  AmigaToolchainStatus *tc = amiga_project_probe_toolchain(extra);
  g_assert_nonnull(tc);
  g_assert_cmpstr(tc->m68k_elf_ld, ==, ld_path);
  g_assert_cmpstr(tc->sdk_include_dir, ==, sdk_inc);
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
