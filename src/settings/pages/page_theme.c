#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../theme.h"
#include "../../config/kajo_config_schema.h"
#include <ctype.h>
#include <gio/gio.h>
#include <math.h>
#include <string.h>

#define ACCENTS   shell_accent_presets
#define N_ACCENTS SHELL_N_ACCENT_PRESETS
typedef ShellAccentPreset AccentPreset;

#define SURFACE_PRESETS   shell_surface_presets
#define N_SURFACE_PRESETS SHELL_N_SURFACE_PRESETS

typedef struct _SettingsPageTheme {
    GtkBox parent_instance;

    GtkWidget      *card_palette;
    GtkWidget      *grid_swatches;
    GtkWidget      *entry_hex;
    GtkWidget      *swatch_buttons[N_ACCENTS];
    GtkCssProvider *swatch_providers[N_ACCENTS];
    GtkCssProvider *custom_swatch_provider;
    GtkCssProvider *glass_swatch_provider;
    GtkCssProvider *surface_swatch_provider;
    GtkCssProvider *preview_provider;

    GtkWidget      *entry_ui_font;
    GtkWidget      *scale_font;
    GtkWidget      *scale_font_heading;
    GtkWidget      *scale_font_sub;
    GtkWidget      *scale_font_badge;

    /* Surface & glass controls */
    GtkWidget      *surface_preset_buttons[N_SURFACE_PRESETS];
    GtkWidget      *entry_glass_tint;
    GtkWidget      *scale_panel_opacity;
    GtkWidget      *scale_glass_opacity;
    GtkWidget      *scale_glass_ctrl;
    GtkWidget      *scale_widget_chip;
    GtkWidget      *scale_accent_glass;
    GtkWidget      *entry_surface_base;
    GtkWidget      *scale_surface_contrast;
    GtkWidget      *scale_border_opacity;
    GtkWidget      *scale_secondary_fg;

    gchar          *current_accent_hex;
    gchar          *saved_accent_hex;
    gchar          *saved_ui_font_family;
    int             saved_body_px;
    int             saved_heading_px;
    int             saved_sub_px;
    int             saved_badge_px;

    gchar          *current_glass_tint;
    gchar          *saved_glass_tint;
    int             saved_panel_opacity_pct;
    int             saved_glass_opacity_pct;
    int             saved_glass_ctrl_pct;
    int             saved_widget_chip_pct;
    int             saved_accent_glass_pct;
    gchar          *current_surface_base;
    gchar          *saved_surface_base;
    int             saved_surface_contrast_pct;
    int             saved_border_opacity_pct;
    int             saved_secondary_fg_pct;

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
set_current_glass_tint(SettingsPageTheme *self, const gchar *hex)
{
    gchar *dup = g_ascii_strdown((hex && is_valid_hex_color(hex)) ? hex : "#121212", -1);
    g_free(self->current_glass_tint);
    self->current_glass_tint = dup;
}

static void
set_current_surface_base(SettingsPageTheme *self, const gchar *hex)
{
    gchar *dup = g_ascii_strdown((hex && is_valid_hex_color(hex)) ? hex : "#141414", -1);
    g_free(self->current_surface_base);
    self->current_surface_base = dup;
}

static void
update_custom_swatch_css(SettingsPageTheme *self)
{
    if (!self)
        return;
    if (self->custom_swatch_provider) {
        const gchar *hex = (self->current_accent_hex && is_valid_hex_color(self->current_accent_hex))
            ? self->current_accent_hex : "#0078d4";
        g_autofree gchar *css = g_strdup_printf(
            ".settings-custom-swatch { background-color: %s; border: 1px solid rgba(255,255,255,0.28); border-radius: 0px; }",
            hex);
        gtk_css_provider_load_from_string(self->custom_swatch_provider, css);
    }
    if (self->glass_swatch_provider) {
        const gchar *hex = (self->current_glass_tint && is_valid_hex_color(self->current_glass_tint))
            ? self->current_glass_tint : "#121212";
        g_autofree gchar *css = g_strdup_printf(
            ".settings-glass-swatch { background-color: %s; border: 1px solid rgba(255,255,255,0.28); border-radius: 0px; }",
            hex);
        gtk_css_provider_load_from_string(self->glass_swatch_provider, css);
    }
    if (self->surface_swatch_provider) {
        const gchar *hex = (self->current_surface_base && is_valid_hex_color(self->current_surface_base))
            ? self->current_surface_base : "#141414";
        g_autofree gchar *css = g_strdup_printf(
            ".settings-surface-swatch { background-color: %s; border: 1px solid rgba(255,255,255,0.28); border-radius: 0px; }",
            hex);
        gtk_css_provider_load_from_string(self->surface_swatch_provider, css);
    }
}

static const gchar *
get_ui_font_text(SettingsPageTheme *self)
{
    if (!self || !self->entry_ui_font)
        return "Fira Code";
    const gchar *txt = gtk_editable_get_text(GTK_EDITABLE(self->entry_ui_font));
    return (txt && txt[0]) ? txt : "Fira Code";
}

static void
populate_draft_cfg_from_ui(SettingsPageTheme *self, KajoThemeCfg *out_cfg)
{
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->accent_color = (gchar *)((self->current_accent_hex && is_valid_hex_color(self->current_accent_hex))
        ? self->current_accent_hex : "#0078d4");
    out_cfg->ui_font_family = (gchar *)get_ui_font_text(self);
    out_cfg->ui_font_size_px = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_font)));
    out_cfg->font_heading_size_px = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_font_heading)));
    out_cfg->font_subtitle_size_px = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_font_sub)));
    out_cfg->font_badge_size_px = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_font_badge)));

    out_cfg->glass_tint = (gchar *)((self->current_glass_tint && is_valid_hex_color(self->current_glass_tint))
        ? self->current_glass_tint : "#121212");
    out_cfg->panel_opacity = gtk_range_get_value(GTK_RANGE(self->scale_panel_opacity)) / 100.0;
    out_cfg->glass_opacity = gtk_range_get_value(GTK_RANGE(self->scale_glass_opacity)) / 100.0;
    out_cfg->glass_control_contrast = gtk_range_get_value(GTK_RANGE(self->scale_glass_ctrl)) / 100.0;
    out_cfg->widget_chip_opacity = gtk_range_get_value(GTK_RANGE(self->scale_widget_chip)) / 100.0;
    out_cfg->accent_glass_tint = gtk_range_get_value(GTK_RANGE(self->scale_accent_glass)) / 100.0;

    out_cfg->surface_base = (gchar *)((self->current_surface_base && is_valid_hex_color(self->current_surface_base))
        ? self->current_surface_base : "#141414");
    out_cfg->surface_contrast = gtk_range_get_value(GTK_RANGE(self->scale_surface_contrast)) / 100.0;
    out_cfg->border_opacity = gtk_range_get_value(GTK_RANGE(self->scale_border_opacity)) / 100.0;
    out_cfg->secondary_fg_contrast = gtk_range_get_value(GTK_RANGE(self->scale_secondary_fg)) / 100.0;
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

    KajoThemeCfg draft;
    populate_draft_cfg_from_ui(self, &draft);
    const gchar *accent = draft.accent_color;
    const gchar *hover  = hover_for_accent_hex(accent);

    g_autofree gchar *tokens = shell_theme_build_token_css(&draft, accent, hover);
    g_autofree gchar *css = g_strdup_printf(
        "%s\n"
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
        tokens,
        draft.ui_font_size_px,
        draft.font_heading_size_px,
        draft.font_subtitle_size_px,
        draft.font_badge_size_px);

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

    const gchar *cur_font = get_ui_font_text(self);
    const gchar *sav_font = (self->saved_ui_font_family && self->saved_ui_font_family[0])
        ? self->saved_ui_font_family : "Fira Code";
    if (g_strcmp0(cur_font, sav_font) != 0)
        return TRUE;

    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_font))) != self->saved_body_px)
        return TRUE;
    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_font_heading))) != self->saved_heading_px)
        return TRUE;
    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_font_sub))) != self->saved_sub_px)
        return TRUE;
    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_font_badge))) != self->saved_badge_px)
        return TRUE;

    const gchar *cur_gt = self->current_glass_tint ? self->current_glass_tint : "#121212";
    const gchar *sav_gt = self->saved_glass_tint ? self->saved_glass_tint : "#121212";
    if (g_ascii_strcasecmp(cur_gt, sav_gt) != 0)
        return TRUE;

    const gchar *cur_sb = self->current_surface_base ? self->current_surface_base : "#141414";
    const gchar *sav_sb = self->saved_surface_base ? self->saved_surface_base : "#141414";
    if (g_ascii_strcasecmp(cur_sb, sav_sb) != 0)
        return TRUE;

    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_panel_opacity))) != self->saved_panel_opacity_pct)
        return TRUE;
    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_glass_opacity))) != self->saved_glass_opacity_pct)
        return TRUE;
    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_glass_ctrl))) != self->saved_glass_ctrl_pct)
        return TRUE;
    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_widget_chip))) != self->saved_widget_chip_pct)
        return TRUE;
    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_accent_glass))) != self->saved_accent_glass_pct)
        return TRUE;
    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_surface_contrast))) != self->saved_surface_contrast_pct)
        return TRUE;
    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_border_opacity))) != self->saved_border_opacity_pct)
        return TRUE;
    if ((int)lround(gtk_range_get_value(GTK_RANGE(self->scale_secondary_fg))) != self->saved_secondary_fg_pct)
        return TRUE;

    return FALSE;
}

