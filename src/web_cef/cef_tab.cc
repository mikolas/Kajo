#include "web_cef/cef_tab.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <functional>

#include "config/kajo_config_types.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"
#include "web/web_media_sniff_core.h"
#include "web_cef/cef_site_util.h"

namespace kajo_web {

namespace {

enum ContextCmd {
  CMD_KAJO_OPEN_LINK_TAB = MENU_ID_USER_FIRST + 1,
  CMD_KAJO_OPEN_PLAYER = MENU_ID_USER_FIRST + 2,
  CMD_KAJO_INSPECT = MENU_ID_USER_FIRST + 3,
};

class UiClosureTask : public CefTask {
public:
  explicit UiClosureTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override {
    if (fn_) fn_();
  }

private:
  std::function<void()> fn_;
  IMPLEMENT_REFCOUNTING(UiClosureTask);
};

ResourceTypeMask MapCefResourceType(cef_resource_type_t rt) {
  switch (rt) {
    case ::RT_MAIN_FRAME:
    case ::RT_NAVIGATION_PRELOAD_MAIN_FRAME:
      return ResourceTypeMask::RT_MAIN_FRAME;
    case ::RT_SUB_FRAME:
    case ::RT_NAVIGATION_PRELOAD_SUB_FRAME:
      return ResourceTypeMask::RT_SUBDOCUMENT;
    case ::RT_STYLESHEET:
      return ResourceTypeMask::RT_STYLESHEET;
    case ::RT_SCRIPT:
    case ::RT_WORKER:
    case ::RT_SHARED_WORKER:
    case ::RT_SERVICE_WORKER:
      return ResourceTypeMask::RT_SCRIPT;
    case ::RT_IMAGE:
    case ::RT_FAVICON:
      return ResourceTypeMask::RT_IMAGE;
    case ::RT_FONT_RESOURCE:
      return ResourceTypeMask::RT_FONT;
    case ::RT_MEDIA:
      return ResourceTypeMask::RT_MEDIA;
    case ::RT_XHR:
    case ::RT_PING:
    case ::RT_CSP_REPORT:
      return ResourceTypeMask::RT_XHR;
    default:
      return ResourceTypeMask::RT_OTHER;
  }
}

bool IsYouTubeHost(const std::string& host) {
  return host == "youtube.com" ||
         (host.size() > 12 && host.compare(host.size() - 12, 12, ".youtube.com") == 0);
}

std::string LoadErrorPageHtml(const std::string& url,
                              int err_code,
                              const std::string& err_text) {
  return "<!doctype html><html><head><meta charset='utf-8'><title>Navigation Error</title>"
         "<style>body{margin:0;background:#141414;color:#ececec;font-family:'Fira Code',monospace;padding:48px;}"
         ".box{max-width:680px;margin:0 auto;background:#181818;border:1px solid #2b2b2b;border-left:4px solid #e81123;padding:24px;}"
         "h1{margin:0 0 10px 0;font-size:16px;text-transform:uppercase;}"
         "p{color:#aaaaaa;word-break:break-all;}"
         ".btn{display:inline-block;margin-top:12px;padding:7px 14px;background:#0078d4;color:#fff;text-decoration:none;}"
         "</style></head><body><div class='box'><h1>PAGE FAILED TO LOAD</h1>"
         "<p>URL: " + HtmlEscape(url) + "</p>"
         "<p>Error " + std::to_string(err_code) + ": " + HtmlEscape(err_text) + "</p>"
         "<a class='btn' href='" + HtmlEscape(SanitizeSafeHref(url)) + "'>RETRY</a></div></body></html>";
}

/* Isolated client for about:blank/empty-URL popups (e.g. OAuth/SSO flows) so
 * they never reuse the opener tab's tab_id_ or clobber opener->browser. */
class CefPopupClient : public CefClient,
                       public CefLifeSpanHandler,
                       public CefRequestHandler,
                       public CefResourceRequestHandler,
                       public CefCookieAccessFilter {
public:
  CefPopupClient(std::shared_ptr<CefAdblockEngine> adblock,
                 bool https_only,
                 int cookie_policy,
                 std::string opener_url)
      : adblock_(std::move(adblock)),
        https_only_(https_only),
        cookie_policy_(cookie_policy),
        top_url_(std::move(opener_url)) {}

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  bool DoClose(CefRefPtr<CefBrowser> browser) override { return false; }

  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request,
      bool is_navigation,
      bool is_download,
      const CefString& request_initiator,
      bool& disable_default_handling) override {
    return this;
  }

