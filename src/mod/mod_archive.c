#include "mod_archive.h"
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean
has_Case_prefix(const gchar *str, const gchar *prefix)
{
    gsize plen = strlen(prefix);
    return g_ascii_strncasecmp(str, prefix, plen) == 0;
}

static gboolean
has_case_suffix(const gchar *str, const gchar *suffix)
{
    gsize slen = strlen(str);
    gsize xlen = strlen(suffix);
    if (slen < xlen) return FALSE;
    return g_ascii_strcasecmp(str + (slen - xlen), suffix) == 0;
}

gboolean
kajo_mod_path_is_archive(const gchar *path)
{
    if (!path || !*path) return FALSE;

    /* .vgz is a gzipped Sega VGM handled directly by libgme */
    if (has_case_suffix(path, ".vgz"))
        return FALSE;

    static const char *const k_arch_exts[] = {
        ".lha", ".lzh", ".lzx", ".zip", ".7z", ".tar", ".tgz", ".tar.gz",
        ".tar.xz", ".tar.bz2", ".gz"
    };
    for (gsize i = 0; i < G_N_ELEMENTS(k_arch_exts); i++) {
        if (has_case_suffix(path, k_arch_exts[i]))
            return TRUE;
    }

    /* Check file magic signature for extensionless or Amiga-named archives */
    FILE *fp = fopen(path, "rb");
    if (!fp) return FALSE;
    guint8 hdr[16] = { 0 };
    size_t n = fread(hdr, 1, sizeof(hdr), fp);
    fclose(fp);
    if (n < 7) return FALSE;

    /* ZIP: PK\x03\x04 */
    if (hdr[0] == 'P' && hdr[1] == 'K' && hdr[2] == 0x03 && hdr[3] == 0x04)
        return TRUE;
    /* 7z: 7z\xBC\xAF\x27\x1C */
    if (hdr[0] == '7' && hdr[1] == 'z' && hdr[2] == 0xBC && hdr[3] == 0xAF &&
        hdr[4] == 0x27 && hdr[5] == 0x1C)
        return TRUE;
    /* LHA / LZH: -lh0- .. -lh7-, -lzs-, -lz4-, -lhd- at offset 2 */
    if (hdr[2] == '-' && (hdr[3] == 'l' || hdr[3] == 'L') &&
        (hdr[4] == 'h' || hdr[4] == 'H' || hdr[4] == 'z' || hdr[4] == 'Z') &&
        hdr[6] == '-')
        return TRUE;
    /* Amiga LZX: "LZX" */
    if (hdr[0] == 'L' && hdr[1] == 'Z' && hdr[2] == 'X')
        return TRUE;

    return FALSE;
}

gboolean
kajo_mod_filename_is_companion(const gchar *filename)
{
    if (!filename || !*filename) return FALSE;
    g_autofree gchar *base = g_path_get_basename(filename);

    /* Amiga prefix conventions for external sample/instrument banks:
     * - TFMX: mdat.<song> + smpl.<song>
     * - Jason Page: jpn.<song> + smp.<song>
     * - Richard Joseph: rjp.<song> + smp.<song>
     * - Quartet / Sonix: qpa.<song> + smp.<song>, *.ins / ins.*
     */
    static const char *const k_comp_prefixes[] = {
        "smpl.", "smp.", "ins.", "sam."
    };
    for (gsize i = 0; i < G_N_ELEMENTS(k_comp_prefixes); i++) {
        if (has_Case_prefix(base, k_comp_prefixes[i]) && strlen(base) > strlen(k_comp_prefixes[i]))
            return TRUE;
    }

    static const char *const k_comp_suffixes[] = {
        ".smpl", ".smp", ".ins", ".str", ".sam", ".ss"
    };
    for (gsize i = 0; i < G_N_ELEMENTS(k_comp_suffixes); i++) {
        if (has_case_suffix(base, k_comp_suffixes[i]) && strlen(base) > strlen(k_comp_suffixes[i]))
            return TRUE;
    }

    return FALSE;
}

