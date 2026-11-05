#include "player_mpv.h"
#include <locale.h>
#include <math.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

enum {
    PROP_TIME_POS = 1,
    PROP_DURATION,
    PROP_PAUSE,
    PROP_EOF_REACHED,
    PROP_VIDEO_W,
    PROP_VIDEO_H,
    PROP_FPS,
    PROP_DROPPED_FRAMES,
    PROP_VIDEO_CODEC,
    PROP_PIXFMT,
    PROP_COLORMATRIX,
    PROP_HWDEC_CURRENT,
    PROP_AUDIO_CODEC,
    PROP_AUDIO_CHANNELS,
    PROP_AUDIO_SAMPLERATE,
    PROP_SPEED,
    PROP_SUB_DELAY,
    PROP_SUB_SCALE,
    PROP_SUB_VISIBILITY,
    PROP_AUDIO_DELAY,
    PROP_CHAPTER,
    PROP_TRACK_LIST,
    PROP_CHAPTER_LIST,
};

struct _KajoPlayerMpv {
    GtkGLArea *gl_area;
    mpv_handle *mpv;
    mpv_render_context *mpv_gl;

    KajoMpvStateChangedFunc state_cb;
    gpointer user_data;

    KajoMpvState state;
    char *video_codec_buf;
    char *pixfmt_buf;
    char *colormatrix_buf;
    char *hwdec_buf;
    char *audio_codec_buf;
    char *error_buf;

    GPtrArray *tracks;
    GPtrArray *chapters;

    gboolean paused;
    gboolean loop_enabled;

    gint wakeup_scheduled;
    gint render_scheduled;
    gboolean destroyed;
};

static void
kajo_mpv_track_free(gpointer data)
{
    KajoMpvTrack *t = data;
    if (!t)
        return;
    g_free(t->title);
    g_free(t->lang);
    g_free(t->codec);
    g_free(t);
}

static void
kajo_mpv_chapter_free(gpointer data)
{
    KajoMpvChapter *c = data;
    if (!c)
        return;
    g_free(c->title);
    g_free(c);
}

char *
kajo_mpv_track_format_label(const KajoMpvTrack *track)
{
    if (!track)
        return g_strdup("Unknown Track");

    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "#%d  ", track->id);

    if (track->title && track->title[0] != '\0') {
        g_string_append(s, track->title);
        if (track->lang && track->lang[0] != '\0') {
            g_autofree char *lang_up = g_ascii_strup(track->lang, -1);
            g_string_append_printf(s, " (%s)", lang_up);
        }
    } else if (track->lang && track->lang[0] != '\0') {
        g_autofree char *lang_up = g_ascii_strup(track->lang, -1);
        g_string_append_printf(s, "%s", lang_up);
    } else {
        g_string_append(s, track->type == KAJO_MPV_TRACK_SUB ? "Subtitle" : "Audio");
    }

    if (track->codec && track->codec[0] != '\0') {
        g_autofree char *codec_up = g_ascii_strup(track->codec, -1);
        if (track->type == KAJO_MPV_TRACK_AUDIO && track->channels > 0) {
            g_string_append_printf(s, "  [%s %dch]", codec_up, track->channels);
        } else {
            g_string_append_printf(s, "  [%s]", codec_up);
        }
    }

    if (track->external)
        g_string_append(s, " • EXT");
    else if (track->is_default)
        g_string_append(s, " • DEF");

    return g_string_free(s, FALSE);
}

static void *
get_proc_address_mpv(void *ctx G_GNUC_UNUSED, const char *name)
{
    return (void *)eglGetProcAddress(name);
}

static void
notify_state(KajoPlayerMpv *self)
{
    if (self->destroyed || !self->state_cb)
        return;

    self->state.video_codec = self->video_codec_buf;
    self->state.pixel_format = self->pixfmt_buf;
    self->state.colormatrix = self->colormatrix_buf;
    self->state.hwdec_current = self->hwdec_buf;
    self->state.audio_codec = self->audio_codec_buf;
    self->state.error_message = self->error_buf;
    self->state.tracks = self->tracks;
    self->state.chapters = self->chapters;
    self->state.playing = self->state.has_media && !self->paused && !self->state.eof_reached;

    self->state_cb(&self->state, self->user_data);
}

/* --- Render Thread -> GTK Main Thread Bridge --- */

static gboolean
on_main_thread_render_update(gpointer user_data)
{
    KajoPlayerMpv *self = user_data;
    g_atomic_int_set(&self->render_scheduled, 0);

    if (self->destroyed || !self->mpv_gl || !self->gl_area)
        return G_SOURCE_REMOVE;

    uint64_t flags = mpv_render_context_update(self->mpv_gl);
    if (flags & MPV_RENDER_UPDATE_FRAME) {
        gtk_gl_area_queue_render(self->gl_area);
    }
    return G_SOURCE_REMOVE;
}

