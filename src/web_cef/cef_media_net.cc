#include "web_cef/cef_media_net.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

#include "include/cef_cookie.h"
#include "include/cef_request.h"
#include "include/cef_response.h"
#include "include/cef_task.h"
#include "include/cef_urlrequest.h"
#include "include/internal/cef_time.h"

namespace kajo_web {

namespace {

std::string LowerAscii(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

class MediaUiTask : public CefTask {
public:
  explicit MediaUiTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override {
    if (fn_) fn_();
  }

private:
  std::function<void()> fn_;
  IMPLEMENT_REFCOUNTING(MediaUiTask);
};

struct StreamBridgeState {
  std::mutex mu;
  std::condition_variable cv;
  bool headers_ready = false;
  int status = 0;
  std::map<std::string, std::string> resp_headers;
  std::deque<std::string> chunks;
  size_t buffered_bytes = 0;
  bool finished = false;
  bool ok = false;
  std::string error;
  std::atomic<bool> aborted{false};
  CefRefPtr<CefURLRequest> req; /* accessed only on TID_UI */
};

class BridgeUrlRequestClient : public CefURLRequestClient {
public:
  explicit BridgeUrlRequestClient(std::shared_ptr<StreamBridgeState> state)
      : state_(std::move(state)) {}

  void AttachRequest(CefRefPtr<CefURLRequest> req) {
    if (state_) state_->req = std::move(req);
  }

  void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
  void OnDownloadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}

  void OnDownloadData(CefRefPtr<CefURLRequest> request,
                      const void* data,
                      size_t data_length) override {
    if (!state_) return;
    if (state_->aborted.load(std::memory_order_acquire)) {
      if (state_->req) state_->req->Cancel();
      return;
    }
    std::lock_guard<std::mutex> lock(state_->mu);
    EnsureHeadersLocked(request);
    if (data && data_length > 0 && state_->buffered_bytes < 64U * 1024U * 1024U) {
      state_->chunks.emplace_back(static_cast<const char*>(data), data_length);
      state_->buffered_bytes += data_length;
    }
    state_->cv.notify_all();
  }

  void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mu);
    EnsureHeadersLocked(request);
    const auto st = request ? request->GetRequestStatus() : UR_FAILED;
    state_->ok = (st == UR_SUCCESS);
    if (!state_->ok && state_->error.empty()) {
      const int err_code = request ? static_cast<int>(request->GetRequestError()) : -1;
      state_->error = "CEF URLRequest failed (" + std::to_string(err_code) + ")";
    }
    state_->finished = true;
    state_->req = nullptr;
    state_->cv.notify_all();
  }

  bool GetAuthCredentials(bool,
                          const CefString&,
                          int,
                          const CefString&,
                          const CefString&,
                          CefRefPtr<CefAuthCallback>) override {
    return false;
  }

private:
  void EnsureHeadersLocked(const CefRefPtr<CefURLRequest>& request) {
    if (state_->headers_ready || !request) return;
    CefRefPtr<CefResponse> resp = request->GetResponse();
    if (!resp) return;
    state_->headers_ready = true;
    state_->status = resp->GetStatus();
    CefResponse::HeaderMap hmap;
    resp->GetHeaderMap(hmap);
    for (const auto& kv : hmap) {
      const std::string k = kv.first.ToString();
      const std::string v = kv.second.ToString();
      if (!k.empty()) state_->resp_headers[k] = v;
    }
    if (state_->resp_headers.find("Content-Type") == state_->resp_headers.end() &&
        state_->resp_headers.find("content-type") == state_->resp_headers.end()) {
      const std::string mime = resp->GetMimeType().ToString();
      if (!mime.empty()) state_->resp_headers["Content-Type"] = mime;
    }
  }

  std::shared_ptr<StreamBridgeState> state_;
  IMPLEMENT_REFCOUNTING(BridgeUrlRequestClient);
};

class NetscapeCookieExportVisitor : public CefCookieVisitor {
public:
  using DoneCb = std::function<void(std::string temp_cookie_file)>;

  explicit NetscapeCookieExportVisitor(DoneCb cb) : cb_(std::move(cb)) {}

