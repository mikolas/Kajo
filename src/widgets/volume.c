/* Volume widget - PulseAudio/PipeWire integration with rich audio popover */

#include <gtk/gtk.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

#include "widget.h"
#include "../osd/osd.h"
#include "../shell.h"

extern const ShellWidgetClass volume_widget_class;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;          /* GtkMenuButton */
    GtkWidget *button_box;      /* icon + label inside button */
    GtkWidget *icon;
    GtkWidget *label;
    GtkWidget *popover;
    GtkWidget *master_scale;    /* volume slider */
    GtkWidget *mute_button;
    GtkWidget *device_dropdown; /* GtkDropDown for output devices */
    GtkStringList *string_list;
    GPtrArray *sink_names;
    gboolean updating_dropdown;
    GtkWidget *apps_box;        /* container for per-app sliders */
    GtkWidget *input_label;     /* source name */
    pa_glib_mainloop *pa_ml;
    pa_context *pa_ctx;
    gchar *default_sink_name;
    gchar *default_source_name;
    guint32 volume_percent;     /* 0..150 */
    gboolean muted;
    gboolean initialized;
    guint sink_query_seq;
} VolumeWidget;

typedef struct {
    VolumeWidget *vol;
    guint query_seq;
} SinkQueryTag;

static void volume_query_default_sink(VolumeWidget *vol);
static void volume_query_sinks(VolumeWidget *vol);
static void volume_query_sink_inputs(VolumeWidget *vol);
static void volume_query_default_source(VolumeWidget *vol);
static void volume_master_scale_changed(GtkRange *range, gpointer userdata);
static void on_device_dropdown_selected(GObject *gobject, GParamSpec *pspec, gpointer userdata);

/* ─── UI update helpers ─── */

static const char *volume_icon_name(VolumeWidget *vol)
{
    if (vol->muted)
        return "audio-volume-muted-symbolic";
    if (vol->volume_percent <= 33)
        return "audio-volume-low-symbolic";
    if (vol->volume_percent <= 66)
        return "audio-volume-medium-symbolic";
    return "audio-volume-high-symbolic";
}

static void volume_update_icon(VolumeWidget *vol)
{
    gtk_image_set_from_icon_name(GTK_IMAGE(vol->icon), volume_icon_name(vol));

    gchar *text = g_strdup_printf("%u%%", vol->volume_percent);
    gtk_label_set_text(GTK_LABEL(vol->label), text);
    g_free(text);

    shell_widget_apply_mode_visibility(vol->base.mode, vol->icon, vol->label);

    if (vol->input_label) {
        gchar *badge = vol->muted ? g_strdup("[ MUTED ]") : g_strdup_printf("[ %u%% ]", vol->volume_percent);
        gtk_label_set_text(GTK_LABEL(vol->input_label), badge);
        if (vol->muted)
            gtk_widget_add_css_class(vol->input_label, "urgent");
        else
            gtk_widget_remove_css_class(vol->input_label, "urgent");
        g_free(badge);
    }

    if (vol->mute_button) {
        gtk_button_set_label(GTK_BUTTON(vol->mute_button),
                             vol->muted ? "UNMUTE SINK" : "MUTE SINK");
    }

    if (vol->muted)
        gtk_widget_add_css_class(vol->button, "muted");
    else
        gtk_widget_remove_css_class(vol->button, "muted");
}

/* ─── PulseAudio volume helpers ─── */

static pa_cvolume volume_make_cvolume(guint32 percent, int channels)
{
    pa_cvolume cv;
    pa_cvolume_init(&cv);
    cv.channels = (uint8_t)channels;
    pa_volume_t v = (pa_volume_t)((percent * PA_VOLUME_NORM + 50) / 100);
    pa_cvolume_set(&cv, cv.channels, v);
    return cv;
}

