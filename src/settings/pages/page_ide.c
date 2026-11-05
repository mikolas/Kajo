#include "../settings_common.h"
#include "../settings_pages.h"
#include <gio/gio.h>
#include <string.h>

typedef struct {
    const char *id;
    const char *name;
    const char *subtitle;
    const char *bg_hex;
    const char *kw_hex;
    const char *fn_hex;
    const char *str_hex;
} IdeSchemePreset;

static const IdeSchemePreset IDE_SCHEMES[] = {
    {
        "kajo-metro-dark",
        "Kajo Metro Dark",
        "#101010 • Aero+Metro High-Contrast Default",
        "#101010", "#569cd6", "#dcdcaa", "#ce9178"
    },
    {
        "kajo-oled-pitch",
        "OLED Pitch Black",
        "#050505 • Zero-Black Neon Telemetry Tokens",
        "#050505", "#00d8f0", "#ffe66d", "#ff9e64"
    },
    {
        "kajo-monokai-metro",
        "Monokai Metro",
        "#121212 • Warm Monokai Pro Token Palette",
        "#121212", "#ff6188", "#a9dc76", "#ffd866"
    },
    {
        "kajo-nord-frost",
        "Nord Frost",
        "#14171d • Arctic Slate & Ice Blue Syntax",
        "#14171d", "#81a1c1", "#88c0d0", "#a3be8c"
    },
    {
        "kajo-amber-crt",
        "Amber Phosphor CRT",
        "#0c0900 • Retro Monochrome Gold Terminal",
        "#0c0900", "#ffd700", "#ffe066", "#ff8000"
    },
    {
        "Adwaita-dark",
        "System Adwaita Dark",
        "#1e1e1e • Standard GtkSourceView 5 Dark",
        "#1e1e1e", "#78aeed", "#f9f06b", "#8ff0a4"
    }
};

#define N_IDE_SCHEMES (sizeof(IDE_SCHEMES) / sizeof(IDE_SCHEMES[0]))

static const char *const IDE_FONT_FAMILIES[] = {
    "Fira Code",
    "JetBrains Mono",
    "Iosevka",
    "Source Code Pro",
    "Hack",
    "Monospace",
    NULL
};

static const char *const IDE_TAB_LABELS[] = {
    "2 Spaces (Kajo / GTK Standard)",
    "4 Spaces",
    "8 Spaces (Linux Kernel Style)",
    NULL
};

typedef struct {
    GtkWidget      *scheme_buttons[N_IDE_SCHEMES];
    GtkCssProvider *scheme_providers[N_IDE_SCHEMES];
    GtkWidget      *preview_label;
    GtkWidget      *combo_font;
    GtkWidget      *scale_font_size;
    GtkWidget      *lbl_font_size_val;
    GtkWidget      *combo_tab_width;
    GtkWidget      *sw_minimap;
    GtkWidget      *sw_gutter;
    GtkWidget      *sw_cur_line;
    guint           current_scheme_idx;
    gboolean        syncing_ui;
} IdeSettingsState;

static void
ide_settings_state_free(gpointer data)
{
    IdeSettingsState *st = data;
    if (!st) return;
    GdkDisplay *display = gdk_display_get_default();
    for (size_t i = 0; i < N_IDE_SCHEMES; i++) {
        if (st->scheme_providers[i]) {
            if (display) {
                gtk_style_context_remove_provider_for_display(
                    display, GTK_STYLE_PROVIDER(st->scheme_providers[i]));
            }
            g_clear_object(&st->scheme_providers[i]);
        }
    }
    g_free(st);
}