static gint
find_matching_surface_preset(SettingsPageTheme *self)
{
    if (!self)
        return -1;
    const gchar *gt = self->current_glass_tint ? self->current_glass_tint : "#121212";
    const gchar *sb = self->current_surface_base ? self->current_surface_base : "#141414";
    int po = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_panel_opacity)));
    int go = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_glass_opacity)));
    int gc = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_glass_ctrl)));
    int wc = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_widget_chip)));
    int ag = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_accent_glass)));
    int sc = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_surface_contrast)));
    int bo = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_border_opacity)));
    int sf = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_secondary_fg)));

    for (size_t i = 0; i < N_SURFACE_PRESETS; i++) {
        const ShellSurfacePreset *p = &SURFACE_PRESETS[i];
        if (g_ascii_strcasecmp(gt, p->glass_tint) == 0 &&
            g_ascii_strcasecmp(sb, p->surface_base) == 0 &&
            po == (int)lround(p->panel_opacity * 100.0) &&
            go == (int)lround(p->glass_opacity * 100.0) &&
            gc == (int)lround(p->glass_control_contrast * 100.0) &&
            wc == (int)lround(p->widget_chip_opacity * 100.0) &&
            ag == (int)lround(p->accent_glass_tint * 100.0) &&
            sc == (int)lround(p->surface_contrast * 100.0) &&
            bo == (int)lround(p->border_opacity * 100.0) &&
            sf == (int)lround(p->secondary_fg_contrast * 100.0))
            return (gint)i;
    }
    return -1;
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

    gint surf_idx = find_matching_surface_preset(self);
    for (size_t i = 0; i < N_SURFACE_PRESETS; i++) {
        if (!self->surface_preset_buttons[i])
            continue;
        if ((gint)i == surf_idx)
            gtk_widget_add_css_class(self->surface_preset_buttons[i], "active");
        else
            gtk_widget_remove_css_class(self->surface_preset_buttons[i], "active");
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

    int body_px  = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_font)));
    int glass_op = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_glass_opacity)));
    int panel_op = (int)lround(gtk_range_get_value(GTK_RANGE(self->scale_panel_opacity)));

    g_autofree gchar *c1 = g_strdup_printf("%dPX", body_px);
    g_autofree gchar *c2 = g_strdup_printf("%d%%", panel_op);
    g_autofree gchar *c3 = g_strdup_printf("%d%%", glass_op);

    settings_page_set_counter(GTK_WIDGET(self), 0, "ACCENT", c0, "accent");
    settings_page_set_counter(GTK_WIDGET(self), 1, "BODY", c1, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 2, "PANEL", c2, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 3, "GLASS", c3, NULL);
    settings_page_set_nav_badge(GTK_WIDGET(self), "theme", short_name, "accent");

    sync_swatch_highlight(self);
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
    update_custom_swatch_css(self);
    self->syncing_ui = FALSE;

    update_theme_telemetry_and_dirty(self);
}

