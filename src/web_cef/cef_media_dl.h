#ifndef KAJO_WEB_CEF_MEDIA_DL_H
#define KAJO_WEB_CEF_MEDIA_DL_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "web_cef/cef_chrome_ui.h"

namespace kajo_web {

struct MediaProgressSample {
  int64_t out_time_us = 0;
  int64_t total_size = 0;
  double speed_x = 0.0;
  bool progress_end = false;
};

/* Pure path/argument/progress helpers (tested standalone by test-web-cef-core). */
std::string SanitizeMediaFilename(const std::string& raw, const std::string& fallback_stem);
std::string BuildUniqueMediaPath(const std::string& dir,
                                 const std::string& stem,
                                 const std::string& ext);
std::string BuildFfmpegHeaderArg(const std::string& referer, const std::string& cookie_header);
std::vector<std::string> BuildFfmpegRemuxArgs(const std::string& ffmpeg_bin,
                                              const std::string& uri,
                                              const std::string& out_path,
                                              const std::string& headers_arg,
                                              const std::string& user_agent,
                                              bool mkv_fallback);
bool ParseFfmpegProgressLine(const std::string& line, MediaProgressSample* sample);
bool ParseYtDlpProgressLine(const std::string& line,
                            int* out_percent,
                            int64_t* out_speed_bps,
                            int64_t* out_total_bytes);
bool IsFfmpegAvailable();
bool IsYtDlpAvailable();

/* Asynchronous GSubprocess-backed ffmpeg & yt-dlp stream downloader.
 * Emits ChromeDownloadView updates from background worker threads; callers
 * targeting CEF UI state should marshal updates via CefPostTask(TID_UI, ...).
 * Destructor / CancelAll() forces subprocess termination and joins all threads. */
class MediaDownloadManager {
public:
  using ProgressCb = std::function<void(const ChromeDownloadView& view)>;

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

  uint32_t StartYtDlp(const std::string& page_url,
                      const std::string& suggested_title,
                      const std::string& download_dir,
                      const std::string& user_agent);

  bool HasJob(uint32_t id) const;
  bool Cancel(uint32_t id);
  void CancelAll();

private:
  struct Job;

  void ReapFinishedLocked();
  void RunFfmpegWorker(const std::shared_ptr<Job>& job,
                       std::string uri,
                       std::string dir,
                       std::string stem,
                       std::string referer,
                       std::string user_agent,
                       std::string cookie_header);
  void RunYtDlpWorker(const std::shared_ptr<Job>& job,
                      std::string page_url,
                      std::string dir,
                      std::string stem,
                      std::string user_agent);
  void EmitUpdate(const ChromeDownloadView& dl);

  ProgressCb cb_;
  mutable std::mutex mu_;
  uint32_t next_id_ = 0x80000001u;
  std::map<uint32_t, std::shared_ptr<Job>> jobs_;
};

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_MEDIA_DL_H
