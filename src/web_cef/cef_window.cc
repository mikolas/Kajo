#include "web_cef/cef_window.h"

#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>

#include "common/spawn_detached.h"
#include "compositor/compositor.h"
#include "config/kajo_config_store.h"
#include "include/cef_app.h"
#include "include/cef_request.h"
#include "include/cef_task.h"
#include "include/cef_urlrequest.h"
#include "include/wrapper/cef_helpers.h"
#include "web/web_sync_merge.h"
#include "web_cef/cef_site_util.h"

namespace kajo_web {

namespace {

enum ChromeRole {
  ROLE_TOPBAR = 1,
  ROLE_RAIL = 2,
  ROLE_OVERLAY = 3,
  ROLE_AC = 4,
  ROLE_INFOBAR = 5,
};

enum WindowAccelId {
  ACCEL_NEW_TAB = 1001,
  ACCEL_CLOSE_TAB,
  ACCEL_REOPEN_TAB,
  ACCEL_FOCUS_OMNIBAR,
  ACCEL_PALETTE,
  ACCEL_FIND,
  ACCEL_RELOAD,
  ACCEL_HARD_RELOAD,
  ACCEL_TOGGLE_RAIL,
  ACCEL_TOGGLE_TOPBAR,
  ACCEL_BOOKMARK,
  ACCEL_MUTE_TAB,
  ACCEL_DOWNLOADS,
  ACCEL_PROFILES,
  ACCEL_PRIVATE_WINDOW,
  ACCEL_SETTINGS,
  ACCEL_FULLSCREEN,
  ACCEL_DEVTOOLS,
};

std::string ToLowerCopy(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool ContainsInsensitive(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return true;
  return ToLowerCopy(hay).find(ToLowerCopy(needle)) != std::string::npos;
}

std::string GetQueryParam(const std::string& query, const std::string& key) {
  if (query.empty()) return "";
  std::string k = key + "=";
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    std::string pair = (amp == std::string::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
    if (pair.rfind(k, 0) == 0) {
      std::string val = pair.substr(k.size());
      for (char& c : val) {
        if (c == '+') c = ' ';
      }
      g_autofree gchar* unescaped = g_uri_unescape_string(val.c_str(), nullptr);
      return unescaped ? std::string(unescaped) : val;
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return "";
}

class UiTask : public CefTask {
public:
  explicit UiTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override {
    if (fn_) fn_();
  }

private:
  std::function<void()> fn_;
  IMPLEMENT_REFCOUNTING(UiTask);
};

/* Hidden 1x1 Alloy window used exclusively to host a detached tab CefBrowserView
 * long enough for CloseBrowser(true) -> DoClose(false) -> CloseHostWindow() ->
 * OnWindowDestroyed() -> DestroyBrowser() -> OnBeforeClose() to terminate the
 * Chromium renderer process without closing the main browser CefWindow. */
class TabReaperWindowDelegate : public CefWindowDelegate {
public:
  TabReaperWindowDelegate(CefRefPtr<CefBrowserView> view, CefRefPtr<CefBrowser> browser)
      : view_(std::move(view)), browser_(std::move(browser)) {}

  cef_runtime_style_t GetWindowRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }
  bool IsFrameless(CefRefPtr<CefWindow> window) override { return true; }
  CefRect GetInitialBounds(CefRefPtr<CefWindow> window) override { return CefRect(0, 0, 1, 1); }
  bool CanClose(CefRefPtr<CefWindow> window) override { return true; }

  void OnWindowCreated(CefRefPtr<CefWindow> window) override {
    win_ = window;
    if (view_) {
      window->AddChildView(view_);
    }
    CefRefPtr<CefBrowser> b = browser_;
    if (!b && view_) b = view_->GetBrowser();
    if (b && b->GetHost()) {
      b->GetHost()->CloseBrowser(true);
      CefRefPtr<TabReaperWindowDelegate> self(this);
      CefPostDelayedTask(TID_UI, new UiTask([self]() {
        if (self->win_ && !self->win_->IsClosed()) {
          self->win_->Close();
        }
      }), 1500);
    } else {
      window->Close();
    }
  }

  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
    view_ = nullptr;
    browser_ = nullptr;
    win_ = nullptr;
  }

private:
  CefRefPtr<CefBrowserView> view_;
  CefRefPtr<CefBrowser> browser_;
  CefRefPtr<CefWindow> win_;
  IMPLEMENT_REFCOUNTING(TabReaperWindowDelegate);
};

class ChromeViewClient : public CefClient,
                         public CefLifeSpanHandler,
                         public CefDragHandler,
                         public CefKeyboardHandler,
                         public CefRequestHandler {
public:
  ChromeViewClient(KajoBrowserWindow* owner, ChromeRole role)
      : owner_(owner), role_(role) {}

  void Detach() { owner_ = nullptr; }

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDragHandler> GetDragHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    if (owner_) owner_->OnChromeBrowserCreated(role_, browser);
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    if (owner_ && owner_->router()) owner_->router()->OnBeforeClose(browser);
  }

  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                 TerminationStatus status,
                                 int error_code,
                                 const CefString& error_string) override {
    if (owner_ && owner_->router()) owner_->router()->OnRenderProcessTerminated(browser);
  }

  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override {
    if (owner_ && owner_->router()) owner_->router()->OnBeforeBrowse(browser, frame);
    return false;
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    if (owner_ && owner_->router()) {
      return owner_->router()->OnProcessMessageReceived(browser, frame, source_process, message);
    }
    return false;
  }

  void OnDraggableRegionsChanged(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 const std::vector<CefDraggableRegion>& regions) override {
    if (owner_ && role_ == ROLE_TOPBAR) {
      owner_->OnChromeDraggableRegions(regions);
    }
  }

  bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                     const CefKeyEvent& event,
                     CefEventHandle os_event,
                     bool* is_keyboard_shortcut) override {
    if (owner_ && owner_->OnChromeShortcut(event)) {
      return true;
    }
    return false;
  }

private:
  KajoBrowserWindow* owner_;
  ChromeRole role_;
  IMPLEMENT_REFCOUNTING(ChromeViewClient);
};

class KajoBrowserViewDelegate : public CefBrowserViewDelegate {
public:
  explicit KajoBrowserViewDelegate(KajoBrowserWindow* owner) : owner_(owner) {}

  void Detach() { owner_ = nullptr; }

  cef_runtime_style_t GetBrowserRuntimeStyle() override {
    return CEF_RUNTIME_STYLE_ALLOY;
  }

  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    if (owner_) return owner_->GetPreferredChromeViewSize(view);
    return CefSize();
  }

  bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView> browser_view,
                                 CefRefPtr<CefBrowserView> popup_browser_view,
                                 bool is_devtools) override {
    /* Let DevTools or explicit opener popups open in their own window */
    return false;
  }

private:
  KajoBrowserWindow* owner_;
  IMPLEMENT_REFCOUNTING(KajoBrowserViewDelegate);
};

class KajoPanelDelegate : public CefPanelDelegate {
public:
  explicit KajoPanelDelegate(KajoBrowserWindow* owner) : owner_(owner) {}

  void Detach() { owner_ = nullptr; }

  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    if (owner_) return owner_->GetPreferredChromeViewSize(view);
    return CefSize();
  }

private:
  KajoBrowserWindow* owner_;
  IMPLEMENT_REFCOUNTING(KajoPanelDelegate);
};

class FilterDownloadClient : public CefURLRequestClient {
public:
  FilterDownloadClient(CefRefPtr<KajoBrowserWindow> win, std::string id)
      : win_(std::move(win)), id_(std::move(id)) {}

  void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
    const int status = (request && request->GetResponse())
                           ? request->GetResponse()->GetStatus()
                           : 0;
    const bool ok = request && request->GetRequestStatus() == UR_SUCCESS && status == 200;
    win_->HandleFilterListDownloadComplete(id_, ok, status, body_);
  }

  void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
  void OnDownloadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
  void OnDownloadData(CefRefPtr<CefURLRequest>, const void* data, size_t len) override {
    if (data && len > 0 && body_.size() < 32U * 1024U * 1024U) {
      body_.append(static_cast<const char*>(data), len);
    }
  }
  bool GetAuthCredentials(bool, const CefString&, int, const CefString&,
                          const CefString&, CefRefPtr<CefAuthCallback>) override {
    return false;
  }

private:
  CefRefPtr<KajoBrowserWindow> win_;
  std::string id_;
  std::string body_;
  IMPLEMENT_REFCOUNTING(FilterDownloadClient);
};

}  // namespace

KajoBrowserWindow::KajoBrowserWindow(const LaunchConfig& cfg,
                                     std::shared_ptr<CefAdblockEngine> adblock,
                                     std::shared_ptr<ChromeStateStore> store,
                                     CefRefPtr<CefRequestContext> request_context)
    : cfg_(cfg),
      adblock_(std::move(adblock)),
      store_(std::move(store)),
      request_context_(std::move(request_context)) {
  CefMessageRouterConfig rc;
  router_ = CefMessageRouterBrowserSide::Create(rc);
  router_->AddHandler(this, true);
  InitBackend();
}

KajoBrowserWindow::~KajoBrowserWindow() {
  if (ipc_server_) {
    ipc_server_->Stop();
    ipc_server_.reset();
  }
  ++push_gen_;
  ++rail_hover_gen_;
  ++topbar_hover_gen_;
  ++suspend_timer_gen_;
  for (auto& req : pending_permissions_) {
    ResolvePermissionReq(req, false, false);
  }
  pending_permissions_.clear();
  if (model_) web_model_free(model_);
  if (storage_) web_storage_free(storage_);
  if (profiles_) web_profile_registry_free(profiles_);
}

void KajoBrowserWindow::InitBackend() {
  model_ = web_model_new();
  storage_ = web_storage_new(cfg_.data_dir.c_str());

  g_autofree gchar* perm_p = g_build_filename(cfg_.data_dir.c_str(), "permissions.json", nullptr);
  if (perm_p) {
    permission_store_path_ = perm_p;
    if (!cfg_.private_mode) permission_store_.Load(permission_store_path_);
  }

  profiles_ = web_profile_registry_new();
  g_autofree gchar* reg_path = g_build_filename(
      g_get_user_data_dir(), "kajo", "web", "profiles.toml", nullptr);
  web_profile_registry_load(profiles_, reg_path);

  if (KajoWebProfileInfo* p = web_profile_registry_find(profiles_, cfg_.profile_id.c_str())) {
    profile_name_ = p->name ? p->name : "DEFAULT";
    profile_icon_ = p->icon ? p->icon : "🌐";
    profile_accent_ = SanitizeHexColor(p->accent_color ? p->accent_color : "#0078d4", "#0078d4");
  }

  ReloadThemeAndConfig();
  ApplyColorSchemeToContext();
  ApplyWebRtcPolicyToContext();
  ReloadUserScripts();
  if (adblock_ && !adblock_custom_rules_path_.empty()) {
    adblock_->ReloadFromDisk(adblock_custom_rules_path_);
  }
  if (!cfg_.private_mode && storage_) {
    if (GPtrArray* saved_dls = web_storage_list_downloads(storage_, 100)) {
      for (guint i = 0; i < saved_dls->len; ++i) {
        const auto* r = static_cast<const KajoWebDownloadRecord*>(g_ptr_array_index(saved_dls, i));
        ChromeDownloadView dv;
        dv.id = 0;
        dv.db_id = r->id;
        dv.url = r->url ? r->url : "";
        dv.filename = r->filename ? r->filename : "";
        dv.full_path = r->full_path ? r->full_path : "";
        dv.received_bytes = r->received_bytes;
        dv.total_bytes = r->total_bytes;
        dv.updated_at_ms = r->updated_at_ms;
        const std::string st = r->state ? r->state : "complete";
        dv.complete = (st == "complete");
        dv.canceled = (st == "canceled");
        dv.percent = dv.complete
                         ? 100
                         : (dv.total_bytes > 0
                                ? static_cast<int>((dv.received_bytes * 100) / dv.total_bytes)
                                : 0);
        downloads_.push_back(std::move(dv));
      }
      g_ptr_array_unref(saved_dls);
    }
  }
  RefreshStorageCache();
}

void KajoBrowserWindow::OnConfigStoreChanged() {
  if (closing_) return;
  const std::string prev_custom_rules = adblock_custom_rules_path_;
  const bool prev_us_enabled = user_scripts_enabled_;
  ReloadThemeAndConfig();
  ApplyColorSchemeToContext();
  ApplyWebRtcPolicyToContext();
  UpdateWindowBorderColor();
  if (user_scripts_enabled_ != prev_us_enabled || (user_scripts_enabled_ && !user_scripts_)) {
    ReloadUserScripts();
  }
  if (adblock_ && adblock_custom_rules_path_ != prev_custom_rules) {
    CefRefPtr<KajoBrowserWindow> self(this);
    std::shared_ptr<CefAdblockEngine> eng = adblock_;
    std::string custom_path = adblock_custom_rules_path_;
    CefPostTask(TID_FILE_USER_VISIBLE, new UiTask([self, eng, custom_path]() {
      eng->ReloadFromDisk(custom_path);
      CefPostTask(TID_UI, new UiTask([self]() {
        if (!self->closing_) self->PushStateImmediate();
      }));
    }));
  }
  for (auto& kv : tabs_) {
    if (kv.second.client) {
      kv.second.client->SetHttpsOnly(https_only_);
      kv.second.client->SetCookiePolicy(cookie_policy_);
      kv.second.client->SetDevToolsEnabled(developer_extras_);
      kv.second.client->SetUserScripts(user_scripts_enabled_, user_scripts_);
    }
  }
  RebuildLayout();
  PushStateImmediate();
}

void KajoBrowserWindow::RefreshStorageCache() {
  cached_bookmarks_.clear();
  cached_recent_history_.clear();
  if (!storage_) return;
  if (GPtrArray* bms = web_storage_list_bookmarks(storage_, FALSE)) {
    for (guint i = 0; i < bms->len; ++i) {
      const auto* b = static_cast<const KajoWebBookmark*>(g_ptr_array_index(bms, i));
      cached_bookmarks_.push_back({b->uuid ? b->uuid : "", b->title ? b->title : "",
                                   b->uri ? b->uri : "", b->folder ? b->folder : ""});
    }
    g_ptr_array_unref(bms);
  }
  if (GPtrArray* hist = web_storage_search_history(storage_, "", 25)) {
    for (guint i = 0; i < hist->len; ++i) {
      const auto* h = static_cast<const KajoWebHistoryEntry*>(g_ptr_array_index(hist, i));
      cached_recent_history_.push_back({h->title ? h->title : "", h->uri ? h->uri : "",
                                        h->visit_count, h->last_visit_ms});
    }
    g_ptr_array_unref(hist);
  }
}

CefBrowserSettings KajoBrowserWindow::MakeContentBrowserSettings() const {
  CefBrowserSettings bs;
  bs.background_color = CefColorSetARGB(255, 0x14, 0x14, 0x14);
  bs.default_font_size = default_font_size_px_;
  bs.default_fixed_font_size = mono_font_size_px_;
  bs.minimum_font_size = min_font_size_px_;
  bs.webgl = enable_webgl_ ? STATE_ENABLED : STATE_DISABLED;
  return bs;
}

void KajoBrowserWindow::ReloadThemeAndConfig() {
  KajoConfigStore* cs = kajo_config_store_get_default();
  if (!cs) return;

  if (const KajoThemeCfg* t = kajo_config_store_acquire_theme(cs)) {
    accent_color_ = SanitizeHexColor(t->accent_color ? t->accent_color : "#0078d4", "#0078d4");
    ui_font_family_ = t->ui_font_family ? t->ui_font_family : "Fira Code";
    ui_font_size_px_ = CLAMP(t->ui_font_size_px, 11, 20);
    heading_size_px_ = CLAMP(t->font_heading_size_px, 10, 18);
    sub_size_px_ = CLAMP(t->font_subtitle_size_px, 10, 18);
    badge_size_px_ = CLAMP(t->font_badge_size_px, 9, 16);
    const cef_color_t surf = ParseHexColorArgb(
        t->surface_base ? t->surface_base : "#141414",
        CefColorSetARGB(255, 0x14, 0x14, 0x14));
    const double lum = (0.2126 * CefColorGetR(surf) +
                        0.7152 * CefColorGetG(surf) +
                        0.0722 * CefColorGetB(surf)) / 255.0;
    kajo_theme_is_light_ = (lum > 0.5);
    kajo_theme_cfg_unref(t);
  }

  if (const KajoWebCfg* w = kajo_config_store_acquire_web(cs)) {
    rail_mode_ = w->rail_mode;
    rail_side_ = w->rail_side;
    rail_width_px_ = CLAMP(w->rail_width_px, 180, 360);
    rail_reveal_delay_ms_ = CLAMP(w->rail_reveal_delay_ms, 0, 300);
    rail_hide_delay_ms_ = CLAMP(w->rail_hide_delay_ms, 50, 1000);
    topbar_mode_ = w->top_bar_mode;
    profile_border_accent_ = (w->profile_border_accent != FALSE);
    show_tab_domains_ = w->show_tab_domains;
    newtab_mode_ = w->new_tab_page;
    newtab_custom_url_ = w->new_tab_custom_url ? w->new_tab_custom_url : "";
    restore_session_ = w->restore_session;
    search_engine_ = w->search_engine;
    search_custom_url_ = w->search_custom_url ? w->search_custom_url : "";
    default_zoom_pct_ = CLAMP(w->default_zoom_pct, 50, 200);
    default_font_size_px_ = CLAMP(w->default_font_size_px, 10, 32);
    mono_font_size_px_ = CLAMP(w->mono_font_size_px, 10, 28);
    min_font_size_px_ = CLAMP(w->min_font_size_px, 0, 24);
    suspend_idle_min_ = (w->suspend_idle_min <= 0) ? 0 : CLAMP(w->suspend_idle_min, 5, 240);
    color_scheme_ = w->color_scheme;
    cookie_policy_ = w->cookie_policy;
    user_scripts_enabled_ = (w->user_scripts_enabled != FALSE);
    enable_webgl_ = (w->enable_webgl != FALSE);
    enable_webrtc_ = (w->enable_webrtc != FALSE);
    https_only_ = (w->https_only != FALSE);
    ask_download_location_ = (w->ask_download_location != FALSE);
    downloads_auto_open_ = (w->downloads_auto_open != FALSE);
    developer_extras_ = (w->developer_extras != FALSE);
    show_picker_on_startup_ = (w->show_picker_on_startup != FALSE);
    adblock_auto_sync_ = (w->adblock_auto_sync != FALSE);
    const std::string custom_dl = w->download_dir ? ExpandTildePath(w->download_dir) : "";
    if (!custom_dl.empty()) {
      download_dir_ = custom_dl;
    } else if (const gchar* dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD)) {
      download_dir_ = dl;
    } else {
      download_dir_ = g_get_home_dir();
    }
    if (adblock_) {
      adblock_->SetEnabled(w->adblock_enabled);
      adblock_custom_rules_path_ =
          w->adblock_custom_rules_path ? ExpandTildePath(w->adblock_custom_rules_path) : "";
    }
    kajo_web_cfg_unref(w);
  }
}

