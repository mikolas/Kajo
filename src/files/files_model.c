#include "files_model.h"
#include <string.h>
#include <sys/stat.h>

struct _KajoFileItem {
    GObject parent_instance;

    GFile *file;
    char *uri;
    char *target_uri;
    char *name;
    char *display_name;
    char *name_folded;
    char *display_folded;
    char *collate_key;
    char *content_type;
    char *type_description;
    char *icon_name;
    GIcon *gicon;

    guint64 size;
    gint64 mtime_sec;
    guint32 unix_mode;

    gboolean is_dir;
    gboolean is_hidden;
    gboolean is_symlink;
    gboolean is_mountable;

    char *symlink_target;
    char *trash_orig_path;
    char *trash_deletion_date;
};

G_DEFINE_TYPE(KajoFileItem, kajo_file_item, G_TYPE_OBJECT)

static void
kajo_file_item_finalize(GObject *object)
{
    KajoFileItem *self = KAJO_FILE_ITEM(object);

    g_clear_object(&self->file);
    g_clear_object(&self->gicon);
    g_clear_pointer(&self->uri, g_free);
    g_clear_pointer(&self->target_uri, g_free);
    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->display_name, g_free);
    g_clear_pointer(&self->name_folded, g_free);
    g_clear_pointer(&self->display_folded, g_free);
    g_clear_pointer(&self->collate_key, g_free);
    g_clear_pointer(&self->content_type, g_free);
    g_clear_pointer(&self->type_description, g_free);
    g_clear_pointer(&self->icon_name, g_free);
    g_clear_pointer(&self->symlink_target, g_free);
    g_clear_pointer(&self->trash_orig_path, g_free);
    g_clear_pointer(&self->trash_deletion_date, g_free);

    G_OBJECT_CLASS(kajo_file_item_parent_class)->finalize(object);
}

static void
kajo_file_item_class_init(KajoFileItemClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = kajo_file_item_finalize;
}

static void
kajo_file_item_init(KajoFileItem *self G_GNUC_UNUSED)
{
}

static char *
resolve_fallback_icon_name(const char *content_type, gboolean is_dir, gboolean is_mountable)
{
    if (is_mountable) return g_strdup("network-server-symbolic");
    if (is_dir) return g_strdup("folder");
    if (!content_type) return g_strdup("text-x-generic");

    if (g_str_has_prefix(content_type, "image/"))
        return g_strdup("image-x-generic");
    if (g_str_has_prefix(content_type, "video/"))
        return g_strdup("video-x-generic");
    if (g_str_has_prefix(content_type, "audio/"))
        return g_strdup("audio-x-generic");
    if (g_strcmp0(content_type, "application/pdf") == 0)
        return g_strdup("x-office-document");
    if (strstr(content_type, "zip") || strstr(content_type, "tar") ||
        strstr(content_type, "compressed") || strstr(content_type, "archive"))
        return g_strdup("package-x-generic");
    if (g_str_has_prefix(content_type, "text/") ||
        strstr(content_type, "json") || strstr(content_type, "xml") ||
        strstr(content_type, "script"))
        return g_strdup("text-x-generic");
    if (strstr(content_type, "executable") || strstr(content_type, "sharedlib"))
        return g_strdup("application-x-executable");

    return g_strdup("text-x-generic");
}

static char *
resolve_type_description(const char *content_type, gboolean is_dir, gboolean is_mountable)
{
    if (is_mountable) return g_strdup("Network Location");
    if (is_dir) return g_strdup("Folder");
    if (!content_type || content_type[0] == '\0') return g_strdup("File");

    char *desc = g_content_type_get_description(content_type);
    if (desc && desc[0] != '\0')
        return desc;
    g_free(desc);
    return g_strdup(content_type);
}

