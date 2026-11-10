#include "settings_window.h"
#include "settings_common.h"
#include "settings_pages.h"
#include "../compositor/compositor.h"
#include <sys/utsname.h>
#include <string.h>

typedef struct {
    const char *key;
    const char *num_short;
    const char *hdr_badge;
    const char *title;
    const char *subtitle;
    const char *badge1;
    const char *badge2;
    const char *c_keys[4];
    const char *c_vals[4];
} SettingsPageMeta;

static const SettingsPageMeta k_page_meta[] = {
    {
        "displays", "01 / 12", "01 / 12 • DISPLAYS",
        "DISPLAYS, IDLE SCREENSAVER & 360° FOLDABLE POSTURE",
        "Niri IPC output management (10-outputs.kdl), ext-idle-notify-v1 & IIO hinge sensors",
        "NIRI IPC", "10-OUTPUTS.KDL",
        { "BACKEND", "CONFIG", "IDLE", "POSTURE" },
        { "Niri IPC", "10-outputs.kdl", "ext-idle-v1", "IIO Sensor" }
    },
    {
        "network", "02 / 12", "02 / 12 • NETWORK",
        "NETWORKMANAGER WI-FI & ACTIVE INTERFACES",
        "Live D-Bus org.freedesktop.NetworkManager access point scanning & inline WPA auth",
        "LIVE D-BUS", "NETWORKMANAGER",
        { "BACKEND", "D-BUS", "WIFI", "STATE" },
        { "NetworkManager", "System Bus", "802.11", "Live Query" }
    },
    {
        "bluetooth", "03 / 12", "03 / 12 • BLUETOOTH",
        "BLUEZ 5 BLUETOOTH ADAPTER & PERIPHERALS",
        "Live GDBusObjectManager org.bluez adapter power, pairing & audio profiles",
        "LIVE D-BUS", "ORG.BLUEZ",
        { "BACKEND", "ADAPTER", "PAIRED", "DISCOVERY" },
        { "BlueZ 5", "org.bluez", "Live Query", "Idle" }
    },
    {
        "audio", "04 / 12", "04 / 12 • AUDIO",
        "PIPEWIRE / PULSEAUDIO ROUTING & CHANNEL MAPS",
        "Multi-channel sink preservation (sink_channels), default routing & auto-reconnect",
        "PIPEWIRE", "LIBPULSE",
        { "SERVER", "ROUTING", "CHANNELS", "MONITOR" },
        { "PipeWire-Pulse", "Default Sink", "Preserved", "Auto-Reconnect" }
    },
    {
        "theme", "05 / 12", "05 / 12 • THEME",
        "AERO + METRO ACCENT PALETTE & TYPOGRAPHY",
        "Live CSS token generation (~/.config/kajo/style.d/00-accent.css) & 4 font categories",
        "00-ACCENT.CSS", "FIRA CODE",
        { "ACCENT", "BODY", "HEADING", "BADGES" },
        { "#0078D4 COBALT", "14PX", "13PX", "11PX" }
    },
    {
        "panel", "06 / 12", "06 / 12 • PANEL & OSK",
        "WAYLAND PANEL, TOUCH GESTURES & VIRTUAL KEYBOARD",
        "Layer-shell anchor geometry, bezel swipe gestures & zwp_input_method_v2 ShellOSK",
        "LAYER-SHELL", "SHELLOSK",
        { "EDGE", "AUTOHIDE", "SWIPES", "OSK" },
        { "TOP (32PX)", "DISABLED", "ENABLED", "260PX" }
    },
    {
        "widgets", "07 / 12", "07 / 12 • WIDGETS",
        "PANEL BAR WIDGET SECTIONS (LEFT / CENTER / RIGHT)",
        "Configure and reorder the 22 built-in ShellWidget modules across panel zones",
        "22 MODULES", "CONFIG.JSON",
        { "LEFT", "CENTER", "RIGHT", "TOTAL" },
        { "0 WIDGETS", "0 WIDGETS", "0 WIDGETS", "0 ACTIVE" }
    },
    {
        "notifications", "08 / 12", "08 / 12 • NOTIFICATIONS",
        "NOTIFICATION TOASTS & FREEDESKTOP DAEMON",
        "Toast timeout, Niri source-window focus activation & Notification Center ring buffer",
        "DBUS DAEMON", "CONFIG.JSON",
        { "DND", "TIMEOUT", "HISTORY", "CLICK" },
        { "OFF", "5000MS (5S)", "50 ITEMS", "FOCUS WINDOW" }
    },
    {
        "niri", "09 / 12", "09 / 12 • NIRI & SHADERS",
        "NIRI COMPOSITOR LAYER RULES & GLSL SHADERS",
        "Per-namespace opacity/blur rules (40-layer-rules.kdl) & GLSL window animations (30-layout-animations.kdl)",
        "GLSL 330", "NIRI KDL",
        { "NAMESPACE", "GAPS", "OPEN", "CLOSE" },
        { "kajo (85%)", "12PX", "DEFAULT", "DEFAULT" }
    },
    {
        "ide", "10 / 12", "10 / 12 • KAJO IDE",
        "KAJO IDE — MODAL CODE EDITOR & CAIRO GUTTER",
        "GtkSourceView-5 color schemes, Tree-sitter queries, 6-column gutter & live preview",
        "ORG.KAJO.IDE", "TREE-SITTER",
        { "SCHEME", "FONT", "TAB WIDTH", "GUTTER" },
        { "METRO DARK", "FIRA CODE 13PX", "4 SPACES", "6-COL CAIRO" }
    },
    {
        "mod", "11 / 12", "11 / 12 • KAJO MOD",
        "KAJO MOD — RETRO AMIGA, C64 SID & TRACKER ENGINE",
        "Persistent global defaults in ~/.config/kajo/mod.toml • Live monitor in org.kajo.Mod",
        "LIBUADE + RESIDFP", "MOD.TOML",
        { "PAULA", "SID CHIP", "STEREO", "TIMEOUT" },
        { "A500 + AUTO", "AUTO (PSID)", "70% SEP", "180S" }
    },
    {
        "about", "12 / 12", "12 / 12 • ABOUT",
        "ABOUT KAJO // OS & DESKTOP SUITE INVENTORY",
        "System kernel, CPU, memory & native C17 / GTK4 / Wayland application suite",
        "KAJO SUITE 0.1", "GTK4 WAYLAND",
        { "KERNEL", "TOOLKIT", "COMPOSITOR", "SUITE" },
        { "Linux", "GTK4 + Layer", "Wayland / Niri", "7 Binaries" }
    }
};

static const SettingsPageMeta *
lookup_page_meta(const gchar *page_name)
{
    if (!page_name)
        return &k_page_meta[0];
    for (gsize i = 0; i < G_N_ELEMENTS(k_page_meta); i++) {
        if (g_strcmp0(k_page_meta[i].key, page_name) == 0)
            return &k_page_meta[i];
    }
    return &k_page_meta[0];
}

static void select_sidebar_row_by_name(SettingsApp *app_ctx, const gchar *target_page);
static void update_responsive_layout(SettingsApp *app_ctx, int width);
static void apply_sidebar_visibility_and_filter(SettingsApp *app_ctx);

static void
settings_app_free(gpointer data)
{
    SettingsApp *app_ctx = data;
    if (!app_ctx)
        return;
    g_free(app_ctx->active_page);
    g_free(app_ctx);
}

