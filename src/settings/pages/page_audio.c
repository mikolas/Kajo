#include "../settings_common.h"
#include "../settings_pages.h"
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <string.h>

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageAudio {
    GtkBox parent_instance;

    GtkWidget *combo_sink;
    GtkWidget *scale_vol;
    GtkWidget *lbl_vol_val;
    GtkWidget *combo_source;
    GtkWidget *scale_mic;
    GtkWidget *lbl_mic_val;

    pa_glib_mainloop *pa_ml;
    pa_context       *pa_ctx;
    gchar            *default_sink_name;
    gchar            *default_source_name;
    guint8            sink_channels;
    guint8            source_channels;
    GPtrArray        *sink_names;
    GPtrArray        *source_names;
    GtkStringList    *sink_string_list;
    GtkStringList    *source_string_list;
    guint             sink_query_seq;
    guint             source_query_seq;
    gboolean          updating_sinks;
    gboolean          updating_sources;
    gboolean          updating_ui;
} SettingsPageAudio;

typedef struct {
    SettingsPageAudio *self;
    guint query_seq;
} AudioQueryTag;

typedef struct _SettingsPageAudioClass {
    GtkBoxClass parent_class;
} SettingsPageAudioClass;

G_DEFINE_TYPE(SettingsPageAudio, settings_page_audio, GTK_TYPE_BOX)

static void audio_query_server_info(SettingsPageAudio *self);
static void audio_query_sinks(SettingsPageAudio *self);
static void audio_query_sources(SettingsPageAudio *self);

static guint32
audio_cvolume_to_percent(const pa_cvolume *cv)
{
    if (!cv || cv->channels == 0) return 0;
    pa_volume_t avg = pa_cvolume_avg(cv);
    double pct = ((double)avg / (double)PA_VOLUME_NORM) * 100.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 150.0) pct = 150.0;
    return (guint32)(pct + 0.5);
}

static void
audio_sink_info_cb(pa_context *ctx G_GNUC_UNUSED, const pa_sink_info *info, int eol, void *userdata)
{
    SettingsPageAudio *self = userdata;
    if (eol > 0 || !info || !self) return;

    if (self->default_sink_name && g_strcmp0(info->name, self->default_sink_name) == 0) {
        if (info->volume.channels > 0) {
            self->sink_channels = info->volume.channels;
        }
        guint32 pct = audio_cvolume_to_percent(&info->volume);
        self->updating_ui = TRUE;
        gtk_range_set_value(GTK_RANGE(self->scale_vol), (double)pct);
        gchar *str = g_strdup_printf("%u%%", pct);
        gtk_label_set_text(GTK_LABEL(self->lbl_vol_val), str);
        g_free(str);
        self->updating_ui = FALSE;
    }
}

static void
audio_source_info_cb(pa_context *ctx G_GNUC_UNUSED, const pa_source_info *info, int eol, void *userdata)
{
    SettingsPageAudio *self = userdata;
    if (eol > 0 || !info || !self) return;

    if (self->default_source_name && g_strcmp0(info->name, self->default_source_name) == 0) {
        if (info->volume.channels > 0) {
            self->source_channels = info->volume.channels;
        }
        guint32 pct = audio_cvolume_to_percent(&info->volume);
        self->updating_ui = TRUE;
        gtk_range_set_value(GTK_RANGE(self->scale_mic), (double)pct);
        gchar *str = g_strdup_printf("%u%%", pct);
        gtk_label_set_text(GTK_LABEL(self->lbl_mic_val), str);
        g_free(str);
        self->updating_ui = FALSE;
    }
}

static void
audio_sink_list_cb(pa_context *ctx G_GNUC_UNUSED, const pa_sink_info *info, int eol, void *userdata)
{
    AudioQueryTag *tag = userdata;
    if (!tag || !tag->self) {
        g_free(tag);
        return;
    }
    SettingsPageAudio *self = tag->self;

    if (tag->query_seq != self->sink_query_seq) {
        if (eol != 0) g_free(tag);
        return;
    }

    if (eol != 0) {
        self->updating_sinks = FALSE;
        g_free(tag);
        return;
    }
    if (!info) return;

    for (guint i = 0; i < self->sink_names->len; i++) {
        const char *name = g_ptr_array_index(self->sink_names, i);
        if (g_strcmp0(name, info->name) == 0) return;
    }

    const char *desc = (info->description && info->description[0] != '\0') ? info->description : info->name;
    g_ptr_array_add(self->sink_names, g_strdup(info->name));
    gtk_string_list_append(self->sink_string_list, desc);

    if (self->default_sink_name && g_strcmp0(info->name, self->default_sink_name) == 0) {
        guint idx = self->sink_names->len - 1;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_sink), idx);
    }
}

