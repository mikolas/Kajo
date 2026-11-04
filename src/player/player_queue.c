#include "player_queue.h"
#include <string.h>

struct _KajoPlayerQueue {
    GtkListBox *list_box;
    GtkWidget *count_badge_lbl;
    GPtrArray *tracks; /* KajoMediaTrack* */
    int current_index;
    gboolean suppress_row_signal;

    KajoQueueTrackSelectedFunc select_cb;
    gpointer user_data;
};

char *
kajo_player_format_time(gint64 time_usec)
{
    if (time_usec < 0)
        time_usec = 0;

    gint64 total_sec = time_usec / G_USEC_PER_SEC;
    gint64 hours = total_sec / 3600;
    gint64 mins = (total_sec % 3600) / 60;
    gint64 secs = total_sec % 60;

    if (hours > 0)
        return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, hours, mins, secs);
    return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, mins, secs);
}

char *
kajo_player_format_size(guint64 bytes)
{
    if (bytes < 1024ULL)
        return g_strdup_printf("%" G_GUINT64_FORMAT " B", bytes);
    if (bytes < 1024ULL * 1024ULL)
        return g_strdup_printf("%.1f KB", (double)bytes / 1024.0);
    if (bytes < 1024ULL * 1024ULL * 1024ULL)
        return g_strdup_printf("%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return g_strdup_printf("%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

gboolean
kajo_player_is_supported_media(const char *content_type, const char *filename)
{
    if (content_type) {
        if (g_str_has_prefix(content_type, "video/") || g_str_has_prefix(content_type, "audio/"))
            return TRUE;
        if (g_strcmp0(content_type, "application/ogg") == 0 ||
            g_strcmp0(content_type, "application/vnd.rn-realmedia") == 0)
            return TRUE;
    }

    if (filename) {
        g_autofree char *lower = g_ascii_strdown(filename, -1);
        static const char *const media_exts[] = {
            ".mkv", ".mp4", ".m4v", ".webm", ".mov", ".avi", ".wmv", ".flv", ".ts", ".m2ts", ".mpg", ".mpeg", ".ogv",
            ".mp3", ".flac", ".ogg", ".opus", ".wav", ".m4a", ".aac", ".wma", ".alac", ".aiff",
            NULL
        };
        for (int i = 0; media_exts[i] != NULL; i++) {
            if (g_str_has_suffix(lower, media_exts[i]))
                return TRUE;
        }
    }

    return FALSE;
}

gboolean
kajo_player_is_audio_only(const char *content_type, const char *filename)
{
    if (content_type && g_str_has_prefix(content_type, "audio/"))
        return TRUE;
    if (content_type && g_str_has_prefix(content_type, "video/"))
        return FALSE;

    if (filename) {
        g_autofree char *lower = g_ascii_strdown(filename, -1);
        static const char *const audio_exts[] = {
            ".mp3", ".flac", ".ogg", ".opus", ".wav", ".m4a", ".aac", ".wma", ".alac", ".aiff",
            NULL
        };
        for (int i = 0; audio_exts[i] != NULL; i++) {
            if (g_str_has_suffix(lower, audio_exts[i]))
                return TRUE;
        }
    }
    return FALSE;
}

KajoMediaTrack *
kajo_media_track_new(GFile *file,
                     const char *display_name,
                     const char *content_type,
                     guint64 size)
{
    g_return_val_if_fail(G_IS_FILE(file), NULL);

    KajoMediaTrack *t = g_new0(KajoMediaTrack, 1);
    t->file = g_object_ref(file);
    t->uri = g_file_get_uri(file);

    if (display_name && display_name[0] != '\0') {
        t->display_name = g_strdup(display_name);
    } else {
        g_autofree char *base = g_file_get_basename(file);
        t->display_name = g_strdup(base ? base : "Media Stream");
    }

    if (content_type && content_type[0] != '\0') {
        t->content_type = g_strdup(content_type);
    } else {
        gboolean uncertain = FALSE;
        g_autofree char *guessed = g_content_type_guess(t->display_name, NULL, 0, &uncertain);
        t->content_type = g_strdup(guessed ? guessed : "video/mp4");
    }

    t->size = size;
    t->is_audio_only = kajo_player_is_audio_only(t->content_type, t->display_name);
    return t;
}

void
kajo_media_track_free(KajoMediaTrack *track)
{
    if (!track) return;
    g_clear_object(&track->file);
    g_free(track->uri);
    g_free(track->display_name);
    g_free(track->content_type);
    g_free(track);
}

static gint
compare_tracks_by_name(gconstpointer a, gconstpointer b)
{
    const KajoMediaTrack *ta = *(const KajoMediaTrack *const *)a;
    const KajoMediaTrack *tb = *(const KajoMediaTrack *const *)b;
    return g_utf8_collate(
        ta && ta->display_name ? ta->display_name : "",
        tb && tb->display_name ? tb->display_name : ""
    );
}

static void
rebuild_queue_listbox(KajoPlayerQueue *self)
{
    if (!self) return;

    if (self->count_badge_lbl) {
        g_autofree char *cnt = g_strdup_printf("[ %u ]", self->tracks ? self->tracks->len : 0);
        gtk_label_set_text(GTK_LABEL(self->count_badge_lbl), cnt);
    }

    if (!self->list_box) return;

    self->suppress_row_signal = TRUE;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != NULL) {
        gtk_list_box_remove(self->list_box, child);
    }

    for (guint i = 0; i < self->tracks->len; i++) {
        KajoMediaTrack *t = g_ptr_array_index(self->tracks, i);

        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

        const char *ic_name = t->is_audio_only ? "audio-x-generic" : "video-x-generic";
        GtkWidget *icon = gtk_image_new_from_icon_name(ic_name);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(vbox, TRUE);
        gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

        GtkWidget *lbl_title = gtk_label_new(t->display_name);
        gtk_widget_add_css_class(lbl_title, "sidebar-label");
        gtk_widget_set_halign(lbl_title, GTK_ALIGN_START);
        gtk_widget_set_valign(lbl_title, GTK_ALIGN_CENTER);
        gtk_label_set_ellipsize(GTK_LABEL(lbl_title), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(vbox), lbl_title);

        if (t->size > 0) {
            g_autofree char *sz_str = kajo_player_format_size(t->size);
            GtkWidget *lbl_sub = gtk_label_new(sz_str);
            gtk_widget_add_css_class(lbl_sub, "shell-popover-subtitle");
            gtk_widget_set_halign(lbl_sub, GTK_ALIGN_START);
            gtk_widget_set_valign(lbl_sub, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(vbox), lbl_sub);
        }

        gtk_box_append(GTK_BOX(hbox), icon);
        gtk_box_append(GTK_BOX(hbox), vbox);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
        gtk_list_box_append(self->list_box, row);

        if ((int)i == self->current_index) {
            gtk_list_box_select_row(self->list_box, GTK_LIST_BOX_ROW(row));
        }
    }

    self->suppress_row_signal = FALSE;
}

static void
on_queue_row_selected(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
    KajoPlayerQueue *self = user_data;
    if (!row || self->suppress_row_signal)
        return;

    int idx = gtk_list_box_row_get_index(row);
    if (idx >= 0 && idx < (int)self->tracks->len && idx != self->current_index) {
        self->current_index = idx;
        const KajoMediaTrack *t = g_ptr_array_index(self->tracks, (guint)idx);
        if (self->select_cb)
            self->select_cb(t, idx, (int)self->tracks->len, self->user_data);
    }
}

static KajoMediaTrack *
query_track_from_gfile(GFile *file)
{
    g_autoptr(GFileInfo) info = g_file_query_info(
        file,
        G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NONE,
        NULL,
        NULL
    );

    const char *disp = info ? g_file_info_get_display_name(info) : NULL;
    const char *ctype = info ? g_file_info_get_content_type(info) : NULL;
    if (!ctype && info)
        ctype = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
    guint64 sz = info ? (guint64)g_file_info_get_size(info) : 0;

    return kajo_media_track_new(file, disp, ctype, sz);
}

void
kajo_player_queue_load_file_and_siblings(KajoPlayerQueue *self, GFile *target_file)
{
    g_return_if_fail(self != NULL && G_IS_FILE(target_file));

    g_ptr_array_set_size(self->tracks, 0);
    self->current_index = -1;

    g_autoptr(GFile) parent = g_file_get_parent(target_file);
    if (parent) {
        g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
            parent,
            G_FILE_ATTRIBUTE_STANDARD_NAME ","
            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
            G_FILE_ATTRIBUTE_STANDARD_TYPE ","
            G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
            G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE ","
            G_FILE_ATTRIBUTE_STANDARD_SIZE ","
            G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
            G_FILE_QUERY_INFO_NONE,
            NULL,
            NULL
        );

        if (enumerator) {
            while (TRUE) {
                GFileInfo *info = NULL;
                GFile *child = NULL;
                if (!g_file_enumerator_iterate(enumerator, &info, &child, NULL, NULL) || !info)
                    break;
                if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR)
                    continue;

                const char *name = g_file_info_get_name(info);
                gboolean is_target = g_file_equal(child, target_file);
                if (!is_target && (g_file_info_get_is_hidden(info) || (name && name[0] == '.')))
                    continue;

                const char *ctype = g_file_info_get_content_type(info);
                if (!ctype)
                    ctype = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);

                if (is_target || kajo_player_is_supported_media(ctype, name)) {
                    KajoMediaTrack *tr = kajo_media_track_new(
                        child,
                        g_file_info_get_display_name(info),
                        ctype,
                        (guint64)g_file_info_get_size(info)
                    );
                    if (tr)
                        g_ptr_array_add(self->tracks, tr);
                }
            }
        }
    }

    if (self->tracks->len == 0) {
        KajoMediaTrack *single = query_track_from_gfile(target_file);
        if (single)
            g_ptr_array_add(self->tracks, single);
    } else {
        g_ptr_array_sort(self->tracks, compare_tracks_by_name);
    }

    self->current_index = 0;
    for (guint i = 0; i < self->tracks->len; i++) {
        KajoMediaTrack *t = g_ptr_array_index(self->tracks, i);
        if (g_file_equal(t->file, target_file)) {
            self->current_index = (int)i;
            break;
        }
    }

    rebuild_queue_listbox(self);

    const KajoMediaTrack *cur = kajo_player_queue_get_current(self);
    if (cur && self->select_cb)
        self->select_cb(cur, self->current_index, (int)self->tracks->len, self->user_data);
}