KajoFileItem *
kajo_file_item_new_from_info(GFile *parent_dir, GFileInfo *info)
{
    g_return_val_if_fail(G_IS_FILE(parent_dir), NULL);
    g_return_val_if_fail(G_IS_FILE_INFO(info), NULL);

    KajoFileItem *self = g_object_new(KAJO_TYPE_FILE_ITEM, NULL);

    const char *name = g_file_info_get_name(info);
    if (!name) name = g_file_info_get_display_name(info);
    if (!name) name = "unknown";

    const char *disp = g_file_info_get_display_name(info);
    if (!disp || disp[0] == '\0') disp = name;

    self->name = g_strdup(name);
    self->display_name = g_strdup(disp);
    self->name_folded = g_utf8_casefold(self->name, -1);
    self->display_folded = g_utf8_casefold(self->display_name, -1);
    self->collate_key = g_utf8_collate_key_for_filename(
        self->display_folded ? self->display_folded : self->display_name, -1
    );
    self->file = g_file_get_child(parent_dir, name);
    self->uri = g_file_get_uri(self->file);

    const char *target = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
    if (target && target[0] != '\0')
        self->target_uri = g_strdup(target);

    GFileType ftype = g_file_info_get_file_type(info);
    self->is_dir = (ftype == G_FILE_TYPE_DIRECTORY);
    self->is_mountable = (ftype == G_FILE_TYPE_MOUNTABLE);
    self->is_hidden = g_file_info_get_is_hidden(info) || (name[0] == '.');
    self->is_symlink = g_file_info_get_is_symlink(info);

    const char *ctype = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
    if (!ctype && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE))
        ctype = g_file_info_get_content_type(info);
    if (!ctype)
        ctype = self->is_dir ? "inode/directory" : "application/octet-stream";

    self->content_type = g_strdup(ctype);
    self->type_description = NULL; /* Lazily resolved on demand */

    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_ICON)) {
        GIcon *icon = g_file_info_get_icon(info);
        if (icon)
            self->gicon = g_object_ref(icon);
    }
    self->icon_name = resolve_fallback_icon_name(self->content_type, self->is_dir, self->is_mountable);

    self->size = (guint64)g_file_info_get_size(info);

    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
        self->mtime_sec = (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    } else {
        g_autoptr(GDateTime) dt = g_file_info_get_modification_date_time(info);
        self->mtime_sec = dt ? g_date_time_to_unix(dt) : 0;
    }

    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE))
        self->unix_mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    else
        self->unix_mode = self->is_dir ? 0755 : 0644;

    if (self->is_symlink) {
        const char *sym = g_file_info_get_symlink_target(info);
        if (sym) self->symlink_target = g_strdup(sym);
    }

    const char *orig_path = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
    if (!orig_path)
        orig_path = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
    if (orig_path)
        self->trash_orig_path = g_strdup(orig_path);

    const char *del_date = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_TRASH_DELETION_DATE);
    if (del_date)
        self->trash_deletion_date = g_strdup(del_date);

    return self;
}

KajoFileItem *
kajo_file_item_new_manual(GFile *file,
                          const char *name,
                          const char *display_name,
                          const char *content_type,
                          guint64 size,
                          gint64 mtime_sec,
                          guint32 unix_mode,
                          gboolean is_dir,
                          gboolean is_hidden)
{
    KajoFileItem *self = g_object_new(KAJO_TYPE_FILE_ITEM, NULL);

    if (file) {
        self->file = g_object_ref(file);
        self->uri = g_file_get_uri(file);
    } else {
        self->uri = g_strdup_printf("file:///%s", name ? name : "item");
        self->file = g_file_new_for_uri(self->uri);
    }

    self->name = g_strdup(name ? name : "item");
    self->display_name = g_strdup(display_name ? display_name : self->name);
    self->name_folded = g_utf8_casefold(self->name, -1);
    self->display_folded = g_utf8_casefold(self->display_name, -1);
    self->collate_key = g_utf8_collate_key_for_filename(
        self->display_folded ? self->display_folded : self->display_name, -1
    );
    self->content_type = g_strdup(content_type ? content_type : (is_dir ? "inode/directory" : "application/octet-stream"));
    self->type_description = resolve_type_description(self->content_type, is_dir, FALSE);
    self->icon_name = resolve_fallback_icon_name(self->content_type, is_dir, FALSE);
    self->size = size;
    self->mtime_sec = mtime_sec;
    self->unix_mode = unix_mode;
    self->is_dir = is_dir;
    self->is_hidden = is_hidden;

    return self;
}

