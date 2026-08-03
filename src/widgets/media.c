#include "widget.h"
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPRIS_BUS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"

extern const ShellWidgetClass media_widget_class;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *popover;

    /* Popover UI Controls */
    GtkWidget *pop_status_badge;
    GtkWidget *pop_title_label;
    GtkWidget *pop_artist_label;
    GtkWidget *pop_art_img;
    GtkWidget *play_btn;
    GtkWidget *prev_btn;
    GtkWidget *next_btn;
    GtkWidget *meter_l;
    GtkWidget *meter_r;
    GtkWidget *lbl_peak_l;
    GtkWidget *lbl_peak_r;
    double peak_l;
    double peak_r;

    GDBusProxy *mpris_proxy;
    guint timer_id;
    guint vu_timer_id;
    guint ticker_timer_id;
    gsize ticker_offset;
    gboolean is_hovered;
    gboolean is_playing;
    gchar *artist;
    gchar *title;
    gchar *art_url;
    gchar *active_player_bus;
} MediaWidget;

static void update_media_ui(MediaWidget *media);
static void refresh_media_state(MediaWidget *media);

static GVariant *
get_mpris_property(GDBusProxy *proxy, const char *prop_name)
{
    if (!proxy) return NULL;
    GVariant *val = g_dbus_proxy_get_cached_property(proxy, prop_name);
    if (val != NULL) return val;

    GVariant *res = g_dbus_proxy_call_sync(
        proxy, "org.freedesktop.DBus.Properties.Get",
        g_variant_new("(ss)", MPRIS_PLAYER_INTERFACE, prop_name),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (res != NULL) {
        GVariant *inner = NULL;
        g_variant_get(res, "(v)", &inner);
        g_variant_unref(res);
        return inner;
    }
    return NULL;
}

static gboolean
on_title_ticker_step(gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media || !media->is_hovered || !media->pop_title_label || !media->title) {
        if (media) media->ticker_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    glong title_len = g_utf8_strlen(media->title, -1);
    if (title_len <= 26) {
        media->ticker_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    gchar *padded_title = g_strdup_printf("%s    %s", media->title, media->title);
    glong padded_len = g_utf8_strlen(padded_title, -1);

    media->ticker_offset = (media->ticker_offset + 1) % (title_len + 4);

    const gchar *start_ptr = g_utf8_offset_to_pointer(padded_title, media->ticker_offset);
    const gchar *end_ptr = g_utf8_offset_to_pointer(start_ptr, MIN(26, padded_len - (glong)media->ticker_offset));

    gchar *window_str = g_strndup(start_ptr, end_ptr - start_ptr);
    gtk_label_set_text(GTK_LABEL(media->pop_title_label), window_str);

    g_free(window_str);
    g_free(padded_title);

    return G_SOURCE_CONTINUE;
}

static void
on_title_hover_enter(GtkEventControllerMotion *controller G_GNUC_UNUSED,
                     gdouble x G_GNUC_UNUSED, gdouble y G_GNUC_UNUSED,
                     gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media || !media->title) return;

    glong title_len = g_utf8_strlen(media->title, -1);
    if (title_len <= 26) return;

    media->is_hovered = TRUE;
    media->ticker_offset = 0;

    if (media->ticker_timer_id == 0) {
        media->ticker_timer_id = g_timeout_add(150, on_title_ticker_step, media);
    }
}

static void
on_title_hover_leave(GtkEventControllerMotion *controller G_GNUC_UNUSED,
                     gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media) return;

    media->is_hovered = FALSE;
    if (media->ticker_timer_id != 0) {
        g_source_remove(media->ticker_timer_id);
        media->ticker_timer_id = 0;
    }

    if (media->pop_title_label && media->title) {
        gtk_label_set_text(GTK_LABEL(media->pop_title_label), media->title);
    }
}

