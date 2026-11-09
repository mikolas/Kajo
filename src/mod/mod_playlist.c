#include "mod_playlist.h"
#include <gio/gio.h>
#include <string.h>

static void
playlist_item_free(gpointer data)
{
    KajoModPlaylistItem *it = (KajoModPlaylistItem *)data;
    if (!it) return;
    g_free(it->file_path);
    g_free(it->display_title);
    g_free(it->badge);
    g_free(it->badge_css);
    g_free(it->sub_text);
    g_free(it->dur_text);
    g_free(it->archive_nfo_name);
    g_free(it->archive_nfo_text);
    g_free(it);
}

static void
badge_for_engine(KajoModEngineKind kind, const gchar **out_badge, const gchar **out_css)
{
    switch (kind) {
    case KAJO_MOD_ENGINE_UADE:
        *out_badge = "UADE 68K";
        *out_css   = "accent";
        break;
    case KAJO_MOD_ENGINE_SIDPLAYFP:
        *out_badge = "RESIDFP";
        *out_css   = "warning";
        break;
    case KAJO_MOD_ENGINE_GME:
        *out_badge = "LIBGME";
        *out_css   = "success";
        break;
    case KAJO_MOD_ENGINE_OPENMPT:
        *out_badge = "OPENMPT";
        *out_css   = "";
        break;
    case KAJO_MOD_ENGINE_XMP:
        *out_badge = "LIBXMP";
        *out_css   = "";
        break;
    default:
        *out_badge = "MODULE";
        *out_css   = "";
        break;
    }
}

KajoModPlaylist *
kajo_mod_playlist_new(void)
{
    KajoModPlaylist *pl = g_new0(KajoModPlaylist, 1);
    pl->items    = g_ptr_array_new_with_free_func(playlist_item_free);
    pl->archives = g_ptr_array_new_with_free_func((GDestroyNotify)kajo_mod_archive_free);
    return pl;
}

void
kajo_mod_playlist_clear(KajoModPlaylist *pl)
{
    if (!pl) return;
    if (pl->items)    g_ptr_array_set_size(pl->items, 0);
    if (pl->archives) g_ptr_array_set_size(pl->archives, 0);
}

void
kajo_mod_playlist_free(KajoModPlaylist *pl)
{
    if (!pl) return;
    if (pl->items)    g_ptr_array_unref(pl->items);
    if (pl->archives) g_ptr_array_unref(pl->archives);
    g_free(pl);
}

void
kajo_mod_playlist_remove_at(KajoModPlaylist *pl, guint index)
{
    if (!pl || !pl->items || index >= pl->items->len) return;
    g_ptr_array_remove_index(pl->items, index);
}

void
kajo_mod_playlist_item_update_from_meta(KajoModPlaylistItem   *item,
                                        const KajoModMetadata *meta)
{
    if (!item || !meta) return;
    item->engine = meta->engine;
    if (meta->title && *meta->title && item->file_path) {
        g_autofree gchar *base = g_path_get_basename(item->file_path);
        if (g_ascii_strcasecmp(meta->title, base) != 0) {
            g_free(item->display_title);
            item->display_title = g_strdup_printf("%s  [%s]", meta->title, base);
        }
    }
    if (meta->pl_badge && *meta->pl_badge) {
        g_free(item->badge);
        item->badge = g_strdup(meta->pl_badge);
    }
    if (meta->pl_badge_css) {
        g_free(item->badge_css);
        item->badge_css = g_strdup(meta->pl_badge_css);
    }
    g_free(item->sub_text);
    item->sub_text = g_strdup_printf("%d SUB", MAX(1, meta->subsong_max));

    int dur = (int)MAX(0.0, meta->duration_sec);
    g_free(item->dur_text);
    item->dur_text = g_strdup_printf("%02d:%02d", dur / 60, dur % 60);
}