static void
update_code_preview_markup(IdeSettingsState *st)
{
    if (!st || !st->preview_label) return;
    guint idx = st->current_scheme_idx < N_IDE_SCHEMES ? st->current_scheme_idx : 0;
    const IdeSchemePreset *s = &IDE_SCHEMES[idx];
    guint f_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->combo_font));
    const char *font_fam = (f_idx < 6 && IDE_FONT_FAMILIES[f_idx]) ? IDE_FONT_FAMILIES[f_idx] : "Fira Code";
    int font_px = (int)gtk_range_get_value(GTK_RANGE(st->scale_font_size));
    if (font_px < 9 || font_px > 28) font_px = 13;

    g_autofree char *markup = g_strdup_printf(
        "<span font_family=\"%s\" size=\"%d\">"
        "<span foreground=\"#6a9955\" style=\"italic\">/* Live Scheme &amp; Font Preview (%s • %dpx) */</span>\n"
        "<span foreground=\"%s\" weight=\"bold\">static gboolean</span> "
        "<span foreground=\"%s\">kajo_ide_activate</span>(<span foreground=\"#4ec9b0\">KajoIdeWindow</span> *self)\n"
        "{\n"
        "  <span foreground=\"%s\">ide_status_notify</span>(self, <span foreground=\"%s\">\"SCHEME: %s\"</span>);\n"
        "  <span foreground=\"%s\" weight=\"bold\">return</span> <span foreground=\"%s\" weight=\"bold\">TRUE</span>;\n"
        "}</span>",
        font_fam, font_px * PANGO_SCALE,
        s->name, font_px,
        s->kw_hex, s->fn_hex, s->fn_hex, s->str_hex, s->id, s->kw_hex, s->kw_hex);

    gtk_label_set_markup(GTK_LABEL(st->preview_label), markup);
}

static void
save_ide_settings(IdeSettingsState *st)
{
    if (!st || st->syncing_ui) return;

    guint s_idx = st->current_scheme_idx < N_IDE_SCHEMES ? st->current_scheme_idx : 0;
    guint f_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->combo_font));
    const char *font_fam = (f_idx < 6 && IDE_FONT_FAMILIES[f_idx]) ? IDE_FONT_FAMILIES[f_idx] : "Fira Code";
    int font_px = CLAMP((int)gtk_range_get_value(GTK_RANGE(st->scale_font_size)), 9, 28);
    guint t_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->combo_tab_width));
    int tab_w = (t_idx == 1) ? 4 : ((t_idx == 2) ? 8 : 2);

    ShellConfig *config = shell_config_load(NULL);
    if (!config)
        config = shell_config_get_default();
    if (config) {
        g_free(config->ide_color_scheme);
        config->ide_color_scheme = g_strdup(IDE_SCHEMES[s_idx].id);
        g_free(config->ide_font_family);
        config->ide_font_family = g_strdup(font_fam);
        config->ide_font_size_px = font_px;
        config->ide_tab_width = tab_w;
        config->ide_show_minimap = gtk_switch_get_active(GTK_SWITCH(st->sw_minimap));
        config->ide_show_line_numbers = gtk_switch_get_active(GTK_SWITCH(st->sw_gutter));
        config->ide_highlight_current_line = gtk_switch_get_active(GTK_SWITCH(st->sw_cur_line));

        shell_theme_sync_from_config(config);

        g_autofree gchar *cfg_path = g_build_filename(g_get_user_config_dir(), "kajo", "config.json", NULL);
        shell_config_save(config, cfg_path);
        shell_config_destroy(config);
    }

    settings_reload_global_theme();
    update_code_preview_markup(st);
}

static void
select_ide_scheme(IdeSettingsState *st, guint idx, gboolean persist)
{
    if (!st || idx >= N_IDE_SCHEMES) return;
    st->current_scheme_idx = idx;
    for (size_t i = 0; i < N_IDE_SCHEMES; i++) {
        if (!st->scheme_buttons[i]) continue;
        if (i == idx) {
            gtk_widget_add_css_class(st->scheme_buttons[i], "active");
        } else {
            gtk_widget_remove_css_class(st->scheme_buttons[i], "active");
        }
    }
    update_code_preview_markup(st);
    if (persist) {
        save_ide_settings(st);
    }
}