static void
on_mpv_render_update(void *ctx)
{
    KajoPlayerMpv *self = ctx;
    if (g_atomic_int_compare_and_exchange(&self->render_scheduled, 0, 1)) {
        g_main_context_invoke_full(
            NULL,
            G_PRIORITY_HIGH_IDLE,
            on_main_thread_render_update,
            self,
            NULL
        );
    }
}

/* --- GtkGLArea Signal Callbacks --- */

static void
on_gl_area_realize(GtkWidget *widget, gpointer user_data)
{
    KajoPlayerMpv *self = user_data;
    GtkGLArea *area = GTK_GL_AREA(widget);

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != NULL)
        return;

    if (!self->mpv || self->mpv_gl)
        return;

    mpv_opengl_init_params gl_init = {
        .get_proc_address = get_proc_address_mpv,
        .get_proc_address_ctx = NULL,
    };

    int adv = 1;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
        { MPV_RENDER_PARAM_ADVANCED_CONTROL, &adv },
        { MPV_RENDER_PARAM_INVALID, NULL },
    };

    if (mpv_render_context_create(&self->mpv_gl, self->mpv, params) >= 0) {
        mpv_render_context_set_update_callback(self->mpv_gl, on_mpv_render_update, self);
    }
}

static void
on_gl_area_unrealize(GtkWidget *widget, gpointer user_data)
{
    KajoPlayerMpv *self = user_data;
    GtkGLArea *area = GTK_GL_AREA(widget);

    gtk_gl_area_make_current(area);
    if (self->mpv_gl) {
        mpv_render_context_set_update_callback(self->mpv_gl, NULL, NULL);
        mpv_render_context_free(self->mpv_gl);
        self->mpv_gl = NULL;
    }
}

static gboolean
on_gl_area_render(GtkGLArea *area, GdkGLContext *context G_GNUC_UNUSED, gpointer user_data)
{
    KajoPlayerMpv *self = user_data;

    if (!self->mpv_gl) {
        glClearColor(0.047f, 0.047f, 0.047f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return TRUE;
    }

    int fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);

    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    if (scale < 1)
        scale = 1;

    int w = gtk_widget_get_width(GTK_WIDGET(area)) * scale;
    int h = gtk_widget_get_height(GTK_WIDGET(area)) * scale;

    mpv_opengl_fbo mpfbo = {
        .fbo = fbo,
        .w = w,
        .h = h,
        .internal_format = 0,
    };
    int flip_y = 1;

    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo },
        { MPV_RENDER_PARAM_FLIP_Y, &flip_y },
        { MPV_RENDER_PARAM_INVALID, NULL },
    };

    mpv_render_context_render(self->mpv_gl, params);
    mpv_render_context_report_swap(self->mpv_gl);
    return TRUE;
}

/* --- Track List & Chapter List Node Parsers --- */

static void
parse_track_list_node(KajoPlayerMpv *self, const mpv_node *node)
{
    g_ptr_array_set_size(self->tracks, 0);
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list)
        return;

    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; i++) {
        mpv_node *entry = &list->values[i];
        if (entry->format != MPV_FORMAT_NODE_MAP || !entry->u.list)
            continue;

        mpv_node_list *map = entry->u.list;
        const char *type_str = NULL;
        const char *title_str = NULL;
        const char *lang_str = NULL;
        const char *codec_str = NULL;
        int id = 0;
        gboolean selected = FALSE;
        gboolean is_default = FALSE;
        gboolean external = FALSE;
        int channels = 0;
        int samplerate = 0;

        for (int k = 0; k < map->num; k++) {
            const char *key = map->keys[k];
            mpv_node *val = &map->values[k];
            if (!key || !val)
                continue;

            if (g_strcmp0(key, "id") == 0 && val->format == MPV_FORMAT_INT64)
                id = (int)val->u.int64;
            else if (g_strcmp0(key, "type") == 0 && val->format == MPV_FORMAT_STRING)
                type_str = val->u.string;
            else if (g_strcmp0(key, "title") == 0 && val->format == MPV_FORMAT_STRING)
                title_str = val->u.string;
            else if (g_strcmp0(key, "lang") == 0 && val->format == MPV_FORMAT_STRING)
                lang_str = val->u.string;
            else if (g_strcmp0(key, "codec") == 0 && val->format == MPV_FORMAT_STRING)
                codec_str = val->u.string;
            else if (g_strcmp0(key, "selected") == 0 && val->format == MPV_FORMAT_FLAG)
                selected = val->u.flag != 0;
            else if (g_strcmp0(key, "default") == 0 && val->format == MPV_FORMAT_FLAG)
                is_default = val->u.flag != 0;
            else if (g_strcmp0(key, "external") == 0 && val->format == MPV_FORMAT_FLAG)
                external = val->u.flag != 0;
            else if (g_strcmp0(key, "demux-channel-count") == 0 && val->format == MPV_FORMAT_INT64)
                channels = (int)val->u.int64;
            else if (g_strcmp0(key, "demux-samplerate") == 0 && val->format == MPV_FORMAT_INT64)
                samplerate = (int)val->u.int64;
        }

        if (!type_str || id <= 0)
            continue;

        KajoMpvTrackType ttype;
        if (g_strcmp0(type_str, "sub") == 0)
            ttype = KAJO_MPV_TRACK_SUB;
        else if (g_strcmp0(type_str, "audio") == 0)
            ttype = KAJO_MPV_TRACK_AUDIO;
        else
            continue;

        KajoMpvTrack *t = g_new0(KajoMpvTrack, 1);
        t->id = id;
        t->type = ttype;
        t->selected = selected;
        t->is_default = is_default;
        t->external = external;
        t->title = g_strdup(title_str);
        t->lang = g_strdup(lang_str);
        t->codec = g_strdup(codec_str);
        t->channels = channels;
        t->samplerate = samplerate;

        g_ptr_array_add(self->tracks, t);
    }
}

