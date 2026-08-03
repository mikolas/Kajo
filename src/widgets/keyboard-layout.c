#include <gtk/gtk.h>
#include "widget.h"
#include "../compositor/compositor.h"

#define MAX_LAYOUTS 16

typedef struct {
    ShellWidget      base;
    GtkWidget       *button;
    GtkWidget       *popover;
    GtkWidget       *panel_label;
    GtkWidget       *icon_img;
    GtkWidget       *layouts_box;
    GtkWidget       *radio_buttons[MAX_LAYOUTS];
    gint             num_radios;
    ShellCompositor *compositor;
    gboolean         updating;
} KeyboardLayoutWidget;

static void keyboard_layout_rebuild_radios(KeyboardLayoutWidget *kl);

static void keyboard_layout_update_panel(KeyboardLayoutWidget *kl)
{
    if (!kl->compositor) {
        gtk_label_set_text(GTK_LABEL(kl->panel_label), "??");
        return;
    }

    const CompositorKeyboardLayouts *layouts = shell_compositor_get_keyboard_layouts(kl->compositor);
    if (!layouts || !layouts->names || !layouts->names[layouts->current_idx]) {
        gtk_label_set_text(GTK_LABEL(kl->panel_label), "??");
        return;
    }

    const gchar *name = layouts->names[layouts->current_idx];

    /* Derive short name: first 2 chars uppercase */
    gchar short_name[3] = {0};
    if (name[0] != '\0') {
        short_name[0] = (gchar)g_ascii_toupper(name[0]);
        if (name[1] != '\0')
            short_name[1] = (gchar)g_ascii_toupper(name[1]);
    }

    gtk_label_set_text(GTK_LABEL(kl->panel_label), short_name);
    shell_widget_apply_mode_visibility(kl->base.mode, kl->icon_img, kl->panel_label);

    /* Update radio selection */
    if (kl->num_radios > 0 && layouts->current_idx < kl->num_radios) {
        kl->updating = TRUE;
        gtk_check_button_set_active(
            GTK_CHECK_BUTTON(kl->radio_buttons[layouts->current_idx]), TRUE);
        kl->updating = FALSE;
    }
}

static void on_radio_toggled(GtkCheckButton *btn, gpointer user_data)
{
    KeyboardLayoutWidget *kl = user_data;
    if (kl->updating)
        return;
    if (!gtk_check_button_get_active(btn))
        return;

    /* Find which index this radio is */
    for (gint i = 0; i < kl->num_radios; i++) {
        if (kl->radio_buttons[i] == (GtkWidget *)btn) {
            shell_compositor_switch_keyboard_layout_idx(kl->compositor, (guint8)i);
            break;
        }
    }
}

static void keyboard_layout_rebuild_radios(KeyboardLayoutWidget *kl)
{
    /* Remove existing children */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(kl->layouts_box)) != NULL)
        gtk_box_remove(GTK_BOX(kl->layouts_box), child);

    kl->num_radios = 0;
    memset(kl->radio_buttons, 0, sizeof(kl->radio_buttons));

    if (!kl->compositor)
        return;

    const CompositorKeyboardLayouts *layouts = shell_compositor_get_keyboard_layouts(kl->compositor);
    if (!layouts || !layouts->names)
        return;

    /* Header */
    GtkWidget *header = gtk_label_new("Keyboard Layout");
    gtk_label_set_xalign(GTK_LABEL(header), 0.0);
    gtk_widget_add_css_class(header, "heading");
    gtk_box_append(GTK_BOX(kl->layouts_box), header);

    GtkWidget *first_radio = NULL;
    for (gint i = 0; layouts->names[i] != NULL && i < MAX_LAYOUTS; i++) {
        GtkWidget *radio = gtk_check_button_new_with_label(layouts->names[i]);
        if (first_radio)
            gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(first_radio));
        else
            first_radio = radio;

        g_signal_connect(radio, "toggled", G_CALLBACK(on_radio_toggled), kl);
        gtk_box_append(GTK_BOX(kl->layouts_box), radio);
        kl->radio_buttons[i] = radio;
        kl->num_radios++;
    }

    /* Select current */
    if (layouts->current_idx < kl->num_radios) {
        kl->updating = TRUE;
        gtk_check_button_set_active(
            GTK_CHECK_BUTTON(kl->radio_buttons[layouts->current_idx]), TRUE);
        kl->updating = FALSE;
    }
}

