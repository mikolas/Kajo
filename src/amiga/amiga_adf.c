#include "amiga_adf.h"

#include <adflib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

static void
adflib_quiet_log(const char *const format G_GNUC_UNUSED, ...)
{
}

static gpointer
adflib_init_once(gpointer data G_GNUC_UNUSED)
{
  adfLibInit();
  adfEnvSetFct(adflib_quiet_log, adflib_quiet_log, adflib_quiet_log, NULL);
  adfEnvSetProperty(ADF_PR_QUIET, 1);
  return GINT_TO_POINTER(1);
}

static void
ensure_adflib_init(void)
{
  static GOnce once = G_ONCE_INIT;
  g_once(&once, adflib_init_once, NULL);
}

const char *
amiga_adf_lib_version(void)
{
  ensure_adflib_init();
  const char *v = adfGetVersionNumber();
  return v ? v : "unknown";
}

uint32_t
amiga_adf_bootblock_checksum(const uint8_t block[1024])
{
  if (!block) {
    return 0;
  }
  uint32_t sum = 0;
  for (size_t i = 0; i < 1024; i += 4) {
    if (i == 4) {
      continue;
    }
    uint32_t w = ((uint32_t)block[i] << 24) |
                 ((uint32_t)block[i + 1] << 16) |
                 ((uint32_t)block[i + 2] << 8) |
                 (uint32_t)block[i + 3];
    uint32_t prev = sum;
    sum += w;
    if (sum < prev) {
      sum++;
    }
  }
  return ~sum;
}

static bool
ensure_guest_dirs_and_cd(struct AdfVolume *vol,
                         const char       *guest_path,
                         const char      **out_basename,
                         GError          **error)
{
  if (adfToRootDir(vol) != ADF_RC_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "adfToRootDir failed");
    return false;
  }
  g_autofree char *copy = g_strdup(guest_path);
  char *slash = strrchr(copy, '/');
  if (!slash) {
    *out_basename = guest_path;
    return true;
  }
  *slash = '\0';
  g_auto(GStrv) dirs = g_strsplit(copy, "/", -1);
  for (int i = 0; dirs[i] != NULL; i++) {
    if (dirs[i][0] == '\0') {
      continue;
    }
    if (adfChangeDir(vol, dirs[i]) != ADF_RC_OK) {
      if (adfCreateDir(vol, vol->curDirPtr, dirs[i]) != ADF_RC_OK ||
          adfChangeDir(vol, dirs[i]) != ADF_RC_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to create ADF directory '%s'", dirs[i]);
        return false;
      }
    }
  }
  const char *orig_slash = strrchr(guest_path, '/');
  *out_basename = orig_slash ? (orig_slash + 1) : guest_path;
  return true;
}

static bool
write_single_adf_file(struct AdfVolume        *vol,
                      const AmigaAdfFileEntry *fe,
                      GError                 **error)
{
  if (!fe || !fe->guest_path || !fe->guest_path[0]) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Empty ADF guest_path");
    return false;
  }

  g_autofree char *host_buf = NULL;
  const uint8_t *data = fe->bytes;
  size_t len = fe->bytes_len;

  if (fe->host_path != NULL) {
    gsize host_len = 0;
    if (!g_file_get_contents(fe->host_path, &host_buf, &host_len, error)) {
      return false;
    }
    data = (const uint8_t *)host_buf;
    len = (size_t)host_len;
  }

  const char *basename = NULL;
  if (!ensure_guest_dirs_and_cd(vol, fe->guest_path, &basename, error)) {
    return false;
  }
  if (!basename || !basename[0]) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid ADF file path '%s'", fe->guest_path);
    return false;
  }

  struct AdfFile *af = adfFileOpen(vol, basename, ADF_FILE_MODE_WRITE);
  if (!af) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "adfFileOpen failed for '%s'", fe->guest_path);
    return false;
  }

  bool ok = true;
  if (len > 0) {
    uint32_t written = adfFileWrite(af, (uint32_t)len, data);
    if ((size_t)written != len) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                  "adfFileWrite short write on '%s' (%u/%zu bytes)",
                  fe->guest_path, written, len);
      ok = false;
    }
  }
  adfFileClose(af);
  return ok;
}