GFile      *kajo_file_item_get_file(const KajoFileItem *self) { return self ? self->file : NULL; }
const char *kajo_file_item_get_uri(const KajoFileItem *self) { return self ? self->uri : NULL; }
const char *kajo_file_item_get_target_uri(const KajoFileItem *self) { return self ? self->target_uri : NULL; }
const char *kajo_file_item_get_name(const KajoFileItem *self) { return self ? self->name : ""; }
const char *kajo_file_item_get_display_name(const KajoFileItem *self) { return self ? self->display_name : ""; }
const char *kajo_file_item_get_content_type(const KajoFileItem *self) { return self ? self->content_type : ""; }
const char *
kajo_file_item_get_type_description(const KajoFileItem *self)
{
    if (!self) return "";
    if (!self->type_description) {
        KajoFileItem *mutable_self = (KajoFileItem *)self;
        mutable_self->type_description = resolve_type_description(
            self->content_type, self->is_dir, self->is_mountable
        );
    }
    return self->type_description ? self->type_description : "";
}
const char *kajo_file_item_get_icon_name(const KajoFileItem *self) { return self ? self->icon_name : "text-x-generic"; }
GIcon      *kajo_file_item_get_gicon(const KajoFileItem *self) { return self ? self->gicon : NULL; }
guint64     kajo_file_item_get_size(const KajoFileItem *self) { return self ? self->size : 0; }
gint64      kajo_file_item_get_mtime(const KajoFileItem *self) { return self ? self->mtime_sec : 0; }
guint32     kajo_file_item_get_unix_mode(const KajoFileItem *self) { return self ? self->unix_mode : 0; }
gboolean    kajo_file_item_is_dir(const KajoFileItem *self) { return self ? self->is_dir : FALSE; }
gboolean    kajo_file_item_is_hidden(const KajoFileItem *self) { return self ? self->is_hidden : FALSE; }
gboolean    kajo_file_item_is_symlink(const KajoFileItem *self) { return self ? self->is_symlink : FALSE; }
gboolean    kajo_file_item_is_mountable(const KajoFileItem *self) { return self ? self->is_mountable : FALSE; }
const char *kajo_file_item_get_symlink_target(const KajoFileItem *self) { return self ? self->symlink_target : NULL; }
const char *kajo_file_item_get_trash_orig_path(const KajoFileItem *self) { return self ? self->trash_orig_path : NULL; }
const char *kajo_file_item_get_trash_deletion_date(const KajoFileItem *self) { return self ? self->trash_deletion_date : NULL; }

