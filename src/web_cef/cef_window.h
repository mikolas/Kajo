#ifndef KAJO_WEB_CEF_WINDOW_H
#define KAJO_WEB_CEF_WINDOW_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_drag_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_request_context.h"
#include "include/cef_urlrequest.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_fill_layout.h"
#include "include/views/cef_overlay_controller.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_panel_delegate.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"
#include "include/wrapper/cef_message_router.h"
#include "web/web_model.h"
#include "web/web_storage.h"
#include "web_cef/cef_adblock.h"
#include "web_cef/cef_chrome_ui.h"
#include "web_cef/cef_prefs_helper.h"
#include "web_cef/cef_tab.h"

namespace kajo_web {

struct LaunchConfig {
  std::string exe_path;
  std::string profile_id = "default";
  bool private_mode = false;
  bool hw_accel_enabled = true;
  bool auto_adblock_sync = true;
  bool widevine_available = false;
  std::string widevine_path;
  std::string user_agent;
  std::string data_dir;
  std::string cache_dir;
  std::vector<std::string> initial_urls;
};

/* Primary Alloy Views window hosting Aero+Metro chrome + multi-tab browsers. */
class KajoBrowserWindow : public CefWindowDelegate,
                          public CefMessageRouterBrowserSide::Handler,
                          public TabCallbacks {
public:
  KajoBrowserWindow(const LaunchConfig& cfg,
                    CefAdblockEngine* adblock,
                    ChromeStateStore* store,
                    CefRefPtr<CefRequestContext> request_context);
  ~KajoBrowserWindow() override;

  void ShowWindow();

  /* Called by chrome view clients for draggable regions & key shortcuts */
  void OnChromeBrowserCreated(int role, CefRefPtr<CefBrowser> browser);
  void OnChromeDraggableRegions(const std::vector<CefDraggableRegion>& regions);
  bool OnChromeShortcut(const CefKeyEvent& event);
  CefSize GetPreferredChromeViewSize(CefRefPtr<CefView> view) const;
  int CurrentDockedRailWidth() const;
  int CurrentRailOverlayWidth() const;
  int CurrentDockedTopbarHeight() const;
  int CurrentTopbarOverlayHeight() const;
  void HandleFilterListDownloadComplete(const std::string& id,
                                        bool ok,
                                        int http_status,
                                        const std::string& body);
  void OnConfigStoreChanged();

  /* CefWindowDelegate */
  void OnWindowCreated(CefRefPtr<CefWindow> window) override;
  void OnWindowClosing(CefRefPtr<CefWindow> window) override;
  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override;
  void OnWindowBoundsChanged(CefRefPtr<CefWindow> window,
                             const CefRect& new_bounds) override;
  CefRect GetInitialBounds(CefRefPtr<CefWindow> window) override;
  bool IsFrameless(CefRefPtr<CefWindow> window) override { return true; }
  bool CanClose(CefRefPtr<CefWindow> window) override;
  bool OnAccelerator(CefRefPtr<CefWindow> window, int command_id) override;
  cef_runtime_style_t GetWindowRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }
  bool GetLinuxWindowProperties(CefRefPtr<CefWindow> window,
                                CefLinuxWindowProperties& properties) override;

  /* CefMessageRouterBrowserSide::Handler */
  bool OnQuery(CefRefPtr<CefBrowser> browser,
               CefRefPtr<CefFrame> frame,
               int64_t query_id,
               const CefString& request,
               bool persistent,
               CefRefPtr<Callback> callback) override;