static void
audio_source_list_cb(pa_context *ctx G_GNUC_UNUSED, const pa_source_info *info, int eol, void *userdata)
{
    AudioQueryTag *tag = userdata;
    if (!tag || !tag->self) {
        g_free(tag);
        return;
    }
    SettingsPageAudio *self = tag->self;

    if (tag->query_seq != self->source_query_seq) {
        if (eol != 0) g_free(tag);
        return;
    }

    if (eol != 0) {
        self->updating_sources = FALSE;
        g_free(tag);
        return;
    }
    if (!info) return;

    /* Filter out monitor sources */
    if (info->monitor_of_sink != PA_INVALID_INDEX) return;

    for (guint i = 0; i < self->source_names->len; i++) {
        const char *name = g_ptr_array_index(self->source_names, i);
        if (g_strcmp0(name, info->name) == 0) return;
    }

    const char *desc = (info->description && info->description[0] != '\0') ? info->description : info->name;
    g_ptr_array_add(self->source_names, g_strdup(info->name));
    gtk_string_list_append(self->source_string_list, desc);

    if (self->default_source_name && g_strcmp0(info->name, self->default_source_name) == 0) {
        guint idx = self->source_names->len - 1;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_source), idx);
    }
}

static void
audio_server_info_cb(pa_context *ctx, const pa_server_info *info, void *userdata)
{
    SettingsPageAudio *self = userdata;
    if (!info || !self) return;

    if (info->default_sink_name) {
        g_free(self->default_sink_name);
        self->default_sink_name = g_strdup(info->default_sink_name);
        pa_context_get_sink_info_by_name(ctx, self->default_sink_name, audio_sink_info_cb, self);
    }
    if (info->default_source_name) {
        g_free(self->default_source_name);
        self->default_source_name = g_strdup(info->default_source_name);
        pa_context_get_source_info_by_name(ctx, self->default_source_name, audio_source_info_cb, self);
    }
}

static void
audio_query_server_info(SettingsPageAudio *self)
{
    if (!self->pa_ctx || pa_context_get_state(self->pa_ctx) != PA_CONTEXT_READY) return;
    pa_context_get_server_info(self->pa_ctx, audio_server_info_cb, self);
}

static void
audio_query_sinks(SettingsPageAudio *self)
{
    if (!self->pa_ctx || pa_context_get_state(self->pa_ctx) != PA_CONTEXT_READY) return;

    self->sink_query_seq++;
    self->updating_sinks = TRUE;

    g_ptr_array_set_size(self->sink_names, 0);

    GtkStringList *new_list = gtk_string_list_new(NULL);
    if (self->sink_string_list) g_object_unref(self->sink_string_list);
    self->sink_string_list = new_list;
    gtk_drop_down_set_model(GTK_DROP_DOWN(self->combo_sink), G_LIST_MODEL(self->sink_string_list));

    AudioQueryTag *tag = g_new0(AudioQueryTag, 1);
    tag->self = self;
    tag->query_seq = self->sink_query_seq;
    pa_context_get_sink_info_list(self->pa_ctx, audio_sink_list_cb, tag);
}

static void
audio_query_sources(SettingsPageAudio *self)
{
    if (!self->pa_ctx || pa_context_get_state(self->pa_ctx) != PA_CONTEXT_READY) return;

    self->source_query_seq++;
    self->updating_sources = TRUE;

    g_ptr_array_set_size(self->source_names, 0);

    GtkStringList *new_list = gtk_string_list_new(NULL);
    if (self->source_string_list) g_object_unref(self->source_string_list);
    self->source_string_list = new_list;
    gtk_drop_down_set_model(GTK_DROP_DOWN(self->combo_source), G_LIST_MODEL(self->source_string_list));

    AudioQueryTag *tag = g_new0(AudioQueryTag, 1);
    tag->self = self;
    tag->query_seq = self->source_query_seq;
    pa_context_get_source_info_list(self->pa_ctx, audio_source_list_cb, tag);
}

