#include "widget.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

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

    gboolean mic_active;
    gboolean cam_active;
} PrivacyWidget;

static void update_privacy_ui(PrivacyWidget *priv);

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
shell_privacy_popover_init(ShellPrivacyPopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_privacy_popover_class_init(ShellPrivacyPopoverClass *klass)
{
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
        shell_widget_apply_mode_visibility(widget->mode, priv->icon_img, priv->label);
    }
}

static void
privacy_widget_disable(ShellWidget *widget)
{
    (void)widget;
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
