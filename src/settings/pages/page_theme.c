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
    GtkWidget *scale_font_heading;
    GtkWidget *lbl_font_heading_val;
    GtkWidget *scale_font_sub;
    GtkWidget *lbl_font_sub_val;
    GtkWidget *scale_font_badge;
    GtkWidget *lbl_font_badge_val;
    GtkWidget *btn_reset_fonts;
    GtkWidget *btn_css;

    GtkWidget *swatch_buttons[N_ACCENTS];
    GtkCssProvider *swatch_providers[N_ACCENTS];
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
    int body_px    = (int)gtk_range_get_value(GTK_RANGE(self->scale_font));
    int heading_px = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_heading));
    int sub_px     = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_sub));
    int badge_px   = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_badge));

    if (body_px < 11 || body_px > 20)       body_px = 14;
    if (heading_px < 10 || heading_px > 18) heading_px = 13;
    if (sub_px < 10 || sub_px > 18)         sub_px = 12;
    if (badge_px < 9 || badge_px > 16)      badge_px = 11;

    shell_theme_write_override_css(preset->accent, preset->hover, body_px, heading_px, sub_px, badge_px);

    const gchar *xdg_config = g_get_user_config_dir();
    ShellConfig *config = shell_config_load(NULL);
    if (!config)
        config = shell_config_get_default();
    if (config) {
        g_free(config->accent_color);
        config->accent_color          = g_strdup(preset->accent);
        config->ui_font_size_px       = body_px;
        config->font_heading_size_px  = heading_px;
        config->font_subtitle_size_px = sub_px;
        config->font_badge_size_px    = badge_px;

        g_autofree gchar *cfg_path = g_build_filename(xdg_config, "kajo", "config.json", NULL);
        shell_config_save(config, cfg_path);
        shell_config_destroy(config);
    }

    settings_reload_global_theme();
    settings_send_ipc_command("reload-theme");

    g_message("Applied Metro accent '%s' (%s, body=%dpx heading=%dpx sub=%dpx badge=%dpx)",
              preset->name, preset->accent, body_px, heading_px, sub_px, badge_px);
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
    SettingsPageTheme *self = (SettingsPageTheme *)object;
    GdkDisplay *display = gdk_display_get_default();
    for (size_t i = 0; i < N_ACCENTS; i++) {
        if (self->swatch_providers[i]) {
            if (display) {
                gtk_style_context_remove_provider_for_display(
                    display, GTK_STYLE_PROVIDER(self->swatch_providers[i]));
            }
            g_clear_object(&self->swatch_providers[i]);
        }
    }
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
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, scale_font_heading);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, lbl_font_heading_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, scale_font_sub);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, lbl_font_sub_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, scale_font_badge);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, lbl_font_badge_val);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, btn_reset_fonts);
    gtk_widget_class_bind_template_child(widget_class, SettingsPageTheme, btn_css);
}

static void
update_font_badge_labels(SettingsPageTheme *self)
{
    if (!self) return;
    g_autofree gchar *b_str = g_strdup_printf("%dpx", (int)gtk_range_get_value(GTK_RANGE(self->scale_font)));
    g_autofree gchar *h_str = g_strdup_printf("%dpx", (int)gtk_range_get_value(GTK_RANGE(self->scale_font_heading)));
    g_autofree gchar *s_str = g_strdup_printf("%dpx", (int)gtk_range_get_value(GTK_RANGE(self->scale_font_sub)));
    g_autofree gchar *m_str = g_strdup_printf("%dpx", (int)gtk_range_get_value(GTK_RANGE(self->scale_font_badge)));

    gtk_label_set_text(GTK_LABEL(self->lbl_font_val), b_str);
    gtk_label_set_text(GTK_LABEL(self->lbl_font_heading_val), h_str);
    gtk_label_set_text(GTK_LABEL(self->lbl_font_sub_val), s_str);
    gtk_label_set_text(GTK_LABEL(self->lbl_font_badge_val), m_str);
}

static void
on_scale_font_changed(GtkRange *range G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    update_font_badge_labels(self);
    if (!self->syncing_ui) {
        write_theme_override_and_config(self);
    }
}

