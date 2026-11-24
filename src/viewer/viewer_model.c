#include "viewer_model.h"

#include <gio/gio.h>
#include <math.h>
#include <string.h>

struct _ViewerHistory {
    GPtrArray *entries; /* ViewerHistoryEntry* */
    int        cursor;
};

static void
history_entry_free(gpointer p)
{
    ViewerHistoryEntry *e = p;
    if (!e)
        return;
    g_free(e->path);
    g_free(e);
}

static gboolean
has_any_ext(const char *lower_path, const char *const *exts)
{
    for (size_t i = 0; exts[i] != NULL; i++) {
        if (g_str_has_suffix(lower_path, exts[i]))
            return TRUE;
    }
    return FALSE;
}

ViewerDocKind
viewer_model_classify_path(const char *path, const char *mime_type)
{
    if (path && *path) {
        g_autofree char *clean = g_strdup(path);
        char *frag = strpbrk(clean, "?#");
        if (frag)
            *frag = '\0';
        g_autofree char *low = g_ascii_strdown(clean, -1);

        static const char *const pdf_exts[] = { ".pdf", NULL };
        static const char *const md_exts[] = { ".md", ".markdown", ".mdown", ".mkd", ".mdx", NULL };
        static const char *const svg_exts[] = { ".svg", ".svgz", NULL };
        static const char *const img_exts[] = {
            ".png", ".jpg", ".jpeg", ".webp", ".avif", ".heic", ".heif",
            ".jxl", ".gif", ".bmp", ".tif", ".tiff", ".ico", ".pnm",
            ".ppm", ".pgm", ".pbm", ".tga", ".qoi", ".dng", ".cr2",
            ".nef", ".arw", ".raf", ".orf", ".rw2",
            NULL
        };

        if (has_any_ext(low, pdf_exts))
            return VIEWER_KIND_PDF;
        if (has_any_ext(low, md_exts))
            return VIEWER_KIND_MARKDOWN;
        if (has_any_ext(low, svg_exts))
            return VIEWER_KIND_SVG;
        if (has_any_ext(low, img_exts))
            return VIEWER_KIND_IMAGE;
    }

    if (mime_type && *mime_type) {
        if (g_ascii_strcasecmp(mime_type, "application/pdf") == 0)
            return VIEWER_KIND_PDF;
        if (g_ascii_strcasecmp(mime_type, "text/markdown") == 0 ||
            g_ascii_strcasecmp(mime_type, "text/x-markdown") == 0)
            return VIEWER_KIND_MARKDOWN;
        if (g_ascii_strcasecmp(mime_type, "image/svg+xml") == 0 ||
            g_ascii_strcasecmp(mime_type, "image/svg+xml-compressed") == 0)
            return VIEWER_KIND_SVG;
        if (g_str_has_prefix(mime_type, "image/"))
            return VIEWER_KIND_IMAGE;
    }

    return VIEWER_KIND_NONE;
}

gboolean
viewer_model_is_image_kind(ViewerDocKind kind)
{
    return kind == VIEWER_KIND_IMAGE || kind == VIEWER_KIND_SVG;
}

typedef struct {
    char *path;
    char *collate_key;
} SiblingSortItem;

static gint
compare_sibling_items(gconstpointer a, gconstpointer b)
{
    const SiblingSortItem *const *pa = a;
    const SiblingSortItem *const *pb = b;
    return g_strcmp0((*pa)->collate_key, (*pb)->collate_key);
}

static void
sibling_item_free(gpointer p)
{
    SiblingSortItem *item = p;
    if (!item)
        return;
    g_free(item->path);
    g_free(item->collate_key);
    g_free(item);
}

GPtrArray *
viewer_model_scan_sibling_images(const char *file_path, int *out_current_idx)
{
    if (out_current_idx)
        *out_current_idx = -1;

    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
    if (!file_path || !*file_path)
        return result;

    g_autofree char *dir_path = g_path_get_dirname(file_path);
    g_autofree char *target_base = g_path_get_basename(file_path);
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir)
        return result;

    GPtrArray *items = g_ptr_array_new_with_free_func(sibling_item_free);
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.' && g_strcmp0(name, target_base) != 0)
            continue;
        ViewerDocKind k = viewer_model_classify_path(name, NULL);
        if (!viewer_model_is_image_kind(k))
            continue;
        g_autofree char *full = g_build_filename(dir_path, name, NULL);
        if (!g_file_test(full, G_FILE_TEST_IS_REGULAR))
            continue;

        SiblingSortItem *item = g_new0(SiblingSortItem, 1);
        item->path = g_steal_pointer(&full);
        item->collate_key = g_utf8_collate_key_for_filename(name, -1);
        g_ptr_array_add(items, item);
    }
    g_dir_close(dir);

    g_ptr_array_sort(items, compare_sibling_items);

    for (guint i = 0; i < items->len; i++) {
        SiblingSortItem *item = g_ptr_array_index(items, i);
        g_autofree char *base = g_path_get_basename(item->path);
        if (out_current_idx && *out_current_idx < 0 && g_strcmp0(base, target_base) == 0) {
            *out_current_idx = (int)i;
        }
        g_ptr_array_add(result, g_steal_pointer(&item->path));
    }

    g_ptr_array_free(items, TRUE);
    return result;
}