static void
populate_live_config_telemetry_defaults(const gchar *page_name,
                                        gchar       *out_vals[4])
{
    for (int i = 0; i < 4; i++)
        out_vals[i] = NULL;

    if (g_strcmp0(page_name, "about") == 0) {
        struct utsname uts;
        if (uname(&uts) == 0)
            out_vals[0] = g_strdup_printf("Linux %s", uts.release);
        out_vals[1] = g_strdup_printf("GTK %d.%d", GTK_MAJOR_VERSION, GTK_MINOR_VERSION);
        const gchar *niri_sock = g_getenv("NIRI_SOCKET");
        out_vals[2] = g_strdup((niri_sock && niri_sock[0]) ? "Niri IPC Active" : "Wayland");
        out_vals[3] = g_strdup("7 Binaries");
        return;
    }

    if (g_strcmp0(page_name, "theme") == 0) {
        g_autoptr(KajoThemeCfgRef) tcfg = kajo_config_store_acquire_theme(NULL);
        const gchar *hex = (tcfg && tcfg->accent_color) ? tcfg->accent_color : "#0078d4";
        out_vals[0] = g_strdup_printf("%s %s", hex, settings_accent_short_name(hex));
        out_vals[1] = g_strdup_printf("%dPX", tcfg ? tcfg->ui_font_size_px : 14);
        out_vals[2] = g_strdup_printf("%dPX", tcfg ? tcfg->font_heading_size_px : 13);
        out_vals[3] = g_strdup_printf("%dPX", tcfg ? tcfg->font_badge_size_px : 11);
    } else if (g_strcmp0(page_name, "panel") == 0) {
        g_autoptr(KajoShellCfgRef) scfg = kajo_config_store_acquire_shell(NULL);
        const gchar *edge = (scfg && scfg->panel_position == PANEL_POSITION_BOTTOM) ? "BOTTOM" : "TOP";
        out_vals[0] = g_strdup_printf("%s (%dPX)", edge, scfg ? scfg->panel_height : 39);
        out_vals[1] = g_strdup((scfg && scfg->autohide_enabled) ? "ENABLED" : "DISABLED");
        out_vals[2] = g_strdup((scfg && scfg->touch_gestures_enabled) ? "ENABLED" : "DISABLED");
        out_vals[3] = g_strdup_printf("%dPX", scfg ? scfg->osk_height_px : 260);
    } else if (g_strcmp0(page_name, "widgets") == 0) {
        g_autoptr(KajoShellCfgRef) scfg = kajo_config_store_acquire_shell(NULL);
        guint nl = (scfg && scfg->layout_left) ? scfg->layout_left->len : 0;
        guint nc = (scfg && scfg->layout_center) ? scfg->layout_center->len : 0;
        guint nr = (scfg && scfg->layout_right) ? scfg->layout_right->len : 0;
        out_vals[0] = g_strdup_printf("%u WIDGETS", nl);
        out_vals[1] = g_strdup_printf("%u WIDGETS", nc);
        out_vals[2] = g_strdup_printf("%u WIDGETS", nr);
        out_vals[3] = g_strdup_printf("%u ACTIVE", nl + nc + nr);
    } else if (g_strcmp0(page_name, "notifications") == 0) {
        g_autoptr(KajoShellCfgRef) scfg = kajo_config_store_acquire_shell(NULL);
        gint tout = scfg ? scfg->toast_timeout_ms : 8000;
        out_vals[0] = g_strdup((scfg && scfg->dnd_enabled) ? "ACTIVE" : "OFF");
        out_vals[1] = g_strdup_printf("%dMS (%dS)", tout, MAX(1, tout / 1000));
        out_vals[2] = g_strdup_printf("%d ITEMS", scfg ? scfg->max_notification_history : 50);
        out_vals[3] = g_strdup((scfg && scfg->toast_click_action == TOAST_CLICK_DISMISS) ? "DISMISS ONLY" : "FOCUS WINDOW");
    } else if (g_strcmp0(page_name, "ide") == 0) {
        g_autoptr(KajoIdeCfgRef) icfg = kajo_config_store_acquire_ide(NULL);
        out_vals[0] = g_ascii_strup((icfg && icfg->color_scheme) ? icfg->color_scheme : "kajo-dark", -1);
        out_vals[1] = g_strdup_printf("%s %dPX",
                                      (icfg && icfg->font_family) ? icfg->font_family : "Fira Code",
                                      icfg ? icfg->font_size_px : 13);
        out_vals[2] = g_strdup_printf("%d %s",
                                      icfg ? icfg->tab_width : 4,
                                      (icfg && icfg->insert_spaces) ? "SPACES" : "TABS");
        out_vals[3] = g_strdup((icfg && icfg->show_line_numbers) ? "6-COL CAIRO" : "COMPACT");
    } else if (g_strcmp0(page_name, "mod") == 0) {
        g_autoptr(KajoModCfgRef) mcfg = kajo_config_store_acquire_mod(NULL);
        g_autofree gchar *flt = g_ascii_strup(kajo_mod_filter_mode_to_string(mcfg ? mcfg->filter_mode : KAJO_MOD_FILTER_A500), -1);
        g_autofree gchar *led = g_ascii_strup(kajo_mod_led_mode_to_string(mcfg ? mcfg->led_mode : KAJO_MOD_LED_AUTO), -1);
        out_vals[0] = g_strdup_printf("%s + LED %s", flt, led);
        out_vals[1] = g_ascii_strup(kajo_mod_sid_model_to_string(mcfg ? mcfg->sid_model : KAJO_MOD_SID_AUTO), -1);
        out_vals[2] = g_strdup_printf("%d%% SEP", mcfg ? mcfg->stereo_sep_pct : 70);
        out_vals[3] = (mcfg && mcfg->uade_timeout_sec == 0)
            ? g_strdup("INFINITE")
            : g_strdup_printf("%dS MAX", mcfg ? mcfg->uade_timeout_sec : 180);
    }
}

static void
apply_counter_val_style(GtkWidget *lbl, const gchar *style_class, gboolean default_accent)
{
    gtk_widget_remove_css_class(lbl, "accent");
    gtk_widget_remove_css_class(lbl, "warn");
    gtk_widget_remove_css_class(lbl, "success");
    if (style_class && style_class[0] != '\0')
        gtk_widget_add_css_class(lbl, style_class);
    else if (default_accent)
        gtk_widget_add_css_class(lbl, "accent");
}

static void
set_shrinkable_button_label(GtkWidget *btn, const gchar *text, int min_chars)
{
    gtk_button_set_label(GTK_BUTTON(btn), text);
    GtkWidget *lbl = gtk_button_get_child(GTK_BUTTON(btn));
    if (lbl && GTK_IS_LABEL(lbl)) {
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(lbl), min_chars);
    }
}