static void
audio_subscription_cb(pa_context *ctx G_GNUC_UNUSED, pa_subscription_event_type_t t,
                      uint32_t idx G_GNUC_UNUSED, void *userdata)
{
    SettingsPageAudio *self = userdata;
    pa_subscription_event_type_t facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    pa_subscription_event_type_t ev_type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    if (facility == PA_SUBSCRIPTION_EVENT_SERVER) {
        audio_query_server_info(self);
        audio_query_sinks(self);
        audio_query_sources(self);
    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
        audio_query_server_info(self);
        if (ev_type == PA_SUBSCRIPTION_EVENT_NEW || ev_type == PA_SUBSCRIPTION_EVENT_REMOVE) {
            audio_query_sinks(self);
        }
    } else if (facility == PA_SUBSCRIPTION_EVENT_SOURCE) {
        audio_query_server_info(self);
        if (ev_type == PA_SUBSCRIPTION_EVENT_NEW || ev_type == PA_SUBSCRIPTION_EVENT_REMOVE) {
            audio_query_sources(self);
        }
    }
}

static void
audio_context_state_cb(pa_context *ctx, void *userdata)
{
    SettingsPageAudio *self = userdata;
    pa_context_state_t state = pa_context_get_state(ctx);

    if (state == PA_CONTEXT_READY) {
        pa_context_set_subscribe_callback(ctx, audio_subscription_cb, self);
        pa_context_subscribe(ctx,
            PA_SUBSCRIPTION_MASK_SINK |
            PA_SUBSCRIPTION_MASK_SOURCE |
            PA_SUBSCRIPTION_MASK_SERVER,
            NULL, NULL);

        audio_query_server_info(self);
        audio_query_sinks(self);
        audio_query_sources(self);
    }
}

static void
on_audio_vol_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    gchar *str = g_strdup_printf("%d%%", (int)gtk_range_get_value(range));
    gtk_label_set_text(GTK_LABEL(self->lbl_vol_val), str);
    g_free(str);

    if (self->updating_ui || !self->pa_ctx || !self->default_sink_name) return;
    if (pa_context_get_state(self->pa_ctx) != PA_CONTEXT_READY) return;

    double val = gtk_range_get_value(range);
    pa_volume_t vol = (pa_volume_t)(val * PA_VOLUME_NORM / 100.0);
    pa_cvolume cv;
    pa_cvolume_set(&cv, self->sink_channels ? self->sink_channels : 2, vol);
    pa_context_set_sink_volume_by_name(self->pa_ctx, self->default_sink_name, &cv, NULL, NULL);
}

static void
on_audio_mic_changed(GtkRange *range, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    gchar *str = g_strdup_printf("%d%%", (int)gtk_range_get_value(range));
    gtk_label_set_text(GTK_LABEL(self->lbl_mic_val), str);
    g_free(str);

    if (self->updating_ui || !self->pa_ctx || !self->default_source_name) return;
    if (pa_context_get_state(self->pa_ctx) != PA_CONTEXT_READY) return;

    double val = gtk_range_get_value(range);
    pa_volume_t vol = (pa_volume_t)(val * PA_VOLUME_NORM / 100.0);
    pa_cvolume cv;
    pa_cvolume_set(&cv, self->source_channels ? self->source_channels : 2, vol);
    pa_context_set_source_volume_by_name(self->pa_ctx, self->default_source_name, &cv, NULL, NULL);
}

static void
on_sink_selected(GObject *gobject, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    if (self->updating_ui || self->updating_sinks || !self->pa_ctx || !self->sink_names) return;
    if (pa_context_get_state(self->pa_ctx) != PA_CONTEXT_READY) return;

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(gobject));
    if (sel < self->sink_names->len) {
        const char *name = g_ptr_array_index(self->sink_names, sel);
        if (name && g_strcmp0(name, self->default_sink_name) != 0) {
            g_free(self->default_sink_name);
            self->default_sink_name = g_strdup(name);
            pa_context_set_default_sink(self->pa_ctx, name, NULL, NULL);
            audio_query_server_info(self);
        }
    }
}