static void
apply_surface_preset(SettingsPageTheme *self, const ShellSurfacePreset *p)
{
    if (!self || !p)
        return;
    self->syncing_ui = TRUE;
    set_current_glass_tint(self, p->glass_tint);
    set_current_surface_base(self, p->surface_base);
    if (self->entry_glass_tint)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_glass_tint), self->current_glass_tint);
    if (self->entry_surface_base)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_surface_base), self->current_surface_base);
    gtk_range_set_value(GTK_RANGE(self->scale_panel_opacity), lround(p->panel_opacity * 100.0));
    gtk_range_set_value(GTK_RANGE(self->scale_glass_opacity), lround(p->glass_opacity * 100.0));
    gtk_range_set_value(GTK_RANGE(self->scale_glass_ctrl), lround(p->glass_control_contrast * 100.0));
    gtk_range_set_value(GTK_RANGE(self->scale_widget_chip), lround(p->widget_chip_opacity * 100.0));
    gtk_range_set_value(GTK_RANGE(self->scale_accent_glass), lround(p->accent_glass_tint * 100.0));
    gtk_range_set_value(GTK_RANGE(self->scale_surface_contrast), lround(p->surface_contrast * 100.0));
    gtk_range_set_value(GTK_RANGE(self->scale_border_opacity), lround(p->border_opacity * 100.0));
    gtk_range_set_value(GTK_RANGE(self->scale_secondary_fg), lround(p->secondary_fg_contrast * 100.0));
    update_custom_swatch_css(self);
    self->syncing_ui = FALSE;
    update_theme_telemetry_and_dirty(self);
}