  CefRefPtr<CefCookieAccessFilter> GetCookieAccessFilter(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request) override {
    CEF_REQUIRE_IO_THREAD();
    if (cookie_policy_ == KAJO_WEB_COOKIES_ALWAYS) return nullptr;
    return this;
  }

  bool CanSendCookie(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     CefRefPtr<CefRequest> request,
                     const CefCookie& cookie) override {
    CEF_REQUIRE_IO_THREAD();
    if (cookie_policy_ == KAJO_WEB_COOKIES_NEVER) return false;
    if (cookie_policy_ == KAJO_WEB_COOKIES_ALWAYS || !request) return true;
    const std::string req_url = request->GetURL().ToString();
    if (req_url.rfind("http://", 0) != 0 && req_url.rfind("https://", 0) != 0) return true;
    std::string fp = request->GetFirstPartyForCookies().ToString();
    if (fp.empty() || fp == "about:blank") {
      std::lock_guard<std::mutex> lock(mu_);
      fp = top_url_;
    }
    return IsSameSiteUrl(req_url, fp);
  }

  bool CanSaveCookie(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     CefRefPtr<CefRequest> request,
                     CefRefPtr<CefResponse> response,
                     const CefCookie& cookie) override {
    CEF_REQUIRE_IO_THREAD();
    return CanSendCookie(browser, frame, request, cookie);
  }

  ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefRequest> request,
                                   CefRefPtr<CefCallback> callback) override {
    CEF_REQUIRE_IO_THREAD();
    if (!request) return RV_CONTINUE;
    const std::string url = request->GetURL().ToString();
    if (https_only_ && url.rfind("http://", 0) == 0 &&
        request->GetResourceType() == ::RT_MAIN_FRAME) {
      const std::string host = CefAdblockEngine::ExtractHost(url);
      if (!IsLocalOrPrivateHost(host)) {
        request->SetURL("https://" + url.substr(7));
      }
    }
    if (adblock_) {
      std::string fp;
      if (frame && frame->IsMain()) {
        fp = frame->GetURL().ToString();
        if (!fp.empty() && fp != "about:blank") {
          std::lock_guard<std::mutex> lock(mu_);
          top_url_ = fp;
        }
      }
      if (fp.empty() || fp == "about:blank") {
        std::lock_guard<std::mutex> lock(mu_);
        fp = top_url_;
      }
      if (fp.empty() && frame) fp = frame->GetURL().ToString();
      if (fp.empty()) fp = request->GetReferrerURL().ToString();
      if (adblock_->ShouldBlock(url, fp, MapCefResourceType(request->GetResourceType()))) {
        return RV_CANCEL;
      }
    }
    return RV_CONTINUE;
  }

private:
  std::shared_ptr<CefAdblockEngine> adblock_;
  bool https_only_ = false;
  int cookie_policy_ = 0;
  std::mutex mu_;
  std::string top_url_;
  IMPLEMENT_REFCOUNTING(CefPopupClient);
};

}  // namespace

CefTabClient::CefTabClient(uint64_t tab_id,
                           std::shared_ptr<CefAdblockEngine> adblock,
                           TabCallbacks* callbacks)
    : tab_id_(tab_id), adblock_(std::move(adblock)), callbacks_(callbacks) {}

void CefTabClient::DetachCallbacks() {
  detached_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lock(cb_mu_);
  callbacks_ = nullptr;
}

TabCallbacks* CefTabClient::GetCallbacks() {
  std::lock_guard<std::mutex> lock(cb_mu_);
  return callbacks_;
}

void CefTabClient::SetHttpsOnly(bool https_only) {
  https_only_.store(https_only, std::memory_order_relaxed);
}

void CefTabClient::SetCookiePolicy(int cookie_policy) {
  cookie_policy_.store(cookie_policy, std::memory_order_relaxed);
}