void KajoBrowserWindow::PersistRailMode(int mode) {
  if (auto* cs = kajo_config_store_get_default()) {
    kajo_config_store_update_web(
        cs,
        [](KajoWebCfg* draft, gpointer ud) {
          draft->rail_mode = static_cast<KajoWebRailMode>(GPOINTER_TO_INT(ud));
        },
        GINT_TO_POINTER(mode),
        nullptr);
  }
}

void KajoBrowserWindow::PersistTopbarMode(int mode) {
  if (auto* cs = kajo_config_store_get_default()) {
    kajo_config_store_update_web(
        cs,
        [](KajoWebCfg* draft, gpointer ud) {
          draft->top_bar_mode = static_cast<KajoWebTopBarMode>(GPOINTER_TO_INT(ud));
        },
        GINT_TO_POINTER(mode),
        nullptr);
  }
}

void KajoBrowserWindow::UpdateWindowBorderColor() {
  if (!window_) return;
  if (IsFullscreen()) {
    window_->SetBackgroundColor(CefColorSetARGB(255, 0, 0, 0));
    return;
  }
  if (profile_border_accent_) {
    const std::string& hex = cfg_.private_mode ? std::string("#b146c2")
                                               : (!profile_accent_.empty() ? profile_accent_ : accent_color_);
    window_->SetBackgroundColor(ParseHexColorArgb(hex, CefColorSetARGB(255, 0x00, 0x78, 0xd4)));
  } else {
    window_->SetBackgroundColor(CefColorSetARGB(255, 0x2b, 0x2b, 0x2b));
  }
}

void KajoBrowserWindow::ApplyColorSchemeToContext() {
  if (!request_context_) return;
  cef_color_variant_t variant = CEF_COLOR_VARIANT_DARK;
  if (color_scheme_ == KAJO_WEB_SCHEME_LIGHT ||
      (color_scheme_ == KAJO_WEB_SCHEME_FOLLOW_KAJO && kajo_theme_is_light_)) {
    variant = CEF_COLOR_VARIANT_LIGHT;
  }
  const cef_color_t tint = ParseHexColorArgb(
      cfg_.private_mode ? "#b146c2" : profile_accent_,
      CefColorSetARGB(255, 0x00, 0x78, 0xd4));
  request_context_->SetChromeColorScheme(variant, tint);
}

void KajoBrowserWindow::ApplyWebRtcPolicyToContext() {
  if (!request_context_) return;
  const char* policy = enable_webrtc_ ? "default" : "disable_non_proxied_udp";
  if (request_context_->CanSetPreference("webrtc.ip_handling_policy")) {
    CefRefPtr<CefValue> val = CefValue::Create();
    val->SetString(policy);
    CefString err;
    if (!request_context_->SetPreference("webrtc.ip_handling_policy", val, err)) {
      g_warning("kajo-web: failed to set webrtc.ip_handling_policy: %s",
                err.ToString().c_str());
    }
  }
}

void KajoBrowserWindow::ReloadUserScripts() {
  if (!user_scripts_enabled_) {
    user_scripts_.reset();
    return;
  }
  const gchar* cfg_dir = nullptr;
  if (auto* cs = kajo_config_store_get_default()) {
    cfg_dir = kajo_config_store_get_config_dir(cs);
  }
  g_autofree gchar* fallback_dir = g_build_filename(g_get_user_config_dir(), "kajo", nullptr);
  const gchar* base = cfg_dir ? cfg_dir : fallback_dir;
  g_autofree gchar* scripts_dir = g_build_filename(base, "web", "scripts", nullptr);
  LoadedUserScripts loaded = LoadUserScriptsFromDir(scripts_dir ? scripts_dir : "");
  user_scripts_ = std::make_shared<LoadedUserScripts>(std::move(loaded));
}

std::string KajoBrowserWindow::DefaultNewTabUrl() const {
  if (newtab_mode_ == KAJO_WEB_NEWTAB_BLANK) return "about:blank";
  if (newtab_mode_ == KAJO_WEB_NEWTAB_CUSTOM && !newtab_custom_url_.empty()) {
    return newtab_custom_url_;
  }
  return "kajo://newtab";
}

void KajoBrowserWindow::ShowWindow() {
  CEF_REQUIRE_UI_THREAD();
  if (config_sub_id_ == 0) {
    if (KajoConfigStore* cs = kajo_config_store_get_default()) {
      CefRefPtr<KajoBrowserWindow> self(this);
      auto* weak_holder = new CefRefPtr<KajoBrowserWindow>(self);
      config_sub_id_ = kajo_config_store_subscribe(
          cs,
          static_cast<KajoConfigDomain>(KAJO_CONFIG_DOMAIN_WEB | KAJO_CONFIG_DOMAIN_THEME),
          [](KajoConfigStore*, KajoConfigDomain, gpointer ud) {
            auto* ref = static_cast<CefRefPtr<KajoBrowserWindow>*>(ud);
            if (!ref || !(*ref)) return;
            CefRefPtr<KajoBrowserWindow> w = *ref;
            CefPostTask(TID_UI, new UiTask([w]() { w->OnConfigStoreChanged(); }));
          },
          weak_holder,
          [](gpointer ud) {
            delete static_cast<CefRefPtr<KajoBrowserWindow>*>(ud);
          });
    }
  }
  PushStateImmediate();
  window_ = CefWindow::CreateTopLevelWindow(this);
  if (!window_ && config_sub_id_) {
    if (auto* cs = kajo_config_store_get_default()) {
      kajo_config_store_unsubscribe(cs, config_sub_id_);
    }
    config_sub_id_ = 0;
  }
}

void KajoBrowserWindow::OnWindowCreated(CefRefPtr<CefWindow> window) {
  window_ = window;
  window_->SetTitle(cfg_.private_mode ? "Kajo Web (Private)" : "Kajo Web");
  UpdateWindowBorderColor();

  bv_delegate_ = new KajoBrowserViewDelegate(this);
  panel_delegate_ = new KajoPanelDelegate(this);
  CefBrowserSettings bs;
  bs.background_color = CefColorSetARGB(255, 0x14, 0x14, 0x14);
  CefBrowserSettings trans_bs;
  trans_bs.background_color = CefColorSetARGB(0, 0, 0, 0);

  CefRefPtr<CefClient> top_c(new ChromeViewClient(this, ROLE_TOPBAR));
  CefRefPtr<CefClient> rail_c(new ChromeViewClient(this, ROLE_RAIL));
  CefRefPtr<CefClient> inf_c(new ChromeViewClient(this, ROLE_INFOBAR));
  CefRefPtr<CefClient> ov_c(new ChromeViewClient(this, ROLE_OVERLAY));
  CefRefPtr<CefClient> ac_c(new ChromeViewClient(this, ROLE_AC));
  chrome_clients_ = {top_c, rail_c, inf_c, ov_c, ac_c};

  topbar_view_ = CefBrowserView::CreateBrowserView(
      top_c, "kajo-chrome://ui/topbar.html", bs, nullptr, request_context_, bv_delegate_);
  rail_view_ = CefBrowserView::CreateBrowserView(
      rail_c, "kajo-chrome://ui/rail.html", trans_bs, nullptr, request_context_, bv_delegate_);
  infobar_view_ = CefBrowserView::CreateBrowserView(
      inf_c, "kajo-chrome://ui/overlay.html?role=perm", bs, nullptr, request_context_, bv_delegate_);
  overlay_view_ = CefBrowserView::CreateBrowserView(
      ov_c, "kajo-chrome://ui/overlay.html", bs, nullptr, request_context_, bv_delegate_);
  ac_view_ = CefBrowserView::CreateBrowserView(
      ac_c, "kajo-chrome://ui/overlay.html?role=ac", bs, nullptr, request_context_, bv_delegate_);

  body_panel_ = CefPanel::CreatePanel(nullptr);
  rail_spacer_ = CefPanel::CreatePanel(panel_delegate_);
  rail_spacer_->SetBackgroundColor(CefColorSetARGB(255, 0x11, 0x11, 0x11));
  content_panel_ = CefPanel::CreatePanel(nullptr);
  content_panel_->SetToFillLayout();
  content_panel_->SetBackgroundColor(CefColorSetARGB(255, 0x14, 0x14, 0x14));

  RebuildLayout();
  rail_ctrl_ = window_->AddOverlayView(rail_view_, CEF_DOCKING_MODE_CUSTOM, false);
  infobar_ctrl_ = window_->AddOverlayView(infobar_view_, CEF_DOCKING_MODE_CUSTOM, false);
  if (infobar_ctrl_) infobar_ctrl_->SetVisible(false);
  ac_ctrl_ = window_->AddOverlayView(ac_view_, CEF_DOCKING_MODE_CUSTOM, false);
  if (ac_ctrl_) ac_ctrl_->SetVisible(false);
  overlay_ctrl_ = window_->AddOverlayView(overlay_view_, CEF_DOCKING_MODE_CUSTOM, true);
  if (overlay_ctrl_) overlay_ctrl_->SetVisible(false);

  /* Register top-level window accelerators for chrome & content focus independence */
  window_->SetAccelerator(ACCEL_NEW_TAB, 'T', false, true, false, false);
  window_->SetAccelerator(ACCEL_REOPEN_TAB, 'T', true, true, false, false);
  window_->SetAccelerator(ACCEL_CLOSE_TAB, 'W', false, true, false, false);
  window_->SetAccelerator(ACCEL_FOCUS_OMNIBAR, 'L', false, true, false, false);
  window_->SetAccelerator(ACCEL_FOCUS_OMNIBAR, 'D', false, false, true, false);
  window_->SetAccelerator(ACCEL_PALETTE, 'K', false, true, false, false);
  window_->SetAccelerator(ACCEL_FIND, 'F', false, true, false, false);
  window_->SetAccelerator(ACCEL_RELOAD, 'R', false, true, false, false);
  window_->SetAccelerator(ACCEL_HARD_RELOAD, 'R', true, true, false, false);
  window_->SetAccelerator(ACCEL_TOGGLE_RAIL, 'B', false, true, false, false);
  window_->SetAccelerator(ACCEL_TOGGLE_TOPBAR, 'U', true, true, false, false);
  window_->SetAccelerator(ACCEL_BOOKMARK, 'D', false, true, false, false);
  window_->SetAccelerator(ACCEL_MUTE_TAB, 'M', false, true, false, false);
  window_->SetAccelerator(ACCEL_DOWNLOADS, 'J', false, true, false, false);
  window_->SetAccelerator(ACCEL_PRIVATE_WINDOW, 'N', true, true, false, false);
  window_->SetAccelerator(ACCEL_PROFILES, 'M', true, true, false, false);
  window_->SetAccelerator(ACCEL_SETTINGS, 0xBC /*VK_OEM_COMMA*/, false, true, false, false);
  window_->SetAccelerator(ACCEL_FULLSCREEN, 0x7A /*F11*/, false, false, false, false);
  window_->SetAccelerator(ACCEL_DEVTOOLS, 0x7B /*F12*/, false, false, false, false);

  window_->Show();
  UpdateOverlayBounds();
  ScheduleIdleSuspendTimer();

  if (!cfg_.private_mode) {
    ipc_server_ = std::make_unique<ProfileIpcServer>();
    CefRefPtr<KajoBrowserWindow> self(this);
    const std::string subdir =
        (cfg_.profile_id.empty() || cfg_.profile_id == "default")
            ? "default"
            : cfg_.profile_id;
    ipc_server_->Start(ProfileIpcSocketPath(subdir), [self](std::vector<std::string> urls) {
      CefPostTask(TID_UI, new UiTask([self, u = std::move(urls)]() {
        if (!self->closing_) self->HandleExternalOpen(u);
      }));
    });
  }

  /* Open CLI URLs, saved session (lazy-restoring inactive tabs as suspended), or default New Tab */
  if (!cfg_.initial_urls.empty()) {
    for (size_t i = 0; i < cfg_.initial_urls.size(); ++i) {
      const std::string& raw = cfg_.initial_urls[i];
      std::string target;
      if (raw.rfind("chrome://", 0) == 0 || raw.rfind("devtools://", 0) == 0) {
        target = raw;
      } else {
        g_autofree gchar* resolved = web_model_resolve_input(
            raw.c_str(),
            static_cast<KajoWebSearchEngine>(search_engine_),
            search_custom_url_.empty() ? nullptr : search_custom_url_.c_str());
        target = resolved ? resolved : raw;
      }
      OpenNewTab(target, i == cfg_.initial_urls.size() - 1);
    }
  } else if (!cfg_.private_mode && restore_session_ && storage_) {
    KajoWebModel* saved = web_model_new();
    if (web_storage_load_session(storage_, saved) && saved->tabs->len > 0) {
      const gint64 now = g_get_monotonic_time();
      uint64_t target_saved_id = saved->active_id;
      if (target_saved_id == 0) {
        target_saved_id = static_cast<const KajoWebTabMeta*>(g_ptr_array_index(saved->tabs, 0))->id;
      }
      uint64_t target_active = 0;
      for (guint i = 0; i < saved->tabs->len; ++i) {
        const auto* st = static_cast<const KajoWebTabMeta*>(g_ptr_array_index(saved->tabs, i));
        const bool is_active = (st->id == target_saved_id);
        const std::string uri = st->uri ? st->uri : DefaultNewTabUrl();
        const std::string title = (st->title && st->title[0]) ? st->title : uri;
        KajoWebTabMeta* tm = web_model_add_tab_ex(
            model_, uri.c_str(), title.c_str(),
            st->group, st->pinned, is_active, now);
        if (tm) {
          if (st->favicon_url && st->favicon_url[0] != '\0') {
            web_model_set_tab_favicon(model_, tm->id, st->favicon_url);
          }
          if (is_active) {
            AttachTabView(tm->id, uri, true);
            target_active = tm->id;
          } else {
            tm->suspended = TRUE;
          }
        }
      }
      if (!target_active && model_->tabs->len > 0) {
        auto* first = static_cast<KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, 0));
        first->suspended = FALSE;
        AttachTabView(first->id, first->uri ? first->uri : DefaultNewTabUrl(), true);
        target_active = first->id;
      }
      if (target_active) ActivateTab(target_active);
      PushStateToChrome();
    } else {
      OpenNewTab(DefaultNewTabUrl(), true);
    }
    web_model_free(saved);
  } else {
    OpenNewTab(DefaultNewTabUrl(), true);
  }

  if (cfg_.auto_adblock_sync && adblock_auto_sync_ && adblock_ && adblock_->IsEnabled() &&
      adblock_->HasMissingOrStaleLists(24)) {
    CefRefPtr<KajoBrowserWindow> self(this);
    CefPostDelayedTask(TID_UI, new UiTask([self]() {
      if (!self->closing_) self->StartFilterListUpdates();
    }), 400);
  }

  if (show_picker_on_startup_ && !cfg_.private_mode && !cfg_.explicit_profile &&
      cfg_.initial_urls.empty() && profiles_ && profiles_->profiles &&
      profiles_->profiles->len > 1) {
    CefRefPtr<KajoBrowserWindow> self(this);
    CefPostDelayedTask(TID_UI, new UiTask([self]() {
      if (!self->closing_) self->ToggleOverlay("profiles");
    }), 120);
  }
}

int KajoBrowserWindow::CurrentDockedRailWidth() const {
  if (IsFullscreen()) return 0;
  if (rail_mode_ == KAJO_WEB_RAIL_PINNED) return rail_width_px_;
  if (rail_mode_ == KAJO_WEB_RAIL_COMPACT) {
    return ComputeChromeUiMetrics(ui_font_size_px_).compact_rail_width_px;
  }
  return 0;
}

int KajoBrowserWindow::CurrentRailOverlayWidth() const {
  if (IsFullscreen()) return 0;
  if (rail_mode_ == KAJO_WEB_RAIL_PINNED) return rail_width_px_;
  if (rail_mode_ == KAJO_WEB_RAIL_COMPACT) {
    return rail_hover_open_ ? rail_width_px_ : ComputeChromeUiMetrics(ui_font_size_px_).compact_rail_width_px;
  }
  return rail_hover_open_ ? rail_width_px_ : 8;
}

int KajoBrowserWindow::CurrentTopbarHeight() const {
  if (IsFullscreen()) return 0;
  if (topbar_mode_ == KAJO_WEB_TOPBAR_ALWAYS ||
      topbar_hover_open_ || topbar_focused_ || ac_visible_ || !overlay_mode_.empty()) {
    return ComputeChromeUiMetrics(ui_font_size_px_).topbar_height_px;
  }
  return 6;
}

CefSize KajoBrowserWindow::GetPreferredChromeViewSize(CefRefPtr<CefView> view) const {
  if (!view) return CefSize();
  if (topbar_view_ && view->IsSame(topbar_view_)) {
    const int h = CurrentTopbarHeight();
    return h > 0 ? CefSize(100, h) : CefSize(1, 1);
  }
  if (rail_spacer_ && view->IsSame(rail_spacer_)) {
    const int w = CurrentDockedRailWidth();
    return w > 0 ? CefSize(w, 100) : CefSize(1, 1);
  }
  if (rail_view_ && view->IsSame(rail_view_)) {
    const int w = CurrentRailOverlayWidth();
    return w > 0 ? CefSize(w, 100) : CefSize(1, 1);
  }
  if (infobar_view_ && view->IsSame(infobar_view_)) {
    return CefSize(100, ComputeChromeUiMetrics(ui_font_size_px_).infobar_height_px);
  }
  if (overlay_view_ && view->IsSame(overlay_view_)) {
    return CefSize(100, 100);
  }
  return CefSize(100, 100);
}

