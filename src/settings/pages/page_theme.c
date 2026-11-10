#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../theme.h"
#include <ctype.h>
#include <gio/gio.h>
#include <string.h>

#define ACCENTS   shell_accent_presets
#define N_ACCENTS SHELL_N_ACCENT_PRESETS
typedef ShellAccentPreset AccentPreset;

typedef struct _SettingsPageTheme {
    GtkBox parent_instance;

    GtkWidget      *card_palette;
    GtkWidget      *grid_swatches;
    GtkWidget      *entry_hex;
    GtkWidget      *swatch_buttons[N_ACCENTS];
    GtkCssProvider *swatch_providers[N_ACCENTS];
    GtkCssProvider *custom_swatch_provider;
    GtkCssProvider *preview_provider;

    GtkWidget      *scale_font;
    GtkWidget      *scale_font_heading;
    GtkWidget      *scale_font_sub;
    GtkWidget      *scale_font_badge;

    gchar          *current_accent_hex;
    gchar          *saved_accent_hex;
    int             saved_body_px;
    int             saved_heading_px;
    int             saved_sub_px;
    int             saved_badge_px;

    gboolean        syncing_ui;
} SettingsPageTheme;

typedef struct _SettingsPageThemeClass {
    GtkBoxClass parent_class;
} SettingsPageThemeClass;

G_DEFINE_TYPE(SettingsPageTheme, settings_page_theme, GTK_TYPE_BOX)

static gboolean
is_valid_hex_color(const gchar *str)
{
    if (!str || strlen(str) != 7 || str[0] != '#')
        return FALSE;
    for (int i = 1; i < 7; i++) {
        if (!g_ascii_isxdigit(str[i]))
            return FALSE;
    }
    return TRUE;
}

static gint
find_preset_for_hex(const gchar *hex)
{
    if (!hex)
        return -1;
    for (size_t i = 0; i < N_ACCENTS; i++) {
        if (g_ascii_strcasecmp(hex, ACCENTS[i].accent) == 0)
            return (gint)i;
    }
    return -1;
}

static const gchar *
hover_for_accent_hex(const gchar *hex)
{
    gint idx = find_preset_for_hex(hex);
    if (idx >= 0 && idx < (gint)N_ACCENTS)
        return ACCENTS[idx].hover;
    return (hex && hex[0]) ? hex : "#1084d8";
}

static void
set_current_accent_hex(SettingsPageTheme *self, const gchar *hex)
{
    gchar *dup = g_ascii_strdown((hex && is_valid_hex_color(hex)) ? hex : "#0078d4", -1);
    g_free(self->current_accent_hex);
    self->current_accent_hex = dup;
}

static void
update_custom_swatch_css(SettingsPageTheme *self)
{
    if (!self || !self->custom_swatch_provider)
        return;
    const gchar *hex = (self->current_accent_hex && is_valid_hex_color(self->current_accent_hex))
        ? self->current_accent_hex
        : "#0078d4";
    g_autofree gchar *css = g_strdup_printf(
        ".settings-custom-swatch { background-color: %s; border: 1px solid rgba(255,255,255,0.28); border-radius: 0px; }",
        hex);
    gtk_css_provider_load_from_string(self->custom_swatch_provider, css);
}

static void
apply_live_window_preview_css(SettingsPageTheme *self, gboolean enable_preview)
{
    if (!self || !self->preview_provider)
        return;

    if (!enable_preview) {
        gtk_css_provider_load_from_string(self->preview_provider, "");
        return;
    }

    const gchar *accent = (self->current_accent_hex && is_valid_hex_color(self->current_accent_hex))
        ? self->current_accent_hex
        : "#0078d4";
    const gchar *hover = hover_for_accent_hex(accent);

    int body_px    = (int)gtk_range_get_value(GTK_RANGE(self->scale_font));
    int heading_px = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_heading));
    int sub_px     = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_sub));
    int badge_px   = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_badge));

    g_autofree gchar *css = g_strdup_printf(
        "@define-color metro-accent %s;\n"
        "@define-color metro-accent-hover %s;\n"
        "window.shell-settings-app,\n"
        "window.shell-settings-app .row-title,\n"
        "window.shell-settings-app .sidebar-label,\n"
        "window.shell-settings-app .tile-title,\n"
        "window.shell-settings-app entry,\n"
        "window.shell-settings-app searchentry,\n"
        "window.shell-settings-app dropdown > button {\n"
        "    font-size: %dpx;\n"
        "}\n"
        "window.shell-settings-app .settings-header-title,\n"
        "window.shell-settings-app .shell-popover-title,\n"
        "window.shell-settings-app .heading {\n"
        "    font-size: %dpx;\n"
        "}\n"
        "window.shell-settings-app .row-subtitle,\n"
        "window.shell-settings-app .dim-label,\n"
        "window.shell-settings-app .shell-popover-subtitle,\n"
        "window.shell-settings-app button.files-nav-btn {\n"
        "    font-size: %dpx;\n"
        "}\n"
        "window.shell-settings-app .shell-popover-badge,\n"
        "window.shell-settings-app .tile-subtitle {\n"
        "    font-size: %dpx;\n"
        "}\n",
        accent, hover, body_px, heading_px, sub_px, badge_px);

    gtk_css_provider_load_from_string(self->preview_provider, css);
}

