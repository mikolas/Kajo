#ifndef KAJO_WEB_CEF_HOST_COORD_H
#define KAJO_WEB_CEF_HOST_COORD_H

#include <memory>
#include <string>
#include <vector>

#include "include/cef_request_context.h"
#include "web_cef/cef_adblock.h"
#include "web_cef/cef_notify.h"
#include "web_cef/cef_prefs_helper.h"
#include "web_cef/cef_webapps.h"

namespace kajo_web {

struct LaunchConfig;

/* Per-profile coordinator implemented by KajoCefApp to orchestrate the shared
 * RequestContext, PermissionStore, WebAppStore, WebNotificationBridge,
 * ProfileIpcServer, the primary browser window (0-1), and WebApp windows (0..N). */
class BrowserHostCoordinator {
public:
  virtual ~BrowserHostCoordinator() = default;

  virtual WebAppStore* AppStore() = 0;
  virtual PermissionStore* SharedPermissions() = 0;
  virtual void SaveSharedPermissions() = 0;
  virtual void GrantSessionNotification(const std::string& origin) = 0;
  virtual bool HasSessionNotificationGrant(const std::string& origin) const = 0;
  virtual WebNotificationBridge* Notifications() = 0;
  virtual std::shared_ptr<CefAdblockEngine> Adblock() = 0;
  virtual const LaunchConfig& Config() const = 0;
  virtual CefRefPtr<CefRequestContext> SharedRequestContext() = 0;
  virtual std::string WebAppsFilePath() const = 0;

  virtual void OpenBrowserUrls(const std::vector<std::string>& urls) = 0;
  virtual void OpenOrFocusWebApp(const std::string& app_id,
                                 const std::string& override_url = "") = 0;
  virtual void LaunchAdHocWebApp(const std::string& url,
                                 const std::string& title = "") = 0;
  virtual bool UninstallWebApp(const std::string& app_id) = 0;
  virtual void NotifyWebAppsChanged() = 0;

  virtual void OnMainWindowDestroyed() = 0;
  virtual void OnWebAppWindowDestroyed(const std::string& window_key) = 0;

  virtual bool FocusNotificationTarget(const WebNotificationTarget& target) = 0;
};

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_HOST_COORD_H
