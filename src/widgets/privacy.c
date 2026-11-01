#define _POSIX_C_SOURCE 200809L
#include "widget.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const ShellWidgetClass privacy_widget_class;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *popover;

    GtkWidget *mic_status_label;
    GtkWidget *cam_status_label;

    guint poll_timer_id;
    gboolean mic_active;
    gboolean cam_active;
} PrivacyWidget;

static void update_privacy_ui(PrivacyWidget *priv);

static gboolean
check_alsa_mic_active(void)
{
    GDir *asound = g_dir_open("/proc/asound", 0, NULL);
    if (!asound)
        return FALSE;

    gboolean active = FALSE;
    const gchar *card_entry = NULL;
    while (!active && (card_entry = g_dir_read_name(asound)) != NULL) {
        if (!g_str_has_prefix(card_entry, "card"))
            continue;
        gchar *card_path = g_build_filename("/proc/asound", card_entry, NULL);
        GDir *card_dir = g_dir_open(card_path, 0, NULL);
        if (card_dir) {
            const gchar *pcm_entry = NULL;
            while (!active && (pcm_entry = g_dir_read_name(card_dir)) != NULL) {
                /* Capture PCM streams end with 'c', e.g. pcm0c */
                if (!g_str_has_prefix(pcm_entry, "pcm") || !g_str_has_suffix(pcm_entry, "c"))
                    continue;
                gchar *status_path = g_build_filename(card_path, pcm_entry, "sub0", "status", NULL);
                gchar *contents = NULL;
                if (g_file_get_contents(status_path, &contents, NULL, NULL)) {
                    if (strstr(contents, "state: RUNNING") != NULL)
                        active = TRUE;
                    g_free(contents);
                }
                g_free(status_path);
            }
            g_dir_close(card_dir);
        }
        g_free(card_path);
    }
    g_dir_close(asound);
    return active;
}

static gboolean
check_v4l2_camera_active(void)
{
    GDir *v4l = g_dir_open("/sys/class/video4linux", 0, NULL);
    if (!v4l)
        return FALSE;
    const gchar *first_dev = g_dir_read_name(v4l);
    if (!first_dev) {
        g_dir_close(v4l);
        return FALSE;
    }
    g_dir_close(v4l);

    GDir *proc_dir = g_dir_open("/proc", 0, NULL);
    if (!proc_dir)
        return FALSE;

    gboolean active = FALSE;
    const gchar *pid_entry = NULL;
    while (!active && (pid_entry = g_dir_read_name(proc_dir)) != NULL) {
        if (!g_ascii_isdigit(pid_entry[0]))
            continue;
        gchar *fd_dir_path = g_build_filename("/proc", pid_entry, "fd", NULL);
        GDir *fd_dir = g_dir_open(fd_dir_path, 0, NULL);
        if (fd_dir) {
            const gchar *fd_entry = NULL;
            while (!active && (fd_entry = g_dir_read_name(fd_dir)) != NULL) {
                gchar *fd_path = g_build_filename(fd_dir_path, fd_entry, NULL);
                char target[256];
                ssize_t len = readlink(fd_path, target, sizeof(target) - 1);
                if (len > 0) {
                    target[len] = '\0';
                    if (g_str_has_prefix(target, "/dev/video"))
                        active = TRUE;
                }
                g_free(fd_path);
            }
            g_dir_close(fd_dir);
        }
        g_free(fd_dir_path);
    }
    g_dir_close(proc_dir);
    return active;
}

static gboolean
on_privacy_poll_tick(gpointer user_data)
{
    PrivacyWidget *priv = user_data;
    if (!priv)
        return G_SOURCE_REMOVE;

    gboolean mic = check_alsa_mic_active();
    gboolean cam = check_v4l2_camera_active();
    if (mic != priv->mic_active || cam != priv->cam_active) {
        priv->mic_active = mic;
        priv->cam_active = cam;
        update_privacy_ui(priv);
    }
    return G_SOURCE_CONTINUE;
}

