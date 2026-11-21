#include "../settings_common.h"
#include "../settings_pages.h"
#include <gio/gio.h>
#include <string.h>

typedef struct {
    const char *id;
    const char *name;
    const char *short_badge;
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
        "METRO DARK",
        "#101010 • Aero+Metro High-Contrast Default",
        "#101010", "#569cd6", "#dcdcaa", "#ce9178"
    },
    {
        "kajo-oled-pitch",
        "OLED Pitch Black",
        "OLED PITCH",
        "#050505 • Zero-Black Neon Telemetry Tokens",
        "#050505", "#00d8f0", "#ffe66d", "#ff9e64"
    },
    {
        "kajo-monokai-metro",
        "Monokai Metro",
        "MONOKAI",
        "#121212 • Warm Monokai Pro Token Palette",
        "#121212", "#ff6188", "#a9dc76", "#ffd866"
    },
    {
        "kajo-nord-frost",
        "Nord Frost",
        "NORD FROST",
        "#14171d • Arctic Slate & Ice Blue Syntax",
        "#14171d", "#81a1c1", "#88c0d0", "#a3be8c"
    },
    {
        "kajo-amber-crt",
        "Amber Phosphor CRT",
        "AMBER CRT",
        "#0c0900 • Retro Monochrome Gold Terminal",
        "#0c0900", "#ffd700", "#ffe066", "#ff8000"
    },
    {
        "Adwaita-dark",
        "System Adwaita Dark",
        "ADWAITA",
        "#1e1e1e • Standard GtkSourceView 5 Dark",
        "#1e1e1e", "#78aeed", "#f9f06b", "#8ff0a4"
    }
};

#define N_IDE_SCHEMES      (sizeof(IDE_SCHEMES) / sizeof(IDE_SCHEMES[0]))
#define N_PREVIEW_LINES    5

static const char *const IDE_FONT_FAMILIES[] = {
    "Fira Code",
    "JetBrains Mono",
    "Iosevka",
    "Source Code Pro",
    "Hack",
    "Monospace",
    NULL
};

static const char *const IDE_TAB_LABELS[] = { "2", "4", "8", NULL };

static const char *const IDE_REMAP_STATES[] = {
    "normal",
    "select",
    "tree",
    "drawer",
    NULL
};

typedef struct _SettingsPageIde {
    GtkBox parent_instance;

    GtkWidget      *scheme_buttons[N_IDE_SCHEMES];
    GtkCssProvider *scheme_providers[N_IDE_SCHEMES];
    GtkCssProvider *preview_bg_provider;
    GtkWidget      *preview_box;
    GtkWidget      *preview_lines[N_PREVIEW_LINES];

    GtkWidget      *combo_font;
    GtkWidget      *scale_font_size;
    GtkWidget      *seg_tab_width;   /* segmented: 2 | 4 | 8 */
    GtkWidget      *sw_spaces;
    GtkWidget      *sw_word_wrap;
    GtkWidget      *sw_margin_guide;
    GtkWidget      *sw_minimap;
    GtkWidget      *sw_gutter;
    GtkWidget      *sw_rel_lines;
    GtkWidget      *sw_cur_line;

    GtkWidget      *card_remaps;
    GtkWidget      *remap_list_box;
    GtkWidget      *combo_remap_state;
    GtkWidget      *entry_remap_key;
    GtkWidget      *entry_remap_action;
    GtkWidget      *btn_remap_add;

    GPtrArray      *draft_remaps;    /* KajoIdeKeyRemap* */
    GPtrArray      *saved_remaps;    /* KajoIdeKeyRemap* */

    guint           current_scheme_idx;
    guint           saved_scheme_idx;
    guint           saved_font_idx;
    int             saved_font_px;
    guint           saved_tab_idx;
    gboolean        saved_spaces;
    gboolean        saved_wrap;
    gboolean        saved_margin;
    gboolean        saved_minimap;
    gboolean        saved_gutter;
    gboolean        saved_rel_lines;
    gboolean        saved_cur_line;

    gboolean        syncing_ui;
} SettingsPageIde;

typedef struct _SettingsPageIdeClass {
    GtkBoxClass parent_class;
} SettingsPageIdeClass;

G_DEFINE_TYPE(SettingsPageIde, settings_page_ide, GTK_TYPE_BOX)

static gint
compare_ide_remaps(gconstpointer pa, gconstpointer pb)
{
    const KajoIdeKeyRemap *a = *(const KajoIdeKeyRemap *const *)pa;
    const KajoIdeKeyRemap *b = *(const KajoIdeKeyRemap *const *)pb;
    if (a == b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;
    gint cmp = g_strcmp0(a->state ? a->state : "", b->state ? b->state : "");
    if (cmp != 0)
        return cmp;
    cmp = g_strcmp0(a->key ? a->key : "", b->key ? b->key : "");
    if (cmp != 0)
        return cmp;
    return g_strcmp0(a->action ? a->action : "", b->action ? b->action : "");
}

static GPtrArray *
clone_remap_array(const GPtrArray *src)
{
    GPtrArray *dst = g_ptr_array_new_with_free_func(kajo_ide_key_remap_free);
    if (!src)
        return dst;
    for (guint i = 0; i < src->len; i++) {
        const KajoIdeKeyRemap *r = g_ptr_array_index(src, i);
        if (r)
            g_ptr_array_add(dst, kajo_ide_key_remap_copy(r));
    }
    g_ptr_array_sort(dst, compare_ide_remaps);
    return dst;
}

static gboolean
remap_arrays_equal(const GPtrArray *a, const GPtrArray *b)
{
    guint la = a ? a->len : 0;
    guint lb = b ? b->len : 0;
    if (la != lb)
        return FALSE;
    for (guint i = 0; i < la; i++) {
        const KajoIdeKeyRemap *ra = g_ptr_array_index(a, i);
        const KajoIdeKeyRemap *rb = g_ptr_array_index(b, i);
        if (!kajo_ide_key_remap_equal(ra, rb))
            return FALSE;
    }
    return TRUE;
}

static void update_ide_telemetry_and_dirty(SettingsPageIde *self);
static void rebuild_remap_rows(SettingsPageIde *self);

static void
on_remap_remove_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageIde *self = (SettingsPageIde *)user_data;
    if (!self || !self->draft_remaps)
        return;

    guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "remap-index"));
    if (idx >= self->draft_remaps->len)
        return;

    g_ptr_array_remove_index(self->draft_remaps, idx);
    rebuild_remap_rows(self);
    update_ide_telemetry_and_dirty(self);
}

