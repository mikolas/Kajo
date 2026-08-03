#include "widget.h"
#include "../theme.h"
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const ShellWidgetClass theme_widget_class;

typedef struct {
    const char *id;
    const char *name;
    const char *accent;
    const char *hover;
} AccentPreset;

static const AccentPreset ACCENTS[] = {
    { "cobalt-blue",  "Cobalt Blue",        "#0078d4", "#1084d8" },
    { "purple",       "Windows Purple",     "#6b69d6", "#7a78e0" },
    { "catppuccin",   "Catppuccin Lavender", "#cba6f7", "#ddb6f2" },
    { "nord-ice",     "Nord Frost Ice",     "#88c0d0", "#99d1e1" },
    { "emerald",      "Emerald Green",      "#107c41", "#138c4a" },
    { "crimson",      "Metro Crimson",      "#d13438", "#e04347" },
    { "amber",        "Amber Gold",         "#c19c00", "#d4ab00" },
    { "cyan",         "Cyberpunk Cyan",     "#008272", "#009684" }
};

#define N_ACCENTS (sizeof(ACCENTS) / sizeof(ACCENTS[0]))

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *popover;
    GtkWidget *preset_grid;
} ThemeWidget;

/* ─── Write Pure Accent Overrides (2 Lines Only) ─── */

static void apply_accent_preset(const AccentPreset *preset)
{
    const gchar *xdg_config = g_get_user_config_dir();
    gchar *styled_dir = g_build_filename(xdg_config, "kajo", "style.d", NULL);
    g_mkdir_with_parents(styled_dir, 0755);

    gchar *theme_css_path = g_build_filename(styled_dir, "00-accent.css", NULL);
    g_free(styled_dir);

    GString *css = g_string_new("/* Auto-generated accent override by kajo */\n");
    g_string_append_printf(css, "@define-color metro-accent %s;\n", preset->accent);
    g_string_append_printf(css, "@define-color metro-accent-hover %s;\n", preset->hover);

    g_file_set_contents(theme_css_path, css->str, css->len, NULL);
    g_string_free(css, TRUE);

    g_message("Applied Metro accent color '%s' -> '%s'", preset->name, theme_css_path);
    g_free(theme_css_path);
}

static void on_accent_button_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    const AccentPreset *preset = user_data;
    apply_accent_preset(preset);
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellThemePopover {
    GtkPopover parent_instance;

    GtkWidget *preset_grid;
    GtkWidget *status_badge;
} ShellThemePopover;

typedef struct _ShellThemePopoverClass {
    GtkPopoverClass parent_class;
} ShellThemePopoverClass;

G_DEFINE_TYPE(ShellThemePopover, shell_theme_popover, GTK_TYPE_POPOVER)

static void
shell_theme_popover_init(ShellThemePopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_theme_popover_class_init(ShellThemePopoverClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/theme_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellThemePopover, preset_grid);
    gtk_widget_class_bind_template_child(widget_class, ShellThemePopover, status_badge);
}

/* ─── Widget Interface ─── */

static ShellWidget *
theme_widget_create(ShellCompositor *compositor G_GNUC_UNUSED)
{
    ThemeWidget *tw = g_new0(ThemeWidget, 1);

    tw->button = gtk_menu_button_new();
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(tw->button), FALSE);
    gtk_widget_add_css_class(tw->button, "shell-widget");
    gtk_widget_add_css_class(tw->button, "shell-widget-theme");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tw->icon_img = gtk_image_new_from_icon_name("fl-color-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(tw->icon_img), shell_widget_get_icon_size((ShellWidget *)tw));
    tw->label = gtk_label_new("Accent");

    gtk_box_append(GTK_BOX(hbox), tw->icon_img);
    gtk_box_append(GTK_BOX(hbox), tw->label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(tw->button), hbox);

    shell_widget_apply_mode_visibility(tw->base.mode, tw->icon_img, tw->label);

    /* Build Popover */
    ShellThemePopover *popover = g_object_new(shell_theme_popover_get_type(), NULL);
    tw->popover = GTK_WIDGET(popover);
    tw->preset_grid = popover->preset_grid;

    /* Populate 8 Accent Color Swatches in 2-Column Grid */
    for (size_t i = 0; i < N_ACCENTS; i++) {
        GtkWidget *card = gtk_button_new();
        gtk_widget_add_css_class(card, "section-card");
        gtk_widget_set_hexpand(card, TRUE);

        GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        /* Accent Color Circle Swatch */
        GtkWidget *swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(swatch, 18, 18);
        gchar *swatch_class = g_strdup_printf("swatch-%zu", i);
        gtk_widget_add_css_class(swatch, swatch_class);

        gchar *swatch_css = g_strdup_printf(".%s { background-color: %s; border-radius: 9px; }", swatch_class, ACCENTS[i].accent);
        GtkCssProvider *p = gtk_css_provider_new();
        gtk_css_provider_load_from_string(p, swatch_css);
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_free(swatch_class);
        g_free(swatch_css);
        g_object_unref(p);

        GtkWidget *name_label = gtk_label_new(ACCENTS[i].name);
        gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
        gtk_widget_set_hexpand(name_label, TRUE);

        gtk_box_append(GTK_BOX(card_box), swatch);
        gtk_box_append(GTK_BOX(card_box), name_label);
        gtk_button_set_child(GTK_BUTTON(card), card_box);

        g_signal_connect(card, "clicked", G_CALLBACK(on_accent_button_clicked), (gpointer)&ACCENTS[i]);

        int col = i % 2;
        int row = i / 2;
        gtk_grid_attach(GTK_GRID(tw->preset_grid), card, col, row, 1, 1);
    }

    gtk_menu_button_set_popover(GTK_MENU_BUTTON(tw->button), tw->popover);

    return (ShellWidget *)tw;
}

static void
theme_widget_destroy(ShellWidget *widget)
{
    ThemeWidget *tw = (ThemeWidget *)widget;
    if (tw) g_free(tw);
}

static GtkWidget *
theme_widget_get_widget(ShellWidget *widget)
{
    return ((ThemeWidget *)widget)->button;
}

static void
theme_widget_enable(ShellWidget *widget)
{
    ThemeWidget *tw = (ThemeWidget *)widget;
    if (tw && tw->button) gtk_widget_set_visible(tw->button, TRUE);
}

static void
theme_widget_disable(ShellWidget *widget)
{
    ThemeWidget *tw = (ThemeWidget *)widget;
    if (tw && tw->button) gtk_widget_set_visible(tw->button, FALSE);
}

const ShellWidgetClass theme_widget_class = {
    .id = "theme",
    .name = "Theme",
    .create = theme_widget_create,
    .destroy = theme_widget_destroy,
    .get_widget = theme_widget_get_widget,
    .enable = theme_widget_enable,
    .disable = theme_widget_disable,
};