static void
on_play_pause_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MediaWidget *media = user_data;
    if (media->mpris_proxy != NULL) {
        g_dbus_proxy_call(media->mpris_proxy, "PlayPause", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
on_prev_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MediaWidget *media = user_data;
    if (media->mpris_proxy != NULL) {
        g_dbus_proxy_call(media->mpris_proxy, "Previous", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
on_next_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    MediaWidget *media = user_data;
    if (media->mpris_proxy != NULL) {
        g_dbus_proxy_call(media->mpris_proxy, "Next", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
refresh_media_state(MediaWidget *media)
{
    GError *error = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    gchar *best_playing_bus = NULL;
    gchar *first_found_bus = NULL;

    if (conn != NULL) {
        GVariant *res = g_dbus_connection_call_sync(
            conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "ListNames", NULL,
            G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

        if (res != NULL) {
            GVariantIter *iter;
            gchar *name;
            g_variant_get(res, "(as)", &iter);
            while (g_variant_iter_loop(iter, "s", &name)) {
                if (g_str_has_prefix(name, MPRIS_BUS_PREFIX)) {
                    if (!first_found_bus) first_found_bus = g_strdup(name);

                    GDBusProxy *temp_proxy = g_dbus_proxy_new_for_bus_sync(
                        G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                        name, "/org/mpris/MediaPlayer2",
                        MPRIS_PLAYER_INTERFACE, NULL, NULL);
                    if (temp_proxy) {
                        GVariant *status_var = get_mpris_property(temp_proxy, "PlaybackStatus");
                        if (status_var) {
                            const gchar *status_str = g_variant_get_string(status_var, NULL);
                            if (g_strcmp0(status_str, "Playing") == 0) {
                                best_playing_bus = g_strdup(name);
                            }
                            g_variant_unref(status_var);
                        }
                        g_object_unref(temp_proxy);
                    }
                    if (best_playing_bus) break;
                }
            }
            g_variant_iter_free(iter);
            g_variant_unref(res);
        }
        g_object_unref(conn);
    }
    if (error) g_clear_error(&error);

    gchar *target_bus = best_playing_bus ? best_playing_bus : first_found_bus;

    if (g_strcmp0(media->active_player_bus, target_bus) != 0) {
        if (media->mpris_proxy) {
            g_object_unref(media->mpris_proxy);
            media->mpris_proxy = NULL;
        }
        g_free(media->active_player_bus);
        media->active_player_bus = g_strdup(target_bus);

        if (media->active_player_bus) {
            GError *error = NULL;
            media->mpris_proxy = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                media->active_player_bus, "/org/mpris/MediaPlayer2",
                MPRIS_PLAYER_INTERFACE, NULL, &error);
            if (error) {
                g_warning("media: failed to connect to player %s: %s", media->active_player_bus, error->message);
                g_clear_error(&error);
            }
        }
    }
    g_free(best_playing_bus);
    g_free(first_found_bus);

    if (media->mpris_proxy != NULL) {
        GVariant *status_var = get_mpris_property(media->mpris_proxy, "PlaybackStatus");
        if (status_var != NULL) {
            const gchar *status_str = g_variant_get_string(status_var, NULL);
            media->is_playing = (g_strcmp0(status_str, "Playing") == 0);
            g_variant_unref(status_var);
        } else {
            media->is_playing = FALSE;
        }

        GVariant *meta_var = get_mpris_property(media->mpris_proxy, "Metadata");
        if (meta_var != NULL) {
            GVariant *title_var = g_variant_lookup_value(meta_var, "xesam:title", G_VARIANT_TYPE_STRING);
            if (title_var != NULL) {
                g_free(media->title);
                media->title = g_variant_dup_string(title_var, NULL);
                g_variant_unref(title_var);
            }

            GVariant *artist_var = g_variant_lookup_value(meta_var, "xesam:artist", NULL);
            if (artist_var != NULL) {
                g_free(media->artist);
                media->artist = NULL;
                if (g_variant_is_of_type(artist_var, G_VARIANT_TYPE_STRING_ARRAY)) {
                    if (g_variant_n_children(artist_var) > 0) {
                        GVariant *child = g_variant_get_child_value(artist_var, 0);
                        media->artist = g_variant_dup_string(child, NULL);
                        g_variant_unref(child);
                    }
                } else if (g_variant_is_of_type(artist_var, G_VARIANT_TYPE_STRING)) {
                    media->artist = g_variant_dup_string(artist_var, NULL);
                }
                g_variant_unref(artist_var);
            }

            GVariant *art_var = g_variant_lookup_value(meta_var, "mpris:artUrl", G_VARIANT_TYPE_STRING);
            if (art_var != NULL) {
                g_free(media->art_url);
                media->art_url = g_variant_dup_string(art_var, NULL);
                g_variant_unref(art_var);
            }

            g_variant_unref(meta_var);
        }
    } else {
        media->is_playing = FALSE;
    }

    update_media_ui(media);
}

static void
update_media_ui(MediaWidget *media)
{
    const gchar *icon_name = media->is_playing
                                  ? "tb-player-pause-symbolic"
                                  : "tb-player-play-symbolic";

    gchar label_text[128];
    if (media->title != NULL && media->title[0] != '\0') {
        if (media->artist != NULL && media->artist[0] != '\0') {
            snprintf(label_text, sizeof(label_text), "%.16s - %.20s", media->artist, media->title);
        } else {
            snprintf(label_text, sizeof(label_text), "%.24s", media->title);
        }
    } else {
        snprintf(label_text, sizeof(label_text), "No Media");
    }

    gtk_image_set_from_icon_name(GTK_IMAGE(media->icon_img), icon_name);
    gtk_label_set_text(GTK_LABEL(media->label), label_text);
    shell_widget_apply_mode_visibility(media->base.mode, media->icon_img, media->label);

    /* Update Popover Play/Pause Button */
    if (media->play_btn != NULL) {
        gtk_button_set_label(GTK_BUTTON(media->play_btn), media->is_playing ? "PAUSE" : "PLAY");
    }

    /* Update Popover Status Badge */
    if (media->pop_status_badge != NULL) {
        if (media->active_player_bus != NULL) {
            gtk_label_set_text(GTK_LABEL(media->pop_status_badge),
                               media->is_playing ? "[ PLAYING ]" : "[ PAUSED ]");
        } else {
            gtk_label_set_text(GTK_LABEL(media->pop_status_badge), "[ NO PLAYER ]");
        }
    }

    if (media->pop_title_label != NULL) {
        const char *t = (media->title && media->title[0] != '\0') ? media->title : "No Track Selected";
        gtk_label_set_text(GTK_LABEL(media->pop_title_label), t);
        gtk_widget_set_tooltip_text(media->pop_title_label, t);
    }
    if (media->pop_artist_label != NULL) {
        const char *a = (media->artist && media->artist[0] != '\0') ? media->artist : "Unknown Artist";
        gtk_label_set_text(GTK_LABEL(media->pop_artist_label), a);
        gtk_widget_set_tooltip_text(media->pop_artist_label, a);
    }

    /* Render Album Art Thumbnail */
    if (media->pop_art_img != NULL) {
        GFile *file = NULL;
        if (media->art_url && g_str_has_prefix(media->art_url, "file://")) {
            file = g_file_new_for_uri(media->art_url);
        } else if (media->art_url && media->art_url[0] == '/') {
            file = g_file_new_for_path(media->art_url);
        }

        if (file != NULL) {
            GdkTexture *texture = gdk_texture_new_from_file(file, NULL);
            g_object_unref(file);
            if (texture != NULL) {
                gtk_image_set_from_paintable(GTK_IMAGE(media->pop_art_img), GDK_PAINTABLE(texture));
                g_object_unref(texture);
            } else {
                gtk_image_set_from_icon_name(GTK_IMAGE(media->pop_art_img), "tb-player-play-symbolic");
            }
        } else {
            gtk_image_set_from_icon_name(GTK_IMAGE(media->pop_art_img), "tb-player-play-symbolic");
        }
    }
}

static gboolean
on_vu_meter_30fps_tick(gpointer user_data)
{
    MediaWidget *media = user_data;
    if (!media) return G_SOURCE_CONTINUE;

    if (media->is_playing) {
        double new_target_l = (double)(rand() % 65 + 35) / 100.0;
        double new_target_r = (double)(rand() % 65 + 35) / 100.0;

        /* Peak Jump + Fast Exponential Decay Physics */
        if (new_target_l > media->peak_l) {
            media->peak_l = new_target_l; /* INSTANT PEAK JUMP */
        } else {
            media->peak_l *= 0.72; /* FAST DECAY */
        }

        if (new_target_r > media->peak_r) {
            media->peak_r = new_target_r; /* INSTANT PEAK JUMP */
        } else {
            media->peak_r *= 0.72; /* FAST DECAY */
        }
    } else {
        media->peak_l *= 0.50;
        media->peak_r *= 0.50;
        if (media->peak_l < 0.01) media->peak_l = 0.0;
        if (media->peak_r < 0.01) media->peak_r = 0.0;
    }

    if (media->meter_l != NULL) gtk_level_bar_set_value(GTK_LEVEL_BAR(media->meter_l), media->peak_l);
    if (media->meter_r != NULL) gtk_level_bar_set_value(GTK_LEVEL_BAR(media->meter_r), media->peak_r);

    if (media->lbl_peak_l != NULL) {
        gchar *pl = g_strdup_printf("%d%%", (int)(media->peak_l * 100.0));
        gtk_label_set_text(GTK_LABEL(media->lbl_peak_l), pl);
        g_free(pl);

        gtk_widget_remove_css_class(media->lbl_peak_l, "peak-warn");
        gtk_widget_remove_css_class(media->lbl_peak_l, "peak-clip");
        if (media->peak_l >= 0.88) {
            gtk_widget_add_css_class(media->lbl_peak_l, "peak-clip");
        } else if (media->peak_l >= 0.72) {
            gtk_widget_add_css_class(media->lbl_peak_l, "peak-warn");
        }
    }

    if (media->lbl_peak_r != NULL) {
        gchar *pr = g_strdup_printf("%d%%", (int)(media->peak_r * 100.0));
        gtk_label_set_text(GTK_LABEL(media->lbl_peak_r), pr);
        g_free(pr);

        gtk_widget_remove_css_class(media->lbl_peak_r, "peak-warn");
        gtk_widget_remove_css_class(media->lbl_peak_r, "peak-clip");
        if (media->peak_r >= 0.88) {
            gtk_widget_add_css_class(media->lbl_peak_r, "peak-clip");
        } else if (media->peak_r >= 0.72) {
            gtk_widget_add_css_class(media->lbl_peak_r, "peak-warn");
        }
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
on_media_timer_tick(gpointer user_data)
{
    MediaWidget *media = user_data;
    refresh_media_state(media);
    return G_SOURCE_CONTINUE;
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellMediaPopover {
    GtkPopover parent_instance;

    GtkWidget *status_badge;
    GtkWidget *pop_art_img;
    GtkWidget *info_vbox;
    GtkWidget *pop_title_label;
    GtkWidget *pop_artist_label;
    GtkWidget *prev_btn;
    GtkWidget *play_btn;
    GtkWidget *next_btn;
    GtkWidget *open_player_btn;
    GtkWidget *meter_l;
    GtkWidget *meter_r;
    GtkWidget *lbl_peak_l;
    GtkWidget *lbl_peak_r;
} ShellMediaPopover;

typedef struct _ShellMediaPopoverClass {
    GtkPopoverClass parent_class;
} ShellMediaPopoverClass;

G_DEFINE_TYPE(ShellMediaPopover, shell_media_popover, GTK_TYPE_POPOVER)

static void
shell_media_popover_init(ShellMediaPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_media_popover_class_init(ShellMediaPopoverClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/media_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, status_badge);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, pop_art_img);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, info_vbox);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, pop_title_label);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, pop_artist_label);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, prev_btn);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, play_btn);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, next_btn);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, open_player_btn);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, meter_l);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, meter_r);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, lbl_peak_l);
    gtk_widget_class_bind_template_child(widget_class, ShellMediaPopover, lbl_peak_r);
}