static void
update_privacy_ui(PrivacyWidget *priv)
{
    if (!priv->mic_active && !priv->cam_active) {
        gtk_widget_set_visible(priv->button, FALSE);
        return;
    }

    gtk_widget_set_visible(priv->button, TRUE);
    const gchar *icon_name = "privacy-mic-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(priv->icon_img), icon_name);

    if (priv->mic_active && priv->cam_active) {
        gtk_label_set_text(GTK_LABEL(priv->label), "Mic + Cam");
    } else if (priv->mic_active) {
        gtk_label_set_text(GTK_LABEL(priv->label), "Mic Active");
    } else {
        gtk_label_set_text(GTK_LABEL(priv->label), "Cam Active");
    }
    shell_widget_apply_mode_visibility(priv->base.mode, priv->icon_img, priv->label);

    if (priv->mic_status_label != NULL) {
        gtk_label_set_text(GTK_LABEL(priv->mic_status_label),
                           priv->mic_active ? "Microphone: In Use" : "Microphone: Inactive");
    }
    if (priv->cam_status_label != NULL) {
        gtk_label_set_text(GTK_LABEL(priv->cam_status_label),
                           priv->cam_active ? "Camera: In Use" : "Camera: Inactive");
    }
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellPrivacyPopover {
    GtkPopover parent_instance;

    GtkWidget *mic_status_label;
    GtkWidget *cam_status_label;
} ShellPrivacyPopover;

typedef struct _ShellPrivacyPopoverClass {
    GtkPopoverClass parent_class;
} ShellPrivacyPopoverClass;

G_DEFINE_TYPE(ShellPrivacyPopover, shell_privacy_popover, GTK_TYPE_POPOVER)

static void
shell_privacy_popover_dispose(GObject *object)
{
    gtk_widget_dispose_template(GTK_WIDGET(object), shell_privacy_popover_get_type());
    G_OBJECT_CLASS(shell_privacy_popover_parent_class)->dispose(object);
}

static void
shell_privacy_popover_init(ShellPrivacyPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_privacy_popover_class_init(ShellPrivacyPopoverClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = shell_privacy_popover_dispose;
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/privacy_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellPrivacyPopover, mic_status_label);
    gtk_widget_class_bind_template_child(widget_class, ShellPrivacyPopover, cam_status_label);
}

static GtkWidget *
build_privacy_popover(PrivacyWidget *priv)
{
    ShellPrivacyPopover *popover = g_object_new(shell_privacy_popover_get_type(), NULL);

    priv->mic_status_label = popover->mic_status_label;
    priv->cam_status_label = popover->cam_status_label;

    return GTK_WIDGET(popover);
}

static ShellWidget *
privacy_widget_create(ShellCompositor *compositor)
{
    PrivacyWidget *priv = g_new0(PrivacyWidget, 1);
    priv->base.klass = &privacy_widget_class;
    priv->compositor = compositor;
    priv->mic_active = FALSE;
    priv->cam_active = FALSE;

    /* Build Button */
    priv->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(priv->button), FALSE);
    gtk_widget_add_css_class(priv->button, "shell-widget");
    gtk_widget_add_css_class(priv->button, "shell-widget-privacy");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    priv->icon_img = gtk_image_new_from_icon_name("fl-privacy-symbolic");
    priv->label = gtk_label_new("Privacy");

    gtk_box_append(GTK_BOX(hbox), priv->icon_img);
    gtk_box_append(GTK_BOX(hbox), priv->label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(priv->button), hbox);

    /* Build Popover */
    priv->popover = build_privacy_popover(priv);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(priv->button), priv->popover);

    update_privacy_ui(priv);

    return (ShellWidget *)priv;
}

static void
privacy_widget_destroy(ShellWidget *widget)
{
    PrivacyWidget *priv = (PrivacyWidget *)widget;
    if (priv == NULL)
        return;
    if (priv->poll_timer_id != 0) {
        g_source_remove(priv->poll_timer_id);
        priv->poll_timer_id = 0;
    }
    g_free(priv);
}

static GtkWidget *
privacy_widget_get_widget(ShellWidget *widget)
{
    PrivacyWidget *priv = (PrivacyWidget *)widget;
    return priv->button;
}

static void
privacy_widget_enable(ShellWidget *widget)
{
    PrivacyWidget *priv = (PrivacyWidget *)widget;
    if (priv) {
        if (priv->icon_img)
            gtk_image_set_pixel_size(GTK_IMAGE(priv->icon_img), shell_widget_get_icon_size(widget));
        shell_widget_apply_mode_visibility(widget->mode, priv->icon_img, priv->label);
        priv->mic_active = check_alsa_mic_active();
        priv->cam_active = check_v4l2_camera_active();
        update_privacy_ui(priv);
        if (priv->poll_timer_id == 0) {
            priv->poll_timer_id = g_timeout_add_seconds(3, on_privacy_poll_tick, priv);
        }
    }
}

static void
privacy_widget_disable(ShellWidget *widget)
{
    PrivacyWidget *priv = (PrivacyWidget *)widget;
    if (priv && priv->poll_timer_id != 0) {
        g_source_remove(priv->poll_timer_id);
        priv->poll_timer_id = 0;
    }
}

const ShellWidgetClass privacy_widget_class = {
    .id = "privacy",
    .name = "Privacy Indicators",
    .create = privacy_widget_create,
    .destroy = privacy_widget_destroy,
    .get_widget = privacy_widget_get_widget,
    .enable = privacy_widget_enable,
    .disable = privacy_widget_disable,
};