void KajoBrowserWindow::RebuildLayout() {
  if (!window_ || !topbar_view_ || !body_panel_ || !rail_spacer_ || !content_panel_) return;

  if (!layout_built_) {
    CefBoxLayoutSettings vset;
    vset.horizontal = 0;
    vset.main_axis_alignment = CEF_AXIS_ALIGNMENT_START;
    vset.cross_axis_alignment = CEF_AXIS_ALIGNMENT_STRETCH;
    vset.inside_border_insets = {1, 1, 1, 1};
    CefRefPtr<CefBoxLayout> vlayout = window_->SetToBoxLayout(vset);
    window_->AddChildView(topbar_view_);
    vlayout->SetFlexForView(topbar_view_, 0);
    window_->AddChildView(body_panel_);
    vlayout->SetFlexForView(body_panel_, 1);

    CefBoxLayoutSettings hset;
    hset.horizontal = 1;
    hset.main_axis_alignment = CEF_AXIS_ALIGNMENT_START;
    hset.cross_axis_alignment = CEF_AXIS_ALIGNMENT_STRETCH;
    CefRefPtr<CefBoxLayout> hlayout = body_panel_->SetToBoxLayout(hset);
    if (rail_side_ == KAJO_WEB_SIDE_RIGHT) {
      body_panel_->AddChildView(content_panel_);
      body_panel_->AddChildView(rail_spacer_);
    } else {
      body_panel_->AddChildView(rail_spacer_);
      body_panel_->AddChildView(content_panel_);
    }
    hlayout->SetFlexForView(rail_spacer_, 0);
    hlayout->SetFlexForView(content_panel_, 1);
    layout_built_ = true;
  }

  UpdateWindowBorderColor();
  if (rail_side_ == KAJO_WEB_SIDE_RIGHT) {
    body_panel_->ReorderChildView(content_panel_, 0);
    body_panel_->ReorderChildView(rail_spacer_, 1);
  } else {
    body_panel_->ReorderChildView(rail_spacer_, 0);
    body_panel_->ReorderChildView(content_panel_, 1);
  }

  ApplyTopbarSizing();
  ApplyRailSizing();
  window_->Layout();
}

void KajoBrowserWindow::ApplyTopbarSizing() {
  if (!topbar_view_ || !window_) return;
  topbar_view_->SetVisible(!IsFullscreen());
  topbar_view_->InvalidateLayout();
  window_->Layout();
  ApplyDraggableRegions();
  UpdateOverlayBounds();
}

void KajoBrowserWindow::ApplyRailSizing() {
  if (!rail_spacer_ || !window_) return;
  const int w = CurrentDockedRailWidth();
  rail_spacer_->SetVisible(!IsFullscreen() && w > 0);
  rail_spacer_->InvalidateLayout();
  body_panel_->Layout();
  window_->Layout();
  UpdateOverlayBounds();
}

void KajoBrowserWindow::UpdateOverlayBounds() {
  if (!window_) return;
  CefRect wb = window_->GetBounds();
  if (wb.width < 200 || wb.height < 120) return;
  const int b = 1;
  const int inner_w = std::max(1, wb.width - 2 * b);
  const int top_h = CurrentTopbarHeight();

  if (rail_ctrl_) {
    const int w = CurrentRailOverlayWidth();
    if (IsFullscreen() || w <= 0) {
      rail_ctrl_->SetVisible(false);
    } else {
      const int y = b + top_h;
      const int h = std::max(0, wb.height - b - y);
      const int x = (rail_side_ == KAJO_WEB_SIDE_RIGHT) ? std::max(b, wb.width - b - w) : b;
      rail_ctrl_->SetBounds(CefRect(x, y, w, h));
      rail_ctrl_->SetVisible(true);
    }
  }

  if (infobar_ctrl_) {
    if (pending_permissions_.empty() || IsFullscreen()) {
      infobar_ctrl_->SetVisible(false);
    } else {
      const int rw = CurrentDockedRailWidth();
      const int x = b + (rail_side_ == KAJO_WEB_SIDE_LEFT ? rw : 0);
      const int w = std::max(240, inner_w - rw);
      const int y = b + top_h;
      infobar_ctrl_->SetBounds(CefRect(x, y, w, ComputeChromeUiMetrics(ui_font_size_px_).infobar_height_px));
      infobar_ctrl_->SetVisible(true);
    }
  }

  if (ac_ctrl_) {
    if (!ac_visible_ || IsFullscreen()) {
      ac_ctrl_->SetVisible(false);
    } else {
      int rw = CurrentDockedRailWidth();
      int x = b + (rail_side_ == KAJO_WEB_SIDE_LEFT ? rw : 0) + 120;
      int w = std::clamp(inner_w - rw - 240, 320, 740);
      int h = std::min(340, wb.height - 60);
      ac_ctrl_->SetBounds(CefRect(std::max(b + 12, std::min(x, wb.width - b - w - 12)), b + top_h + 1, w, h));
      ac_ctrl_->SetVisible(true);
    }
  }

  if (overlay_ctrl_) {
    if (overlay_mode_.empty() || IsFullscreen()) {
      overlay_ctrl_->SetVisible(false);
    } else {
      CefRect ob;
      if (overlay_mode_ == "find") {
        ob = CefRect(std::max(b + 12, wb.width - b - 392),
                     b + top_h + 6,
                     380,
                     ComputeChromeUiMetrics(ui_font_size_px_).findbar_height_px);
      } else if (overlay_mode_ == "bookmark") {
        int w = std::min(380, wb.width - 24);
        ob = CefRect(std::max(b + 12, wb.width - b - w - 160), b + top_h + 1, w, 246);
      } else if (overlay_mode_ == "new_group") {
        int w = std::min(360, wb.width - 24);
        ob = CefRect((wb.width - w) / 2, b + top_h + 64, w, 170);
      } else if (overlay_mode_ == "palette") {
        int w = std::clamp(wb.width - 80, 360, 680);
        int h = std::min(440, wb.height - 100);
        ob = CefRect((wb.width - w) / 2, b + top_h + 38, w, h);
      } else {
        int w = std::min(440, wb.width - 24);
        int h = std::min(420, wb.height - 60);
        ob = CefRect(std::max(b + 12, wb.width - b - w - 12), b + top_h + 1, w, h);
      }
      overlay_ctrl_->SetBounds(ob);
      overlay_ctrl_->SetVisible(true);
    }
  }
}

void KajoBrowserWindow::OnWindowBoundsChanged(CefRefPtr<CefWindow> window,
                                              const CefRect& new_bounds) {
  if (window_) window_->Layout();
  UpdateOverlayBounds();
}

CefRect KajoBrowserWindow::GetInitialBounds(CefRefPtr<CefWindow> window) {
  return CefRect(0, 0, 1440, 900);
}

bool KajoBrowserWindow::GetLinuxWindowProperties(CefRefPtr<CefWindow> window,
                                                 CefLinuxWindowProperties& properties) {
  CefString(&properties.wayland_app_id) = "org.kajo.Web";
  CefString(&properties.wm_class_class) = "org.kajo.Web";
  CefString(&properties.wm_class_name) = "kajo-web";
  return true;
}

bool KajoBrowserWindow::CanClose(CefRefPtr<CefWindow> window) {
  return true;
}

void KajoBrowserWindow::OnWindowClosing(CefRefPtr<CefWindow> window) {
  closing_ = true;
  if (ipc_server_) {
    ipc_server_->Stop();
    ipc_server_.reset();
  }
  ++push_gen_;
  ++rail_hover_gen_;
  ++topbar_hover_gen_;
  ++suspend_timer_gen_;
  for (auto& req : pending_permissions_) {
    ResolvePermissionReq(req, false, false);
  }
  pending_permissions_.clear();
  if (config_sub_id_) {
    if (auto* cs = kajo_config_store_get_default()) {
      kajo_config_store_unsubscribe(cs, config_sub_id_);
    }
    config_sub_id_ = 0;
  }
  for (auto& req : active_filter_reqs_) {
    if (req) req->Cancel();
  }
  active_filter_reqs_.clear();
  PersistSession();
  if (request_context_) {
    request_context_->ClearSchemeHandlerFactories();
    if (CefRefPtr<CefCookieManager> cm = request_context_->GetCookieManager(nullptr)) {
      cm->FlushStore(nullptr);
    }
  }
  for (auto& cc : chrome_clients_) {
    if (cc) static_cast<ChromeViewClient*>(cc.get())->Detach();
  }
  chrome_clients_.clear();
  if (bv_delegate_) {
    static_cast<KajoBrowserViewDelegate*>(bv_delegate_.get())->Detach();
    bv_delegate_ = nullptr;
  }
  if (panel_delegate_) {
    static_cast<KajoPanelDelegate*>(panel_delegate_.get())->Detach();
    panel_delegate_ = nullptr;
  }
  if (topbar_view_) {
    if (auto b = topbar_view_->GetBrowser()) router_->OnBeforeClose(b);
  }
  if (rail_view_) {
    if (auto b = rail_view_->GetBrowser()) router_->OnBeforeClose(b);
  }
  if (infobar_view_) {
    if (auto b = infobar_view_->GetBrowser()) router_->OnBeforeClose(b);
  }
  if (overlay_view_) {
    if (auto b = overlay_view_->GetBrowser()) router_->OnBeforeClose(b);
  }
  if (ac_view_) {
    if (auto b = ac_view_->GetBrowser()) router_->OnBeforeClose(b);
  }
  for (auto& kv : tabs_) {
    if (kv.second.client) kv.second.client->DetachCallbacks();
  }
  download_callbacks_.clear();
  if (router_) router_->RemoveHandler(this);
}

void KajoBrowserWindow::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
  window_ = nullptr;
  body_panel_ = nullptr;
  rail_spacer_ = nullptr;
  content_panel_ = nullptr;
  topbar_view_ = nullptr;
  rail_view_ = nullptr;
  infobar_view_ = nullptr;
  overlay_view_ = nullptr;
  ac_view_ = nullptr;
  rail_ctrl_ = nullptr;
  infobar_ctrl_ = nullptr;
  overlay_ctrl_ = nullptr;
  ac_ctrl_ = nullptr;
  topbar_browser_ = nullptr;
  rail_browser_ = nullptr;
  infobar_browser_ = nullptr;
  overlay_browser_ = nullptr;
  ac_browser_ = nullptr;
  chrome_clients_.clear();
  active_filter_reqs_.clear();
  download_callbacks_.clear();
  pending_permissions_.clear();
  tabs_.clear();
  router_ = nullptr;
  request_context_ = nullptr;
  CefQuitMessageLoop();
}

void KajoBrowserWindow::OnWindowActivationChanged(CefRefPtr<CefWindow> window, bool active) {
  if (closing_) return;
  const gint64 now = g_get_monotonic_time();
  if (!active) {
    window_active_ = false;
    window_inactive_since_us_ = now;
    if (KajoWebTabMeta* act = web_model_get_active(model_)) {
      act->last_active_us = now;
    }
    return;
  }
  window_active_ = true;
  window_inactive_since_us_ = 0;
  if (KajoWebTabMeta* act = web_model_get_active(model_)) {
    if (act->suspended) {
      ActivateTab(act->id);
    } else {
      act->last_active_us = now;
    }
  }
}

void KajoBrowserWindow::OnChromeBrowserCreated(int role, CefRefPtr<CefBrowser> browser) {
  if (role == ROLE_TOPBAR) topbar_browser_ = browser;
  else if (role == ROLE_RAIL) rail_browser_ = browser;
  else if (role == ROLE_INFOBAR) infobar_browser_ = browser;
  else if (role == ROLE_OVERLAY) overlay_browser_ = browser;
  else if (role == ROLE_AC) ac_browser_ = browser;
  PushStateImmediate();
}

void KajoBrowserWindow::ApplyDraggableRegions() {
  if (!window_ || !topbar_view_) return;
  if (IsFullscreen() ||
      (topbar_mode_ == KAJO_WEB_TOPBAR_AUTOHIDE && !topbar_hover_open_ && !topbar_focused_ &&
       !ac_visible_ && overlay_mode_.empty())) {
    window_->SetDraggableRegions({});
    return;
  }
  if (cached_topbar_drag_regions_.empty()) return;
  std::vector<CefDraggableRegion> mapped;
  mapped.reserve(cached_topbar_drag_regions_.size());
  for (const auto& r : cached_topbar_drag_regions_) {
    CefDraggableRegion out = r;
    CefPoint pt(r.bounds.x, r.bounds.y);
    if (topbar_view_->ConvertPointToWindow(pt)) {
      out.bounds.x = pt.x;
      out.bounds.y = pt.y;
    }
    mapped.push_back(out);
  }
  window_->SetDraggableRegions(mapped);
}

void KajoBrowserWindow::OnChromeDraggableRegions(const std::vector<CefDraggableRegion>& regions) {
  cached_topbar_drag_regions_ = regions;
  ApplyDraggableRegions();
}

bool KajoBrowserWindow::OnChromeShortcut(const CefKeyEvent& event) {
  return OnTabShortcut(model_ ? model_->active_id : 0, event);
}

void KajoBrowserWindow::AttachTabView(uint64_t tab_id, const std::string& target, bool foreground) {
  std::string preserved_favicon;
  if (auto it = tabs_.find(tab_id); it != tabs_.end()) {
    preserved_favicon = it->second.favicon_url;
  }
  if (preserved_favicon.empty() && model_) {
    if (KajoWebTabMeta* t = web_model_find_tab(model_, tab_id)) {
      if (t->favicon_url) preserved_favicon = t->favicon_url;
    }
  }
  TabEntry entry;
  entry.id = tab_id;
  entry.favicon_url = preserved_favicon;
  entry.client = new CefTabClient(tab_id, adblock_, this);
  entry.client->SetHttpsOnly(https_only_);
  entry.client->SetCookiePolicy(cookie_policy_);
  entry.client->SetDevToolsEnabled(developer_extras_);
  entry.client->SetUserScripts(user_scripts_enabled_, user_scripts_);
  tabs_[tab_id] = entry;

  CefBrowserSettings bs = MakeContentBrowserSettings();
  CefRefPtr<CefBrowserView> view = CefBrowserView::CreateBrowserView(
      entry.client, target, bs, nullptr, request_context_, bv_delegate_);

  tabs_[tab_id].view = view;
  content_panel_->AddChildView(view);
  view->SetVisible(false);
  if (!tabs_[tab_id].browser && view && view->GetBrowser()) {
    tabs_[tab_id].browser = view->GetBrowser();
  }

  if (foreground || tabs_.size() == 1) {
    ActivateTab(tab_id);
  } else {
    PushStateToChrome();
  }
}

uint64_t KajoBrowserWindow::OpenNewTab(const std::string& url,
                                       bool foreground,
                                       const std::string& group) {
  const std::string target = url.empty() ? DefaultNewTabUrl() : url;
  KajoWebTabMeta* meta = web_model_add_tab_ex(
      model_, target.c_str(), "Loading...",
      group.empty() ? nullptr : group.c_str(), FALSE, foreground, g_get_monotonic_time());
  if (!meta) return 0;
  AttachTabView(meta->id, target, foreground);
  return meta->id;
}

void KajoBrowserWindow::ActivateTab(uint64_t tab_id) {
  if (!model_ || !web_model_activate(model_, tab_id, g_get_monotonic_time())) return;
  KajoWebTabMeta* target_meta = web_model_find_tab(model_, tab_id);
  TabEntry* existing = FindEntry(tab_id);
  if (target_meta && (target_meta->suspended || !existing || !existing->view)) {
    target_meta->suspended = FALSE;
    AttachTabView(tab_id, target_meta->uri ? target_meta->uri : DefaultNewTabUrl(), false);
  }
  for (auto& kv : tabs_) {
    const bool is_act = (kv.first == tab_id);
    if (kv.second.view) kv.second.view->SetVisible(is_act);
    if (is_act && kv.second.view && kv.second.browser && kv.second.browser->GetHost()) {
      kv.second.view->RequestFocus();
    }
  }
  if (target_meta) target_meta->suspended = FALSE;
  if (content_panel_) content_panel_->Layout();
  if (window_) window_->Layout();
  PushStateImmediate();
}

void KajoBrowserWindow::DestroyTabView(CefRefPtr<CefBrowserView> view,
                                       CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefTabClient> client) {
  if (client) client->DetachCallbacks();
  if (!browser && view) browser = view->GetBrowser();
  if (content_panel_ && view) {
    content_panel_->RemoveChildView(view);
  }
  if (closing_) {
    view = nullptr;
    if (browser && browser->GetHost()) {
      browser->GetHost()->CloseBrowser(true);
    }
    return;
  }
  if (view) {
    CefRefPtr<TabReaperWindowDelegate> reaper(
        new TabReaperWindowDelegate(std::move(view), std::move(browser)));
    CefRefPtr<CefWindow> rw = CefWindow::CreateTopLevelWindow(reaper);
    if (!rw && browser && browser->GetHost()) {
      browser->GetHost()->CloseBrowser(true);
    }
  } else if (browser && browser->GetHost()) {
    browser->GetHost()->CloseBrowser(true);
  }
}

void KajoBrowserWindow::CloseTab(uint64_t tab_id) {
  if (!model_ || closing_) return;
  const bool was_active = (model_->active_id == tab_id);
  CancelPermissionsForTab(tab_id);
  if (adblock_) adblock_->RemoveTabBlockedCount(static_cast<int>(tab_id));
  auto it = tabs_.find(tab_id);
  if (it != tabs_.end()) {
    CefRefPtr<CefBrowserView> v = it->second.view;
    CefRefPtr<CefBrowser> b = it->second.browser;
    CefRefPtr<CefTabClient> c = it->second.client;
    tabs_.erase(it);
    DestroyTabView(std::move(v), std::move(b), std::move(c));
  }

  web_model_close_tab(model_, tab_id);
  if (model_->tabs->len == 0) {
    OpenNewTab(DefaultNewTabUrl(), true);
  } else if (was_active) {
    ActivateTab(model_->active_id);
  }
  PersistSession();
  PushStateToChrome();
}

void KajoBrowserWindow::OnTabRequestClose(uint64_t tab_id) {
  if (!closing_) CloseTab(tab_id);
}

void KajoBrowserWindow::ReopenClosedTab() {
  if (!model_) return;
  KajoWebTabMeta* reopened = web_model_reopen_closed(model_, g_get_monotonic_time());
  if (!reopened) return;
  const std::string uri = reopened->uri ? reopened->uri : DefaultNewTabUrl();
  AttachTabView(reopened->id, uri, true);
  PersistSession();
  PushStateToChrome();
}

void KajoBrowserWindow::DuplicateTab(uint64_t tab_id) {
  if (!model_) return;
  KajoWebTabMeta* src = web_model_find_tab(model_, tab_id);
  if (!src) return;
  const std::string uri = src->uri ? src->uri : DefaultNewTabUrl();
  const std::string grp = src->group ? src->group : "";
  std::string fav = src->favicon_url ? src->favicon_url : "";
  if (fav.empty()) {
    if (TabEntry* se = FindEntry(tab_id)) fav = se->favicon_url;
  }
  const uint64_t new_id = OpenNewTab(uri, true, grp);
  if (new_id && !fav.empty()) {
    web_model_set_tab_favicon(model_, new_id, fav.c_str());
    if (TabEntry* ne = FindEntry(new_id)) ne->favicon_url = fav;
  }
  PersistSession();
}