  /* TabCallbacks */
  void OnTabBrowserCreated(uint64_t tab_id, CefRefPtr<CefBrowser> browser) override;
  void OnTabBrowserClosing(uint64_t tab_id) override;
  void OnTabAddressChanged(uint64_t tab_id, const std::string& url) override;
  void OnTabTitleChanged(uint64_t tab_id, const std::string& title) override;
  void OnTabFaviconChanged(uint64_t tab_id, const std::string& favicon_url) override;
  void OnTabLoadingChanged(uint64_t tab_id, bool loading, bool can_back, bool can_forward) override;
  void OnTabFullscreenChanged(uint64_t tab_id, bool fullscreen) override;
  void OnTabAudioStateChanged(uint64_t tab_id, bool audible) override;
  void OnTabOpenUrl(uint64_t from_tab_id, const std::string& url, bool foreground) override;
  bool OnTabBeforeBrowseAction(uint64_t tab_id, const std::string& url) override;
  void OnTabMediaDetected(uint64_t tab_id,
                          const std::string& kind,
                          const std::string& uri,
                          const std::string& mime,
                          int64_t size_bytes) override;
  void OnTabBlockedChanged(uint64_t tab_id) override;
  void OnTabFindResult(uint64_t tab_id, int count, int active_ordinal, bool final_update) override;
  bool OnTabShortcut(uint64_t tab_id, const CefKeyEvent& event) override;
  void OnTabGotFocus(uint64_t tab_id) override;
  std::string GetSuggestedDownloadPath(const std::string& suggested_name, bool* out_ask) override;
  void OnTabDownloadUpdated(uint64_t tab_id,
                            const ChromeDownloadView& dl,
                            CefRefPtr<CefDownloadItemCallback> callback) override;
  bool OnTabContextCommand(uint64_t tab_id, int command_id, const std::string& target_url) override;
  void OnTabCrashed(uint64_t tab_id,
                    int status,
                    int error_code,
                    const std::string& error_string) override;
  bool OnTabMediaPermissionRequest(uint64_t tab_id,
                                   const std::string& origin,
                                   uint32_t perms,
                                   CefRefPtr<CefMediaAccessCallback> callback) override;
  bool OnTabPermissionPrompt(uint64_t tab_id,
                             uint64_t prompt_id,
                             const std::string& origin,
                             uint32_t perms,
                             CefRefPtr<CefPermissionPromptCallback> callback) override;
  void OnTabPermissionDismiss(uint64_t tab_id, uint64_t prompt_id) override;

  CefRefPtr<CefMessageRouterBrowserSide> router() const { return router_; }

private:
  struct PendingPermissionReq {
    uint64_t req_id = 0;
    uint64_t tab_id = 0;
    uint64_t prompt_id = 0;
    std::string origin;
    std::string label;
    std::string perm_key;
    uint32_t media_perms = 0;
    CefRefPtr<CefMediaAccessCallback> media_cb;
    CefRefPtr<CefPermissionPromptCallback> prompt_cb;
  };

  struct TabEntry {
    uint64_t id = 0;
    CefRefPtr<CefTabClient> client;
    CefRefPtr<CefBrowserView> view;
    CefRefPtr<CefBrowser> browser;
    std::string favicon_url;
    bool can_go_back = false;
    bool can_go_forward = false;
    int find_identifier = 0;
    std::vector<ChromeMediaView> media;
  };

  void InitBackend();
  void ReloadThemeAndConfig();
  void PersistRailMode(int mode);
  void PersistTopbarMode(int mode);
  void UpdateWindowBorderColor();
  void ApplyColorSchemeToContext();
  void ReloadUserScripts();
  void RebuildLayout();
  void ApplyRailSizing();
  void ApplyTopbarSizing();
  void UpdateOverlayBounds();
  uint64_t OpenNewTab(const std::string& url, bool foreground, const std::string& group = "");
  void AttachTabView(uint64_t tab_id, const std::string& target, bool foreground);
  void ActivateTab(uint64_t tab_id);
  void CloseTab(uint64_t tab_id);
  void ReopenClosedTab();
  void DuplicateTab(uint64_t tab_id);
  bool SuspendTab(uint64_t tab_id, bool manual);
  void SweepIdleTabs();
  void ScheduleIdleSuspendTimer();
  void PeekRailTemporarily(int duration_ms);
  void FocusOmnibar();
  void OpenBookmarkPopover();
  void RespondActivePermission(uint64_t req_id, const std::string& decision);
  void ResolvePermissionReq(PendingPermissionReq& req, bool allow, bool persist);
  void CancelPermissionsForTab(uint64_t tab_id);
  void NavigateActive(const std::string& raw_input);
  void ToggleActiveBookmark();
  void ToggleOverlay(const std::string& mode);
  void CloseOverlay();
  void HideAutocomplete();
  void BuildAutocomplete(const std::string& query);
  void BuildPalette(const std::string& query);
  void StartFilterListUpdates();
  void FinishFilterListReloadOnUi(size_t ok_count, size_t fail_count);
  void RefreshStorageCache();
  void PushStateToChrome();
  void PushStateImmediate();
  void PersistSession();
  CefBrowserSettings MakeContentBrowserSettings() const;
  std::string DefaultNewTabUrl() const;
  TabEntry* FindEntry(uint64_t tab_id);
  TabEntry* ActiveEntry();

  LaunchConfig cfg_;
  CefAdblockEngine* adblock_;
  ChromeStateStore* store_;
  CefRefPtr<CefRequestContext> request_context_;

  KajoWebModel* model_ = nullptr;
  KajoWebStorage* storage_ = nullptr;
  KajoWebProfileRegistry* profiles_ = nullptr;
  guint config_sub_id_ = 0;

