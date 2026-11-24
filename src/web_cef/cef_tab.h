#ifndef KAJO_WEB_CEF_TAB_H
#define KAJO_WEB_CEF_TAB_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "include/cef_client.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_find_handler.h"
#include "include/cef_focus_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_permission_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_request_handler.h"
#include "web_cef/cef_adblock.h"
#include "web_cef/cef_chrome_ui.h"
#include "web_cef/cef_prefs_helper.h"

namespace kajo_web {

class TabCallbacks {
public:
  virtual ~TabCallbacks() = default;

  virtual void OnTabBrowserCreated(uint64_t tab_id, CefRefPtr<CefBrowser> browser) = 0;
  virtual void OnTabBrowserClosing(uint64_t tab_id) = 0;
  virtual void OnTabAddressChanged(uint64_t tab_id, const std::string& url) = 0;
  virtual void OnTabTitleChanged(uint64_t tab_id, const std::string& title) = 0;
  virtual void OnTabFaviconChanged(uint64_t tab_id, const std::string& favicon_url) = 0;
  virtual void OnTabLoadingChanged(uint64_t tab_id, bool loading, bool can_back, bool can_forward) = 0;
  virtual void OnTabFullscreenChanged(uint64_t tab_id, bool fullscreen) = 0;
  virtual void OnTabAudioStateChanged(uint64_t tab_id, bool audible) = 0;
  virtual void OnTabOpenUrl(uint64_t from_tab_id, const std::string& url, bool foreground) = 0;
  virtual bool OnTabBeforeBrowseAction(uint64_t tab_id,
                                       const std::string& url,
                                       const std::string& prev_url,
                                       bool user_gesture) = 0;
  virtual void OnTabRequestClose(uint64_t tab_id) = 0;
  virtual void OnTabMediaDetected(uint64_t tab_id,
                                  const std::string& kind,
                                  const std::string& uri,
                                  const std::string& mime,
                                  int64_t size_bytes) = 0;
  virtual void OnTabBlockedChanged(uint64_t tab_id) = 0;
  virtual void OnTabFindResult(uint64_t tab_id, int count, int active_ordinal, bool final_update) = 0;
  virtual bool OnTabShortcut(uint64_t tab_id, const CefKeyEvent& event) = 0;
  virtual void OnTabGotFocus(uint64_t tab_id) = 0;
  virtual std::string GetSuggestedDownloadPath(const std::string& suggested_name, bool* out_ask) = 0;
  virtual void OnTabDownloadUpdated(uint64_t tab_id,
                                    const ChromeDownloadView& dl,
                                    CefRefPtr<CefDownloadItemCallback> callback) = 0;
  virtual bool OnTabContextCommand(uint64_t tab_id, int command_id, const std::string& target_url) = 0;
  virtual void OnTabCrashed(uint64_t tab_id,
                            int status,
                            int error_code,
                            const std::string& error_string) = 0;
  virtual bool OnTabMediaPermissionRequest(uint64_t tab_id,
                                           const std::string& origin,
                                           uint32_t perms,
                                           CefRefPtr<CefMediaAccessCallback> callback) = 0;
  virtual bool OnTabPermissionPrompt(uint64_t tab_id,
                                     uint64_t prompt_id,
                                     const std::string& origin,
                                     uint32_t perms,
                                     CefRefPtr<CefPermissionPromptCallback> callback) = 0;
  virtual void OnTabPermissionDismiss(uint64_t tab_id, uint64_t prompt_id) = 0;