void CefTabClient::SetDevToolsEnabled(bool enabled) {
  devtools_enabled_.store(enabled, std::memory_order_relaxed);
}

void CefTabClient::SetUserScripts(bool enabled,
                                  std::shared_ptr<const LoadedUserScripts> scripts) {
  std::lock_guard<std::mutex> lock(scripts_mu_);
  user_scripts_enabled_ = enabled;
  user_scripts_ = std::move(scripts);
}

void CefTabClient::RefreshCosmeticInjection(CefRefPtr<CefBrowser> browser,
                                            CefRefPtr<CefFrame> frame) {
  if (!browser || !frame || !frame->IsMain() || !adblock_) return;
  const std::string url = frame->GetURL().ToString();
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return;
  if (!adblock_->IsEnabled() || adblock_->IsHostPaused(url)) {
    CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("kajo-apply-cosmetic");
    msg->GetArgumentList()->SetString(0, "");
    msg->GetArgumentList()->SetBool(1, false);
    frame->SendProcessMessage(PID_RENDERER, msg);
    return;
  }
  const std::string host = CefAdblockEngine::ExtractHost(url);
  const std::string css = adblock_->GetCosmeticCssForHost(host);
  const bool yt_skip = IsYouTubeHost(host);
  if (!css.empty() || yt_skip) {
    CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("kajo-apply-cosmetic");
    msg->GetArgumentList()->SetString(0, css);
    msg->GetArgumentList()->SetBool(1, yt_skip);
    frame->SendProcessMessage(PID_RENDERER, msg);
  }
}

void CefTabClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  if (detached_.load(std::memory_order_acquire)) {
    if (browser && browser->GetHost()) browser->GetHost()->CloseBrowser(true);
    return;
  }
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabBrowserCreated(tab_id_, browser);
}

bool CefTabClient::DoClose(CefRefPtr<CefBrowser> browser) {
  /* If CloseTab() or SuspendTab() already detached this client and moved the
   * CefBrowserView to the teardown window, return false so CEF finishes native
   * browser teardown and terminates the renderer cleanly. If JS called
   * window.close() directly on a live tab, defer to CloseTab() on TID_UI and
   * return true so we don't close the parent CefWindow. */
  if (detached_.load(std::memory_order_acquire)) {
    return false;
  }
  CefRefPtr<CefTabClient> self(this);
  CefPostTask(TID_UI, new UiClosureTask([self]() {
    if (TabCallbacks* cb = self->GetCallbacks()) {
      cb->OnTabRequestClose(self->tab_id_);
    }
  }));
  return true;
}

void CefTabClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  g_debug("kajo-web: tab %lu browser OnBeforeClose", static_cast<unsigned long>(tab_id_));
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabBrowserClosing(tab_id_);
}

bool CefTabClient::OnBeforePopup(CefRefPtr<CefBrowser> browser,
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
                                 bool* no_javascript_access) {
  const std::string url = target_url.ToString();
  if (!url.empty() && url != "about:blank") {
    const bool bg = (target_disposition == CEF_WOD_NEW_BACKGROUND_TAB);
    CefRefPtr<CefTabClient> self(this);
    CefPostTask(TID_UI, new UiClosureTask([self, url, bg]() {
      if (TabCallbacks* cb = self->GetCallbacks()) {
        cb->OnTabOpenUrl(self->tab_id_, url, !bg);
      }
    }));
    return true;
  }
  std::string opener_url;
  {
    std::lock_guard<std::mutex> lock(url_mu_);
    opener_url = last_top_url_;
  }
  client = new CefPopupClient(
      adblock_,
      https_only_.load(std::memory_order_relaxed),
      cookie_policy_.load(std::memory_order_relaxed),
      std::move(opener_url));
  return false;
}

void CefTabClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                                        bool isLoading,
                                        bool canGoBack,
                                        bool canGoForward) {
  if (TabCallbacks* cb = GetCallbacks()) {
    cb->OnTabLoadingChanged(tab_id_, isLoading, canGoBack, canGoForward);
  }
}