static void
on_surface_preset_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "surface-preset-index"));
    if (idx >= N_SURFACE_PRESETS)
        return;
    apply_surface_preset(self, &SURFACE_PRESETS[idx]);
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
    update_custom_swatch_css(self);
    update_theme_telemetry_and_dirty(self);
}

static void
on_glass_tint_entry_changed(GtkEditable *editable, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    if (self->syncing_ui)
        return;

    const gchar *txt = gtk_editable_get_text(editable);
    if (!is_valid_hex_color(txt))
        return;

    set_current_glass_tint(self, txt);
    update_custom_swatch_css(self);
    update_theme_telemetry_and_dirty(self);
}

static void
on_surface_base_entry_changed(GtkEditable *editable, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    if (self->syncing_ui)
        return;

    const gchar *txt = gtk_editable_get_text(editable);
    if (!is_valid_hex_color(txt))
        return;

    set_current_surface_base(self, txt);
    update_custom_swatch_css(self);
    update_theme_telemetry_and_dirty(self);
}

static void
on_ui_font_entry_changed(GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    if (self->syncing_ui)
        return;
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
    if (self->entry_ui_font)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_ui_font), "Fira Code");
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
on_reset_surfaces_btn_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    apply_surface_preset(self, &SURFACE_PRESETS[0]);
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
    if (self->entry_ui_font)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_ui_font),
                              self->saved_ui_font_family ? self->saved_ui_font_family : "Fira Code");
    gtk_range_set_value(GTK_RANGE(self->scale_font), self->saved_body_px);
    gtk_range_set_value(GTK_RANGE(self->scale_font_heading), self->saved_heading_px);
    gtk_range_set_value(GTK_RANGE(self->scale_font_sub), self->saved_sub_px);
    gtk_range_set_value(GTK_RANGE(self->scale_font_badge), self->saved_badge_px);

    set_current_glass_tint(self, self->saved_glass_tint ? self->saved_glass_tint : "#121212");
    set_current_surface_base(self, self->saved_surface_base ? self->saved_surface_base : "#141414");
    if (self->entry_glass_tint)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_glass_tint), self->current_glass_tint);
    if (self->entry_surface_base)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_surface_base), self->current_surface_base);
    gtk_range_set_value(GTK_RANGE(self->scale_panel_opacity), self->saved_panel_opacity_pct);
    gtk_range_set_value(GTK_RANGE(self->scale_glass_opacity), self->saved_glass_opacity_pct);
    gtk_range_set_value(GTK_RANGE(self->scale_glass_ctrl), self->saved_glass_ctrl_pct);
    gtk_range_set_value(GTK_RANGE(self->scale_widget_chip), self->saved_widget_chip_pct);
    gtk_range_set_value(GTK_RANGE(self->scale_accent_glass), self->saved_accent_glass_pct);
    gtk_range_set_value(GTK_RANGE(self->scale_surface_contrast), self->saved_surface_contrast_pct);
    gtk_range_set_value(GTK_RANGE(self->scale_border_opacity), self->saved_border_opacity_pct);
    gtk_range_set_value(GTK_RANGE(self->scale_secondary_fg), self->saved_secondary_fg_pct);

    update_custom_swatch_css(self);
    self->syncing_ui = FALSE;

    update_theme_telemetry_and_dirty(self);
}

typedef struct {
    const KajoThemeCfg *src;
} SaveThemeCtx;

static void
mutate_save_theme(KajoThemeCfg *draft, gpointer user_data)
{
    SaveThemeCtx *ctx = (SaveThemeCtx *)user_data;
    const KajoThemeCfg *s = ctx->src;

    gchar *acc_copy = g_strdup(s->accent_color);
    g_free(draft->accent_color);
    draft->accent_color = acc_copy;

    gchar *font_copy = g_strdup(s->ui_font_family);
    g_free(draft->ui_font_family);
    draft->ui_font_family = font_copy;

    draft->ui_font_size_px       = s->ui_font_size_px;
    draft->font_heading_size_px  = s->font_heading_size_px;
    draft->font_subtitle_size_px = s->font_subtitle_size_px;
    draft->font_badge_size_px    = s->font_badge_size_px;

    gchar *gt_copy = g_strdup(s->glass_tint);
    g_free(draft->glass_tint);
    draft->glass_tint = gt_copy;

    draft->panel_opacity          = s->panel_opacity;
    draft->glass_opacity          = s->glass_opacity;
    draft->glass_control_contrast = s->glass_control_contrast;
    draft->widget_chip_opacity    = s->widget_chip_opacity;
    draft->accent_glass_tint      = s->accent_glass_tint;

    gchar *sb_copy = g_strdup(s->surface_base);
    g_free(draft->surface_base);
    draft->surface_base = sb_copy;

    draft->surface_contrast      = s->surface_contrast;
    draft->border_opacity        = s->border_opacity;
    draft->secondary_fg_contrast = s->secondary_fg_contrast;
}

