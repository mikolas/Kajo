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
    PROP_VIDEO_CODEC,
    PROP_AUDIO_CODEC,
    PROP_HWDEC_CURRENT,
};

struct _KajoPlayerMpv {
    GtkGLArea *gl_area;
    mpv_handle *mpv;
    mpv_render_context *mpv_gl;

    KajoMpvStateChangedFunc state_cb;
    gpointer user_data;

    KajoMpvState state;
    char *video_codec_buf;
    char *audio_codec_buf;
    char *hwdec_buf;
    char *error_buf;

    gboolean paused;
    gboolean loop_enabled;

    gint wakeup_scheduled;
    gint render_scheduled;
    gboolean destroyed;
};

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
    self->state.audio_codec = self->audio_codec_buf;
    self->state.hwdec_current = self->hwdec_buf;
    self->state.error_message = self->error_buf;
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

/* --- MPV Event Loop -> GTK Main Thread Bridge --- */

static void
handle_property_change(KajoPlayerMpv *self, mpv_event_property *prop, uint64_t reply_userdata)
{
    switch (reply_userdata) {
    case PROP_TIME_POS:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
            double sec = *(double *)prop->data;
            if (sec < 0.0)
                sec = 0.0;
            self->state.position_usec = (gint64)(sec * 1000000.0);
        }
        break;

    case PROP_DURATION:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data) {
            double sec = *(double *)prop->data;
            if (sec < 0.0)
                sec = 0.0;
            self->state.duration_usec = (gint64)(sec * 1000000.0);
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
        if (prop->format == MPV_FORMAT_INT64 && prop->data) {
            self->state.video_width = (int)(*(int64_t *)prop->data);
        } else if (prop->format == MPV_FORMAT_NONE) {
            self->state.video_width = 0;
        }
        break;

    case PROP_VIDEO_H:
        if (prop->format == MPV_FORMAT_INT64 && prop->data) {
            self->state.video_height = (int)(*(int64_t *)prop->data);
        } else if (prop->format == MPV_FORMAT_NONE) {
            self->state.video_height = 0;
        }
        break;

    case PROP_VIDEO_CODEC:
        g_clear_pointer(&self->video_codec_buf, g_free);
        if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data) {
            self->video_codec_buf = g_strdup(*(char **)prop->data);
        }
        break;

    case PROP_AUDIO_CODEC:
        g_clear_pointer(&self->audio_codec_buf, g_free);
        if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data) {
            self->audio_codec_buf = g_strdup(*(char **)prop->data);
        }
        break;

    case PROP_HWDEC_CURRENT:
        g_clear_pointer(&self->hwdec_buf, g_free);
        if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data) {
            const char *val = *(char **)prop->data;
            if (val[0] != '\0' && g_strcmp0(val, "no") != 0)
                self->hwdec_buf = g_strdup(val);
        }
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

    gtk_gl_area_set_auto_render(gl_area, FALSE);

    self->mpv = mpv_create();
    if (self->mpv) {
        mpv_set_option_string(self->mpv, "vo", "libmpv");
        mpv_set_option_string(self->mpv, "hwdec", "auto-safe");
        mpv_set_option_string(self->mpv, "keep-open", "yes");
        mpv_set_option_string(self->mpv, "sub-auto", "fuzzy");
        mpv_set_option_string(self->mpv, "audio-client-name", "Kajo Player");
        mpv_set_option_string(self->mpv, "title", "Kajo Player");

        if (mpv_initialize(self->mpv) >= 0) {
            mpv_observe_property(self->mpv, PROP_TIME_POS, "time-pos", MPV_FORMAT_DOUBLE);
            mpv_observe_property(self->mpv, PROP_DURATION, "duration", MPV_FORMAT_DOUBLE);
            mpv_observe_property(self->mpv, PROP_PAUSE, "pause", MPV_FORMAT_FLAG);
            mpv_observe_property(self->mpv, PROP_EOF_REACHED, "eof-reached", MPV_FORMAT_FLAG);
            mpv_observe_property(self->mpv, PROP_VIDEO_W, "video-params/w", MPV_FORMAT_INT64);
            mpv_observe_property(self->mpv, PROP_VIDEO_H, "video-params/h", MPV_FORMAT_INT64);
            mpv_observe_property(self->mpv, PROP_VIDEO_CODEC, "video-format", MPV_FORMAT_STRING);
            mpv_observe_property(self->mpv, PROP_AUDIO_CODEC, "audio-codec-name", MPV_FORMAT_STRING);
            mpv_observe_property(self->mpv, PROP_HWDEC_CURRENT, "hwdec-current", MPV_FORMAT_STRING);

            mpv_set_wakeup_callback(self->mpv, on_mpv_wakeup, self);
        }
    }

    g_signal_connect(gl_area, "realize", G_CALLBACK(on_gl_area_realize), self);
    g_signal_connect(gl_area, "unrealize", G_CALLBACK(on_gl_area_unrealize), self);
    g_signal_connect(gl_area, "render", G_CALLBACK(on_gl_area_render), self);

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

    /* Flush any pending idle callbacks targeting self */
    while (g_idle_remove_by_data(self)) {
        /* removed */
    }

    g_free(self->video_codec_buf);
    g_free(self->audio_codec_buf);
    g_free(self->hwdec_buf);
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

    g_clear_pointer(&self->error_buf, g_free);
    g_clear_pointer(&self->video_codec_buf, g_free);
    g_clear_pointer(&self->audio_codec_buf, g_free);
    g_clear_pointer(&self->hwdec_buf, g_free);

    self->state.has_media = TRUE;
    self->state.eof_reached = FALSE;
    self->state.position_usec = 0;
    self->state.duration_usec = 0;
    self->state.video_width = 0;
    self->state.video_height = 0;

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

    self->state.has_media = FALSE;
    self->state.playing = FALSE;
    self->state.eof_reached = FALSE;
    self->state.position_usec = 0;
    self->state.duration_usec = 0;
    self->state.video_width = 0;
    self->state.video_height = 0;
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

const KajoMpvState *
kajo_player_mpv_get_state(const KajoPlayerMpv *self)
{
    return self ? &self->state : NULL;
}