static void
on_scheme_card_clicked(GtkButton *btn, gpointer user_data)
{
    IdeSettingsState *st = user_data;
    guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "scheme-index"));
    select_ide_scheme(st, idx, TRUE);
}

static void
on_ide_font_size_changed(GtkRange *range, gpointer user_data)
{
    IdeSettingsState *st = user_data;
    int px = (int)gtk_range_get_value(range);
    g_autofree char *txt = g_strdup_printf("%dpx", px);
    gtk_label_set_text(GTK_LABEL(st->lbl_font_size_val), txt);
    save_ide_settings(st);
}

static void
on_ide_dropdown_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    IdeSettingsState *st = user_data;
    save_ide_settings(st);
}

static void
on_ide_switch_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    IdeSettingsState *st = user_data;
    save_ide_settings(st);
}

static void
on_reset_ide_font_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    IdeSettingsState *st = user_data;
    st->syncing_ui = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(st->combo_font), 0);
    gtk_range_set_value(GTK_RANGE(st->scale_font_size), 13);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(st->combo_tab_width), 0);
    gtk_label_set_text(GTK_LABEL(st->lbl_font_size_val), "13px");
    st->syncing_ui = FALSE;
    save_ide_settings(st);
}

GtkWidget *
build_page_ide(void)
{
    IdeSettingsState *st = g_new0(IdeSettingsState, 1);
    st->syncing_ui = TRUE;

    ShellConfig *config = shell_config_load(NULL);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    g_object_set_data_full(G_OBJECT(root), "ide-settings-state", st, ide_settings_state_free);

    /* ─── SECTION 1: SYNTAX COLOR SCHEMES ─── */
    GtkWidget *lbl_sec1 = gtk_label_new("SYNTAX COLOR SCHEME (GTKSOURCEVIEW 5)");
    gtk_label_set_xalign(GTK_LABEL(lbl_sec1), 0.0f);
    gtk_widget_add_css_class(lbl_sec1, "settings-section-title");
    gtk_box_append(GTK_BOX(root), lbl_sec1);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    guint init_scheme_idx = 0;
    const char *cfg_scheme = (config && config->ide_color_scheme) ? config->ide_color_scheme : "kajo-metro-dark";

    for (size_t i = 0; i < N_IDE_SCHEMES; i++) {
        if (g_ascii_strcasecmp(cfg_scheme, IDE_SCHEMES[i].id) == 0) {
            init_scheme_idx = (guint)i;
        }

        GtkWidget *card = gtk_button_new();
        gtk_widget_add_css_class(card, "widget-row-card");
        gtk_widget_set_hexpand(card, TRUE);
        g_object_set_data(G_OBJECT(card), "scheme-index", GUINT_TO_POINTER((guint)i));
        st->scheme_buttons[i] = card;

        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

        /* 4-band mini syntax swatch */
        GtkWidget *swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(swatch, 34, 22);
        gtk_widget_set_valign(swatch, GTK_ALIGN_CENTER);
        g_autofree char *sw_cls = g_strdup_printf("ide-scheme-swatch-%zu", i);
        gtk_widget_add_css_class(swatch, sw_cls);

        g_autofree char *sw_css = g_strdup_printf(
            ".%s { background: linear-gradient(90deg, %s 0%%, %s 45%%, %s 45%%, %s 72%%, %s 72%%, %s 100%%); "
            "border: 1px solid rgba(255,255,255,0.28); border-radius: 0px; }",
            sw_cls,
            IDE_SCHEMES[i].bg_hex, IDE_SCHEMES[i].bg_hex,
            IDE_SCHEMES[i].kw_hex, IDE_SCHEMES[i].kw_hex,
            IDE_SCHEMES[i].fn_hex, IDE_SCHEMES[i].fn_hex);

        GtkCssProvider *p = gtk_css_provider_new();
        gtk_css_provider_load_from_string(p, sw_css);
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        st->scheme_providers[i] = p;

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(vbox, TRUE);

        GtkWidget *name_lbl = gtk_label_new(IDE_SCHEMES[i].name);
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
        gtk_widget_add_css_class(name_lbl, "row-title");

        GtkWidget *sub_lbl = gtk_label_new(IDE_SCHEMES[i].subtitle);
        gtk_label_set_xalign(GTK_LABEL(sub_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(sub_lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(sub_lbl, "row-subtitle");

        gtk_box_append(GTK_BOX(vbox), name_lbl);
        gtk_box_append(GTK_BOX(vbox), sub_lbl);

        gtk_box_append(GTK_BOX(hbox), swatch);
        gtk_box_append(GTK_BOX(hbox), vbox);
        gtk_button_set_child(GTK_BUTTON(card), hbox);

        g_signal_connect(card, "clicked", G_CALLBACK(on_scheme_card_clicked), st);
        gtk_grid_attach(GTK_GRID(grid), card, (int)(i % 2), (int)(i / 2), 1, 1);
    }
    gtk_box_append(GTK_BOX(root), grid);

    /* Live Syntax & Font Preview Box */
    st->preview_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(st->preview_label), 0.0f);
    gtk_widget_add_css_class(st->preview_label, "code-preview-box");
    gtk_box_append(GTK_BOX(root), st->preview_label);

    /* ─── SECTION 2: EDITOR TYPOGRAPHY & INDENTATION ─── */
    GtkWidget *lbl_sec2 = gtk_label_new("EDITOR TYPOGRAPHY & INDENTATION");
    gtk_label_set_xalign(GTK_LABEL(lbl_sec2), 0.0f);
    gtk_widget_add_css_class(lbl_sec2, "settings-section-title");
    gtk_box_append(GTK_BOX(root), lbl_sec2);

    st->combo_font = create_string_dropdown(IDE_FONT_FAMILIES);
    guint init_font_idx = 0;
    if (config && config->ide_font_family) {
        for (guint i = 0; IDE_FONT_FAMILIES[i] != NULL; i++) {
            if (g_ascii_strcasecmp(config->ide_font_family, IDE_FONT_FAMILIES[i]) == 0) {
                init_font_idx = i;
                break;
            }
        }
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(st->combo_font), init_font_idx);
    gtk_box_append(GTK_BOX(root),
                   create_action_row("preferences-desktop-font-symbolic",
                                     "Monospace Font Family",
                                     "Primary code canvas, gutter & tree font in kajo-ide",
                                     st->combo_font));

    /* Font size slider card */
    GtkWidget *size_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(size_box, GTK_ALIGN_CENTER);

    int init_px = (config && config->ide_font_size_px >= 9 && config->ide_font_size_px <= 28)
                      ? config->ide_font_size_px
                      : 13;
    st->scale_font_size = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 10.0, 24.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(st->scale_font_size), FALSE);
    gtk_widget_set_size_request(st->scale_font_size, 180, -1);
    gtk_widget_set_valign(st->scale_font_size, GTK_ALIGN_CENTER);
    gtk_range_set_value(GTK_RANGE(st->scale_font_size), init_px);

    g_autofree char *init_px_str = g_strdup_printf("%dpx", init_px);
    st->lbl_font_size_val = gtk_label_new(init_px_str);
    gtk_widget_add_css_class(st->lbl_font_size_val, "shell-popover-badge");
    gtk_widget_add_css_class(st->lbl_font_size_val, "accent");
    gtk_widget_set_valign(st->lbl_font_size_val, GTK_ALIGN_CENTER);

    GtkWidget *btn_reset = gtk_button_new_with_label("RESET (13PX)");
    gtk_widget_add_css_class(btn_reset, "files-nav-btn");
    gtk_widget_set_valign(btn_reset, GTK_ALIGN_CENTER);
    g_signal_connect(btn_reset, "clicked", G_CALLBACK(on_reset_ide_font_clicked), st);

    gtk_box_append(GTK_BOX(size_box), st->scale_font_size);
    gtk_box_append(GTK_BOX(size_box), st->lbl_font_size_val);
    gtk_box_append(GTK_BOX(size_box), btn_reset);

    gtk_box_append(GTK_BOX(root),
                   create_action_row("zoom-in-symbolic",
                                     "Editor Font Size",
                                     "Live zoom also available in kajo-ide via Ctrl++ / Ctrl+- / Ctrl+0",
                                     size_box));

    st->combo_tab_width = create_string_dropdown(IDE_TAB_LABELS);
    guint init_tab_idx = 0;
    if (config) {
        if (config->ide_tab_width == 4) init_tab_idx = 1;
        else if (config->ide_tab_width == 8) init_tab_idx = 2;
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(st->combo_tab_width), init_tab_idx);
    gtk_box_append(GTK_BOX(root),
                   create_action_row("format-indent-more-symbolic",
                                     "Indentation & Tab Width",
                                     "Spaces inserted per Tab stop in GtkSourceView",
                                     st->combo_tab_width));

    /* ─── SECTION 3: WORKSPACE CHROME & GUTTER ─── */
    GtkWidget *lbl_sec3 = gtk_label_new("WORKSPACE CHROME & GUTTER");
    gtk_label_set_xalign(GTK_LABEL(lbl_sec3), 0.0f);
    gtk_widget_add_css_class(lbl_sec3, "settings-section-title");
    gtk_box_append(GTK_BOX(root), lbl_sec3);

    st->sw_minimap = gtk_switch_new();
    gtk_widget_set_valign(st->sw_minimap, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(st->sw_minimap), config ? config->ide_show_minimap : TRUE);
    gtk_box_append(GTK_BOX(root),
                   create_action_row("view-grid-symbolic",
                                     "Show Code Overview Minimap",
                                     "Right-docked GtkSourceMap overview strip (toggleable via 🗺 MAP)",
                                     st->sw_minimap));

    st->sw_gutter = gtk_switch_new();
    gtk_widget_set_valign(st->sw_gutter, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(st->sw_gutter), config ? config->ide_show_line_numbers : TRUE);
    gtk_box_append(GTK_BOX(root),
                   create_action_row("view-list-symbolic",
                                     "Show 6-Column Cairo Gutter",
                                     "Breakpoints, LSP diagnostics, DAP PC arrow, line numbers, Git diff & fold",
                                     st->sw_gutter));

    st->sw_cur_line = gtk_switch_new();
    gtk_widget_set_valign(st->sw_cur_line, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(st->sw_cur_line), config ? config->ide_highlight_current_line : TRUE);
    gtk_box_append(GTK_BOX(root),
                   create_action_row("format-text-underline-symbolic",
                                     "Highlight Active Cursor Line",
                                     "Subtle surface tint on the current editor cursor line",
                                     st->sw_cur_line));

    select_ide_scheme(st, init_scheme_idx, FALSE);
    st->syncing_ui = FALSE;

    if (config) shell_config_destroy(config);

    g_signal_connect(st->combo_font, "notify::selected", G_CALLBACK(on_ide_dropdown_changed), st);
    g_signal_connect(st->scale_font_size, "value-changed", G_CALLBACK(on_ide_font_size_changed), st);
    g_signal_connect(st->combo_tab_width, "notify::selected", G_CALLBACK(on_ide_dropdown_changed), st);
    g_signal_connect(st->sw_minimap, "notify::active", G_CALLBACK(on_ide_switch_changed), st);
    g_signal_connect(st->sw_gutter, "notify::active", G_CALLBACK(on_ide_switch_changed), st);
    g_signal_connect(st->sw_cur_line, "notify::active", G_CALLBACK(on_ide_switch_changed), st);

    return create_settings_page_card("KAJO IDE & EDITOR", "[ ORG.KAJO.IDE ]", root, NULL);
}