static void
parse_chapter_list_node(KajoPlayerMpv *self, const mpv_node *node)
{
    g_ptr_array_set_size(self->chapters, 0);
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list)
        return;

    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; i++) {
        mpv_node *entry = &list->values[i];
        if (entry->format != MPV_FORMAT_NODE_MAP || !entry->u.list)
            continue;

        mpv_node_list *map = entry->u.list;
        const char *title_str = NULL;
        double time_sec = 0.0;

        for (int k = 0; k < map->num; k++) {
            const char *key = map->keys[k];
            mpv_node *val = &map->values[k];
            if (!key || !val)
                continue;

            if (g_strcmp0(key, "title") == 0 && val->format == MPV_FORMAT_STRING)
                title_str = val->u.string;
            else if (g_strcmp0(key, "time") == 0 && val->format == MPV_FORMAT_DOUBLE)
                time_sec = val->u.double_;
        }

        KajoMpvChapter *ch = g_new0(KajoMpvChapter, 1);
        ch->index = i;
        ch->title = (title_str && title_str[0] != '\0')
            ? g_strdup(title_str)
            : g_strdup_printf("Chapter %02d", i + 1);
        ch->time_usec = (gint64)(MAX(0.0, time_sec) * 1000000.0);

        g_ptr_array_add(self->chapters, ch);
    }
}

/* --- MPV Event Loop -> GTK Main Thread Bridge --- */