  /* Optional hooks for WebApp navigation, native auth popups, Web Notifications & PWA probing. */
  virtual bool ShouldAllowNativePopup(uint64_t /*tab_id*/,
                                      const std::string& /*url*/,
                                      cef_window_open_disposition_t /*disposition*/) {
    return false;
  }
  virtual bool OnTabShouldInterceptNavigation(uint64_t /*tab_id*/,
                                            const std::string& /*url*/,
                                            bool /*is_redirect*/,
                                            bool /*user_gesture*/) {
    return false;
  }
  virtual void OnTabWebNotifyShow(uint64_t /*tab_id*/,
                                    const std::string& /*origin*/,
                                    const std::string& /*title*/,
                                    const std::string& /*body*/,
                                    const std::string& /*tag*/,
                                    const std::string& /*js_notif_id*/) {}
  virtual void OnTabWebNotifyClose(uint64_t /*tab_id*/,
                                     const std::string& /*js_notif_id*/) {}
  virtual void OnTabWebNotifyRequestPerm(uint64_t /*tab_id*/,
                                           const std::string& /*origin*/,
                                           int /*req_id*/) {}
  virtual std::string QueryTabWebNotifyPermission(uint64_t /*tab_id*/,
                                                  const std::string& /*origin*/) {
    return "default";
  }
  virtual void OnTabWebAppProbeResult(uint64_t /*tab_id*/,
                                      const std::string& /*page_url*/,
                                      const std::string& /*manifest_url*/,
                                      const std::string& /*icon_url*/,
                                      const std::string& /*title*/,
                                      const std::string& /*theme_color*/) {}
};

/* Per-tab CefClient handling blocking, media sniffing, popups, downloads, and events. */
class CefTabClient : public CefClient,
                     public CefLifeSpanHandler,
                     public CefLoadHandler,
                     public CefDisplayHandler,
                     public CefRequestHandler,
                     public CefResourceRequestHandler,
                     public CefCookieAccessFilter,
                     public CefPermissionHandler,
                     public CefDownloadHandler,
                     public CefFindHandler,
                     public CefKeyboardHandler,
                     public CefContextMenuHandler,
                     public CefFocusHandler {
public:
  CefTabClient(uint64_t tab_id,
               std::shared_ptr<CefAdblockEngine> adblock,
               TabCallbacks* callbacks);

  void DetachCallbacks();
  void SetHttpsOnly(bool https_only);
  void SetCookiePolicy(int cookie_policy);
  void SetDevToolsEnabled(bool enabled);
  void SetUserScripts(bool enabled, std::shared_ptr<const LoadedUserScripts> scripts);
  void RefreshCosmeticInjection(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame);

  /* CefClient */
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
  CefRefPtr<CefFindHandler> GetFindHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
  CefRefPtr<CefFocusHandler> GetFocusHandler() override { return this; }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  /* CefLifeSpanHandler */
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     int popup_id,
                     const CefString& target_url,
                     const CefString& target_frame_name,
                     cef_window_open_disposition_t target_disposition,
                     bool user_gesture,
                     const CefPopupFeatures& popupFeatures,
                     CefWindowInfo& windowInfo,
                     CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override;

  /* CefLoadHandler */
  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                            bool isLoading,
                            bool canGoBack,
                            bool canGoForward) override;
  void OnLoadStart(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   TransitionType transition_type) override;
  void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int httpStatusCode) override;
  void OnLoadError(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode,
                   const CefString& errorText,
                   const CefString& failedUrl) override;

  /* CefDisplayHandler */
  void OnAddressChange(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       const CefString& url) override;
  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString& title) override;
  void OnFaviconURLChange(CefRefPtr<CefBrowser> browser,
                          const std::vector<CefString>& icon_urls) override;
  void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser,
                              bool fullscreen) override;

  /* CefRequestHandler */
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override;
  bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        const CefString& target_url,
                        cef_window_open_disposition_t target_disposition,
                        bool user_gesture) override;
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request,
      bool is_navigation,
      bool is_download,
      const CefString& request_initiator,
      bool& disable_default_handling) override;
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                 TerminationStatus status,
                                 int error_code,
                                 const CefString& error_string) override;

  /* CefResourceRequestHandler (TID_IO) */
  CefRefPtr<CefCookieAccessFilter> GetCookieAccessFilter(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request) override;
  ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefRequest> request,
                                   CefRefPtr<CefCallback> callback) override;
  bool OnResourceResponse(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefRequest> request,
                          CefRefPtr<CefResponse> response) override;

  /* CefCookieAccessFilter (TID_IO) */
  bool CanSendCookie(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     CefRefPtr<CefRequest> request,
                     const CefCookie& cookie) override;
  bool CanSaveCookie(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     CefRefPtr<CefRequest> request,
                     CefRefPtr<CefResponse> response,
                     const CefCookie& cookie) override;

  /* CefPermissionHandler (TID_UI) */
  bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefFrame> frame,
                                      const CefString& requesting_origin,
                                      uint32_t requested_permissions,
                                      CefRefPtr<CefMediaAccessCallback> callback) override;
  bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser,
                              uint64_t prompt_id,
                              const CefString& requesting_origin,
                              uint32_t requested_permissions,
                              CefRefPtr<CefPermissionPromptCallback> callback) override;
  void OnDismissPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                 uint64_t prompt_id,
                                 cef_permission_request_result_t result) override;

  /* CefDownloadHandler */
  bool OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDownloadItem> download_item,
                        const CefString& suggested_name,
                        CefRefPtr<CefBeforeDownloadCallback> callback) override;
  void OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefDownloadItem> download_item,
                         CefRefPtr<CefDownloadItemCallback> callback) override;

  /* CefFindHandler */
  void OnFindResult(CefRefPtr<CefBrowser> browser,
                    int identifier,
                    int count,
                    const CefRect& selectionRect,
                    int activeMatchOrdinal,
                    bool finalUpdate) override;

  /* CefKeyboardHandler */
  bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                     const CefKeyEvent& event,
                     CefEventHandle os_event,
                     bool* is_keyboard_shortcut) override;

  /* CefContextMenuHandler */
  void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefContextMenuParams> params,
                           CefRefPtr<CefMenuModel> model) override;
  bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefContextMenuParams> params,
                            int command_id,
                            EventFlags event_flags) override;

  /* CefFocusHandler */
  void OnGotFocus(CefRefPtr<CefBrowser> browser) override;

private:
  TabCallbacks* GetCallbacks();
  void PushNotifyPermissionSync(CefRefPtr<CefBrowser> browser, const std::string& url);

  uint64_t tab_id_;
  std::shared_ptr<CefAdblockEngine> adblock_;
  std::mutex cb_mu_;
  std::mutex url_mu_;
  std::mutex scripts_mu_;
  TabCallbacks* callbacks_;
  std::atomic<bool> detached_{false};
  std::atomic<bool> https_only_{false};
  std::atomic<int> cookie_policy_{0};
  std::atomic<bool> devtools_enabled_{true};
  bool user_scripts_enabled_ = false;
  std::shared_ptr<const LoadedUserScripts> user_scripts_;
  std::string last_top_url_;

  IMPLEMENT_REFCOUNTING(CefTabClient);
};

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_TAB_H