static guint32 volume_percent_from_cvolume(const pa_cvolume *cv)
{
    if (!cv || cv->channels == 0)
        return 0;
    pa_volume_t avg = pa_cvolume_avg(cv);
    return (guint32)((avg * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
}

/* ─── Master volume / default sink callbacks ─── */

static void volume_sink_info_cb(pa_context *ctx G_GNUC_UNUSED, const pa_sink_info *info,
                                int eol, void *userdata)
{
    VolumeWidget *vol = userdata;

    if (eol > 0 || !info)
        return;

    /* Only update master from default sink */
    if (!vol->default_sink_name || g_strcmp0(info->name, vol->default_sink_name) != 0)
        return;

    guint32 new_percent = volume_percent_from_cvolume(&info->volume);
    gboolean new_muted = info->mute ? TRUE : FALSE;
    gboolean changed = (vol->volume_percent != new_percent || vol->muted != new_muted);

    vol->volume_percent = new_percent;
    vol->muted = new_muted;

    volume_update_icon(vol);

    /* Update master scale without retriggering the signal */
    if (vol->master_scale && GTK_IS_RANGE(vol->master_scale)) {
        g_signal_handlers_block_by_func(vol->master_scale,
                                        (gpointer)G_CALLBACK(volume_master_scale_changed), vol);
        gtk_range_set_value(GTK_RANGE(vol->master_scale), (double)vol->volume_percent);
        g_signal_handlers_unblock_by_func(vol->master_scale,
                                          (gpointer)G_CALLBACK(volume_master_scale_changed), vol);
    }

    /* Update mute button label */
    if (vol->mute_button) {
        gtk_button_set_label(GTK_BUTTON(vol->mute_button),
                             vol->muted ? "Unmute" : "Mute");
    }

    if (!vol->initialized) {
        vol->initialized = TRUE;
    } else if (changed && vol->base.app) {
        ShellOSD *osd = shell_app_get_osd(vol->base.app);
        if (osd) {
            shell_osd_show_volume(osd, vol->volume_percent, vol->muted);
        }
    }
}

static void volume_server_info_cb(pa_context *ctx, const pa_server_info *info,
                                  void *userdata)
{
    VolumeWidget *vol = userdata;

    if (!info)
        return;

    if (info->default_sink_name) {
        g_free(vol->default_sink_name);
        vol->default_sink_name = g_strdup(info->default_sink_name);
    }
    if (info->default_source_name) {
        g_free(vol->default_source_name);
        vol->default_source_name = g_strdup(info->default_source_name);
    }

    if (vol->default_sink_name) {
        pa_context_get_sink_info_by_name(ctx, vol->default_sink_name,
                                         volume_sink_info_cb, vol);
    }
    volume_query_default_source(vol);
}

static void volume_query_default_sink(VolumeWidget *vol)
{
    if (!vol->pa_ctx ||
        pa_context_get_state(vol->pa_ctx) != PA_CONTEXT_READY)
        return;

    pa_context_get_server_info(vol->pa_ctx, volume_server_info_cb, vol);
}

/* ─── Output device list (sinks enumeration) ─── */

static void on_device_dropdown_selected(GObject *gobject, GParamSpec *pspec G_GNUC_UNUSED, gpointer userdata)
{
    VolumeWidget *vol = userdata;
    if (vol->updating_dropdown || !vol->pa_ctx || !vol->sink_names)
        return;

    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(gobject));
    if (selected < vol->sink_names->len) {
        const char *sink_name = g_ptr_array_index(vol->sink_names, selected);
        if (sink_name && pa_context_get_state(vol->pa_ctx) == PA_CONTEXT_READY) {
            if (g_strcmp0(sink_name, vol->default_sink_name) != 0) {
                g_free(vol->default_sink_name);
                vol->default_sink_name = g_strdup(sink_name);
                pa_context_set_default_sink(vol->pa_ctx, sink_name, NULL, NULL);
                volume_query_default_sink(vol);
            }
        }
    }
}

static void volume_sink_list_cb(pa_context *ctx G_GNUC_UNUSED, const pa_sink_info *info,
                                int eol, void *userdata)
{
    SinkQueryTag *tag = userdata;
    if (!tag || !tag->vol) return;
    VolumeWidget *vol = tag->vol;

    if (tag->query_seq != vol->sink_query_seq) {
        if (eol > 0) g_free(tag);
        return;
    }

    if (eol > 0) {
        vol->updating_dropdown = FALSE;
        g_free(tag);
        return;
    }
    if (!info)
        return;

    /* Prevent duplicate entries if multiple queries are in flight */
    for (guint i = 0; i < vol->sink_names->len; i++) {
        const char *existing = g_ptr_array_index(vol->sink_names, i);
        if (g_strcmp0(existing, info->name) == 0)
            return;
    }

    const char *desc = info->description ? info->description : info->name;
    g_ptr_array_add(vol->sink_names, g_strdup(info->name));
    gtk_string_list_append(vol->string_list, desc);

    if (vol->default_sink_name && g_strcmp0(info->name, vol->default_sink_name) == 0) {
        guint idx = vol->sink_names->len - 1;
        if (vol->device_dropdown) {
            g_signal_handlers_block_by_func(vol->device_dropdown, on_device_dropdown_selected, vol);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(vol->device_dropdown), idx);
            g_signal_handlers_unblock_by_func(vol->device_dropdown, on_device_dropdown_selected, vol);
        }
    }
}

