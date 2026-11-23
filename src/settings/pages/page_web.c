#include "../settings_common.h"
#include "../settings_pages.h"
#include "../../web/web_model.h"
#include <gio/gio.h>
#include <string.h>

static const char *const WEB_RAIL_MODE_LABELS[]  = { "AUTOHIDE", "COMPACT", "PINNED", NULL };
static const char *const WEB_RAIL_SIDE_LABELS[]  = { "LEFT", "RIGHT", NULL };
static const char *const WEB_TOPBAR_LABELS[]     = { "VISIBLE", "AUTOHIDE", NULL };
static const char *const WEB_NEWTAB_LABELS[]     = { "START", "BLANK", "CUSTOM", NULL };
static const char *const WEB_HW_LABELS[]         = { "ALWAYS", "AUTO", "NEVER", NULL };
static const char *const WEB_COOKIE_LABELS[]     = { "NO 3RD", "ALWAYS", "NEVER", NULL };
static const char *const WEB_AUTOPLAY_LABELS[]   = { "MUTED", "ALLOW", "DENY", NULL };
static const char *const WEB_SEARCH_LABELS[]     = { "DDG", "KAGI", "STARTPAGE", "BRAVE", "GOOGLE", "CUSTOM", NULL };
static const char *const WEB_UA_LABELS[]         = { "DEFAULT", "SAFARI", "CHROME", "CUSTOM", NULL };
static const char *const WEB_SCHEME_LABELS[]     = { "FOLLOW", "DARK", "LIGHT", NULL };
static const char *const WEB_DL_FMT_LABELS[]     = { "BEST MP4", "1080P", "720P", "OPUS", "MP3", NULL };

typedef struct _SettingsPageWeb {
    GtkBox parent_instance;

    /* Card 1: Rail & Chrome */
    GtkWidget *seg_rail_mode;
    GtkWidget *seg_rail_side;
    GtkWidget *scale_rail_w;
    GtkWidget *lbl_rail_w;
    GtkWidget *scale_reveal_ms;
    GtkWidget *lbl_reveal_ms;
    GtkWidget *scale_hide_ms;
    GtkWidget *lbl_hide_ms;
    GtkWidget *seg_topbar;
    GtkWidget *sw_tab_domains;
    GtkWidget *seg_newtab;
    GtkWidget *entry_newtab_url;
    GtkWidget *sw_restore;

    /* Card 2: Hardware Acceleration & Resources */
    GtkWidget *seg_hw;
    GtkWidget *sw_webgl;
    GtkWidget *sw_gpu_webgl;
    GtkWidget *sw_smooth_scroll;
    GtkWidget *scale_suspend_min;
    GtkWidget *lbl_suspend_min;
    GtkWidget *scale_mem_mb;
    GtkWidget *lbl_mem_mb;

    /* Card 3: Privacy, Ad Blocker & Media Downloader */
    GtkWidget *sw_adblock;
    GtkWidget *entry_adblock_rules;
    GtkWidget *sw_userscripts;
    GtkWidget *sw_itp;
    GtkWidget *seg_cookies;
    GtkWidget *sw_https_only;
    GtkWidget *seg_autoplay;
    GtkWidget *sw_webrtc;
    GtkWidget *sw_dns_prefetch;
    GtkWidget *dd_dl_fmt;

    /* Card 4: Profiles & Google Drive AppData Sync */
    GtkWidget *sw_picker_startup;
    GtkWidget *sw_profile_border;
    GtkWidget *entry_oauth_id;
    GtkWidget *entry_oauth_secret;
    GtkWidget *sw_sync_bookmarks;
    GtkWidget *sw_sync_history;
    GtkWidget *sw_sync_tabs;
    GtkWidget *sw_sync_settings;
    GtkWidget *sw_sync_e2ee;

    /* Card 5: Search, Zoom & User-Agent */
    GtkWidget *dd_search;
    GtkWidget *entry_search_url;
    GtkWidget *scale_zoom;
    GtkWidget *lbl_zoom;
    GtkWidget *seg_ua;
    GtkWidget *entry_ua_custom;

    /* Card 6: Appearance, Downloads & DevTools */
    GtkWidget *seg_scheme;
    GtkWidget *scale_font_px;
    GtkWidget *lbl_font_px;
    GtkWidget *scale_mono_px;
    GtkWidget *lbl_mono_px;
    GtkWidget *scale_min_px;
    GtkWidget *lbl_min_px;
    GtkWidget *entry_dl_dir;
    GtkWidget *sw_ask_dl;
    GtkWidget *sw_devtools;

    KajoWebCfg *saved;
    gboolean    syncing_ui;
} SettingsPageWeb;

typedef struct _SettingsPageWebClass {
    GtkBoxClass parent_class;
} SettingsPageWebClass;

G_DEFINE_TYPE(SettingsPageWeb, settings_page_web, GTK_TYPE_BOX)