  ~NetscapeCookieExportVisitor() override {
    FinishNow();
  }

  bool Visit(const CefCookie& cookie,
             int count,
             int total,
             bool& deleteCookie) override {
    deleteCookie = false;
    const std::string name = CefString(&cookie.name).ToString();
    const std::string val = CefString(&cookie.value).ToString();
    const std::string domain = CefString(&cookie.domain).ToString();
    const std::string path = CefString(&cookie.path).ToString();
    if (!name.empty() && !domain.empty()) {
      int64_t exp_unix = 0;
      if (cookie.has_expires) {
        cef_time_t ct{};
        if (cef_time_from_basetime(cookie.expires, &ct)) {
          g_autoptr(GDateTime) dt = g_date_time_new_utc(
              ct.year, ct.month, ct.day_of_month, ct.hour, ct.minute, ct.second);
          if (dt) exp_unix = g_date_time_to_unix(dt);
        }
      }
      const bool sub = (!domain.empty() && domain.front() == '.');
      lines_.push_back(FormatNetscapeCookieLine(
          domain, sub, path, cookie.secure != 0, exp_unix, name, val));
    }
    if (count + 1 >= total) {
      FinishNow();
      return false;
    }
    return true;
  }

private:
  void FinishNow() {
    if (fired_ || !cb_) return;
    fired_ = true;
    auto cb = std::move(cb_);
    std::string temp_path;
    if (!lines_.empty()) {
      const gchar* base_dir = g_get_user_runtime_dir();
      if (!base_dir || !base_dir[0]) base_dir = g_get_tmp_dir();
      g_autofree gchar* tmpl = g_build_filename(base_dir, "kajo-web-ck-XXXXXX.txt", nullptr);
      int fd = tmpl ? g_mkstemp_full(tmpl, 0, 0600) : -1;
      if (fd >= 0) {
        std::string payload = "# Netscape HTTP Cookie File\n";
        for (const auto& l : lines_) {
          payload += l;
          payload.push_back('\n');
        }
        size_t written = 0;
        bool write_ok = true;
        while (written < payload.size()) {
          ssize_t n = ::write(fd, payload.data() + written, payload.size() - written);
          if (n > 0) written += static_cast<size_t>(n);
          else {
            write_ok = false;
            break;
          }
        }
        ::close(fd);
        if (write_ok) {
          temp_path = tmpl;
        } else {
          g_unlink(tmpl);
        }
      }
    }
    CefPostTask(TID_UI, new MediaUiTask([cb = std::move(cb), temp_path = std::move(temp_path)]() mutable {
      cb(std::move(temp_path));
    }));
  }

  DoneCb cb_;
  std::vector<std::string> lines_;
  bool fired_ = false;
  IMPLEMENT_REFCOUNTING(NetscapeCookieExportVisitor);
};

}  // namespace