bool KajoBrowserWindow::SuspendTab(uint64_t tab_id,
                                   bool manual,
                                   bool allow_inactive_window_active) {
  if (!model_ || closing_) return false;
  KajoWebTabMeta* t = web_model_find_tab(model_, tab_id);
  if (!t || t->suspended || t->capture_active || t->download_active) {
    return false;
  }
  if (t->id == model_->active_id && !(allow_inactive_window_active && !window_active_)) {
    return false;
  }
  if (!manual && !allow_inactive_window_active &&
      !web_model_can_suspend(model_, t, suspend_idle_min_, g_get_monotonic_time())) {
    return false;
  }
  CancelPermissionsForTab(tab_id);
  if (auto it = tabs_.find(tab_id); it != tabs_.end()) {
    CefRefPtr<CefBrowserView> v = it->second.view;
    CefRefPtr<CefBrowser> b = it->second.browser;
    CefRefPtr<CefTabClient> c = it->second.client;
    it->second.client = nullptr;
    it->second.view = nullptr;
    it->second.browser = nullptr;
    it->second.can_go_back = false;
    it->second.can_go_forward = false;
    it->second.media.clear();
    DestroyTabView(std::move(v), std::move(b), std::move(c));
  }
  t->suspended = TRUE;
  t->loading = FALSE;
  t->audible = FALSE;
  return true;
}

void KajoBrowserWindow::SweepIdleTabs() {
  if (!model_ || closing_ || suspend_idle_min_ <= 0) return;
  const gint64 now = g_get_monotonic_time();
  std::vector<uint64_t> to_suspend;
  for (guint i = 0; i < model_->tabs->len; ++i) {
    const auto* t = static_cast<const KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, i));
    if (web_model_can_suspend(model_, t, suspend_idle_min_, now)) {
      to_suspend.push_back(t->id);
    }
  }
  bool any = false;
  for (uint64_t tid : to_suspend) {
    if (SuspendTab(tid, false)) any = true;
  }
  if (!window_active_ && window_inactive_since_us_ > 0) {
    const gint64 threshold_us = static_cast<gint64>(suspend_idle_min_) * 60 * G_USEC_PER_SEC;
    if ((now - window_inactive_since_us_) >= threshold_us) {
      if (KajoWebTabMeta* act = web_model_get_active(model_)) {
        if (!act->suspended && !act->pinned && !act->audible && !act->capture_active &&
            !act->download_active && !(act->uri && g_str_has_prefix(act->uri, "kajo://")) &&
            (now - act->last_active_us) >= threshold_us) {
          if (SuspendTab(act->id, false, true)) any = true;
        }
      }
    }
  }
  if (any) PushStateToChrome();
}

void KajoBrowserWindow::ScheduleIdleSuspendTimer() {
  if (closing_) return;
  const uint64_t gen = ++suspend_timer_gen_;
  CefRefPtr<KajoBrowserWindow> self(this);
  CefPostDelayedTask(TID_UI, new UiTask([self, gen]() {
    if (self->closing_ || self->suspend_timer_gen_ != gen) return;
    self->SweepIdleTabs();
    self->ScheduleIdleSuspendTimer();
  }), 60000);
}

void KajoBrowserWindow::PeekRailTemporarily(int duration_ms) {
  if (rail_mode_ != KAJO_WEB_RAIL_AUTOHIDE || closing_) return;
  if (!rail_hover_open_) {
    rail_hover_open_ = true;
    ApplyRailSizing();
    PushStateImmediate();
  }
  const uint64_t gen = ++rail_hover_gen_;
  CefRefPtr<KajoBrowserWindow> self(this);
  CefPostDelayedTask(TID_UI, new UiTask([self, gen]() {
    if (self->closing_ || self->rail_hover_gen_ != gen) return;
    if (self->rail_mode_ == KAJO_WEB_RAIL_AUTOHIDE &&
        self->rail_hover_open_ && self->rail_hold_count_ == 0) {
      self->rail_hover_open_ = false;
      self->ApplyRailSizing();
      self->PushStateImmediate();
    }
  }), duration_ms);
}

void KajoBrowserWindow::FocusOmnibar() {
  if (topbar_mode_ == KAJO_WEB_TOPBAR_AUTOHIDE && !topbar_hover_open_) {
    topbar_hover_open_ = true;
    ++topbar_hover_gen_;
  }
  topbar_focused_ = true;
  ApplyTopbarSizing();
  PushStateImmediate();
  if (topbar_view_ && topbar_browser_ && topbar_browser_->GetMainFrame()) {
    topbar_view_->RequestFocus();
    topbar_browser_->GetMainFrame()->ExecuteJavaScript(
        "window.__kajoFocusOmnibar && window.__kajoFocusOmnibar();", "", 0);
  }
}

void KajoBrowserWindow::OpenBookmarkPopover() {
  KajoWebTabMeta* t = web_model_get_active(model_);
  if (!t || !t->uri || !storage_) return;
  g_autofree gchar* existing_uuid = nullptr;
  if (!web_storage_is_bookmarked(storage_, t->uri, &existing_uuid)) {
    g_autofree gchar* uuid = g_uuid_string_random();
    KajoWebBookmark* bm = web_bookmark_new(
        uuid, (t->title && t->title[0]) ? t->title : t->uri,
        t->uri, "", g_get_real_time() / 1000, FALSE);
    web_storage_upsert_bookmark(storage_, bm);
    web_bookmark_free(bm);
    RefreshStorageCache();
  }
  if (topbar_mode_ == KAJO_WEB_TOPBAR_AUTOHIDE && !topbar_hover_open_) {
    topbar_hover_open_ = true;
    ApplyTopbarSizing();
  }
  if (overlay_mode_ == "bookmark") {
    CloseOverlay();
  } else {
    ToggleOverlay("bookmark");
  }
}

void KajoBrowserWindow::ResolvePermissionReq(PendingPermissionReq& req, bool allow, bool persist) {
  if (persist && !cfg_.private_mode && !req.origin.empty() && !req.perm_key.empty()) {
    permission_store_.Set(req.origin, req.perm_key, allow);
    if (!permission_store_path_.empty()) permission_store_.Save(permission_store_path_);
  }
  if (req.media_cb) {
    if (allow) req.media_cb->Continue(req.media_perms);
    else req.media_cb->Cancel();
    req.media_cb = nullptr;
  }
  if (req.prompt_cb) {
    req.prompt_cb->Continue(allow ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
    req.prompt_cb = nullptr;
  }
}

void KajoBrowserWindow::RespondActivePermission(uint64_t req_id, const std::string& decision) {
  for (auto it = pending_permissions_.begin(); it != pending_permissions_.end(); ++it) {
    if (it->req_id == req_id) {
      PendingPermissionReq req = std::move(*it);
      pending_permissions_.erase(it);
      if (decision == "once") ResolvePermissionReq(req, true, false);
      else if (decision == "always") ResolvePermissionReq(req, true, true);
      else if (decision == "deny") ResolvePermissionReq(req, false, true);
      else ResolvePermissionReq(req, false, false);
      break;
    }
  }
  UpdateOverlayBounds();
  PushStateImmediate();
}

void KajoBrowserWindow::CancelPermissionsForTab(uint64_t tab_id) {
  bool removed = false;
  for (auto it = pending_permissions_.begin(); it != pending_permissions_.end();) {
    if (it->tab_id == tab_id) {
      PendingPermissionReq req = std::move(*it);
      it = pending_permissions_.erase(it);
      ResolvePermissionReq(req, false, false);
      removed = true;
    } else {
      ++it;
    }
  }
  if (removed) UpdateOverlayBounds();
}

void KajoBrowserWindow::NavigateActive(const std::string& raw_input) {
  if (raw_input.empty()) return;
  std::string target;
  if (raw_input.rfind("chrome://", 0) == 0 || raw_input.rfind("devtools://", 0) == 0) {
    target = raw_input;
  } else {
    g_autofree gchar* resolved = web_model_resolve_input(
        raw_input.c_str(),
        static_cast<KajoWebSearchEngine>(search_engine_),
        search_custom_url_.empty() ? nullptr : search_custom_url_.c_str());
    target = resolved ? resolved : raw_input;
  }
  HideAutocomplete();
  CloseOverlay();
  if (TabEntry* e = ActiveEntry()) {
    if (e->browser && e->browser->GetMainFrame()) {
      e->browser->GetMainFrame()->LoadURL(target);
      if (e->view) e->view->RequestFocus();
    }
  }
}

void KajoBrowserWindow::ToggleActiveBookmark() {
  KajoWebTabMeta* t = web_model_get_active(model_);
  if (!t || !t->uri || !storage_) return;
  g_autofree gchar* existing_uuid = nullptr;
  if (web_storage_is_bookmarked(storage_, t->uri, &existing_uuid) && existing_uuid) {
    web_storage_delete_bookmark(storage_, existing_uuid);
  } else {
    g_autofree gchar* uuid = g_uuid_string_random();
    KajoWebBookmark* bm = web_bookmark_new(
        uuid, (t->title && t->title[0]) ? t->title : t->uri,
        t->uri, "", g_get_real_time() / 1000, FALSE);
    web_storage_upsert_bookmark(storage_, bm);
    web_bookmark_free(bm);
  }
  RefreshStorageCache();
  PushStateImmediate();
}

void KajoBrowserWindow::ToggleOverlay(const std::string& mode) {
  HideAutocomplete();
  if (overlay_mode_ == mode) {
    CloseOverlay();
    return;
  }
  overlay_mode_ = mode;
  if (mode == "palette") BuildPalette("");
  if (mode == "downloads") downloads_unseen_complete_ = false;
  UpdateOverlayBounds();
  PushStateImmediate();
  if (overlay_view_ && overlay_browser_) {
    overlay_view_->RequestFocus();
    if (auto f = overlay_browser_->GetMainFrame()) {
      f->ExecuteJavaScript("window.__kajoOverlayFocus && window.__kajoOverlayFocus();", f->GetURL(), 0);
    }
  }
}

void KajoBrowserWindow::CloseOverlay() {
  if (overlay_mode_.empty()) return;
  if (overlay_mode_ == "find") {
    if (TabEntry* e = ActiveEntry()) {
      if (e->browser && e->browser->GetHost()) e->browser->GetHost()->StopFinding(true);
    }
    find_count_ = 0;
    find_active_ = 0;
  }
  overlay_mode_.clear();
  if (overlay_ctrl_) overlay_ctrl_->SetVisible(false);
  if (TabEntry* e = ActiveEntry()) {
    if (e->view) e->view->RequestFocus();
  }
  PushStateToChrome();
}

void KajoBrowserWindow::HideAutocomplete() {
  if (!ac_visible_ && ac_results_.empty()) return;
  ac_visible_ = false;
  ac_selected_idx_ = -1;
  ac_results_.clear();
  if (ac_ctrl_) ac_ctrl_->SetVisible(false);
  PushStateToChrome();
}

void KajoBrowserWindow::BuildAutocomplete(const std::string& query) {
  ac_query_ = query;
  ac_selected_idx_ = -1;
  ac_results_.clear();
  if (query.empty()) {
    HideAutocomplete();
    return;
  }
  /* Open tabs matching */
  for (guint i = 0; model_ && i < model_->tabs->len && ac_results_.size() < 4; ++i) {
    const auto* t = static_cast<const KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, i));
    if (ContainsInsensitive(t->title ? t->title : "", query) ||
        ContainsInsensitive(t->uri ? t->uri : "", query)) {
      ChromeSuggestionView s;
      s.kind = "tab";
      s.label = t->title ? t->title : "";
      s.sublabel = t->uri ? t->uri : "";
      s.action = "activate_tab";
      s.tab_id = t->id;
      ac_results_.push_back(s);
    }
  }
  /* History + Bookmarks via SQLite */
  if (storage_) {
    GPtrArray* bms = web_storage_list_bookmarks(storage_, FALSE);
    for (guint i = 0; bms && i < bms->len && ac_results_.size() < 7; ++i) {
      const auto* b = static_cast<const KajoWebBookmark*>(g_ptr_array_index(bms, i));
      if (ContainsInsensitive(b->title ? b->title : "", query) ||
          ContainsInsensitive(b->uri ? b->uri : "", query)) {
        ChromeSuggestionView s;
        s.kind = "bookmark";
        s.label = b->title ? b->title : b->uri;
        s.sublabel = b->uri ? b->uri : "";
        s.action = "navigate";
        s.payload = s.sublabel;
        ac_results_.push_back(s);
      }
    }
    if (bms) g_ptr_array_unref(bms);

    GPtrArray* hist = web_storage_search_history(storage_, query.c_str(), 6);
    for (guint i = 0; hist && i < hist->len && ac_results_.size() < 10; ++i) {
      const auto* h = static_cast<const KajoWebHistoryEntry*>(g_ptr_array_index(hist, i));
      ChromeSuggestionView s;
      s.kind = "history";
      s.label = (h->title && h->title[0]) ? h->title : (h->uri ? h->uri : "");
      s.sublabel = h->uri ? h->uri : "";
      s.action = "navigate";
      s.payload = s.sublabel;
      ac_results_.push_back(s);
    }
    if (hist) g_ptr_array_unref(hist);
  }
  /* Fallback explicit search/navigate entry */
  ChromeSuggestionView direct;
  direct.kind = "search";
  direct.label = "Go / Search: " + query;
  direct.sublabel = "Press Enter to navigate";
  direct.action = "navigate";
  direct.payload = query;
  ac_results_.push_back(direct);

  ac_visible_ = true;
  UpdateOverlayBounds();
  PushStateImmediate();
}

void KajoBrowserWindow::BuildPalette(const std::string& query) {
  palette_query_ = query;
  palette_results_.clear();

  struct BuiltinCmd { const char* label; const char* sub; const char* act; const char* pay; };
  const BuiltinCmd cmds[] = {
      {"New Tab", "Ctrl+T", "new_tab", ""},
      {"Close Active Tab", "Ctrl+W", "close_active_tab", ""},
      {"Reopen Closed Tab", "Ctrl+Shift+T", "reopen_tab", ""},
      {"Duplicate Active Tab", "Tab Action", "duplicate_active_tab", ""},
      {"Focus Omnibar", "Ctrl+L · Alt+D", "focus_omnibar", ""},
      {"Hard Reload Active Tab", "Ctrl+Shift+R", "hard_reload", ""},
      {"Mute / Unmute Active Tab", "Ctrl+M", "mute_active_tab", ""},
      {"Pin / Unpin Active Tab", "Tab Action", "pin_active_tab", ""},
      {"Suspend Inactive Tabs Now", "Memory Saver", "suspend_inactive_tabs", ""},
      {"Toggle Vertical Tab Rail", "Ctrl+B", "toggle_rail", ""},
      {"Cycle Rail Mode (Compact/Pinned/Autohide)", "Layout", "cycle_rail_mode", ""},
      {"Toggle Top Bar Autohide (Zen Mode)", "Ctrl+Shift+U", "toggle_topbar_mode", ""},
      {"Bookmark Page", "Ctrl+D", "open_bookmark_popover", ""},
      {"Find in Page", "Ctrl+F", "open_find", ""},
      {"Open Downloads Overlay", "Ctrl+J", "open_downloads_overlay", ""},
      {"Open Profiles & Private Switcher", "Ctrl+Shift+M", "open_profiles_overlay", ""},
      {"Open Adblock Dashboard", "kajo://adblock", "open_kajo_page", "kajo://adblock"},
      {"Open GPU & VA-API Diagnostics", "kajo://gpu", "open_kajo_page", "kajo://gpu"},
      {"Open Bookmarks Manager", "kajo://bookmarks", "open_kajo_page", "kajo://bookmarks"},
      {"Open History Manager", "kajo://history", "open_kajo_page", "kajo://history"},
      {"Open Downloads Manager", "kajo://downloads", "open_kajo_page", "kajo://downloads"},
      {"Open Site Permissions", "kajo://permissions", "open_kajo_page", "kajo://permissions"},
      {"Open Web Settings", "Ctrl+, · kajo-settings", "open_settings", ""},
      {"Toggle Niri Float / Tile", "Compositor", "toggle_float", ""},
      {"Open Private Window", "Ctrl+Shift+N", "launch_private", ""},
  };
  for (const auto& c : cmds) {
    if (ContainsInsensitive(c.label, query) || ContainsInsensitive(c.sub, query)) {
      ChromeSuggestionView v;
      v.kind = "command";
      v.label = c.label;
      v.sublabel = c.sub;
      v.action = c.act;
      v.payload = c.pay;
      palette_results_.push_back(v);
    }
  }
  for (guint i = 0; model_ && i < model_->tabs->len; ++i) {
    const auto* t = static_cast<const KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, i));
    if (ContainsInsensitive(t->title ? t->title : "", query) ||
        ContainsInsensitive(t->uri ? t->uri : "", query)) {
      ChromeSuggestionView v;
      v.kind = "tab";
      v.label = t->title ? t->title : "Tab";
      v.sublabel = t->uri ? t->uri : "";
      v.action = "activate_tab";
      v.tab_id = t->id;
      palette_results_.push_back(v);
    }
  }
}

void KajoBrowserWindow::StartFilterListUpdates() {
  if (filter_update_busy_ || !adblock_ || closing_) return;
  const auto specs = GetDefaultFilterListSpecs();
  filter_update_busy_ = true;
  filter_updates_pending_ = specs.size();
  filter_updates_ok_ = 0;
  filter_updates_failed_ = 0;
  filter_update_status_ = "Downloading filter lists (0/" + std::to_string(specs.size()) + ")...";
  active_filter_reqs_.clear();
  PushStateToChrome();

  for (const auto& spec : specs) {
    CefRefPtr<CefRequest> req = CefRequest::Create();
    req->SetURL(spec.url);
    req->SetMethod("GET");
    auto r = CefURLRequest::Create(req, new FilterDownloadClient(this, spec.slug), request_context_);
    if (r) active_filter_reqs_.push_back(r);
  }
}

void KajoBrowserWindow::HandleFilterListDownloadComplete(const std::string& id,
                                                       bool ok,
                                                       int http_status,
                                                       const std::string& body) {
  if (closing_) return;
  if (ok && !body.empty() && adblock_) {
    CefRefPtr<KajoBrowserWindow> self(this);
    std::shared_ptr<CefAdblockEngine> eng = adblock_;
    std::string body_copy = body;
    CefPostTask(TID_FILE_USER_VISIBLE, new UiTask([self, eng, id, body_copy, http_status]() {
      const bool wrote = eng ? eng->WriteListFileOnly(id, body_copy) : false;
      CefPostTask(TID_UI, new UiTask([self, id, wrote, http_status]() {
        self->FinishFilterListWriteOnUi(id, wrote, http_status);
      }));
    }));
    return;
  }
  FinishFilterListWriteOnUi(id, false, http_status);
}