static gboolean
compute_theme_is_dirty(SettingsPageTheme *self)
{
    if (!self)
        return FALSE;
    const gchar *cur_hex = self->current_accent_hex ? self->current_accent_hex : "#0078d4";
    const gchar *sav_hex = self->saved_accent_hex ? self->saved_accent_hex : "#0078d4";

    if (g_ascii_strcasecmp(cur_hex, sav_hex) != 0)
        return TRUE;
    if ((int)gtk_range_get_value(GTK_RANGE(self->scale_font)) != self->saved_body_px)
        return TRUE;
    if ((int)gtk_range_get_value(GTK_RANGE(self->scale_font_heading)) != self->saved_heading_px)
        return TRUE;
    if ((int)gtk_range_get_value(GTK_RANGE(self->scale_font_sub)) != self->saved_sub_px)
        return TRUE;
    if ((int)gtk_range_get_value(GTK_RANGE(self->scale_font_badge)) != self->saved_badge_px)
        return TRUE;
    return FALSE;
}

static void
sync_swatch_highlight(SettingsPageTheme *self)
{
    gint match_idx = find_preset_for_hex(self->current_accent_hex);
    for (size_t i = 0; i < N_ACCENTS; i++) {
        if (!self->swatch_buttons[i])
            continue;
        if ((gint)i == match_idx)
            gtk_widget_add_css_class(self->swatch_buttons[i], "active");
        else
            gtk_widget_remove_css_class(self->swatch_buttons[i], "active");
    }
}

static void
update_theme_telemetry_and_dirty(SettingsPageTheme *self)
{
    if (!self)
        return;

    const gchar *hex = self->current_accent_hex ? self->current_accent_hex : "#0078d4";
    g_autofree gchar *upper_hex = g_ascii_strup(hex, -1);
    const gchar *short_name = settings_accent_short_name(hex);
    g_autofree gchar *c0 = g_strdup_printf("%s %s", upper_hex, short_name);

    int body_px    = (int)gtk_range_get_value(GTK_RANGE(self->scale_font));
    int heading_px = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_heading));
    int badge_px   = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_badge));

    g_autofree gchar *c1 = g_strdup_printf("%dPX", body_px);
    g_autofree gchar *c2 = g_strdup_printf("%dPX", heading_px);
    g_autofree gchar *c3 = g_strdup_printf("%dPX", badge_px);

    settings_page_set_counter(GTK_WIDGET(self), 0, "ACCENT", c0, "accent");
    settings_page_set_counter(GTK_WIDGET(self), 1, "BODY", c1, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 2, "HEADING", c2, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 3, "BADGES", c3, NULL);
    settings_page_set_nav_badge(GTK_WIDGET(self), "theme", short_name, "accent");

    gboolean dirty = compute_theme_is_dirty(self);
    apply_live_window_preview_css(self, dirty);
    settings_page_set_dirty(GTK_WIDGET(self), dirty);
}

static void
on_accent_button_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "preset-index"));
    if (idx >= N_ACCENTS)
        return;

    self->syncing_ui = TRUE;
    set_current_accent_hex(self, ACCENTS[idx].accent);
    if (self->entry_hex)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_hex), self->current_accent_hex);
    sync_swatch_highlight(self);
    update_custom_swatch_css(self);
    self->syncing_ui = FALSE;

    update_theme_telemetry_and_dirty(self);
}

static void
on_hex_entry_changed(GtkEditable *editable, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    if (self->syncing_ui)
        return;

    const gchar *txt = gtk_editable_get_text(editable);
    if (!is_valid_hex_color(txt))
        return;

    set_current_accent_hex(self, txt);
    sync_swatch_highlight(self);
    update_custom_swatch_css(self);
    update_theme_telemetry_and_dirty(self);
}

static void
on_scale_font_changed(GtkRange *range G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    if (self->syncing_ui)
        return;
    update_theme_telemetry_and_dirty(self);
}