void CefTabClient::OnLoadStart(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               TransitionType transition_type) {
  if (!frame || !frame->IsMain()) return;
  const std::string url = frame->GetURL().ToString();
  {
    std::lock_guard<std::mutex> lock(url_mu_);
    last_top_url_ = url;
  }
  if (adblock_) adblock_->ResetTabBlockedCount(static_cast<int>(tab_id_));
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabBlockedChanged(tab_id_);
}

void CefTabClient::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             int httpStatusCode) {
  RefreshCosmeticInjection(browser, frame);
  if (!browser || !frame || !frame->IsMain()) return;
  const std::string url = frame->GetURL().ToString();
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return;

  bool enabled = false;
  std::shared_ptr<const LoadedUserScripts> scripts;
  {
    std::lock_guard<std::mutex> lock(scripts_mu_);
    enabled = user_scripts_enabled_;
    scripts = user_scripts_;
  }
  if (!enabled || !scripts) return;

  if (!scripts->combined_css.empty()) {
    const std::string css_js =
        "(() => {"
        "  let el = document.getElementById('kajo-user-css');"
        "  if (!el) {"
        "    el = document.createElement('style');"
        "    el.id = 'kajo-user-css';"
        "    (document.head || document.documentElement).appendChild(el);"
        "  }"
        "  el.textContent = \"" + JsonEscape(scripts->combined_css) + "\";"
        "})();";
    frame->ExecuteJavaScript(css_js, url, 0);
  }
  for (const auto& js_entry : scripts->js_files) {
    if (!js_entry.second.empty()) {
      frame->ExecuteJavaScript(
          js_entry.second, "kajo-userscript://" + js_entry.first, 0);
    }
  }
}

void CefTabClient::OnLoadError(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               ErrorCode errorCode,
                               const CefString& errorText,
                               const CefString& failedUrl) {
  if (errorCode == ERR_ABORTED || !frame || !frame->IsMain()) return;
  const std::string f_url = failedUrl.ToString();
  if (f_url.rfind("data:", 0) == 0) return;
  const std::string html = LoadErrorPageHtml(f_url,
                                             static_cast<int>(errorCode),
                                             errorText.ToString());
  g_autofree gchar* b64 = g_base64_encode(
      reinterpret_cast<const guchar*>(html.data()), html.size());
  if (b64) {
    frame->LoadURL(std::string("data:text/html;charset=utf-8;base64,") + b64);
  }
}

void CefTabClient::OnAddressChange(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   const CefString& url) {
  if (!frame || !frame->IsMain()) return;
  const std::string s = url.ToString();
  if (s.rfind("data:", 0) == 0) return;
  {
    std::lock_guard<std::mutex> lock(url_mu_);
    last_top_url_ = s;
  }
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabAddressChanged(tab_id_, s);
}

void CefTabClient::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                 const CefString& title) {
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabTitleChanged(tab_id_, title.ToString());
}

void CefTabClient::OnFaviconURLChange(CefRefPtr<CefBrowser> browser,
                                      const std::vector<CefString>& icon_urls) {
  if (icon_urls.empty()) return;
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabFaviconChanged(tab_id_, icon_urls.front().ToString());
}

void CefTabClient::OnFullscreenModeChange(CefRefPtr<CefBrowser> browser,
                                          bool fullscreen) {
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabFullscreenChanged(tab_id_, fullscreen);
}

bool CefTabClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefRefPtr<CefRequest> request,
                                  bool user_gesture,
                                  bool is_redirect) {
  if (!frame || !frame->IsMain() || !request) return false;
  const std::string url = request->GetURL().ToString();
  const std::string prev_url = frame->GetURL().ToString();
  if (url.rfind("kajo://", 0) == 0) {
    if (TabCallbacks* cb = GetCallbacks()) {
      if (cb->OnTabBeforeBrowseAction(tab_id_, url, prev_url, user_gesture)) return true;
    }
  }
  if (url.rfind("data:", 0) != 0) {
    {
      std::lock_guard<std::mutex> lock(url_mu_);
      last_top_url_ = url;
    }
    if (!is_redirect && adblock_) {
      adblock_->ResetTabBlockedCount(static_cast<int>(tab_id_));
      if (TabCallbacks* cb = GetCallbacks()) cb->OnTabBlockedChanged(tab_id_);
    }
  }
  return false;
}