static void
on_remap_add_clicked(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageIde *self = (SettingsPageIde *)user_data;
    if (!self || !self->draft_remaps)
        return;

    guint st_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_remap_state));
    const char *state = (st_idx < 4 && IDE_REMAP_STATES[st_idx]) ? IDE_REMAP_STATES[st_idx] : "normal";

    g_autofree gchar *key = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_remap_key)));
    g_autofree gchar *action = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_remap_action)));
    g_strstrip(key);
    g_strstrip(action);

    if (!key[0] || !action[0])
        return;

    gboolean replaced = FALSE;
    for (guint i = 0; i < self->draft_remaps->len; i++) {
        KajoIdeKeyRemap *existing = g_ptr_array_index(self->draft_remaps, i);
        if (existing &&
            g_strcmp0(existing->state, state) == 0 &&
            g_strcmp0(existing->key, key) == 0) {
            g_free(existing->action);
            existing->action = g_strdup(action);
            replaced = TRUE;
            break;
        }
    }

    if (!replaced)
        g_ptr_array_add(self->draft_remaps, kajo_ide_key_remap_new(state, key, action));

    g_ptr_array_sort(self->draft_remaps, compare_ide_remaps);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_remap_key), "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_remap_action), "");

    rebuild_remap_rows(self);
    update_ide_telemetry_and_dirty(self);
}

static void
rebuild_remap_rows(SettingsPageIde *self)
{
    if (!self || !self->remap_list_box)
        return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->remap_list_box)) != NULL) {
        gtk_box_remove(GTK_BOX(self->remap_list_box), child);
    }

    guint count = self->draft_remaps ? self->draft_remaps->len : 0;
    if (self->card_remaps) {
        g_autofree gchar *badge = g_strdup_printf("%u %s", count, count == 1 ? "REMAP" : "REMAPS");
        section_card_set_badge(self->card_remaps, badge, count > 0 ? "accent" : NULL);
    }

    if (count == 0) {
        gtk_box_append(GTK_BOX(self->remap_list_box),
                       create_empty_state_label("No custom key remappings — add a state, key, and action below"));
        return;
    }

    for (guint i = 0; i < count; i++) {
        const KajoIdeKeyRemap *r = g_ptr_array_index(self->draft_remaps, i);
        if (!r)
            continue;

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(row, "widget-row-card");
        gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(row, TRUE);

        g_autofree gchar *up_state = g_ascii_strup(r->state ? r->state : "normal", -1);
        g_autofree gchar *badge_txt = g_strdup_printf("[%s]", up_state);
        GtkWidget *badge = gtk_label_new(badge_txt);
        gtk_widget_add_css_class(badge, "shell-popover-badge");
        gtk_widget_set_hexpand(badge, FALSE);
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

        g_autofree gchar *map_txt = g_strdup_printf("%s  →  %s",
                                                    r->key ? r->key : "",
                                                    r->action ? r->action : "");
        GtkWidget *lbl = gtk_label_new(map_txt);
        gtk_widget_add_css_class(lbl, "row-title");
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_widget_set_halign(lbl, GTK_ALIGN_FILL);
        gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(lbl), 4);
        gtk_widget_set_tooltip_text(lbl, map_txt);

        GtkWidget *btn_rem = gtk_button_new_with_label("✕");
        gtk_widget_add_css_class(btn_rem, "files-nav-btn");
        gtk_widget_add_css_class(btn_rem, "widget-item-btn");
        gtk_widget_add_css_class(btn_rem, "action-btn-danger");
        gtk_widget_set_hexpand(btn_rem, FALSE);
        gtk_widget_set_valign(btn_rem, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(btn_rem, "Remove Key Remapping");
        g_object_set_data(G_OBJECT(btn_rem), "remap-index", GUINT_TO_POINTER(i));
        g_signal_connect(btn_rem, "clicked", G_CALLBACK(on_remap_remove_clicked), self);

        gtk_box_append(GTK_BOX(row), badge);
        gtk_box_append(GTK_BOX(row), lbl);
        gtk_box_append(GTK_BOX(row), btn_rem);
        gtk_box_append(GTK_BOX(self->remap_list_box), row);
    }
}