static void
on_open_player_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    MediaWidget *media = user_data;
    if (media && media->mpris_proxy) {
        g_dbus_proxy_call(media->mpris_proxy, "org.mpris.MediaPlayer2.Raise", NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static GtkWidget *
build_media_popover(MediaWidget *media)
{
    ShellMediaPopover *popover = g_object_new(shell_media_popover_get_type(), NULL);

    media->pop_status_badge = popover->status_badge;
    media->pop_art_img = popover->pop_art_img;
    media->pop_title_label = popover->pop_title_label;
    media->pop_artist_label = popover->pop_artist_label;
    media->prev_btn = popover->prev_btn;
    media->play_btn = popover->play_btn;
    media->next_btn = popover->next_btn;
    media->meter_l = popover->meter_l;
    media->meter_r = popover->meter_r;
    media->lbl_peak_l = popover->lbl_peak_l;
    media->lbl_peak_r = popover->lbl_peak_r;

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_title_hover_enter), media);
    g_signal_connect(motion, "leave", G_CALLBACK(on_title_hover_leave), media);
    gtk_widget_add_controller(popover->info_vbox, motion);

    g_signal_connect(media->prev_btn, "clicked", G_CALLBACK(on_prev_clicked), media);
    g_signal_connect(media->play_btn, "clicked", G_CALLBACK(on_play_pause_clicked), media);
    g_signal_connect(media->next_btn, "clicked", G_CALLBACK(on_next_clicked), media);
    g_signal_connect(popover->open_player_btn, "clicked", G_CALLBACK(on_open_player_clicked), media);

    return GTK_WIDGET(popover);
}

