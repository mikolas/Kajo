#ifndef KAJO_WEB_CEF_MEDIA_DL_H
#define KAJO_WEB_CEF_MEDIA_DL_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "web_cef/cef_chrome_ui.h"

namespace kajo_web {

struct MediaProgressSample {
  int64_t out_time_us = 0;
  int64_t total_size = 0;
  double speed_x = 0.0;
  bool progress_end = false;
};

struct YtDlpFormatOption {
  std::string format_id;
  std::string label;
  std::string ext;
  int width = 0;
  int height = 0;
  int64_t bandwidth = 0;
  int64_t filesize = 0;
  double duration_sec = 0.0;
  bool audio_only = false;
};

struct FfmpegJobSpec {
  std::string ffmpeg_bin = "ffmpeg";
  std::string video_uri;
  std::string audio_uri;
  std::string subtitle_uri;
  std::string out_path;
  std::string input_format;  /* "hls", "dash", or "" */
  std::string dash_rep_id;
  std::string headers_arg;   /* only used for direct non-proxied HTTP URLs */
  std::string user_agent;
  bool mkv_fallback = false;
  bool audio_only = false;
  bool proxied = false;
};

/* Synchronous streaming fetch callback invoked on proxy/download worker threads.
 * Implementations must call on_headers once received (even on non-2xx HTTP),
 * push chunks via on_chunk (stopping early if it returns false), and finish with on_done. */
using MediaFetchStreamFn = std::function<void(
    const std::string& url,
    const std::string& method,
    const std::map<std::string, std::string>& req_headers,
    const std::atomic<bool>* cancel_flag,
    std::function<void(int status, const std::map<std::string, std::string>& resp_headers)> on_headers,
    std::function<bool(const char* data, size_t len)> on_chunk,
    std::function<void(bool ok, const std::string& error)> on_done)>;

/* Pure path/argument/progress/manifest helpers (tested standalone by test-web-cef-core). */
std::string SanitizeMediaFilename(const std::string& raw, const std::string& fallback_stem);
std::string BuildUniqueMediaPath(const std::string& dir,
                                 const std::string& stem,
                                 const std::string& ext);
std::string StripUrlFragment(const std::string& uri);
std::string Base64UrlEncode(const std::string& raw);
std::string Base64UrlDecode(const std::string& encoded);
std::string BuildFfmpegHeaderArg(const std::string& referer, const std::string& cookie_header);
std::vector<std::string> BuildFfmpegRemuxArgs(const std::string& ffmpeg_bin,
                                              const std::string& uri,
                                              const std::string& out_path,
                                              const std::string& headers_arg,
                                              const std::string& user_agent,
                                              bool mkv_fallback);
std::vector<std::string> BuildFfmpegJobArgs(const FfmpegJobSpec& spec);
std::string RewriteHlsManifestForProxy(const std::string& m3u8,
                                       const std::string& base_url,
                                       const std::string& proxy_fetch_prefix);
std::string RewriteDashManifestForProxy(const std::string& mpd,
                                        const std::string& base_url,
                                        const std::string& proxy_rel_prefix,
                                        const std::string& proxy_fetch_prefix);
std::string FormatNetscapeCookieLine(const std::string& domain,
                                     bool include_subdomains,
                                     const std::string& path,
                                     bool secure,
                                     int64_t expires_unix,
                                     const std::string& name,
                                     const std::string& value);
std::vector<YtDlpFormatOption> ParseYtDlpInfoJson(const std::string& json_text,
                                                  std::string* out_title);
bool ParseFfmpegProgressLine(const std::string& line, MediaProgressSample* sample);
bool ParseYtDlpProgressLine(const std::string& line,
                            int* out_percent,
                            int64_t* out_speed_bps,
                            int64_t* out_total_bytes);
bool IsFfmpegAvailable();
bool IsYtDlpAvailable();

/* Ephemeral per-job 127.0.0.1 HTTP loopback proxy bridging native ffmpeg to
 * Chromium's CefURLRequest stack (cookies, TLS, HTTP/2, captured headers). */
class MediaLoopbackProxy {
public:
  explicit MediaLoopbackProxy(MediaFetchStreamFn fetch_fn);
  ~MediaLoopbackProxy();

  MediaLoopbackProxy(const MediaLoopbackProxy&) = delete;
  MediaLoopbackProxy& operator=(const MediaLoopbackProxy&) = delete;

  bool Start();
  void Stop();