void
settings_window_sync_active_page_chrome(SettingsApp *app_ctx)
{
    if (!app_ctx || !app_ctx->stack)
        return;

    const gchar *page_name = app_ctx->active_page ? app_ctx->active_page : "displays";
    const SettingsPageMeta *meta = lookup_page_meta(page_name);
    GtkWidget *vis_page = gtk_stack_get_visible_child(GTK_STACK(app_ctx->stack));
    SettingsPageState *st = vis_page ? g_object_get_data(G_OBJECT(vis_page), "settings-page-state") : NULL;

    /* Zone 1: Active Page Index Badge */
    if (app_ctx->hdr_page_badge) {
        gtk_label_set_text(GTK_LABEL(app_ctx->hdr_page_badge),
                           app_ctx->is_narrow_mode ? meta->num_short : meta->hdr_badge);
    }

    /* Zone 2: Pinned Page Header & 4-Cell Live Telemetry Strip */
    if (app_ctx->deck_title_lbl)
        gtk_label_set_text(GTK_LABEL(app_ctx->deck_title_lbl), meta->title);
    if (app_ctx->deck_sub_lbl)
        gtk_label_set_text(GTK_LABEL(app_ctx->deck_sub_lbl), meta->subtitle);
    if (app_ctx->deck_badge1_lbl)
        gtk_label_set_text(GTK_LABEL(app_ctx->deck_badge1_lbl),
                           (st && st->deck_badge1) ? st->deck_badge1 : meta->badge1);
    if (app_ctx->deck_badge2_lbl) {
        gtk_label_set_text(GTK_LABEL(app_ctx->deck_badge2_lbl),
                           (st && st->deck_badge2) ? st->deck_badge2 : meta->badge2);
        gtk_widget_set_visible(app_ctx->deck_badge2_lbl, app_ctx->current_width >= 600);
    }

    gchar *dyn_vals[4] = { NULL, NULL, NULL, NULL };
    populate_live_config_telemetry_defaults(page_name, dyn_vals);

    for (int i = 0; i < 4; i++) {
        const gchar *k = (st && st->cnt_keys[i]) ? st->cnt_keys[i] : meta->c_keys[i];
        const gchar *v = (st && st->cnt_vals[i]) ? st->cnt_vals[i]
                       : (dyn_vals[i] ? dyn_vals[i] : meta->c_vals[i]);
        const gchar *sc = (st && st->cnt_styles[i]) ? st->cnt_styles[i] : NULL;

        if (app_ctx->cnt_key_lbls[i])
            gtk_label_set_text(GTK_LABEL(app_ctx->cnt_key_lbls[i]), k);
        if (app_ctx->cnt_val_lbls[i]) {
            gtk_label_set_text(GTK_LABEL(app_ctx->cnt_val_lbls[i]), v);
            apply_counter_val_style(app_ctx->cnt_val_lbls[i], sc, i == 0);
        }
        g_free(dyn_vals[i]);
    }

    /* Zone 4: Docked Action & Persistence Footer Bar */
    if (!app_ctx->footer_bar)
        return;

    gtk_widget_remove_css_class(app_ctx->footer_state_badge, "accent");
    gtk_widget_remove_css_class(app_ctx->footer_state_badge, "success");

    if (!st || (!st->on_secondary && !st->has_save_revert)) {
        /* Page still carries inline controls prior to Stage 2-4 migration */
        gtk_widget_add_css_class(app_ctx->footer_state_badge, "success");
        gtk_label_set_text(GTK_LABEL(app_ctx->footer_state_badge),
                           app_ctx->is_narrow_mode ? "✓ READY" : "✓ LIVE / INLINE CONTROLS");
        gtk_label_set_text(GTK_LABEL(app_ctx->footer_status_lbl),
                           "~/.config/kajo/*.toml • Live IPC ready");
        gtk_widget_set_visible(app_ctx->footer_actions_box, FALSE);
        return;
    }

    gtk_widget_set_visible(app_ctx->footer_actions_box, TRUE);

    if (st->has_save_revert) {
        if (st->is_dirty) {
            gtk_widget_add_css_class(app_ctx->footer_state_badge, "accent");
            gtk_label_set_text(GTK_LABEL(app_ctx->footer_state_badge),
                               app_ctx->is_narrow_mode
                                   ? "● UNSAVED"
                                   : "● UNSAVED CHANGES — PREVIEW ONLY");
            gtk_label_set_text(GTK_LABEL(app_ctx->footer_status_lbl),
                               "Click SAVE & APPLY to write ~/.config/kajo/*.toml");
        } else {
            gtk_widget_add_css_class(app_ctx->footer_state_badge, "success");
            gtk_label_set_text(GTK_LABEL(app_ctx->footer_state_badge),
                               app_ctx->is_narrow_mode
                                   ? "✓ SAVED"
                                   : (st->live_badge_text ? st->live_badge_text : "✓ ALL CHANGES SAVED"));
            gtk_label_set_text(GTK_LABEL(app_ctx->footer_status_lbl),
                               st->live_status_text
                                   ? st->live_status_text
                                   : "~/.config/kajo/*.toml • Live IPC ready");
        }
        gtk_widget_set_visible(app_ctx->btn_revert, st->on_revert != NULL);
        gtk_widget_set_sensitive(app_ctx->btn_revert, st->is_dirty);
        gtk_widget_set_visible(app_ctx->btn_save, TRUE);
        set_shrinkable_button_label(
            app_ctx->btn_save,
            app_ctx->is_narrow_mode
                ? (st->is_dirty ? "[ ✓ SAVE ]" : "[ ✓ SAVED ]")
                : (st->is_dirty ? "[ ✓ SAVE & APPLY ]" : "[ ✓ SAVED ]"),
            6);
    } else {
        gtk_widget_add_css_class(app_ctx->footer_state_badge, "success");
        gtk_label_set_text(GTK_LABEL(app_ctx->footer_state_badge),
                           st->live_badge_text
                               ? st->live_badge_text
                               : (app_ctx->is_narrow_mode ? "✓ LIVE" : "✓ LIVE SYSTEM DAEMON"));
        gtk_label_set_text(GTK_LABEL(app_ctx->footer_status_lbl),
                           st->live_status_text
                               ? st->live_status_text
                               : "Changes apply immediately via system daemon");
        gtk_widget_set_visible(app_ctx->btn_revert, FALSE);
        gtk_widget_set_visible(app_ctx->btn_save, FALSE);
    }

    if (st->secondary_label && st->on_secondary) {
        set_shrinkable_button_label(app_ctx->btn_secondary, st->secondary_label, 4);
        gtk_widget_set_visible(app_ctx->btn_secondary, TRUE);
    } else {
        gtk_widget_set_visible(app_ctx->btn_secondary, FALSE);
    }
}

static void
ensure_settings_page(SettingsApp *app_ctx, const gchar *page_name)
{
    if (!app_ctx || !app_ctx->stack || !page_name)
        return;

    if (gtk_stack_get_child_by_name(GTK_STACK(app_ctx->stack), page_name) != NULL)
        return;

    GtkWidget *page = NULL;
    if (g_strcmp0(page_name, "displays") == 0)
        page = build_page_displays();
    else if (g_strcmp0(page_name, "network") == 0)
        page = build_page_network();
    else if (g_strcmp0(page_name, "bluetooth") == 0)
        page = build_page_bluetooth();
    else if (g_strcmp0(page_name, "audio") == 0)
        page = build_page_audio();
    else if (g_strcmp0(page_name, "theme") == 0)
        page = build_page_theme();
    else if (g_strcmp0(page_name, "panel") == 0)
        page = build_page_panel();
    else if (g_strcmp0(page_name, "widgets") == 0)
        page = build_page_widgets();
    else if (g_strcmp0(page_name, "notifications") == 0)
        page = build_page_notifications();
    else if (g_strcmp0(page_name, "niri") == 0)
        page = build_page_niri();
    else if (g_strcmp0(page_name, "ide") == 0)
        page = build_page_ide();
    else if (g_strcmp0(page_name, "mod") == 0)
        page = build_page_mod();
    else if (g_strcmp0(page_name, "about") == 0)
        page = build_page_about();

    if (page != NULL)
        gtk_stack_add_named(GTK_STACK(app_ctx->stack), page, page_name);
}

static void
rearrange_counter_grid(SettingsApp *app_ctx, gboolean use_2x2)
{
    if (!app_ctx || !app_ctx->counter_grid)
        return;

    gpointer cur_flag = g_object_get_data(G_OBJECT(app_ctx->counter_grid), "grid-is-2x2");
    gboolean cur_2x2 = (cur_flag != NULL) && (GPOINTER_TO_INT(cur_flag) == 2);
    gboolean initialized = (cur_flag != NULL);
    if (initialized && cur_2x2 == use_2x2)
        return;

    for (int i = 0; i < 4; i++) {
        g_object_ref(app_ctx->cnt_cells[i]);
        if (gtk_widget_get_parent(app_ctx->cnt_cells[i]) == app_ctx->counter_grid)
            gtk_grid_remove(GTK_GRID(app_ctx->counter_grid), app_ctx->cnt_cells[i]);
    }

    if (use_2x2) {
        gtk_grid_attach(GTK_GRID(app_ctx->counter_grid), app_ctx->cnt_cells[0], 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(app_ctx->counter_grid), app_ctx->cnt_cells[1], 1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(app_ctx->counter_grid), app_ctx->cnt_cells[2], 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(app_ctx->counter_grid), app_ctx->cnt_cells[3], 1, 1, 1, 1);
    } else {
        for (int i = 0; i < 4; i++)
            gtk_grid_attach(GTK_GRID(app_ctx->counter_grid), app_ctx->cnt_cells[i], i, 0, 1, 1);
    }

    for (int i = 0; i < 4; i++)
        g_object_unref(app_ctx->cnt_cells[i]);

    g_object_set_data(G_OBJECT(app_ctx->counter_grid),
                      "grid-is-2x2",
                      GINT_TO_POINTER(use_2x2 ? 2 : 1));
}