static void
on_source_selected(GObject *gobject, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageAudio *self = user_data;
    if (self->updating_ui || self->updating_sources || !self->pa_ctx || !self->source_names) return;
    if (pa_context_get_state(self->pa_ctx) != PA_CONTEXT_READY) return;

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(gobject));
    if (sel < self->source_names->len) {
        const char *name = g_ptr_array_index(self->source_names, sel);
        if (name && g_strcmp0(name, self->default_source_name) != 0) {
            g_free(self->default_source_name);
            self->default_source_name = g_strdup(name);
            pa_context_set_default_source(self->pa_ctx, name, NULL, NULL);
            audio_query_server_info(self);
        }
    }
}

static void
settings_page_audio_dispose(GObject *object)
{
    SettingsPageAudio *self = (SettingsPageAudio *)object;

    self->sink_query_seq++;
    self->source_query_seq++;

    if (self->pa_ctx) {
        pa_context_set_state_callback(self->pa_ctx, NULL, NULL);
        pa_context_set_subscribe_callback(self->pa_ctx, NULL, NULL);
        pa_context_disconnect(self->pa_ctx);
        pa_context_unref(self->pa_ctx);
        self->pa_ctx = NULL;
    }
    if (self->pa_ml) {
        pa_glib_mainloop_free(self->pa_ml);
        self->pa_ml = NULL;
    }

    g_clear_pointer(&self->default_sink_name, g_free);
    g_clear_pointer(&self->default_source_name, g_free);

    if (self->sink_names) {
        g_ptr_array_free(self->sink_names, TRUE);
        self->sink_names = NULL;
    }
    if (self->source_names) {
        g_ptr_array_free(self->source_names, TRUE);
        self->source_names = NULL;
    }

    g_clear_object(&self->sink_string_list);
    g_clear_object(&self->source_string_list);

    gtk_widget_dispose_template(GTK_WIDGET(object), settings_page_audio_get_type());
    G_OBJECT_CLASS(settings_page_audio_parent_class)->dispose(object);
}

static void
settings_page_audio_init(SettingsPageAudio *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->sink_channels = 2;
    self->source_channels = 2;
    self->sink_names = g_ptr_array_new_with_free_func(g_free);
    self->source_names = g_ptr_array_new_with_free_func(g_free);

    self->sink_string_list = gtk_string_list_new(NULL);
    gtk_drop_down_set_model(GTK_DROP_DOWN(self->combo_sink), G_LIST_MODEL(self->sink_string_list));

    self->source_string_list = gtk_string_list_new(NULL);
    gtk_drop_down_set_model(GTK_DROP_DOWN(self->combo_source), G_LIST_MODEL(self->source_string_list));

    /* Initialize PulseAudio / PipeWire context via GLib mainloop */
    self->pa_ml = pa_glib_mainloop_new(NULL);
    if (self->pa_ml) {
        pa_mainloop_api *api = pa_glib_mainloop_get_api(self->pa_ml);
        self->pa_ctx = pa_context_new(api, "kajo-settings");
        if (self->pa_ctx) {
            pa_context_set_state_callback(self->pa_ctx, audio_context_state_cb, self);
            pa_context_connect(self->pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
        }
    }
}

static void
settings_page_audio_class_init(SettingsPageAudioClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = settings_page_audio_dispose;

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_audio.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPageAudio, combo_sink);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageAudio, scale_vol);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageAudio, lbl_vol_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageAudio, combo_source);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageAudio, scale_mic);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageAudio, lbl_mic_val);
}

GtkWidget *
build_page_audio(void)
{
    SettingsPageAudio *page = g_object_new(settings_page_audio_get_type(), NULL);

    g_signal_connect(page->scale_vol, "value-changed", G_CALLBACK(on_audio_vol_changed), page);
    g_signal_connect(page->scale_mic, "value-changed", G_CALLBACK(on_audio_mic_changed), page);
    g_signal_connect(page->combo_sink, "notify::selected", G_CALLBACK(on_sink_selected), page);
    g_signal_connect(page->combo_source, "notify::selected", G_CALLBACK(on_source_selected), page);

    return create_settings_page_card("PIPEWIRE AUDIO SOUND & ROUTING", "[ PIPEWIRE ACTIVE ]", GTK_WIDGET(page), NULL);
}
