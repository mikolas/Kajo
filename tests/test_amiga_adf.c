#include "../src/amiga/amiga_adf.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

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

static bool
array_contains_str(GPtrArray *arr, const char *needle)
{
  if (!arr || !needle) {
    return false;
  }
  for (guint i = 0; i < arr->len; i++) {
    const char *s = g_ptr_array_index(arr, i);
    if (g_strcmp0(s, needle) == 0) {
      return true;
    }
  }
  return false;
}

static void
test_adflib_dos_ofs_and_ffs_roundtrip(void)
{
  const char *ver = amiga_adf_lib_version();
  g_assert_nonnull(ver);
  g_assert_cmpuint((guint)strlen(ver), >, 0);

  g_autoptr(GError) err = NULL;
  g_autofree char *tmp = g_dir_make_tmp("kajo-amiga-adf-XXXXXX", &err);
  g_assert_no_error(err);

  g_autofree char *host_exe = g_build_filename(tmp, "demo.exe", NULL);
  const uint8_t fake_hunk[] = { 0x00, 0x00, 0x03, 0xF3, 0x00, 0x00, 0x00, 0x00 };
  g_assert_true(g_file_set_contents(
      host_exe, (const char *)fake_hunk, sizeof(fake_hunk), &err));

  const uint8_t startup[] = "demo.exe\n";
  const uint8_t palette[] = { 0x00, 0x00, 0x0F, 0x00 };
  const AmigaAdfFileEntry entries[] = {
    { .guest_path = "s/startup-sequence", .bytes = startup, .bytes_len = sizeof(startup) - 1 },
    { .guest_path = "demo.exe", .host_path = host_exe },
    { .guest_path = "data/palette.bin", .bytes = palette, .bytes_len = sizeof(palette) },
  };

  g_autofree char *ofs_adf = g_build_filename(tmp, "test_ofs.adf", NULL);
  AmigaAdfDosSpec spec = {
    .adf_path = ofs_adf,
    .volume_name = "KAJOTEST",
    .ffs = false,
    .intl = false,
    .hd = false,
    .bootable = true,
    .bootblock = NULL,
    .files = entries,
    .n_files = G_N_ELEMENTS(entries),
  };
  g_assert_true(amiga_adf_create_dos(&spec, &err));
  g_assert_no_error(err);

  g_autofree char *raw_buf = NULL;
  gsize raw_len = 0;
  g_assert_true(g_file_get_contents(ofs_adf, &raw_buf, &raw_len, &err));
  g_assert_cmpuint((guint)raw_len, ==, 901120u);
  g_assert_cmpuint(raw_buf[0], ==, 'D');
  g_assert_cmpuint(raw_buf[1], ==, 'O');
  g_assert_cmpuint(raw_buf[2], ==, 'S');
  g_assert_cmpuint(raw_buf[3], ==, 0);

  uint32_t expected_ck = amiga_adf_bootblock_checksum((const uint8_t *)raw_buf);
  uint32_t stored_ck = ((uint32_t)(uint8_t)raw_buf[4] << 24) |
                       ((uint32_t)(uint8_t)raw_buf[5] << 16) |
                       ((uint32_t)(uint8_t)raw_buf[6] << 8) |
                       (uint32_t)(uint8_t)raw_buf[7];
  g_assert_cmpuint(stored_ck, ==, expected_ck);

  /* Verify default DOS bootblock FindResident("dos.library") & PC-relative displacement */
  g_assert_cmpuint((uint8_t)raw_buf[12], ==, 0x43u);
  g_assert_cmpuint((uint8_t)raw_buf[13], ==, 0xFAu);
  uint16_t lea_disp = (uint16_t)(((uint8_t)raw_buf[14] << 8) | (uint8_t)raw_buf[15]);
  g_assert_cmpstr(raw_buf + 14 + lea_disp, ==, "dos.library");
  g_assert_cmpuint((uint8_t)raw_buf[16], ==, 0x4Eu);
  g_assert_cmpuint((uint8_t)raw_buf[17], ==, 0xAEu);
  g_assert_cmpuint((uint8_t)raw_buf[18], ==, 0xFFu);
  g_assert_cmpuint((uint8_t)raw_buf[19], ==, 0xA0u);

  g_autoptr(GPtrArray) paths = NULL;
  g_assert_true(amiga_adf_list_files(ofs_adf, &paths, &err));
  g_assert_no_error(err);
  g_assert_true(array_contains_str(paths, "demo.exe"));
  g_assert_true(array_contains_str(paths, "s/startup-sequence"));
  g_assert_true(array_contains_str(paths, "data/palette.bin"));

  /* HD FFS volume check */
  g_autofree char *ffs_adf = g_build_filename(tmp, "test_ffs_hd.adf", NULL);
  AmigaAdfDosSpec ffs_spec = {
    .adf_path = ffs_adf,
    .volume_name = "KAJOHD",
    .ffs = true,
    .intl = true,
    .hd = true,
    .bootable = false,
    .files = entries,
    .n_files = 1,
  };
  g_assert_true(amiga_adf_create_dos(&ffs_spec, &err));
  g_assert_no_error(err);

  g_autofree char *hd_buf = NULL;
  gsize hd_len = 0;
  g_assert_true(g_file_get_contents(ffs_adf, &hd_buf, &hd_len, &err));
  g_assert_cmpuint((guint)hd_len, ==, 1802240u);

  g_autoptr(GPtrArray) hd_paths = NULL;
  g_assert_true(amiga_adf_list_files(ffs_adf, &hd_paths, &err));
  g_assert_true(array_contains_str(hd_paths, "s/startup-sequence"));

  rm_rf(tmp);
}