static void
action_save_theme(gpointer user_data)
{
    SettingsPageTheme *self = (SettingsPageTheme *)user_data;
    if (!self)
        return;

    KajoThemeCfg draft;
    populate_draft_cfg_from_ui(self, &draft);

    if (draft.ui_font_size_px < 11 || draft.ui_font_size_px > 20)
        draft.ui_font_size_px = 14;
    if (draft.font_heading_size_px < 10 || draft.font_heading_size_px > 18)
        draft.font_heading_size_px = 13;
    if (draft.font_subtitle_size_px < 10 || draft.font_subtitle_size_px > 18)
        draft.font_subtitle_size_px = 12;
    if (draft.font_badge_size_px < 9 || draft.font_badge_size_px > 16)
        draft.font_badge_size_px = 11;

    SaveThemeCtx ctx = { .src = &draft };
    kajo_config_store_update_theme(NULL, mutate_save_theme, &ctx, NULL);

    g_autoptr(KajoThemeCfgRef) saved = kajo_config_store_acquire_theme(NULL);
    shell_theme_sync_from_theme_cfg(saved ? saved : &draft);

    /* Keep compositor inactive window border in sync with derived @metro-border. */
    gchar border_hex[8] = {0};
    shell_theme_compute_border_hex(saved ? saved : &draft, border_hex);
    (void)kajo_config_comp_save_inactive_border_color(NULL, border_hex, NULL);

    gchar *saved_dup = g_strdup(draft.accent_color);
    g_free(self->saved_accent_hex);
    self->saved_accent_hex = saved_dup;

    gchar *font_dup = g_strdup(draft.ui_font_family);
    g_free(self->saved_ui_font_family);
    self->saved_ui_font_family = font_dup;

    self->saved_body_px    = draft.ui_font_size_px;
    self->saved_heading_px = draft.font_heading_size_px;
    self->saved_sub_px     = draft.font_subtitle_size_px;
    self->saved_badge_px   = draft.font_badge_size_px;

    gchar *gt_dup = g_strdup(draft.glass_tint);
    g_free(self->saved_glass_tint);
    self->saved_glass_tint = gt_dup;

    gchar *sb_dup = g_strdup(draft.surface_base);
    g_free(self->saved_surface_base);
    self->saved_surface_base = sb_dup;

    self->saved_panel_opacity_pct    = (int)lround(draft.panel_opacity * 100.0);
    self->saved_glass_opacity_pct    = (int)lround(draft.glass_opacity * 100.0);
    self->saved_glass_ctrl_pct       = (int)lround(draft.glass_control_contrast * 100.0);
    self->saved_widget_chip_pct      = (int)lround(draft.widget_chip_opacity * 100.0);
    self->saved_accent_glass_pct     = (int)lround(draft.accent_glass_tint * 100.0);
    self->saved_surface_contrast_pct = (int)lround(draft.surface_contrast * 100.0);
    self->saved_border_opacity_pct   = (int)lround(draft.border_opacity * 100.0);
    self->saved_secondary_fg_pct     = (int)lround(draft.secondary_fg_contrast * 100.0);

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
    if (self->glass_swatch_provider) {
        if (display) {
            gtk_style_context_remove_provider_for_display(
                display, GTK_STYLE_PROVIDER(self->glass_swatch_provider));
        }
        g_clear_object(&self->glass_swatch_provider);
    }
    if (self->surface_swatch_provider) {
        if (display) {
            gtk_style_context_remove_provider_for_display(
                display, GTK_STYLE_PROVIDER(self->surface_swatch_provider));
        }
        g_clear_object(&self->surface_swatch_provider);
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
    g_clear_pointer(&self->saved_ui_font_family, g_free);
    g_clear_pointer(&self->current_glass_tint, g_free);
    g_clear_pointer(&self->saved_glass_tint, g_free);
    g_clear_pointer(&self->current_surface_base, g_free);
    g_clear_pointer(&self->saved_surface_base, g_free);

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

static void
theme_init_providers_and_config(SettingsPageTheme *self, GdkDisplay *display)
{
    self->preview_provider = gtk_css_provider_new();
    self->custom_swatch_provider = gtk_css_provider_new();
    self->glass_swatch_provider = gtk_css_provider_new();
    self->surface_swatch_provider = gtk_css_provider_new();
    if (display) {
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(self->custom_swatch_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(self->glass_swatch_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(self->surface_swatch_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(self->preview_provider),
            GTK_STYLE_PROVIDER_PRIORITY_USER + 15);
    }

    g_autoptr(KajoThemeCfgRef) config = kajo_config_store_acquire_theme(NULL);

    /* Resolve initial accent hex strictly from theme.toml snapshot */
    g_autofree gchar *init_hex = NULL;
    if (config && config->accent_color && is_valid_hex_color(config->accent_color))
        init_hex = g_ascii_strdown(config->accent_color, -1);
    else
        init_hex = g_strdup(ACCENTS[0].accent);

    self->saved_accent_hex = g_strdup(init_hex);
    set_current_accent_hex(self, init_hex);

    self->saved_ui_font_family = g_strdup(
        (config && config->ui_font_family && config->ui_font_family[0])
            ? config->ui_font_family : "Fira Code");

    self->saved_body_px = (config && config->ui_font_size_px >= 11 && config->ui_font_size_px <= 20)
        ? config->ui_font_size_px : 14;
    self->saved_heading_px = (config && config->font_heading_size_px >= 10 && config->font_heading_size_px <= 18)
        ? config->font_heading_size_px : 13;
    self->saved_sub_px = (config && config->font_subtitle_size_px >= 10 && config->font_subtitle_size_px <= 18)
        ? config->font_subtitle_size_px : 12;
    self->saved_badge_px = (config && config->font_badge_size_px >= 9 && config->font_badge_size_px <= 16)
        ? config->font_badge_size_px : 11;

    g_autofree gchar *init_gt = (config && config->glass_tint && is_valid_hex_color(config->glass_tint))
        ? g_ascii_strdown(config->glass_tint, -1) : g_strdup("#121212");
    self->saved_glass_tint = g_strdup(init_gt);
    set_current_glass_tint(self, init_gt);

    g_autofree gchar *init_sb = (config && config->surface_base && is_valid_hex_color(config->surface_base))
        ? g_ascii_strdown(config->surface_base, -1) : g_strdup("#141414");
    self->saved_surface_base = g_strdup(init_sb);
    set_current_surface_base(self, init_sb);

    self->saved_panel_opacity_pct    = (int)lround(CLAMP(config ? config->panel_opacity : 0.58, 0.0, 1.0) * 100.0);
    self->saved_glass_opacity_pct    = (int)lround(CLAMP(config ? config->glass_opacity : 0.62, 0.0, 1.0) * 100.0);
    self->saved_glass_ctrl_pct       = (int)lround(CLAMP(config ? config->glass_control_contrast : 1.0, 0.0, 2.0) * 100.0);
    self->saved_widget_chip_pct      = (int)lround(CLAMP(config ? config->widget_chip_opacity : 0.0, 0.0, 0.25) * 100.0);
    self->saved_accent_glass_pct     = (int)lround(CLAMP(config ? config->accent_glass_tint : 0.0, 0.0, 0.25) * 100.0);
    self->saved_surface_contrast_pct = (int)lround(CLAMP(config ? config->surface_contrast : 1.0, 0.0, 2.0) * 100.0);
    self->saved_border_opacity_pct   = (int)lround(CLAMP(config ? config->border_opacity : 1.0, 0.0, 1.0) * 100.0);
    self->saved_secondary_fg_pct     = (int)lround(CLAMP(config ? config->secondary_fg_contrast : 1.0, 0.5, 1.5) * 100.0);
}

static GtkWidget *
theme_build_card_palette(SettingsPageTheme *self, GdkDisplay *display)
{
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

    return self->card_palette;
}

static GtkWidget *
theme_build_card_typography(SettingsPageTheme *self)
{
    /* ─── Card 2: Typography & Font Scale ─── */
    GtkWidget *btn_reset_hdr = gtk_button_new_with_label("↺ RESET DEFAULTS");
    gtk_widget_add_css_class(btn_reset_hdr, "files-nav-btn");
    g_signal_connect(btn_reset_hdr, "clicked", G_CALLBACK(on_reset_fonts_btn_clicked), self);

    GtkWidget *card_typo = create_section_card(
        SETTINGS_CARD_CENTER,
        "2. TYPOGRAPHY & FONT SCALE",
        NULL,
        NULL,
        btn_reset_hdr);

    self->entry_ui_font = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_ui_font), 16);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(self->entry_ui_font), 24);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_ui_font), "Fira Code");
    gtk_widget_set_valign(self->entry_ui_font, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card_typo), create_boxed_row(
        NULL,
        "UI Font Family",
        "ui_font_family (default Fira Code, monospace fallback)",
        self->entry_ui_font));

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

    return card_typo;
}