static void
collect_ui_into_cfg(SettingsPageWeb *self, KajoWebCfg *out)
{
    out->rail_mode            = (KajoWebRailMode)MIN(segmented_group_get_selected(self->seg_rail_mode), 2u);
    out->rail_side            = (KajoWebRailSide)MIN(segmented_group_get_selected(self->seg_rail_side), 1u);
    out->rail_width_px        = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_rail_w)) + 0.5), 180, 360);
    out->rail_reveal_delay_ms = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_reveal_ms)) + 0.5), 0, 300);
    out->rail_hide_delay_ms   = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_hide_ms)) + 0.5), 50, 1000);
    out->top_bar_mode         = (KajoWebTopBarMode)MIN(segmented_group_get_selected(self->seg_topbar), 1u);
    out->show_tab_domains     = gtk_switch_get_active(GTK_SWITCH(self->sw_tab_domains));
    out->new_tab_page         = (KajoWebNewTabMode)MIN(segmented_group_get_selected(self->seg_newtab), 2u);
    g_free(out->new_tab_custom_url);
    out->new_tab_custom_url   = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_newtab_url)));
    out->restore_session      = gtk_switch_get_active(GTK_SWITCH(self->sw_restore));

    out->hardware_acceleration = (KajoWebHwAccel)MIN(segmented_group_get_selected(self->seg_hw), 2u);
    out->enable_webgl          = gtk_switch_get_active(GTK_SWITCH(self->sw_webgl));
    out->gpu_process_webgl     = gtk_switch_get_active(GTK_SWITCH(self->sw_gpu_webgl));
    out->smooth_scrolling      = gtk_switch_get_active(GTK_SWITCH(self->sw_smooth_scroll));
    out->suspend_idle_min      = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_suspend_min)) + 0.5), 0, 240);
    if (out->suspend_idle_min > 0 && out->suspend_idle_min < 5)
        out->suspend_idle_min = 5;
    out->memory_limit_mb       = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_mem_mb)) + 0.5), 512, 16384);

    out->adblock_enabled      = gtk_switch_get_active(GTK_SWITCH(self->sw_adblock));
    g_free(out->adblock_custom_rules_path);
    out->adblock_custom_rules_path = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_adblock_rules)));
    out->user_scripts_enabled = gtk_switch_get_active(GTK_SWITCH(self->sw_userscripts));
    out->itp_enabled          = gtk_switch_get_active(GTK_SWITCH(self->sw_itp));
    out->cookie_policy        = (KajoWebCookiePolicy)MIN(segmented_group_get_selected(self->seg_cookies), 2u);
    out->https_only           = gtk_switch_get_active(GTK_SWITCH(self->sw_https_only));
    out->autoplay_policy      = (KajoWebAutoplayPolicy)MIN(segmented_group_get_selected(self->seg_autoplay), 2u);
    out->enable_webrtc        = gtk_switch_get_active(GTK_SWITCH(self->sw_webrtc));
    out->dns_prefetch         = gtk_switch_get_active(GTK_SWITCH(self->sw_dns_prefetch));
    out->media_download_format = (KajoWebDownloadFmt)MIN(gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_dl_fmt)), 4u);

    out->show_picker_on_startup = gtk_switch_get_active(GTK_SWITCH(self->sw_picker_startup));
    out->profile_border_accent  = gtk_switch_get_active(GTK_SWITCH(self->sw_profile_border));
    g_free(out->sync_oauth_client_id);
    out->sync_oauth_client_id = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_oauth_id)));
    g_free(out->sync_oauth_client_secret);
    out->sync_oauth_client_secret = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_oauth_secret)));
    out->sync_bookmarks       = gtk_switch_get_active(GTK_SWITCH(self->sw_sync_bookmarks));
    out->sync_history         = gtk_switch_get_active(GTK_SWITCH(self->sw_sync_history));
    out->sync_tabs_and_groups = gtk_switch_get_active(GTK_SWITCH(self->sw_sync_tabs));
    out->sync_settings        = gtk_switch_get_active(GTK_SWITCH(self->sw_sync_settings));
    out->sync_e2ee_enabled    = gtk_switch_get_active(GTK_SWITCH(self->sw_sync_e2ee));

    out->search_engine        = (KajoWebSearchEngine)MIN(gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_search)), 5u);
    g_free(out->search_custom_url);
    out->search_custom_url    = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_search_url)));
    out->default_zoom_pct     = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_zoom)) + 0.5), 50, 200);
    out->user_agent_preset    = (KajoWebUaPreset)MIN(segmented_group_get_selected(self->seg_ua), 3u);
    g_free(out->user_agent_custom);
    out->user_agent_custom    = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_ua_custom)));

    out->color_scheme         = (KajoWebColorScheme)MIN(segmented_group_get_selected(self->seg_scheme), 2u);
    out->default_font_size_px = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_font_px)) + 0.5), 10, 32);
    out->mono_font_size_px    = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_mono_px)) + 0.5), 10, 28);
    out->min_font_size_px     = CLAMP((int)(gtk_range_get_value(GTK_RANGE(self->scale_min_px)) + 0.5), 0, 24);
    g_free(out->download_dir);
    out->download_dir         = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_dl_dir)));
    out->ask_download_location = gtk_switch_get_active(GTK_SWITCH(self->sw_ask_dl));
    out->developer_extras     = gtk_switch_get_active(GTK_SWITCH(self->sw_devtools));
}