static void volume_clear_container(GtkWidget *container)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(container)) != NULL)
        gtk_box_remove(GTK_BOX(container), child);
}

static void volume_query_sinks(VolumeWidget *vol)
{
    if (!vol->pa_ctx ||
        pa_context_get_state(vol->pa_ctx) != PA_CONTEXT_READY)
        return;

    vol->sink_query_seq++;
    vol->updating_dropdown = TRUE;

    if (vol->sink_names)
        g_ptr_array_set_size(vol->sink_names, 0);

    GtkStringList *new_list = gtk_string_list_new(NULL);
    if (vol->string_list) {
        g_object_unref(vol->string_list);
    }
    vol->string_list = new_list;

    if (vol->device_dropdown) {
        gtk_drop_down_set_model(GTK_DROP_DOWN(vol->device_dropdown), G_LIST_MODEL(vol->string_list));
    }

    SinkQueryTag *tag = g_new0(SinkQueryTag, 1);
    tag->vol = vol;
    tag->query_seq = vol->sink_query_seq;

    pa_context_get_sink_info_list(vol->pa_ctx, volume_sink_list_cb, tag);
}

/* ─── Per-application volume (sink-inputs) ─── */

static void volume_app_scale_changed(GtkRange *range, gpointer userdata)
{
    VolumeWidget *vol = userdata;
    guint32 idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(range), "sink-input-index"));
    guint32 channels = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(range), "channels"));

    if (!vol->pa_ctx ||
        pa_context_get_state(vol->pa_ctx) != PA_CONTEXT_READY)
        return;

    guint32 percent = (guint32)gtk_range_get_value(range);
    pa_cvolume cv = volume_make_cvolume(percent, channels > 0 ? (int)channels : 2);

    pa_context_set_sink_input_volume(vol->pa_ctx, idx, &cv, NULL, NULL);
}

static void volume_sink_input_list_cb(pa_context *ctx G_GNUC_UNUSED,
                                      const pa_sink_input_info *info,
                                      int eol, void *userdata)
{
    VolumeWidget *vol = userdata;

    if (eol > 0 || !info)
        return;

    /* Get application name */
    const char *app_name = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    if (!app_name)
        app_name = "Unknown";

    guint32 percent = volume_percent_from_cvolume(&info->volume);
    gchar app_title[128];
    snprintf(app_title, sizeof(app_title), "%s (%u%%)", app_name, percent);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *name_label = gtk_label_new(app_title);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0f);
    gtk_box_append(GTK_BOX(row), name_label);

    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 150, 1);
    gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_range_set_value(GTK_RANGE(scale), (double)percent);

    g_object_set_data(G_OBJECT(scale), "sink-input-index",
                      GUINT_TO_POINTER(info->index));
    g_object_set_data(G_OBJECT(scale), "channels",
                      GUINT_TO_POINTER((guint32)info->volume.channels));

    g_signal_connect(scale, "value-changed",
                     G_CALLBACK(volume_app_scale_changed), vol);

    gtk_box_append(GTK_BOX(row), scale);
    gtk_box_append(GTK_BOX(vol->apps_box), row);
}