static void
sync_active_page_bento(SettingsApp *app_ctx)
{
    if (!app_ctx || !app_ctx->stack)
        return;

    GtkWidget *vis = gtk_stack_get_visible_child(GTK_STACK(app_ctx->stack));
    if (!vis)
        return;

    int sidebar_w = app_ctx->is_compact_rail ? 46 : 224;
    int avail_w = MAX(0, app_ctx->current_width - sidebar_w - 28);

    if (g_object_get_data(G_OBJECT(vis), "is-bento-canvas"))
        settings_bento_update_columns(vis, app_ctx->is_wide_mode, avail_w);

    settings_update_descendant_scales(vis, app_ctx->is_narrow_mode);
}

static void
apply_sidebar_visibility_and_filter(SettingsApp *app_ctx)
{
    if (!app_ctx || !app_ctx->sidebar_list)
        return;

    const gchar *text = app_ctx->search_entry
        ? gtk_editable_get_text(GTK_EDITABLE(app_ctx->search_entry))
        : "";
    g_autofree gchar *query = text ? g_ascii_strdown(text, -1) : g_strdup("");
    g_strstrip(query);
    gboolean empty_query = (query[0] == '\0');

    GtkWidget *current_header_row = NULL;
    gboolean header_has_visible_child = FALSE;

    for (GtkWidget *child = gtk_widget_get_first_child(app_ctx->sidebar_list);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        if (!GTK_IS_LIST_BOX_ROW(child))
            continue;

        GtkWidget *row_child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));
        if (gtk_widget_has_css_class(child, "sidebar-header-row")) {
            if (current_header_row) {
                gtk_widget_set_visible(
                    current_header_row,
                    !app_ctx->is_compact_rail && (empty_query || header_has_visible_child));
            }
            current_header_row = child;
            header_has_visible_child = FALSE;
            gtk_widget_set_visible(child, !app_ctx->is_compact_rail && empty_query);
        } else if (row_child) {
            GtkWidget *nav_icon  = g_object_get_data(G_OBJECT(row_child), "nav-icon");
            GtkWidget *nav_lbl   = g_object_get_data(G_OBJECT(row_child), "nav-label");
            GtkWidget *nav_badge = g_object_get_data(G_OBJECT(row_child), "nav-badge");
            if (nav_icon)
                gtk_widget_set_hexpand(nav_icon, app_ctx->is_compact_rail);
            if (nav_lbl)
                gtk_widget_set_visible(nav_lbl, !app_ctx->is_compact_rail);
            if (nav_badge) {
                const gchar *bt = gtk_label_get_text(GTK_LABEL(nav_badge));
                gtk_widget_set_visible(nav_badge, !app_ctx->is_compact_rail && bt && bt[0] != '\0');
            }

            if (empty_query) {
                gtk_widget_set_visible(child, TRUE);
                header_has_visible_child = TRUE;
            } else {
                const gchar *page_name  = g_object_get_data(G_OBJECT(row_child), "page-name");
                const gchar *label_text = g_object_get_data(G_OBJECT(row_child), "label-text");
                const gchar *sub_text   = g_object_get_data(G_OBJECT(row_child), "subtitle-text");
                const gchar *keywords   = g_object_get_data(G_OBJECT(row_child), "keywords");

                gboolean match = FALSE;
                if (page_name && strstr(page_name, query))
                    match = TRUE;
                if (!match && label_text) {
                    g_autofree gchar *l = g_ascii_strdown(label_text, -1);
                    if (strstr(l, query)) match = TRUE;
                }
                if (!match && sub_text) {
                    g_autofree gchar *s = g_ascii_strdown(sub_text, -1);
                    if (strstr(s, query)) match = TRUE;
                }
                if (!match && keywords) {
                    g_autofree gchar *k = g_ascii_strdown(keywords, -1);
                    if (strstr(k, query)) match = TRUE;
                }
                gtk_widget_set_visible(child, match);
                if (match)
                    header_has_visible_child = TRUE;
            }
        }
    }

    if (current_header_row) {
        gtk_widget_set_visible(
            current_header_row,
            !app_ctx->is_compact_rail && (empty_query || header_has_visible_child));
    }
}

static void
update_responsive_layout(SettingsApp *app_ctx, int width)
{
    if (!app_ctx || !app_ctx->window || width <= 0)
        return;

    int eff_w = (app_ctx->sim_col_width >= 480) ? app_ctx->sim_col_width : width;
    app_ctx->current_width   = eff_w;
    app_ctx->is_narrow_mode  = (eff_w < 720);
    app_ctx->is_wide_mode    = (eff_w >= 960);
    app_ctx->is_compact_rail = app_ctx->is_narrow_mode
        ? !app_ctx->rail_force_expanded
        : app_ctx->rail_force_compact;

    if (app_ctx->is_narrow_mode)
        gtk_widget_add_css_class(app_ctx->window, "narrow-col");
    else
        gtk_widget_remove_css_class(app_ctx->window, "narrow-col");

    if (app_ctx->is_wide_mode)
        gtk_widget_add_css_class(app_ctx->window, "wide-col");
    else
        gtk_widget_remove_css_class(app_ctx->window, "wide-col");

    if (app_ctx->is_compact_rail)
        gtk_widget_add_css_class(app_ctx->window, "compact-rail");
    else
        gtk_widget_remove_css_class(app_ctx->window, "compact-rail");

    if (app_ctx->main_paned) {
        gtk_paned_set_position(GTK_PANED(app_ctx->main_paned),
                               app_ctx->is_compact_rail ? 46 : 224);
    }

    /* Zone 1 responsive chrome */
    if (app_ctx->hdr_title_lbl) {
        gtk_label_set_text(GTK_LABEL(app_ctx->hdr_title_lbl),
                           (eff_w < 640) ? "⚙ SETTINGS" : "⚙ SYSTEM SETTINGS");
    }
    if (app_ctx->btn_float) {
        GtkWidget *fl_lbl = gtk_button_get_child(GTK_BUTTON(app_ctx->btn_float));
        if (fl_lbl && GTK_IS_LABEL(fl_lbl)) {
            gtk_label_set_text(GTK_LABEL(fl_lbl),
                               app_ctx->is_narrow_mode ? "🗗" : "🗗 FLOAT / TILE");
        }
    }
    if (app_ctx->search_entry) {
        g_object_set(app_ctx->search_entry,
                     "placeholder-text",
                     app_ctx->is_narrow_mode
                         ? "Filter..."
                         : "Filter settings (Ctrl+F)...",
                     NULL);
    }

    /* Zone 2 counter strip (2x2 < 720px, 4x1 >= 720px) */
    rearrange_counter_grid(app_ctx, app_ctx->is_narrow_mode);

    /* Zone 4 status text visibility */
    if (app_ctx->footer_status_lbl)
        gtk_widget_set_visible(app_ctx->footer_status_lbl, eff_w >= 760);

    apply_sidebar_visibility_and_filter(app_ctx);
    settings_window_sync_active_page_chrome(app_ctx);
    sync_active_page_bento(app_ctx);
}

static void
on_surface_width_changed(GdkSurface *surface, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(user_data);
    SettingsApp *app_ctx = g_object_get_data(G_OBJECT(window), "app-ctx");
    if (!app_ctx)
        return;
    int width = gdk_surface_get_width(surface);
    if (width > 0)
        update_responsive_layout(app_ctx, width);
}

static void
on_window_realize(GtkWidget *widget, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (surface) {
        g_signal_connect_object(surface, "notify::width", G_CALLBACK(on_surface_width_changed), widget, 0);
        int width = gdk_surface_get_width(surface);
        if (width > 0)
            update_responsive_layout(app_ctx, width);
    }
}