static void
test_raw_adf_and_errors(void)
{
  g_autoptr(GError) err = NULL;
  g_autofree char *tmp = g_dir_make_tmp("kajo-amiga-raw-XXXXXX", &err);
  g_assert_no_error(err);

  uint8_t boot[1024];
  memset(boot, 0, sizeof(boot));
  boot[0] = 'D'; boot[1] = 'O'; boot[2] = 'S'; boot[3] = 0;
  boot[12] = 0x4Eu; boot[13] = 0x75u; /* RTS */

  const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };
  g_autofree char *raw_adf = g_build_filename(tmp, "trackdemo.adf", NULL);
  g_assert_true(amiga_adf_create_raw(
      raw_adf, boot, sizeof(boot), payload, sizeof(payload), &err));
  g_assert_no_error(err);

  g_autofree char *img = NULL;
  gsize len = 0;
  g_assert_true(g_file_get_contents(raw_adf, &img, &len, &err));
  g_assert_cmpuint((guint)len, ==, 901120u);
  g_assert_cmpint(memcmp(img + 1024, payload, sizeof(payload)), ==, 0);

  uint32_t expect_ck = amiga_adf_bootblock_checksum((const uint8_t *)img);
  uint32_t stored_ck = ((uint32_t)(uint8_t)img[4] << 24) |
                       ((uint32_t)(uint8_t)img[5] << 16) |
                       ((uint32_t)(uint8_t)img[6] << 8) |
                       (uint32_t)(uint8_t)img[7];
  g_assert_cmpuint(stored_ck, ==, expect_ck);

  /* Missing file error check */
  g_autoptr(GPtrArray) bad_paths = NULL;
  g_autofree char *missing = g_build_filename(tmp, "nonexistent.adf", NULL);
  g_assert_false(amiga_adf_list_files(missing, &bad_paths, &err));
  g_assert_nonnull(err);
  g_clear_error(&err);

  rm_rf(tmp);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/amiga/adf/dos-ofs-and-ffs-roundtrip", test_adflib_dos_ofs_and_ffs_roundtrip);
  g_test_add_func("/amiga/adf/raw-adf-and-errors", test_raw_adf_and_errors);
  return g_test_run();
}
