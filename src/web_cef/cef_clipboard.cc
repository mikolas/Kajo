#include "web_cef/cef_clipboard.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <vector>

#include "include/cef_request.h"
#include "include/cef_task.h"
#include "include/cef_urlrequest.h"
#include "include/wrapper/cef_helpers.h"
#include "web_cef/cef_chrome_ui.h"

namespace kajo_web {

namespace {

std::string NormalizeImageMime(const std::string& raw, const std::string& url) {
  std::string m = raw;
  size_t semi = m.find(';');
  if (semi != std::string::npos) m = m.substr(0, semi);
  for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (m.rfind("image/", 0) == 0) return m;

  std::string u = url;
  size_t q = u.find_first_of("?#");
  if (q != std::string::npos) u = u.substr(0, q);
  for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto ends_with = [&](const char* s) {
    size_t sl = std::char_traits<char>::length(s);
    return u.size() >= sl && u.compare(u.size() - sl, sl, s) == 0;
  };
  if (ends_with(".jpg") || ends_with(".jpeg")) return "image/jpeg";
  if (ends_with(".webp")) return "image/webp";
  if (ends_with(".gif")) return "image/gif";
  if (ends_with(".svg")) return "image/svg+xml";
  if (ends_with(".avif")) return "image/avif";
  return "image/png";
}

bool DecodeImageDataUri(const std::string& uri,
                        std::string* out_mime,
                        std::vector<uint8_t>* out_bytes) {
  if (uri.rfind("data:", 0) != 0) return false;
  size_t comma = uri.find(',');
  if (comma == std::string::npos) return false;
  const std::string meta = uri.substr(5, comma - 5);
  const std::string payload = uri.substr(comma + 1);
  const bool is_b64 = meta.find(";base64") != std::string::npos;
  std::string mime = meta;
  size_t semi = mime.find(';');
  if (semi != std::string::npos) mime = mime.substr(0, semi);
  if (mime.rfind("image/", 0) != 0) return false;
  if (!is_b64) return false;

  gsize dec_len = 0;
  g_autofree guchar* raw = g_base64_decode(payload.c_str(), &dec_len);
  if (!raw || dec_len == 0 || dec_len > 32U * 1024U * 1024U) return false;
  *out_mime = mime;
  out_bytes->assign(raw, raw + dec_len);
  return true;
}

void WriteImageBytesToClipboardJs(CefRefPtr<CefBrowser> browser,
                                  const std::vector<uint8_t>& bytes,
                                  const std::string& mime,
                                  const std::function<void(bool ok)>& on_done) {
  if (!browser || !browser->GetMainFrame() || bytes.empty()) {
    if (on_done) on_done(false);
    return;
  }
  g_autofree gchar* b64 = g_base64_encode(bytes.data(), bytes.size());
  if (!b64) {
    if (on_done) on_done(false);
    return;
  }
  const std::string safe_mime = JsonEscape(mime.empty() ? "image/png" : mime);
  const std::string js =
      "(async () => {"
      "  try {"
      "    const b64 = \"" + std::string(b64) + "\";"
      "    const mime = \"" + safe_mime + "\";"
      "    const bin = atob(b64);"
      "    const arr = new Uint8Array(bin.length);"
      "    for (let i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i);"
      "    let pngBlob = new Blob([arr], { type: mime });"
      "    if (mime !== 'image/png') {"
      "      let bmp = null;"
      "      try { bmp = await createImageBitmap(pngBlob); } catch (_) {}"
      "      if (!bmp) {"
      "        const objUrl = URL.createObjectURL(pngBlob);"
      "        bmp = await new Promise((resolve, reject) => {"
      "          const im = new Image();"
      "          im.onload = () => { URL.revokeObjectURL(objUrl); resolve(im); };"
      "          im.onerror = (e) => { URL.revokeObjectURL(objUrl); reject(e); };"
      "          im.src = objUrl;"
      "        });"
      "      }"
      "      const c = document.createElement('canvas');"
      "      c.width = bmp.width || 1;"
      "      c.height = bmp.height || 1;"
      "      c.getContext('2d').drawImage(bmp, 0, 0);"
      "      if (typeof bmp.close === 'function') bmp.close();"
      "      pngBlob = await new Promise((res, rej) => c.toBlob((b) => b ? res(b) : rej(), 'image/png'));"
      "    }"
      "    await navigator.clipboard.write([new ClipboardItem({ 'image/png': pngBlob })]);"
      "  } catch (_) {}"
      "})();";
  browser->GetMainFrame()->ExecuteJavaScript(js, browser->GetMainFrame()->GetURL(), 0);
  if (on_done) on_done(true);
}

class ImageClipboardFetchClient : public CefURLRequestClient {
public:
  ImageClipboardFetchClient(CefRefPtr<CefBrowser> browser,
                            std::string url,
                            std::function<void(bool ok)> on_done)
      : browser_(std::move(browser)),
        url_(std::move(url)),
        on_done_(std::move(on_done)) {}