  uint16_t port() const { return port_; }
  std::string token() const { return token_; }
  std::string ProxyFetchPrefix() const;
  std::string ProxyRelPrefix() const;
  std::string WrapUrl(const std::string& target_url) const;

private:
  struct WorkerEntry {
    std::thread th;
    std::atomic<bool> done{false};
  };

  void AcceptLoop();
  void PruneFinishedWorkersLocked();
  void HandleClient(int client_fd);

  MediaFetchStreamFn fetch_fn_;
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::string token_;
  std::atomic<bool> stopped_{false};
  std::thread accept_thread_;

  std::mutex fds_mu_;
  std::unordered_set<int> active_fds_;

  std::mutex workers_mu_;
  std::vector<std::unique_ptr<WorkerEntry>> workers_;
};

struct ProxiedStreamRequest {
  std::string video_uri;
  std::string audio_uri;
  std::string subtitle_uri;
  std::string kind; /* "HLS" or "DASH" */
  std::string dash_rep_id;
  std::string suggested_title;
  std::string download_dir;
  double duration_sec = 0.0;
  bool audio_only = false;
};

struct ProgressiveDownloadRequest {
  std::string uri;
  std::string suggested_title;
  std::string suggested_filename;
  std::string mime_type;
  std::string download_dir;
  int64_t expected_size = 0;
};

/* Asynchronous GSubprocess-backed ffmpeg & yt-dlp stream downloader.
 * Emits ChromeDownloadView updates from background worker threads; callers
 * targeting CEF UI state should marshal updates via CefPostTask(TID_UI, ...).
 * Destructor / CancelAll() forces termination and joins all threads cleanly. */
class MediaDownloadManager {
public:
  using ProgressCb = std::function<void(const ChromeDownloadView& view)>;
  using YtDlpProbeCb = std::function<void(bool ok,
                                          std::string title,
                                          std::vector<YtDlpFormatOption> formats,
                                          std::string error)>;

  explicit MediaDownloadManager(ProgressCb cb);
  ~MediaDownloadManager();

  MediaDownloadManager(const MediaDownloadManager&) = delete;
  MediaDownloadManager& operator=(const MediaDownloadManager&) = delete;

  uint32_t StartFfmpegStream(const std::string& uri,
                             const std::string& suggested_title,
                             const std::string& download_dir,
                             const std::string& referer,
                             const std::string& user_agent,
                             const std::string& cookie_header);

  uint32_t StartProxiedStream(const ProxiedStreamRequest& req,
                              MediaFetchStreamFn fetcher);

  uint32_t StartProgressiveDownload(const ProgressiveDownloadRequest& req,
                                    MediaFetchStreamFn fetcher);

  uint32_t StartYtDlp(const std::string& page_url,
                      const std::string& suggested_title,
                      const std::string& download_dir,
                      const std::string& user_agent,
                      const std::string& referer = "",
                      const std::string& cookies_file = "",
                      const std::string& format_id = "",
                      bool audio_only = false);

  void ProbeYtDlpAsync(const std::string& page_url,
                       const std::string& user_agent,
                       const std::string& referer,
                       const std::string& cookies_file,
                       YtDlpProbeCb cb);

  bool HasJob(uint32_t id) const;
  bool Cancel(uint32_t id);
  void CancelAll();

private:
  struct Job;

  uint32_t AllocateIdLocked();
  std::string ResolveDownloadDir(const std::string& requested) const;
  void ReapFinishedLocked();
  void RunFfmpegJob(const std::shared_ptr<Job>& job,
                    const std::string& display_url,
                    const std::string& dir,
                    const std::string& stem,
                    double duration_sec,
                    FfmpegJobSpec spec,
                    std::shared_ptr<MediaLoopbackProxy> proxy);
  void RunProgressiveWorker(const std::shared_ptr<Job>& job,
                            ProgressiveDownloadRequest req,
                            std::string dir,
                            std::string out_path,
                            MediaFetchStreamFn fetcher);
  void RunYtDlpWorker(const std::shared_ptr<Job>& job,
                      std::string page_url,
                      std::string dir,
                      std::string stem,
                      std::string user_agent,
                      std::string referer,
                      std::string cookies_file,
                      std::string format_id,
                      bool audio_only);
  void EmitUpdate(const ChromeDownloadView& dl);

  ProgressCb cb_;
  mutable std::mutex mu_;
  uint32_t next_id_ = 0x80000001u;
  std::map<uint32_t, std::shared_ptr<Job>> jobs_;
};

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_MEDIA_DL_H