static void
handle_property_change(KajoPlayerMpv *self, mpv_event_property *prop, uint64_t reply_userdata)
{
    switch (reply_userdata) {
    case PROP_TIME_POS:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
            double sec = *(double *)prop->data;
            self->state.position_usec = (gint64)(MAX(0.0, sec) * 1000000.0);
        }
        break;

    case PROP_DURATION:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
            double sec = *(double *)prop->data;
            self->state.duration_usec = (gint64)(MAX(0.0, sec) * 1000000.0);
        }
        break;

    case PROP_PAUSE:
        if (prop->format == MPV_FORMAT_FLAG && prop->data) {
            self->paused = (*(int *)prop->data) != 0;
        }
        break;

    case PROP_EOF_REACHED:
        if (prop->format == MPV_FORMAT_FLAG && prop->data) {
            self->state.eof_reached = (*(int *)prop->data) != 0;
        }
        break;

    case PROP_VIDEO_W:
        if (prop->format == MPV_FORMAT_INT64 && prop->data)
            self->state.video_width = (int)(*(int64_t *)prop->data);
        else if (prop->format == MPV_FORMAT_NONE)
            self->state.video_width = 0;
        break;

    case PROP_VIDEO_H:
        if (prop->format == MPV_FORMAT_INT64 && prop->data)
            self->state.video_height = (int)(*(int64_t *)prop->data);
        else if (prop->format == MPV_FORMAT_NONE)
            self->state.video_height = 0;
        break;

    case PROP_FPS:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            self->state.fps = *(double *)prop->data;
        else if (prop->format == MPV_FORMAT_NONE)
            self->state.fps = 0.0;
        break;

    case PROP_DROPPED_FRAMES:
        if (prop->format == MPV_FORMAT_INT64 && prop->data)
            self->state.dropped_frames = *(int64_t *)prop->data;
        break;

    case PROP_VIDEO_CODEC:
        g_clear_pointer(&self->video_codec_buf, g_free);
        if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
            self->video_codec_buf = g_strdup(*(char **)prop->data);
        break;

    case PROP_PIXFMT:
        g_clear_pointer(&self->pixfmt_buf, g_free);
        if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
            self->pixfmt_buf = g_strdup(*(char **)prop->data);
        break;

    case PROP_COLORMATRIX:
        g_clear_pointer(&self->colormatrix_buf, g_free);
        if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
            self->colormatrix_buf = g_strdup(*(char **)prop->data);
        break;

    case PROP_HWDEC_CURRENT:
        g_clear_pointer(&self->hwdec_buf, g_free);
        if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data) {
            const char *val = *(char **)prop->data;
            if (val[0] != '\0' && g_strcmp0(val, "no") != 0)
                self->hwdec_buf = g_strdup(val);
        }
        break;

    case PROP_AUDIO_CODEC:
        g_clear_pointer(&self->audio_codec_buf, g_free);
        if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
            self->audio_codec_buf = g_strdup(*(char **)prop->data);
        break;

    case PROP_AUDIO_CHANNELS:
        if (prop->format == MPV_FORMAT_INT64 && prop->data)
            self->state.audio_channels = (int)(*(int64_t *)prop->data);
        else if (prop->format == MPV_FORMAT_NONE)
            self->state.audio_channels = 0;
        break;

    case PROP_AUDIO_SAMPLERATE:
        if (prop->format == MPV_FORMAT_INT64 && prop->data)
            self->state.audio_samplerate = (int)(*(int64_t *)prop->data);
        else if (prop->format == MPV_FORMAT_NONE)
            self->state.audio_samplerate = 0;
        break;

    case PROP_SPEED:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            self->state.speed = *(double *)prop->data;
        break;

    case PROP_SUB_DELAY:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            self->state.sub_delay_sec = *(double *)prop->data;
        break;

    case PROP_SUB_SCALE:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            self->state.sub_scale = *(double *)prop->data;
        break;

    case PROP_SUB_VISIBILITY:
        if (prop->format == MPV_FORMAT_FLAG && prop->data)
            self->state.sub_visible = (*(int *)prop->data) != 0;
        break;

    case PROP_AUDIO_DELAY:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            self->state.audio_delay_sec = *(double *)prop->data;
        break;

    case PROP_CHAPTER:
        if (prop->format == MPV_FORMAT_INT64 && prop->data)
            self->state.current_chapter = (int)(*(int64_t *)prop->data);
        else if (prop->format == MPV_FORMAT_NONE)
            self->state.current_chapter = -1;
        break;

    case PROP_TRACK_LIST:
        if (prop->format == MPV_FORMAT_NODE && prop->data)
            parse_track_list_node(self, prop->data);
        break;

    case PROP_CHAPTER_LIST:
        if (prop->format == MPV_FORMAT_NODE && prop->data)
            parse_chapter_list_node(self, prop->data);
        break;

    default:
        break;
    }
}

static gboolean
on_main_thread_mpv_events(gpointer user_data)
{
    KajoPlayerMpv *self = user_data;
    g_atomic_int_set(&self->wakeup_scheduled, 0);

    if (self->destroyed || !self->mpv)
        return G_SOURCE_REMOVE;

    gboolean changed = FALSE;

    while (!self->destroyed && self->mpv) {
        mpv_event *event = mpv_wait_event(self->mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;

        switch (event->event_id) {
        case MPV_EVENT_START_FILE:
            g_clear_pointer(&self->error_buf, g_free);
            self->state.has_media = TRUE;
            self->state.eof_reached = FALSE;
            self->state.position_usec = 0;
            changed = TRUE;
            break;

        case MPV_EVENT_FILE_LOADED:
            self->state.has_media = TRUE;
            self->state.eof_reached = FALSE;
            changed = TRUE;
            break;

        case MPV_EVENT_END_FILE: {
            mpv_event_end_file *ef = event->data;
            if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) {
                g_clear_pointer(&self->error_buf, g_free);
                self->error_buf = g_strdup(mpv_error_string(ef->error));
            } else if (ef && ef->reason == MPV_END_FILE_REASON_EOF) {
                self->state.eof_reached = TRUE;
            }
            changed = TRUE;
            break;
        }

        case MPV_EVENT_PROPERTY_CHANGE:
            if (event->data) {
                handle_property_change(self, event->data, event->reply_userdata);
                changed = TRUE;
            }
            break;

        default:
            break;
        }
    }

    if (changed) {
        notify_state(self);
    }

    return G_SOURCE_REMOVE;
}

