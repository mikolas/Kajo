#ifndef KAJO_WEB_CEF_APP_H
#define KAJO_WEB_CEF_APP_H

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "include/cef_app.h"
#include "include/wrapper/cef_message_router.h"
#include "web_cef/cef_adblock.h"
#include "web_cef/cef_chrome_ui.h"
#include "web_cef/cef_host_coord.h"
#include "web_cef/cef_ipc.h"
#include "web_cef/cef_notify.h"
#include "web_cef/cef_prefs_helper.h"
#include "web_cef/cef_webapp_window.h"
#include "web_cef/cef_webapps.h"
#include "web_cef/cef_window.h"

namespace kajo_web {

std::string DetectSystemWidevinePath();
void EnsureWidevineComponentHint(const std::string& root_cache_dir,
                                 const std::string& widevine_dir);

class KajoCefApp : public CefApp,
                   public CefBrowserProcessHandler,
                   public CefRenderProcessHandler,
                   public BrowserHostCoordinator {
public:
  explicit KajoCefApp(const LaunchConfig& cfg);

  void ShutdownRelease();

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

  /* CefApp */
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;
  void OnBeforeCommandLineProcessing(const CefString& process_type,
                                     CefRefPtr<CefCommandLine> command_line) override;

  /* CefBrowserProcessHandler */
  void OnContextInitialized() override;

  /* CefRenderProcessHandler */
  void OnWebKitInitialized() override;
  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;
  void OnContextReleased(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  /* BrowserHostCoordinator (browser process, TID_UI) */
  WebAppStore* AppStore() override { return &webapp_store_; }
  PermissionStore* SharedPermissions() override { return &permission_store_; }
  void SaveSharedPermissions() override;
  void GrantSessionNotification(const std::string& origin) override;
  bool HasSessionNotificationGrant(const std::string& origin) const override;
  WebNotificationBridge* Notifications() override { return notifications_.get(); }
  std::shared_ptr<CefAdblockEngine> Adblock() override { return adblock_; }
  const LaunchConfig& Config() const override { return cfg_; }
  CefRefPtr<CefRequestContext> SharedRequestContext() override { return request_context_; }
  std::string WebAppsFilePath() const override { return webapps_store_path_; }

  void OpenBrowserUrls(const std::vector<std::string>& urls) override;
  void OpenOrFocusWebApp(const std::string& app_id,
                         const std::string& override_url = "") override;
  void LaunchAdHocWebApp(const std::string& url,
                         const std::string& title = "") override;
  bool UninstallWebApp(const std::string& app_id) override;
  void NotifyWebAppsChanged() override;

  void OnMainWindowDestroyed() override;
  void OnWebAppWindowDestroyed(const std::string& window_key) override;

  bool FocusNotificationTarget(const WebNotificationTarget& target) override;

  /* Renderer helpers */
  int RegisterPendingNotifyPerm(CefRefPtr<CefV8Context> ctx,
                                CefRefPtr<CefV8Value> promise,
                                CefRefPtr<CefV8Value> legacy_cb);

private:
  struct PendingRendererNotifyPerm {
    CefRefPtr<CefV8Context> context;
    CefRefPtr<CefV8Value> promise;
    CefRefPtr<CefV8Value> legacy_cb;
  };

  void HandleIpcOpen(const IpcOpenRequest& req);
  void HandleNotificationClickOnUi(const WebNotificationTarget& target);
  void HandleNotificationCloseOnUi(const WebNotificationTarget& target);
  void MaybeQuitIfNoWindows();

  LaunchConfig cfg_;
  bool shutting_down_ = false;
  std::shared_ptr<CefAdblockEngine> adblock_;
  std::shared_ptr<ChromeStateStore> store_;
  CefRefPtr<CefRequestContext> request_context_;
  PermissionStore permission_store_;
  std::string permission_store_path_;
  WebAppStore webapp_store_;
  std::string webapps_store_path_;
  std::unordered_set<std::string> session_notify_granted_;
  std::unique_ptr<ProfileIpcServer> ipc_server_;
  std::unique_ptr<WebNotificationBridge> notifications_;

  CefRefPtr<KajoBrowserWindow> main_window_;
  std::map<std::string, CefRefPtr<KajoWebAppWindow>> app_windows_;
  uint64_t next_adhoc_id_ = 1;

  /* Renderer process state */
  CefRefPtr<CefMessageRouterRendererSide> renderer_router_;
  std::mutex perm_mu_;
  int next_renderer_perm_req_id_ = 1;
  std::unordered_map<int, PendingRendererNotifyPerm> pending_renderer_perms_;

  IMPLEMENT_REFCOUNTING(KajoCefApp);
};

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_APP_H