static void
apply_cfg_to_ui(SettingsPageWeb *self, const KajoWebCfg *cfg)
{
    self->syncing_ui = TRUE;

    segmented_group_set_selected(self->seg_rail_mode, (guint)cfg->rail_mode);
    segmented_group_set_selected(self->seg_rail_side, (guint)cfg->rail_side);
    gtk_range_set_value(GTK_RANGE(self->scale_rail_w), (double)cfg->rail_width_px);
    gtk_range_set_value(GTK_RANGE(self->scale_reveal_ms), (double)cfg->rail_reveal_delay_ms);
    gtk_range_set_value(GTK_RANGE(self->scale_hide_ms), (double)cfg->rail_hide_delay_ms);
    segmented_group_set_selected(self->seg_topbar, (guint)cfg->top_bar_mode);
    gtk_switch_set_active(GTK_SWITCH(self->sw_tab_domains), cfg->show_tab_domains);
    segmented_group_set_selected(self->seg_newtab, (guint)cfg->new_tab_page);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_newtab_url), cfg->new_tab_custom_url ? cfg->new_tab_custom_url : "");
    gtk_switch_set_active(GTK_SWITCH(self->sw_restore), cfg->restore_session);

    segmented_group_set_selected(self->seg_hw, (guint)cfg->hardware_acceleration);
    gtk_switch_set_active(GTK_SWITCH(self->sw_webgl), cfg->enable_webgl);
    gtk_switch_set_active(GTK_SWITCH(self->sw_gpu_webgl), cfg->gpu_process_webgl);
    gtk_switch_set_active(GTK_SWITCH(self->sw_smooth_scroll), cfg->smooth_scrolling);
    gtk_range_set_value(GTK_RANGE(self->scale_suspend_min), (double)cfg->suspend_idle_min);
    gtk_range_set_value(GTK_RANGE(self->scale_mem_mb), (double)cfg->memory_limit_mb);

    gtk_switch_set_active(GTK_SWITCH(self->sw_adblock), cfg->adblock_enabled);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_adblock_rules), cfg->adblock_custom_rules_path ? cfg->adblock_custom_rules_path : "");
    gtk_switch_set_active(GTK_SWITCH(self->sw_userscripts), cfg->user_scripts_enabled);
    gtk_switch_set_active(GTK_SWITCH(self->sw_itp), cfg->itp_enabled);
    segmented_group_set_selected(self->seg_cookies, (guint)cfg->cookie_policy);
    gtk_switch_set_active(GTK_SWITCH(self->sw_https_only), cfg->https_only);
    segmented_group_set_selected(self->seg_autoplay, (guint)cfg->autoplay_policy);
    gtk_switch_set_active(GTK_SWITCH(self->sw_webrtc), cfg->enable_webrtc);
    gtk_switch_set_active(GTK_SWITCH(self->sw_dns_prefetch), cfg->dns_prefetch);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_dl_fmt), (guint)cfg->media_download_format);

    gtk_switch_set_active(GTK_SWITCH(self->sw_picker_startup), cfg->show_picker_on_startup);
    gtk_switch_set_active(GTK_SWITCH(self->sw_profile_border), cfg->profile_border_accent);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_oauth_id), cfg->sync_oauth_client_id ? cfg->sync_oauth_client_id : "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_oauth_secret), cfg->sync_oauth_client_secret ? cfg->sync_oauth_client_secret : "");
    gtk_switch_set_active(GTK_SWITCH(self->sw_sync_bookmarks), cfg->sync_bookmarks);
    gtk_switch_set_active(GTK_SWITCH(self->sw_sync_history), cfg->sync_history);
    gtk_switch_set_active(GTK_SWITCH(self->sw_sync_tabs), cfg->sync_tabs_and_groups);
    gtk_switch_set_active(GTK_SWITCH(self->sw_sync_settings), cfg->sync_settings);
    gtk_switch_set_active(GTK_SWITCH(self->sw_sync_e2ee), cfg->sync_e2ee_enabled);

    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_search), (guint)cfg->search_engine);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_search_url), cfg->search_custom_url ? cfg->search_custom_url : "");
    gtk_range_set_value(GTK_RANGE(self->scale_zoom), (double)cfg->default_zoom_pct);
    segmented_group_set_selected(self->seg_ua, (guint)cfg->user_agent_preset);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_ua_custom), cfg->user_agent_custom ? cfg->user_agent_custom : "");

    segmented_group_set_selected(self->seg_scheme, (guint)cfg->color_scheme);
    gtk_range_set_value(GTK_RANGE(self->scale_font_px), (double)cfg->default_font_size_px);
    gtk_range_set_value(GTK_RANGE(self->scale_mono_px), (double)cfg->mono_font_size_px);
    gtk_range_set_value(GTK_RANGE(self->scale_min_px), (double)cfg->min_font_size_px);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_dl_dir), cfg->download_dir ? cfg->download_dir : "");
    gtk_switch_set_active(GTK_SWITCH(self->sw_ask_dl), cfg->ask_download_location);
    gtk_switch_set_active(GTK_SWITCH(self->sw_devtools), cfg->developer_extras);

    self->syncing_ui = FALSE;
}