static void
action_reset_fonts(gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    if (!self)
        return;
    self->syncing_ui = TRUE;
    gtk_range_set_value(GTK_RANGE(self->scale_font), 14.0);
    gtk_range_set_value(GTK_RANGE(self->scale_font_heading), 13.0);
    gtk_range_set_value(GTK_RANGE(self->scale_font_sub), 12.0);
    gtk_range_set_value(GTK_RANGE(self->scale_font_badge), 11.0);
    self->syncing_ui = FALSE;
    update_theme_telemetry_and_dirty(self);
}

static void
on_reset_fonts_btn_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    action_reset_fonts(user_data);
}

static void
action_revert_theme(gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    if (!self)
        return;

    self->syncing_ui = TRUE;
    set_current_accent_hex(self, self->saved_accent_hex ? self->saved_accent_hex : "#0078d4");
    if (self->entry_hex)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_hex), self->current_accent_hex);
    gtk_range_set_value(GTK_RANGE(self->scale_font), self->saved_body_px);
    gtk_range_set_value(GTK_RANGE(self->scale_font_heading), self->saved_heading_px);
    gtk_range_set_value(GTK_RANGE(self->scale_font_sub), self->saved_sub_px);
    gtk_range_set_value(GTK_RANGE(self->scale_font_badge), self->saved_badge_px);
    sync_swatch_highlight(self);
    update_custom_swatch_css(self);
    self->syncing_ui = FALSE;

    update_theme_telemetry_and_dirty(self);
}

typedef struct {
    const gchar *accent;
    gint         body_px;
    gint         heading_px;
    gint         sub_px;
    gint         badge_px;
} SaveThemeCtx;

static void
mutate_save_theme(KajoThemeCfg *draft, gpointer user_data)
{
    SaveThemeCtx *ctx = (SaveThemeCtx *)user_data;
    gchar *acc_copy = g_strdup(ctx->accent);
    g_free(draft->accent_color);
    draft->accent_color          = acc_copy;
    draft->ui_font_size_px       = ctx->body_px;
    draft->font_heading_size_px  = ctx->heading_px;
    draft->font_subtitle_size_px = ctx->sub_px;
    draft->font_badge_size_px    = ctx->badge_px;
}

static void
action_save_theme(gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    if (!self)
        return;

    const gchar *accent = (self->current_accent_hex && is_valid_hex_color(self->current_accent_hex))
        ? self->current_accent_hex
        : "#0078d4";
    const gchar *hover = hover_for_accent_hex(accent);

    int body_px    = (int)gtk_range_get_value(GTK_RANGE(self->scale_font));
    int heading_px = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_heading));
    int sub_px     = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_sub));
    int badge_px   = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_badge));

    if (body_px < 11 || body_px > 20)       body_px = 14;
    if (heading_px < 10 || heading_px > 18) heading_px = 13;
    if (sub_px < 10 || sub_px > 18)         sub_px = 12;
    if (badge_px < 9 || badge_px > 16)      badge_px = 11;

    shell_theme_write_override_css(accent, hover, body_px, heading_px, sub_px, badge_px);

    SaveThemeCtx ctx = { accent, body_px, heading_px, sub_px, badge_px };
    kajo_config_store_update_theme(NULL, mutate_save_theme, &ctx, NULL);

    gchar *saved_dup = g_strdup(accent);
    g_free(self->saved_accent_hex);
    self->saved_accent_hex = saved_dup;
    self->saved_body_px    = body_px;
    self->saved_heading_px = heading_px;
    self->saved_sub_px     = sub_px;
    self->saved_badge_px   = badge_px;

    apply_live_window_preview_css(self, FALSE);
    settings_reload_global_theme();
    settings_send_ipc_command("reload-theme");
    update_theme_telemetry_and_dirty(self);
}

static void
on_theme_page_mapped(GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    update_theme_telemetry_and_dirty((SettingsPageTheme *)widget);
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
    if (self->custom_swatch_provider) {
        if (display) {
            gtk_style_context_remove_provider_for_display(
                display, GTK_STYLE_PROVIDER(self->custom_swatch_provider));
        }
        g_clear_object(&self->custom_swatch_provider);
    }
    if (self->preview_provider) {
        if (display) {
            gtk_style_context_remove_provider_for_display(
                display, GTK_STYLE_PROVIDER(self->preview_provider));
        }
        g_clear_object(&self->preview_provider);
    }

    g_clear_pointer(&self->current_accent_hex, g_free);
    g_clear_pointer(&self->saved_accent_hex, g_free);

    G_OBJECT_CLASS(settings_page_theme_parent_class)->dispose(object);
}

static void
settings_page_theme_init(SettingsPageTheme *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));
}