MediaFetchStreamFn CreateCefMediaFetcher(CefRefPtr<CefRequestContext> ctx,
                                         std::map<std::string, std::string> captured_headers,
                                         std::string referer,
                                         std::string user_agent) {
  return [ctx, captured_headers = std::move(captured_headers),
          referer = std::move(referer), user_agent = std::move(user_agent)](
             const std::string& url,
             const std::string& method,
             const std::map<std::string, std::string>& req_headers,
             const std::atomic<bool>* cancel_flag,
             std::function<void(int, const std::map<std::string, std::string>&)> on_headers,
             std::function<bool(const char*, size_t)> on_chunk,
             std::function<void(bool, const std::string&)> on_done) {
    if (!ctx || url.empty() || (cancel_flag && cancel_flag->load(std::memory_order_acquire))) {
      if (on_done) on_done(false, "Request canceled");
      return;
    }

    auto state = std::make_shared<StreamBridgeState>();
    CefRefPtr<BridgeUrlRequestClient> client(new BridgeUrlRequestClient(state));

    /* Build merged request header map */
    CefRequest::HeaderMap hmap;
    auto put_hdr = [&](const std::string& k, const std::string& v) {
      const std::string lk = LowerAscii(k);
      if (lk.empty() || v.empty()) return;
      if (lk == "host" || lk == "content-length" || lk == "connection" ||
          lk == "accept-encoding" || lk == "referer") {
        return;
      }
      /* Replace any previous entry with same lowercase key */
      for (auto it = hmap.begin(); it != hmap.end();) {
        if (LowerAscii(it->first.ToString()) == lk) it = hmap.erase(it);
        else ++it;
      }
      hmap.emplace(k, v);
    };

    for (const auto& kv : captured_headers) put_hdr(kv.first, kv.second);
    if (!user_agent.empty()) put_hdr("User-Agent", user_agent);
    for (const auto& kv : req_headers) put_hdr(kv.first, kv.second);

    std::string eff_method = method.empty() ? "GET" : method;
    CefPostTask(TID_UI, new MediaUiTask([ctx, state, client, url, eff_method, referer, hmap]() {
      if (state->aborted.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->finished = true;
        state->error = "Canceled";
        state->cv.notify_all();
        return;
      }
      CefRefPtr<CefRequest> req = CefRequest::Create();
      req->SetURL(url);
      req->SetMethod(eff_method);
      req->SetFlags(UR_FLAG_NONE);
      if (!referer.empty()) {
        req->SetReferrer(referer, REFERRER_POLICY_DEFAULT);
      }
      if (!hmap.empty()) {
        req->SetHeaderMap(hmap);
      }
      CefRefPtr<CefURLRequest> ureq = CefURLRequest::Create(req, client, ctx);
      if (!ureq) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->finished = true;
        state->error = "Failed to create CefURLRequest";
        state->cv.notify_all();
        return;
      }
      client->AttachRequest(ureq);
    }));

    bool dispatched_headers = false;
    bool chunk_aborted = false;
    for (;;) {
      std::unique_lock<std::mutex> lock(state->mu);
      state->cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
        return (!dispatched_headers && (state->headers_ready || state->finished)) ||
               !state->chunks.empty() || state->finished ||
               (cancel_flag && cancel_flag->load(std::memory_order_acquire));
      });

      if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
        state->aborted.store(true, std::memory_order_release);
        CefPostTask(TID_UI, new MediaUiTask([state]() {
          if (state->req) state->req->Cancel();
        }));
        lock.unlock();
        if (on_done) on_done(false, "Canceled");
        return;
      }

      if (!dispatched_headers && (state->headers_ready || state->finished)) {
        dispatched_headers = true;
        int st = state->headers_ready ? state->status : 502;
        auto rh = state->resp_headers;
        lock.unlock();
        if (on_headers) on_headers(st, rh);
        lock.lock();
      }

      while (!state->chunks.empty() && !chunk_aborted) {
        std::string chunk = std::move(state->chunks.front());
        state->chunks.pop_front();
        if (state->buffered_bytes >= chunk.size()) state->buffered_bytes -= chunk.size();
        else state->buffered_bytes = 0;
        lock.unlock();
        bool keep = on_chunk ? on_chunk(chunk.data(), chunk.size()) : true;
        lock.lock();
        if (!keep) {
          chunk_aborted = true;
          state->aborted.store(true, std::memory_order_release);
          CefPostTask(TID_UI, new MediaUiTask([state]() {
            if (state->req) state->req->Cancel();
          }));
          break;
        }
      }

      if (chunk_aborted || (state->finished && state->chunks.empty())) {
        const bool ok = !chunk_aborted && state->ok;
        const std::string err = state->error;
        lock.unlock();
        if (on_done) on_done(ok, err);
        return;
      }
    }
  };
}

void ExportNetscapeCookiesAsync(CefRefPtr<CefRequestContext> ctx,
                                const std::string& page_url,
                                std::function<void(std::string temp_cookie_file)> cb) {
  if (!ctx || page_url.empty()) {
    if (cb) cb("");
    return;
  }
  CefRefPtr<CefCookieManager> cm = ctx->GetCookieManager(nullptr);
  if (!cm) {
    if (cb) cb("");
    return;
  }
  CefRefPtr<NetscapeCookieExportVisitor> visitor(new NetscapeCookieExportVisitor(cb));
  if (!cm->VisitUrlCookies(page_url, true, visitor)) {
    if (cb) cb("");
  }
}

}  // namespace kajo_web