static void
update_code_preview_markup(SettingsPageIde *self)
{
    if (!self || !self->preview_lines[0])
        return;

    guint idx = self->current_scheme_idx < N_IDE_SCHEMES ? self->current_scheme_idx : 0;
    const IdeSchemePreset *s = &IDE_SCHEMES[idx];
    guint f_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_font));
    const char *font_fam = (f_idx < 6 && IDE_FONT_FAMILIES[f_idx]) ? IDE_FONT_FAMILIES[f_idx] : "Fira Code";
    int font_px = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_size));
    if (font_px < 9 || font_px > 28)
        font_px = 13;

    if (self->preview_bg_provider) {
        g_autofree gchar *bg_css = g_strdup_printf(
            "window.shell-settings-app .ide-live-preview-box { background-color: %s; border-left-color: %s; }",
            s->bg_hex, s->kw_hex);
        gtk_css_provider_load_from_string(self->preview_bg_provider, bg_css);
    }

    /* Same family stack as the editor CSS, so a missing font falls back to the
     * same face (Fira Code / FiraCode Nerd Font / monospace) instead of the
     * proportional UI font. */
    g_autofree char *font_stack = shell_ide_font_stack(font_fam);
    /* The IDE sizes its editor in CSS px, but Pango's `size` is in points
     * (1/1024 pt): 1 px = 0.75 pt at GTK's 96 dpi. Without this conversion
     * the preview rendered ~33% larger than the real editor. */
    int pango_sz = (font_px * 3 * PANGO_SCALE) / 4;

    g_autofree char *l0 = g_strdup_printf(
        "<span font_family=\"%s\" size=\"%d\" foreground=\"#6a9955\" style=\"italic\">"
        "/* Live Preview (%s • %dpx) */</span>",
        font_stack, pango_sz, s->name, font_px);
    g_autofree char *l1 = g_strdup_printf(
        "<span font_family=\"%s\" size=\"%d\">"
        "<span foreground=\"%s\" weight=\"bold\">static gboolean</span> "
        "<span foreground=\"%s\">kajo_ide_activate</span>(<span foreground=\"#4ec9b0\">KajoIdeWindow</span> *self) {"
        "</span>",
        font_stack, pango_sz, s->kw_hex, s->fn_hex);
    g_autofree char *l2 = g_strdup_printf(
        "<span font_family=\"%s\" size=\"%d\">"
        "  <span foreground=\"%s\">ide_status_notify</span>(self, <span foreground=\"%s\">\"SCHEME: %s\"</span>);"
        "</span>",
        font_stack, pango_sz, s->fn_hex, s->str_hex, s->id);
    g_autofree char *l3 = g_strdup_printf(
        "<span font_family=\"%s\" size=\"%d\">"
        "  <span foreground=\"%s\" weight=\"bold\">return</span> "
        "<span foreground=\"%s\" weight=\"bold\">TRUE</span>;"
        "</span>",
        font_stack, pango_sz, s->kw_hex, s->kw_hex);
    g_autofree char *l4 = g_strdup_printf(
        "<span font_family=\"%s\" size=\"%d\">}</span>",
        font_stack, pango_sz);

    gtk_label_set_markup(GTK_LABEL(self->preview_lines[0]), l0);
    gtk_label_set_markup(GTK_LABEL(self->preview_lines[1]), l1);
    gtk_label_set_markup(GTK_LABEL(self->preview_lines[2]), l2);
    gtk_label_set_markup(GTK_LABEL(self->preview_lines[3]), l3);
    gtk_label_set_markup(GTK_LABEL(self->preview_lines[4]), l4);
}

static gboolean
compute_ide_dirty(SettingsPageIde *self)
{
    guint f_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_font));
    int font_px = (int)gtk_range_get_value(GTK_RANGE(self->scale_font_size));
    guint t_idx = segmented_group_get_selected(self->seg_tab_width);

    if (self->current_scheme_idx != self->saved_scheme_idx) return TRUE;
    if (f_idx != self->saved_font_idx) return TRUE;
    if (font_px != self->saved_font_px) return TRUE;
    if (t_idx != self->saved_tab_idx) return TRUE;
    if (gtk_switch_get_active(GTK_SWITCH(self->sw_spaces)) != self->saved_spaces) return TRUE;
    if (gtk_switch_get_active(GTK_SWITCH(self->sw_word_wrap)) != self->saved_wrap) return TRUE;
    if (gtk_switch_get_active(GTK_SWITCH(self->sw_margin_guide)) != self->saved_margin) return TRUE;
    if (gtk_switch_get_active(GTK_SWITCH(self->sw_minimap)) != self->saved_minimap) return TRUE;
    if (gtk_switch_get_active(GTK_SWITCH(self->sw_gutter)) != self->saved_gutter) return TRUE;
    if (gtk_switch_get_active(GTK_SWITCH(self->sw_rel_lines)) != self->saved_rel_lines) return TRUE;
    if (gtk_switch_get_active(GTK_SWITCH(self->sw_cur_line)) != self->saved_cur_line) return TRUE;
    if (!remap_arrays_equal(self->draft_remaps, self->saved_remaps)) return TRUE;
    return FALSE;
}

