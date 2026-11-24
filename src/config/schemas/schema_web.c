#include "../kajo_config_schema.h"

static const KajoEnumMap k_web_rail_mode_map[] = {
    { "autohide", KAJO_WEB_RAIL_AUTOHIDE },
    { "compact",  KAJO_WEB_RAIL_COMPACT },
    { "pinned",   KAJO_WEB_RAIL_PINNED },
    { NULL, 0 }
};

static const KajoEnumMap k_web_rail_side_map[] = {
    { "left",  KAJO_WEB_SIDE_LEFT },
    { "right", KAJO_WEB_SIDE_RIGHT },
    { NULL, 0 }
};

static const KajoEnumMap k_web_topbar_map[] = {
    { "always",   KAJO_WEB_TOPBAR_ALWAYS },
    { "autohide", KAJO_WEB_TOPBAR_AUTOHIDE },
    { NULL, 0 }
};

static const KajoEnumMap k_web_newtab_map[] = {
    { "dashboard", KAJO_WEB_NEWTAB_DASHBOARD },
    { "blank",     KAJO_WEB_NEWTAB_BLANK },
    { "custom",    KAJO_WEB_NEWTAB_CUSTOM },
    { NULL, 0 }
};

static const KajoEnumMap k_web_hw_accel_map[] = {
    { "always",    KAJO_WEB_HW_ALWAYS },
    { "on-demand", KAJO_WEB_HW_ON_DEMAND },
    { "never",     KAJO_WEB_HW_NEVER },
    { NULL, 0 }
};

static const KajoEnumMap k_web_cookie_map[] = {
    { "no-third-party", KAJO_WEB_COOKIES_NO_THIRD_PARTY },
    { "always",         KAJO_WEB_COOKIES_ALWAYS },
    { "never",          KAJO_WEB_COOKIES_NEVER },
    { NULL, 0 }
};

static const KajoEnumMap k_web_autoplay_map[] = {
    { "allow-without-sound", KAJO_WEB_AUTOPLAY_MUTE },
    { "allow",               KAJO_WEB_AUTOPLAY_ALLOW },
    { "deny",                KAJO_WEB_AUTOPLAY_DENY },
    { NULL, 0 }
};

static const KajoEnumMap k_web_search_map[] = {
    { "duckduckgo", KAJO_WEB_SEARCH_DUCKDUCKGO },
    { "kagi",       KAJO_WEB_SEARCH_KAGI },
    { "startpage",  KAJO_WEB_SEARCH_STARTPAGE },
    { "brave",      KAJO_WEB_SEARCH_BRAVE },
    { "google",     KAJO_WEB_SEARCH_GOOGLE },
    { "custom",     KAJO_WEB_SEARCH_CUSTOM },
    { NULL, 0 }
};

static const KajoEnumMap k_web_ua_map[] = {
    { "default",      KAJO_WEB_UA_DEFAULT },
    { "safari",       KAJO_WEB_UA_SAFARI },
    { "chrome-linux", KAJO_WEB_UA_CHROME_LINUX },
    { "custom",       KAJO_WEB_UA_CUSTOM },
    { NULL, 0 }
};

static const KajoEnumMap k_web_scheme_map[] = {
    { "follow-kajo", KAJO_WEB_SCHEME_FOLLOW_KAJO },
    { "dark",        KAJO_WEB_SCHEME_DARK },
    { "light",       KAJO_WEB_SCHEME_LIGHT },
    { NULL, 0 }
};

static const KajoEnumMap k_web_dl_fmt_map[] = {
    { "best-mp4",   KAJO_WEB_DL_BEST_MP4 },
    { "1080p",      KAJO_WEB_DL_1080P },
    { "720p",       KAJO_WEB_DL_720P },
    { "audio-opus", KAJO_WEB_DL_AUDIO_OPUS },
    { "audio-mp3",  KAJO_WEB_DL_AUDIO_MP3 },
    { NULL, 0 }
};

static gint
normalize_suspend_idle(gint val, gint def_val G_GNUC_UNUSED)
{
    if (val <= 0)
        return 0;
    return CLAMP(val, 5, 240);
}

const gchar *
kajo_web_rail_mode_to_string(KajoWebRailMode mode)
{
    switch (mode) {
    case KAJO_WEB_RAIL_COMPACT: return "compact";
    case KAJO_WEB_RAIL_PINNED:  return "pinned";
    case KAJO_WEB_RAIL_AUTOHIDE:
    default:                    return "autohide";
    }
}

const gchar *
kajo_web_hw_accel_to_string(KajoWebHwAccel hw)
{
    switch (hw) {
    case KAJO_WEB_HW_ON_DEMAND: return "on-demand";
    case KAJO_WEB_HW_NEVER:     return "never";
    case KAJO_WEB_HW_ALWAYS:
    default:                    return "always";
    }
}