static void
on_mpv_wakeup(void *ctx)
{
    KajoPlayerMpv *self = ctx;
    if (g_atomic_int_compare_and_exchange(&self->wakeup_scheduled, 0, 1)) {
        g_main_context_invoke_full(
            NULL,
            G_PRIORITY_DEFAULT_IDLE,
            on_main_thread_mpv_events,
            self,
            NULL
        );
    }
}

/* --- Public API --- */

KajoPlayerMpv *
kajo_player_mpv_new(GtkGLArea *gl_area,
                    KajoMpvStateChangedFunc state_cb,
                    gpointer user_data)
{
    g_return_val_if_fail(GTK_IS_GL_AREA(gl_area), NULL);

    /* libmpv requires C numeric locale for floating-point parsing */
    setlocale(LC_NUMERIC, "C");

    KajoPlayerMpv *self = g_new0(KajoPlayerMpv, 1);
    self->gl_area = gl_area;
    self->state_cb = state_cb;
    self->user_data = user_data;
    self->tracks = g_ptr_array_new_with_free_func(kajo_mpv_track_free);
    self->chapters = g_ptr_array_new_with_free_func(kajo_mpv_chapter_free);
    self->state.speed = 1.0;
    self->state.sub_scale = 1.0;
    self->state.sub_visible = TRUE;
    self->state.current_chapter = -1;
    self->state.tracks = self->tracks;
    self->state.chapters = self->chapters;

    gtk_gl_area_set_auto_render(gl_area, FALSE);

    self->mpv = mpv_create();
    if (self->mpv) {
        mpv_set_option_string(self->mpv, "vo", "libmpv");
        mpv_set_option_string(self->mpv, "hwdec", "auto-safe");
        mpv_set_option_string(self->mpv, "keep-open", "yes");
        mpv_set_option_string(self->mpv, "sub-auto", "fuzzy");
        mpv_set_option_string(self->mpv, "audio-pitch-correction", "yes");
        mpv_set_option_string(self->mpv, "audio-client-name", "Kajo Player");
        mpv_set_option_string(self->mpv, "title", "Kajo Player");

        if (mpv_initialize(self->mpv) >= 0) {
            mpv_observe_property(self->mpv, PROP_TIME_POS, "time-pos", MPV_FORMAT_DOUBLE);
            mpv_observe_property(self->mpv, PROP_DURATION, "duration", MPV_FORMAT_DOUBLE);
            mpv_observe_property(self->mpv, PROP_PAUSE, "pause", MPV_FORMAT_FLAG);
            mpv_observe_property(self->mpv, PROP_EOF_REACHED, "eof-reached", MPV_FORMAT_FLAG);
            mpv_observe_property(self->mpv, PROP_VIDEO_W, "video-params/w", MPV_FORMAT_INT64);
            mpv_observe_property(self->mpv, PROP_VIDEO_H, "video-params/h", MPV_FORMAT_INT64);
            mpv_observe_property(self->mpv, PROP_FPS, "container-fps", MPV_FORMAT_DOUBLE);
            mpv_observe_property(self->mpv, PROP_DROPPED_FRAMES, "frame-drop-count", MPV_FORMAT_INT64);
            mpv_observe_property(self->mpv, PROP_VIDEO_CODEC, "video-format", MPV_FORMAT_STRING);
            mpv_observe_property(self->mpv, PROP_PIXFMT, "video-params/pixelformat", MPV_FORMAT_STRING);
            mpv_observe_property(self->mpv, PROP_COLORMATRIX, "video-params/colormatrix", MPV_FORMAT_STRING);
            mpv_observe_property(self->mpv, PROP_HWDEC_CURRENT, "hwdec-current", MPV_FORMAT_STRING);
            mpv_observe_property(self->mpv, PROP_AUDIO_CODEC, "audio-codec-name", MPV_FORMAT_STRING);
            mpv_observe_property(self->mpv, PROP_AUDIO_CHANNELS, "audio-params/channel-count", MPV_FORMAT_INT64);
            mpv_observe_property(self->mpv, PROP_AUDIO_SAMPLERATE, "audio-params/samplerate", MPV_FORMAT_INT64);
            mpv_observe_property(self->mpv, PROP_SPEED, "speed", MPV_FORMAT_DOUBLE);
            mpv_observe_property(self->mpv, PROP_SUB_DELAY, "sub-delay", MPV_FORMAT_DOUBLE);
            mpv_observe_property(self->mpv, PROP_SUB_SCALE, "sub-scale", MPV_FORMAT_DOUBLE);
            mpv_observe_property(self->mpv, PROP_SUB_VISIBILITY, "sub-visibility", MPV_FORMAT_FLAG);
            mpv_observe_property(self->mpv, PROP_AUDIO_DELAY, "audio-delay", MPV_FORMAT_DOUBLE);
            mpv_observe_property(self->mpv, PROP_CHAPTER, "chapter", MPV_FORMAT_INT64);
            mpv_observe_property(self->mpv, PROP_TRACK_LIST, "track-list", MPV_FORMAT_NODE);
            mpv_observe_property(self->mpv, PROP_CHAPTER_LIST, "chapter-list", MPV_FORMAT_NODE);

            mpv_set_wakeup_callback(self->mpv, on_mpv_wakeup, self);
        }
    }

    g_signal_connect(gl_area, "realize", G_CALLBACK(on_gl_area_realize), self);
    g_signal_connect(gl_area, "unrealize", G_CALLBACK(on_gl_area_unrealize), self);
    g_signal_connect(gl_area, "render", G_CALLBACK(on_gl_area_render), self);

    if (gtk_widget_get_realized(GTK_WIDGET(gl_area))) {
        on_gl_area_realize(GTK_WIDGET(gl_area), self);
    }

    return self;
}