static void
update_web_telemetry_and_dirty(SettingsPageWeb *self)
{
    if (!self || self->syncing_ui)
        return;

    g_autoptr(KajoWebCfg) cur = kajo_web_cfg_new_default();
    collect_ui_into_cfg(self, cur);

    GtkWidget *page = GTK_WIDGET(self);
    const gchar *rail_str = WEB_RAIL_MODE_LABELS[MIN((guint)cur->rail_mode, 2u)];
    const gchar *hw_str   = (cur->hardware_acceleration == KAJO_WEB_HW_ALWAYS) ? "GPU ALWAYS"
                          : (cur->hardware_acceleration == KAJO_WEB_HW_ON_DEMAND) ? "ON DEMAND" : "⚠ SW ONLY";
    const gchar *ab_str   = cur->adblock_enabled ? "SHIELD ON" : "SHIELD OFF";
    const gchar *srch_str = WEB_SEARCH_LABELS[MIN((guint)cur->search_engine, 5u)];

    settings_page_set_counter(page, 0, "RAIL", rail_str, "accent");
    settings_page_set_counter(page, 1, "GPU", hw_str,
                              cur->hardware_acceleration == KAJO_WEB_HW_NEVER ? "warn" : "success");
    settings_page_set_counter(page, 2, "BLOCKER", ab_str, cur->adblock_enabled ? "success" : "warn");
    settings_page_set_counter(page, 3, "SEARCH", srch_str, NULL);
    settings_page_set_nav_badge(page, "web", rail_str, NULL);

    gboolean dirty = !kajo_cfg_domain_equal(kajo_schema_get_web(), self->saved, cur);
    settings_page_set_dirty(page, dirty);
}

static void on_web_seg_changed(GtkWidget *g G_GNUC_UNUSED, guint s G_GNUC_UNUSED, gpointer ud) { update_web_telemetry_and_dirty(ud); }
static void on_web_scale_changed(GtkRange *r G_GNUC_UNUSED, gpointer ud) { update_web_telemetry_and_dirty(ud); }
static void on_web_prop_changed(GObject *o G_GNUC_UNUSED, GParamSpec *p G_GNUC_UNUSED, gpointer ud) { update_web_telemetry_and_dirty(ud); }
static void on_web_entry_changed(GtkEditable *e G_GNUC_UNUSED, gpointer ud) { update_web_telemetry_and_dirty(ud); }

static void
action_reset_web_defaults(gpointer user_data)
{
    SettingsPageWeb *self = user_data;
    g_autoptr(KajoWebCfg) def = kajo_web_cfg_new_default();
    apply_cfg_to_ui(self, def);
    update_web_telemetry_and_dirty(self);
}

static void
action_revert_web(gpointer user_data)
{
    SettingsPageWeb *self = user_data;
    apply_cfg_to_ui(self, self->saved);
    update_web_telemetry_and_dirty(self);
}

static void
mutate_save_web(KajoWebCfg *draft, gpointer user_data)
{
    const KajoWebCfg *src = user_data;
    g_autoptr(KajoWebCfg) copy = kajo_web_cfg_copy(src);

    /* Transfer all fields cleanly */
    g_free(draft->new_tab_custom_url);
    g_free(draft->adblock_custom_rules_path);
    g_free(draft->search_custom_url);
    g_free(draft->user_agent_custom);
    g_free(draft->download_dir);
    g_free(draft->sync_oauth_client_id);
    g_free(draft->sync_oauth_client_secret);

    *draft = *copy;
    draft->new_tab_custom_url        = g_strdup(src->new_tab_custom_url);
    draft->adblock_custom_rules_path = g_strdup(src->adblock_custom_rules_path);
    draft->search_custom_url         = g_strdup(src->search_custom_url);
    draft->user_agent_custom         = g_strdup(src->user_agent_custom);
    draft->download_dir              = g_strdup(src->download_dir);
    draft->sync_oauth_client_id      = g_strdup(src->sync_oauth_client_id);
    draft->sync_oauth_client_secret  = g_strdup(src->sync_oauth_client_secret);
}

static void
action_save_web(gpointer user_data)
{
    SettingsPageWeb *self = user_data;
    if (!self || self->syncing_ui)
        return;

    g_autoptr(KajoWebCfg) next = kajo_web_cfg_new_default();
    collect_ui_into_cfg(self, next);
    kajo_config_store_update_web(NULL, mutate_save_web, next, NULL);

    kajo_web_cfg_free(self->saved);
    self->saved = kajo_web_cfg_copy(next);
    update_web_telemetry_and_dirty(self);
}