static GtkWidget *
theme_build_card_glass(SettingsPageTheme *self)
{
    /* ─── Card 3: Frosted Glass & Panel Surfaces ─── */
    GtkWidget *btn_reset_surf = gtk_button_new_with_label("↺ RESET SURFACES");
    gtk_widget_add_css_class(btn_reset_surf, "files-nav-btn");
    g_signal_connect(btn_reset_surf, "clicked", G_CALLBACK(on_reset_surfaces_btn_clicked), self);

    GtkWidget *card_glass = create_section_card(
        SETTINGS_CARD_LEFT,
        "3. FROSTED GLASS & PANEL SURFACES",
        "theme.toml",
        "accent",
        btn_reset_surf);

    GtkWidget *grid_presets = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid_presets), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid_presets), 6);
    gtk_grid_set_row_spacing(GTK_GRID(grid_presets), 6);
    gtk_widget_set_hexpand(grid_presets, TRUE);

    for (size_t i = 0; i < N_SURFACE_PRESETS; i++) {
        GtkWidget *pbtn = gtk_button_new();
        gtk_widget_add_css_class(pbtn, "widget-row-card");
        gtk_widget_set_hexpand(pbtn, TRUE);
        g_object_set_data(G_OBJECT(pbtn), "surface-preset-index", GUINT_TO_POINTER((guint)i));
        self->surface_preset_buttons[i] = pbtn;

        GtkWidget *lbl = gtk_label_new(SURFACE_PRESETS[i].name);
        gtk_widget_add_css_class(lbl, "row-title");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
        gtk_button_set_child(GTK_BUTTON(pbtn), lbl);

        g_signal_connect(pbtn, "clicked", G_CALLBACK(on_surface_preset_clicked), self);
        gtk_grid_attach(GTK_GRID(grid_presets), pbtn, (int)(i % 2), (int)(i / 2), 1, 1);
    }
    gtk_box_append(GTK_BOX(card_glass), grid_presets);

    GtkWidget *gt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(gt_box, GTK_ALIGN_CENTER);
    GtkWidget *gt_swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(gt_swatch, 18, 18);
    gtk_widget_set_valign(gt_swatch, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(gt_swatch, "settings-glass-swatch");
    self->entry_glass_tint = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(self->entry_glass_tint), 7);
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_glass_tint), 8);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(self->entry_glass_tint), 9);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_glass_tint), "#121212");
    gtk_widget_set_valign(self->entry_glass_tint, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(gt_box), gt_swatch);
    gtk_box_append(GTK_BOX(gt_box), self->entry_glass_tint);
    gtk_box_append(GTK_BOX(card_glass), create_boxed_row(
        NULL,
        "Glass Tint Hex",
        "Base hue for panel, popovers, launcher & notifications",
        gt_box));

    GtkWidget *w_panel_op = create_scale_with_badge(
        0, 100, 1, self->saved_panel_opacity_pct, "%.0f%%", &self->scale_panel_opacity, NULL);
    gtk_box_append(GTK_BOX(card_glass), create_boxed_row(
        NULL,
        "Panel Bar Opacity",
        "panel_opacity (0–100%, default 58%)",
        w_panel_op));

    GtkWidget *w_glass_op = create_scale_with_badge(
        0, 100, 1, self->saved_glass_opacity_pct, "%.0f%%", &self->scale_glass_opacity, NULL);
    gtk_box_append(GTK_BOX(card_glass), create_boxed_row(
        NULL,
        "Popovers & Launcher Opacity",
        "glass_opacity (0–100%, default 62%)",
        w_glass_op));

    GtkWidget *w_glass_ctrl = create_scale_with_badge(
        0, 200, 5, self->saved_glass_ctrl_pct, "%.0f%%", &self->scale_glass_ctrl, NULL);
    gtk_box_append(GTK_BOX(card_glass), create_boxed_row(
        NULL,
        "Glass Control Contrast",
        "glass_control_contrast — inner wells, headers & buttons (default 100%)",
        w_glass_ctrl));

    GtkWidget *w_chip_op = create_scale_with_badge(
        0, 25, 1, self->saved_widget_chip_pct, "%.0f%%", &self->scale_widget_chip, NULL);
    gtk_box_append(GTK_BOX(card_glass), create_boxed_row(
        NULL,
        "Panel Widget Chip Backing",
        "widget_chip_opacity — subtle backing behind panel widgets (0–25%)",
        w_chip_op));

    GtkWidget *w_acc_glass = create_scale_with_badge(
        0, 25, 1, self->saved_accent_glass_pct, "%.0f%%", &self->scale_accent_glass, NULL);
    gtk_box_append(GTK_BOX(card_glass), create_boxed_row(
        NULL,
        "Accent Glass Tint",
        "accent_glass_tint — blend accent color into glass surfaces (0–25%)",
        w_acc_glass));

    return card_glass;
}