void
kajo_player_mpv_free(KajoPlayerMpv *self)
{
    if (!self)
        return;

    self->destroyed = TRUE;

    if (self->gl_area) {
        g_signal_handlers_disconnect_by_data(self->gl_area, self);
    }

    if (self->mpv_gl) {
        if (self->gl_area && gtk_widget_get_realized(GTK_WIDGET(self->gl_area))) {
            gtk_gl_area_make_current(self->gl_area);
        }
        mpv_render_context_set_update_callback(self->mpv_gl, NULL, NULL);
        mpv_render_context_free(self->mpv_gl);
        self->mpv_gl = NULL;
    }

    if (self->mpv) {
        mpv_set_wakeup_callback(self->mpv, NULL, NULL);
        mpv_terminate_destroy(self->mpv);
        self->mpv = NULL;
    }

    while (g_idle_remove_by_data(self)) {
        /* removed */
    }

    g_ptr_array_free(self->tracks, TRUE);
    g_ptr_array_free(self->chapters, TRUE);

    g_free(self->video_codec_buf);
    g_free(self->pixfmt_buf);
    g_free(self->colormatrix_buf);
    g_free(self->hwdec_buf);
    g_free(self->audio_codec_buf);
    g_free(self->error_buf);
    g_free(self);
}

void
kajo_player_mpv_load_file(KajoPlayerMpv *self, GFile *file)
{
    if (!self || !self->mpv || !file)
        return;

    g_autofree char *path = g_file_get_path(file);
    g_autofree char *uri = g_file_get_uri(file);
    const char *target = path ? path : uri;
    if (!target)
        return;

    if (!self->mpv_gl && self->gl_area) {
        if (!gtk_widget_get_realized(GTK_WIDGET(self->gl_area)))
            gtk_widget_realize(GTK_WIDGET(self->gl_area));
        on_gl_area_realize(GTK_WIDGET(self->gl_area), self);
    }

    g_clear_pointer(&self->error_buf, g_free);
    g_clear_pointer(&self->video_codec_buf, g_free);
    g_clear_pointer(&self->pixfmt_buf, g_free);
    g_clear_pointer(&self->colormatrix_buf, g_free);
    g_clear_pointer(&self->hwdec_buf, g_free);
    g_clear_pointer(&self->audio_codec_buf, g_free);
    g_ptr_array_set_size(self->tracks, 0);
    g_ptr_array_set_size(self->chapters, 0);

    self->state.has_media = TRUE;
    self->state.eof_reached = FALSE;
    self->state.position_usec = 0;
    self->state.duration_usec = 0;
    self->state.video_width = 0;
    self->state.video_height = 0;
    self->state.fps = 0.0;
    self->state.dropped_frames = 0;
    self->state.current_chapter = -1;

    int pause_flag = 0;
    mpv_set_property(self->mpv, "pause", MPV_FORMAT_FLAG, &pause_flag);

    const char *cmd[] = { "loadfile", target, "replace", NULL };
    mpv_command_async(self->mpv, 0, cmd);
}

void
kajo_player_mpv_stop(KajoPlayerMpv *self)
{
    if (!self || !self->mpv)
        return;

    const char *cmd[] = { "stop", NULL };
    mpv_command_async(self->mpv, 0, cmd);

    g_ptr_array_set_size(self->tracks, 0);
    g_ptr_array_set_size(self->chapters, 0);

    self->state.has_media = FALSE;
    self->state.playing = FALSE;
    self->state.eof_reached = FALSE;
    self->state.position_usec = 0;
    self->state.duration_usec = 0;
    self->state.video_width = 0;
    self->state.video_height = 0;
    self->state.current_chapter = -1;
}

