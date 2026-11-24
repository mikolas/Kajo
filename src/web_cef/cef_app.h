#ifndef KAJO_WEB_CEF_APP_H
#define KAJO_WEB_CEF_APP_H

#include <memory>

#include "include/cef_app.h"
#include "include/cef_browser_process_handler.h"
#include "include/cef_render_process_handler.h"
#include "include/wrapper/cef_message_router.h"
#include "web_cef/cef_adblock.h"
#include "web_cef/cef_chrome_ui.h"
#include "web_cef/cef_window.h"

namespace kajo_web {

/* Detects a valid system WidevineCDM directory if installed. */
std::string DetectSystemWidevinePath();

/* Writes Chrome component-updater hint JSON if missing and valid CDM exists. */
void EnsureWidevineComponentHint(const std::string& root_cache_dir,
                                 const std::string& widevine_dir);

class KajoCefApp : public CefApp,
                   public CefBrowserProcessHandler,
                   public CefRenderProcessHandler {
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

private:
  LaunchConfig cfg_;
  std::unique_ptr<CefAdblockEngine> adblock_;
  std::unique_ptr<ChromeStateStore> store_;
  CefRefPtr<CefMessageRouterRendererSide> renderer_router_;
  CefRefPtr<KajoBrowserWindow> main_window_;

  IMPLEMENT_REFCOUNTING(KajoCefApp);
};

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_APP_H