static ShellWidget *
media_widget_create(ShellCompositor *compositor)
{
    MediaWidget *media = g_new0(MediaWidget, 1);
    media->base.klass = &media_widget_class;
    media->compositor = compositor;

    /* Build Panel Button */
    media->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(media->button), FALSE);
    gtk_widget_add_css_class(media->button, "shell-widget");
    gtk_widget_add_css_class(media->button, "shell-widget-media");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    media->icon_img = gtk_image_new_from_icon_name("fl-media-symbolic");
    media->label = gtk_label_new("Media");

    gtk_box_append(GTK_BOX(hbox), media->icon_img);
    gtk_box_append(GTK_BOX(hbox), media->label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(media->button), hbox);

    /* Build Popover */
    media->popover = build_media_popover(media);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(media->button), media->popover);

    refresh_media_state(media);
    media->timer_id = g_timeout_add_seconds(2, on_media_timer_tick, media);
    media->vu_timer_id = g_timeout_add(33, on_vu_meter_30fps_tick, media);

    return (ShellWidget *)media;
}

static void
media_widget_destroy(ShellWidget *widget)
{
    MediaWidget *media = (MediaWidget *)widget;
    if (media == NULL)
        return;

    if (media->timer_id != 0) {
        g_source_remove(media->timer_id);
        media->timer_id = 0;
    }

    if (media->vu_timer_id != 0) {
        g_source_remove(media->vu_timer_id);
        media->vu_timer_id = 0;
    }

    if (media->ticker_timer_id != 0) {
        g_source_remove(media->ticker_timer_id);
        media->ticker_timer_id = 0;
    }

    if (media->mpris_proxy != NULL) {
        g_object_unref(media->mpris_proxy);
        media->mpris_proxy = NULL;
    }

    g_free(media->artist);
    g_free(media->title);
    g_free(media->art_url);
    g_free(media->active_player_bus);
    g_free(media);
}

static GtkWidget *
media_widget_get_widget(ShellWidget *widget)
{
    MediaWidget *media = (MediaWidget *)widget;
    return media->button;
}

static void
media_widget_enable(ShellWidget *widget)
{
    MediaWidget *media = (MediaWidget *)widget;
    if (media) {
        shell_widget_apply_mode_visibility(widget->mode, media->icon_img, media->label);
    }
}

static void
media_widget_disable(ShellWidget *widget)
{
    (void)widget;
}

const ShellWidgetClass media_widget_class = {
    .id = "media",
    .name = "Media Player",
    .create = media_widget_create,
    .destroy = media_widget_destroy,
    .get_widget = media_widget_get_widget,
    .enable = media_widget_enable,
    .disable = media_widget_disable,
};