ViewerHistory *
viewer_history_new(void)
{
    ViewerHistory *h = g_new0(ViewerHistory, 1);
    h->entries = g_ptr_array_new_with_free_func(history_entry_free);
    h->cursor = -1;
    return h;
}

void
viewer_history_free(ViewerHistory *hist)
{
    if (!hist)
        return;
    g_ptr_array_free(hist->entries, TRUE);
    g_free(hist);
}

void
viewer_history_push(ViewerHistory *hist,
                    const char    *path,
                    int            page_index,
                    double         scroll_frac)
{
    if (!hist || !path || !*path)
        return;

    /* Avoid duplicate top entry for same path */
    if (hist->cursor >= 0 && hist->cursor < (int)hist->entries->len) {
        ViewerHistoryEntry *cur = g_ptr_array_index(hist->entries, (guint)hist->cursor);
        if (g_strcmp0(cur->path, path) == 0) {
            cur->page_index = page_index;
            cur->scroll_frac = scroll_frac;
            return;
        }
    }

    if (hist->cursor + 1 < (int)hist->entries->len) {
        g_ptr_array_set_size(hist->entries, (guint)(hist->cursor + 1));
    }

    ViewerHistoryEntry *entry = g_new0(ViewerHistoryEntry, 1);
    entry->path = g_strdup(path);
    entry->page_index = page_index;
    entry->scroll_frac = scroll_frac;
    g_ptr_array_add(hist->entries, entry);
    hist->cursor = (int)hist->entries->len - 1;
}

void
viewer_history_update_current(ViewerHistory *hist, int page_index, double scroll_frac)
{
    if (!hist || hist->cursor < 0 || hist->cursor >= (int)hist->entries->len)
        return;
    ViewerHistoryEntry *cur = g_ptr_array_index(hist->entries, (guint)hist->cursor);
    cur->page_index = page_index;
    cur->scroll_frac = scroll_frac;
}

gboolean
viewer_history_can_back(const ViewerHistory *hist)
{
    return hist && hist->cursor > 0;
}

gboolean
viewer_history_can_forward(const ViewerHistory *hist)
{
    return hist && hist->cursor >= 0 && (hist->cursor + 1) < (int)hist->entries->len;
}

const ViewerHistoryEntry *
viewer_history_back(ViewerHistory *hist)
{
    if (!viewer_history_can_back(hist))
        return NULL;
    hist->cursor--;
    return g_ptr_array_index(hist->entries, (guint)hist->cursor);
}

const ViewerHistoryEntry *
viewer_history_forward(ViewerHistory *hist)
{
    if (!viewer_history_can_forward(hist))
        return NULL;
    hist->cursor++;
    return g_ptr_array_index(hist->entries, (guint)hist->cursor);
}

const ViewerHistoryEntry *
viewer_history_current(const ViewerHistory *hist)
{
    if (!hist || hist->cursor < 0 || hist->cursor >= (int)hist->entries->len)
        return NULL;
    return g_ptr_array_index(hist->entries, (guint)hist->cursor);
}

double
viewer_model_step_zoom(double current, int direction, double min_zoom, double max_zoom)
{
    static const double steps[] = {
        0.10, 0.15, 0.20, 0.25, 0.33, 0.50, 0.67, 0.75, 0.85, 1.00,
        1.15, 1.25, 1.50, 1.75, 2.00, 2.50, 3.00, 4.00, 6.00, 8.00,
        12.00, 16.00
    };

    double next = current;
    if (direction > 0) {
        next = max_zoom;
        for (size_t i = 0; i < G_N_ELEMENTS(steps); i++) {
            if (steps[i] > current + 0.005) {
                next = steps[i];
                break;
            }
        }
    } else if (direction < 0) {
        next = min_zoom;
        for (size_t i = G_N_ELEMENTS(steps); i > 0; i--) {
            if (steps[i - 1] < current - 0.005) {
                next = steps[i - 1];
                break;
            }
        }
    }
    return CLAMP(next, min_zoom, max_zoom);
}

double
viewer_model_compute_fit_scale(double        content_w,
                               double        content_h,
                               double        view_w,
                               double        view_h,
                               ViewerFitMode mode)
{
    if (content_w <= 1.0 || content_h <= 1.0 || view_w <= 1.0 || view_h <= 1.0)
        return 1.0;

    double sx = view_w / content_w;
    double sy = view_h / content_h;

    switch (mode) {
    case VIEWER_FIT_WIDTH:
        return CLAMP(sx, 0.1, 16.0);
    case VIEWER_FIT_PAGE:
    case VIEWER_FIT_CONTAIN:
        return CLAMP(fmin(sx, sy), 0.1, 16.0);
    case VIEWER_FIT_FILL:
        return CLAMP(fmax(sx, sy), 0.1, 16.0);
    case VIEWER_FIT_ACTUAL:
    default:
        return 1.0;
    }
}

void
viewer_model_format_bytes(guint64 bytes, char *out_buf, size_t buf_len)
{
    if (!out_buf || buf_len == 0)
        return;

    if (bytes < 1024ULL) {
        g_snprintf(out_buf, buf_len, "%u B", (unsigned)bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        g_snprintf(out_buf, buf_len, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        g_snprintf(out_buf, buf_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        g_snprintf(out_buf, buf_len, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}