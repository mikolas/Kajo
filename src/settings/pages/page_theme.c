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

/* ─── Declarative GtkBox Template Subclass ─── */

typedef struct _SettingsPageTheme {
    GtkBox parent_instance;

    GtkWidget *combo_palette;
    GtkWidget *grid_swatches;
    GtkWidget *scale_font;
    GtkWidget *lbl_font_val;
    GtkWidget *btn_css;

    GtkWidget *swatch_buttons[N_ACCENTS];
    guint current_preset_idx;
    gboolean syncing_ui;
} SettingsPageTheme;

typedef struct _SettingsPageThemeClass {
    GtkBoxClass parent_class;
} SettingsPageThemeClass;

G_DEFINE_TYPE(SettingsPageTheme, settings_page_theme, GTK_TYPE_BOX)

static void
write_theme_override_and_config(SettingsPageTheme *self)
{
    if (!self) return;

    guint idx = self->current_preset_idx < N_ACCENTS ? self->current_preset_idx : 0;
    const AccentPreset *preset = &ACCENTS[idx];
    int font_px = (int)gtk_range_get_value(GTK_RANGE(self->scale_font));
    if (font_px < 11 || font_px > 20) font_px = 13;

    const gchar *xdg_config = g_get_user_config_dir();
    g_autofree gchar *styled_dir = g_build_filename(xdg_config, "kajo", "style.d", NULL);
    g_mkdir_with_parents(styled_dir, 0755);

    g_autofree gchar *theme_css_path = g_build_filename(styled_dir, "00-accent.css", NULL);

    GString *css = g_string_new("/* Auto-generated accent override by kajo */\n");
    g_string_append_printf(css, "@define-color metro-accent %s;\n", preset->accent);
    g_string_append_printf(css, "@define-color metro-accent-hover %s;\n", preset->hover);
    g_string_append_printf(css,
        "window.shell-settings-window, window.shell-panel, .shell-popover {\n"
        "    font-size: %dpx;\n"
        "}\n",
        font_px);

    g_file_set_contents(theme_css_path, css->str, css->len, NULL);
    g_string_free(css, TRUE);

    ShellConfig *config = shell_config_load(NULL);
    if (config) {
        g_free(config->accent_color);
        config->accent_color = g_strdup(preset->accent);
        config->ui_font_size_px = font_px;

        g_autofree gchar *cfg_path = g_build_filename(xdg_config, "kajo", "config.json", NULL);
        shell_config_save(config, cfg_path);
        shell_config_destroy(config);
    }

    g_message("Applied Metro accent '%s' (%s, %dpx) -> '%s'",
              preset->name, preset->accent, font_px, theme_css_path);
}

static void
select_accent_preset(SettingsPageTheme *self, guint idx, gboolean apply_now)
{
    if (!self || idx >= N_ACCENTS) return;

    self->syncing_ui = TRUE;
    self->current_preset_idx = idx;

    if (gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_palette)) != idx) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_palette), idx);
    }

    for (size_t i = 0; i < N_ACCENTS; i++) {
        if (!self->swatch_buttons[i]) continue;
        if (i == idx) {
            gtk_widget_add_css_class(self->swatch_buttons[i], "active");
        } else {
            gtk_widget_remove_css_class(self->swatch_buttons[i], "active");
        }
    }
    self->syncing_ui = FALSE;

    if (apply_now) {
        write_theme_override_and_config(self);
    }
}

static void
on_accent_button_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    gpointer idx_ptr = g_object_get_data(G_OBJECT(btn), "preset-index");
    guint idx = GPOINTER_TO_UINT(idx_ptr);
    select_accent_preset(self, idx, TRUE);
}

static void
on_combo_palette_changed(GtkDropDown *dropdown, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    if (self->syncing_ui) return;
    guint idx = gtk_drop_down_get_selected(dropdown);
    if (idx < N_ACCENTS) {
        select_accent_preset(self, idx, TRUE);
    }
}

static void
settings_page_theme_dispose(GObject *object)
{
    gtk_widget_dispose_template(GTK_WIDGET(object), settings_page_theme_get_type());
    G_OBJECT_CLASS(settings_page_theme_parent_class)->dispose(object);
}