void KajoBrowserWindow::FinishFilterListWriteOnUi(const std::string& id,
                                                  bool wrote_ok,
                                                  int http_status) {
  if (closing_) return;
  if (wrote_ok) {
    ++filter_updates_ok_;
  } else {
    ++filter_updates_failed_;
    if (http_status > 0) {
      filter_update_status_ = "List '" + id + "' HTTP " + std::to_string(http_status);
    }
  }
  if (filter_updates_pending_ > 0) --filter_updates_pending_;
  if (filter_updates_pending_ == 0) {
    active_filter_reqs_.clear();
    filter_update_status_ = "Compiling filter lists...";
    PushStateToChrome();
    CefRefPtr<KajoBrowserWindow> self(this);
    std::shared_ptr<CefAdblockEngine> eng = adblock_;
    std::string custom_path = adblock_custom_rules_path_;
    size_t ok_cnt = filter_updates_ok_;
    size_t fail_cnt = filter_updates_failed_;
    CefPostTask(TID_FILE_USER_VISIBLE, new UiTask([self, eng, custom_path, ok_cnt, fail_cnt]() {
      if (eng) eng->ReloadFromDisk(custom_path);
      CefPostTask(TID_UI, new UiTask([self, ok_cnt, fail_cnt]() {
        if (!self->closing_) self->FinishFilterListReloadOnUi(ok_cnt, fail_cnt);
      }));
    }));
  } else {
    filter_update_status_ = "Downloading filter lists (" +
                            std::to_string(filter_updates_ok_ + filter_updates_failed_) + "/" +
                            std::to_string(filter_updates_ok_ + filter_updates_failed_ + filter_updates_pending_) + ")...";
    PushStateToChrome();
  }
}

void KajoBrowserWindow::FinishFilterListReloadOnUi(size_t ok_count, size_t fail_count) {
  filter_update_busy_ = false;
  const size_t total_rules = adblock_ ? adblock_->GetRuleCount() : 0;
  if (fail_count > 0 && ok_count == 0) {
    filter_update_status_ = "Update failed (" + std::to_string(fail_count) + " lists failed; " +
                            std::to_string(total_rules) + " baseline rules active)";
  } else if (fail_count > 0) {
    filter_update_status_ = "Updated " + std::to_string(ok_count) + " lists (" +
                            std::to_string(fail_count) + " failed) · " +
                            std::to_string(total_rules) + " rules";
  } else {
    filter_update_status_ = "Updated all " + std::to_string(ok_count) + " lists · " +
                            std::to_string(total_rules) + " rules";
  }
  PushStateImmediate();

  /* Reload any open internal adblock/newtab tab so live rule counts show up cleanly */
  for (auto& kv : tabs_) {
    KajoWebTabMeta* t = web_model_find_tab(model_, kv.first);
    if (t && t->uri &&
        (strncmp(t->uri, "kajo://adblock", 14) == 0 ||
         strncmp(t->uri, "kajo://newtab", 13) == 0)) {
      CefRefPtr<CefBrowser> b = kv.second.browser
                                    ? kv.second.browser
                                    : (kv.second.view ? kv.second.view->GetBrowser() : nullptr);
      if (b) b->ReloadIgnoreCache();
    }
  }
}

KajoBrowserWindow::TabEntry* KajoBrowserWindow::FindEntry(uint64_t tab_id) {
  auto it = tabs_.find(tab_id);
  return (it == tabs_.end()) ? nullptr : &it->second;
}

KajoBrowserWindow::TabEntry* KajoBrowserWindow::ActiveEntry() {
  return model_ ? FindEntry(model_->active_id) : nullptr;
}

void KajoBrowserWindow::PersistSession() {
  if (!cfg_.private_mode && storage_ && model_) {
    web_storage_save_session(storage_, model_);
  }
}

void KajoBrowserWindow::PushStateToChrome() {
  if (closing_ || push_pending_) return;
  push_pending_ = true;
  const uint64_t gen = ++push_gen_;
  CefRefPtr<KajoBrowserWindow> self(this);
  CefPostDelayedTask(TID_UI, new UiTask([self, gen]() {
    if (!self->closing_ && self->push_gen_ == gen) {
      self->push_pending_ = false;
      self->PushStateImmediate();
    }
  }), 30);
}

void KajoBrowserWindow::PushStateImmediate() {
  if (!store_ || !model_ || closing_) return;
  ChromeStateSnapshot s;
  s.private_mode = cfg_.private_mode;
  s.is_maximized = window_ ? window_->IsMaximized() : false;
  s.rail_mode = rail_mode_;
  s.rail_side = rail_side_;
  s.rail_hover_open = rail_hover_open_;
  s.rail_width_px = rail_width_px_;
  s.topbar_mode = topbar_mode_;
  s.topbar_hover_open = topbar_hover_open_ || topbar_focused_ || ac_visible_ || !overlay_mode_.empty();
  s.profile_border_accent = profile_border_accent_;
  s.show_tab_domains = show_tab_domains_;
  s.ac_visible = ac_visible_;
  s.ac_selected_idx = ac_selected_idx_;
  s.closed_tab_count = (model_ && model_->closed_stack) ? static_cast<int>(model_->closed_stack->len) : 0;
  s.overlay_mode = overlay_mode_;
  s.accent_color = accent_color_;
  s.profile_accent = profile_accent_;
  s.profile_id = cfg_.profile_id;
  s.profile_name = profile_name_;
  s.profile_icon = profile_icon_;
  const ChromeUiMetrics metrics = ComputeChromeUiMetrics(ui_font_size_px_);
  s.ui_font_family = ui_font_family_;
  s.ui_font_size_px = ui_font_size_px_;
  s.heading_size_px = heading_size_px_;
  s.sub_size_px = sub_size_px_;
  s.badge_size_px = badge_size_px_;
  s.topbar_height_px = metrics.topbar_height_px;
  s.control_height_px = metrics.control_height_px;
  s.icon_size_px = metrics.icon_size_px;
  s.hw_accel_enabled = cfg_.hw_accel_enabled;
  s.widevine_available = cfg_.widevine_available;
  s.widevine_path = cfg_.widevine_path;
  s.user_agent = cfg_.user_agent;
  s.find_query = find_query_;
  s.find_active = find_active_;
  s.find_count = find_count_;
  s.find_match_case = find_match_case_;
  s.filter_update_busy = filter_update_busy_;
  s.filter_update_status = filter_update_status_;

  KajoWebTabMeta* act = web_model_get_active(model_);
  TabEntry* act_e = ActiveEntry();
  if (act) {
    s.active_tab_id = act->id;
    s.active_url = act->uri ? act->uri : "";
    s.active_title = act->title ? act->title : "";
    g_autofree gchar* host = web_model_extract_host(s.active_url.c_str());
    s.active_host = host ? host : "";
    s.is_loading = act->loading;
  }
  if (act_e) {
    s.can_go_back = act_e->can_go_back;
    s.can_go_forward = act_e->can_go_forward;
    s.media_streams = act_e->media;
    s.default_zoom_pct = default_zoom_pct_;
    if (act_e->browser && act_e->browser->GetHost()) {
      double zl = act_e->browser->GetHost()->GetZoomLevel();
      s.zoom_pct = static_cast<int>(100.0 * pow(1.2, zl) + 0.5);
    }
  } else {
    s.default_zoom_pct = default_zoom_pct_;
  }
  if (storage_ && !s.active_url.empty()) {
    s.is_bookmarked = web_storage_is_bookmarked(storage_, s.active_url.c_str(), nullptr);
  }

  if (adblock_) {
    s.adblock_enabled = adblock_->IsEnabled();
    s.host_paused = !s.active_url.empty() && adblock_->IsHostPaused(s.active_url);
    s.tab_blocked = adblock_->GetBlockedCountForTab(static_cast<int>(s.active_tab_id));
    s.total_blocked = adblock_->GetTotalBlockedCount();
    s.baseline_rules = adblock_->GetBaselineRuleCount();
    s.adblock_rules = adblock_->GetRuleCount();
    s.paused_hosts = adblock_->GetPausedHostCount();
    s.allowlist_hosts = adblock_->GetPausedHosts();
    for (const auto& sp : GetDefaultFilterListSpecs()) {
      ChromeFilterListView fl;
      fl.id = sp.slug;
      fl.name = sp.title;
      fl.url = sp.url;
      fl.rule_count = adblock_->GetRuleCountForList(sp.slug);
      fl.enabled = (fl.rule_count > 0);
      s.filter_lists.push_back(fl);
    }
  }

  for (guint i = 0; i < model_->tabs->len; ++i) {
    const auto* t = static_cast<const KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, i));
    ChromeTabView tv;
    tv.id = t->id;
    tv.title = t->title ? t->title : "";
    tv.url = t->uri ? t->uri : "";
    g_autofree gchar* dom = web_model_extract_host(tv.url.c_str());
    tv.domain = dom ? dom : "";
    tv.group = t->group ? t->group : "";
    if (TabEntry* e = FindEntry(t->id); e && !e->favicon_url.empty()) {
      tv.favicon_url = e->favicon_url;
    } else if (t->favicon_url && t->favicon_url[0] != '\0') {
      tv.favicon_url = t->favicon_url;
    } else {
      tv.favicon_url = DeriveOriginFaviconFallback(tv.url);
    }
    tv.pinned = t->pinned;
    tv.active = (t->id == model_->active_id);
    tv.loading = t->loading;
    tv.audible = t->audible;
    tv.muted = t->muted;
    tv.suspended = t->suspended;
    tv.blocked_count = adblock_ ? adblock_->GetBlockedCountForTab(static_cast<int>(t->id)) : 0;
    s.tabs.push_back(tv);
  }

  if (model_->collapsed_grps) {
    GHashTableIter it;
    gpointer k = nullptr, v = nullptr;
    g_hash_table_iter_init(&it, model_->collapsed_grps);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      if (k) s.collapsed_groups.emplace_back(static_cast<const char*>(k));
    }
  }

  if (profiles_ && profiles_->profiles) {
    for (guint i = 0; i < profiles_->profiles->len; ++i) {
      const auto* p = static_cast<const KajoWebProfileInfo*>(g_ptr_array_index(profiles_->profiles, i));
      ChromeProfileView pv;
      pv.id = p->id ? p->id : "default";
      pv.name = p->name ? p->name : "DEFAULT";
      pv.icon = p->icon ? p->icon : "🌐";
      pv.accent_color = p->accent_color ? p->accent_color : "#0078d4";
      pv.active = (pv.id == cfg_.profile_id);
      s.profiles.push_back(pv);
    }
  }

  s.bookmarks = cached_bookmarks_;
  {
    std::set<std::string> folders;
    for (const auto& bm : cached_bookmarks_) {
      if (!bm.folder.empty()) folders.insert(bm.folder);
      if (!s.active_url.empty() && bm.url == s.active_url) {
        s.active_bookmark = bm;
      }
    }
    s.bookmark_folders.assign(folders.begin(), folders.end());
  }
  if (!pending_permissions_.empty()) {
    const auto& p = pending_permissions_.front();
    s.active_permission = {p.req_id, p.origin, p.label, p.perm_key};
  }
  s.recent_history = cached_recent_history_;
  if (storage_ && !history_query_.empty()) {
    s.history_search_query = history_query_;
    s.history_query_results = history_results_;
  }

  s.downloads_unseen_complete = downloads_unseen_complete_;
  s.download_dir = download_dir_;
  s.downloads_filter = downloads_filter_;
  s.downloads_search_query = downloads_search_query_;
  s.downloads = downloads_;
  for (const auto& pe : permission_store_.ListEntries()) {
    s.saved_permissions.push_back(
        {pe.origin, pe.perm_key, FormatPermissionKeyLabel(pe.perm_key), pe.allow});
  }
  s.autocomplete = ac_results_;
  s.palette_results = palette_results_;

  store_->Update(s);
  const std::string json = store_->SnapshotJson();
  const std::string js = "window.__kajoApply && window.__kajoApply(" + json + ");";

  const bool inf_vis = !pending_permissions_.empty() && !IsFullscreen();
  const bool ov_vis = !overlay_mode_.empty() && !IsFullscreen();
  const bool ac_vis = ac_visible_ && !IsFullscreen();

  auto push_to = [&js](CefRefPtr<CefBrowser> b) {
    if (b && b->GetMainFrame()) {
      b->GetMainFrame()->ExecuteJavaScript(js, b->GetMainFrame()->GetURL(), 0);
    }
  };
  push_to(topbar_browser_);
  push_to(rail_browser_);
  if (inf_vis || last_infobar_visible_) push_to(infobar_browser_);
  if (ov_vis || last_overlay_visible_) push_to(overlay_browser_);
  if (ac_vis || last_ac_visible_) push_to(ac_browser_);
  last_infobar_visible_ = inf_vis;
  last_overlay_visible_ = ov_vis;
  last_ac_visible_ = ac_vis;

  const std::string dl_js = "window.__kajoDlUpdate && window.__kajoDlUpdate(" + json + ");";
  for (const auto& kv : tabs_) {
    KajoWebTabMeta* tm = web_model_find_tab(model_, kv.first);
    if (tm && tm->uri && strncmp(tm->uri, "kajo://downloads", 16) == 0 &&
        kv.second.browser && kv.second.browser->GetMainFrame()) {
      kv.second.browser->GetMainFrame()->ExecuteJavaScript(
          dl_js, kv.second.browser->GetMainFrame()->GetURL(), 0);
    }
  }
}

double KajoBrowserWindow::DefaultZoomLevel() const {
  if (default_zoom_pct_ == 100) return 0.0;
  return log(default_zoom_pct_ / 100.0) / log(1.2);
}

bool KajoBrowserWindow::IsKnownDownloadPath(const std::string& path) const {
  if (path.empty() || path[0] != '/') return false;
  for (const auto& d : downloads_) {
    if (!d.full_path.empty() && d.full_path == path) return true;
  }
  return false;
}

void KajoBrowserWindow::RemoveDownload(uint32_t did, int64_t db_id, bool cancel_active) {
  if (did != 0) {
    dismissed_downloads_.insert(did);
    auto cb_it = download_callbacks_.find(did);
    if (cb_it != download_callbacks_.end()) {
      if (cancel_active && cb_it->second) cb_it->second->Cancel();
      download_callbacks_.erase(cb_it);
    }
    active_download_tabs_.erase(did);
  }
  downloads_.erase(
      std::remove_if(downloads_.begin(), downloads_.end(),
                     [did, db_id](const ChromeDownloadView& d) {
                       if (did != 0 && d.id == did) return true;
                       if (db_id > 0 && d.db_id == db_id) return true;
                       return false;
                     }),
      downloads_.end());
  if (model_) {
    std::set<uint64_t> busy_tabs;
    for (const auto& kv : active_download_tabs_) busy_tabs.insert(kv.second);
    for (guint i = 0; i < model_->tabs->len; ++i) {
      auto* t = static_cast<KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, i));
      t->download_active = busy_tabs.count(t->id) ? TRUE : FALSE;
    }
  }
  if (db_id > 0 && storage_ && !cfg_.private_mode) {
    web_storage_delete_download(storage_, db_id);
  }
}

void KajoBrowserWindow::HandleExternalOpen(const std::vector<std::string>& urls) {
  if (closing_) return;
  for (size_t i = 0; i < urls.size(); ++i) {
    const std::string& raw = urls[i];
    if (raw.empty()) continue;
    std::string target;
    if (raw.rfind("chrome://", 0) == 0 || raw.rfind("devtools://", 0) == 0) {
      target = raw;
    } else {
      g_autofree gchar* resolved = web_model_resolve_input(
          raw.c_str(),
          static_cast<KajoWebSearchEngine>(search_engine_),
          search_custom_url_.empty() ? nullptr : search_custom_url_.c_str());
      target = resolved ? resolved : raw;
    }
    OpenNewTab(target, true);
  }
  if (window_) window_->Activate();
}