gboolean
kajo_mod_filename_is_nfo(const gchar *filename)
{
    if (!filename || !*filename) return FALSE;
    g_autofree gchar *base = g_path_get_basename(filename);

    static const char *const k_nfo_suffixes[] = {
        ".nfo", ".diz", ".txt", ".readme", ".doc", ".1st"
    };
    for (gsize i = 0; i < G_N_ELEMENTS(k_nfo_suffixes); i++) {
        if (has_case_suffix(base, k_nfo_suffixes[i]))
            return TRUE;
    }
    if (has_Case_prefix(base, "readme") ||
        g_ascii_strcasecmp(base, "file_id.diz") == 0)
        return TRUE;

    return FALSE;
}

static gboolean
filename_is_ignored_junk(const gchar *filename)
{
    if (!filename || !*filename) return TRUE;
    g_autofree gchar *base = g_path_get_basename(filename);
    if (base[0] == '.') return TRUE;
    if (has_case_suffix(base, ".info")) return TRUE; /* Amiga Workbench icon */
    return FALSE;
}

gchar *
kajo_mod_text_to_utf8(const guint8 *data, gsize len)
{
    if (!data || len == 0)
        return g_strdup("");

    if (g_utf8_validate((const gchar *)data, (gssize)len, NULL)) {
        return g_strndup((const gchar *)data, len);
    }

    /* Convert Amiga ISO-8859-1 / ASCII with control filtering to UTF-8 */
    GString *out = g_string_sized_new(len + 32);
    for (gsize i = 0; i < len; i++) {
        guint8 b = data[i];
        if (b == 0) break;
        if (b == '\r') continue;
        if (b == '\n' || b == '\t' || (b >= 0x20 && b <= 0x7E)) {
            g_string_append_c(out, (gchar)b);
        } else if (b >= 0xA0) {
            /* Latin-1 supplement codepoint U+00A0..U+00FF */
            g_string_append_unichar(out, (gunichar)b);
        } else {
            g_string_append_c(out, ' ');
        }
    }
    return g_string_free(out, FALSE);
}

static gchar *
sanitize_archive_relpath(const gchar *raw_path)
{
    if (!raw_path || !*raw_path) return NULL;
    g_auto(GStrv) parts = g_strsplit_set(raw_path, "/\\", -1);
    GPtrArray *safe = g_ptr_array_new();

    for (int i = 0; parts && parts[i] != NULL; i++) {
        const gchar *p = parts[i];
        if (!*p || g_strcmp0(p, ".") == 0 || g_strcmp0(p, "..") == 0)
            continue;
        g_ptr_array_add(safe, (gpointer)p);
    }
    if (safe->len == 0) {
        g_ptr_array_free(safe, TRUE);
        return NULL;
    }
    g_ptr_array_add(safe, NULL);
    gchar *joined = g_build_filenamev((gchar **)safe->pdata);
    g_ptr_array_free(safe, TRUE);
    return joined;
}

static gint
compare_paths_natural(gconstpointer a, gconstpointer b)
{
    const gchar *pa = *(const gchar *const *)a;
    const gchar *pb = *(const gchar *const *)b;
    g_autofree gchar *ka = g_utf8_collate_key_for_filename(pa ? pa : "", -1);
    g_autofree gchar *kb = g_utf8_collate_key_for_filename(pb ? pb : "", -1);
    return g_strcmp0(ka, kb);
}

static void
rmdir_recursive(const gchar *dir_path)
{
    if (!dir_path || !*dir_path) return;
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (dir) {
        const gchar *name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            g_autofree gchar *child = g_build_filename(dir_path, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_SYMLINK)) {
                g_unlink(child);
            } else if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
                rmdir_recursive(child);
            } else {
                g_unlink(child);
            }
        }
        g_dir_close(dir);
    }
    g_rmdir(dir_path);
}