void
kajo_player_mpv_set_playing(KajoPlayerMpv *self, gboolean playing)
{
    if (!self || !self->mpv || !self->state.has_media)
        return;

    if (playing && self->state.eof_reached) {
        const char *seek_cmd[] = { "seek", "0", "absolute", NULL };
        mpv_command_async(self->mpv, 0, seek_cmd);
    }

    int pause_flag = playing ? 0 : 1;
    mpv_set_property_async(self->mpv, 0, "pause", MPV_FORMAT_FLAG, &pause_flag);
}

gboolean
kajo_player_mpv_get_playing(const KajoPlayerMpv *self)
{
    if (!self || !self->state.has_media)
        return FALSE;
    return !self->paused && !self->state.eof_reached;
}

gboolean
kajo_player_mpv_get_eof_reached(const KajoPlayerMpv *self)
{
    return self ? self->state.eof_reached : FALSE;
}

void
kajo_player_mpv_seek_absolute_usec(KajoPlayerMpv *self, gint64 target_usec)
{
    if (!self || !self->mpv || !self->state.has_media)
        return;

    double sec = (double)MAX(0, target_usec) / 1000000.0;
    char buf[64];
    g_ascii_dtostr(buf, sizeof(buf), sec);

    const char *cmd[] = { "seek", buf, "absolute", NULL };
    mpv_command_async(self->mpv, 0, cmd);
}

void
kajo_player_mpv_seek_relative_sec(KajoPlayerMpv *self, double delta_sec)
{
    if (!self || !self->mpv || !self->state.has_media)
        return;

    char buf[64];
    g_ascii_dtostr(buf, sizeof(buf), delta_sec);

    const char *cmd[] = { "seek", buf, "relative", NULL };
    mpv_command_async(self->mpv, 0, cmd);
}

gint64
kajo_player_mpv_get_position_usec(const KajoPlayerMpv *self)
{
    return self ? self->state.position_usec : 0;
}

gint64
kajo_player_mpv_get_duration_usec(const KajoPlayerMpv *self)
{
    return self ? self->state.duration_usec : 0;
}

void
kajo_player_mpv_frame_step(KajoPlayerMpv *self, gboolean backward)
{
    if (!self || !self->mpv || !self->state.has_media)
        return;

    const char *cmd[] = { backward ? "frame-back-step" : "frame-step", NULL };
    mpv_command_async(self->mpv, 0, cmd);
}

void
kajo_player_mpv_set_chapter(KajoPlayerMpv *self, int chapter_idx)
{
    if (!self || !self->mpv || !self->state.has_media)
        return;

    int64_t ch = chapter_idx;
    mpv_set_property_async(self->mpv, 0, "chapter", MPV_FORMAT_INT64, &ch);
}

void
kajo_player_mpv_cycle_ab_loop(KajoPlayerMpv *self)
{
    if (!self || !self->mpv || !self->state.has_media)
        return;

    const char *cmd[] = { "ab-loop", NULL };
    mpv_command_async(self->mpv, 0, cmd);
}

void
kajo_player_mpv_set_volume(KajoPlayerMpv *self, double volume_0_to_1)
{
    if (!self || !self->mpv)
        return;

    double vol_pct = CLAMP(volume_0_to_1 * 100.0, 0.0, 100.0);
    mpv_set_property_async(self->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &vol_pct);
}

void
kajo_player_mpv_set_muted(KajoPlayerMpv *self, gboolean muted)
{
    if (!self || !self->mpv)
        return;

    int mute_flag = muted ? 1 : 0;
    mpv_set_property_async(self->mpv, 0, "mute", MPV_FORMAT_FLAG, &mute_flag);
}

void
kajo_player_mpv_set_loop(KajoPlayerMpv *self, gboolean loop_file)
{
    if (!self || !self->mpv)
        return;

    self->loop_enabled = loop_file;
    mpv_set_option_string(self->mpv, "loop-file", loop_file ? "inf" : "no");
}

void
kajo_player_mpv_set_speed(KajoPlayerMpv *self, double speed)
{
    if (!self || !self->mpv)
        return;

    double clamped = CLAMP(speed, 0.1, 4.0);
    mpv_set_property_async(self->mpv, 0, "speed", MPV_FORMAT_DOUBLE, &clamped);
}

void
kajo_player_mpv_adjust_speed(KajoPlayerMpv *self, double delta)
{
    if (!self)
        return;
    kajo_player_mpv_set_speed(self, self->state.speed + delta);
}

