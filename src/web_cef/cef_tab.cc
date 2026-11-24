#include "web_cef/cef_tab.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <functional>

#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"
#include "web/web_media_sniff_core.h"

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
         "<a class='btn' href='" + HtmlEscape(url) + "'>RETRY</a></div></body></html>";
}

}  // namespace

CefTabClient::CefTabClient(uint64_t tab_id,
                           CefAdblockEngine* adblock,
                           TabCallbacks* callbacks)
    : tab_id_(tab_id), adblock_(adblock), callbacks_(callbacks) {}

void CefTabClient::DetachCallbacks() {
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
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabBrowserCreated(tab_id_, browser);
}

void CefTabClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
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
  if (url.rfind("kajo://", 0) == 0) {
    if (TabCallbacks* cb = GetCallbacks()) {
      if (cb->OnTabBeforeBrowseAction(tab_id_, url)) return true;
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

CefResourceRequestHandler::ReturnValue CefTabClient::OnBeforeResourceLoad(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefCallback> callback) {
  CEF_REQUIRE_IO_THREAD();
  if (!request) return RV_CONTINUE;

  const std::string url = request->GetURL().ToString();
  if (https_only_.load(std::memory_order_relaxed) && url.rfind("http://", 0) == 0 &&
      request->GetResourceType() == ::RT_MAIN_FRAME) {
    const std::string host = CefAdblockEngine::ExtractHost(url);
    if (host != "localhost" && host != "127.0.0.1" && host != "::1") {
      request->SetURL("https://" + url.substr(7));
    }
  }

  if (adblock_) {
    std::string first_party;
    if (frame) {
      first_party = frame->GetURL().ToString();
    }
    if (first_party.empty()) {
      std::lock_guard<std::mutex> lock(url_mu_);
      first_party = last_top_url_;
    }
    if (first_party.empty()) {
      first_party = request->GetReferrerURL().ToString();
    }
    ResourceTypeMask rt = MapCefResourceType(request->GetResourceType());
    if (adblock_->ShouldBlock(url, first_party, rt)) {
      adblock_->RecordBlockedForTab(static_cast<int>(tab_id_));
      CefRefPtr<CefTabClient> self(this);
      CefPostTask(TID_UI, new UiClosureTask([self]() {
        if (TabCallbacks* cb = self->GetCallbacks()) cb->OnTabBlockedChanged(self->tab_id_);
      }));
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
  if (!request || !response) return false;
  const std::string uri = request->GetURL().ToString();
  if (uri.rfind("http://", 0) != 0 && uri.rfind("https://", 0) != 0) return false;

  const std::string mime = response->GetMimeType().ToString();
  const std::string len_str = response->GetHeaderByName("Content-Length").ToString();
  int64_t content_len = 0;
  if (!len_str.empty()) {
    content_len = std::strtoll(len_str.c_str(), nullptr, 10);
  }
  KajoWebMediaKind kind = web_media_classify_resource(uri.c_str(), mime.c_str(), content_len);
  if (kind != KAJO_MEDIA_NONE) {
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
  dl.in_progress = download_item->IsInProgress();
  dl.complete = download_item->IsComplete();
  dl.canceled = download_item->IsCanceled();
  dl.paused = download_item->IsPaused();

  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabDownloadUpdated(dl, callback);
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
  model->AddItem(CMD_KAJO_INSPECT, "Inspect Element");
}

bool CefTabClient::OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        CefRefPtr<CefContextMenuParams> params,
                                        int command_id,
                                        EventFlags event_flags) {
  if (!params) return false;
  if (command_id == CMD_KAJO_INSPECT && browser && browser->GetHost()) {
    CefWindowInfo wi;
    CefBrowserSettings bs;
    CefPoint pt(params->GetXCoord(), params->GetYCoord());
    browser->GetHost()->ShowDevTools(wi, nullptr, bs, pt);
    return true;
  }
  if (command_id == CMD_KAJO_OPEN_LINK_TAB || command_id == CMD_KAJO_OPEN_PLAYER) {
    std::string target = params->GetSourceUrl().ToString();
    if (target.empty()) target = params->GetLinkUrl().ToString();
    if (target.empty()) target = params->GetPageUrl().ToString();
    if (TabCallbacks* cb = GetCallbacks()) {
      return cb->OnTabContextCommand(tab_id_, command_id == CMD_KAJO_OPEN_LINK_TAB ? 1 : 2, target);
    }
  }
  return false;
}

void CefTabClient::OnGotFocus(CefRefPtr<CefBrowser> browser) {
  if (TabCallbacks* cb = GetCallbacks()) cb->OnTabGotFocus(tab_id_);
}

}  // namespace kajo_web