KajoModArchive *
kajo_mod_archive_extract(const gchar *archive_path, GError **error)
{
    if (!archive_path || !g_file_test(archive_path, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Archive file not found: %s", archive_path ? archive_path : "(null)");
        return NULL;
    }

    const gchar *runtime_dir = g_get_user_runtime_dir();
    g_autofree gchar *tmpl = g_build_filename(
        (runtime_dir && *runtime_dir) ? runtime_dir : g_get_tmp_dir(),
        "kajo-mod-XXXXXX", NULL);
    gchar *extract_dir = g_mkdtemp_full(g_strdup(tmpl), 0700);
    if (!extract_dir) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to create temporary archive directory");
        return NULL;
    }

    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archive_path, 65536) != ARCHIVE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "libarchive open failed: %s", archive_error_string(a));
        archive_read_free(a);
        rmdir_recursive(extract_dir);
        g_free(extract_dir);
        return NULL;
    }

    KajoModArchive *res = g_new0(KajoModArchive, 1);
    res->archive_path    = g_strdup(archive_path);
    res->extract_dir     = extract_dir;
    res->module_paths    = g_ptr_array_new_with_free_func(g_free);
    res->companion_paths = g_ptr_array_new_with_free_func(g_free);

    struct archive_entry *entry = NULL;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        mode_t ftype = archive_entry_filetype(entry);
        const char *raw_name = archive_entry_pathname(entry);
        if (ftype != AE_IFREG || !raw_name) {
            archive_read_data_skip(a);
            continue;
        }

        g_autofree gchar *rel = sanitize_archive_relpath(raw_name);
        if (!rel) {
            archive_read_data_skip(a);
            continue;
        }

        g_autofree gchar *dest = g_build_filename(extract_dir, rel, NULL);
        g_autofree gchar *parent = g_path_get_dirname(dest);
        g_mkdir_with_parents(parent, 0700);

        FILE *out_fp = fopen(dest, "wb");
        if (!out_fp) {
            archive_read_data_skip(a);
            continue;
        }

        guint8 buf[16384];
        la_ssize_t r;
        gsize total_written = 0;
        while ((r = archive_read_data(a, buf, sizeof(buf))) > 0) {
            if (fwrite(buf, 1, (size_t)r, out_fp) != (size_t)r)
                break;
            total_written += (gsize)r;
            if (total_written > 64 * 1024 * 1024) /* 64 MB safety cap per entry */
                break;
        }
        fclose(out_fp);

        if (filename_is_ignored_junk(dest))
            continue;

        if (kajo_mod_filename_is_nfo(dest)) {
            if (!res->nfo_text) {
                g_autofree gchar *raw = NULL;
                gsize raw_len = 0;
                if (g_file_get_contents(dest, &raw, &raw_len, NULL) && raw_len > 0) {
                    res->nfo_name = g_path_get_basename(dest);
                    res->nfo_text = kajo_mod_text_to_utf8((const guint8 *)raw, raw_len);
                }
            }
            continue;
        }

        if (kajo_mod_filename_is_companion(dest)) {
            g_ptr_array_add(res->companion_paths, g_strdup(dest));
            continue;
        }

        g_ptr_array_add(res->module_paths, g_strdup(dest));
    }

    archive_read_free(a);
    g_ptr_array_sort(res->module_paths, compare_paths_natural);
    g_ptr_array_sort(res->companion_paths, compare_paths_natural);
    return res;
}

void
kajo_mod_archive_free(KajoModArchive *arch)
{
    if (!arch) return;
    if (arch->extract_dir) {
        rmdir_recursive(arch->extract_dir);
        g_free(arch->extract_dir);
    }
    if (arch->module_paths)
        g_ptr_array_unref(arch->module_paths);
    if (arch->companion_paths)
        g_ptr_array_unref(arch->companion_paths);
    g_free(arch->archive_path);
    g_free(arch->nfo_name);
    g_free(arch->nfo_text);
    g_free(arch);
}