static void
settings_page_theme_init(SettingsPageTheme *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

static void
settings_page_theme_class_init(SettingsPageThemeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = settings_page_theme_dispose;

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
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    g_autofree gchar *str = g_strdup_printf("%dpx Fira Code", (int)gtk_range_get_value(range));
    gtk_label_set_text(GTK_LABEL(self->lbl_font_val), str);
    if (!self->syncing_ui) {
        write_theme_override_and_config(self);
    }
}

static void
on_reload_css_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    write_theme_override_and_config(self);
    settings_send_ipc_command("reload-theme");
    g_message("Reloaded GTK CSS design tokens.");
}

GtkWidget *
build_page_theme(void)
{
    SettingsPageTheme *page = g_object_new(settings_page_theme_get_type(), NULL);
    ShellConfig *config = shell_config_load(NULL);

    /* Populate palette dropdown */
    const char *palette_names[N_ACCENTS + 1];
    for (size_t i = 0; i < N_ACCENTS; i++) {
        palette_names[i] = ACCENTS[i].name;
    }
    palette_names[N_ACCENTS] = NULL;

    page->syncing_ui = TRUE;
    GtkStringList *palette_model = gtk_string_list_new(palette_names);
    gtk_drop_down_set_model(GTK_DROP_DOWN(page->combo_palette), G_LIST_MODEL(palette_model));
    g_object_unref(palette_model);

    /* Determine initial active preset from 00-accent.css or config */
    guint initial_idx = 0;
    g_autofree gchar *accent_css_path = g_build_filename(
        g_get_user_config_dir(), "kajo", "style.d", "00-accent.css", NULL);
    g_autofree gchar *css_contents = NULL;
    if (g_file_get_contents(accent_css_path, &css_contents, NULL, NULL) && css_contents) {
        for (size_t i = 0; i < N_ACCENTS; i++) {
            if (strstr(css_contents, ACCENTS[i].accent) != NULL) {
                initial_idx = (guint)i;
                break;
            }
        }
    } else if (config && config->accent_color) {
        for (size_t i = 0; i < N_ACCENTS; i++) {
            if (g_ascii_strcasecmp(config->accent_color, ACCENTS[i].accent) == 0) {
                initial_idx = (guint)i;
                break;
            }
        }
    }

    /* Populate 8 Accent Color Swatches in 2-Column Grid */
    for (size_t i = 0; i < N_ACCENTS; i++) {
        GtkWidget *card = gtk_button_new();
        gtk_widget_add_css_class(card, "section-card");
        gtk_widget_set_hexpand(card, TRUE);
        g_object_set_data(G_OBJECT(card), "preset-index", GUINT_TO_POINTER((guint)i));
        page->swatch_buttons[i] = card;

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

        g_signal_connect(card, "clicked", G_CALLBACK(on_accent_button_clicked), page);

        int col = i % 2;
        int row = i / 2;
        gtk_grid_attach(GTK_GRID(page->grid_swatches), card, col, row, 1, 1);
    }

    int init_font = (config && config->ui_font_size_px >= 11 && config->ui_font_size_px <= 15)
                        ? config->ui_font_size_px
                        : 13;
    gtk_range_set_value(GTK_RANGE(page->scale_font), init_font);
    g_autofree gchar *font_str = g_strdup_printf("%dpx Fira Code", init_font);
    gtk_label_set_text(GTK_LABEL(page->lbl_font_val), font_str);

    select_accent_preset(page, initial_idx, FALSE);
    page->syncing_ui = FALSE;

    if (config) shell_config_destroy(config);

    g_signal_connect(page->combo_palette, "notify::selected", G_CALLBACK(on_combo_palette_changed), page);
    g_signal_connect(page->scale_font, "value-changed", G_CALLBACK(on_scale_font_changed), page);
    g_signal_connect(page->btn_css, "clicked", G_CALLBACK(on_reload_css_clicked), page);

    return create_settings_page_card("THEME & ACCENTS", "[ METRO UI ]", GTK_WIDGET(page), NULL);
}