bool
amiga_adf_create_dos(const AmigaAdfDosSpec *spec, GError **error)
{
  if (!spec || !spec->adf_path || !spec->adf_path[0]) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing adf_path");
    return false;
  }
  ensure_adflib_init();

  g_autofree char *dir = g_path_get_dirname(spec->adf_path);
  g_mkdir_with_parents(dir, 0755);
  g_unlink(spec->adf_path);

  uint32_t sectors = spec->hd ? 22u : 11u;
  struct AdfDevice *dev = adfDevCreate("dump", spec->adf_path, 80u, 2u, sectors);
  if (!dev) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "adfDevCreate failed for '%s'", spec->adf_path);
    return false;
  }

  const char *vname = (spec->volume_name && spec->volume_name[0]) ? spec->volume_name : "KAJO";
  uint8_t fs_type = 0;
  if (spec->ffs) fs_type |= ADF_DOSFS_FFS;
  if (spec->intl) fs_type |= ADF_DOSFS_INTL;

  if (adfCreateFlop(dev, vname, fs_type) != ADF_RC_OK) {
    adfDevClose(dev);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "adfCreateFlop failed for '%s'", spec->adf_path);
    return false;
  }
  adfDevUnMount(dev);

  if (adfDevMount(dev) != ADF_RC_OK) {
    adfDevClose(dev);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "adfDevMount failed");
    return false;
  }

  struct AdfVolume *vol = adfVolMount(dev, 0, ADF_ACCESS_MODE_READWRITE);
  if (!vol) {
    adfDevUnMount(dev);
    adfDevClose(dev);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "adfVolMount failed");
    return false;
  }

  bool ok = true;
  if (spec->bootable || spec->bootblock != NULL) {
    uint8_t default_boot[1024];
    const uint8_t *boot_ptr = spec->bootblock;
    if (!boot_ptr) {
      /* Standard minimal AmigaDOS bootcode (opens dos.library & runs s/startup-sequence) */
      static const uint8_t dos_bootcode[] = {
        0x43, 0xFA, 0x00, 0x3E, /* lea dosname(pc),a1 */
        0x70, 0x25,             /* moveq #37,d0 */
        0x4E, 0xAE, 0xFD, 0xD8, /* jsr -552(a6) (_LVOOpenLibrary) */
        0x4A, 0x80,             /* tst.l d0 */
        0x67, 0x0C,             /* beq.s err */
        0x22, 0x40,             /* move.l d0,a1 */
        0x08, 0xE9, 0x00, 0x00, 0x00, 0x06,
        0x4E, 0xAE, 0xFE, 0x62,
        0x60, 0x04,
        0x70, 0xFF,             /* moveq #-1,d0 */
        0x4E, 0x75,             /* rts */
        'd', 'o', 's', '.', 'l', 'i', 'b', 'r', 'a', 'r', 'y', 0,
      };
      memset(default_boot, 0, sizeof(default_boot));
      default_boot[0] = 'D';
      default_boot[1] = 'O';
      default_boot[2] = 'S';
      default_boot[3] = fs_type;
      memcpy(default_boot + 12, dos_bootcode, sizeof(dos_bootcode));
      boot_ptr = default_boot;
    }
    if (adfVolInstallBootBlock(vol, boot_ptr) != ADF_RC_OK) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "adfVolInstallBootBlock failed");
      ok = false;
    }
  }

  for (size_t i = 0; ok && i < spec->n_files; i++) {
    if (!write_single_adf_file(vol, &spec->files[i], error)) {
      ok = false;
    }
  }

  adfVolUnMount(vol);
  adfDevUnMount(dev);
  adfDevClose(dev);
  return ok;
}

static void
collect_dir_entries(struct AdfVolume *vol,
                    ADF_SECTNUM       dir_sect,
                    const char       *prefix,
                    GPtrArray        *out)
{
  struct AdfList *list = adfGetDirEnt(vol, dir_sect);
  for (struct AdfList *it = list; it != NULL; it = it->next) {
    struct AdfEntry *ent = (struct AdfEntry *)it->content;
    if (!ent || !ent->name) {
      continue;
    }
    g_autofree char *rel = (prefix && prefix[0])
                               ? g_strdup_printf("%s/%s", prefix, ent->name)
                               : g_strdup(ent->name);
    if (ent->type == ADF_ST_DIR) {
      g_ptr_array_add(out, g_strdup_printf("%s/", rel));
      collect_dir_entries(vol, ent->sector, rel, out);
    } else {
      g_ptr_array_add(out, g_strdup(rel));
    }
  }
  adfFreeDirList(list);
}

bool
amiga_adf_list_files(const char  *adf_path,
                     GPtrArray  **out_paths,
                     GError     **error)
{
  if (!adf_path || !out_paths) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid argument");
    return false;
  }
  *out_paths = NULL;
  ensure_adflib_init();

  struct AdfDevice *dev = adfDevOpen(adf_path, ADF_ACCESS_MODE_READONLY);
  if (!dev) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Cannot open ADF '%s'", adf_path);
    return false;
  }
  if (adfDevMount(dev) != ADF_RC_OK) {
    adfDevClose(dev);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "adfDevMount failed on '%s'", adf_path);
    return false;
  }
  struct AdfVolume *vol = adfVolMount(dev, 0, ADF_ACCESS_MODE_READONLY);
  if (!vol) {
    adfDevUnMount(dev);
    adfDevClose(dev);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "adfVolMount failed on '%s'", adf_path);
    return false;
  }

  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  collect_dir_entries(vol, vol->rootBlock, "", arr);
  *out_paths = arr;

  adfVolUnMount(vol);
  adfDevUnMount(dev);
  adfDevClose(dev);
  return true;
}

bool
amiga_adf_create_raw(const char    *adf_path,
                     const uint8_t *bootblock,
                     size_t         boot_len,
                     const uint8_t *payload,
                     size_t         payload_len,
                     GError       **error)
{
  enum { DD_BYTES = 901120 };
  if (!adf_path || !adf_path[0]) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing adf_path");
    return false;
  }
  if (boot_len > 1024 || 1024 + payload_len > DD_BYTES) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                "Raw ADF overflow (boot=%zu payload=%zu max=%d)",
                boot_len, payload_len, DD_BYTES);
    return false;
  }

  g_autofree uint8_t *disk = g_malloc0(DD_BYTES);
  if (bootblock && boot_len > 0) {
    memcpy(disk, bootblock, boot_len);
  }
  disk[0] = 'D'; disk[1] = 'O'; disk[2] = 'S'; disk[3] = 0;
  uint32_t csum = amiga_adf_bootblock_checksum(disk);
  disk[4] = (uint8_t)(csum >> 24);
  disk[5] = (uint8_t)(csum >> 16);
  disk[6] = (uint8_t)(csum >> 8);
  disk[7] = (uint8_t)(csum & 0xFFu);

  if (payload && payload_len > 0) {
    memcpy(disk + 1024, payload, payload_len);
  }

  g_autofree char *dir = g_path_get_dirname(adf_path);
  g_mkdir_with_parents(dir, 0755);
  return g_file_set_contents(adf_path, (const char *)disk, DD_BYTES, error);
}