void
kajo_player_mpv_set_sid(KajoPlayerMpv *self, int sid)
{
    if (!self || !self->mpv)
        return;

    if (sid <= 0) {
        mpv_set_property_string(self->mpv, "sid", "no");
    } else {
        g_autofree char *s = g_strdup_printf("%d", sid);
        int vis = 1;
        mpv_set_property_async(self->mpv, 0, "sub-visibility", MPV_FORMAT_FLAG, &vis);
        mpv_set_property_string(self->mpv, "sid", s);
    }
}

void
kajo_player_mpv_cycle_sub(KajoPlayerMpv *self)
{
    if (!self || !self->mpv)
        return;

    const char *cmd[] = { "cycle", "sub", NULL };
    mpv_command_async(self->mpv, 0, cmd);
}

void
kajo_player_mpv_toggle_sub_visibility(KajoPlayerMpv *self)
{
    if (!self || !self->mpv)
        return;

    int vis = self->state.sub_visible ? 0 : 1;
    mpv_set_property_async(self->mpv, 0, "sub-visibility", MPV_FORMAT_FLAG, &vis);
}

void
kajo_player_mpv_add_subtitle_file(KajoPlayerMpv *self, const char *path)
{
    if (!self || !self->mpv || !path || path[0] == '\0')
        return;

    int vis = 1;
    mpv_set_property_async(self->mpv, 0, "sub-visibility", MPV_FORMAT_FLAG, &vis);

    const char *cmd[] = { "sub-add", path, "select", NULL };
    mpv_command_async(self->mpv, 0, cmd);
}

void
kajo_player_mpv_set_sub_delay(KajoPlayerMpv *self, double delay_sec)
{
    if (!self || !self->mpv)
        return;

    double d = CLAMP(delay_sec, -60.0, 60.0);
    mpv_set_property_async(self->mpv, 0, "sub-delay", MPV_FORMAT_DOUBLE, &d);
}

void
kajo_player_mpv_adjust_sub_delay(KajoPlayerMpv *self, double delta_sec)
{
    if (!self)
        return;
    kajo_player_mpv_set_sub_delay(self, self->state.sub_delay_sec + delta_sec);
}

void
kajo_player_mpv_adjust_sub_scale(KajoPlayerMpv *self, double delta)
{
    if (!self || !self->mpv)
        return;

    double sc = CLAMP(self->state.sub_scale + delta, 0.3, 3.0);
    mpv_set_property_async(self->mpv, 0, "sub-scale", MPV_FORMAT_DOUBLE, &sc);
}

void
kajo_player_mpv_set_aid(KajoPlayerMpv *self, int aid)
{
    if (!self || !self->mpv)
        return;

    if (aid <= 0) {
        mpv_set_property_string(self->mpv, "aid", "no");
    } else {
        g_autofree char *s = g_strdup_printf("%d", aid);
        mpv_set_property_string(self->mpv, "aid", s);
    }
}

void
kajo_player_mpv_cycle_audio(KajoPlayerMpv *self)
{
    if (!self || !self->mpv)
        return;

    const char *cmd[] = { "cycle", "audio", NULL };
    mpv_command_async(self->mpv, 0, cmd);
}

void
kajo_player_mpv_set_audio_delay(KajoPlayerMpv *self, double delay_sec)
{
    if (!self || !self->mpv)
        return;

    double d = CLAMP(delay_sec, -60.0, 60.0);
    mpv_set_property_async(self->mpv, 0, "audio-delay", MPV_FORMAT_DOUBLE, &d);
}

void
kajo_player_mpv_adjust_audio_delay(KajoPlayerMpv *self, double delta_sec)
{
    if (!self)
        return;
    kajo_player_mpv_set_audio_delay(self, self->state.audio_delay_sec + delta_sec);
}

gboolean
kajo_player_mpv_take_screenshot(KajoPlayerMpv *self, char **out_path)
{
    if (!self || !self->mpv || !self->state.has_media)
        return FALSE;

    const char *pics_dir = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (!pics_dir)
        pics_dir = g_get_home_dir();

    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *stamp = g_date_time_format(now, "%Y%m%d_%H%M%S");
    g_autofree char *filename = g_strdup_printf("kajo_player_%s.png", stamp);
    char *full_path = g_build_filename(pics_dir, filename, NULL);

    const char *cmd[] = { "screenshot-to-file", full_path, "subtitles", NULL };
    int rc = mpv_command_async(self->mpv, 0, cmd);
    if (rc < 0) {
        g_free(full_path);
        return FALSE;
    }

    if (out_path)
        *out_path = full_path;
    else
        g_free(full_path);

    return TRUE;
}

const KajoMpvState *
kajo_player_mpv_get_state(const KajoPlayerMpv *self)
{
    return self ? &self->state : NULL;
}