static GtkWidget *
theme_build_card_surface(SettingsPageTheme *self)
{
    /* ─── Card 4: Opaque Surfaces, Borders & Legibility ─── */
    GtkWidget *card_surface = create_section_card(
        SETTINGS_CARD_CENTER,
        "4. OPAQUE SURFACES, BORDERS & LEGIBILITY",
        NULL,
        NULL,
        NULL);

    GtkWidget *sb_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(sb_box, GTK_ALIGN_CENTER);
    GtkWidget *sb_swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(sb_swatch, 18, 18);
    gtk_widget_set_valign(sb_swatch, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(sb_swatch, "settings-surface-swatch");
    self->entry_surface_base = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(self->entry_surface_base), 7);
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_surface_base), 8);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(self->entry_surface_base), 9);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_surface_base), "#141414");
    gtk_widget_set_valign(self->entry_surface_base, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(sb_box), sb_swatch);
    gtk_box_append(GTK_BOX(sb_box), self->entry_surface_base);
    gtk_box_append(GTK_BOX(card_surface), create_boxed_row(
        NULL,
        "Opaque Surface Base Hex",
        "surface_base — Settings, Files, IDE & Player canvas (#141414)",
        sb_box));

    GtkWidget *w_surf_c = create_scale_with_badge(
        0, 200, 5, self->saved_surface_contrast_pct, "%.0f%%", &self->scale_surface_contrast, NULL);
    gtk_box_append(GTK_BOX(card_surface), create_boxed_row(
        NULL,
        "Surface Elevation Contrast",
        "surface_contrast — step contrast between cards, chrome & borders",
        w_surf_c));

    GtkWidget *w_bdr_op = create_scale_with_badge(
        0, 100, 5, self->saved_border_opacity_pct, "%.0f%%", &self->scale_border_opacity, NULL);
    gtk_box_append(GTK_BOX(card_surface), create_boxed_row(
        NULL,
        "Border Opacity",
        "border_opacity — structural hairlines (0–100%, synced to comp.toml)",
        w_bdr_op));

    GtkWidget *w_sec_fg = create_scale_with_badge(
        50, 150, 5, self->saved_secondary_fg_pct, "%.0f%%", &self->scale_secondary_fg, NULL);
    gtk_box_append(GTK_BOX(card_surface), create_boxed_row(
        NULL,
        "Secondary Text Contrast",
        "secondary_fg_contrast — subtitles & metadata legibility (50–150%)",
        w_sec_fg));

    return card_surface;
}

