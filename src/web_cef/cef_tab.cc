#include "web_cef/cef_tab.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>

#include "config/kajo_config_types.h"
#include "include/cef_response_filter.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"
#include "web/web_media_sniff_core.h"
#include "web_cef/cef_site_util.h"
#include "web_cef/cef_webapps.h"

namespace kajo_web {

namespace {

enum ContextCmd {
  CMD_KAJO_OPEN_LINK_TAB = MENU_ID_USER_FIRST + 1,
  CMD_KAJO_COPY_LINK_URL = MENU_ID_USER_FIRST + 2,
  CMD_KAJO_SAVE_LINK_AS = MENU_ID_USER_FIRST + 3,
  CMD_KAJO_OPEN_IMAGE_TAB = MENU_ID_USER_FIRST + 4,
  CMD_KAJO_SAVE_IMAGE_AS = MENU_ID_USER_FIRST + 5,
  CMD_KAJO_COPY_IMAGE = MENU_ID_USER_FIRST + 6,
  CMD_KAJO_COPY_IMAGE_URL = MENU_ID_USER_FIRST + 7,
  CMD_KAJO_OPEN_MEDIA_TAB = MENU_ID_USER_FIRST + 8,
  CMD_KAJO_SAVE_MEDIA_AS = MENU_ID_USER_FIRST + 9,
  CMD_KAJO_COPY_MEDIA_URL = MENU_ID_USER_FIRST + 10,
  CMD_KAJO_OPEN_PLAYER = MENU_ID_USER_FIRST + 11,
  CMD_KAJO_VIEW_SOURCE = MENU_ID_USER_FIRST + 12,
  CMD_KAJO_INSPECT = MENU_ID_USER_FIRST + 13,
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

std::string LowerStr(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

class ManifestCaptureFilter : public CefResponseFilter {
public:
  static constexpr size_t kMaxCaptureBytes = 2U * 1024U * 1024U;

  bool InitFilter() override { return true; }

  FilterStatus Filter(void* data_in,
                      size_t data_in_size,
                      size_t& data_in_read,
                      void* data_out,
                      size_t data_out_size,
                      size_t& data_out_written) override {
    if (!data_in || data_in_size == 0) {
      data_in_read = 0;
      data_out_written = 0;
      return RESPONSE_FILTER_DONE;
    }
    const size_t n = std::min(data_in_size, data_out_size);
    if (n > 0 && data_out) {
      std::memcpy(data_out, data_in, n);
    }
    data_in_read = n;
    data_out_written = n;
    if (n > 0 && !overflow_) {
      if (body_.size() + n <= kMaxCaptureBytes) {
        body_.append(static_cast<const char*>(data_in), n);
      } else {
        overflow_ = true;
        body_.clear();
      }
    }
    return (n < data_in_size) ? RESPONSE_FILTER_NEED_MORE_DATA : RESPONSE_FILTER_DONE;
  }

  const std::string& body() const { return body_; }
  bool overflow() const { return overflow_; }

private:
  std::string body_;
  bool overflow_ = false;
  IMPLEMENT_REFCOUNTING(ManifestCaptureFilter);
};

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
  {
    std::lock_guard<std::mutex> lock(cb_mu_);
    callbacks_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> mlock(media_mu_);
    req_headers_.clear();
    active_filters_.clear();
  }
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
  bool allow_native = false;
  if (url.empty() || url == "about:blank") {
    allow_native = true;
  } else if (target_disposition == CEF_WOD_NEW_POPUP && IsLikelyAuthFlowUrl(url)) {
    allow_native = true;
  } else if (TabCallbacks* cb = GetCallbacks()) {
    allow_native = cb->ShouldAllowNativePopup(tab_id_, url, target_disposition);
  }
  if (!allow_native) {
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
  page_has_manifest_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> mlock(media_mu_);
    active_filters_.clear();
  }
  if (adblock_) adblock_->ResetTabBlockedCount(static_cast<int>(tab_id_));
  if (TabCallbacks* cb = GetCallbacks()) {
    cb->OnTabMainNavigation(tab_id_, url);
    cb->OnTabBlockedChanged(tab_id_);
  }
  PushNotifyPermissionSync(browser, url);
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
  } else if (TabCallbacks* cb = GetCallbacks()) {
    if (cb->OnTabShouldInterceptNavigation(tab_id_, url, is_redirect, user_gesture)) {
      return true;
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
  const bool is_http = (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0);
  if (https_only_.load(std::memory_order_relaxed) && url.rfind("http://", 0) == 0 &&
      request->GetResourceType() == ::RT_MAIN_FRAME) {
    const std::string host = CefAdblockEngine::ExtractHost(url);
    if (!IsLocalOrPrivateHost(host)) {
      request->SetURL("https://" + url.substr(7));
    }
  }

  if (is_http && adblock_) {
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

  if (is_http) {
    const uint64_t req_id = request->GetIdentifier();
    if (req_id != 0) {
      CapturedReqHeaders cap;
      cap.ts_us = g_get_monotonic_time();
      cap.referer = request->GetReferrerURL().ToString();
      CefRequest::HeaderMap hmap;
      request->GetHeaderMap(hmap);
      for (const auto& kv : hmap) {
        const std::string k = kv.first.ToString();
        const std::string v = kv.second.ToString();
        const std::string lk = LowerStr(k);
        if (lk.empty() || v.empty()) continue;
        if (lk == "user-agent") {
          cap.user_agent = v;
        } else if (lk == "origin") {
          cap.origin = v;
          cap.headers[k] = v;
        } else if (lk == "referer" && cap.referer.empty()) {
          cap.referer = v;
        } else if (lk == "authorization" || lk.rfind("x-", 0) == 0 ||
                   lk == "accept-language" || lk == "sec-fetch-site" ||
                   lk == "sec-fetch-mode" || lk == "sec-fetch-dest") {
          cap.headers[k] = v;
        }
      }
      std::lock_guard<std::mutex> mlock(media_mu_);
      const int64_t cutoff = cap.ts_us - 600LL * 1000000LL;
      for (auto it = req_headers_.begin(); it != req_headers_.end();) {
        if (it->second.ts_us < cutoff || req_headers_.size() >= 256) {
          it = req_headers_.erase(it);
        } else {
          ++it;
        }
      }
      req_headers_[req_id] = std::move(cap);
    }
  }
  return RV_CONTINUE;
}

CefRefPtr<CefResponseFilter> CefTabClient::GetResourceResponseFilter(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefResponse> response) {
  CEF_REQUIRE_IO_THREAD();
  if (detached_.load(std::memory_order_acquire) || !request || !response) return nullptr;
  const int st = response->GetStatus();
  if (st < 200 || st >= 300) return nullptr;
  std::string uri = response->GetURL().ToString();
  if (uri.empty()) uri = request->GetURL().ToString();
  const std::string mime = response->GetMimeType().ToString();
  if (!web_media_should_capture_manifest(uri.c_str(), mime.c_str())) return nullptr;

  page_has_manifest_.store(true, std::memory_order_release);
  const uint64_t req_id = request->GetIdentifier();
  if (req_id == 0) return nullptr;
  CefRefPtr<ManifestCaptureFilter> filter(new ManifestCaptureFilter());
  {
    std::lock_guard<std::mutex> mlock(media_mu_);
    if (active_filters_.size() >= 64) active_filters_.clear();
    active_filters_[req_id] = filter;
  }
  return filter;
}

bool CefTabClient::OnResourceResponse(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefFrame> frame,
                                      CefRefPtr<CefRequest> request,
                                      CefRefPtr<CefResponse> response) {
  CEF_REQUIRE_IO_THREAD();
  if (detached_.load(std::memory_order_acquire) || !request || !response) return false;
  const int st = response->GetStatus();
  if (st < 200 || st >= 300) return false;
  const std::string uri = request->GetURL().ToString();
  if (uri.rfind("http://", 0) != 0 && uri.rfind("https://", 0) != 0) return false;

  const std::string mime = response->GetMimeType().ToString();
  if (web_media_should_capture_manifest(uri.c_str(), mime.c_str())) {
    page_has_manifest_.store(true, std::memory_order_release);
    return false; /* handled passively by GetResourceResponseFilter + OnResourceLoadComplete */
  }

  const std::string len_str = response->GetHeaderByName("Content-Length").ToString();
  const std::string range_str = response->GetHeaderByName("Content-Range").ToString();
  const std::string disp_str = response->GetHeaderByName("Content-Disposition").ToString();
  int64_t content_len = !len_str.empty() ? std::strtoll(len_str.c_str(), nullptr, 10) : 0;
  int64_t range_total = !range_str.empty() ? web_media_parse_content_range_total(range_str.c_str()) : 0;
  int64_t eff_size = range_total > 0 ? range_total : content_len;

  KajoWebMediaKind kind = web_media_classify_resource_ex(
      uri.c_str(), mime.c_str(), content_len,
      range_str.empty() ? nullptr : range_str.c_str(),
      page_has_manifest_.load(std::memory_order_acquire) ? TRUE : FALSE);
  if (kind == KAJO_MEDIA_PROGRESSIVE && !detached_.load(std::memory_order_acquire)) {
    TabMediaCapture cap;
    cap.kind = (LowerStr(mime).rfind("audio/", 0) == 0) ? "AUDIO" : "VIDEO";
    cap.uri = uri;
    cap.mime = mime;
    cap.size_bytes = eff_size;
    if (!disp_str.empty()) {
      g_autofree gchar* fn = web_media_parse_content_disposition_filename(disp_str.c_str());
      if (fn) cap.suggested_filename = fn;
    }
    {
      std::lock_guard<std::mutex> mlock(media_mu_);
      auto it = req_headers_.find(request->GetIdentifier());
      if (it != req_headers_.end()) {
        cap.referer = it->second.referer;
        cap.origin = it->second.origin;
        cap.user_agent = it->second.user_agent;
        cap.headers = it->second.headers;
      }
    }
    if (cap.referer.empty()) cap.referer = request->GetReferrerURL().ToString();
    if (cap.user_agent.empty()) cap.user_agent = request->GetHeaderByName("User-Agent").ToString();

    CefRefPtr<CefTabClient> self(this);
    CefPostTask(TID_UI, new UiClosureTask([self, cap = std::move(cap)]() mutable {
      if (TabCallbacks* cb = self->GetCallbacks()) {
        cb->OnTabMediaCaptured(self->tab_id_, cap);
      }
    }));
  }
  return false;
}

void CefTabClient::OnResourceLoadComplete(CefRefPtr<CefBrowser> browser,
                                          CefRefPtr<CefFrame> frame,
                                          CefRefPtr<CefRequest> request,
                                          CefRefPtr<CefResponse> response,
                                          URLRequestStatus status,
                                          int64_t received_content_length) {
  CEF_REQUIRE_IO_THREAD();
  if (!request) return;
  const uint64_t req_id = request->GetIdentifier();
  if (req_id == 0) return;

  CefRefPtr<ManifestCaptureFilter> cap_filter;
  CapturedReqHeaders hdrs;
  bool had_hdrs = false;
  {
    std::lock_guard<std::mutex> mlock(media_mu_);
    auto fit = active_filters_.find(req_id);
    if (fit != active_filters_.end()) {
      cap_filter = static_cast<ManifestCaptureFilter*>(fit->second.get());
      active_filters_.erase(fit);
    }
    auto hit = req_headers_.find(req_id);
    if (hit != req_headers_.end()) {
      hdrs = std::move(hit->second);
      had_hdrs = true;
      req_headers_.erase(hit);
    }
  }

  if (!cap_filter || detached_.load(std::memory_order_acquire) || status != UR_SUCCESS || !response) {
    return;
  }
  if (cap_filter->overflow() || cap_filter->body().empty()) return;

  std::string uri = response->GetURL().ToString();
  if (uri.empty()) uri = request->GetURL().ToString();
  const std::string mime = response->GetMimeType().ToString();
  TabMediaCapture cap;
  cap.uri = uri;
  cap.mime = mime;
  cap.manifest_body = cap_filter->body();
  cap.size_bytes = received_content_length;
  if (LowerStr(uri).find("player.vimeo.com") != std::string::npos) {
    cap.kind = "VIMEO";
  } else if (LowerStr(mime).find("dash") != std::string::npos || LowerStr(uri).find(".mpd") != std::string::npos) {
    cap.kind = "DASH";
  } else {
    cap.kind = "HLS";
  }
  if (had_hdrs) {
    cap.referer = hdrs.referer;
    cap.origin = hdrs.origin;
    cap.user_agent = hdrs.user_agent;
    cap.headers = std::move(hdrs.headers);
  }
  if (cap.referer.empty()) cap.referer = request->GetReferrerURL().ToString();
  if (cap.user_agent.empty()) cap.user_agent = request->GetHeaderByName("User-Agent").ToString();

  CefRefPtr<CefTabClient> self(this);
  CefPostTask(TID_UI, new UiClosureTask([self, cap = std::move(cap)]() mutable {
    if (TabCallbacks* cb = self->GetCallbacks()) {
      cb->OnTabMediaCaptured(self->tab_id_, cap);
    }
  }));
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
  const std::string link = params->GetLinkUrl().ToString();
  const std::string src = params->GetSourceUrl().ToString();
  const std::string page = params->GetPageUrl().ToString();
  const cef_context_menu_media_type_t mtype = params->GetMediaType();
  const bool is_image = (mtype == CM_MEDIATYPE_IMAGE) || (params->HasImageContents() && !src.empty());
  const bool is_av = (mtype == CM_MEDIATYPE_VIDEO || mtype == CM_MEDIATYPE_AUDIO);

  model->AddSeparator();
  if (!link.empty()) {
    model->AddItem(CMD_KAJO_OPEN_LINK_TAB, "Open Link in New Tab");
    model->AddItem(CMD_KAJO_COPY_LINK_URL, "Copy Link Address");
    if (link.rfind("http://", 0) == 0 || link.rfind("https://", 0) == 0 ||
        link.rfind("data:", 0) == 0) {
      model->AddItem(CMD_KAJO_SAVE_LINK_AS, "Save Link As...");
    }
  }

  if (is_image && !src.empty()) {
    if (!link.empty()) model->AddSeparator();
    model->AddItem(CMD_KAJO_OPEN_IMAGE_TAB, "Open Image in New Tab");
    model->AddItem(CMD_KAJO_SAVE_IMAGE_AS, "Save Image As...");
    model->AddItem(CMD_KAJO_COPY_IMAGE, "Copy Image");
    model->AddItem(CMD_KAJO_COPY_IMAGE_URL, "Copy Image Address");
  }

  if (is_av && !src.empty()) {
    if (!link.empty() || is_image) model->AddSeparator();
    model->AddItem(CMD_KAJO_OPEN_MEDIA_TAB, "Open Media in New Tab");
    model->AddItem(CMD_KAJO_SAVE_MEDIA_AS, "Save Media As...");
    model->AddItem(CMD_KAJO_COPY_MEDIA_URL, "Copy Media Address");
    model->AddItem(CMD_KAJO_OPEN_PLAYER, "Play in kajo-player");
  } else {
    std::string target = !src.empty() ? src : (!link.empty() ? link : page);
    if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0 ||
        target.rfind("file://", 0) == 0) {
      model->AddItem(CMD_KAJO_OPEN_PLAYER, "Play in kajo-player");
    }
  }

  const bool can_view_source =
      (page.rfind("http://", 0) == 0 || page.rfind("https://", 0) == 0 ||
       page.rfind("file://", 0) == 0) &&
      page.rfind("view-source:", 0) != 0;
  if (can_view_source && !model->IsVisible(MENU_ID_VIEW_SOURCE)) {
    model->AddItem(CMD_KAJO_VIEW_SOURCE, "View Page Source");
  }

  if (devtools_enabled_.load(std::memory_order_relaxed)) {
    model->AddItem(CMD_KAJO_INSPECT, "Inspect Element");
  }
}

bool CefTabClient::RunContextMenu(CefRefPtr<CefBrowser> /*browser*/,
                                  CefRefPtr<CefFrame> /*frame*/,
                                  CefRefPtr<CefContextMenuParams> params,
                                  CefRefPtr<CefMenuModel> model,
                                  CefRefPtr<CefRunContextMenuCallback> callback) {
  const std::vector<ChromeContextMenuItem> items = SnapshotCefMenuModel(model);
  if (items.empty()) {
    if (callback) callback->Cancel();
    return true;
  }
  if (TabCallbacks* cb = GetCallbacks()) {
    const int x = params ? params->GetXCoord() : 0;
    const int y = params ? params->GetYCoord() : 0;
    return cb->OnTabRunContextMenu(tab_id_, x, y, items, callback);
  }
  return false;
}

void CefTabClient::OnContextMenuDismissed(CefRefPtr<CefBrowser> /*browser*/,
                                          CefRefPtr<CefFrame> /*frame*/) {
  if (TabCallbacks* cb = GetCallbacks()) {
    cb->OnTabContextMenuDismissed(tab_id_);
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
  TabCallbacks* cb = GetCallbacks();
  if (!cb) return false;

  const std::string link = params->GetLinkUrl().ToString();
  const std::string src = params->GetSourceUrl().ToString();
  const std::string page = params->GetPageUrl().ToString();

  switch (command_id) {
    case CMD_KAJO_OPEN_LINK_TAB:
      return !link.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_OPEN_LINK_TAB, link);
    case CMD_KAJO_COPY_LINK_URL:
      return !link.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_COPY_LINK_URL, link);
    case CMD_KAJO_SAVE_LINK_AS:
      return !link.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_SAVE_LINK_AS, link);
    case CMD_KAJO_OPEN_IMAGE_TAB:
      return !src.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_OPEN_IMAGE_TAB, src);
    case CMD_KAJO_SAVE_IMAGE_AS:
      return !src.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_SAVE_IMAGE_AS, src);
    case CMD_KAJO_COPY_IMAGE:
      return !src.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_COPY_IMAGE, src);
    case CMD_KAJO_COPY_IMAGE_URL:
      return !src.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_COPY_IMAGE_URL, src);
    case CMD_KAJO_OPEN_MEDIA_TAB:
      return !src.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_OPEN_MEDIA_TAB, src);
    case CMD_KAJO_SAVE_MEDIA_AS:
      return !src.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_SAVE_MEDIA_AS, src);
    case CMD_KAJO_COPY_MEDIA_URL:
      return !src.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_COPY_MEDIA_URL, src);
    case CMD_KAJO_OPEN_PLAYER: {
      std::string target = !src.empty() ? src : (!link.empty() ? link : page);
      return !target.empty() && cb->OnTabContextCommand(tab_id_, KAJO_CTX_PLAY_IN_PLAYER, target);
    }
    case MENU_ID_VIEW_SOURCE:
    case CMD_KAJO_VIEW_SOURCE:
      if (!page.empty() && page.rfind("view-source:", 0) != 0) {
        return cb->OnTabContextCommand(tab_id_, KAJO_CTX_VIEW_SOURCE, page);
      }
      return true;
    default:
      return false;
  }
}