static void
settings_page_theme_class_init(SettingsPageThemeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = settings_page_theme_dispose;
}

GtkWidget *
build_page_theme(void)
{
    SettingsPageTheme *self = g_object_new(settings_page_theme_get_type(), NULL);
    GdkDisplay *display = gdk_display_get_default();

    self->preview_provider = gtk_css_provider_new();
    self->custom_swatch_provider = gtk_css_provider_new();
    if (display) {
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(self->custom_swatch_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(self->preview_provider),
            GTK_STYLE_PROVIDER_PRIORITY_USER + 15);
    }

    g_autoptr(KajoThemeCfgRef) config = kajo_config_store_acquire_theme(NULL);

    /* Resolve initial accent hex from config or 00-accent.css */
    g_autofree gchar *init_hex = NULL;
    if (config && config->accent_color && is_valid_hex_color(config->accent_color)) {
        init_hex = g_ascii_strdown(config->accent_color, -1);
    } else {
        g_autofree gchar *accent_css_path = g_build_filename(
            g_get_user_config_dir(), "kajo", "style.d", "00-accent.css", NULL);
        g_autofree gchar *css_contents = NULL;
        if (g_file_get_contents(accent_css_path, &css_contents, NULL, NULL) && css_contents) {
            for (size_t i = 0; i < N_ACCENTS; i++) {
                if (strstr(css_contents, ACCENTS[i].accent) != NULL) {
                    init_hex = g_strdup(ACCENTS[i].accent);
                    break;
                }
            }
        }
    }
    if (!init_hex)
        init_hex = g_strdup(ACCENTS[0].accent);

    self->saved_accent_hex = g_strdup(init_hex);
    set_current_accent_hex(self, init_hex);

    self->saved_body_px = (config && config->ui_font_size_px >= 11 && config->ui_font_size_px <= 20)
        ? config->ui_font_size_px : 14;
    self->saved_heading_px = (config && config->font_heading_size_px >= 10 && config->font_heading_size_px <= 18)
        ? config->font_heading_size_px : 13;
    self->saved_sub_px = (config && config->font_subtitle_size_px >= 10 && config->font_subtitle_size_px <= 18)
        ? config->font_subtitle_size_px : 12;
    self->saved_badge_px = (config && config->font_badge_size_px >= 9 && config->font_badge_size_px <= 16)
        ? config->font_badge_size_px : 11;

    /* ─── Card 1: Metro Accent Color Palette (8 Presets + Custom Hex) ─── */
    self->card_palette = create_section_card(
        SETTINGS_CARD_LEFT,
        "1. METRO ACCENT COLOR PALETTE (8 PRESETS)",
        "00-accent.css",
        "accent",
        NULL);

    self->grid_swatches = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(self->grid_swatches), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(self->grid_swatches), 6);
    gtk_grid_set_row_spacing(GTK_GRID(self->grid_swatches), 6);
    gtk_widget_set_hexpand(self->grid_swatches, TRUE);

    for (size_t i = 0; i < N_ACCENTS; i++) {
        GtkWidget *card = gtk_button_new();
        gtk_widget_add_css_class(card, "widget-row-card");
        gtk_widget_set_hexpand(card, TRUE);
        g_object_set_data(G_OBJECT(card), "preset-index", GUINT_TO_POINTER((guint)i));
        self->swatch_buttons[i] = card;

        GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(card_box, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(card_box, TRUE);

        GtkWidget *swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(swatch, 16, 16);
        gtk_widget_set_hexpand(swatch, FALSE);
        gtk_widget_set_valign(swatch, GTK_ALIGN_CENTER);

        g_autofree gchar *swatch_class = g_strdup_printf("settings-swatch-%zu", i);
        gtk_widget_add_css_class(swatch, swatch_class);

        g_autofree gchar *swatch_css = g_strdup_printf(
            ".%s { background-color: %s; border: 1px solid rgba(255,255,255,0.28); border-radius: 0px; }",
            swatch_class, ACCENTS[i].accent);
        GtkCssProvider *p = gtk_css_provider_new();
        gtk_css_provider_load_from_string(p, swatch_css);
        if (display) {
            gtk_style_context_add_provider_for_display(
                display, GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        self->swatch_providers[i] = p;

        g_autofree gchar *tile_txt = g_strdup_printf("%s (%s)", ACCENTS[i].name, ACCENTS[i].accent);
        GtkWidget *name_lbl = gtk_label_new(tile_txt);
        gtk_widget_add_css_class(name_lbl, "row-title");
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(name_lbl), 6);
        gtk_widget_set_hexpand(name_lbl, TRUE);
        gtk_widget_set_valign(name_lbl, GTK_ALIGN_CENTER);

        gtk_box_append(GTK_BOX(card_box), swatch);
        gtk_box_append(GTK_BOX(card_box), name_lbl);
        gtk_button_set_child(GTK_BUTTON(card), card_box);

        g_signal_connect(card, "clicked", G_CALLBACK(on_accent_button_clicked), self);
        gtk_grid_attach(GTK_GRID(self->grid_swatches), card, (int)(i % 2), (int)(i / 2), 1, 1);
    }

    gtk_box_append(GTK_BOX(self->card_palette), self->grid_swatches);

    /* Custom Hex Override Row */
    GtkWidget *hex_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(hex_box, GTK_ALIGN_CENTER);

    GtkWidget *custom_swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(custom_swatch, 18, 18);
    gtk_widget_set_hexpand(custom_swatch, FALSE);
    gtk_widget_set_valign(custom_swatch, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(custom_swatch, "settings-custom-swatch");

    self->entry_hex = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(self->entry_hex), 7);
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_hex), 8);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(self->entry_hex), 9);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_hex), "#0078d4");
    gtk_widget_set_valign(self->entry_hex, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hex_box), custom_swatch);
    gtk_box_append(GTK_BOX(hex_box), self->entry_hex);

    GtkWidget *row_hex = create_boxed_row(
        "🎨",
        "Custom Accent Hex",
        "6-digit #RRGGBB override synced to 00-accent.css",
        hex_box);
    gtk_box_append(GTK_BOX(self->card_palette), row_hex);

    /* ─── Card 2: Typography Category Sizes (Fira Code) ─── */
    GtkWidget *btn_reset_hdr = gtk_button_new_with_label("↺ RESET DEFAULTS");
    gtk_widget_add_css_class(btn_reset_hdr, "files-nav-btn");
    g_signal_connect(btn_reset_hdr, "clicked", G_CALLBACK(on_reset_fonts_btn_clicked), self);

    GtkWidget *card_typo = create_section_card(
        SETTINGS_CARD_CENTER,
        "2. TYPOGRAPHY CATEGORY SIZES (FIRA CODE)",
        NULL,
        NULL,
        btn_reset_hdr);

    GtkWidget *w_body = create_scale_with_badge(
        11, 20, 1, self->saved_body_px, "%.0fpx", &self->scale_font, NULL);
    gtk_box_append(GTK_BOX(card_typo), create_boxed_row(
        NULL,
        "Primary Body, Controls & Row Titles",
        "ui_font_size_px (11–20px, default 14px)",
        w_body));

    GtkWidget *w_head = create_scale_with_badge(
        10, 18, 1, self->saved_heading_px, "%.0fpx", &self->scale_font_heading, NULL);
    gtk_box_append(GTK_BOX(card_typo), create_boxed_row(
        NULL,
        "Section & Window Headers",
        "font_heading_size_px (10–18px, default 13px)",
        w_head));

    GtkWidget *w_sub = create_scale_with_badge(
        10, 18, 1, self->saved_sub_px, "%.0fpx", &self->scale_font_sub, NULL);
    gtk_box_append(GTK_BOX(card_typo), create_boxed_row(
        NULL,
        "Subtitles, Buttons & Metadata",
        "font_subtitle_size_px (10–18px, default 12px)",
        w_sub));

    GtkWidget *w_bdg = create_scale_with_badge(
        9, 16, 1, self->saved_badge_px, "%.0fpx", &self->scale_font_badge, NULL);
    gtk_box_append(GTK_BOX(card_typo), create_boxed_row(
        NULL,
        "Technical Badges & Micro Labels",
        "font_badge_size_px (9–16px, default 11px)",
        w_bdg));

    settings_bento_add_pair(GTK_WIDGET(self), self->card_palette, card_typo);

    /* Sync initial UI state & wire signals */
    self->syncing_ui = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(self->entry_hex), self->current_accent_hex);
    sync_swatch_highlight(self);
    update_custom_swatch_css(self);
    self->syncing_ui = FALSE;

    g_signal_connect(self->entry_hex, "changed", G_CALLBACK(on_hex_entry_changed), self);
    g_signal_connect(self->scale_font, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_font_heading, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_font_sub, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_font_badge, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self, "map", G_CALLBACK(on_theme_page_mapped), NULL);

    settings_page_bind_actions(
        GTK_WIDGET(self),
        "↺ RESET FONTS",
        action_reset_fonts,
        action_revert_theme,
        action_save_theme,
        self);

    update_theme_telemetry_and_dirty(self);
    return GTK_WIDGET(self);
}
