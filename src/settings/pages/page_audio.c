#include "../settings_common.h"
#include "../settings_pages.h"

static void
on_audio_vol_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    gchar *str = g_strdup_printf("%d%%", (int)gtk_range_get_value(range));
    gtk_label_set_text(lbl, str);
    g_free(str);
}

static void
on_audio_mic_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    gchar *str = g_strdup_printf("%d%%", (int)gtk_range_get_value(range));
    gtk_label_set_text(lbl, str);
    g_free(str);
}

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageAudio {
    GtkBox parent_instance;

    GtkWidget *combo_sink;
    GtkWidget *scale_vol;
    GtkWidget *lbl_vol_val;
    GtkWidget *combo_source;
    GtkWidget *scale_mic;
    GtkWidget *lbl_mic_val;
} SettingsPageAudio;

typedef struct _SettingsPageAudioClass {
    GtkBoxClass parent_class;
} SettingsPageAudioClass;

G_DEFINE_TYPE(SettingsPageAudio, settings_page_audio, GTK_TYPE_BOX)

static void
settings_page_audio_init(SettingsPageAudio *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_audio_class_init(SettingsPageAudioClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

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

    const char *sink_items[] = {
        "Built-in Speaker / Analog Stereo (Default Output)",
        "Dolby Access / Virtual Surround 7.1 (Spatial Audio Virtual Sink)",
        NULL
    };
    GtkStringList *slist_sink = gtk_string_list_new(sink_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_sink), G_LIST_MODEL(slist_sink));

    const char *source_items[] = {
        "Internal Array Microphone (Analog Stereo Input)",
        "USB Headset Digital Microphone",
        NULL
    };
    GtkStringList *slist_source = gtk_string_list_new(source_items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_source), G_LIST_MODEL(slist_source));

    gtk_range_set_value(GTK_RANGE(page->scale_vol), 65);
    g_signal_connect(page->scale_vol, "value-changed", G_CALLBACK(on_audio_vol_changed), page->lbl_vol_val);

    gtk_range_set_value(GTK_RANGE(page->scale_mic), 80);
    g_signal_connect(page->scale_mic, "value-changed", G_CALLBACK(on_audio_mic_changed), page->lbl_mic_val);

    return create_settings_page_card("PIPEWIRE AUDIO SOUND & ROUTING", "[ PIPEWIRE ACTIVE ]", GTK_WIDGET(page), NULL);
}
