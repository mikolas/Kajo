#include "widget.h"
#include "../theme.h"
#include "../config.h"
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const ShellWidgetClass theme_widget_class;

#define ACCENTS shell_accent_presets
#define N_ACCENTS SHELL_N_ACCENT_PRESETS
typedef ShellAccentPreset AccentPreset;

typedef struct {
    ShellWidget base;
    ShellCompositor *compositor;
    GtkWidget *button;
    GtkWidget *icon_img;
    GtkWidget *label;
    GtkWidget *popover;
    GtkWidget *preset_grid;
    GtkWidget *status_badge;
    GtkWidget *settings_btn;
    GtkWidget *swatch_cards[N_ACCENTS];
    GtkCssProvider *swatch_providers[N_ACCENTS];
} ThemeWidget;

static void
sync_theme_popover_active(ThemeWidget *tw, const gchar *active_hex)
{
    if (!tw)
        return;

    g_autofree gchar *loaded_hex = NULL;
    if (!active_hex || !*active_hex) {
        g_autoptr(KajoThemeCfgRef) cfg = kajo_config_store_acquire_theme(NULL);
        loaded_hex = g_strdup((cfg && cfg->accent_color) ? cfg->accent_color : "#0078d4");
        active_hex = loaded_hex;
    }

    if (tw->status_badge) {
        g_autofree gchar *upper = g_ascii_strup(active_hex, -1);
        g_autofree gchar *badge = g_strdup_printf("[ %s ]", upper);
        gtk_label_set_text(GTK_LABEL(tw->status_badge), badge);
        gtk_widget_add_css_class(tw->status_badge, "active");
    }

    for (size_t i = 0; i < N_ACCENTS; i++) {
        if (!tw->swatch_cards[i])
            continue;
        if (g_ascii_strcasecmp(ACCENTS[i].accent, active_hex) == 0)
            gtk_widget_add_css_class(tw->swatch_cards[i], "active");
        else
            gtk_widget_remove_css_class(tw->swatch_cards[i], "active");
    }
}

/* ─── Write Pure Accent Overrides ─── */

static void
mutate_theme_accent(KajoThemeCfg *draft, gpointer user_data)
{
    const gchar *accent = (const gchar *)user_data;
    gchar *dup = g_strdup(accent);
    g_free(draft->accent_color);
    draft->accent_color = dup;
}

static void apply_accent_preset(const AccentPreset *preset)
{
    g_autoptr(KajoThemeCfgRef) cfg = kajo_config_store_acquire_theme(NULL);
    gint body_px    = cfg ? cfg->ui_font_size_px : 14;
    gint heading_px = cfg ? cfg->font_heading_size_px : 13;
    gint sub_px     = cfg ? cfg->font_subtitle_size_px : 12;
    gint badge_px   = cfg ? cfg->font_badge_size_px : 11;

    shell_theme_write_override_css(preset->accent, preset->hover, body_px, heading_px, sub_px, badge_px);
    kajo_config_store_update_theme(NULL, mutate_theme_accent, (gpointer)preset->accent, NULL);

    g_message("Applied Metro accent color '%s' (%s)", preset->name, preset->accent);
}

static void on_accent_button_clicked(GtkButton *btn, gpointer user_data)
{
    ThemeWidget *tw = user_data;
    const AccentPreset *preset = g_object_get_data(G_OBJECT(btn), "accent-preset");
    if (!preset)
        return;
    apply_accent_preset(preset);
    sync_theme_popover_active(tw, preset->accent);
}

static void on_theme_popover_map(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    sync_theme_popover_active((ThemeWidget *)user_data, NULL);
}

static void on_theme_settings_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    ThemeWidget *tw = user_data;
    shell_spawn_settings("theme", tw ? tw->popover : NULL);
}

/* ─── Declarative GtkPopover Template Subclass ─── */

typedef struct _ShellThemePopover {
    GtkPopover parent_instance;

    GtkWidget *preset_grid;
    GtkWidget *status_badge;
    GtkWidget *settings_btn;
} ShellThemePopover;

typedef struct _ShellThemePopoverClass {
    GtkPopoverClass parent_class;
} ShellThemePopoverClass;

G_DEFINE_TYPE(ShellThemePopover, shell_theme_popover, GTK_TYPE_POPOVER)

static void
shell_theme_popover_dispose(GObject *object)
{
    gtk_widget_dispose_template(GTK_WIDGET(object), shell_theme_popover_get_type());
    G_OBJECT_CLASS(shell_theme_popover_parent_class)->dispose(object);
}