char *
kajo_files_format_size(guint64 bytes, gboolean is_dir)
{
    if (is_dir)
        return g_strdup("[ FOLDER ]");
    if (bytes < 1024ULL)
        return g_strdup_printf("%" G_GUINT64_FORMAT " B", bytes);
    if (bytes < 1024ULL * 1024ULL)
        return g_strdup_printf("%.1f KB", (double)bytes / 1024.0);
    if (bytes < 1024ULL * 1024ULL * 1024ULL)
        return g_strdup_printf("%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return g_strdup_printf("%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

char *
kajo_files_format_permissions(guint32 mode, gboolean is_dir, gboolean is_symlink)
{
    char perm[11];
    perm[0] = is_symlink ? 'l' : (is_dir ? 'd' : '-');
    perm[1] = (mode & 0400) ? 'r' : '-';
    perm[2] = (mode & 0200) ? 'w' : '-';
    perm[3] = (mode & 0100) ? 'x' : '-';
    perm[4] = (mode & 0040) ? 'r' : '-';
    perm[5] = (mode & 0020) ? 'w' : '-';
    perm[6] = (mode & 0010) ? 'x' : '-';
    perm[7] = (mode & 0004) ? 'r' : '-';
    perm[8] = (mode & 0002) ? 'w' : '-';
    perm[9] = (mode & 0001) ? 'x' : '-';
    perm[10] = '\0';

    return g_strdup_printf("%s (%04o)", perm, mode & 07777);
}

char *
kajo_files_format_mtime(gint64 mtime_sec)
{
    if (mtime_sec <= 0)
        return g_strdup("--");
    g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(mtime_sec);
    if (!dt)
        return g_strdup("--");
    return g_date_time_format(dt, "%Y-%m-%d %H:%M");
}

gboolean
kajo_files_is_text_or_code(const char *content_type, const char *filename)
{
    if (content_type) {
        if (g_str_has_prefix(content_type, "text/"))
            return TRUE;
        if (strstr(content_type, "json") || strstr(content_type, "xml") ||
            strstr(content_type, "javascript") || strstr(content_type, "shellscript") ||
            strstr(content_type, "x-sh") || strstr(content_type, "yaml") ||
            strstr(content_type, "toml") || strstr(content_type, "markdown"))
            return TRUE;
    }

    if (filename) {
        static const char *const text_exts[] = {
            ".c", ".h", ".cpp", ".hpp", ".cc", ".rs", ".py", ".js", ".ts",
            ".json", ".toml", ".yaml", ".yml", ".ini", ".conf", ".cfg",
            ".kdl", ".blp", ".ui", ".css", ".html", ".xml", ".svg",
            ".md", ".txt", ".log", ".sh", ".zsh", ".bash", ".fish",
            ".desktop", ".service", ".meson", "meson.build", "Makefile",
            ".gitignore", ".gitmodules", NULL
        };
        for (int i = 0; text_exts[i] != NULL; i++) {
            if (g_str_has_suffix(filename, text_exts[i]) || g_strcmp0(filename, text_exts[i]) == 0)
                return TRUE;
        }
    }

    return FALSE;
}

gboolean
kajo_files_is_archive(const char *content_type, const char *filename)
{
    if (content_type) {
        if (strstr(content_type, "zip") ||
            strstr(content_type, "tar") ||
            strstr(content_type, "7z") ||
            strstr(content_type, "rar") ||
            strstr(content_type, "compressed") ||
            strstr(content_type, "archive") ||
            strstr(content_type, "x-iso9660-image")) {
            return TRUE;
        }
    }
    if (filename) {
        static const char *const exts[] = {
            ".zip", ".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz",
            ".tar.bz2", ".tbz2", ".tar.zst", ".tzst", ".7z", ".rar",
            ".iso", ".cpio", ".deb", ".rpm", NULL
        };
        for (int i = 0; exts[i] != NULL; i++) {
            if (g_str_has_suffix(filename, exts[i]))
                return TRUE;
        }
    }
    return FALSE;
}

gboolean
kajo_file_sort_field_default_ascending(KajoFileSortField field)
{
    switch (field) {
    case KAJO_SORT_SIZE:
    case KAJO_SORT_MODIFIED:
        return FALSE; /* Largest / Newest first by default */
    case KAJO_SORT_NAME:
    case KAJO_SORT_TYPE:
    default:
        return TRUE;  /* A -> Z first by default */
    }
}

const char *
kajo_file_sort_field_to_string(KajoFileSortField field)
{
    switch (field) {
    case KAJO_SORT_SIZE:     return "size";
    case KAJO_SORT_MODIFIED: return "modified";
    case KAJO_SORT_TYPE:     return "type";
    case KAJO_SORT_NAME:
    default:                 return "name";
    }
}

KajoFileSortField
kajo_file_sort_field_from_string(const char *str)
{
    if (!str) return KAJO_SORT_NAME;
    if (g_ascii_strcasecmp(str, "size") == 0)     return KAJO_SORT_SIZE;
    if (g_ascii_strcasecmp(str, "modified") == 0) return KAJO_SORT_MODIFIED;
    if (g_ascii_strcasecmp(str, "type") == 0)     return KAJO_SORT_TYPE;
    return KAJO_SORT_NAME;
}

const char *
kajo_file_view_mode_to_string(KajoFileViewMode mode)
{
    return (mode == KAJO_VIEW_LIST) ? "list" : "grid";
}

KajoFileViewMode
kajo_file_view_mode_from_string(const char *str)
{
    if (str && g_ascii_strcasecmp(str, "list") == 0)
        return KAJO_VIEW_LIST;
    return KAJO_VIEW_GRID;
}

static int
compare_natural_names(const KajoFileItem *a, const KajoFileItem *b)
{
    const char *ka = a->collate_key ? a->collate_key : "";
    const char *kb = b->collate_key ? b->collate_key : "";
    int cmp = strcmp(ka, kb);
    if (cmp == 0) {
        cmp = strcmp(a->display_name ? a->display_name : "",
                     b->display_name ? b->display_name : "");
    }
    return cmp;
}

int
kajo_file_item_compare_full(const KajoFileItem *a,
                            const KajoFileItem *b,
                            KajoFileSortField field,
                            gboolean ascending,
                            gboolean folders_first)
{
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;

    if (folders_first && a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;

    int cmp = 0;
    switch (field) {
    case KAJO_SORT_SIZE:
        if (a->size < b->size) cmp = -1;
        else if (a->size > b->size) cmp = 1;
        break;
    case KAJO_SORT_MODIFIED:
        if (a->mtime_sec < b->mtime_sec) cmp = -1;
        else if (a->mtime_sec > b->mtime_sec) cmp = 1;
        break;
    case KAJO_SORT_TYPE:
        cmp = g_ascii_strcasecmp(kajo_file_item_get_type_description(a),
                                 kajo_file_item_get_type_description(b));
        break;
    case KAJO_SORT_NAME:
    default:
        cmp = compare_natural_names(a, b);
        return ascending ? cmp : -cmp;
    }

    if (cmp != 0)
        return ascending ? cmp : -cmp;

    /* Tie-breaker always remains natural A -> Z regardless of ascending/descending */
    return compare_natural_names(a, b);
}

int
kajo_file_item_compare(const KajoFileItem *a,
                       const KajoFileItem *b,
                       KajoFileSortField field,
                       gboolean ascending)
{
    return kajo_file_item_compare_full(a, b, field, ascending, TRUE);
}

gboolean
kajo_file_item_matches_filter(const KajoFileItem *item,
                              gboolean show_hidden,
                              const char *search_query)
{
    if (!item) return FALSE;
    if (!show_hidden && item->is_hidden)
        return FALSE;

    if (!search_query || search_query[0] == '\0')
        return TRUE;

    g_autofree char *q_folded = g_utf8_casefold(search_query, -1);
    const char *q = q_folded ? q_folded : search_query;

    if (item->display_folded && strstr(item->display_folded, q) != NULL)
        return TRUE;
    if (item->name_folded && strstr(item->name_folded, q) != NULL)
        return TRUE;

    return FALSE;
}

void
kajo_breadcrumb_segment_free(gpointer data)
{
    KajoBreadcrumbSegment *seg = data;
    if (!seg) return;
    g_free(seg->uri);
    g_free(seg->label);
    g_free(seg->tooltip);
    g_free(seg);
}

GPtrArray *
kajo_files_build_breadcrumb_chain(GFile *dir)
{
    GPtrArray *result = g_ptr_array_new_with_free_func(kajo_breadcrumb_segment_free);
    if (!dir)
        return result;

    g_autoptr(GPtrArray) chain = g_ptr_array_new_with_free_func(g_object_unref);
    GFile *cursor = g_object_ref(dir);
    const char *home_dir = g_get_home_dir();
    g_autoptr(GFile) home_file = home_dir ? g_file_new_for_path(home_dir) : NULL;

    while (cursor != NULL) {
        g_ptr_array_insert(chain, 0, cursor);
        cursor = g_file_get_parent(cursor);
    }

    for (guint i = 0; i < chain->len; i++) {
        GFile *seg = g_ptr_array_index(chain, i);
        g_autofree char *seg_uri = g_file_get_uri(seg);
        g_autofree char *seg_path = g_file_get_path(seg);
        char *seg_label = NULL;

        if (home_file && g_file_equal(seg, home_file)) {
            g_autofree char *hbase = g_file_get_basename(seg);
            seg_label = (hbase && hbase[0] != '\0')
                ? g_strdup_printf("⌂ %s", hbase)
                : g_strdup("⌂ HOME");
        } else {
            g_autofree char *base = g_file_get_basename(seg);
            if (!base || g_strcmp0(base, "/") == 0 || base[0] == '\0') {
                g_autofree char *scheme = g_file_get_uri_scheme(seg);
                if (g_strcmp0(scheme, "trash") == 0)
                    seg_label = g_strdup("🗑 TRASH");
                else if (g_strcmp0(scheme, "network") == 0)
                    seg_label = g_strdup("🌐 NETWORK");
                else if (g_strcmp0(scheme, "recent") == 0)
                    seg_label = g_strdup("🕒 RECENT");
                else if (g_strcmp0(scheme, "file") == 0)
                    seg_label = g_strdup("/");
                else
                    seg_label = g_strdup(seg_uri ? seg_uri : "/");
            } else {
                seg_label = g_steal_pointer(&base);
            }
        }

        KajoBreadcrumbSegment *out_seg = g_new0(KajoBreadcrumbSegment, 1);
        out_seg->uri = g_strdup(seg_uri ? seg_uri : "");
        out_seg->label = seg_label;
        out_seg->tooltip = g_strdup(seg_path ? seg_path : (seg_uri ? seg_uri : ""));
        g_ptr_array_add(result, out_seg);
    }

    return result;
}

KajoRebuildSelectionAction
kajo_files_rebuild_selection_action(gboolean has_matches,
                                    gboolean autoselect_pending,
                                    gboolean search_active)
{
    if (has_matches)
        return KAJO_REBUILD_SEL_RESTORE;
    if (autoselect_pending || search_active)
        return KAJO_REBUILD_SEL_FIRST;
    return KAJO_REBUILD_SEL_CLEAR;
}

GtkBitset *
kajo_files_match_selection_uris(GPtrArray *visible_items, GPtrArray *selected_uris)
{
    GtkBitset *bitset = gtk_bitset_new_empty();
    if (!visible_items || !selected_uris || visible_items->len == 0 || selected_uris->len == 0)
        return bitset;

    g_autoptr(GHashTable) uri_set = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < selected_uris->len; i++) {
        const char *u = g_ptr_array_index(selected_uris, i);
        if (u && u[0] != '\0')
            g_hash_table_add(uri_set, (gpointer)u);
    }

    for (guint i = 0; i < visible_items->len; i++) {
        KajoFileItem *it = g_ptr_array_index(visible_items, i);
        if (!it) continue;
        const char *u = kajo_file_item_get_uri(it);
        if (!u || u[0] == '\0')
            u = kajo_file_item_get_name(it);
        if (u && g_hash_table_contains(uri_set, u)) {
            gtk_bitset_add(bitset, i);
        }
    }

    return bitset;
}

/* ─── Async Worker-Thread Directory Enumeration ─── */

#define KAJO_ENUM_ATTRIBUTES \
    G_FILE_ATTRIBUTE_STANDARD_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
    G_FILE_ATTRIBUTE_STANDARD_SIZE "," \
    G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK "," \
    G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET "," \
    G_FILE_ATTRIBUTE_STANDARD_TARGET_URI "," \
    G_FILE_ATTRIBUTE_TIME_MODIFIED "," \
    G_FILE_ATTRIBUTE_UNIX_MODE "," \
    G_FILE_ATTRIBUTE_TRASH_ORIG_PATH "," \
    G_FILE_ATTRIBUTE_TRASH_DELETION_DATE "," \
    G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT

typedef struct {
    GFile *dir;
    KajoDirEnumCallback callback;
    gpointer user_data;
} EnumTaskData;

static void
enum_task_data_free(EnumTaskData *d)
{
    if (!d) return;
    g_clear_object(&d->dir);
    g_free(d);
}

static void
enumerate_thread_cb(GTask *task,
                    gpointer source_object G_GNUC_UNUSED,
                    gpointer task_data,
                    GCancellable *cancellable)
{
    EnumTaskData *d = task_data;
    GError *error = NULL;

    g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
        d->dir,
        KAJO_ENUM_ATTRIBUTES,
        G_FILE_QUERY_INFO_NONE,
        cancellable,
        &error
    );

    if (!enumerator) {
        g_task_return_error(task, error);
        return;
    }

    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);

    while (!g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        GFileInfo *info = g_file_enumerator_next_file(enumerator, cancellable, &error);
        if (error) {
            g_ptr_array_unref(items);
            g_file_enumerator_close(enumerator, NULL, NULL);
            g_task_return_error(task, error);
            return;
        }
        if (!info)
            break;

        KajoFileItem *item = kajo_file_item_new_from_info(d->dir, info);
        if (item)
            g_ptr_array_add(items, item);
        g_object_unref(info);
    }

    g_file_enumerator_close(enumerator, NULL, NULL);

    if (error) {
        g_ptr_array_unref(items);
        g_task_return_error(task, error);
        return;
    }

    g_task_return_pointer(task, items, (GDestroyNotify)g_ptr_array_unref);
}

static void
on_enumerate_task_completed(GObject *source G_GNUC_UNUSED,
                            GAsyncResult *res,
                            gpointer user_data)
{
    EnumTaskData *d = user_data;
    GError *error = NULL;
    GPtrArray *items = g_task_propagate_pointer(G_TASK(res), &error);

    if (d->callback) {
        d->callback(items, error, d->user_data);
    } else if (items) {
        g_ptr_array_unref(items);
    }

    g_clear_error(&error);
    enum_task_data_free(d);
}

void
kajo_files_enumerate_async(GFile *dir,
                           GCancellable *cancellable,
                           KajoDirEnumCallback callback,
                           gpointer user_data)
{
    g_return_if_fail(G_IS_FILE(dir));

    EnumTaskData *d = g_new0(EnumTaskData, 1);
    d->dir = g_object_ref(dir);
    d->callback = callback;
    d->user_data = user_data;

    g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_enumerate_task_completed, d);
    g_task_set_task_data(task, d, NULL);
    g_task_run_in_thread(task, enumerate_thread_cb);
}