static void on_keyboard_layout_changed(ShellCompositor *compositor G_GNUC_UNUSED, gpointer userdata)
{
    KeyboardLayoutWidget *kl = userdata;

    /* Check if layouts list changed (rebuild radios) */
    const CompositorKeyboardLayouts *layouts = shell_compositor_get_keyboard_layouts(kl->compositor);
    if (layouts && layouts->names) {
        gint count = 0;
        while (layouts->names[count]) count++;
        if (count != kl->num_radios)
            keyboard_layout_rebuild_radios(kl);
    }

    keyboard_layout_update_panel(kl);
}

/* --- Widget interface --- */

static ShellWidget *keyboard_layout_create(ShellCompositor *compositor)
{
    KeyboardLayoutWidget *kl = g_new0(KeyboardLayoutWidget, 1);
    kl->compositor = compositor;

    /* Panel button child hbox */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    kl->icon_img = gtk_image_new_from_icon_name("fl-keyboard-symbolic");
    kl->panel_label = gtk_label_new("??");
    gtk_box_append(GTK_BOX(hbox), kl->icon_img);
    gtk_box_append(GTK_BOX(hbox), kl->panel_label);

    /* Menu button */
    kl->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(kl->button), FALSE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(kl->button), hbox);
    gtk_widget_add_css_class(kl->button, "flat");
    gtk_widget_add_css_class(kl->button, "shell-widget");
    gtk_widget_add_css_class(kl->button, "shell-widget-keyboard-layout");

    /* Popover content */
    kl->layouts_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(kl->layouts_box, 12);
    gtk_widget_set_margin_end(kl->layouts_box, 12);
    gtk_widget_set_margin_top(kl->layouts_box, 12);
    gtk_widget_set_margin_bottom(kl->layouts_box, 12);

    /* Popover */
    kl->popover = gtk_popover_new();
    gtk_widget_add_css_class(kl->popover, "shell-popover");
    gtk_popover_set_child(GTK_POPOVER(kl->popover), kl->layouts_box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(kl->button), kl->popover);

    return (ShellWidget *)kl;
}

static void keyboard_layout_destroy(ShellWidget *widget)
{
    KeyboardLayoutWidget *kl = (KeyboardLayoutWidget *)widget;
    if (kl) {
        if (kl->compositor) {
            shell_compositor_remove_callbacks(kl->compositor, kl);
        }
        g_free(kl);
    }
}

static GtkWidget *keyboard_layout_get_widget(ShellWidget *widget)
{
    return ((KeyboardLayoutWidget *)widget)->button;
}

static void keyboard_layout_enable(ShellWidget *widget)
{
    KeyboardLayoutWidget *kl = (KeyboardLayoutWidget *)widget;
    if (kl) {
        shell_widget_apply_mode_visibility(widget->mode, kl->icon_img, kl->panel_label);
    }

    if (kl->compositor) {
        shell_compositor_on_keyboard_layout_changed(kl->compositor, on_keyboard_layout_changed, kl);
        keyboard_layout_rebuild_radios(kl);
        keyboard_layout_update_panel(kl);
    }
}

static void keyboard_layout_disable(ShellWidget *widget)
{
    (void)widget;
    /* Callbacks remain registered; cleanup in destroy */
}

const ShellWidgetClass keyboard_layout_widget_class = {
    .id         = "keyboard-layout",
    .name       = "Keyboard Layout",
    .create     = keyboard_layout_create,
    .destroy    = keyboard_layout_destroy,
    .get_widget = keyboard_layout_get_widget,
    .enable     = keyboard_layout_enable,
    .disable    = keyboard_layout_disable,
};