static GtkWidget *
make_switch(SettingsPageWeb *self)
{
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_web_prop_changed), self);
    return sw;
}

static GtkWidget *
make_entry(SettingsPageWeb *self, const gchar *placeholder, gboolean secret)
{
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(e), 12);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(e), 28);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    if (secret)
        gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
    gtk_widget_set_valign(e, GTK_ALIGN_CENTER);
    g_signal_connect(e, "changed", G_CALLBACK(on_web_entry_changed), self);
    return e;
}

static void
on_web_page_mapped(GtkWidget *w, gpointer ud G_GNUC_UNUSED)
{
    update_web_telemetry_and_dirty((SettingsPageWeb *)w);
}

static void
settings_page_web_dispose(GObject *obj)
{
    SettingsPageWeb *self = (SettingsPageWeb *)obj;
    g_clear_pointer(&self->saved, kajo_web_cfg_free);
    G_OBJECT_CLASS(settings_page_web_parent_class)->dispose(obj);
}

static void
settings_page_web_init(SettingsPageWeb *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 10);
    gtk_widget_add_css_class(GTK_WIDGET(self), "settings-bento-canvas");
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    g_object_set_data(G_OBJECT(self), "is-bento-canvas", GINT_TO_POINTER(1));
}

static void
settings_page_web_class_init(SettingsPageWebClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = settings_page_web_dispose;
}