static void volume_query_sink_inputs(VolumeWidget *vol)
{
    if (!vol->pa_ctx ||
        pa_context_get_state(vol->pa_ctx) != PA_CONTEXT_READY)
        return;

    volume_clear_container(vol->apps_box);
    pa_context_get_sink_input_info_list(vol->pa_ctx,
                                        volume_sink_input_list_cb, vol);
}

/* ─── Input (source/microphone) ─── */

static void volume_source_info_cb(pa_context *ctx G_GNUC_UNUSED, const pa_source_info *info,
                                  int eol, void *userdata)
{
    VolumeWidget *vol = userdata;

    if (eol > 0 || !info)
        return;

    if (!vol->default_source_name ||
        g_strcmp0(info->name, vol->default_source_name) != 0)
        return;

    guint32 percent = volume_percent_from_cvolume(&info->volume);

    const char *desc = info->description ? info->description : info->name;
    gchar input_buf[128];
    snprintf(input_buf, sizeof(input_buf), "%s (%u%%)", desc, percent);
    gtk_label_set_text(GTK_LABEL(vol->input_label), input_buf);
}

static void volume_query_default_source(VolumeWidget *vol)
{
    if (!vol->pa_ctx ||
        pa_context_get_state(vol->pa_ctx) != PA_CONTEXT_READY ||
        !vol->default_source_name)
        return;

    pa_context_get_source_info_by_name(vol->pa_ctx, vol->default_source_name,
                                       volume_source_info_cb, vol);
}

/* ─── Subscription callback ─── */

static void volume_subscription_cb(pa_context *ctx G_GNUC_UNUSED,
                                   pa_subscription_event_type_t type,
                                   uint32_t idx G_GNUC_UNUSED, void *userdata)
{
    VolumeWidget *vol = userdata;
    unsigned facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;

    if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
        volume_query_default_sink(vol);
        volume_query_sinks(vol);
    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        volume_query_sink_inputs(vol);
    } else if (facility == PA_SUBSCRIPTION_EVENT_SOURCE) {
        volume_query_default_source(vol);
    }
}

/* ─── PA context state callback ─── */

static void volume_context_state_cb(pa_context *ctx, void *userdata)
{
    VolumeWidget *vol = userdata;
    pa_context_state_t state = pa_context_get_state(ctx);

    switch (state) {
    case PA_CONTEXT_READY:
        pa_context_set_subscribe_callback(ctx, volume_subscription_cb, vol);
        pa_context_subscribe(ctx,
            PA_SUBSCRIPTION_MASK_SINK |
            PA_SUBSCRIPTION_MASK_SINK_INPUT |
            PA_SUBSCRIPTION_MASK_SOURCE,
            NULL, NULL);
        volume_query_default_sink(vol);
        volume_query_sinks(vol);
        volume_query_sink_inputs(vol);
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        gtk_label_set_text(GTK_LABEL(vol->label), "--");
        gtk_image_set_from_icon_name(GTK_IMAGE(vol->icon),
                                     "audio-volume-muted-symbolic");
        break;
    default:
        break;
    }
}

/* ─── UI interaction handlers ─── */

static void volume_set_volume_absolute(VolumeWidget *vol, guint32 percent)
{
    if (!vol->pa_ctx ||
        pa_context_get_state(vol->pa_ctx) != PA_CONTEXT_READY ||
        !vol->default_sink_name)
        return;

    pa_cvolume cv = volume_make_cvolume(percent, 2);
    pa_context_set_sink_volume_by_name(vol->pa_ctx, vol->default_sink_name,
                                       &cv, NULL, NULL);
}