bool CefTabClient::OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    const CefString& target_url,
                                    cef_window_open_disposition_t target_disposition,
                                    bool user_gesture) {
  if (target_disposition == CEF_WOD_NEW_FOREGROUND_TAB ||
      target_disposition == CEF_WOD_NEW_BACKGROUND_TAB ||
      target_disposition == CEF_WOD_NEW_POPUP ||
      target_disposition == CEF_WOD_NEW_WINDOW) {
    const std::string url = target_url.ToString();
    const bool fg = (target_disposition != CEF_WOD_NEW_BACKGROUND_TAB);
    if (TabCallbacks* cb = GetCallbacks()) cb->OnTabOpenUrl(tab_id_, url, fg);
    return true;
  }
  return false;
}

CefRefPtr<CefResourceRequestHandler> CefTabClient::GetResourceRequestHandler(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    bool is_navigation,
    bool is_download,
    const CefString& request_initiator,
    bool& disable_default_handling) {
  return this;
}

void CefTabClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                             TerminationStatus status,
                                             int error_code,
                                             const CefString& error_string) {
  if (TabCallbacks* cb = GetCallbacks()) {
    cb->OnTabCrashed(tab_id_, static_cast<int>(status), error_code, error_string.ToString());
  }
}

CefRefPtr<CefCookieAccessFilter> CefTabClient::GetCookieAccessFilter(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request) {
  CEF_REQUIRE_IO_THREAD();
  if (detached_.load(std::memory_order_acquire)) return nullptr;
  if (cookie_policy_.load(std::memory_order_relaxed) == KAJO_WEB_COOKIES_ALWAYS) {
    return nullptr;
  }
  return this;
}

bool CefTabClient::CanSendCookie(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefRefPtr<CefRequest> request,
                                 const CefCookie& cookie) {
  CEF_REQUIRE_IO_THREAD();
  if (detached_.load(std::memory_order_acquire)) return true;
  const int policy = cookie_policy_.load(std::memory_order_relaxed);
  if (policy == KAJO_WEB_COOKIES_NEVER) return false;
  if (policy == KAJO_WEB_COOKIES_ALWAYS || !request) return true;
  const std::string req_url = request->GetURL().ToString();
  if (req_url.rfind("http://", 0) != 0 && req_url.rfind("https://", 0) != 0) return true;
  std::string fp = request->GetFirstPartyForCookies().ToString();
  if (fp.empty() || fp == "about:blank") {
    std::lock_guard<std::mutex> lock(url_mu_);
    fp = last_top_url_;
  }
  return IsSameSiteUrl(req_url, fp);
}

bool CefTabClient::CanSaveCookie(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefRefPtr<CefRequest> request,
                                 CefRefPtr<CefResponse> response,
                                 const CefCookie& cookie) {
  CEF_REQUIRE_IO_THREAD();
  if (detached_.load(std::memory_order_acquire)) return true;
  return CanSendCookie(browser, frame, request, cookie);
}

bool CefTabClient::OnRequestMediaAccessPermission(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    const CefString& requesting_origin,
    uint32_t requested_permissions,
    CefRefPtr<CefMediaAccessCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  if (TabCallbacks* cb = GetCallbacks()) {
    return cb->OnTabMediaPermissionRequest(
        tab_id_, requesting_origin.ToString(), requested_permissions, callback);
  }
  return false;
}

bool CefTabClient::OnShowPermissionPrompt(
    CefRefPtr<CefBrowser> browser,
    uint64_t prompt_id,
    const CefString& requesting_origin,
    uint32_t requested_permissions,
    CefRefPtr<CefPermissionPromptCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  if (TabCallbacks* cb = GetCallbacks()) {
    return cb->OnTabPermissionPrompt(
        tab_id_, prompt_id, requesting_origin.ToString(), requested_permissions, callback);
  }
  return false;
}

void CefTabClient::OnDismissPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                             uint64_t prompt_id,
                                             cef_permission_request_result_t result) {
  CEF_REQUIRE_UI_THREAD();
  if (TabCallbacks* cb = GetCallbacks()) {
    cb->OnTabPermissionDismiss(tab_id_, prompt_id);
  }
}