static void
update_ide_telemetry_and_dirty(SettingsPageIde *self)
{
    if (!self)
        return;

    guint s_idx = self->current_scheme_idx < N_IDE_SCHEMES ? self->current_scheme_idx : 0;
    guint f_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_font));
    const char *font_fam = (f_idx < 6 && IDE_FONT_FAMILIES[f_idx]) ? IDE_FONT_FAMILIES[f_idx] : "Fira Code";
    int font_px = CLAMP((int)gtk_range_get_value(GTK_RANGE(self->scale_font_size)), 9, 28);
    guint t_idx = segmented_group_get_selected(self->seg_tab_width);
    int tab_w = (t_idx == 1) ? 4 : ((t_idx == 2) ? 8 : 2);
    gboolean spaces = gtk_switch_get_active(GTK_SWITCH(self->sw_spaces));
    gboolean gutter = gtk_switch_get_active(GTK_SWITCH(self->sw_gutter));
    gboolean rel_lines = gtk_switch_get_active(GTK_SWITCH(self->sw_rel_lines));

    g_autofree gchar *up_font = g_ascii_strup(font_fam, -1);
    g_autofree gchar *c2 = g_strdup_printf("%s %dPX", up_font, font_px);
    g_autofree gchar *c3 = g_strdup_printf("%d %s", tab_w, spaces ? "SPACES" : "TABS");

    settings_page_set_counter(GTK_WIDGET(self), 0, "SCHEME", IDE_SCHEMES[s_idx].short_badge, "accent");
    settings_page_set_counter(GTK_WIDGET(self), 1, "FONT", c2, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 2, "TAB WIDTH", c3, NULL);
    settings_page_set_counter(GTK_WIDGET(self), 3, "GUTTER",
                              gutter ? (rel_lines ? "6-COL REL" : "6-COL CAIRO") : "HIDDEN",
                              gutter ? "success" : NULL);

    settings_page_set_nav_badge(GTK_WIDGET(self), "ide", "DARK", NULL);

    /* Reflects the saved/unsaved state in the footer. Edits only change the live
     * preview; nothing is written to config.json until SAVE & APPLY is pressed. */
    settings_page_set_dirty(GTK_WIDGET(self), compute_ide_dirty(self));
}

static void
select_ide_scheme(SettingsPageIde *self, guint idx, gboolean mark_changed)
{
    if (!self || idx >= N_IDE_SCHEMES)
        return;
    self->current_scheme_idx = idx;
    for (size_t i = 0; i < N_IDE_SCHEMES; i++) {
        if (!self->scheme_buttons[i])
            continue;
        if (i == idx)
            gtk_widget_add_css_class(self->scheme_buttons[i], "active");
        else
            gtk_widget_remove_css_class(self->scheme_buttons[i], "active");
    }
    update_code_preview_markup(self);
    if (mark_changed && !self->syncing_ui)
        update_ide_telemetry_and_dirty(self);
}

static void
on_scheme_card_clicked(GtkButton *btn, gpointer user_data)
{
    SettingsPageIde *self = (SettingsPageIde *)user_data;
    guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "scheme-index"));
    select_ide_scheme(self, idx, TRUE);
}

static void
on_ide_control_changed(SettingsPageIde *self)
{
    if (!self || self->syncing_ui)
        return;
    update_code_preview_markup(self);
    update_ide_telemetry_and_dirty(self);
}

static void
on_ide_font_size_changed(GtkRange *range G_GNUC_UNUSED, gpointer user_data)
{
    on_ide_control_changed((SettingsPageIde *)user_data);
}

static void
on_ide_dropdown_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    on_ide_control_changed((SettingsPageIde *)user_data);
}

static void
on_ide_segment_changed(GtkWidget *group G_GNUC_UNUSED, guint selected G_GNUC_UNUSED, gpointer user_data)
{
    on_ide_control_changed((SettingsPageIde *)user_data);
}

static void
on_ide_switch_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    on_ide_control_changed((SettingsPageIde *)user_data);
}

static void
action_reset_ide_defaults(gpointer user_data)
{
    SettingsPageIde *self = (SettingsPageIde *)user_data;
    if (!self)
        return;

    self->syncing_ui = TRUE;
    select_ide_scheme(self, 0, FALSE);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_font), 0);
    gtk_range_set_value(GTK_RANGE(self->scale_font_size), 13.0);
    segmented_group_set_selected(self->seg_tab_width, 0);
    gtk_switch_set_active(GTK_SWITCH(self->sw_spaces), TRUE);
    gtk_switch_set_active(GTK_SWITCH(self->sw_word_wrap), FALSE);
    gtk_switch_set_active(GTK_SWITCH(self->sw_margin_guide), FALSE);
    gtk_switch_set_active(GTK_SWITCH(self->sw_minimap), TRUE);
    gtk_switch_set_active(GTK_SWITCH(self->sw_gutter), TRUE);
    gtk_switch_set_active(GTK_SWITCH(self->sw_rel_lines), FALSE);
    gtk_switch_set_active(GTK_SWITCH(self->sw_cur_line), TRUE);
    if (self->draft_remaps)
        g_ptr_array_set_size(self->draft_remaps, 0);
    if (self->entry_remap_key)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_remap_key), "");
    if (self->entry_remap_action)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_remap_action), "");
    self->syncing_ui = FALSE;

    rebuild_remap_rows(self);
    update_code_preview_markup(self);
    update_ide_telemetry_and_dirty(self);
}

static void
on_card_reset_typo_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsPageIde *self = (SettingsPageIde *)user_data;
    if (!self)
        return;

    self->syncing_ui = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_font), 0);
    gtk_range_set_value(GTK_RANGE(self->scale_font_size), 13.0);
    segmented_group_set_selected(self->seg_tab_width, 0);
    gtk_switch_set_active(GTK_SWITCH(self->sw_spaces), TRUE);
    self->syncing_ui = FALSE;

    update_code_preview_markup(self);
    update_ide_telemetry_and_dirty(self);
}