static void volume_set_volume(VolumeWidget *vol, gint delta_percent)
{
    gint new_percent = (gint)vol->volume_percent + delta_percent;
    if (new_percent < 0) new_percent = 0;
    if (new_percent > 150) new_percent = 150;

    volume_set_volume_absolute(vol, (guint32)new_percent);
}

static gboolean volume_scroll_cb(GtkEventControllerScroll *controller G_GNUC_UNUSED,
                                 double dx G_GNUC_UNUSED, double dy, gpointer userdata)
{
    VolumeWidget *vol = userdata;

    if (dy < 0)
        volume_set_volume(vol, 5);
    else if (dy > 0)
        volume_set_volume(vol, -5);

    return TRUE;
}

static void volume_master_scale_changed(GtkRange *range, gpointer userdata)
{
    VolumeWidget *vol = userdata;
    guint32 percent = (guint32)gtk_range_get_value(range);
    volume_set_volume_absolute(vol, percent);
}

static void volume_mute_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer userdata)
{
    VolumeWidget *vol = userdata;

    if (!vol->pa_ctx ||
        pa_context_get_state(vol->pa_ctx) != PA_CONTEXT_READY ||
        !vol->default_sink_name)
        return;

    pa_context_set_sink_mute_by_name(vol->pa_ctx, vol->default_sink_name,
                                     !vol->muted, NULL, NULL);
}

void volume_widget_change_volume(ShellWidget *widget, gint delta_percent)
{
    if (!widget) return;
    VolumeWidget *vol = (VolumeWidget *)widget;
    volume_set_volume(vol, delta_percent);
}

void volume_widget_toggle_mute(ShellWidget *widget)
{
    if (!widget) return;
    VolumeWidget *vol = (VolumeWidget *)widget;
    volume_mute_clicked(NULL, vol);
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellVolumePopover {
    GtkPopover parent_instance;

    GtkWidget *input_label;
    GtkWidget *master_scale;
    GtkWidget *device_dropdown;
    GtkWidget *apps_box;
    GtkWidget *mute_button;
    GtkWidget *settings_btn;
} ShellVolumePopover;

typedef struct _ShellVolumePopoverClass {
    GtkPopoverClass parent_class;
} ShellVolumePopoverClass;

G_DEFINE_TYPE(ShellVolumePopover, shell_volume_popover, GTK_TYPE_POPOVER)

static void
shell_volume_popover_init(ShellVolumePopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_volume_popover_class_init(ShellVolumePopoverClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/volume_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellVolumePopover, input_label);
    gtk_widget_class_bind_template_child(widget_class, ShellVolumePopover, master_scale);
    gtk_widget_class_bind_template_child(widget_class, ShellVolumePopover, device_dropdown);
    gtk_widget_class_bind_template_child(widget_class, ShellVolumePopover, apps_box);
    gtk_widget_class_bind_template_child(widget_class, ShellVolumePopover, mute_button);
    gtk_widget_class_bind_template_child(widget_class, ShellVolumePopover, settings_btn);
}

/* ─── Popover construction ─── */

static GtkWidget *volume_build_popover(VolumeWidget *vol)
{
    ShellVolumePopover *popover = g_object_new(shell_volume_popover_get_type(), NULL);

    vol->input_label = popover->input_label;
    vol->master_scale = popover->master_scale;
    vol->device_dropdown = popover->device_dropdown;
    vol->apps_box = popover->apps_box;
    vol->mute_button = popover->mute_button;

    vol->string_list = gtk_string_list_new(NULL);
    gtk_drop_down_set_model(GTK_DROP_DOWN(vol->device_dropdown), G_LIST_MODEL(vol->string_list));

    g_signal_connect(vol->master_scale, "value-changed",
                     G_CALLBACK(volume_master_scale_changed), vol);

    g_signal_connect(vol->device_dropdown, "notify::selected",
                     G_CALLBACK(on_device_dropdown_selected), vol);

    g_signal_connect(vol->mute_button, "clicked",
                     G_CALLBACK(volume_mute_clicked), vol);

    return GTK_WIDGET(popover);
}

/* ─── Widget lifecycle ─── */

static ShellWidget *volume_create(ShellCompositor *compositor)
{
    VolumeWidget *vol = g_new0(VolumeWidget, 1);
    vol->base.klass = &volume_widget_class;
    vol->compositor = compositor;
    vol->sink_names = g_ptr_array_new_with_free_func(g_free);

    /* Panel button: GtkMenuButton (flat) with icon + label */
    vol->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(vol->button), FALSE);
    gtk_widget_add_css_class(vol->button, "flat");
    gtk_widget_add_css_class(vol->button, "shell-widget");
    gtk_widget_add_css_class(vol->button, "shell-widget-volume");

    vol->button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    vol->icon = gtk_image_new_from_icon_name("audio-volume-muted-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(vol->icon), shell_widget_get_icon_size((ShellWidget *)vol));
    vol->label = gtk_label_new("--");
    gtk_box_append(GTK_BOX(vol->button_box), vol->icon);
    gtk_box_append(GTK_BOX(vol->button_box), vol->label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(vol->button), vol->button_box);

    /* Build popover and attach to button */
    vol->popover = volume_build_popover(vol);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(vol->button), vol->popover);

    /* Scroll controller on button for volume adjustment */
    GtkEventController *scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
        GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(volume_scroll_cb), vol);
    gtk_widget_add_controller(vol->button, scroll);

    /* Connect to PulseAudio */
    vol->pa_ml = pa_glib_mainloop_new(NULL);
    if (vol->pa_ml) {
        pa_mainloop_api *api = pa_glib_mainloop_get_api(vol->pa_ml);
        vol->pa_ctx = pa_context_new(api, "shell-volume-widget");

        if (vol->pa_ctx) {
            pa_context_set_state_callback(vol->pa_ctx,
                                          volume_context_state_cb, vol);
            pa_context_connect(vol->pa_ctx, NULL, PA_CONTEXT_NOFAIL, NULL);
        }
    }

    return (ShellWidget *)vol;
}