bool KajoBrowserWindow::OnQuery(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                int64_t query_id,
                                const CefString& request,
                                bool persistent,
                                CefRefPtr<Callback> callback) {
  if (!frame || frame->GetURL().ToString().rfind("kajo-chrome://", 0) != 0) {
    return false;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  const std::string req_str = request.ToString();
  if (!json_parser_load_from_data(parser, req_str.c_str(), static_cast<gssize>(req_str.size()), nullptr)) {
    callback->Failure(-1, "invalid json");
    return true;
  }
  JsonNode* root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    callback->Failure(-1, "expected object");
    return true;
  }
  JsonObject* obj = json_node_get_object(root);
  const gchar* cmd_c = json_object_has_member(obj, "cmd") ? json_object_get_string_member(obj, "cmd") : "";
  std::string cmd = cmd_c ? cmd_c : "";

  auto get_str = [&](const char* k) -> std::string {
    if (!json_object_has_member(obj, k)) return "";
    const gchar* v = json_object_get_string_member(obj, k);
    return v ? v : "";
  };
  auto get_bool = [&](const char* k) -> bool {
    return json_object_has_member(obj, k) && json_object_get_boolean_member(obj, k);
  };
  auto get_int = [&](const char* k) -> int64_t {
    return json_object_has_member(obj, k) ? json_object_get_int_member(obj, k) : 0;
  };

  if (cmd == "get_state") {
    callback->Success(store_->SnapshotJson());
    return true;
  }
  window_active_ = true;
  window_inactive_since_us_ = 0;
  if (cmd == "back") {
    if (TabEntry* e = ActiveEntry()) { if (e->browser) e->browser->GoBack(); }
  } else if (cmd == "forward") {
    if (TabEntry* e = ActiveEntry()) { if (e->browser) e->browser->GoForward(); }
  } else if (cmd == "reload_or_stop") {
    if (TabEntry* e = ActiveEntry()) {
      if (e->browser) {
        if (e->browser->IsLoading()) e->browser->StopLoad();
        else e->browser->Reload();
      }
    }
  } else if (cmd == "navigate") {
    NavigateActive(get_str("input"));
  } else if (cmd == "new_tab") {
    OpenNewTab(DefaultNewTabUrl(), true);
  } else if (cmd == "activate_tab") {
    ActivateTab(static_cast<uint64_t>(get_int("tab_id")));
  } else if (cmd == "close_tab") {
    CloseTab(static_cast<uint64_t>(get_int("tab_id")));
  } else if (cmd == "reopen_tab") {
    ReopenClosedTab();
  } else if (cmd == "duplicate_tab") {
    DuplicateTab(static_cast<uint64_t>(get_int("tab_id")));
  } else if (cmd == "suspend_tab") {
    if (SuspendTab(static_cast<uint64_t>(get_int("tab_id")), true)) PushStateImmediate();
  } else if (cmd == "suspend_inactive_tabs") {
    if (model_) {
      std::vector<uint64_t> ids;
      for (guint i = 0; i < model_->tabs->len; ++i) {
        ids.push_back(static_cast<const KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, i))->id);
      }
      bool any = false;
      for (uint64_t tid : ids) {
        if (SuspendTab(tid, true)) any = true;
      }
      if (any) PushStateImmediate();
    }
  } else if (cmd == "close_other_tabs") {
    uint64_t keep_id = static_cast<uint64_t>(get_int("tab_id"));
    if (model_ && keep_id) {
      std::vector<uint64_t> others;
      for (guint i = 0; i < model_->tabs->len; ++i) {
        uint64_t tid = static_cast<const KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, i))->id;
        if (tid != keep_id) others.push_back(tid);
      }
      for (uint64_t tid : others) CloseTab(tid);
    }
  } else if (cmd == "set_tab_group") {
    uint64_t tid = static_cast<uint64_t>(get_int("tab_id"));
    if (tid == 0) tid = group_prompt_tab_id_;
    const std::string grp = get_str("group");
    if (KajoWebTabMeta* t = web_model_find_tab(model_, tid)) {
      g_free(t->group);
      t->group = grp.empty() ? nullptr : g_strdup(grp.c_str());
      PersistSession();
      PushStateImmediate();
    }
    if (overlay_mode_ == "new_group") CloseOverlay();
  } else if (cmd == "open_group_prompt") {
    group_prompt_tab_id_ = static_cast<uint64_t>(get_int("tab_id"));
    if (group_prompt_tab_id_) ToggleOverlay("new_group");
  } else if (cmd == "move_tab") {
    uint64_t tid = static_cast<uint64_t>(get_int("tab_id"));
    uint64_t target_id = static_cast<uint64_t>(get_int("target_id"));
    const bool after = get_bool("after");
    const std::string grp = get_str("group");
    KajoWebTabMeta* src = web_model_find_tab(model_, tid);
    KajoWebTabMeta* dst = web_model_find_tab(model_, target_id);
    if (model_ && src && dst && tid != target_id) {
      if (!src->pinned && !dst->pinned) {
        g_free(src->group);
        src->group = grp.empty() ? nullptr : g_strdup(grp.c_str());
      }
      int src_idx = -1, dst_idx = -1;
      for (guint i = 0; i < model_->tabs->len; ++i) {
        auto* cand = static_cast<KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, i));
        if (cand->id == tid) src_idx = static_cast<int>(i);
        if (cand->id == target_id) dst_idx = static_cast<int>(i);
      }
      if (src_idx >= 0 && dst_idx >= 0) {
        int ti = dst_idx;
        if (src_idx < dst_idx) ti = after ? dst_idx : (dst_idx - 1);
        else ti = after ? (dst_idx + 1) : dst_idx;
        ti = std::max(0, ti);
        web_model_move_tab(model_, tid, static_cast<guint>(ti));
        PersistSession();
        PushStateImmediate();
      }
    }
  } else if (cmd == "toggle_pin") {
    uint64_t tid = static_cast<uint64_t>(get_int("tab_id"));
    if (KajoWebTabMeta* t = web_model_find_tab(model_, tid)) {
      web_model_set_pinned(model_, tid, !t->pinned);
      PersistSession();
      PushStateImmediate();
    }
  } else if (cmd == "toggle_mute") {
    uint64_t tid = static_cast<uint64_t>(get_int("tab_id"));
    if (KajoWebTabMeta* t = web_model_find_tab(model_, tid)) {
      t->muted = !t->muted;
      if (TabEntry* e = FindEntry(tid)) {
        if (e->browser && e->browser->GetHost()) e->browser->GetHost()->SetAudioMuted(t->muted);
      }
      PushStateImmediate();
    }
  } else if (cmd == "toggle_group") {
    const std::string g = get_str("group");
    if (!g.empty()) {
      bool cur = web_model_is_group_collapsed(model_, g.c_str());
      web_model_set_group_collapsed(model_, g.c_str(), !cur);
      PushStateToChrome();
    }
  } else if (cmd == "rail_hold") {
    const bool hold = get_bool("hold");
    if (hold) ++rail_hold_count_;
    else if (rail_hold_count_ > 0) --rail_hold_count_;
  } else if (cmd == "toggle_rail") {
    if (rail_mode_ == KAJO_WEB_RAIL_PINNED) {
      rail_mode_ = KAJO_WEB_RAIL_COMPACT;
      rail_hover_open_ = false;
      ++rail_hover_gen_;
      PersistRailMode(rail_mode_);
    } else {
      rail_hover_open_ = !rail_hover_open_;
      ++rail_hover_gen_;
    }
    ApplyRailSizing();
    PushStateImmediate();
  } else if (cmd == "cycle_rail_mode") {
    rail_mode_ = (rail_mode_ + 1) % 3;
    rail_hover_open_ = false;
    ++rail_hover_gen_;
    PersistRailMode(rail_mode_);
    ApplyRailSizing();
    PushStateImmediate();
  } else if (cmd == "rail_hover") {
    if (rail_mode_ == KAJO_WEB_RAIL_AUTOHIDE || rail_mode_ == KAJO_WEB_RAIL_COMPACT) {
      const bool inside = get_bool("inside");
      const uint64_t gen = ++rail_hover_gen_;
      const int delay_ms = inside ? rail_reveal_delay_ms_ : rail_hide_delay_ms_;
      CefRefPtr<KajoBrowserWindow> self(this);
      CefPostDelayedTask(TID_UI, new UiTask([self, gen, inside]() {
        if (self->closing_ || self->rail_hover_gen_ != gen) return;
        if (self->rail_mode_ != KAJO_WEB_RAIL_AUTOHIDE &&
            self->rail_mode_ != KAJO_WEB_RAIL_COMPACT) {
          return;
        }
        if (!inside && self->rail_hold_count_ > 0) return;
        if (self->rail_hover_open_ != inside) {
          self->rail_hover_open_ = inside;
          self->ApplyRailSizing();
          self->PushStateImmediate();
        }
      }), delay_ms);
    }
  } else if (cmd == "topbar_hover") {
    if (topbar_mode_ == KAJO_WEB_TOPBAR_AUTOHIDE) {
      const bool inside = get_bool("inside");
      const uint64_t gen = ++topbar_hover_gen_;
      const int delay_ms = inside ? std::min(rail_reveal_delay_ms_, 60) : rail_hide_delay_ms_;
      CefRefPtr<KajoBrowserWindow> self(this);
      CefPostDelayedTask(TID_UI, new UiTask([self, gen, inside]() {
        if (self->closing_ || self->topbar_hover_gen_ != gen) return;
        if (self->topbar_mode_ != KAJO_WEB_TOPBAR_AUTOHIDE) return;
        if (!inside && (self->topbar_focused_ || self->ac_visible_ || !self->overlay_mode_.empty())) return;
        if (self->topbar_hover_open_ != inside) {
          self->topbar_hover_open_ = inside;
          self->ApplyTopbarSizing();
          self->PushStateImmediate();
        }
      }), delay_ms);
    }
  } else if (cmd == "topbar_focus") {
    topbar_focused_ = get_bool("focused");
    if (topbar_focused_ && topbar_mode_ == KAJO_WEB_TOPBAR_AUTOHIDE) {
      topbar_hover_open_ = true;
      ++topbar_hover_gen_;
    }
    ApplyTopbarSizing();
    PushStateImmediate();
  } else if (cmd == "toggle_bookmark") {
    OpenBookmarkPopover();
  } else if (cmd == "save_bookmark") {
    KajoWebTabMeta* t = web_model_get_active(model_);
    if (t && t->uri && storage_) {
      std::string uuid = get_str("uuid");
      if (uuid.empty()) {
        g_autofree gchar* found_uuid = nullptr;
        if (web_storage_is_bookmarked(storage_, t->uri, &found_uuid) && found_uuid) {
          uuid = found_uuid;
        } else {
          g_autofree gchar* new_u = g_uuid_string_random();
          uuid = new_u ? new_u : "";
        }
      }
      const std::string title = get_str("title");
      const std::string folder = get_str("folder");
      KajoWebBookmark* bm = web_bookmark_new(
          uuid.c_str(), title.empty() ? t->uri : title.c_str(),
          t->uri, folder.c_str(), g_get_real_time() / 1000, FALSE);
      web_storage_upsert_bookmark(storage_, bm);
      web_bookmark_free(bm);
      RefreshStorageCache();
    }
    CloseOverlay();
  } else if (cmd == "delete_bookmark") {
    const std::string uuid = get_str("uuid");
    if (!uuid.empty() && storage_) {
      web_storage_delete_bookmark(storage_, uuid.c_str());
      RefreshStorageCache();
    }
    CloseOverlay();
  } else if (cmd == "perm_respond") {
    RespondActivePermission(static_cast<uint64_t>(get_int("req_id")), get_str("decision"));
  } else if (cmd == "toggle_overlay") {
    ToggleOverlay(get_str("mode"));
  } else if (cmd == "close_overlay") {
    CloseOverlay();
  } else if (cmd == "close_overlay_if_mode") {
    if (overlay_mode_ == get_str("mode")) CloseOverlay();
  } else if (cmd == "focus_content") {
    if (TabEntry* e = ActiveEntry()) { if (e->view) e->view->RequestFocus(); }
  } else if (cmd == "ac_query") {
    BuildAutocomplete(get_str("query"));
  } else if (cmd == "ac_step") {
    int idx = static_cast<int>(get_int("index"));
    if (!ac_results_.empty()) {
      ac_selected_idx_ = std::clamp(idx, -1, static_cast<int>(ac_results_.size()) - 1);
      PushStateImmediate();
    }
  } else if (cmd == "ac_accept") {
    int idx = static_cast<int>(get_int("index"));
    if (idx >= 0 && idx < static_cast<int>(ac_results_.size())) {
      const auto it = ac_results_[static_cast<size_t>(idx)];
      HideAutocomplete();
      if (it.action == "activate_tab") ActivateTab(it.tab_id);
      else if (it.action == "navigate") NavigateActive(it.payload);
    }
  } else if (cmd == "ac_close") {
    HideAutocomplete();
  } else if (cmd == "palette_query") {
    BuildPalette(get_str("query"));
    PushStateImmediate();
  } else if (cmd == "overlay_step") {
    if (overlay_browser_ && overlay_browser_->GetMainFrame()) {
      std::string js = "window.__kajoOverlayStep && window.__kajoOverlayStep(" +
                       std::to_string(get_int("delta")) + ");";
      overlay_browser_->GetMainFrame()->ExecuteJavaScript(js, "", 0);
    }
  } else if (cmd == "run_suggestion") {
    const std::string act = get_str("action");
    const std::string pay = get_str("payload");
    uint64_t tid = static_cast<uint64_t>(get_int("tab_id"));
    HideAutocomplete();
    CloseOverlay();
    if (act == "activate_tab") ActivateTab(tid);
    else if (act == "navigate") NavigateActive(pay);
    else if (act == "new_tab") OpenNewTab(DefaultNewTabUrl(), true);
    else if (act == "close_active_tab") OnAccelerator(window_, ACCEL_CLOSE_TAB);
    else if (act == "reopen_tab") OnAccelerator(window_, ACCEL_REOPEN_TAB);
    else if (act == "duplicate_active_tab") { if (model_ && model_->active_id) DuplicateTab(model_->active_id); }
    else if (act == "focus_omnibar") OnAccelerator(window_, ACCEL_FOCUS_OMNIBAR);
    else if (act == "hard_reload") OnAccelerator(window_, ACCEL_HARD_RELOAD);
    else if (act == "mute_active_tab") OnAccelerator(window_, ACCEL_MUTE_TAB);
    else if (act == "pin_active_tab") {
      if (model_ && model_->active_id) {
        if (KajoWebTabMeta* t = web_model_find_tab(model_, model_->active_id)) {
          web_model_set_pinned(model_, t->id, !t->pinned);
          PersistSession();
          PushStateImmediate();
        }
      }
    } else if (act == "suspend_inactive_tabs") {
      if (model_) {
        std::vector<uint64_t> ids;
        for (guint i = 0; i < model_->tabs->len; ++i) {
          ids.push_back(static_cast<const KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, i))->id);
        }
        bool any = false;
        for (uint64_t id : ids) { if (SuspendTab(id, true)) any = true; }
        if (any) PushStateImmediate();
      }
    } else if (act == "toggle_topbar_mode") {
      OnAccelerator(window_, ACCEL_TOGGLE_TOPBAR);
    } else if (act == "open_bookmark_popover") {
      OpenBookmarkPopover();
    } else if (act == "open_find") {
      ToggleOverlay("find");
    } else if (act == "open_downloads_overlay") {
      ToggleOverlay("downloads");
    } else if (act == "open_profiles_overlay") {
      ToggleOverlay("profiles");
    } else if (act == "toggle_rail") {
      OnAccelerator(window_, ACCEL_TOGGLE_RAIL);
    } else if (act == "cycle_rail_mode") {
      rail_mode_ = (rail_mode_ + 1) % 3;
      rail_hover_open_ = false;
      ++rail_hover_gen_;
      PersistRailMode(rail_mode_);
      ApplyRailSizing();
      PushStateImmediate();
    } else if (act == "open_kajo_page") {
      NavigateActive(pay);
    } else if (act == "open_settings") {
      OnAccelerator(window_, ACCEL_SETTINGS);
    } else if (act == "toggle_float") {
      niri_toggle_window_floating();
    } else if (act == "launch_private") {
      OnAccelerator(window_, ACCEL_PRIVATE_WINDOW);
    }
  } else if (cmd == "find_query") {
    find_query_ = get_str("query");
    if (TabEntry* e = ActiveEntry()) {
      if (e->browser && e->browser->GetHost()) {
        if (find_query_.empty()) e->browser->GetHost()->StopFinding(true);
        else e->browser->GetHost()->Find(find_query_, true, find_match_case_, false);
      }
    }
    PushStateToChrome();
  } else if (cmd == "find_step") {
    if (TabEntry* e = ActiveEntry()) {
      if (e->browser && e->browser->GetHost() && !find_query_.empty()) {
        e->browser->GetHost()->Find(find_query_, get_bool("forward"), find_match_case_, true);
      }
    }
  } else if (cmd == "find_toggle_case") {
    find_match_case_ = !find_match_case_;
    if (TabEntry* e = ActiveEntry()) {
      if (e->browser && e->browser->GetHost() && !find_query_.empty()) {
        e->browser->GetHost()->Find(find_query_, true, find_match_case_, false);
      }
    }
    PushStateImmediate();
  } else if (cmd == "toggle_host_adblock") {
    if (KajoWebTabMeta* t = web_model_get_active(model_)) {
      if (t->uri && adblock_) {
        adblock_->ToggleHostPaused(t->uri);
        if (TabEntry* e = ActiveEntry()) {
          if (e->browser) e->client->RefreshCosmeticInjection(e->browser, e->browser->GetMainFrame());
        }
        PushStateImmediate();
      }
    }
  } else if (cmd == "update_filter_lists") {
    StartFilterListUpdates();
  } else if (cmd == "open_kajo_page") {
    NavigateActive(get_str("page"));
  } else if (cmd == "play_in_kajo_player") {
    const std::string uri = get_str("uri");
    if (uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0 ||
        uri.rfind("file://", 0) == 0) {
      const char* argv[] = {"kajo-player", "--", uri.c_str(), nullptr};
      kajo_spawn_detached_argv(nullptr, argv, nullptr, nullptr);
    }
  } else if (cmd == "download_media_uri") {
    const std::string uri = get_str("uri");
    if (!uri.empty()) {
      if (TabEntry* e = ActiveEntry()) {
        if (e->browser && e->browser->GetHost()) e->browser->GetHost()->StartDownload(uri);
      }
    }
  } else if (cmd == "pause_download") {
    uint32_t did = static_cast<uint32_t>(get_int("id"));
    auto it = download_callbacks_.find(did);
    if (it != download_callbacks_.end() && it->second) {
      it->second->Pause();
      for (auto& d : downloads_) {
        if (d.id == did) { d.paused = true; break; }
      }
      PushStateImmediate();
    }
  } else if (cmd == "resume_download") {
    uint32_t did = static_cast<uint32_t>(get_int("id"));
    auto it = download_callbacks_.find(did);
    if (it != download_callbacks_.end() && it->second) {
      it->second->Resume();
      for (auto& d : downloads_) {
        if (d.id == did) { d.paused = false; break; }
      }
      PushStateImmediate();
    }
  } else if (cmd == "cancel_download") {
    uint32_t did = static_cast<uint32_t>(get_int("id"));
    auto it = download_callbacks_.find(did);
    if (it != download_callbacks_.end() && it->second) {
      it->second->Cancel();
    }
  } else if (cmd == "open_download_file") {
    const std::string path = get_str("path");
    if (IsKnownDownloadPath(path) && g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
      g_autofree gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
      g_autoptr(GError) err = nullptr;
      if (!uri || !g_app_info_launch_default_for_uri(uri, nullptr, &err)) {
        const char* argv[] = {"xdg-open", path.c_str(), nullptr};
        kajo_spawn_detached_argv(nullptr, argv, nullptr, nullptr);
      }
    }
  } else if (cmd == "open_download_folder") {
    const std::string path = get_str("path");
    g_autofree gchar* dir = nullptr;
    if (path.empty()) {
      dir = g_strdup(download_dir_.c_str());
    } else if (IsKnownDownloadPath(path)) {
      dir = g_path_get_dirname(path.c_str());
    }
    if (dir && dir[0] != '\0') {
      const char* argv[] = {"kajo-files", "--", dir, nullptr};
      kajo_spawn_detached_argv(nullptr, argv, nullptr, nullptr);
    }
  } else if (cmd == "retry_download") {
    const std::string url = get_str("url");
    if (!url.empty()) {
      if (TabEntry* e = ActiveEntry()) {
        if (e->browser && e->browser->GetHost()) e->browser->GetHost()->StartDownload(url);
      }
    }
  } else if (cmd == "remove_download") {
    uint32_t did = static_cast<uint32_t>(get_int("id"));
    int64_t db_id = get_int("db_id");
    RemoveDownload(did, db_id, true);
    PushStateImmediate();
  } else if (cmd == "clear_finished_downloads") {
    downloads_.erase(
        std::remove_if(downloads_.begin(), downloads_.end(),
                       [](const ChromeDownloadView& d) { return d.complete || d.canceled; }),
        downloads_.end());
    if (storage_ && !cfg_.private_mode) {
      web_storage_clear_downloads(storage_);
    }
    PushStateImmediate();
  } else if (cmd == "launch_profile") {
    const std::string pid = get_str("id");
    const bool priv = get_bool("private_mode");
    if (!priv && !pid.empty() && profiles_) {
      g_free(profiles_->last_profile_id);
      profiles_->last_profile_id = g_strdup(pid.c_str());
      g_autofree gchar* reg_dir = g_build_filename(
          g_get_user_data_dir(), "kajo", "web", nullptr);
      g_autofree gchar* reg_path = g_build_filename(reg_dir, "profiles.toml", nullptr);
      g_mkdir_with_parents(reg_dir, 0700);
      web_profile_registry_save(profiles_, reg_path, nullptr);
    }
    std::vector<const char*> argv = {cfg_.exe_path.c_str()};
    if (!pid.empty()) { argv.push_back("--profile"); argv.push_back(pid.c_str()); }
    if (priv) argv.push_back("--private");
    argv.push_back(nullptr);
    kajo_spawn_detached_argv(nullptr, argv.data(), nullptr, nullptr);
    CloseOverlay();
  } else if (cmd == "create_profile") {
    const std::string name = get_str("name");
    if (!name.empty() && profiles_) {
      web_profile_registry_add(profiles_, name.c_str(), "🌐", accent_color_.c_str());
      g_autofree gchar* reg_path = g_build_filename(
          g_get_user_data_dir(), "kajo", "web", "profiles.toml", nullptr);
      web_profile_registry_save(profiles_, reg_path, nullptr);
      PushStateImmediate();
    }
  } else if (cmd == "rename_profile") {
    const std::string pid = get_str("id");
    const std::string name = get_str("name");
    if (!pid.empty() && !name.empty() && profiles_) {
      if (KajoWebProfileInfo* p = web_profile_registry_find(profiles_, pid.c_str())) {
        g_free(p->name);
        p->name = g_utf8_strup(name.c_str(), -1);
        g_autofree gchar* reg_path = g_build_filename(
            g_get_user_data_dir(), "kajo", "web", "profiles.toml", nullptr);
        web_profile_registry_save(profiles_, reg_path, nullptr);
        if (pid == cfg_.profile_id && p->name) profile_name_ = p->name;
        PushStateImmediate();
      }
    }
  } else if (cmd == "delete_profile") {
    const std::string pid = get_str("id");
    if (IsSafeProfileSlug(pid) && pid != cfg_.profile_id && profiles_ &&
        !IsProfileInstanceRunning(pid)) {
      if (web_profile_registry_remove(profiles_, pid.c_str())) {
        g_autofree gchar* reg_path = g_build_filename(
            g_get_user_data_dir(), "kajo", "web", "profiles.toml", nullptr);
        web_profile_registry_save(profiles_, reg_path, nullptr);
        g_autofree gchar* pdata = g_build_filename(
            g_get_user_data_dir(), "kajo", "web-cef", pid.c_str(), nullptr);
        g_autofree gchar* pcache = g_build_filename(
            g_get_user_cache_dir(), "kajo", "web-cef", pid.c_str(), nullptr);
        std::string pdata_s = pdata ? pdata : "";
        std::string pcache_s = pcache ? pcache : "";
        CefPostTask(TID_FILE_USER_VISIBLE, new UiTask([pdata_s, pcache_s]() {
          std::error_code ec;
          if (!pdata_s.empty()) std::filesystem::remove_all(pdata_s, ec);
          if (!pcache_s.empty()) std::filesystem::remove_all(pcache_s, ec);
        }));
        PushStateImmediate();
      }
    }
  } else if (cmd == "zoom_reset") {
    if (TabEntry* e = ActiveEntry()) {
      if (e->browser && e->browser->GetHost()) {
        e->browser->GetHost()->SetZoomLevel(DefaultZoomLevel());
      }
      PushStateImmediate();
    }
  } else if (cmd == "open_settings") {
    const char* argv[] = {"kajo-settings", "--page", "web", nullptr};
    kajo_spawn_detached_argv(nullptr, argv, nullptr, nullptr);
  } else if (cmd == "toggle_float") {
    niri_toggle_window_floating();
  } else if (cmd == "close_window") {
    if (window_) window_->Close();
  }

  callback->Success("{}");
  return true;
}