void
kajo_player_queue_set_files(KajoPlayerQueue *self, GFile **files, int n_files)
{
    g_return_if_fail(self != NULL);
    if (!files || n_files <= 0)
        return;

    if (n_files == 1) {
        kajo_player_queue_load_file_and_siblings(self, files[0]);
        return;
    }

    g_ptr_array_set_size(self->tracks, 0);
    for (int i = 0; i < n_files; i++) {
        if (!G_IS_FILE(files[i])) continue;
        KajoMediaTrack *t = query_track_from_gfile(files[i]);
        if (t)
            g_ptr_array_add(self->tracks, t);
    }

    self->current_index = (self->tracks->len > 0) ? 0 : -1;
    rebuild_queue_listbox(self);

    const KajoMediaTrack *cur = kajo_player_queue_get_current(self);
    if (cur && self->select_cb)
        self->select_cb(cur, self->current_index, (int)self->tracks->len, self->user_data);
}

const KajoMediaTrack *
kajo_player_queue_get_current(const KajoPlayerQueue *self)
{
    if (!self || !self->tracks || self->current_index < 0 || self->current_index >= (int)self->tracks->len)
        return NULL;
    return g_ptr_array_index(self->tracks, (guint)self->current_index);
}

int
kajo_player_queue_get_current_index(const KajoPlayerQueue *self)
{
    return self ? self->current_index : -1;
}