static void volume_destroy(ShellWidget *widget)
{
    VolumeWidget *vol = (VolumeWidget *)widget;

    if (vol->master_scale)
        g_signal_handlers_disconnect_by_data(vol->master_scale, vol);
    if (vol->device_dropdown)
        g_signal_handlers_disconnect_by_data(vol->device_dropdown, vol);
    if (vol->mute_button)
        g_signal_handlers_disconnect_by_data(vol->mute_button, vol);

    if (vol->sink_names) {
        g_ptr_array_free(vol->sink_names, TRUE);
        vol->sink_names = NULL;
    }

    if (vol->pa_ctx) {
        pa_context_set_state_callback(vol->pa_ctx, NULL, NULL);
        pa_context_set_subscribe_callback(vol->pa_ctx, NULL, NULL);
        pa_context_disconnect(vol->pa_ctx);
        pa_context_unref(vol->pa_ctx);
        vol->pa_ctx = NULL;
    }

    if (vol->pa_ml) {
        pa_glib_mainloop_free(vol->pa_ml);
        vol->pa_ml = NULL;
    }

    if (vol->string_list) {
        g_clear_object(&vol->string_list);
    }
    g_free(vol->default_sink_name);
    g_free(vol->default_source_name);
    g_free(vol);
}

static GtkWidget *volume_get_widget(ShellWidget *widget)
{
    VolumeWidget *vol = (VolumeWidget *)widget;
    return vol->button;
}

static void volume_enable(ShellWidget *widget)
{
    VolumeWidget *vol = (VolumeWidget *)widget;
    if (vol) {
        shell_widget_apply_mode_visibility(widget->mode, vol->icon, vol->label);
    }
}

static void volume_disable(ShellWidget *widget)
{
    (void)widget;
}

const ShellWidgetClass volume_widget_class = {
    .id         = "volume",
    .name       = "Volume",
    .create     = volume_create,
    .destroy    = volume_destroy,
    .get_widget = volume_get_widget,
    .enable     = volume_enable,
    .disable    = volume_disable,
};