void CefTabClient::OnGotFocus(CefRefPtr<CefBrowser> browser) {
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabGotFocus(tab_id_);
}

void CefTabClient::PushNotifyPermissionSync(CefRefPtr<CefBrowser> browser, const std::string& url) {
  if (!browser || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) return;
  TabCallbacks* cb = GetCallbacks();
  if (!cb) return;
  CefRefPtr<CefFrame> mf = browser->GetMainFrame();
  if (!mf) return;
  const std::string origin = DeriveOriginFromUrl(url);
  if (origin.empty()) return;
  const std::string perm = cb->QueryTabWebNotifyPermission(tab_id_, url);
  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("kajo-notify-perm-sync");
  CefRefPtr<CefListValue> args = msg->GetArgumentList();
  args->SetString(0, origin);
  args->SetString(1, perm);
  mf->SendProcessMessage(PID_RENDERER, msg);
}

bool CefTabClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                            CefRefPtr<CefFrame> frame,
                                            CefProcessId source_process,
                                            CefRefPtr<CefProcessMessage> message) {
  if (source_process != PID_RENDERER || !message) return false;
  const std::string name = message->GetName().ToString();
  TabCallbacks* cb = GetCallbacks();
  if (!cb) return false;
  CefRefPtr<CefListValue> args = message->GetArgumentList();

  if (name == "kajo-notify-sync-req") {
    std::string url = frame ? frame->GetURL().ToString() : "";
    if (url.empty() && browser && browser->GetMainFrame()) {
      url = browser->GetMainFrame()->GetURL().ToString();
    }
    PushNotifyPermissionSync(browser, url);
    return true;
  }
  if (name == "kajo-notify-show" && args && args->GetSize() >= 5) {
    cb->OnTabWebNotifyShow(tab_id_,
                             args->GetString(0).ToString(),
                             args->GetString(1).ToString(),
                             args->GetString(2).ToString(),
                             args->GetString(3).ToString(),
                             args->GetString(4).ToString());
    return true;
  }
  if (name == "kajo-notify-close" && args && args->GetSize() >= 1) {
    cb->OnTabWebNotifyClose(tab_id_, args->GetString(0).ToString());
    return true;
  }
  if (name == "kajo-notify-req-perm" && args && args->GetSize() >= 2) {
    cb->OnTabWebNotifyRequestPerm(tab_id_, args->GetString(0).ToString(), args->GetInt(1));
    return true;
  }
  if (name == "kajo-webapp-probe-result" && args && args->GetSize() >= 5) {
    cb->OnTabWebAppProbeResult(tab_id_,
                               args->GetString(0).ToString(),
                               args->GetString(1).ToString(),
                               args->GetString(2).ToString(),
                               args->GetString(3).ToString(),
                               args->GetString(4).ToString());
    return true;
  }
  return false;
}

}  // namespace kajo_web
