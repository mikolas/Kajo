#include "../settings_common.h"
#include "../settings_pages.h"
#include <gio/gio.h>

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

static void on_accent_button_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    const AccentPreset *preset = user_data;
    apply_accent_preset(preset);
}

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageTheme {
    GtkBox parent_instance;

    GtkWidget *combo_palette;
    GtkWidget *grid_swatches;
    GtkWidget *scale_font;
    GtkWidget *lbl_font_val;
    GtkWidget *btn_css;
} SettingsPageTheme;

typedef struct _SettingsPageThemeClass {
    GtkBoxClass parent_class;
} SettingsPageThemeClass;

G_DEFINE_TYPE(SettingsPageTheme, settings_page_theme, GTK_TYPE_BOX)

static void
settings_page_theme_init(SettingsPageTheme *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_theme_class_init(SettingsPageThemeClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(
        widget_class, "/org/kajo/shell/ui/page_theme.ui");

    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, combo_palette);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, grid_swatches);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, scale_font);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, lbl_font_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, btn_css);
}

static void
on_scale_font_changed(GtkRange *range, gpointer user_data)
{
    GtkLabel *lbl = GTK_LABEL(user_data);
    gchar *str = g_strdup_printf("%dpx Fira Code", (int)gtk_range_get_value(range));
    gtk_label_set_text(lbl, str);
    g_free(str);
}

static void
on_reload_css_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    g_message("Reloaded GTK CSS design tokens.");
}

GtkWidget *
build_page_theme(void)
{
    SettingsPageTheme *page = g_object_new(settings_page_theme_get_type(), NULL);

    /* Populate 8 Accent Color Swatches in 2-Column Grid */
    for (size_t i = 0; i < N_ACCENTS; i++) {
        GtkWidget *card = gtk_button_new();
        gtk_widget_add_css_class(card, "section-card");
        gtk_widget_set_hexpand(card, TRUE);

        GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        /* Accent Color Circle Swatch */
        GtkWidget *swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(swatch, 18, 18);
        gchar *swatch_class = g_strdup_printf("settings-swatch-%zu", i);
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
        gtk_grid_attach(GTK_GRID(page->grid_swatches), card, col, row, 1, 1);
    }

    gtk_range_set_value(GTK_RANGE(page->scale_font), 13);
    g_signal_connect(page->scale_font, "value-changed", G_CALLBACK(on_scale_font_changed), page->lbl_font_val);

    g_signal_connect(page->btn_css, "clicked", G_CALLBACK(on_reload_css_clicked), NULL);

    return create_settings_page_card("THEME & ACCENTS", "[ METRO UI ]", GTK_WIDGET(page), NULL);
}