  CefRefPtr<CefWindow> window_;
  CefRefPtr<CefPanel> topbar_spacer_;
  CefRefPtr<CefPanel> body_panel_;
  CefRefPtr<CefPanel> rail_spacer_;
  CefRefPtr<CefPanel> content_panel_;
  CefRefPtr<CefBrowserView> topbar_view_;
  CefRefPtr<CefBrowserView> rail_view_;
  CefRefPtr<CefBrowserView> infobar_view_;
  CefRefPtr<CefBrowserView> overlay_view_;
  CefRefPtr<CefBrowserView> ac_view_;
  CefRefPtr<CefOverlayController> topbar_ctrl_;
  CefRefPtr<CefOverlayController> rail_ctrl_;
  CefRefPtr<CefOverlayController> infobar_ctrl_;
  CefRefPtr<CefOverlayController> overlay_ctrl_;
  CefRefPtr<CefOverlayController> ac_ctrl_;
  CefRefPtr<CefBrowser> topbar_browser_;
  CefRefPtr<CefBrowser> rail_browser_;
  CefRefPtr<CefBrowser> infobar_browser_;
  CefRefPtr<CefBrowser> overlay_browser_;
  CefRefPtr<CefBrowser> ac_browser_;
  CefRefPtr<CefMessageRouterBrowserSide> router_;

  std::map<uint64_t, TabEntry> tabs_;
  std::map<uint32_t, uint64_t> active_download_tabs_;
  std::vector<ChromeDownloadView> downloads_;
  std::map<uint32_t, CefRefPtr<CefDownloadItemCallback>> download_callbacks_;
  std::vector<PendingPermissionReq> pending_permissions_;
  uint64_t next_perm_req_id_ = 1;
  PermissionStore permission_store_;
  std::string permission_store_path_;
  std::shared_ptr<const LoadedUserScripts> user_scripts_;
  std::vector<CefRefPtr<CefClient>> chrome_clients_;
  std::vector<CefRefPtr<CefURLRequest>> active_filter_reqs_;
  CefRefPtr<CefBrowserViewDelegate> bv_delegate_;
  CefRefPtr<CefPanelDelegate> panel_delegate_;

  int rail_mode_ = 2;
  int rail_side_ = 0;
  int rail_width_px_ = 240;
  int rail_reveal_delay_ms_ = 80;
  int rail_hide_delay_ms_ = 220;
  int topbar_mode_ = 0;
  bool topbar_hover_open_ = false;
  bool topbar_focused_ = false;
  bool profile_border_accent_ = true;
  bool show_tab_domains_ = true;
  bool rail_hover_open_ = false;
  int rail_hold_count_ = 0;
  bool web_fullscreen_ = false;
  bool layout_built_ = false;
  bool ac_visible_ = false;
  int ac_selected_idx_ = -1;
  std::string overlay_mode_;
  std::string find_query_;
  int find_active_ = 0;
  int find_count_ = 0;
  bool find_match_case_ = false;
  std::string ac_query_;
  std::string palette_query_;
  std::string history_query_;
  std::vector<ChromeSuggestionView> ac_results_;
  std::vector<ChromeSuggestionView> palette_results_;
  std::vector<ChromeBookmarkView> cached_bookmarks_;
  std::vector<ChromeHistoryView> cached_recent_history_;
  std::vector<ChromeHistoryView> history_results_;

  bool filter_update_busy_ = false;
  size_t filter_updates_pending_ = 0;
  size_t filter_updates_ok_ = 0;
  size_t filter_updates_failed_ = 0;
  std::string filter_update_status_;

  std::string accent_color_ = "#0078d4";
  std::string profile_accent_ = "#0078d4";
  std::string profile_name_ = "DEFAULT";
  std::string profile_icon_ = "🌐";
  std::string ui_font_family_ = "Fira Code";
  int ui_font_size_px_ = 13;
  int badge_size_px_ = 11;
  int newtab_mode_ = 0;
  std::string newtab_custom_url_;
  int search_engine_ = 0;
  std::string search_custom_url_;
  int default_zoom_pct_ = 100;
  int default_font_size_px_ = 16;
  int mono_font_size_px_ = 13;
  int min_font_size_px_ = 9;
  int suspend_idle_min_ = 30;
  int color_scheme_ = 0;
  int cookie_policy_ = 0;
  bool user_scripts_enabled_ = false;
  bool enable_webgl_ = true;
  bool https_only_ = false;
  std::string download_dir_;
  bool ask_download_location_ = false;
  bool restore_session_ = true;
  std::string adblock_custom_rules_path_;

  uint64_t push_gen_ = 0;
  bool push_pending_ = false;
  uint64_t rail_hover_gen_ = 0;
  uint64_t topbar_hover_gen_ = 0;
  uint64_t suspend_timer_gen_ = 0;
  bool closing_ = false;

  IMPLEMENT_REFCOUNTING(KajoBrowserWindow);
};

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_WINDOW_H