bool KajoBrowserWindow::OnAccelerator(CefRefPtr<CefWindow> window, int command_id) {
  switch (command_id) {
    case ACCEL_NEW_TAB:
      OpenNewTab(DefaultNewTabUrl(), true);
      return true;
    case ACCEL_CLOSE_TAB:
      if (model_ && model_->active_id) CloseTab(model_->active_id);
      return true;
    case ACCEL_REOPEN_TAB:
      ReopenClosedTab();
      return true;
    case ACCEL_FOCUS_OMNIBAR:
      FocusOmnibar();
      return true;
    case ACCEL_PALETTE:
      ToggleOverlay("palette");
      return true;
    case ACCEL_FIND:
      ToggleOverlay("find");
      return true;
    case ACCEL_RELOAD:
      if (TabEntry* e = ActiveEntry()) { if (e->browser) e->browser->Reload(); }
      return true;
    case ACCEL_HARD_RELOAD:
      if (TabEntry* e = ActiveEntry()) { if (e->browser) e->browser->ReloadIgnoreCache(); }
      return true;
    case ACCEL_TOGGLE_RAIL:
      if (rail_mode_ == KAJO_WEB_RAIL_PINNED) {
        rail_mode_ = KAJO_WEB_RAIL_COMPACT;
        rail_hover_open_ = false;
        ++rail_hover_gen_;
        PersistRailMode(rail_mode_);
      } else {
        rail_hover_open_ = !rail_hover_open_;
        ++rail_hover_gen_;
      }
      ApplyRailSizing();
      PushStateImmediate();
      return true;
    case ACCEL_TOGGLE_TOPBAR:
      topbar_mode_ = (topbar_mode_ == KAJO_WEB_TOPBAR_ALWAYS)
                         ? KAJO_WEB_TOPBAR_AUTOHIDE
                         : KAJO_WEB_TOPBAR_ALWAYS;
      topbar_hover_open_ = false;
      ++topbar_hover_gen_;
      PersistTopbarMode(topbar_mode_);
      RebuildLayout();
      PushStateImmediate();
      return true;
    case ACCEL_BOOKMARK:
      OpenBookmarkPopover();
      return true;
    case ACCEL_MUTE_TAB:
      if (model_ && model_->active_id) {
        if (KajoWebTabMeta* t = web_model_find_tab(model_, model_->active_id)) {
          t->muted = !t->muted;
          if (TabEntry* e = FindEntry(t->id)) {
            if (e->browser && e->browser->GetHost()) {
              e->browser->GetHost()->SetAudioMuted(t->muted);
            }
          }
          PushStateImmediate();
        }
      }
      return true;
    case ACCEL_DOWNLOADS:
      ToggleOverlay("downloads");
      return true;
    case ACCEL_PROFILES:
      ToggleOverlay("profiles");
      return true;
    case ACCEL_PRIVATE_WINDOW: {
      const char* argv[] = {cfg_.exe_path.c_str(), "--private", nullptr};
      kajo_spawn_detached_argv(nullptr, argv, nullptr, nullptr);
      return true;
    }
    case ACCEL_SETTINGS: {
      const char* argv[] = {"kajo-settings", "--page", "web", nullptr};
      kajo_spawn_detached_argv(nullptr, argv, nullptr, nullptr);
      return true;
    }
    case ACCEL_FULLSCREEN:
      user_fullscreen_ = !user_fullscreen_;
      if (window_) {
        window_->SetFullscreen(IsFullscreen());
      }
      ApplyDraggableRegions();
      RebuildLayout();
      UpdateOverlayBounds();
      return true;
    case ACCEL_DEVTOOLS:
      if (!developer_extras_) return true;
      if (TabEntry* e = ActiveEntry()) {
        if (e->browser && e->browser->GetHost()) {
          CefWindowInfo wi;
          CefBrowserSettings bs;
          e->browser->GetHost()->ShowDevTools(wi, nullptr, bs, CefPoint());
        }
      }
      return true;
    default:
      return false;
  }
}

bool KajoBrowserWindow::OnTabShortcut(uint64_t tab_id, const CefKeyEvent& event) {
  if (event.type != KEYEVENT_RAWKEYDOWN) return false;
  const bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
  const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
  const bool alt = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;
  const int vk = event.windows_key_code;

  if (ctrl && !alt) {
    if (vk == 'T') return OnAccelerator(window_, shift ? ACCEL_REOPEN_TAB : ACCEL_NEW_TAB);
    if (vk == 'W' && !shift) return OnAccelerator(window_, ACCEL_CLOSE_TAB);
    if (vk == 'L' && !shift) return OnAccelerator(window_, ACCEL_FOCUS_OMNIBAR);
    if (vk == 'K' && !shift) return OnAccelerator(window_, ACCEL_PALETTE);
    if (vk == 'F' && !shift) return OnAccelerator(window_, ACCEL_FIND);
    if (vk == 'R') return OnAccelerator(window_, shift ? ACCEL_HARD_RELOAD : ACCEL_RELOAD);
    if (vk == 0x74 /*Ctrl+F5*/ && !shift) return OnAccelerator(window_, ACCEL_HARD_RELOAD);
    if (vk == 'B' && !shift) return OnAccelerator(window_, ACCEL_TOGGLE_RAIL);
    if (vk == 'U' && shift) return OnAccelerator(window_, ACCEL_TOGGLE_TOPBAR);
    if (vk == 'D' && !shift) return OnAccelerator(window_, ACCEL_BOOKMARK);
    if (vk == 'M') return OnAccelerator(window_, shift ? ACCEL_PROFILES : ACCEL_MUTE_TAB);
    if (vk == 'J' && !shift) return OnAccelerator(window_, ACCEL_DOWNLOADS);
    if (vk == 'N' && shift) return OnAccelerator(window_, ACCEL_PRIVATE_WINDOW);
    if (vk == 0xBC /*VK_OEM_COMMA*/ && !shift) return OnAccelerator(window_, ACCEL_SETTINGS);
    if (vk == 0x09 /*VK_TAB*/) {
      if (model_) {
        guint64 next_id = web_model_cycle_visual(model_, !shift);
        if (next_id) {
          ActivateTab(next_id);
          PeekRailTemporarily(1200);
        }
      }
      return true;
    }
    if (vk == 0xBB /*'='*/ || vk == '0' || vk == 0xBD /*'-'*/) {
      if (TabEntry* e = ActiveEntry()) {
        if (e->browser && e->browser->GetHost()) {
          double cur = e->browser->GetHost()->GetZoomLevel();
          if (vk == '0') cur = DefaultZoomLevel();
          else if (vk == 0xBB) cur += 0.5;
          else cur -= 0.5;
          e->browser->GetHost()->SetZoomLevel(cur);
          PushStateImmediate();
        }
      }
      return true;
    }
  }
  if (alt && !ctrl && !shift) {
    if (vk == 'D') return OnAccelerator(window_, ACCEL_FOCUS_OMNIBAR);
    if (vk >= '1' && vk <= '9' && model_ && model_->tabs->len > 0) {
      guint idx = (vk == '9') ? (model_->tabs->len - 1) : static_cast<guint>(vk - '1');
      if (idx < model_->tabs->len) {
        const auto* t = static_cast<const KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, idx));
        if (t) {
          ActivateTab(t->id);
          PeekRailTemporarily(1200);
        }
      }
      return true;
    }
    if (vk == 0x25 /*LEFT*/ || vk == 0x27 /*RIGHT*/) {
      if (TabEntry* e = ActiveEntry()) {
        if (e->browser) {
          if (vk == 0x25) e->browser->GoBack();
          else e->browser->GoForward();
        }
      }
      return true;
    }
  }
  if (!ctrl && !alt && !shift) {
    if (vk == 0x74 /*F5*/) return OnAccelerator(window_, ACCEL_RELOAD);
    if (vk == 0x7A /*F11*/) return OnAccelerator(window_, ACCEL_FULLSCREEN);
    if (vk == 0x7B /*F12*/) return OnAccelerator(window_, ACCEL_DEVTOOLS);
  }
  if (vk == 0x1B /*ESC*/) {
    if (ac_visible_) {
      HideAutocomplete();
      return true;
    }
    if (rail_hold_count_ > 0) {
      rail_hold_count_ = 0;
      if (rail_browser_ && rail_browser_->GetMainFrame()) {
        rail_browser_->GetMainFrame()->ExecuteJavaScript(
            "window.__kajoCloseCtxMenu && window.__kajoCloseCtxMenu();", "", 0);
      }
      return true;
    }
    if (!overlay_mode_.empty()) {
      CloseOverlay();
      return true;
    }
    if (rail_hover_open_ &&
        (rail_mode_ == KAJO_WEB_RAIL_AUTOHIDE || rail_mode_ == KAJO_WEB_RAIL_COMPACT)) {
      rail_hover_open_ = false;
      ++rail_hover_gen_;
      ApplyRailSizing();
      PushStateImmediate();
      return true;
    }
    if (topbar_hover_open_ && topbar_mode_ == KAJO_WEB_TOPBAR_AUTOHIDE) {
      topbar_hover_open_ = false;
      topbar_focused_ = false;
      ++topbar_hover_gen_;
      ApplyTopbarSizing();
      PushStateImmediate();
      return true;
    }
    if (user_fullscreen_) {
      return OnAccelerator(window_, ACCEL_FULLSCREEN);
    }
  }
  return false;
}

void KajoBrowserWindow::OnTabGotFocus(uint64_t tab_id) {
  window_active_ = true;
  window_inactive_since_us_ = 0;
  HideAutocomplete();
  if (!overlay_mode_.empty() && overlay_mode_ != "find") {
    CloseOverlay();
  }
  if (rail_hover_open_ && rail_hold_count_ == 0 &&
      (rail_mode_ == KAJO_WEB_RAIL_AUTOHIDE || rail_mode_ == KAJO_WEB_RAIL_COMPACT)) {
    rail_hover_open_ = false;
    ++rail_hover_gen_;
    ApplyRailSizing();
    PushStateImmediate();
  }
  if (topbar_hover_open_ && topbar_mode_ == KAJO_WEB_TOPBAR_AUTOHIDE) {
    topbar_hover_open_ = false;
    topbar_focused_ = false;
    ++topbar_hover_gen_;
    ApplyTopbarSizing();
    PushStateImmediate();
  }
}

void KajoBrowserWindow::OnTabBrowserCreated(uint64_t tab_id, CefRefPtr<CefBrowser> browser) {
  if (TabEntry* e = FindEntry(tab_id)) {
    e->browser = browser;
    if (browser && browser->GetHost()) {
      if (default_zoom_pct_ != 100) {
        browser->GetHost()->SetZoomLevel(DefaultZoomLevel());
      }
      if (KajoWebTabMeta* t = web_model_find_tab(model_, tab_id)) {
        if (t->muted) browser->GetHost()->SetAudioMuted(true);
      }
    }
  }
}

void KajoBrowserWindow::OnTabBrowserClosing(uint64_t tab_id) {
  if (TabEntry* e = FindEntry(tab_id)) e->browser = nullptr;
}

void KajoBrowserWindow::OnTabAddressChanged(uint64_t tab_id, const std::string& url) {
  if (!model_) return;
  if (url.rfind("kajo://crashed", 0) == 0) return;
  if (url.rfind("kajo://downloads", 0) == 0) {
    downloads_unseen_complete_ = false;
  }
  bool host_changed = false;
  if (KajoWebTabMeta* t = web_model_find_tab(model_, tab_id)) {
    g_autofree gchar* old_host = t->uri ? web_model_extract_host(t->uri) : nullptr;
    g_autofree gchar* new_host = web_model_extract_host(url.c_str());
    host_changed = (g_strcmp0(old_host, new_host) != 0);
    g_free(t->uri);
    t->uri = g_strdup(url.c_str());
    if (host_changed) {
      web_model_set_tab_favicon(model_, tab_id, nullptr);
    }
  }
  if (TabEntry* e = FindEntry(tab_id)) {
    e->media.clear();
    e->last_recorded_visit_uri.clear();
    if (host_changed) e->favicon_url.clear();
  }
  PersistSession();
  PushStateToChrome();
}

void KajoBrowserWindow::OnTabTitleChanged(uint64_t tab_id, const std::string& title) {
  if (!model_) return;
  if (KajoWebTabMeta* t = web_model_find_tab(model_, tab_id)) {
    g_free(t->title);
    t->title = g_strdup(title.c_str());
    if (!cfg_.private_mode && storage_ && t->uri &&
        (strncmp(t->uri, "http://", 7) == 0 || strncmp(t->uri, "https://", 8) == 0)) {
      TabEntry* e = FindEntry(tab_id);
      const std::string cur_uri = t->uri;
      if (e && e->last_recorded_visit_uri != cur_uri) {
        web_storage_record_visit(storage_, t->uri, title.c_str());
        e->last_recorded_visit_uri = cur_uri;
        RefreshStorageCache();
      } else {
        web_storage_update_title(storage_, t->uri, title.c_str());
      }
    }
  }
  PushStateToChrome();
}

void KajoBrowserWindow::OnTabFaviconChanged(uint64_t tab_id, const std::string& favicon_url) {
  bool changed = false;
  if (KajoWebTabMeta* t = web_model_find_tab(model_, tab_id)) {
    const char* cur = t->favicon_url ? t->favicon_url : "";
    if (g_strcmp0(cur, favicon_url.c_str()) != 0) {
      web_model_set_tab_favicon(model_, tab_id, favicon_url.c_str());
      changed = true;
    }
  }
  if (TabEntry* e = FindEntry(tab_id)) {
    if (e->favicon_url != favicon_url) {
      e->favicon_url = favicon_url;
      changed = true;
    }
  }
  if (changed) {
    if (!cfg_.private_mode) PersistSession();
    PushStateToChrome();
  }
}

void KajoBrowserWindow::OnTabLoadingChanged(uint64_t tab_id,
                                            bool loading,
                                            bool can_back,
                                            bool can_forward) {
  if (KajoWebTabMeta* t = web_model_find_tab(model_, tab_id)) {
    t->loading = loading;
  }
  if (TabEntry* e = FindEntry(tab_id)) {
    e->can_go_back = can_back;
    e->can_go_forward = can_forward;
  }
  PushStateToChrome();
}

void KajoBrowserWindow::OnTabFullscreenChanged(uint64_t tab_id, bool fullscreen) {
  if (model_ && tab_id == model_->active_id) {
    html_fullscreen_ = fullscreen;
    if (window_) {
      window_->SetFullscreen(IsFullscreen());
    }
    ApplyDraggableRegions();
    RebuildLayout();
    UpdateOverlayBounds();
  }
}