static void
theme_connect_signals(SettingsPageTheme *self)
{
    g_signal_connect(self->entry_hex, "changed", G_CALLBACK(on_hex_entry_changed), self);
    g_signal_connect(self->entry_ui_font, "changed", G_CALLBACK(on_ui_font_entry_changed), self);
    g_signal_connect(self->entry_glass_tint, "changed", G_CALLBACK(on_glass_tint_entry_changed), self);
    g_signal_connect(self->entry_surface_base, "changed", G_CALLBACK(on_surface_base_entry_changed), self);
    g_signal_connect(self->scale_font, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_font_heading, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_font_sub, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_font_badge, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_panel_opacity, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_glass_opacity, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_glass_ctrl, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_widget_chip, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_accent_glass, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_surface_contrast, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_border_opacity, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self->scale_secondary_fg, "value-changed", G_CALLBACK(on_scale_font_changed), self);
    g_signal_connect(self, "map", G_CALLBACK(on_theme_page_mapped), NULL);
}

GtkWidget *
build_page_theme(void)
{
    SettingsPageTheme *self = g_object_new(settings_page_theme_get_type(), NULL);
    GdkDisplay *display = gdk_display_get_default();

    theme_init_providers_and_config(self, display);

    GtkWidget *card_palette = theme_build_card_palette(self, display);
    GtkWidget *card_typo    = theme_build_card_typography(self);
    settings_bento_add_pair(GTK_WIDGET(self), card_palette, card_typo);

    GtkWidget *card_glass   = theme_build_card_glass(self);
    GtkWidget *card_surface = theme_build_card_surface(self);
    settings_bento_add_pair(GTK_WIDGET(self), card_glass, card_surface);

    /* Sync initial UI state & wire signals */
    self->syncing_ui = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(self->entry_hex), self->current_accent_hex);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_ui_font), self->saved_ui_font_family);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_glass_tint), self->current_glass_tint);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_surface_base), self->current_surface_base);
    sync_swatch_highlight(self);
    update_custom_swatch_css(self);
    self->syncing_ui = FALSE;

    theme_connect_signals(self);

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