GtkWidget *
build_page_web(void)
{
    SettingsPageWeb *self = g_object_new(settings_page_web_get_type(), NULL);
    g_autoptr(KajoWebCfgRef) snap = kajo_config_store_acquire_web(NULL);
    self->saved = snap ? kajo_web_cfg_copy(snap) : kajo_web_cfg_new_default();

    /* ─── Card 1: Rail & Chrome ─── */
    GtkWidget *c1 = create_section_card(SETTINGS_CARD_LEFT, "1. VERTICAL TAB RAIL & CHROME", "AUTOHIDE", "accent", NULL);

    self->seg_rail_mode = create_segmented_group(WEB_RAIL_MODE_LABELS, 0);
    segmented_group_connect(self->seg_rail_mode, on_web_seg_changed, self);
    gtk_box_append(GTK_BOX(c1), create_boxed_row("◧", "Vertical Rail Mode",
        "Autohide overlays on edge hover (zero page reflow); Compact 44px; Pinned split", self->seg_rail_mode));

    self->seg_rail_side = create_segmented_group(WEB_RAIL_SIDE_LABELS, 0);
    segmented_group_connect(self->seg_rail_side, on_web_seg_changed, self);
    gtk_box_append(GTK_BOX(c1), create_boxed_row("⇄", "Rail Edge",
        "Anchor the vertical tab rail to the left or right window edge", self->seg_rail_side));

    GtkWidget *rw_wrap = create_scale_with_badge(180, 360, 10, 240, "%.0fPX", &self->scale_rail_w, &self->lbl_rail_w);
    g_signal_connect(self->scale_rail_w, "value-changed", G_CALLBACK(on_web_scale_changed), self);
    gtk_box_append(GTK_BOX(c1), create_boxed_row("↔", "Expanded Rail Width", "Width of expanded vertical tab drawer", rw_wrap));

    GtkWidget *rd_wrap = create_scale_with_badge(0, 300, 5, 35, "%.0fMS", &self->scale_reveal_ms, &self->lbl_reveal_ms);
    g_signal_connect(self->scale_reveal_ms, "value-changed", G_CALLBACK(on_web_scale_changed), self);
    gtk_box_append(GTK_BOX(c1), create_boxed_row("⏱", "Autohide Reveal Delay", "Hover dwell before rail slides open", rd_wrap));

    GtkWidget *hd_wrap = create_scale_with_badge(50, 1000, 10, 220, "%.0fMS", &self->scale_hide_ms, &self->lbl_hide_ms);
    g_signal_connect(self->scale_hide_ms, "value-changed", G_CALLBACK(on_web_scale_changed), self);
    gtk_box_append(GTK_BOX(c1), create_boxed_row("⏳", "Autohide Collapse Delay", "Grace window after pointer leaves rail", hd_wrap));

    self->seg_topbar = create_segmented_group(WEB_TOPBAR_LABELS, 0);
    segmented_group_connect(self->seg_topbar, on_web_seg_changed, self);
    gtk_box_append(GTK_BOX(c1), create_boxed_row("▬", "Top Chrome Bar", "Always visible (34px) or Zen autohide", self->seg_topbar));

    self->sw_tab_domains = make_switch(self);
    gtk_box_append(GTK_BOX(c1), create_boxed_row("◉", "Show Domain Under Tab Title", "Display hostname subtitle on vertical tab rows", self->sw_tab_domains));

    self->seg_newtab = create_segmented_group(WEB_NEWTAB_LABELS, 0);
    segmented_group_connect(self->seg_newtab, on_web_seg_changed, self);
    gtk_box_append(GTK_BOX(c1), create_boxed_row("+", "New Tab Page", "kajo://newtab speed dial, blank, or custom URL", self->seg_newtab));

    self->entry_newtab_url = make_entry(self, "https://...", FALSE);
    gtk_box_append(GTK_BOX(c1), create_boxed_row("🔗", "Custom New Tab URL", "Used when New Tab Page is set to CUSTOM", self->entry_newtab_url));

    self->sw_restore = make_switch(self);
    gtk_box_append(GTK_BOX(c1), create_boxed_row("↺", "Restore Previous Session", "Reopen tabs & groups from last profile run", self->sw_restore));

    /* ─── Card 2: Hardware Acceleration & Resources ─── */
    GtkWidget *c2 = create_section_card(SETTINGS_CARD_CENTER, "2. HARDWARE ACCELERATION & RESOURCES", "DMA-BUF + SKIA", "success", NULL);

    self->seg_hw = create_segmented_group(WEB_HW_LABELS, 0);
    segmented_group_connect(self->seg_hw, on_web_seg_changed, self);
    gtk_box_append(GTK_BOX(c2), create_boxed_row("⚡", "Hardware Acceleration",
        "ALWAYS enables Skia GPU + zero-copy Wayland DMA-BUF; NEVER shows [⚠ SW RENDER]", self->seg_hw));

    self->sw_webgl = make_switch(self);
    gtk_box_append(GTK_BOX(c2), create_boxed_row("🧊", "Enable WebGL", "Hardware-accelerated 3D canvas rendering", self->sw_webgl));

    self->sw_gpu_webgl = make_switch(self);
    gtk_box_append(GTK_BOX(c2), create_boxed_row("🖥", "GPU Process for WebGL", "Isolate WebGL inside WebKit GPU process", self->sw_gpu_webgl));

    self->sw_smooth_scroll = make_switch(self);
    gtk_box_append(GTK_BOX(c2), create_boxed_row("↕", "Smooth Scrolling", "120Hz/60Hz frame-paced wheel & kinetic scrolling", self->sw_smooth_scroll));

    GtkWidget *su_wrap = create_scale_with_badge(0, 240, 5, 30, "%.0fM", &self->scale_suspend_min, &self->lbl_suspend_min);
    g_signal_connect(self->scale_suspend_min, "value-changed", G_CALLBACK(on_web_scale_changed), self);
    gtk_box_append(GTK_BOX(c2), create_boxed_row("💤", "Suspend Idle Tabs (Minutes)", "0 disables; unloads background web processes keeping session state", su_wrap));

    GtkWidget *mm_wrap = create_scale_with_badge(512, 16384, 256, 2048, "%.0fMB", &self->scale_mem_mb, &self->lbl_mem_mb);
    g_signal_connect(self->scale_mem_mb, "value-changed", G_CALLBACK(on_web_scale_changed), self);
    gtk_box_append(GTK_BOX(c2), create_boxed_row("🧠", "Per-Tab Memory Limit", "WebKitMemoryPressureSettings threshold per web process", mm_wrap));

    settings_bento_add_pair(GTK_WIDGET(self), c1, c2);

    /* ─── Card 3: Privacy, Ad Blocker & Media Downloader ─── */
    g_autofree gchar *ffmpeg_p = g_find_program_in_path("ffmpeg");
    g_autofree gchar *ytdlp_p  = g_find_program_in_path("yt-dlp");
    g_autofree gchar *c3_badge = g_strdup_printf("FFMPEG:%s YT-DLP:%s",
                                                 ffmpeg_p ? "OK" : "NO",
                                                 ytdlp_p  ? "OK" : "NO");
    GtkWidget *c3 = create_section_card(SETTINGS_CARD_LEFT, "3. PRIVACY, AD BLOCKER & MEDIA DOWNLOADER",
                                        c3_badge, ytdlp_p ? "success" : "peak-warn", NULL);

    self->sw_adblock = make_switch(self);
    gtk_box_append(GTK_BOX(c3), create_boxed_row("🛡", "Native Ad & Tracker Blocker",
        "WebKitUserContentFilterStore compiled bytecode rules (zero JS overhead)", self->sw_adblock));

    self->entry_adblock_rules = make_entry(self, "~/.config/kajo/web/adblock-custom.json", FALSE);
    gtk_box_append(GTK_BOX(c3), create_boxed_row("📄", "Custom Filter JSON Path",
        "Optional Safari/WebKit JSON rulepack replacing built-in list", self->entry_adblock_rules));

    self->sw_userscripts = make_switch(self);
    gtk_box_append(GTK_BOX(c3), create_boxed_row("📜", "Custom User Scripts & CSS",
        "Load ~/.config/kajo/web/scripts/*.{js,css} into pages (opt-in)", self->sw_userscripts));

    self->sw_itp = make_switch(self);
    gtk_box_append(GTK_BOX(c3), create_boxed_row("🕵", "Intelligent Tracking Prevention (ITP)",
        "Partition third-party trackers per WebKitNetworkSession", self->sw_itp));

    self->seg_cookies = create_segmented_group(WEB_COOKIE_LABELS, 0);
    segmented_group_connect(self->seg_cookies, on_web_seg_changed, self);
    gtk_box_append(GTK_BOX(c3), create_boxed_row("🍪", "Cookie Policy", "Block third-party cookies by default", self->seg_cookies));

    self->sw_https_only = make_switch(self);
    gtk_box_append(GTK_BOX(c3), create_boxed_row("🔒", "HTTPS-Only Mode", "Upgrade http:// navigations to https:// automatically", self->sw_https_only));

    self->seg_autoplay = create_segmented_group(WEB_AUTOPLAY_LABELS, 0);
    segmented_group_connect(self->seg_autoplay, on_web_seg_changed, self);
    gtk_box_append(GTK_BOX(c3), create_boxed_row("▶", "Media Autoplay", "Allow muted video, block all, or allow with sound", self->seg_autoplay));

    self->sw_webrtc = make_switch(self);
    gtk_box_append(GTK_BOX(c3), create_boxed_row("📹", "Enable WebRTC", "Needed for Meet/Jitsi/Discord calls; disable to prevent IP leaks", self->sw_webrtc));

    self->sw_dns_prefetch = make_switch(self);
    gtk_box_append(GTK_BOX(c3), create_boxed_row("🌐", "DNS Prefetching", "Resolve hovered links ahead of navigation", self->sw_dns_prefetch));

    self->dd_dl_fmt = create_string_dropdown(WEB_DL_FMT_LABELS);
    settings_make_dropdown_shrinkable(self->dd_dl_fmt);
    g_signal_connect(self->dd_dl_fmt, "notify::selected", G_CALLBACK(on_web_prop_changed), self);
    gtk_box_append(GTK_BOX(c3), create_boxed_row("⬇", "Default yt-dlp Download Format",
        ytdlp_p ? "yt-dlp detected in PATH (Tier 3 active)" : "Requires yt-dlp in PATH (Tier 1 & Tier 2 ffmpeg still work)",
        self->dd_dl_fmt));

    /* ─── Card 4: Profiles & Google Drive AppData Sync ─── */
    GtkWidget *c4 = create_section_card(SETTINGS_CARD_RIGHT, "4. PROFILES & GOOGLE DRIVE APPDATA SYNC", "DRIVE.APPDATA", "accent", NULL);

    self->sw_picker_startup = make_switch(self);
    gtk_box_append(GTK_BOX(c4), create_boxed_row("⊞", "Show Profile Picker on Startup",
        "Open Chrome/Thorium-style profile grid when >1 profile exists", self->sw_picker_startup));

    self->sw_profile_border = make_switch(self);
    gtk_box_append(GTK_BOX(c4), create_boxed_row("▭", "Profile Window Accent Border",
        "Tint the 1px Metro window frame using each profile's accent color", self->sw_profile_border));

    self->entry_oauth_id = make_entry(self, "Google Desktop OAuth Client ID", FALSE);
    gtk_box_append(GTK_BOX(c4), create_boxed_row("🔑", "Google OAuth Client ID",
        "Free Desktop App client ID for drive.appdata scope (PKCE loopback)", self->entry_oauth_id));

    self->entry_oauth_secret = make_entry(self, "Optional Client Secret", TRUE);
    gtk_box_append(GTK_BOX(c4), create_boxed_row("🗝", "Google OAuth Client Secret",
        "Optional desktop client secret; tokens stored in libsecret keyring", self->entry_oauth_secret));

    self->sw_sync_bookmarks = make_switch(self);
    gtk_box_append(GTK_BOX(c4), create_boxed_row("★", "Sync Bookmarks", "CRDT UUID+tombstone merge via Drive appDataFolder", self->sw_sync_bookmarks));

    self->sw_sync_history = make_switch(self);
    gtk_box_append(GTK_BOX(c4), create_boxed_row("🕘", "Sync History", "Include browsing history in profile sync payload", self->sw_sync_history));

    self->sw_sync_tabs = make_switch(self);
    gtk_box_append(GTK_BOX(c4), create_boxed_row("📑", "Sync Open Tabs & Groups", "Sync active session tabs across devices", self->sw_sync_tabs));

    self->sw_sync_settings = make_switch(self);
    gtk_box_append(GTK_BOX(c4), create_boxed_row("⚙", "Sync Browser Settings", "Sync web.toml preferences snapshot", self->sw_sync_settings));

    self->sw_sync_e2ee = make_switch(self);
    gtk_box_append(GTK_BOX(c4), create_boxed_row("🛡", "End-to-End Encryption (AES-256-GCM)",
        "Encrypt Drive payloads locally with PBKDF2 passphrase before upload", self->sw_sync_e2ee));

    settings_bento_add_pair(GTK_WIDGET(self), c3, c4);

    /* ─── Card 5: Search, Zoom & User-Agent ─── */
    GtkWidget *c5 = create_section_card(SETTINGS_CARD_LEFT, "5. SEARCH, ZOOM & USER-AGENT", "OMNIBAR", NULL, NULL);

    self->dd_search = create_string_dropdown(WEB_SEARCH_LABELS);
    settings_make_dropdown_shrinkable(self->dd_search);
    g_signal_connect(self->dd_search, "notify::selected", G_CALLBACK(on_web_prop_changed), self);
    gtk_box_append(GTK_BOX(c5), create_boxed_row("🔍", "Default Search Engine", "Used by omnibar queries (!g, !ddg, !w, !gh bangs always work)", self->dd_search));

    self->entry_search_url = make_entry(self, "https://example.com/?q=%s", FALSE);
    gtk_box_append(GTK_BOX(c5), create_boxed_row("🔗", "Custom Search URL (%s)", "Query template when Search Engine is CUSTOM", self->entry_search_url));

    GtkWidget *zm_wrap = create_scale_with_badge(50, 200, 5, 100, "%.0f%%", &self->scale_zoom, &self->lbl_zoom);
    g_signal_connect(self->scale_zoom, "value-changed", G_CALLBACK(on_web_scale_changed), self);
    gtk_box_append(GTK_BOX(c5), create_boxed_row("🔎", "Default Page Zoom", "Applied to newly opened tabs (Ctrl +/- overrides per tab)", zm_wrap));

    self->seg_ua = create_segmented_group(WEB_UA_LABELS, 0);
    segmented_group_connect(self->seg_ua, on_web_seg_changed, self);
    gtk_box_append(GTK_BOX(c5), create_boxed_row("🪪", "User-Agent Preset", "WebKit default, Linux Desktop, or custom string", self->seg_ua));

    self->entry_ua_custom = make_entry(self, "Mozilla/5.0 ...", FALSE);
    gtk_box_append(GTK_BOX(c5), create_boxed_row("✎", "Custom User-Agent", "Used when User-Agent Preset is CUSTOM", self->entry_ua_custom));

    /* ─── Card 6: Appearance, Downloads & DevTools ─── */
    GtkWidget *c6 = create_section_card(SETTINGS_CARD_RIGHT, "6. APPEARANCE, DOWNLOADS & DEVTOOLS", "WEB.TOML", NULL, NULL);

    self->seg_scheme = create_segmented_group(WEB_SCHEME_LABELS, 0);
    segmented_group_connect(self->seg_scheme, on_web_seg_changed, self);
    gtk_box_append(GTK_BOX(c6), create_boxed_row("◐", "Preferred Web Color Scheme", "prefers-color-scheme hint for web pages", self->seg_scheme));

    GtkWidget *fp_wrap = create_scale_with_badge(10, 32, 1, 16, "%.0fPX", &self->scale_font_px, &self->lbl_font_px);
    g_signal_connect(self->scale_font_px, "value-changed", G_CALLBACK(on_web_scale_changed), self);
    gtk_box_append(GTK_BOX(c6), create_boxed_row("Aa", "Default Font Size", "Base proportional web font size", fp_wrap));

    GtkWidget *mp_wrap = create_scale_with_badge(10, 28, 1, 13, "%.0fPX", &self->scale_mono_px, &self->lbl_mono_px);
    g_signal_connect(self->scale_mono_px, "value-changed", G_CALLBACK(on_web_scale_changed), self);
    gtk_box_append(GTK_BOX(c6), create_boxed_row("<>", "Monospace Font Size", "Code block & <pre> font size", mp_wrap));

    GtkWidget *np_wrap = create_scale_with_badge(0, 24, 1, 9, "%.0fPX", &self->scale_min_px, &self->lbl_min_px);
    g_signal_connect(self->scale_min_px, "value-changed", G_CALLBACK(on_web_scale_changed), self);
    gtk_box_append(GTK_BOX(c6), create_boxed_row("aa", "Minimum Font Size", "Floor for tiny web text (0 disables)", np_wrap));

    self->entry_dl_dir = make_entry(self, "~/Downloads (empty = XDG)", FALSE);
    gtk_box_append(GTK_BOX(c6), create_boxed_row("📁", "Download Directory", "Target folder for direct, ffmpeg, and yt-dlp downloads", self->entry_dl_dir));

    self->sw_ask_dl = make_switch(self);
    gtk_box_append(GTK_BOX(c6), create_boxed_row("❓", "Ask Where to Save", "Prompt before saving direct browser downloads", self->sw_ask_dl));

    self->sw_devtools = make_switch(self);
    gtk_box_append(GTK_BOX(c6), create_boxed_row("🛠", "Enable Web Inspector (F12)", "Allow opening WebKitGTK Developer Extras", self->sw_devtools));

    settings_bento_add_pair(GTK_WIDGET(self), c5, c6);

    apply_cfg_to_ui(self, self->saved);

    GtkWidget *root = GTK_WIDGET(self);
    settings_page_bind_actions(root,
                               "↺ RESET WEB DEFAULTS",
                               action_reset_web_defaults,
                               action_revert_web,
                               action_save_web,
                               self);
    g_signal_connect(root, "map", G_CALLBACK(on_web_page_mapped), NULL);
    update_web_telemetry_and_dirty(self);

    return root;
}