void KajoBrowserWindow::OnTabAudioStateChanged(uint64_t tab_id, bool audible) {
  if (KajoWebTabMeta* t = web_model_find_tab(model_, tab_id)) {
    t->audible = audible;
    PushStateToChrome();
  }
}

void KajoBrowserWindow::OnTabOpenUrl(uint64_t from_tab_id, const std::string& url, bool foreground) {
  std::string grp;
  if (KajoWebTabMeta* src = web_model_find_tab(model_, from_tab_id)) {
    if (src->group) grp = src->group;
  }
  OpenNewTab(url, foreground, grp);
}

bool KajoBrowserWindow::OnTabBeforeBrowseAction(uint64_t tab_id,
                                                const std::string& url,
                                                const std::string& prev_url,
                                                bool user_gesture) {
  size_t qpos = url.find('?');
  if (qpos == std::string::npos) return false;
  std::string base = url.substr(0, qpos);
  while (!base.empty() && base.back() == '/') base.pop_back();
  const std::string query = url.substr(qpos + 1);
  const std::string act = GetQueryParam(query, "action");
  if (act.empty()) return false;

  /* Require the initiating frame to already be on the matching kajo:// page so
   * untrusted websites cannot trigger privileged kajo:// action URLs via CSRF. */
  const bool from_same_kajo_page = (prev_url.rfind(base, 0) == 0);
  if (!from_same_kajo_page) {
    g_warning("kajo-web: blocked cross-origin action '%s' on %s from '%s'",
              act.c_str(), base.c_str(), prev_url.c_str());
    return true;
  }
  const bool is_read_filter = (act == "search" || act == "filter");
  if (!is_read_filter && !user_gesture) {
    g_warning("kajo-web: blocked non-gesture action '%s' on %s",
              act.c_str(), base.c_str());
    return true;
  }

  if (base == "kajo://newtab" && act == "search") {
    const std::string q = GetQueryParam(query, "q");
    if (!q.empty()) {
      NavigateActive(q);
      return true;
    }
  } else if (base == "kajo://bookmarks" && act == "delete" && storage_) {
    const std::string uuid = GetQueryParam(query, "uuid");
    if (!uuid.empty()) {
      web_storage_delete_bookmark(storage_, uuid.c_str());
      RefreshStorageCache();
    }
  } else if (base == "kajo://history" && storage_) {
    if (act == "clear") {
      web_storage_clear_history(storage_);
      history_query_.clear();
      history_results_.clear();
      RefreshStorageCache();
    } else if (act == "delete") {
      const std::string uri = GetQueryParam(query, "uri");
      if (!uri.empty()) {
        web_storage_delete_history_uri(storage_, uri.c_str());
        RefreshStorageCache();
      }
    } else if (act == "search") {
      history_query_ = GetQueryParam(query, "q");
      history_results_.clear();
      GPtrArray* res = web_storage_search_history(storage_, history_query_.c_str(), 100);
      for (guint i = 0; res && i < res->len; ++i) {
        const auto* h = static_cast<const KajoWebHistoryEntry*>(g_ptr_array_index(res, i));
        history_results_.push_back({h->title ? h->title : "", h->uri ? h->uri : "",
                                    h->visit_count, h->last_visit_ms});
      }
      if (res) g_ptr_array_unref(res);
    }
  } else if (base == "kajo://adblock" && adblock_) {
    if (act == "toggle_global") {
      const bool next_enabled = !adblock_->IsEnabled();
      adblock_->SetEnabled(next_enabled);
      if (auto* cs = kajo_config_store_get_default()) {
        kajo_config_store_update_web(
            cs,
            [](KajoWebCfg* draft, gpointer ud) {
              draft->adblock_enabled = GPOINTER_TO_INT(ud) ? TRUE : FALSE;
            },
            GINT_TO_POINTER(next_enabled ? 1 : 0),
            nullptr);
      }
    } else if (act == "unpause") {
      const std::string h = GetQueryParam(query, "host");
      if (!h.empty()) adblock_->ToggleHostPaused("https://" + h + "/");
    } else if (act == "update_lists") {
      StartFilterListUpdates();
    }
  } else if (base == "kajo://downloads") {
    if (act == "clear_done") {
      downloads_.erase(
          std::remove_if(downloads_.begin(), downloads_.end(),
                         [](const ChromeDownloadView& d) { return d.complete || d.canceled; }),
          downloads_.end());
      if (storage_ && !cfg_.private_mode) web_storage_clear_downloads(storage_);
    } else if (act == "filter") {
      const std::string f = GetQueryParam(query, "f");
      downloads_filter_ = (f == "active" || f == "completed") ? f : "";
    } else if (act == "search") {
      downloads_search_query_ = GetQueryParam(query, "q");
    } else if (act == "open_folder") {
      const std::string p = GetQueryParam(query, "path");
      g_autofree gchar* dir = nullptr;
      if (p.empty()) {
        dir = g_strdup(download_dir_.c_str());
      } else if (IsKnownDownloadPath(p)) {
        dir = g_path_get_dirname(p.c_str());
      }
      if (dir && dir[0] != '\0') {
        const char* argv[] = {"kajo-files", "--", dir, nullptr};
        kajo_spawn_detached_argv(nullptr, argv, nullptr, nullptr);
      }
    } else if (act == "open_file") {
      const std::string p = GetQueryParam(query, "path");
      if (IsKnownDownloadPath(p) && g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) {
        g_autofree gchar* uri = g_filename_to_uri(p.c_str(), nullptr, nullptr);
        g_autoptr(GError) err = nullptr;
        if (!uri || !g_app_info_launch_default_for_uri(uri, nullptr, &err)) {
          const char* argv[] = {"xdg-open", p.c_str(), nullptr};
          kajo_spawn_detached_argv(nullptr, argv, nullptr, nullptr);
        }
      }
    } else if (act == "pause") {
      const uint32_t did = static_cast<uint32_t>(std::strtoul(GetQueryParam(query, "id").c_str(), nullptr, 10));
      if (auto it = download_callbacks_.find(did); it != download_callbacks_.end() && it->second) {
        it->second->Pause();
        for (auto& d : downloads_) { if (d.id == did) { d.paused = true; break; } }
      }
    } else if (act == "resume") {
      const uint32_t did = static_cast<uint32_t>(std::strtoul(GetQueryParam(query, "id").c_str(), nullptr, 10));
      if (auto it = download_callbacks_.find(did); it != download_callbacks_.end() && it->second) {
        it->second->Resume();
        for (auto& d : downloads_) { if (d.id == did) { d.paused = false; break; } }
      }
    } else if (act == "cancel") {
      const uint32_t did = static_cast<uint32_t>(std::strtoul(GetQueryParam(query, "id").c_str(), nullptr, 10));
      if (auto it = download_callbacks_.find(did); it != download_callbacks_.end() && it->second) {
        it->second->Cancel();
      }
    } else if (act == "retry") {
      const std::string target_url = GetQueryParam(query, "url");
      if (!target_url.empty()) {
        if (TabEntry* e = ActiveEntry()) {
          if (e->browser && e->browser->GetHost()) e->browser->GetHost()->StartDownload(target_url);
        }
      }
    } else if (act == "remove") {
      const uint32_t did = static_cast<uint32_t>(std::strtoul(GetQueryParam(query, "id").c_str(), nullptr, 10));
      const int64_t db_id = std::strtoll(GetQueryParam(query, "db_id").c_str(), nullptr, 10);
      RemoveDownload(did, db_id, true);
    }
  } else if (base == "kajo://permissions") {
    if (act == "revoke") {
      const std::string orig = GetQueryParam(query, "origin");
      const std::string key = GetQueryParam(query, "key");
      if (!orig.empty() && !key.empty() && permission_store_.Remove(orig, key)) {
        if (!cfg_.private_mode && !permission_store_path_.empty()) {
          permission_store_.Save(permission_store_path_);
        }
      }
    } else if (act == "clear_all") {
      permission_store_.Clear();
      if (!cfg_.private_mode && !permission_store_path_.empty()) {
        permission_store_.Save(permission_store_path_);
      }
    }
  } else if (base == "kajo://crashed") {
    if (act == "close") {
      CefRefPtr<KajoBrowserWindow> self(this);
      CefPostTask(TID_UI, new UiTask([self, tab_id]() {
        if (!self->closing_) self->CloseTab(tab_id);
      }));
      return true;
    }
    if (act == "reload") {
      KajoWebTabMeta* t = web_model_find_tab(model_, tab_id);
      std::string target = (t && t->uri && strncmp(t->uri, "kajo://crashed", 14) != 0)
                               ? t->uri
                               : DefaultNewTabUrl();
      CefRefPtr<KajoBrowserWindow> self(this);
      CefPostTask(TID_UI, new UiTask([self, tab_id, target]() {
        if (self->closing_) return;
        if (TabEntry* e = self->FindEntry(tab_id)) {
          if (e->browser && e->browser->GetMainFrame()) {
            e->browser->GetMainFrame()->LoadURL(target);
          }
        }
      }));
      return true;
    }
  } else if (base == "kajo://gpu" && act == "open_chrome_gpu") {
    CefRefPtr<KajoBrowserWindow> self(this);
    CefPostTask(TID_UI, new UiTask([self, tab_id]() {
      if (self->closing_) return;
      if (TabEntry* e = self->FindEntry(tab_id)) {
        if (e->browser && e->browser->GetMainFrame()) {
          e->browser->GetMainFrame()->LoadURL("chrome://gpu");
        }
      }
    }));
    return true;
  }

  PushStateImmediate();
  CefRefPtr<KajoBrowserWindow> self(this);
  CefPostTask(TID_UI, new UiTask([self, tab_id, base]() {
    if (self->closing_) return;
    if (TabEntry* e = self->FindEntry(tab_id)) {
      if (e->browser && e->browser->GetMainFrame()) {
        e->browser->GetMainFrame()->LoadURL(base);
      }
    }
  }));
  return true;
}

void KajoBrowserWindow::OnTabMediaDetected(uint64_t tab_id,
                                           const std::string& kind,
                                           const std::string& uri,
                                           const std::string& mime,
                                           int64_t size_bytes) {
  TabEntry* e = FindEntry(tab_id);
  if (!e) return;
  for (const auto& existing : e->media) {
    if (existing.uri == uri) return;
  }
  if (e->media.size() >= 32) return;
  ChromeMediaView mv;
  mv.kind = kind;
  mv.uri = uri;
  mv.mime_type = mime;
  mv.size_bytes = size_bytes;
  size_t slash = uri.find_last_of('/');
  std::string tail = (slash != std::string::npos) ? uri.substr(slash + 1) : uri;
  size_t q = tail.find('?');
  if (q != std::string::npos) tail = tail.substr(0, q);
  mv.label = kind + (tail.empty() ? "" : " · " + tail);
  e->media.push_back(mv);
  PushStateToChrome();
}

void KajoBrowserWindow::OnTabBlockedChanged(uint64_t tab_id) {
  PushStateToChrome();
}

void KajoBrowserWindow::OnTabFindResult(uint64_t tab_id,
                                        int count,
                                        int active_ordinal,
                                        bool final_update) {
  if (model_ && tab_id == model_->active_id) {
    find_count_ = count;
    find_active_ = active_ordinal;
    PushStateImmediate();
  }
}

std::string KajoBrowserWindow::GetSuggestedDownloadPath(const std::string& suggested_name,
                                                        bool* out_ask) {
  if (out_ask) *out_ask = ask_download_location_;
  g_mkdir_with_parents(download_dir_.c_str(), 0755);
  std::string base = suggested_name.empty() ? "download" : suggested_name;
  g_autofree gchar* full = g_build_filename(download_dir_.c_str(), base.c_str(), nullptr);
  return full ? std::string(full) : base;
}

void KajoBrowserWindow::OnTabDownloadUpdated(uint64_t tab_id,
                                             const ChromeDownloadView& dl,
                                             CefRefPtr<CefDownloadItemCallback> callback) {
  if (dl.id != 0 && dismissed_downloads_.count(dl.id)) {
    if (dl.complete || dl.canceled) {
      dismissed_downloads_.erase(dl.id);
    } else if (callback) {
      callback->Cancel();
    }
    return;
  }
  if (dl.complete || dl.canceled) {
    download_callbacks_.erase(dl.id);
    active_download_tabs_.erase(dl.id);
  } else {
    if (callback) download_callbacks_[dl.id] = callback;
    active_download_tabs_[dl.id] = tab_id;
  }
  if (model_) {
    std::set<uint64_t> busy_tabs;
    for (const auto& kv : active_download_tabs_) busy_tabs.insert(kv.second);
    for (guint i = 0; i < model_->tabs->len; ++i) {
      auto* t = static_cast<KajoWebTabMeta*>(g_ptr_array_index(model_->tabs, i));
      t->download_active = busy_tabs.count(t->id) ? TRUE : FALSE;
    }
  }
  bool found = false;
  bool became_complete = false;
  ChromeDownloadView* target_ptr = nullptr;
  for (auto& existing : downloads_) {
    if (existing.id != 0 && existing.id == dl.id) {
      const int64_t prev_db_id = existing.db_id;
      const bool prev_complete = existing.complete;
      existing = dl;
      existing.db_id = prev_db_id;
      if (!prev_complete && existing.complete) became_complete = true;
      target_ptr = &existing;
      found = true;
      break;
    }
  }
  if (!found) {
    downloads_.insert(downloads_.begin(), dl);
    target_ptr = &downloads_.front();
    if (target_ptr->complete) became_complete = true;
  }
  if (target_ptr && (target_ptr->complete || target_ptr->canceled) &&
      target_ptr->db_id == 0 && storage_ && !cfg_.private_mode && !target_ptr->filename.empty()) {
    target_ptr->db_id = web_storage_record_download(
        storage_, target_ptr->url.c_str(), target_ptr->filename.c_str(),
        target_ptr->full_path.c_str(), target_ptr->received_bytes,
        target_ptr->total_bytes, target_ptr->complete ? "complete" : "canceled");
  }
  if (!found && dl.in_progress && downloads_auto_open_ && overlay_mode_ != "downloads") {
    ToggleOverlay("downloads");
  } else if (became_complete && overlay_mode_ != "downloads") {
    downloads_unseen_complete_ = true;
  }
  PushStateToChrome();
}

bool KajoBrowserWindow::OnTabContextCommand(uint64_t tab_id,
                                            int command_id,
                                            const std::string& target_url) {
  if (command_id == 1) {
    OpenNewTab(target_url, true);
    return true;
  }
  if (command_id == 2 &&
      (target_url.rfind("http://", 0) == 0 || target_url.rfind("https://", 0) == 0 ||
       target_url.rfind("file://", 0) == 0)) {
    const char* argv[] = {"kajo-player", "--", target_url.c_str(), nullptr};
    kajo_spawn_detached_argv(nullptr, argv, nullptr, nullptr);
    return true;
  }
  return false;
}

void KajoBrowserWindow::OnTabCrashed(uint64_t tab_id,
                                     int status,
                                     int error_code,
                                     const std::string& error_string) {
  if (closing_ || !model_) return;
  KajoWebTabMeta* t = web_model_find_tab(model_, tab_id);
  if (!t || t->suspended) return;
  t->loading = FALSE;
  t->audible = FALSE;

  const char* reason_base = (status == TS_PROCESS_CRASHED)
                                ? "Renderer crashed"
                                : (status == TS_PROCESS_WAS_KILLED ? "Renderer killed / OOM"
                                                                   : "Renderer terminated");
  std::string reason = std::string(reason_base) + " (code " + std::to_string(error_code) + ")";
  if (!error_string.empty()) reason += ": " + error_string;
  g_autofree gchar* esc_reason = g_uri_escape_string(reason.c_str(), nullptr, TRUE);
  g_autofree gchar* esc_url = g_uri_escape_string(t->uri ? t->uri : "", nullptr, TRUE);
  std::string crash_url = std::string("kajo://crashed?reason=") +
                          (esc_reason ? esc_reason : "") + "&url=" + (esc_url ? esc_url : "");

  CefRefPtr<KajoBrowserWindow> self(this);
  CefPostTask(TID_UI, new UiTask([self, tab_id, crash_url]() {
    if (self->closing_) return;
    if (TabEntry* e = self->FindEntry(tab_id)) {
      if (e->browser && e->browser->GetMainFrame()) {
        e->browser->GetMainFrame()->LoadURL(crash_url);
      }
    }
    self->PushStateImmediate();
  }));
}

bool KajoBrowserWindow::OnTabMediaPermissionRequest(
    uint64_t tab_id,
    const std::string& origin,
    uint32_t perms,
    CefRefPtr<CefMediaAccessCallback> callback) {
  if (!callback || closing_) return false;
  const std::string key = PermissionKey(perms, 0);
  const int decision = cfg_.private_mode ? 0 : permission_store_.Lookup(origin, key);
  if (decision == 1) {
    callback->Continue(perms);
    return true;
  }
  if (decision == -1) {
    callback->Cancel();
    return true;
  }
  PendingPermissionReq req;
  req.req_id = next_perm_req_id_++;
  req.tab_id = tab_id;
  req.origin = NormalizeOrigin(origin);
  req.label = FormatPermissionLabel(perms, 0);
  req.perm_key = key;
  req.media_perms = perms;
  req.media_cb = callback;
  pending_permissions_.push_back(std::move(req));
  UpdateOverlayBounds();
  PushStateImmediate();
  return true;
}

bool KajoBrowserWindow::OnTabPermissionPrompt(
    uint64_t tab_id,
    uint64_t prompt_id,
    const std::string& origin,
    uint32_t perms,
    CefRefPtr<CefPermissionPromptCallback> callback) {
  if (!callback || closing_) return false;
  const std::string key = PermissionKey(0, perms);
  const int decision = cfg_.private_mode ? 0 : permission_store_.Lookup(origin, key);
  if (decision == 1) {
    callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
    return true;
  }
  if (decision == -1) {
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return true;
  }
  PendingPermissionReq req;
  req.req_id = next_perm_req_id_++;
  req.tab_id = tab_id;
  req.prompt_id = prompt_id;
  req.origin = NormalizeOrigin(origin);
  req.label = FormatPermissionLabel(0, perms);
  req.perm_key = key;
  req.prompt_cb = callback;
  pending_permissions_.push_back(std::move(req));
  UpdateOverlayBounds();
  PushStateImmediate();
  return true;
}

void KajoBrowserWindow::OnTabPermissionDismiss(uint64_t tab_id, uint64_t prompt_id) {
  bool removed = false;
  for (auto it = pending_permissions_.begin(); it != pending_permissions_.end();) {
    if (it->tab_id == tab_id && it->prompt_id == prompt_id) {
      it = pending_permissions_.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  if (removed) {
    UpdateOverlayBounds();
    PushStateImmediate();
  }
}

}  // namespace kajo_web