static void
on_window_size_changed(GtkWindow *win G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    int width = gtk_widget_get_width(GTK_WIDGET(app_ctx->window));
    if (width > 0)
        update_responsive_layout(app_ctx, width);
}

static void
on_rail_toggle_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    if (app_ctx->is_narrow_mode)
        app_ctx->rail_force_expanded = !app_ctx->rail_force_expanded;
    else
        app_ctx->rail_force_compact = !app_ctx->rail_force_compact;
    update_responsive_layout(app_ctx, app_ctx->current_width > 0 ? app_ctx->current_width : 814);
}

static void
on_sidebar_row_selected(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
    if (!row)
        return;
    SettingsApp *app_ctx = user_data;
    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!child)
        return;

    const gchar *page_name = g_object_get_data(G_OBJECT(child), "page-name");
    if (!page_name || !app_ctx->stack)
        return;

    ensure_settings_page(app_ctx, page_name);
    if (!gtk_stack_get_child_by_name(GTK_STACK(app_ctx->stack), page_name))
        return;

    g_free(app_ctx->active_page);
    app_ctx->active_page = g_strdup(page_name);

    gtk_stack_set_visible_child_name(GTK_STACK(app_ctx->stack), page_name);
    if (app_ctx->stack_scroll) {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(app_ctx->stack_scroll));
        if (vadj)
            gtk_adjustment_set_value(vadj, 0.0);
    }

    settings_window_sync_active_page_chrome(app_ctx);
    sync_active_page_bento(app_ctx);
}

static void
on_search_changed(GtkSearchEntry *entry G_GNUC_UNUSED, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    apply_sidebar_visibility_and_filter(app_ctx);
}

static void
on_footer_secondary_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    GtkWidget *vis = app_ctx->stack ? gtk_stack_get_visible_child(GTK_STACK(app_ctx->stack)) : NULL;
    SettingsPageState *st = vis ? g_object_get_data(G_OBJECT(vis), "settings-page-state") : NULL;
    if (st && st->on_secondary)
        st->on_secondary(st->user_data);
}

static void
on_footer_revert_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    GtkWidget *vis = app_ctx->stack ? gtk_stack_get_visible_child(GTK_STACK(app_ctx->stack)) : NULL;
    SettingsPageState *st = vis ? g_object_get_data(G_OBJECT(vis), "settings-page-state") : NULL;
    if (st && st->on_revert)
        st->on_revert(st->user_data);
}

static void
on_footer_save_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    SettingsApp *app_ctx = user_data;
    GtkWidget *vis = app_ctx->stack ? gtk_stack_get_visible_child(GTK_STACK(app_ctx->stack)) : NULL;
    SettingsPageState *st = vis ? g_object_get_data(G_OBJECT(vis), "settings-page-state") : NULL;
    if (st && st->on_save)
        st->on_save(st->user_data);
}

static void
on_toggle_float_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    niri_toggle_window_floating();
}

static void
on_close_btn_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkWindow *win = GTK_WINDOW(user_data);
    gtk_window_close(win);
}

static gboolean
on_window_key_pressed(GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                      guint                  keyval,
                      guint                  keycode G_GNUC_UNUSED,
                      GdkModifierType        state,
                      gpointer               user_data)
{
    SettingsApp *app_ctx = user_data;
    GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask();

    if ((mods == GDK_CONTROL_MASK) && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
        if (app_ctx->search_entry) {
            gtk_widget_grab_focus(app_ctx->search_entry);
            return TRUE;
        }
    }
    if ((mods == GDK_CONTROL_MASK) && (keyval == GDK_KEY_q || keyval == GDK_KEY_Q)) {
        gtk_window_close(GTK_WINDOW(app_ctx->window));
        return TRUE;
    }
    if (mods == 0 && keyval == GDK_KEY_Escape && app_ctx->search_entry) {
        const gchar *txt = gtk_editable_get_text(GTK_EDITABLE(app_ctx->search_entry));
        if (txt && txt[0] != '\0') {
            gtk_editable_set_text(GTK_EDITABLE(app_ctx->search_entry), "");
            return TRUE;
        }
    }
    return FALSE;
}

static void
add_sidebar_category(GtkListBox *box, const gchar *name)
{
    GtkWidget *lbl = create_sidebar_category_header(name);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "sidebar-header-row");
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
    gtk_list_box_append(box, row);
}

static GtkListBoxRow *
append_nav_item(GtkListBox  *box,
                const gchar *glyph,
                const gchar *label_text,
                const gchar *badge_text,
                const gchar *badge_style,
                const gchar *tooltip_text,
                const gchar *page_name,
                const gchar *keywords)
{
    GtkWidget *inner = create_settings_nav_row(
        glyph, label_text, badge_text, badge_style, tooltip_text, page_name, keywords);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), inner);
    if (tooltip_text && tooltip_text[0] != '\0')
        gtk_widget_set_tooltip_text(row, tooltip_text);
    gtk_list_box_append(box, row);
    return GTK_LIST_BOX_ROW(row);
}

static void
select_sidebar_row_by_name(SettingsApp *app_ctx, const gchar *target_page)
{
    if (!app_ctx || !app_ctx->sidebar_list || !target_page)
        return;

    for (GtkWidget *child = gtk_widget_get_first_child(app_ctx->sidebar_list);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        if (!GTK_IS_LIST_BOX_ROW(child))
            continue;
        if (!gtk_list_box_row_get_selectable(GTK_LIST_BOX_ROW(child)))
            continue;
        GtkWidget *row_child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));
        if (!row_child)
            continue;

        const gchar *name = g_object_get_data(G_OBJECT(row_child), "page-name");
        if (g_strcmp0(name, target_page) == 0) {
            ensure_settings_page(app_ctx, target_page);
            gtk_list_box_select_row(GTK_LIST_BOX(app_ctx->sidebar_list), GTK_LIST_BOX_ROW(child));
            return;
        }
    }
}