static void
on_reset_fonts_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    self->syncing_ui = TRUE;
    gtk_range_set_value(GTK_RANGE(self->scale_font), 14);
    gtk_range_set_value(GTK_RANGE(self->scale_font_heading), 13);
    gtk_range_set_value(GTK_RANGE(self->scale_font_sub), 12);
    gtk_range_set_value(GTK_RANGE(self->scale_font_badge), 11);
    self->syncing_ui = FALSE;
    update_font_badge_labels(self);
    write_theme_override_and_config(self);
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
        gtk_widget_add_css_class(card, "widget-row-card");
        gtk_widget_set_hexpand(card, TRUE);
        g_object_set_data(G_OBJECT(card), "preset-index", GUINT_TO_POINTER((guint)i));
        page->swatch_buttons[i] = card;

        GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(card_box, GTK_ALIGN_CENTER);

        /* Accent Color Square Swatch */
        GtkWidget *swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(swatch, 18, 18);
        gtk_widget_set_valign(swatch, GTK_ALIGN_CENTER);
        gchar *swatch_class = g_strdup_printf("settings-swatch-%zu", i);
        gtk_widget_add_css_class(swatch, swatch_class);

        gchar *swatch_css = g_strdup_printf(".%s { background-color: %s; border: 1px solid rgba(255,255,255,0.25); border-radius: 0px; }", swatch_class, ACCENTS[i].accent);
        GtkCssProvider *p = gtk_css_provider_new();
        gtk_css_provider_load_from_string(p, swatch_css);
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        page->swatch_providers[i] = p;
        g_free(swatch_class);
        g_free(swatch_css);

        GtkWidget *name_label = gtk_label_new(ACCENTS[i].name);
        gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
        gtk_widget_set_hexpand(name_label, TRUE);
        gtk_widget_set_valign(name_label, GTK_ALIGN_CENTER);

        gtk_box_append(GTK_BOX(card_box), swatch);
        gtk_box_append(GTK_BOX(card_box), name_label);
        gtk_button_set_child(GTK_BUTTON(card), card_box);

        g_signal_connect(card, "clicked", G_CALLBACK(on_accent_button_clicked), page);

        int col = i % 2;
        int row = i / 2;
        gtk_grid_attach(GTK_GRID(page->grid_swatches), card, col, row, 1, 1);
    }

    int init_body = (config && config->ui_font_size_px >= 11 && config->ui_font_size_px <= 20)
                        ? config->ui_font_size_px
                        : 14;
    int init_head = (config && config->font_heading_size_px >= 10 && config->font_heading_size_px <= 18)
                        ? config->font_heading_size_px
                        : 13;
    int init_sub  = (config && config->font_subtitle_size_px >= 10 && config->font_subtitle_size_px <= 18)
                        ? config->font_subtitle_size_px
                        : 12;
    int init_bdg  = (config && config->font_badge_size_px >= 9 && config->font_badge_size_px <= 16)
                        ? config->font_badge_size_px
                        : 11;

    gtk_range_set_value(GTK_RANGE(page->scale_font), init_body);
    gtk_range_set_value(GTK_RANGE(page->scale_font_heading), init_head);
    gtk_range_set_value(GTK_RANGE(page->scale_font_sub), init_sub);
    gtk_range_set_value(GTK_RANGE(page->scale_font_badge), init_bdg);
    update_font_badge_labels(page);

    select_accent_preset(page, initial_idx, FALSE);
    page->syncing_ui = FALSE;

    if (config) shell_config_destroy(config);

    g_signal_connect(page->combo_palette, "notify::selected", G_CALLBACK(on_combo_palette_changed), page);
    g_signal_connect(page->scale_font, "value-changed", G_CALLBACK(on_scale_font_changed), page);
    g_signal_connect(page->scale_font_heading, "value-changed", G_CALLBACK(on_scale_font_changed), page);
    g_signal_connect(page->scale_font_sub, "value-changed", G_CALLBACK(on_scale_font_changed), page);
    g_signal_connect(page->scale_font_badge, "value-changed", G_CALLBACK(on_scale_font_changed), page);
    g_signal_connect(page->btn_reset_fonts, "clicked", G_CALLBACK(on_reset_fonts_clicked), page);
    g_signal_connect(page->btn_css, "clicked", G_CALLBACK(on_reload_css_clicked), page);

    return create_settings_page_card("THEME & ACCENTS", "[ METRO UI ]", GTK_WIDGET(page), NULL);
}