static int
add_single_file(KajoModPlaylist *pl,
                const gchar     *file_path,
                const gchar     *archive_label,
                const gchar     *nfo_name,
                const gchar     *nfo_text)
{
    g_autofree gchar *base = g_path_get_basename(file_path);
    if (kajo_mod_filename_is_companion(base) || kajo_mod_filename_is_nfo(base)) {
        return 0;
    }

    KajoModEngineKind kind = kajo_mod_probe_file(file_path, NULL);
    if (kind == KAJO_MOD_ENGINE_NONE) {
        return 0;
    }

    const gchar *bdg = "MODULE";
    const gchar *css = "";
    badge_for_engine(kind, &bdg, &css);

    KajoModPlaylistItem *it = g_new0(KajoModPlaylistItem, 1);
    it->file_path        = g_strdup(file_path);
    it->display_title    = archive_label ? g_strdup_printf("%s ▸ %s", archive_label, base)
                                         : g_strdup(base);
    it->badge            = g_strdup(bdg);
    it->badge_css        = g_strdup(css);
    it->sub_text         = g_strdup("1 SUB");
    it->dur_text         = g_strdup("--:--");
    it->archive_nfo_name = g_strdup(nfo_name);
    it->archive_nfo_text = g_strdup(nfo_text);
    it->engine           = kind;

    g_ptr_array_add(pl->items, it);
    return 1;
}

static int
cmp_str_ptr(gconstpointer a, gconstpointer b)
{
    const gchar *sa = *(const gchar * const *)a;
    const gchar *sb = *(const gchar * const *)b;
    return g_ascii_strcasecmp(sa ? sa : "", sb ? sb : "");
}

int
kajo_mod_playlist_add_path(KajoModPlaylist *pl,
                           const gchar     *path,
                           GError         **error)
{
    if (!pl || !path || *path == '\0') return 0;

    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *dir = g_dir_open(path, 0, error);
        if (!dir) return 0;

        g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
        const gchar *name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (name[0] == '.') continue;
            g_ptr_array_add(names, g_strdup(name));
        }
        g_dir_close(dir);
        g_ptr_array_sort(names, cmp_str_ptr);

        /* First look for a directory-level .nfo / .diz / README */
        g_autofree gchar *dir_nfo_name = NULL;
        g_autofree gchar *dir_nfo_text = NULL;
        for (guint i = 0; i < names->len; i++) {
            const gchar *nm = g_ptr_array_index(names, i);
            if (kajo_mod_filename_is_nfo(nm)) {
                g_autofree gchar *fp = g_build_filename(path, nm, NULL);
                g_autofree guint8 *raw = NULL;
                gsize rlen = 0;
                if (g_file_get_contents(fp, (gchar **)&raw, &rlen, NULL)) {
                    dir_nfo_name = g_strdup(nm);
                    dir_nfo_text = kajo_mod_text_to_utf8(raw, rlen);
                    break;
                }
            }
        }

        int total_added = 0;
        for (guint i = 0; i < names->len; i++) {
            const gchar *nm = g_ptr_array_index(names, i);
            g_autofree gchar *child = g_build_filename(path, nm, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
                total_added += kajo_mod_playlist_add_path(pl, child, NULL);
            } else if (kajo_mod_path_is_archive(child)) {
                total_added += kajo_mod_playlist_add_path(pl, child, NULL);
            } else {
                total_added += add_single_file(pl, child, NULL, dir_nfo_name, dir_nfo_text);
            }
        }
        return total_added;
    }

    if (kajo_mod_path_is_archive(path)) {
        KajoModArchive *arch = kajo_mod_archive_extract(path, error);
        if (!arch) return 0;

        g_autofree gchar *arch_base = g_path_get_basename(path);
        int added = 0;
        for (guint i = 0; i < arch->module_paths->len; i++) {
            const gchar *mpath = g_ptr_array_index(arch->module_paths, i);
            added += add_single_file(pl, mpath, arch_base, arch->nfo_name, arch->nfo_text);
        }
        if (added > 0) {
            g_ptr_array_add(pl->archives, arch);
        } else {
            kajo_mod_archive_free(arch);
        }
        return added;
    }

    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        return add_single_file(pl, path, NULL, NULL, NULL);
    }

    return 0;
}