GtkWidget *
desktop_settings_window_new_with_page(GtkApplication *app, const char *initial_page)
{
    SettingsApp *app_ctx = g_new0(SettingsApp, 1);

    g_autoptr(KajoThemeCfgRef) tcfg = kajo_config_store_acquire_theme(NULL);
    g_autoptr(KajoShellCfgRef) scfg = kajo_config_store_acquire_shell(NULL);
    g_autoptr(KajoIdeCfgRef)   icfg = kajo_config_store_acquire_ide(NULL);
    g_autoptr(KajoModCfgRef)   mcfg = kajo_config_store_acquire_mod(NULL);

    int init_w = CLAMP(tcfg ? tcfg->settings_width : 900, 480, 3840);
    int init_h = CLAMP(tcfg ? tcfg->settings_height : 640, 360, 2160);

    const gchar *theme_badge = settings_accent_short_name(tcfg ? tcfg->accent_color : "#0078d4");
    const gchar *panel_badge = (scfg && scfg->panel_position == PANEL_POSITION_BOTTOM) ? "BTM" : "TOP";
    guint total_widgets = ((scfg && scfg->layout_left) ? scfg->layout_left->len : 0) +
                          ((scfg && scfg->layout_center) ? scfg->layout_center->len : 0) +
                          ((scfg && scfg->layout_right) ? scfg->layout_right->len : 0);
    g_autofree gchar *widgets_badge = g_strdup_printf("%u", total_widgets);
    gboolean dnd_on = scfg ? scfg->dnd_enabled : FALSE;
    gint tout_ms = scfg ? scfg->toast_timeout_ms : 8000;
    g_autofree gchar *notif_badge = dnd_on
        ? g_strdup("DND")
        : g_strdup_printf("%dS", MAX(1, tout_ms / 1000));
    const gchar *ide_badge = (icfg && icfg->color_scheme && strstr(icfg->color_scheme, "light"))
        ? "LIGHT" : "DARK";
    const gchar *mod_badge = "A500";
    if (mcfg) {
        switch (mcfg->filter_mode) {
        case KAJO_MOD_FILTER_A1200:  mod_badge = "A1200"; break;
        case KAJO_MOD_FILTER_NEAR:   mod_badge = "NEAR";  break;
        case KAJO_MOD_FILTER_LINEAR: mod_badge = "LIN";   break;
        case KAJO_MOD_FILTER_SINC:   mod_badge = "SINC";  break;
        case KAJO_MOD_FILTER_A500:
        default:                     mod_badge = "A500";  break;
        }
    }

    app_ctx->current_width = init_w;
    app_ctx->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(app_ctx->window), "SYSTEM SETTINGS");
    gtk_window_set_default_size(GTK_WINDOW(app_ctx->window), init_w, init_h);
    gtk_window_set_decorated(GTK_WINDOW(app_ctx->window), FALSE);
    gtk_widget_add_css_class(app_ctx->window, "shell-settings-window");
    gtk_widget_add_css_class(app_ctx->window, "shell-settings-app");

    app_ctx->root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(app_ctx->root_vbox, TRUE);
    gtk_widget_set_vexpand(app_ctx->root_vbox, TRUE);

    /* ─── ZONE 1: Frameless Window Chrome Header Bar (.settings-header-bar) ─── */
    GtkWidget *window_handle = gtk_window_handle_new();
    GtkWidget *header_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header_bar, "settings-header-bar");
    gtk_widget_set_valign(header_bar, GTK_ALIGN_CENTER);

    app_ctx->btn_rail = gtk_button_new_with_label("☰");
    gtk_widget_add_css_class(app_ctx->btn_rail, "files-nav-btn");
    gtk_widget_set_valign(app_ctx->btn_rail, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(app_ctx->btn_rail, "Toggle Compact Icon Rail vs Full Category Sidebar");
    g_signal_connect(app_ctx->btn_rail, "clicked", G_CALLBACK(on_rail_toggle_clicked), app_ctx);

    app_ctx->hdr_title_lbl = gtk_label_new("⚙ SYSTEM SETTINGS");
    gtk_widget_add_css_class(app_ctx->hdr_title_lbl, "settings-header-title");
    gtk_widget_set_halign(app_ctx->hdr_title_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(app_ctx->hdr_title_lbl, GTK_ALIGN_CENTER);

    app_ctx->hdr_page_badge = gtk_label_new("01 / 12 • DISPLAYS");
    gtk_widget_add_css_class(app_ctx->hdr_page_badge, "shell-popover-badge");
    gtk_widget_add_css_class(app_ctx->hdr_page_badge, "accent");
    gtk_widget_set_valign(app_ctx->hdr_page_badge, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(app_ctx->hdr_page_badge), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(app_ctx->hdr_page_badge), 3);

    GtkWidget *search_center_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(search_center_box, TRUE);
    gtk_widget_set_halign(search_center_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(search_center_box, GTK_ALIGN_CENTER);

    app_ctx->search_entry = gtk_search_entry_new();
    gtk_widget_add_css_class(app_ctx->search_entry, "settings-header-search");
    gtk_editable_set_width_chars(GTK_EDITABLE(app_ctx->search_entry), 1);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(app_ctx->search_entry), 26);
    gtk_widget_set_hexpand(app_ctx->search_entry, TRUE);
    gtk_widget_set_valign(app_ctx->search_entry, GTK_ALIGN_CENTER);
    g_object_set(app_ctx->search_entry, "placeholder-text", "Filter settings (Ctrl+F)...", NULL);
    g_signal_connect(app_ctx->search_entry, "search-changed", G_CALLBACK(on_search_changed), app_ctx);
    gtk_box_append(GTK_BOX(search_center_box), app_ctx->search_entry);

    app_ctx->btn_float = gtk_button_new_with_label("🗗 FLOAT / TILE");
    gtk_widget_add_css_class(app_ctx->btn_float, "files-nav-btn");
    gtk_widget_set_valign(app_ctx->btn_float, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(app_ctx->btn_float, "Toggle Floating / Tiling Window (Niri IPC)");
    GtkWidget *fl_lbl = gtk_button_get_child(GTK_BUTTON(app_ctx->btn_float));
    if (fl_lbl && GTK_IS_LABEL(fl_lbl)) {
        gtk_label_set_ellipsize(GTK_LABEL(fl_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(fl_lbl), 2);
    }
    g_signal_connect(app_ctx->btn_float, "clicked", G_CALLBACK(on_toggle_float_clicked), NULL);

    app_ctx->btn_close = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(app_ctx->btn_close, "settings-close-btn");
    gtk_widget_set_valign(app_ctx->btn_close, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(app_ctx->btn_close, "Close Window (Ctrl+Q)");
    g_signal_connect(app_ctx->btn_close, "clicked", G_CALLBACK(on_close_btn_clicked), app_ctx->window);

    gtk_box_append(GTK_BOX(header_bar), app_ctx->btn_rail);
    gtk_box_append(GTK_BOX(header_bar), app_ctx->hdr_title_lbl);
    gtk_box_append(GTK_BOX(header_bar), app_ctx->hdr_page_badge);
    gtk_box_append(GTK_BOX(header_bar), search_center_box);
    gtk_box_append(GTK_BOX(header_bar), app_ctx->btn_float);
    gtk_box_append(GTK_BOX(header_bar), app_ctx->btn_close);

    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(window_handle), header_bar);
    gtk_box_append(GTK_BOX(app_ctx->root_vbox), window_handle);

    /* ─── MAIN SPLIT: Adaptive Navigation Sidebar + Right Page Viewport ─── */
    app_ctx->main_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_resize_start_child(GTK_PANED(app_ctx->main_paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(app_ctx->main_paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(app_ctx->main_paned), TRUE);
    gtk_paned_set_position(GTK_PANED(app_ctx->main_paned), 224);
    gtk_widget_set_hexpand(app_ctx->main_paned, TRUE);
    gtk_widget_set_vexpand(app_ctx->main_paned, TRUE);

    /* Left Sidebar VBox (224px normal via paned position, 46px minimum icon rail) */
    app_ctx->sidebar_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(app_ctx->sidebar_vbox, "settings-sidebar-vbox");
    gtk_widget_set_size_request(app_ctx->sidebar_vbox, 46, -1);

    app_ctx->sidebar_list = gtk_list_box_new();
    gtk_widget_add_css_class(app_ctx->sidebar_list, "navigation-sidebar");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app_ctx->sidebar_list), GTK_SELECTION_BROWSE);

    GtkListBox *nav = GTK_LIST_BOX(app_ctx->sidebar_list);

    /* Category 1: HARDWARE & SYSTEM */
    add_sidebar_category(nav, "HARDWARE & SYSTEM");
    append_nav_item(nav, "🖥", "Displays & Posture", "NIRI", NULL,
                    "Displays, Idle & Foldable Posture", "displays",
                    "display monitor screen resolution refresh scale scaling vrr orientation idle screensaver hinge posture foldable");
    append_nav_item(nav, "🌐", "Network & Wi-Fi", "NM", NULL,
                    "Network & Wi-Fi (NetworkManager)", "network",
                    "network wifi wireless ethernet connection access point ssid internet ip wpa");
    append_nav_item(nav, "ᛒ", "Bluetooth", "BT", NULL,
                    "Bluetooth Adapter & Devices (BlueZ 5)", "bluetooth",
                    "bluetooth devices paired pairing discovery scan controller connect bluez");
    append_nav_item(nav, "🔊", "Audio & PipeWire", "PW", NULL,
                    "Audio Routing & PipeWire", "audio",
                    "audio sound volume speaker microphone mic headset pipewire pulse sink source mixer channels");

    /* Category 2: SHELL & COMPOSITOR */
    add_sidebar_category(nav, "SHELL & COMPOSITOR");
    append_nav_item(nav, "🎨", "Theme & Fonts", theme_badge, "accent",
                    "Metro Accent Palette & Typography", "theme",
                    "theme accent color palette font typography css metro aero style appearance");
    append_nav_item(nav, "▬", "Panel, Touch & OSK", panel_badge, NULL,
                    "Panel Geometry, Touch Gestures & Virtual Keyboard", "panel",
                    "panel geometry autohide height position top bottom edge bar touch gesture swipe osk keyboard virtual");
    append_nav_item(nav, "🧩", "Panel Widgets", widgets_badge, NULL,
                    "Panel Bar Widgets (Left / Center / Right)", "widgets",
                    "widgets layout ordering panel left center right items modules add remove");
    append_nav_item(nav, "🔔", "Notifications", notif_badge, dnd_on ? "peak-warn" : NULL,
                    "Notification Daemon & Toast Popups", "notifications",
                    "notifications toast banner timeout history dnd do not disturb alert daemon focus");
    append_nav_item(nav, "❖", "Niri & Shaders", "GLSL", NULL,
                    "Niri Layer Surface Rules & GLSL Transition Shaders", "niri",
                    "niri compositor rules layout gaps blur xray window floating tiling kdl shader shaders glsl animation");

    /* Category 3: KAJO SUITE APPS */
    add_sidebar_category(nav, "KAJO SUITE APPS");
    append_nav_item(nav, "🮖", "Kajo IDE", ide_badge, NULL,
                    "Kajo IDE Modal Code Editor, Gutter & Color Schemes", "ide",
                    "ide editor code syntax color scheme theme font size monospace tab indent minimap gutter kajo-ide");
    append_nav_item(nav, "🎹", "Kajo Mod", mod_badge, NULL,
                    "Kajo Mod — Amiga UADE, C64 SID & Tracker Defaults", "mod",
                    "mod uade amiga paula sid c64 residfp openmpt tracker hvsc stereo archive kajo-mod");

    /* Category 4: SYSTEM INFO */
    add_sidebar_category(nav, "SYSTEM INFO");
    append_nav_item(nav, "ℹ", "About Kajo // OS", "v0.1", NULL,
                    "About Kajo // OS & Suite Inventory", "about",
                    "about system hardware specs cpu memory ram kernel version os linux desktop suite");

    g_signal_connect(app_ctx->sidebar_list, "row-selected", G_CALLBACK(on_sidebar_row_selected), app_ctx);

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll), app_ctx->sidebar_list);
    gtk_widget_set_vexpand(sidebar_scroll, TRUE);
    gtk_box_append(GTK_BOX(app_ctx->sidebar_vbox), sidebar_scroll);

    /* Right Page Viewport: Zone 2 (Pinned Deck) + Zone 3 (Scroll Canvas) + Zone 4 (Footer) */
    app_ctx->right_viewport = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(app_ctx->right_viewport, TRUE);
    gtk_widget_set_vexpand(app_ctx->right_viewport, TRUE);

    /* ZONE 2: Pinned Page Header & 4-Cell Live Telemetry Strip */
    app_ctx->deck_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(app_ctx->deck_box, "settings-page-deck");
    gtk_widget_set_hexpand(app_ctx->deck_box, TRUE);

    GtkWidget *deck_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(deck_top, GTK_ALIGN_CENTER);

    GtkWidget *deck_titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(deck_titles, TRUE);
    gtk_widget_set_valign(deck_titles, GTK_ALIGN_CENTER);

    app_ctx->deck_title_lbl = gtk_label_new("");
    gtk_widget_add_css_class(app_ctx->deck_title_lbl, "shell-popover-title");
    gtk_widget_set_hexpand(app_ctx->deck_title_lbl, TRUE);
    gtk_widget_set_halign(app_ctx->deck_title_lbl, GTK_ALIGN_FILL);
    gtk_widget_set_valign(app_ctx->deck_title_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(app_ctx->deck_title_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app_ctx->deck_title_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(app_ctx->deck_title_lbl), 6);

    app_ctx->deck_sub_lbl = gtk_label_new("");
    gtk_widget_add_css_class(app_ctx->deck_sub_lbl, "shell-popover-subtitle");
    gtk_widget_set_hexpand(app_ctx->deck_sub_lbl, TRUE);
    gtk_widget_set_halign(app_ctx->deck_sub_lbl, GTK_ALIGN_FILL);
    gtk_widget_set_valign(app_ctx->deck_sub_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(app_ctx->deck_sub_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app_ctx->deck_sub_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(app_ctx->deck_sub_lbl), 6);

    gtk_box_append(GTK_BOX(deck_titles), app_ctx->deck_title_lbl);
    gtk_box_append(GTK_BOX(deck_titles), app_ctx->deck_sub_lbl);

    GtkWidget *deck_badges = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(deck_badges, GTK_ALIGN_CENTER);

    app_ctx->deck_badge1_lbl = gtk_label_new("");
    gtk_widget_add_css_class(app_ctx->deck_badge1_lbl, "shell-popover-badge");
    gtk_widget_add_css_class(app_ctx->deck_badge1_lbl, "accent");
    gtk_widget_set_valign(app_ctx->deck_badge1_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(app_ctx->deck_badge1_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(app_ctx->deck_badge1_lbl), 4);

    app_ctx->deck_badge2_lbl = gtk_label_new("");
    gtk_widget_add_css_class(app_ctx->deck_badge2_lbl, "shell-popover-badge");
    gtk_widget_add_css_class(app_ctx->deck_badge2_lbl, "success");
    gtk_widget_set_valign(app_ctx->deck_badge2_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(app_ctx->deck_badge2_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(app_ctx->deck_badge2_lbl), 4);

    gtk_box_append(GTK_BOX(deck_badges), app_ctx->deck_badge1_lbl);
    gtk_box_append(GTK_BOX(deck_badges), app_ctx->deck_badge2_lbl);

    gtk_box_append(GTK_BOX(deck_top), deck_titles);
    gtk_box_append(GTK_BOX(deck_top), deck_badges);
    gtk_box_append(GTK_BOX(app_ctx->deck_box), deck_top);

    app_ctx->counter_grid = gtk_grid_new();
    gtk_widget_add_css_class(app_ctx->counter_grid, "settings-counter-strip");
    gtk_grid_set_column_homogeneous(GTK_GRID(app_ctx->counter_grid), TRUE);
    gtk_widget_set_hexpand(app_ctx->counter_grid, TRUE);

    for (int i = 0; i < 4; i++) {
        GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(cell, "settings-counter-cell");
        gtk_widget_set_hexpand(cell, TRUE);
        gtk_widget_set_valign(cell, GTK_ALIGN_CENTER);

        GtkWidget *lk = gtk_label_new("");
        gtk_widget_add_css_class(lk, "settings-counter-key");
        gtk_widget_set_valign(lk, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(lk, GTK_ALIGN_START);

        GtkWidget *lv = gtk_label_new("");
        gtk_widget_add_css_class(lv, "settings-counter-val");
        gtk_widget_set_hexpand(lv, TRUE);
        gtk_widget_set_halign(lv, GTK_ALIGN_FILL);
        gtk_widget_set_valign(lv, GTK_ALIGN_CENTER);
        gtk_label_set_xalign(GTK_LABEL(lv), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lv), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(lv), 4);

        gtk_box_append(GTK_BOX(cell), lk);
        gtk_box_append(GTK_BOX(cell), lv);

        app_ctx->cnt_cells[i]    = cell;
        app_ctx->cnt_key_lbls[i] = lk;
        app_ctx->cnt_val_lbls[i] = lv;
    }
    rearrange_counter_grid(app_ctx, FALSE);
    gtk_box_append(GTK_BOX(app_ctx->deck_box), settings_flex_bin_new(app_ctx->counter_grid));
    gtk_box_append(GTK_BOX(app_ctx->right_viewport), app_ctx->deck_box);

    /* ZONE 3: Scrollable Page Content Canvas (non-homogeneous GtkStack) */
    app_ctx->stack = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(app_ctx->stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(app_ctx->stack), FALSE);
    gtk_stack_set_transition_type(GTK_STACK(app_ctx->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(app_ctx->stack), 90);

    app_ctx->stack_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app_ctx->stack_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(app_ctx->stack_scroll),
                                  settings_flex_bin_new(app_ctx->stack));
    gtk_widget_set_hexpand(app_ctx->stack_scroll, TRUE);
    gtk_widget_set_vexpand(app_ctx->stack_scroll, TRUE);
    gtk_box_append(GTK_BOX(app_ctx->right_viewport), app_ctx->stack_scroll);

    /* ZONE 4: Docked Action & Persistence Footer Bar (.settings-footer-bar) */
    app_ctx->footer_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(app_ctx->footer_bar, "settings-footer-bar");
    gtk_widget_set_hexpand(app_ctx->footer_bar, TRUE);
    gtk_widget_set_valign(app_ctx->footer_bar, GTK_ALIGN_CENTER);

    GtkWidget *footer_left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(footer_left, TRUE);
    gtk_widget_set_valign(footer_left, GTK_ALIGN_CENTER);

    app_ctx->footer_state_badge = gtk_label_new("✓ ALL CHANGES SAVED");
    gtk_widget_add_css_class(app_ctx->footer_state_badge, "shell-popover-badge");
    gtk_widget_add_css_class(app_ctx->footer_state_badge, "success");
    gtk_widget_set_valign(app_ctx->footer_state_badge, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(app_ctx->footer_state_badge), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(app_ctx->footer_state_badge), 4);

    app_ctx->footer_status_lbl = gtk_label_new("~/.config/kajo/*.toml • Live IPC ready");
    gtk_widget_add_css_class(app_ctx->footer_status_lbl, "row-subtitle");
    gtk_widget_set_hexpand(app_ctx->footer_status_lbl, TRUE);
    gtk_widget_set_halign(app_ctx->footer_status_lbl, GTK_ALIGN_FILL);
    gtk_widget_set_valign(app_ctx->footer_status_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(app_ctx->footer_status_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app_ctx->footer_status_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(app_ctx->footer_status_lbl), 1);

    gtk_box_append(GTK_BOX(footer_left), app_ctx->footer_state_badge);
    gtk_box_append(GTK_BOX(footer_left), app_ctx->footer_status_lbl);

    app_ctx->footer_actions_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(app_ctx->footer_actions_box, GTK_ALIGN_END);
    gtk_widget_set_valign(app_ctx->footer_actions_box, GTK_ALIGN_CENTER);

    app_ctx->btn_secondary = gtk_button_new_with_label("↺ RESET DEFAULTS");
    gtk_widget_add_css_class(app_ctx->btn_secondary, "files-nav-btn");
    gtk_widget_set_valign(app_ctx->btn_secondary, GTK_ALIGN_CENTER);
    set_shrinkable_button_label(app_ctx->btn_secondary, "↺ RESET DEFAULTS", 4);
    g_signal_connect(app_ctx->btn_secondary, "clicked", G_CALLBACK(on_footer_secondary_clicked), app_ctx);

    app_ctx->btn_revert = gtk_button_new_with_label("↺ REVERT");
    gtk_widget_add_css_class(app_ctx->btn_revert, "files-nav-btn");
    gtk_widget_set_valign(app_ctx->btn_revert, GTK_ALIGN_CENTER);
    set_shrinkable_button_label(app_ctx->btn_revert, "↺ REVERT", 3);
    g_signal_connect(app_ctx->btn_revert, "clicked", G_CALLBACK(on_footer_revert_clicked), app_ctx);

    app_ctx->btn_save = gtk_button_new_with_label("[ ✓ SAVE & APPLY ]");
    gtk_widget_add_css_class(app_ctx->btn_save, "files-nav-btn");
    gtk_widget_add_css_class(app_ctx->btn_save, "action-btn-primary");
    gtk_widget_set_valign(app_ctx->btn_save, GTK_ALIGN_CENTER);
    set_shrinkable_button_label(app_ctx->btn_save, "[ ✓ SAVE & APPLY ]", 6);
    g_signal_connect(app_ctx->btn_save, "clicked", G_CALLBACK(on_footer_save_clicked), app_ctx);

    gtk_box_append(GTK_BOX(app_ctx->footer_actions_box), app_ctx->btn_secondary);
    gtk_box_append(GTK_BOX(app_ctx->footer_actions_box), app_ctx->btn_revert);
    gtk_box_append(GTK_BOX(app_ctx->footer_actions_box), app_ctx->btn_save);

    gtk_box_append(GTK_BOX(app_ctx->footer_bar), footer_left);
    gtk_box_append(GTK_BOX(app_ctx->footer_bar), app_ctx->footer_actions_box);
    gtk_box_append(GTK_BOX(app_ctx->right_viewport), settings_flex_bin_new(app_ctx->footer_bar));

    gtk_paned_set_start_child(GTK_PANED(app_ctx->main_paned), app_ctx->sidebar_vbox);
    gtk_paned_set_end_child(GTK_PANED(app_ctx->main_paned), app_ctx->right_viewport);
    for (GtkWidget *pc = gtk_widget_get_first_child(app_ctx->main_paned);
         pc != NULL;
         pc = gtk_widget_get_next_sibling(pc)) {
        if (pc != app_ctx->sidebar_vbox && pc != app_ctx->right_viewport)
            gtk_widget_set_can_target(pc, FALSE);
    }
    gtk_box_append(GTK_BOX(app_ctx->root_vbox), app_ctx->main_paned);

    gtk_window_set_child(GTK_WINDOW(app_ctx->window), app_ctx->root_vbox);
    g_object_set_data_full(G_OBJECT(app_ctx->window), "app-ctx", app_ctx, settings_app_free);

    /* Window Keyboard Shortcuts: Ctrl+F (search), Ctrl+Q (close), Esc (clear search) */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_window_key_pressed), app_ctx);
    gtk_widget_add_controller(app_ctx->window, key_ctrl);

    /* Select initial page (lazy-builds only the requested page) */
    const gchar *target = (initial_page && initial_page[0] != '\0') ? initial_page : "displays";
    select_sidebar_row_by_name(app_ctx, target);
    if (!gtk_list_box_get_selected_row(GTK_LIST_BOX(app_ctx->sidebar_list)))
        select_sidebar_row_by_name(app_ctx, "displays");

    update_responsive_layout(app_ctx, init_w);

    g_signal_connect(app_ctx->window, "notify::default-width", G_CALLBACK(on_window_size_changed), app_ctx);
    g_signal_connect(app_ctx->window, "realize", G_CALLBACK(on_window_realize), app_ctx);

    return app_ctx->window;
}

GtkWidget *
desktop_settings_window_new(GtkApplication *app)
{
    return desktop_settings_window_new_with_page(app, "displays");
}

void
desktop_settings_window_set_simulated_column_width(GtkWidget *window, int col_width)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    SettingsApp *app_ctx = g_object_get_data(G_OBJECT(window), "app-ctx");
    if (!app_ctx)
        return;

    app_ctx->sim_col_width = col_width;
    if (col_width >= 480 && gtk_window_get_child(GTK_WINDOW(window)) == app_ctx->root_vbox) {
        g_object_ref(app_ctx->root_vbox);
        gtk_window_set_child(GTK_WINDOW(window), NULL);

        GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_paned_set_resize_start_child(GTK_PANED(hpaned), FALSE);
        gtk_paned_set_shrink_start_child(GTK_PANED(hpaned), TRUE);
        gtk_paned_set_position(GTK_PANED(hpaned), col_width);
        gtk_paned_set_start_child(GTK_PANED(hpaned), app_ctx->root_vbox);
        gtk_paned_set_end_child(GTK_PANED(hpaned), gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
        gtk_window_set_child(GTK_WINDOW(window), hpaned);
        g_object_unref(app_ctx->root_vbox);

        update_responsive_layout(app_ctx, col_width);
    }
}
