#include "osd.h"
#include <gtk4-layer-shell.h>
#include <stdio.h>

struct _ShellOSD {
    GtkWindow *window;
    GtkWidget *card_box;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *level_bar;
    guint      timer_id;
};

static gboolean
on_osd_hide_timeout(gpointer user_data)
{
    ShellOSD *osd = user_data;
    if (osd->window) {
        gtk_widget_set_visible(GTK_WIDGET(osd->window), FALSE);
    }
    osd->timer_id = 0;
    return G_SOURCE_REMOVE;
}

/* ─── Declarative GtkWindow Template Subclass ─── */

typedef struct _ShellOSDWindow {
    GtkWindow parent_instance;

    GtkWidget *card_box;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *level_bar;
} ShellOSDWindow;

typedef struct _ShellOSDWindowClass {
    GtkWindowClass parent_class;
} ShellOSDWindowClass;

G_DEFINE_TYPE(ShellOSDWindow, shell_osd_window, GTK_TYPE_WINDOW)

static void
shell_osd_window_init(ShellOSDWindow *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_osd_window_class_init(ShellOSDWindowClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/osd_window.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellOSDWindow, card_box);
    gtk_widget_class_bind_template_child(widget_class, ShellOSDWindow, icon_img);
    gtk_widget_class_bind_template_child(widget_class, ShellOSDWindow, label);
    gtk_widget_class_bind_template_child(widget_class, ShellOSDWindow, level_bar);
}

ShellOSD *
shell_osd_new(void)
{
    ShellOSD *osd = g_new0(ShellOSD, 1);

    ShellOSDWindow *win = g_object_new(shell_osd_window_get_type(), NULL);
    osd->window = GTK_WINDOW(win);
    osd->card_box = win->card_box;
    osd->icon_img = win->icon_img;
    osd->label = win->label;
    osd->level_bar = win->level_bar;

    /* Wayland Layer Shell Overlay setup */
    gtk_layer_init_for_window(osd->window);
    gtk_layer_set_layer(osd->window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(osd->window, "kajo-osd");
    gtk_layer_set_anchor(osd->window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_margin(osd->window, GTK_LAYER_SHELL_EDGE_BOTTOM, 64);
    gtk_layer_set_exclusive_zone(osd->window, 0);

    gtk_widget_set_visible(GTK_WIDGET(osd->window), FALSE);

    return osd;
}

void
shell_osd_show_volume(ShellOSD *osd, gint percent, gboolean muted)
{
    if (!osd) return;

    if (osd->timer_id != 0) {
        g_source_remove(osd->timer_id);
    }

    const gchar *icon_name = muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(osd->icon_img), icon_name);

    if (muted) {
        gtk_label_set_text(GTK_LABEL(osd->label), "Muted");
        gtk_level_bar_set_value(GTK_LEVEL_BAR(osd->level_bar), 0.0);
    } else {
        gchar buf[32];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        gtk_label_set_text(GTK_LABEL(osd->label), buf);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(osd->level_bar), (double)percent / 100.0);
    }

    gtk_widget_set_visible(GTK_WIDGET(osd->window), TRUE);
    gtk_window_present(osd->window);

    osd->timer_id = g_timeout_add(1500, on_osd_hide_timeout, osd);
}

void
shell_osd_show_brightness(ShellOSD *osd, gint percent)
{
    if (!osd) return;

    if (osd->timer_id != 0) {
        g_source_remove(osd->timer_id);
    }

    gtk_image_set_from_icon_name(GTK_IMAGE(osd->icon_img), "display-brightness-symbolic");

    gchar buf[32];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    gtk_label_set_text(GTK_LABEL(osd->label), buf);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(osd->level_bar), (double)percent / 100.0);

    gtk_widget_set_visible(GTK_WIDGET(osd->window), TRUE);
    gtk_window_present(osd->window);

    osd->timer_id = g_timeout_add(1500, on_osd_hide_timeout, osd);
}

void
shell_osd_destroy(ShellOSD *osd)
{
    if (!osd) return;

    if (osd->timer_id != 0) {
        g_source_remove(osd->timer_id);
        osd->timer_id = 0;
    }

    if (osd->window) {
        gtk_window_destroy(osd->window);
    }

    g_free(osd);
}