guint
kajo_player_queue_get_length(const KajoPlayerQueue *self)
{
    return (self && self->tracks) ? self->tracks->len : 0;
}

gboolean
kajo_player_queue_select_index(KajoPlayerQueue *self, int index)
{
    if (!self || !self->tracks || index < 0 || index >= (int)self->tracks->len)
        return FALSE;

    self->current_index = index;
    if (self->list_box) {
        self->suppress_row_signal = TRUE;
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(self->list_box, index);
        if (row)
            gtk_list_box_select_row(self->list_box, row);
        self->suppress_row_signal = FALSE;
    }

    const KajoMediaTrack *t = g_ptr_array_index(self->tracks, (guint)index);
    if (self->select_cb)
        self->select_cb(t, index, (int)self->tracks->len, self->user_data);
    return TRUE;
}

gboolean
kajo_player_queue_next(KajoPlayerQueue *self)
{
    if (!self || !self->tracks || self->tracks->len == 0)
        return FALSE;
    int next_idx = (self->current_index + 1) % (int)self->tracks->len;
    return kajo_player_queue_select_index(self, next_idx);
}

gboolean
kajo_player_queue_prev(KajoPlayerQueue *self)
{
    if (!self || !self->tracks || self->tracks->len == 0)
        return FALSE;
    int prev_idx = (self->current_index - 1 + (int)self->tracks->len) % (int)self->tracks->len;
    return kajo_player_queue_select_index(self, prev_idx);
}

KajoPlayerQueue *
kajo_player_queue_new(GtkListBox *list_box,
                      GtkWidget *count_badge_lbl,
                      KajoQueueTrackSelectedFunc select_cb,
                      gpointer user_data)
{
    KajoPlayerQueue *self = g_new0(KajoPlayerQueue, 1);
    self->list_box = list_box;
    self->count_badge_lbl = count_badge_lbl;
    self->tracks = g_ptr_array_new_with_free_func((GDestroyNotify)kajo_media_track_free);
    self->current_index = -1;
    self->select_cb = select_cb;
    self->user_data = user_data;

    if (self->list_box) {
        g_signal_connect(self->list_box, "row-selected", G_CALLBACK(on_queue_row_selected), self);
    }

    return self;
}

void
kajo_player_queue_free(KajoPlayerQueue *self)
{
    if (!self) return;
    if (self->tracks)
        g_ptr_array_unref(self->tracks);
    g_free(self);
}