/* Re-applies the saved config snapshot into the controls, discarding unsaved edits. */
static void
action_revert_ide(gpointer user_data)
{
    SettingsPageIde *self = (SettingsPageIde *)user_data;
    if (!self)
        return;

    self->syncing_ui = TRUE;
    select_ide_scheme(self, self->saved_scheme_idx, FALSE);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_font), self->saved_font_idx);
    gtk_range_set_value(GTK_RANGE(self->scale_font_size), self->saved_font_px);
    segmented_group_set_selected(self->seg_tab_width, self->saved_tab_idx);
    gtk_switch_set_active(GTK_SWITCH(self->sw_spaces), self->saved_spaces);
    gtk_switch_set_active(GTK_SWITCH(self->sw_word_wrap), self->saved_wrap);
    gtk_switch_set_active(GTK_SWITCH(self->sw_margin_guide), self->saved_margin);
    gtk_switch_set_active(GTK_SWITCH(self->sw_minimap), self->saved_minimap);
    gtk_switch_set_active(GTK_SWITCH(self->sw_gutter), self->saved_gutter);
    gtk_switch_set_active(GTK_SWITCH(self->sw_rel_lines), self->saved_rel_lines);
    gtk_switch_set_active(GTK_SWITCH(self->sw_cur_line), self->saved_cur_line);
    g_clear_pointer(&self->draft_remaps, g_ptr_array_unref);
    self->draft_remaps = clone_remap_array(self->saved_remaps);
    if (self->entry_remap_key)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_remap_key), "");
    if (self->entry_remap_action)
        gtk_editable_set_text(GTK_EDITABLE(self->entry_remap_action), "");
    self->syncing_ui = FALSE;

    rebuild_remap_rows(self);
    update_code_preview_markup(self);
    update_ide_telemetry_and_dirty(self);
}

typedef struct {
    const gchar     *scheme_id;
    const gchar     *font_fam;
    gint             font_px;
    gint             tab_w;
    gboolean         minimap;
    gboolean         gutter;
    gboolean         rel_lines;
    gboolean         cur_line;
    gboolean         wrap;
    gboolean         margin;
    gboolean         spaces;
    const GPtrArray *remaps;
} SaveIdeCtx;

static void
mutate_save_ide(KajoIdeCfg *config, gpointer user_data)
{
    SaveIdeCtx *ctx = (SaveIdeCtx *)user_data;
    g_free(config->color_scheme);
    config->color_scheme = g_strdup(ctx->scheme_id);
    g_free(config->font_family);
    config->font_family = g_strdup(ctx->font_fam);
    config->font_size_px = ctx->font_px;
    config->tab_width = ctx->tab_w;
    config->show_minimap = ctx->minimap;
    config->show_line_numbers = ctx->gutter;
    config->relative_line_numbers = ctx->rel_lines;
    config->highlight_current_line = ctx->cur_line;
    config->word_wrap = ctx->wrap;
    config->show_margin_guide = ctx->margin;
    config->insert_spaces = ctx->spaces;
    if (config->key_remaps)
        g_ptr_array_free(config->key_remaps, TRUE);
    config->key_remaps = clone_remap_array(ctx->remaps);
}

/* Writes the current control values to ide.toml and reloads every running
 * Kajo process. Only called from the SAVE & APPLY button. */
static void
action_save_ide(gpointer user_data)
{
    SettingsPageIde *self = (SettingsPageIde *)user_data;
    if (!self || self->syncing_ui)
        return;

    guint s_idx = self->current_scheme_idx < N_IDE_SCHEMES ? self->current_scheme_idx : 0;
    guint f_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->combo_font));
    const char *font_fam = (f_idx < 6 && IDE_FONT_FAMILIES[f_idx]) ? IDE_FONT_FAMILIES[f_idx] : "Fira Code";
    int font_px = CLAMP((int)gtk_range_get_value(GTK_RANGE(self->scale_font_size)), 9, 28);
    guint t_idx = segmented_group_get_selected(self->seg_tab_width);
    int tab_w = (t_idx == 1) ? 4 : ((t_idx == 2) ? 8 : 2);

    gboolean spaces    = gtk_switch_get_active(GTK_SWITCH(self->sw_spaces));
    gboolean wrap      = gtk_switch_get_active(GTK_SWITCH(self->sw_word_wrap));
    gboolean margin    = gtk_switch_get_active(GTK_SWITCH(self->sw_margin_guide));
    gboolean minimap   = gtk_switch_get_active(GTK_SWITCH(self->sw_minimap));
    gboolean gutter    = gtk_switch_get_active(GTK_SWITCH(self->sw_gutter));
    gboolean rel_lines = gtk_switch_get_active(GTK_SWITCH(self->sw_rel_lines));
    gboolean cur_line  = gtk_switch_get_active(GTK_SWITCH(self->sw_cur_line));

    SaveIdeCtx ctx = {
        IDE_SCHEMES[s_idx].id,
        font_fam,
        font_px,
        tab_w,
        minimap,
        gutter,
        rel_lines,
        cur_line,
        wrap,
        margin,
        spaces,
        self->draft_remaps,
    };
    kajo_config_store_update_ide(NULL, mutate_save_ide, &ctx, NULL);

    self->saved_scheme_idx = s_idx;
    self->saved_font_idx   = f_idx;
    self->saved_font_px    = font_px;
    self->saved_tab_idx    = t_idx;
    self->saved_spaces     = spaces;
    self->saved_wrap       = wrap;
    self->saved_margin     = margin;
    self->saved_minimap    = minimap;
    self->saved_gutter     = gutter;
    self->saved_rel_lines  = rel_lines;
    self->saved_cur_line   = cur_line;
    g_clear_pointer(&self->saved_remaps, g_ptr_array_unref);
    self->saved_remaps = clone_remap_array(self->draft_remaps);

    settings_reload_global_theme();
    settings_send_ipc_command("reload-config");
    update_code_preview_markup(self);
    update_ide_telemetry_and_dirty(self);
}