  void AttachRequest(CefRefPtr<CefURLRequest> req) { req_ = std::move(req); }

  void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
    CEF_REQUIRE_UI_THREAD();
    const int status = (request && request->GetResponse())
                           ? request->GetResponse()->GetStatus()
                           : 0;
    const std::string raw_mime = (request && request->GetResponse())
                                     ? request->GetResponse()->GetMimeType().ToString()
                                     : "";
    const bool ok = request && request->GetRequestStatus() == UR_SUCCESS &&
                    status >= 200 && status < 300 && !body_.empty();
    if (ok) {
      WriteImageBytesToClipboardJs(
          browser_, body_, NormalizeImageMime(raw_mime, url_), on_done_);
    } else if (on_done_) {
      on_done_(false);
    }
    on_done_ = nullptr;
    browser_ = nullptr;
    req_ = nullptr;
  }

  void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
  void OnDownloadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
  void OnDownloadData(CefRefPtr<CefURLRequest>, const void* data, size_t len) override {
    if (data && len > 0 && body_.size() + len <= 32U * 1024U * 1024U) {
      const auto* p = static_cast<const uint8_t*>(data);
      body_.insert(body_.end(), p, p + len);
    }
  }
  bool GetAuthCredentials(bool, const CefString&, int, const CefString&,
                          const CefString&, CefRefPtr<CefAuthCallback>) override {
    return false;
  }

private:
  CefRefPtr<CefBrowser> browser_;
  CefRefPtr<CefURLRequest> req_;
  std::string url_;
  std::function<void(bool ok)> on_done_;
  std::vector<uint8_t> body_;
  IMPLEMENT_REFCOUNTING(ImageClipboardFetchClient);
};

}  // namespace

void CopyTextViaBrowser(CefRefPtr<CefBrowser> browser, const std::string& text) {
  if (!browser || !browser->GetMainFrame() || text.empty()) return;
  const std::string escaped = JsonEscape(text);
  const std::string js =
      "(() => {"
      "  const t = \"" + escaped + "\";"
      "  if (navigator.clipboard && navigator.clipboard.writeText) {"
      "    navigator.clipboard.writeText(t).catch(() => {});"
      "    return;"
      "  }"
      "  const ta = document.createElement('textarea');"
      "  ta.value = t;"
      "  ta.style.position = 'fixed'; ta.style.left = '-9999px';"
      "  document.body && document.body.appendChild(ta);"
      "  ta.select();"
      "  try { document.execCommand('copy'); } catch (_) {}"
      "  ta.remove();"
      "})();";
  browser->GetMainFrame()->ExecuteJavaScript(js, browser->GetMainFrame()->GetURL(), 0);
}

void CopyImageViaBrowser(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefRequestContext> ctx,
                         const std::string& image_url,
                         std::function<void(bool ok)> on_done) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser || image_url.empty()) {
    if (on_done) on_done(false);
    return;
  }
  if (image_url.rfind("data:", 0) == 0) {
    std::string mime;
    std::vector<uint8_t> bytes;
    if (DecodeImageDataUri(image_url, &mime, &bytes)) {
      WriteImageBytesToClipboardJs(browser, bytes, mime, on_done);
    } else if (on_done) {
      on_done(false);
    }
    return;
  }
  if ((image_url.rfind("http://", 0) != 0 && image_url.rfind("https://", 0) != 0) || !ctx) {
    if (on_done) on_done(false);
    return;
  }

  CefRefPtr<CefRequest> req = CefRequest::Create();
  req->SetURL(image_url);
  req->SetMethod("GET");
  CefRefPtr<ImageClipboardFetchClient> client(
      new ImageClipboardFetchClient(browser, image_url, std::move(on_done)));
  CefRefPtr<CefURLRequest> ureq = CefURLRequest::Create(req, client, ctx);
  if (!ureq) {
    client->OnRequestComplete(nullptr);
    return;
  }
  client->AttachRequest(ureq);
}

}  // namespace kajo_web