const gchar *
kajo_web_search_engine_to_string(KajoWebSearchEngine eng)
{
    switch (eng) {
    case KAJO_WEB_SEARCH_KAGI:       return "kagi";
    case KAJO_WEB_SEARCH_STARTPAGE:  return "startpage";
    case KAJO_WEB_SEARCH_BRAVE:      return "brave";
    case KAJO_WEB_SEARCH_GOOGLE:     return "google";
    case KAJO_WEB_SEARCH_CUSTOM:     return "custom";
    case KAJO_WEB_SEARCH_DUCKDUCKGO:
    default:                         return "duckduckgo";
    }
}

static const KajoFieldSpec k_web_fields[] = {
    { NULL, KAJO_CFG_KEY_WEB_RAIL_MODE,        KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, rail_mode),                 .def.e = KAJO_WEB_RAIL_AUTOHIDE, .enum_map = k_web_rail_mode_map },
    { NULL, KAJO_CFG_KEY_WEB_RAIL_SIDE,        KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, rail_side),                 .def.e = KAJO_WEB_SIDE_LEFT,     .enum_map = k_web_rail_side_map },
    { NULL, KAJO_CFG_KEY_WEB_RAIL_WIDTH_PX,    KAJO_FIELD_INT,    offsetof(KajoWebCfg, rail_width_px),             .def.i = 240,  .min_int = 180, .max_int = 360 },
    { NULL, KAJO_CFG_KEY_WEB_RAIL_REV_DELAY,   KAJO_FIELD_INT,    offsetof(KajoWebCfg, rail_reveal_delay_ms),      .def.i = 35,   .min_int = 0,   .max_int = 300 },
    { NULL, KAJO_CFG_KEY_WEB_RAIL_HIDE_DELAY,  KAJO_FIELD_INT,    offsetof(KajoWebCfg, rail_hide_delay_ms),        .def.i = 220,  .min_int = 50,  .max_int = 1000 },
    { NULL, KAJO_CFG_KEY_WEB_TOP_BAR_MODE,     KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, top_bar_mode),              .def.e = KAJO_WEB_TOPBAR_ALWAYS, .enum_map = k_web_topbar_map },
    { NULL, KAJO_CFG_KEY_WEB_SHOW_DOMAINS,     KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, show_tab_domains),          .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_NEW_TAB_PAGE,     KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, new_tab_page),              .def.e = KAJO_WEB_NEWTAB_DASHBOARD, .enum_map = k_web_newtab_map },
    { NULL, KAJO_CFG_KEY_WEB_NEW_TAB_URL,      KAJO_FIELD_STRING, offsetof(KajoWebCfg, new_tab_custom_url),        .def.s = "" },
    { NULL, KAJO_CFG_KEY_WEB_RESTORE_SESSION,  KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, restore_session),           .def.b = TRUE },

    { NULL, KAJO_CFG_KEY_WEB_HW_ACCEL,         KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, hardware_acceleration),     .def.e = KAJO_WEB_HW_ALWAYS,     .enum_map = k_web_hw_accel_map },
    { NULL, KAJO_CFG_KEY_WEB_ENABLE_WEBGL,     KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, enable_webgl),              .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_GPU_PROC_WEBGL,   KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, gpu_process_webgl),         .def.b = FALSE },
    { NULL, KAJO_CFG_KEY_WEB_SMOOTH_SCROLL,    KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, smooth_scrolling),          .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_SUSPEND_IDLE_MIN, KAJO_FIELD_INT,    offsetof(KajoWebCfg, suspend_idle_min),          .def.i = 30,   .normalize_int = normalize_suspend_idle },
    { NULL, KAJO_CFG_KEY_WEB_MEMORY_LIMIT_MB,  KAJO_FIELD_INT,    offsetof(KajoWebCfg, memory_limit_mb),           .def.i = 2048, .min_int = 512, .max_int = 16384 },

    { NULL, KAJO_CFG_KEY_WEB_ADBLOCK_ENABLED,  KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, adblock_enabled),           .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_ADBLOCK_SYNC,     KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, adblock_auto_sync),         .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_ADBLOCK_RULES,    KAJO_FIELD_STRING, offsetof(KajoWebCfg, adblock_custom_rules_path), .def.s = "" },
    { NULL, KAJO_CFG_KEY_WEB_USER_SCRIPTS,     KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, user_scripts_enabled),      .def.b = FALSE },
    { NULL, KAJO_CFG_KEY_WEB_ITP_ENABLED,      KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, itp_enabled),               .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_COOKIE_POLICY,    KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, cookie_policy),             .def.e = KAJO_WEB_COOKIES_NO_THIRD_PARTY, .enum_map = k_web_cookie_map },
    { NULL, KAJO_CFG_KEY_WEB_HTTPS_ONLY,       KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, https_only),                .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_AUTOPLAY_POLICY,  KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, autoplay_policy),           .def.e = KAJO_WEB_AUTOPLAY_ALLOW, .enum_map = k_web_autoplay_map },
    { NULL, KAJO_CFG_KEY_WEB_ENABLE_WEBRTC,    KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, enable_webrtc),             .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_DNS_PREFETCH,     KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, dns_prefetch),              .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_MEDIA_DL_FMT,     KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, media_download_format),     .def.e = KAJO_WEB_DL_BEST_MP4,   .enum_map = k_web_dl_fmt_map },

    { NULL, KAJO_CFG_KEY_WEB_SEARCH_ENGINE,    KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, search_engine),             .def.e = KAJO_WEB_SEARCH_DUCKDUCKGO, .enum_map = k_web_search_map },
    { NULL, KAJO_CFG_KEY_WEB_SEARCH_CUSTOM,    KAJO_FIELD_STRING, offsetof(KajoWebCfg, search_custom_url),         .def.s = "" },
    { NULL, KAJO_CFG_KEY_WEB_DEFAULT_ZOOM,     KAJO_FIELD_INT,    offsetof(KajoWebCfg, default_zoom_pct),          .def.i = 100,  .min_int = 50,  .max_int = 200 },
    { NULL, KAJO_CFG_KEY_WEB_UA_PRESET,        KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, user_agent_preset),         .def.e = KAJO_WEB_UA_DEFAULT,    .enum_map = k_web_ua_map },
    { NULL, KAJO_CFG_KEY_WEB_UA_CUSTOM,        KAJO_FIELD_STRING, offsetof(KajoWebCfg, user_agent_custom),         .def.s = "" },

    { NULL, KAJO_CFG_KEY_WEB_COLOR_SCHEME,     KAJO_FIELD_ENUM,   offsetof(KajoWebCfg, color_scheme),              .def.e = KAJO_WEB_SCHEME_FOLLOW_KAJO, .enum_map = k_web_scheme_map },
    { NULL, KAJO_CFG_KEY_WEB_FONT_SIZE,        KAJO_FIELD_INT,    offsetof(KajoWebCfg, default_font_size_px),      .def.i = 16,   .min_int = 10,  .max_int = 32 },
    { NULL, KAJO_CFG_KEY_WEB_MONO_FONT_SIZE,   KAJO_FIELD_INT,    offsetof(KajoWebCfg, mono_font_size_px),         .def.i = 13,   .min_int = 10,  .max_int = 28 },
    { NULL, KAJO_CFG_KEY_WEB_MIN_FONT_SIZE,    KAJO_FIELD_INT,    offsetof(KajoWebCfg, min_font_size_px),          .def.i = 9,    .min_int = 0,   .max_int = 24 },
    { NULL, KAJO_CFG_KEY_WEB_DOWNLOAD_DIR,     KAJO_FIELD_STRING, offsetof(KajoWebCfg, download_dir),              .def.s = "" },
    { NULL, KAJO_CFG_KEY_WEB_ASK_DOWNLOAD,     KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, ask_download_location),     .def.b = FALSE },
    { NULL, KAJO_CFG_KEY_WEB_DL_AUTO_OPEN,     KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, downloads_auto_open),       .def.b = FALSE },
    { NULL, KAJO_CFG_KEY_WEB_DEV_EXTRAS,       KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, developer_extras),          .def.b = TRUE },

    { NULL, KAJO_CFG_KEY_WEB_SHOW_PICKER,      KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, show_picker_on_startup),    .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_PROFILE_BORDER,   KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, profile_border_accent),     .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_SYNC_CLIENT_ID,   KAJO_FIELD_STRING, offsetof(KajoWebCfg, sync_oauth_client_id),      .def.s = "" },
    { NULL, KAJO_CFG_KEY_WEB_SYNC_CLIENT_SEC,  KAJO_FIELD_STRING, offsetof(KajoWebCfg, sync_oauth_client_secret),  .def.s = "" },
    { NULL, KAJO_CFG_KEY_WEB_SYNC_BOOKMARKS,   KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, sync_bookmarks),            .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_SYNC_HISTORY,     KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, sync_history),              .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_SYNC_TABS,        KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, sync_tabs_and_groups),      .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_SYNC_SETTINGS,    KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, sync_settings),             .def.b = TRUE },
    { NULL, KAJO_CFG_KEY_WEB_SYNC_E2EE,        KAJO_FIELD_BOOL,   offsetof(KajoWebCfg, sync_e2ee_enabled),         .def.b = FALSE },
};

static const KajoDomainSchema k_web_schema = {
    .domain            = KAJO_CONFIG_DOMAIN_WEB,
    .filename          = KAJO_CFG_FILE_WEB,
    .domain_table_name = KAJO_CFG_DOMAIN_WEB,
    .struct_size       = sizeof(KajoWebCfg),
    .fields            = k_web_fields,
    .n_fields          = G_N_ELEMENTS(k_web_fields),
};

const KajoDomainSchema *
kajo_schema_get_web(void)
{
    return &k_web_schema;
}