static void
on_ide_page_mapped(GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    update_ide_telemetry_and_dirty((SettingsPageIde *)widget);
}

static void
settings_page_ide_dispose(GObject *object)
{
    SettingsPageIde *self = (SettingsPageIde *)object;
    GdkDisplay *display = gdk_display_get_default();

    for (size_t i = 0; i < N_IDE_SCHEMES; i++) {
        if (self->scheme_providers[i]) {
            if (display) {
                gtk_style_context_remove_provider_for_display(
                    display, GTK_STYLE_PROVIDER(self->scheme_providers[i]));
            }
            g_clear_object(&self->scheme_providers[i]);
        }
    }
    if (self->preview_bg_provider) {
        if (display) {
            gtk_style_context_remove_provider_for_display(
                display, GTK_STYLE_PROVIDER(self->preview_bg_provider));
        }
        g_clear_object(&self->preview_bg_provider);
    }

    g_clear_pointer(&self->draft_remaps, g_ptr_array_unref);
    g_clear_pointer(&self->saved_remaps, g_ptr_array_unref);

    G_OBJECT_CLASS(settings_page_ide_parent_class)->dispose(object);
}

static void
settings_page_ide_init(SettingsPageIde *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));
}

static void
settings_page_ide_class_init(SettingsPageIdeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = settings_page_ide_dispose;
}