CefResourceRequestHandler::ReturnValue CefTabClient::OnBeforeResourceLoad(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefCallback> callback) {
  CEF_REQUIRE_IO_THREAD();
  if (detached_.load(std::memory_order_acquire) || !request) return RV_CONTINUE;

  const std::string url = request->GetURL().ToString();
  if (https_only_.load(std::memory_order_relaxed) && url.rfind("http://", 0) == 0 &&
      request->GetResourceType() == ::RT_MAIN_FRAME) {
    const std::string host = CefAdblockEngine::ExtractHost(url);
    if (!IsLocalOrPrivateHost(host)) {
      request->SetURL("https://" + url.substr(7));
    }
  }

  if (adblock_) {
    std::string first_party;
    {
      std::lock_guard<std::mutex> lock(url_mu_);
      first_party = last_top_url_;
    }
    if ((first_party.empty() || first_party == "about:blank") && browser && browser->GetMainFrame()) {
      first_party = browser->GetMainFrame()->GetURL().ToString();
    }
    if ((first_party.empty() || first_party == "about:blank") && frame) {
      first_party = frame->GetURL().ToString();
    }
    if (first_party.empty() || first_party == "about:blank") {
      first_party = request->GetReferrerURL().ToString();
    }
    ResourceTypeMask rt = MapCefResourceType(request->GetResourceType());
    if (adblock_->ShouldBlock(url, first_party, rt)) {
      adblock_->RecordBlockedForTab(static_cast<int>(tab_id_));
      if (!detached_.load(std::memory_order_acquire)) {
        CefRefPtr<CefTabClient> self(this);
        CefPostTask(TID_UI, new UiClosureTask([self]() {
          if (TabCallbacks* cb = self->GetCallbacks()) cb->OnTabBlockedChanged(self->tab_id_);
        }));
      }
      return RV_CANCEL;
    }
  }
  return RV_CONTINUE;
}

bool CefTabClient::OnResourceResponse(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefFrame> frame,
                                      CefRefPtr<CefRequest> request,
                                      CefRefPtr<CefResponse> response) {
  CEF_REQUIRE_IO_THREAD();
  if (detached_.load(std::memory_order_acquire) || !request || !response) return false;
  const std::string uri = request->GetURL().ToString();
  if (uri.rfind("http://", 0) != 0 && uri.rfind("https://", 0) != 0) return false;

  const std::string mime = response->GetMimeType().ToString();
  const std::string len_str = response->GetHeaderByName("Content-Length").ToString();
  int64_t content_len = 0;
  if (!len_str.empty()) {
    content_len = std::strtoll(len_str.c_str(), nullptr, 10);
  }
  KajoWebMediaKind kind = web_media_classify_resource(uri.c_str(), mime.c_str(), content_len);
  if (kind != KAJO_MEDIA_NONE && !detached_.load(std::memory_order_acquire)) {
    const char* kind_str = "VIDEO";
    if (kind == KAJO_MEDIA_HLS) kind_str = "HLS";
    else if (kind == KAJO_MEDIA_DASH) kind_str = "DASH";
    CefRefPtr<CefTabClient> self(this);
    std::string kstr(kind_str);
    CefPostTask(TID_UI, new UiClosureTask([self, kstr, uri, mime, content_len]() {
      if (TabCallbacks* cb = self->GetCallbacks()) {
        cb->OnTabMediaDetected(self->tab_id_, kstr, uri, mime, content_len);
      }
    }));
  }
  return false;
}

bool CefTabClient::OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefDownloadItem> download_item,
                                    const CefString& suggested_name,
                                    CefRefPtr<CefBeforeDownloadCallback> callback) {
  bool ask = false;
  std::string path;
  if (TabCallbacks* cb = GetCallbacks()) {
    path = cb->GetSuggestedDownloadPath(suggested_name.ToString(), &ask);
  }
  callback->Continue(path, ask);
  return true;
}

