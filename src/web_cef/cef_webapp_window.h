#ifndef KAJO_WEB_CEF_WEBAPP_WINDOW_H
#define KAJO_WEB_CEF_WEBAPP_WINDOW_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"
#include "include/wrapper/cef_message_router.h"
#include "web_cef/cef_chrome_ui.h"
#include "web_cef/cef_site_util.h"
#include "web_cef/cef_tab.h"
#include "web_cef/cef_webapps.h"

namespace kajo_web {

class BrowserHostCoordinator;

/* Dedicated chromeless Alloy Views window for a single Web App (PWA / SSB).
 * Shares the parent profile's CefRequestContext, cookies, auth, adblock,
 * permissions, and WebNotificationBridge with KajoBrowserWindow. */
class KajoWebAppWindow : public CefWindowDelegate,
                         public CefMessageRouterBrowserSide::Handler,
                         public TabCallbacks {
public:
  KajoWebAppWindow(BrowserHostCoordinator* coordinator,
                   const std::string& window_key,
                   const WebAppEntry& app,
                   const std::string& initial_url = "");
  ~KajoWebAppWindow() override;

  void ShowWindow();
  void FocusAndActivate();
  void LoadUrl(const std::string& url);
  void UpdateAppMetadata(const WebAppEntry& app);
  void DispatchNotificationEvent(const std::string& js_notif_id, const std::string& event_type);
  void CloseWindow();

  const std::string& WindowKey() const { return window_key_; }
  const WebAppEntry& AppEntry() const { return app_; }
  int BrowserId() const;

  void OnInfobarBrowserCreated(CefRefPtr<CefBrowser> browser);
  bool HandleShortcut(const CefKeyEvent& event);
  CefSize GetPreferredInfobarSize() const;
  CefMessageRouterBrowserSide* Router() { return router_.get(); }

  /* CefWindowDelegate */
  void OnWindowCreated(CefRefPtr<CefWindow> window) override;
  void OnWindowClosing(CefRefPtr<CefWindow> window) override;
  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override;
  bool CanClose(CefRefPtr<CefWindow> window) override;
  void OnWindowBoundsChanged(CefRefPtr<CefWindow> window, const CefRect& new_bounds) override;
  CefRect GetInitialBounds(CefRefPtr<CefWindow> window) override;
  bool GetLinuxWindowProperties(CefRefPtr<CefWindow> window,
                                CefLinuxWindowProperties& properties) override;
  bool OnAccelerator(CefRefPtr<CefWindow> window, int command_id) override;

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
  void OnTabFaviconChanged(uint64_t tab_id, const std::string& favicon_url) override {}
  void OnTabLoadingChanged(uint64_t tab_id, bool loading, bool can_back, bool can_forward) override {}
  void OnTabFullscreenChanged(uint64_t tab_id, bool fullscreen) override;
  void OnTabAudioStateChanged(uint64_t tab_id, bool audible) override {}
  void OnTabOpenUrl(uint64_t from_tab_id, const std::string& url, bool foreground) override;
  bool OnTabBeforeBrowseAction(uint64_t tab_id,
                               const std::string& url,
                               const std::string& prev_url,
                               bool user_gesture) override { return false; }
  void OnTabRequestClose(uint64_t tab_id) override;
  void OnTabMediaDetected(uint64_t tab_id,
                          const std::string& kind,
                          const std::string& uri,
                          const std::string& mime,
                          int64_t size_bytes,
                          const std::string& referer,
                          const std::string& user_agent) override {}
  void OnTabBlockedChanged(uint64_t tab_id) override {}
  void OnTabFindResult(uint64_t tab_id, int count, int active_ordinal, bool final_update) override {}
  bool OnTabShortcut(uint64_t tab_id, const CefKeyEvent& event) override;
  void OnTabGotFocus(uint64_t tab_id) override {}
  std::string GetSuggestedDownloadPath(const std::string& suggested_name, bool* out_ask) override;
  void OnTabDownloadUpdated(uint64_t tab_id,
                            const ChromeDownloadView& dl,
                            CefRefPtr<CefDownloadItemCallback> callback) override {}
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

  bool ShouldAllowNativePopup(uint64_t tab_id,
                              const std::string& url,
                              cef_window_open_disposition_t disposition) override;
  bool OnTabShouldInterceptNavigation(uint64_t tab_id,
                                      const std::string& url,
                                      bool is_redirect,
                                      bool user_gesture) override;
  void OnTabWebNotifyShow(uint64_t tab_id,
                            const std::string& origin,
                            const std::string& title,
                            const std::string& body,
                            const std::string& tag,
                            const std::string& js_notif_id) override;
  void OnTabWebNotifyClose(uint64_t tab_id,
                             const std::string& js_notif_id) override;
  void OnTabWebNotifyRequestPerm(uint64_t tab_id,
                                   const std::string& origin,
                                   int req_id) override;
  std::string QueryTabWebNotifyPermission(uint64_t tab_id,
                                          const std::string& origin) override;

private:
  struct PendingPermission {
    uint64_t req_id = 0;
    uint64_t prompt_id = 0;
    std::string origin;
    std::string label;
    std::string perm_key;
    uint32_t media_perms = 0;
    CefRefPtr<CefMediaAccessCallback> media_cb;
    CefRefPtr<CefPermissionPromptCallback> prompt_cb;
    int notify_js_req_id = -1;
    std::string notify_origin;
  };

  void UpdateWindowBorderAndTitle();
  void UpdateInfobarOverlayBounds();
  void PushInfobarState();
  void RespondToPermission(uint64_t req_id, bool allow, bool remember);
  void ResolvePermission(PendingPermission& req, bool allow, bool remember);
  void ClearPermissionCallbacks();
  void DeliverNotifyPermResult(int js_req_id, const std::string& origin, const std::string& perm);
  void ApplyZoomDelta(ZoomShortcutAction act);

  BrowserHostCoordinator* coord_;
  std::string window_key_;
  WebAppEntry app_;
  std::string initial_url_;
  std::string current_title_;

  std::shared_ptr<ChromeStateStore> infobar_store_;
  CefRefPtr<CefMessageRouterBrowserSide> router_;

  CefRefPtr<CefWindow> window_;
  CefRefPtr<CefBrowserView> content_view_;
  CefRefPtr<CefBrowserView> infobar_view_;
  CefRefPtr<CefOverlayController> infobar_ctrl_;
  CefRefPtr<CefBrowser> content_browser_;
  CefRefPtr<CefBrowser> infobar_browser_;
  CefRefPtr<CefTabClient> tab_client_;
  CefRefPtr<CefClient> infobar_client_;
  CefRefPtr<CefBrowserViewDelegate> bv_delegate_;

  std::vector<PendingPermission> pending_permissions_;
  uint64_t next_perm_req_id_ = 1;
  int default_zoom_pct_ = 100;
  bool force_ask_next_download_ = false;
  int64_t clipboard_auto_allow_until_us_ = 0;
  bool closing_ = false;
  bool fullscreen_ = false;

  IMPLEMENT_REFCOUNTING(KajoWebAppWindow);
};

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_WEBAPP_WINDOW_H