GtkWidget *
build_page_ide(void)
{
    SettingsPageIde *self = g_object_new(settings_page_ide_get_type(), NULL);
    self->syncing_ui = TRUE;

    g_autoptr(KajoIdeCfgRef) config = kajo_config_store_acquire_ide(NULL);

    GdkDisplay *display = gdk_display_get_default();

    /* ─── Card 1: Editor Color Scheme (6 Presets) & Live Syntax Preview ─── */
    GtkWidget *card_schemes = create_section_card(
        SETTINGS_CARD_LEFT,
        "1. EDITOR COLOR SCHEME",
        "6 PRESETS",
        "accent",
        NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_widget_set_hexpand(grid, TRUE);

    guint init_scheme_idx = 0;
    const char *cfg_scheme = (config && config->color_scheme)
        ? config->color_scheme
        : "kajo-metro-dark";

    for (size_t i = 0; i < N_IDE_SCHEMES; i++) {
        if (g_ascii_strcasecmp(cfg_scheme, IDE_SCHEMES[i].id) == 0)
            init_scheme_idx = (guint)i;

        GtkWidget *card = gtk_button_new();
        gtk_widget_add_css_class(card, "widget-row-card");
        gtk_widget_set_hexpand(card, TRUE);
        gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(card, IDE_SCHEMES[i].subtitle);
        g_object_set_data(G_OBJECT(card), "scheme-index", GUINT_TO_POINTER((guint)i));
        self->scheme_buttons[i] = card;

        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

        /* 3-band mini syntax swatch */
        GtkWidget *swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(swatch, 24, 16);
        gtk_widget_set_hexpand(swatch, FALSE);
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
        if (display) {
            gtk_style_context_add_provider_for_display(
                display, GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        self->scheme_providers[i] = p;

        GtkWidget *name_lbl = gtk_label_new(IDE_SCHEMES[i].name);
        gtk_widget_add_css_class(name_lbl, "row-title");
        gtk_widget_set_hexpand(name_lbl, TRUE);
        gtk_widget_set_halign(name_lbl, GTK_ALIGN_FILL);
        gtk_widget_set_valign(name_lbl, GTK_ALIGN_CENTER);
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(name_lbl), 4);

        gtk_box_append(GTK_BOX(hbox), swatch);
        gtk_box_append(GTK_BOX(hbox), name_lbl);
        gtk_button_set_child(GTK_BUTTON(card), hbox);

        g_signal_connect(card, "clicked", G_CALLBACK(on_scheme_card_clicked), self);
        gtk_grid_attach(GTK_GRID(grid), card, (int)(i % 2), (int)(i / 2), 1, 1);
    }
    gtk_box_append(GTK_BOX(card_schemes), grid);

    /* Canonical .code-preview-box with 5 ellipsized Pango lines */
    self->preview_bg_provider = gtk_css_provider_new();
    if (display) {
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(self->preview_bg_provider),
            GTK_STYLE_PROVIDER_PRIORITY_USER);
    }

    self->preview_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(self->preview_box, "code-preview-box");
    gtk_widget_add_css_class(self->preview_box, "ide-live-preview-box");
    gtk_widget_set_hexpand(self->preview_box, TRUE);

    for (size_t i = 0; i < N_PREVIEW_LINES; i++) {
        GtkWidget *ln = gtk_label_new(NULL);
        gtk_widget_set_hexpand(ln, TRUE);
        gtk_widget_set_halign(ln, GTK_ALIGN_FILL);
        gtk_label_set_xalign(GTK_LABEL(ln), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(ln), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(ln), 4);
        self->preview_lines[i] = ln;
        gtk_box_append(GTK_BOX(self->preview_box), ln);
    }
    gtk_box_append(GTK_BOX(card_schemes), self->preview_box);

    /* ─── Card 2: Editor Typography & Indentation ─── */
    GtkWidget *btn_card_reset = gtk_button_new_with_label("↺ DEFAULTS");
    gtk_widget_add_css_class(btn_card_reset, "files-nav-btn");
    gtk_widget_set_valign(btn_card_reset, GTK_ALIGN_CENTER);
    g_signal_connect(btn_card_reset, "clicked", G_CALLBACK(on_card_reset_typo_clicked), self);

    GtkWidget *card_typo = create_section_card(
        SETTINGS_CARD_CENTER,
        "2. TYPOGRAPHY & INDENTATION",
        NULL,
        NULL,
        btn_card_reset);

    self->combo_font = create_string_dropdown(IDE_FONT_FAMILIES);
    guint init_font_idx = 0;
    if (config && config->font_family) {
        for (guint i = 0; IDE_FONT_FAMILIES[i] != NULL; i++) {
            if (g_ascii_strcasecmp(config->font_family, IDE_FONT_FAMILIES[i]) == 0) {
                init_font_idx = i;
                break;
            }
        }
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->combo_font), init_font_idx);
    gtk_box_append(GTK_BOX(card_typo), create_boxed_row(
        "🔤",
        "Editor Monospace Font",
        "Falls back via shell_ide_font_stack() in kajo-ide",
        self->combo_font));

    int init_px = (config && config->font_size_px >= 9 && config->font_size_px <= 28)
        ? config->font_size_px
        : 13;
    GtkWidget *size_wrap = create_scale_with_badge(
        10.0, 24.0, 1.0, (double)init_px, "%.0fPX", &self->scale_font_size, NULL);
    gtk_box_append(GTK_BOX(card_typo), create_boxed_row(
        "🔍",
        "Editor Font Size",
        "Live zoom also available via Ctrl++ / Ctrl+- / Ctrl+0",
        size_wrap));

    guint init_tab_idx = 0;
    if (config) {
        if (config->tab_width == 4) init_tab_idx = 1;
        else if (config->tab_width == 8) init_tab_idx = 2;
    }
    self->seg_tab_width = create_segmented_group(IDE_TAB_LABELS, init_tab_idx);
    gtk_box_append(GTK_BOX(card_typo), create_boxed_row(
        "⇥",
        "Tab Stop Width",
        "Indentation width in columns (GtkSourceView)",
        self->seg_tab_width));

    self->sw_spaces = gtk_switch_new();
    gtk_widget_set_valign(self->sw_spaces, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(self->sw_spaces), config ? config->insert_spaces : TRUE);
    gtk_box_append(GTK_BOX(card_typo), create_boxed_row(
        "␣",
        "Insert Spaces Instead of Tabs",
        "Tab key inserts spaces; off inserts literal \\t",
        self->sw_spaces));

    settings_bento_add_pair(GTK_WIDGET(self), card_schemes, card_typo);

    /* ─── Card 3: Workspace Chrome & 6-Column Cairo Gutter ─── */
    GtkWidget *card_chrome = create_section_card(
        SETTINGS_CARD_RIGHT,
        "3. WORKSPACE CHROME & 6-COLUMN CAIRO GUTTER",
        "CAIRO + MAP",
        "success",
        NULL);

    self->sw_gutter = gtk_switch_new();
    gtk_widget_set_valign(self->sw_gutter, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(self->sw_gutter), config ? config->show_line_numbers : TRUE);
    gtk_box_append(GTK_BOX(card_chrome), create_boxed_row(
        "☰",
        "Show 6-Column Cairo Gutter",
        "Breakpoints • LSP Diagnostics • DAP PC • Line • Git Diff • Fold",
        self->sw_gutter));

    self->sw_rel_lines = gtk_switch_new();
    gtk_widget_set_valign(self->sw_rel_lines, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(self->sw_rel_lines), config ? config->relative_line_numbers : FALSE);
    gtk_box_append(GTK_BOX(card_chrome), create_boxed_row(
        "↕",
        "Relative Line Numbers",
        "Hybrid relative distance from cursor line (absolute on active line)",
        self->sw_rel_lines));

    self->sw_minimap = gtk_switch_new();
    gtk_widget_set_valign(self->sw_minimap, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(self->sw_minimap), config ? config->show_minimap : TRUE);
    gtk_box_append(GTK_BOX(card_chrome), create_boxed_row(
        "🗺",
        "Show Code Overview Minimap",
        "Right-docked GtkSourceMap overview strip (toggleable via MAP)",
        self->sw_minimap));

    self->sw_cur_line = gtk_switch_new();
    gtk_widget_set_valign(self->sw_cur_line, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(self->sw_cur_line), config ? config->highlight_current_line : TRUE);
    gtk_box_append(GTK_BOX(card_chrome), create_boxed_row(
        "▬",
        "Highlight Active Cursor Line",
        "Subtle surface tint on the current editor cursor line",
        self->sw_cur_line));

    self->sw_word_wrap = gtk_switch_new();
    gtk_widget_set_valign(self->sw_word_wrap, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(self->sw_word_wrap), config ? config->word_wrap : FALSE);
    gtk_box_append(GTK_BOX(card_chrome), create_boxed_row(
        "↩",
        "Word Wrap Long Lines",
        "Soft-wrap lines exceeding the editor viewport edge",
        self->sw_word_wrap));

    self->sw_margin_guide = gtk_switch_new();
    gtk_widget_set_valign(self->sw_margin_guide, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(self->sw_margin_guide), config ? config->show_margin_guide : FALSE);
    gtk_box_append(GTK_BOX(card_chrome), create_boxed_row(
        "│",
        "Margin Guide at Column 100",
        "Thin vertical guide marking the 100-column boundary",
        self->sw_margin_guide));

    settings_bento_add_full(GTK_WIDGET(self), card_chrome);

    /* ─── Card 4: Key Remapping (ide.toml) ─── */
    self->card_remaps = create_section_card(
        SETTINGS_CARD_LEFT,
        "4. KEY REMAPPING (ide.toml)",
        "0 REMAPS",
        NULL,
        NULL);

    self->remap_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(self->remap_list_box, TRUE);
    gtk_box_append(GTK_BOX(self->card_remaps), self->remap_list_box);

    GtkWidget *add_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(add_bar, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(add_bar, TRUE);

    self->combo_remap_state = create_string_dropdown(IDE_REMAP_STATES);
    gtk_widget_set_valign(self->combo_remap_state, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(self->combo_remap_state, FALSE);

    self->entry_remap_key = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_remap_key), 10);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_remap_key), "Key (e.g. j, Ctrl+k, Space)");
    gtk_widget_set_hexpand(self->entry_remap_key, TRUE);
    gtk_widget_set_valign(self->entry_remap_key, GTK_ALIGN_CENTER);

    self->entry_remap_action = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_remap_action), 14);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_remap_action), "Action (e.g. move_down, save)");
    gtk_widget_set_hexpand(self->entry_remap_action, TRUE);
    gtk_widget_set_valign(self->entry_remap_action, GTK_ALIGN_CENTER);

    self->btn_remap_add = gtk_button_new_with_label("+ ADD");
    gtk_widget_add_css_class(self->btn_remap_add, "files-nav-btn");
    gtk_widget_set_hexpand(self->btn_remap_add, FALSE);
    gtk_widget_set_valign(self->btn_remap_add, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(add_bar), self->combo_remap_state);
    gtk_box_append(GTK_BOX(add_bar), self->entry_remap_key);
    gtk_box_append(GTK_BOX(add_bar), self->entry_remap_action);
    gtk_box_append(GTK_BOX(add_bar), self->btn_remap_add);
    gtk_box_append(GTK_BOX(self->card_remaps), add_bar);

    settings_bento_add_full(GTK_WIDGET(self), self->card_remaps);

    self->saved_remaps = clone_remap_array(config ? config->key_remaps : NULL);
    self->draft_remaps = clone_remap_array(self->saved_remaps);
    rebuild_remap_rows(self);

    self->saved_scheme_idx = init_scheme_idx;
    self->saved_font_idx   = init_font_idx;
    self->saved_font_px    = init_px;
    self->saved_tab_idx    = init_tab_idx;
    self->saved_spaces     = gtk_switch_get_active(GTK_SWITCH(self->sw_spaces));
    self->saved_wrap       = gtk_switch_get_active(GTK_SWITCH(self->sw_word_wrap));
    self->saved_margin     = gtk_switch_get_active(GTK_SWITCH(self->sw_margin_guide));
    self->saved_minimap    = gtk_switch_get_active(GTK_SWITCH(self->sw_minimap));
    self->saved_gutter     = gtk_switch_get_active(GTK_SWITCH(self->sw_gutter));
    self->saved_rel_lines  = gtk_switch_get_active(GTK_SWITCH(self->sw_rel_lines));
    self->saved_cur_line   = gtk_switch_get_active(GTK_SWITCH(self->sw_cur_line));

    select_ide_scheme(self, init_scheme_idx, FALSE);
    self->syncing_ui = FALSE;

    g_signal_connect(self->combo_font, "notify::selected", G_CALLBACK(on_ide_dropdown_changed), self);
    g_signal_connect(self->scale_font_size, "value-changed", G_CALLBACK(on_ide_font_size_changed), self);
    segmented_group_connect(self->seg_tab_width, on_ide_segment_changed, self);
    g_signal_connect(self->sw_spaces, "notify::active", G_CALLBACK(on_ide_switch_changed), self);
    g_signal_connect(self->sw_word_wrap, "notify::active", G_CALLBACK(on_ide_switch_changed), self);
    g_signal_connect(self->sw_margin_guide, "notify::active", G_CALLBACK(on_ide_switch_changed), self);
    g_signal_connect(self->sw_minimap, "notify::active", G_CALLBACK(on_ide_switch_changed), self);
    g_signal_connect(self->sw_gutter, "notify::active", G_CALLBACK(on_ide_switch_changed), self);
    g_signal_connect(self->sw_rel_lines, "notify::active", G_CALLBACK(on_ide_switch_changed), self);
    g_signal_connect(self->sw_cur_line, "notify::active", G_CALLBACK(on_ide_switch_changed), self);
    g_signal_connect(self->btn_remap_add, "clicked", G_CALLBACK(on_remap_add_clicked), self);
    g_signal_connect(self->entry_remap_action, "activate", G_CALLBACK(on_remap_add_clicked), self);
    g_signal_connect(self, "map", G_CALLBACK(on_ide_page_mapped), NULL);

    settings_page_bind_actions(
        GTK_WIDGET(self),
        "↺ RESET IDE DEFAULTS",
        action_reset_ide_defaults,
        action_revert_ide,
        action_save_ide,
        self);

    update_ide_telemetry_and_dirty(self);
    return GTK_WIDGET(self);
}