void CefTabClient::OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefDownloadItem> download_item,
                                     CefRefPtr<CefDownloadItemCallback> callback) {
  if (!download_item || !download_item->IsValid()) return;
  ChromeDownloadView dl;
  dl.id = download_item->GetId();
  dl.url = download_item->GetURL().ToString();
  dl.full_path = download_item->GetFullPath().ToString();
  dl.filename = download_item->GetSuggestedFileName().ToString();
  if (dl.filename.empty() && !dl.full_path.empty()) {
    size_t pos = dl.full_path.find_last_of('/');
    dl.filename = (pos != std::string::npos) ? dl.full_path.substr(pos + 1) : dl.full_path;
  }
  dl.percent = download_item->GetPercentComplete();
  dl.received_bytes = download_item->GetReceivedBytes();
  dl.total_bytes = download_item->GetTotalBytes();
  dl.current_speed = download_item->GetCurrentSpeed();
  dl.updated_at_ms = g_get_real_time() / 1000;
  dl.in_progress = download_item->IsInProgress();
  dl.complete = download_item->IsComplete();
  dl.canceled = download_item->IsCanceled();
  dl.paused = download_item->IsPaused();

  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabDownloadUpdated(tab_id_, dl, callback);
}

void CefTabClient::OnFindResult(CefRefPtr<CefBrowser> browser,
                                int identifier,
                                int count,
                                const CefRect& selectionRect,
                                int activeMatchOrdinal,
                                bool finalUpdate) {
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabFindResult(tab_id_, count, activeMatchOrdinal, finalUpdate);
}

bool CefTabClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                                 const CefKeyEvent& event,
                                 CefEventHandle os_event,
                                 bool* is_keyboard_shortcut) {
  if (TabCallbacks* cb = GetCallbacks()) {
    if (cb->OnTabShortcut(tab_id_, event)) return true;
  }
  return false;
}

void CefTabClient::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       CefRefPtr<CefContextMenuParams> params,
                                       CefRefPtr<CefMenuModel> model) {
  if (!model || !params) return;
  model->AddSeparator();
  if (!params->GetLinkUrl().empty()) {
    model->AddItem(CMD_KAJO_OPEN_LINK_TAB, "Open Link in New Tab");
  }
  std::string target = params->GetSourceUrl().ToString();
  if (target.empty()) target = params->GetLinkUrl().ToString();
  if (target.empty()) target = params->GetPageUrl().ToString();
  if (!target.empty()) {
    model->AddItem(CMD_KAJO_OPEN_PLAYER, "Play in kajo-player");
  }
  if (devtools_enabled_.load(std::memory_order_relaxed)) {
    model->AddItem(CMD_KAJO_INSPECT, "Inspect Element");
  }
}

bool CefTabClient::OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        CefRefPtr<CefContextMenuParams> params,
                                        int command_id,
                                        EventFlags event_flags) {
  if (!params) return false;
  if (command_id == CMD_KAJO_INSPECT && devtools_enabled_.load(std::memory_order_relaxed) &&
      browser && browser->GetHost()) {
    CefWindowInfo wi;
    CefBrowserSettings bs;
    CefPoint pt(params->GetXCoord(), params->GetYCoord());
    browser->GetHost()->ShowDevTools(wi, nullptr, bs, pt);
    return true;
  }
  if (command_id == CMD_KAJO_OPEN_LINK_TAB) {
    const std::string link = params->GetLinkUrl().ToString();
    if (!link.empty()) {
      if (TabCallbacks* cb = GetCallbacks()) {
        return cb->OnTabContextCommand(tab_id_, 1, link);
      }
    }
    return false;
  }
  if (command_id == CMD_KAJO_OPEN_PLAYER) {
    std::string target = params->GetSourceUrl().ToString();
    if (target.empty()) target = params->GetLinkUrl().ToString();
    if (target.empty()) target = params->GetPageUrl().ToString();
    if (!target.empty()) {
      if (TabCallbacks* cb = GetCallbacks()) {
        return cb->OnTabContextCommand(tab_id_, 2, target);
      }
    }
    return false;
  }
  return false;
}

void CefTabClient::OnGotFocus(CefRefPtr<CefBrowser> browser) {
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabGotFocus(tab_id_);
}

}  // namespace kajo_web