static void
shell_theme_popover_init(ShellThemePopover *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
shell_theme_popover_class_init(ShellThemePopoverClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = shell_theme_popover_dispose;
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/theme_popover.ui");

    gtk_widget_class_bind_template_child(widget_class, ShellThemePopover, preset_grid);
    gtk_widget_class_bind_template_child(widget_class, ShellThemePopover, status_badge);
    gtk_widget_class_bind_template_child(widget_class, ShellThemePopover, settings_btn);
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

    tw->icon_img = gtk_image_new_from_icon_name("fl-color-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(tw->icon_img), shell_widget_get_icon_size((ShellWidget *)tw));
    tw->label = gtk_label_new("Accent");
    GtkWidget *hbox = shell_widget_pack_bar_box(tw->icon_img, tw->label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(tw->button), hbox);

    shell_widget_apply_mode_visibility(tw->base.mode, tw->icon_img, tw->label);

    /* Build Popover */
    ShellThemePopover *popover = g_object_new(shell_theme_popover_get_type(), NULL);
    tw->popover = GTK_WIDGET(popover);
    tw->preset_grid = popover->preset_grid;
    tw->status_badge = popover->status_badge;
    tw->settings_btn = popover->settings_btn;
    g_signal_connect(tw->popover, "map", G_CALLBACK(on_theme_popover_map), tw);
    g_signal_connect(tw->settings_btn, "clicked", G_CALLBACK(on_theme_settings_clicked), tw);

    /* Populate 8 Accent Color Swatches in 2-Column Grid */
    for (size_t i = 0; i < N_ACCENTS; i++) {
        GtkWidget *card = gtk_button_new();
        gtk_widget_add_css_class(card, "widget-row-card");
        gtk_widget_set_hexpand(card, TRUE);

        GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(card_box, GTK_ALIGN_CENTER);

        /* Accent Color Square Swatch */
        GtkWidget *swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(swatch, 18, 18);
        gtk_widget_set_valign(swatch, GTK_ALIGN_CENTER);
        gchar *swatch_class = g_strdup_printf("theme-popover-swatch-%zu", i);
        gtk_widget_add_css_class(swatch, swatch_class);

        gchar *swatch_css = g_strdup_printf(".%s { background-color: %s; border: 1px solid rgba(255,255,255,0.25); border-radius: 0px; }", swatch_class, ACCENTS[i].accent);
        GtkCssProvider *p = gtk_css_provider_new();
        gtk_css_provider_load_from_string(p, swatch_css);
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_free(swatch_class);
        g_free(swatch_css);
        tw->swatch_providers[i] = p;

        GtkWidget *name_label = gtk_label_new(ACCENTS[i].name);
        gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
        gtk_widget_set_hexpand(name_label, TRUE);
        gtk_widget_set_valign(name_label, GTK_ALIGN_CENTER);

        gtk_box_append(GTK_BOX(card_box), swatch);
        gtk_box_append(GTK_BOX(card_box), name_label);
        gtk_button_set_child(GTK_BUTTON(card), card_box);

        g_object_set_data(G_OBJECT(card), "accent-preset", (gpointer)&ACCENTS[i]);
        g_signal_connect(card, "clicked", G_CALLBACK(on_accent_button_clicked), tw);
        tw->swatch_cards[i] = card;

        int col = i % 2;
        int row = i / 2;
        gtk_grid_attach(GTK_GRID(tw->preset_grid), card, col, row, 1, 1);
    }

    sync_theme_popover_active(tw, NULL);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(tw->button), tw->popover);

    return (ShellWidget *)tw;
}

static void
theme_widget_destroy(ShellWidget *widget)
{
    ThemeWidget *tw = (ThemeWidget *)widget;
    if (tw) {
        if (tw->popover)
            g_signal_handlers_disconnect_by_data(tw->popover, tw);
        if (tw->settings_btn)
            g_signal_handlers_disconnect_by_data(tw->settings_btn, tw);
        GdkDisplay *display = gdk_display_get_default();
        for (size_t i = 0; i < N_ACCENTS; i++) {
            if (tw->swatch_cards[i])
                g_signal_handlers_disconnect_by_data(tw->swatch_cards[i], tw);
            if (tw->swatch_providers[i]) {
                if (display) {
                    gtk_style_context_remove_provider_for_display(
                        display, GTK_STYLE_PROVIDER(tw->swatch_providers[i]));
                }
                g_clear_object(&tw->swatch_providers[i]);
            }
        }
        g_free(tw);
    }
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
    if (tw) {
        if (tw->icon_img)
            gtk_image_set_pixel_size(GTK_IMAGE(tw->icon_img), shell_widget_get_icon_size(widget));
        shell_widget_apply_mode_visibility(widget->mode, tw->icon_img, tw->label);
        if (tw->button)
            gtk_widget_set_visible(tw->button, TRUE);
    }
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
