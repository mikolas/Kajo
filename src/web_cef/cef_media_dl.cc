#include "web_cef/cef_media_dl.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace kajo_web {

namespace {

std::string StripCrLf(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    if (c != '\r' && c != '\n') out.push_back(c);
  }
  return out;
}

int64_t ParseUnitBytes(double val, const std::string& unit) {
  if (val <= 0.0) return 0;
  std::string u;
  for (char c : unit) u.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  double mult = 1.0;
  if (u == "kib" || u == "kib/s") mult = 1024.0;
  else if (u == "mib" || u == "mib/s") mult = 1024.0 * 1024.0;
  else if (u == "gib" || u == "gib/s") mult = 1024.0 * 1024.0 * 1024.0;
  else if (u == "kb" || u == "kb/s") mult = 1000.0;
  else if (u == "mb" || u == "mb/s") mult = 1000.0 * 1000.0;
  else if (u == "gb" || u == "gb/s") mult = 1000.0 * 1000.0 * 1000.0;
  return static_cast<int64_t>(val * mult);
}

int64_t FileSizeBytes(const std::string& path) {
  if (path.empty()) return 0;
  GStatBuf st{};
  if (g_stat(path.c_str(), &st) == 0 && st.st_size > 0) {
    return static_cast<int64_t>(st.st_size);
  }
  return 0;
}

}  // namespace

struct MediaDownloadManager::Job {
  uint32_t id = 0;
  std::atomic<bool> canceled{false};
  std::atomic<bool> finished{false};
  std::mutex proc_mu;
  GSubprocess* proc = nullptr;
  std::thread worker;

  ~Job() {
    if (proc) {
      g_object_unref(proc);
      proc = nullptr;
    }
  }
};

std::string SanitizeMediaFilename(const std::string& raw, const std::string& fallback_stem) {
  std::string s = raw;
  size_t q = s.find_first_of("?#");
  if (q != std::string::npos && (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0)) {
    s = s.substr(0, q);
  }
  size_t slash = s.find_last_of("/\\");
  if (slash != std::string::npos &&
      (raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0 || raw.rfind("file://", 0) == 0)) {
    s = s.substr(slash + 1);
  }
  /* Strip a trailing manifest/media extension if present so we don't produce foo.m3u8.mp4 */
  for (const char* ext : {".m3u8", ".mpd", ".mp4", ".webm", ".mkv", ".ts"}) {
    const size_t elen = std::strlen(ext);
    if (s.size() > elen && g_ascii_strcasecmp(s.c_str() + s.size() - elen, ext) == 0) {
      s.resize(s.size() - elen);
      break;
    }
  }

  std::string out;
  out.reserve(s.size());
  for (unsigned char ch : s) {
    if (ch < 32 || ch == 127) continue;
    if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
        ch == '"' || ch == '<' || ch == '>' || ch == '|') {
      out.push_back('_');
    } else {
      out.push_back(static_cast<char>(ch));
    }
  }
  while (!out.empty() && (out.front() == ' ' || out.front() == '.' || out.front() == '_')) {
    out.erase(out.begin());
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.' || out.back() == '_')) {
    out.pop_back();
  }
  if (out.size() > 110) out.resize(110);
  if (out.empty()) return fallback_stem.empty() ? "video" : fallback_stem;
  return out;
}

std::string BuildUniqueMediaPath(const std::string& dir,
                                 const std::string& stem,
                                 const std::string& ext) {
  const std::string clean_stem = SanitizeMediaFilename(stem, "video");
  const std::string clean_ext = (!ext.empty() && ext.front() == '.') ? ext.substr(1) : ext;
  const std::string suffix = clean_ext.empty() ? "" : ("." + clean_ext);
  const std::string base_dir = dir.empty() ? "." : dir;

  for (int idx = 0; idx < 1000; ++idx) {
    std::string fname = (idx == 0)
                            ? (clean_stem + suffix)
                            : (clean_stem + "-" + std::to_string(idx) + suffix);
    g_autofree gchar* joined = g_build_filename(base_dir.c_str(), fname.c_str(), nullptr);
    if (joined && !g_file_test(joined, G_FILE_TEST_EXISTS)) {
      return std::string(joined);
    }
  }
  g_autofree gchar* fallback = g_build_filename(
      base_dir.c_str(), (clean_stem + "-" + std::to_string(g_get_real_time()) + suffix).c_str(),
      nullptr);
  return fallback ? std::string(fallback) : (clean_stem + suffix);
}

std::string BuildFfmpegHeaderArg(const std::string& referer, const std::string& cookie_header) {
  std::string out;
  const std::string ref = StripCrLf(referer);
  const std::string ck = StripCrLf(cookie_header);
  if (!ref.empty()) out += "Referer: " + ref + "\r\n";
  if (!ck.empty()) out += "Cookie: " + ck + "\r\n";
  return out;
}

std::vector<std::string> BuildFfmpegRemuxArgs(const std::string& ffmpeg_bin,
                                              const std::string& uri,
                                              const std::string& out_path,
                                              const std::string& headers_arg,
                                              const std::string& user_agent,
                                              bool mkv_fallback) {
  std::vector<std::string> args = {
      ffmpeg_bin.empty() ? "ffmpeg" : ffmpeg_bin,
      "-y",
      "-nostdin",
      "-hide_banner",
      "-loglevel",
      "error",
      "-progress",
      "pipe:1",
  };
  if (uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0) {
    if (!user_agent.empty()) {
      args.emplace_back("-user_agent");
      args.push_back(StripCrLf(user_agent));
    }
    if (!headers_arg.empty()) {
      args.emplace_back("-headers");
      args.push_back(headers_arg);
    }
  }
  args.emplace_back("-i");
  args.push_back(uri);
  args.emplace_back("-c");
  args.emplace_back("copy");
  if (!mkv_fallback) {
    args.emplace_back("-bsf:a");
    args.emplace_back("aac_adtstoasc");
    args.emplace_back("-movflags");
    args.emplace_back("+faststart");
  }
  args.push_back(out_path);
  return args;
}

bool ParseFfmpegProgressLine(const std::string& line, MediaProgressSample* sample) {
  if (!sample) return false;
  const size_t eq = line.find('=');
  if (eq == std::string::npos || eq == 0) return false;
  std::string k = line.substr(0, eq);
  std::string v = line.substr(eq + 1);
  while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();

  if (k == "out_time_us" || k == "out_time_ms") {
    sample->out_time_us = std::max<int64_t>(0, std::strtoll(v.c_str(), nullptr, 10));
    return true;
  }
  if (k == "total_size") {
    sample->total_size = std::max<int64_t>(0, std::strtoll(v.c_str(), nullptr, 10));
    return true;
  }
  if (k == "speed") {
    sample->speed_x = std::max(0.0, std::strtod(v.c_str(), nullptr));
    return true;
  }
  if (k == "progress") {
    sample->progress_end = (v == "end");
    return true;
  }
  return false;
}

bool ParseYtDlpProgressLine(const std::string& line,
                            int* out_percent,
                            int64_t* out_speed_bps,
                            int64_t* out_total_bytes) {
  if (line.find("[download]") == std::string::npos) return false;
  const size_t pct_pos = line.find('%');
  if (pct_pos == std::string::npos || pct_pos == 0) return false;

  size_t num_start = pct_pos;
  while (num_start > 0 &&
         (std::isdigit(static_cast<unsigned char>(line[num_start - 1])) || line[num_start - 1] == '.')) {
    --num_start;
  }
  if (num_start == pct_pos) return false;

  const double pct_d = std::strtod(line.substr(num_start, pct_pos - num_start).c_str(), nullptr);
  if (out_percent) *out_percent = std::clamp(static_cast<int>(pct_d + 0.5), 0, 100);

  if (out_total_bytes) {
    size_t of_pos = line.find(" of ", pct_pos);
    if (of_pos != std::string::npos) {
      size_t p = of_pos + 4;
      while (p < line.size() && (line[p] == ' ' || line[p] == '~')) ++p;
      char* endp = nullptr;
      double val = std::strtod(line.c_str() + p, &endp);
      if (endp && endp > line.c_str() + p) {
        size_t ustart = static_cast<size_t>(endp - line.c_str());
        size_t uend = ustart;
        while (uend < line.size() && !std::isspace(static_cast<unsigned char>(line[uend]))) ++uend;
        int64_t bytes = ParseUnitBytes(val, line.substr(ustart, uend - ustart));
        if (bytes > 0) *out_total_bytes = bytes;
      }
    }
  }

  if (out_speed_bps) {
    size_t at_pos = line.find(" at ", pct_pos);
    if (at_pos != std::string::npos) {
      size_t p = at_pos + 4;
      while (p < line.size() && line[p] == ' ') ++p;
      char* endp = nullptr;
      double val = std::strtod(line.c_str() + p, &endp);
      if (endp && endp > line.c_str() + p) {
        size_t ustart = static_cast<size_t>(endp - line.c_str());
        size_t uend = ustart;
        while (uend < line.size() && !std::isspace(static_cast<unsigned char>(line[uend]))) ++uend;
        int64_t bps = ParseUnitBytes(val, line.substr(ustart, uend - ustart));
        if (bps >= 0) *out_speed_bps = bps;
      }
    }
  }
  return true;
}

bool IsFfmpegAvailable() {
  g_autofree gchar* p = g_find_program_in_path("ffmpeg");
  return p != nullptr && p[0] != '\0';
}

bool IsYtDlpAvailable() {
  g_autofree gchar* p = g_find_program_in_path("yt-dlp");
  return p != nullptr && p[0] != '\0';
}

MediaDownloadManager::MediaDownloadManager(ProgressCb cb) : cb_(std::move(cb)) {}

MediaDownloadManager::~MediaDownloadManager() {
  CancelAll();
}

void MediaDownloadManager::ReapFinishedLocked() {
  for (auto it = jobs_.begin(); it != jobs_.end();) {
    if (it->second && it->second->finished.load(std::memory_order_acquire)) {
      if (it->second->worker.joinable()) it->second->worker.join();
      it = jobs_.erase(it);
    } else {
      ++it;
    }
  }
}

void MediaDownloadManager::EmitUpdate(const ChromeDownloadView& dl) {
  if (cb_) cb_(dl);
}

bool MediaDownloadManager::HasJob(uint32_t id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = jobs_.find(id);
  return it != jobs_.end() && it->second && !it->second->finished.load(std::memory_order_acquire);
}

bool MediaDownloadManager::Cancel(uint32_t id) {
  std::shared_ptr<Job> job;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end() || !it->second) return false;
    job = it->second;
  }
  job->canceled.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> plock(job->proc_mu);
  if (job->proc) g_subprocess_force_exit(job->proc);
  return true;
}

void MediaDownloadManager::CancelAll() {
  std::map<uint32_t, std::shared_ptr<Job>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot.swap(jobs_);
  }
  for (auto& kv : snapshot) {
    if (!kv.second) continue;
    kv.second->canceled.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> plock(kv.second->proc_mu);
      if (kv.second->proc) g_subprocess_force_exit(kv.second->proc);
    }
  }
  for (auto& kv : snapshot) {
    if (kv.second && kv.second->worker.joinable()) {
      kv.second->worker.join();
    }
  }
}

uint32_t MediaDownloadManager::StartFfmpegStream(const std::string& uri,
                                                 const std::string& suggested_title,
                                                 const std::string& download_dir,
                                                 const std::string& referer,
                                                 const std::string& user_agent,
                                                 const std::string& cookie_header) {
  g_autofree gchar* ffmpeg_path = g_find_program_in_path("ffmpeg");
  if (!ffmpeg_path || uri.empty()) return 0;

  std::lock_guard<std::mutex> lock(mu_);
  ReapFinishedLocked();
  auto job = std::make_shared<Job>();
  job->id = next_id_++;
  if (next_id_ < 0x80000001u) next_id_ = 0x80000001u;

  std::string dir = download_dir;
  if (dir.empty()) {
    const gchar* dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    dir = dl ? dl : g_get_home_dir();
  }
  g_mkdir_with_parents(dir.c_str(), 0755);
  std::string stem = SanitizeMediaFilename(suggested_title, "");
  if (stem.empty()) stem = SanitizeMediaFilename(uri, "stream");

  jobs_[job->id] = job;
  job->worker = std::thread([this, job, uri, dir, stem, referer, user_agent, cookie_header]() {
    RunFfmpegWorker(job, uri, dir, stem, referer, user_agent, cookie_header);
  });
  return job->id;
}

void MediaDownloadManager::RunFfmpegWorker(const std::shared_ptr<Job>& job,
                                           std::string uri,
                                           std::string dir,
                                           std::string stem,
                                           std::string referer,
                                           std::string user_agent,
                                           std::string cookie_header) {
  g_autofree gchar* ffmpeg_bin = g_find_program_in_path("ffmpeg");
  if (!ffmpeg_bin) {
    job->finished.store(true, std::memory_order_release);
    return;
  }

  const std::string headers_arg = BuildFfmpegHeaderArg(referer, cookie_header);
  std::string out_path = BuildUniqueMediaPath(dir, stem, "mp4");
  g_autofree gchar* bname = g_path_get_basename(out_path.c_str());

  ChromeDownloadView dl;
  dl.id = job->id;
  dl.url = uri;
  dl.full_path = out_path;
  dl.filename = bname ? bname : (stem + ".mp4");
  dl.percent = 0;
  dl.updated_at_ms = g_get_real_time() / 1000;
  dl.in_progress = true;
  EmitUpdate(dl);

  auto run_once = [&](const std::string& target_path, bool mkv_fallback) -> bool {
    std::vector<std::string> args = BuildFfmpegRemuxArgs(
        ffmpeg_bin, uri, target_path, headers_arg, user_agent, mkv_fallback);
    std::vector<const gchar*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    g_autoptr(GError) err = nullptr;
    GSubprocess* proc = g_subprocess_launcher_spawnv(launcher, argv.data(), &err);
    if (!proc) return false;
    {
      std::lock_guard<std::mutex> plock(job->proc_mu);
      job->proc = proc;
    }

    if (GInputStream* pipe = g_subprocess_get_stdout_pipe(proc)) {
      g_autoptr(GDataInputStream) dis = g_data_input_stream_new(pipe);
      MediaProgressSample sample;
      gint64 start_us = g_get_monotonic_time();
      gint64 last_emit_us = start_us;
      while (!job->canceled.load(std::memory_order_acquire)) {
        gsize len = 0;
        g_autofree gchar* line = g_data_input_stream_read_line(dis, &len, nullptr, nullptr);
        if (!line) break;
        if (ParseFfmpegProgressLine(std::string(line, len), &sample)) {
          gint64 now_us = g_get_monotonic_time();
          if (sample.progress_end || (now_us - last_emit_us) >= 250000) {
            last_emit_us = now_us;
            int64_t bytes = sample.total_size > 0 ? sample.total_size : FileSizeBytes(target_path);
            double elapsed_s = std::max(0.1, static_cast<double>(now_us - start_us) / 1e6);
            dl.received_bytes = bytes;
            dl.current_speed = static_cast<int64_t>(static_cast<double>(bytes) / elapsed_s);
            dl.percent = -1;
            dl.updated_at_ms = g_get_real_time() / 1000;
            EmitUpdate(dl);
          }
        }
      }
    }

    gboolean ok = g_subprocess_wait_check(proc, nullptr, nullptr);
    {
      std::lock_guard<std::mutex> plock(job->proc_mu);
      g_clear_object(&job->proc);
    }
    return ok != FALSE;
  };

  bool ok = run_once(out_path, false);
  if (!ok && !job->canceled.load(std::memory_order_acquire)) {
    g_unlink(out_path.c_str());
    out_path = BuildUniqueMediaPath(dir, stem, "mkv");
    g_autofree gchar* mkv_name = g_path_get_basename(out_path.c_str());
    dl.full_path = out_path;
    dl.filename = mkv_name ? mkv_name : (stem + ".mkv");
    dl.received_bytes = 0;
    dl.percent = 0;
    dl.updated_at_ms = g_get_real_time() / 1000;
    EmitUpdate(dl);
    ok = run_once(out_path, true);
  }

  const int64_t final_bytes = FileSizeBytes(out_path);
  if (ok && !job->canceled.load(std::memory_order_acquire) && final_bytes > 0) {
    dl.in_progress = false;
    dl.complete = true;
    dl.canceled = false;
    dl.percent = 100;
    dl.received_bytes = final_bytes;
    dl.total_bytes = final_bytes;
    dl.current_speed = 0;
  } else {
    g_unlink(out_path.c_str());
    dl.in_progress = false;
    dl.complete = false;
    dl.canceled = true;
    dl.current_speed = 0;
  }
  dl.updated_at_ms = g_get_real_time() / 1000;
  EmitUpdate(dl);
  job->finished.store(true, std::memory_order_release);
}

uint32_t MediaDownloadManager::StartYtDlp(const std::string& page_url,
                                          const std::string& suggested_title,
                                          const std::string& download_dir,
                                          const std::string& user_agent) {
  g_autofree gchar* ytdlp_path = g_find_program_in_path("yt-dlp");
  if (!ytdlp_path || page_url.empty()) return 0;

  std::lock_guard<std::mutex> lock(mu_);
  ReapFinishedLocked();
  auto job = std::make_shared<Job>();
  job->id = next_id_++;
  if (next_id_ < 0x80000001u) next_id_ = 0x80000001u;

  std::string dir = download_dir;
  if (dir.empty()) {
    const gchar* dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    dir = dl ? dl : g_get_home_dir();
  }
  g_mkdir_with_parents(dir.c_str(), 0755);
  std::string stem = SanitizeMediaFilename(suggested_title, "");
  if (stem.empty()) stem = SanitizeMediaFilename(page_url, "video");

  jobs_[job->id] = job;
  job->worker = std::thread([this, job, page_url, dir, stem, user_agent]() {
    RunYtDlpWorker(job, page_url, dir, stem, user_agent);
  });
  return job->id;
}

void MediaDownloadManager::RunYtDlpWorker(const std::shared_ptr<Job>& job,
                                          std::string page_url,
                                          std::string dir,
                                          std::string stem,
                                          std::string user_agent) {
  g_autofree gchar* ytdlp_bin = g_find_program_in_path("yt-dlp");
  if (!ytdlp_bin) {
    job->finished.store(true, std::memory_order_release);
    return;
  }

  g_autofree gchar* out_tmpl = g_build_filename(dir.c_str(), "%(title).100s-%(id)s.%(ext)s", nullptr);
  ChromeDownloadView dl;
  dl.id = job->id;
  dl.url = page_url;
  dl.filename = stem + " (yt-dlp)";
  dl.percent = 0;
  dl.updated_at_ms = g_get_real_time() / 1000;
  dl.in_progress = true;
  EmitUpdate(dl);

  std::vector<std::string> args = {
      ytdlp_bin,
      "--no-playlist",
      "--newline",
      "--print",
      "after_move:KAJO_FINAL_PATH:%(filepath)s",
      "-o",
      out_tmpl ? out_tmpl : (dir + "/%(title)s.%(ext)s"),
  };
  if (!user_agent.empty()) {
    args.emplace_back("--user-agent");
    args.push_back(StripCrLf(user_agent));
  }
  args.push_back(page_url);

  std::vector<const gchar*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);

  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                    G_SUBPROCESS_FLAGS_STDERR_MERGE));
  g_autoptr(GError) err = nullptr;
  GSubprocess* proc = g_subprocess_launcher_spawnv(launcher, argv.data(), &err);
  std::string final_path;
  bool ok = false;
  if (proc) {
    {
      std::lock_guard<std::mutex> plock(job->proc_mu);
      job->proc = proc;
    }
    if (GInputStream* pipe = g_subprocess_get_stdout_pipe(proc)) {
      g_autoptr(GDataInputStream) dis = g_data_input_stream_new(pipe);
      gint64 last_emit_us = 0;
      while (!job->canceled.load(std::memory_order_acquire)) {
        gsize len = 0;
        g_autofree gchar* line_c = g_data_input_stream_read_line(dis, &len, nullptr, nullptr);
        if (!line_c) break;
        std::string line(line_c, len);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        static const char kFinalPrefix[] = "KAJO_FINAL_PATH:";
        if (line.rfind(kFinalPrefix, 0) == 0) {
          final_path = line.substr(sizeof(kFinalPrefix) - 1);
          continue;
        }
        int pct = dl.percent;
        int64_t spd = dl.current_speed;
        int64_t tot = dl.total_bytes;
        if (ParseYtDlpProgressLine(line, &pct, &spd, &tot)) {
          gint64 now_us = g_get_monotonic_time();
          if ((now_us - last_emit_us) >= 250000 || pct == 100) {
            last_emit_us = now_us;
            dl.percent = pct;
            dl.current_speed = spd;
            dl.total_bytes = tot;
            if (tot > 0) dl.received_bytes = (tot * pct) / 100;
            dl.updated_at_ms = g_get_real_time() / 1000;
            EmitUpdate(dl);
          }
        }
      }
    }
    ok = g_subprocess_wait_check(proc, nullptr, nullptr) != FALSE;
    {
      std::lock_guard<std::mutex> plock(job->proc_mu);
      g_clear_object(&job->proc);
    }
  }

  if (ok && !job->canceled.load(std::memory_order_acquire) &&
      !final_path.empty() && g_file_test(final_path.c_str(), G_FILE_TEST_EXISTS)) {
    g_autofree gchar* bname = g_path_get_basename(final_path.c_str());
    const int64_t fbytes = FileSizeBytes(final_path);
    dl.full_path = final_path;
    if (bname) dl.filename = bname;
    dl.in_progress = false;
    dl.complete = true;
    dl.canceled = false;
    dl.percent = 100;
    dl.received_bytes = fbytes;
    dl.total_bytes = fbytes;
    dl.current_speed = 0;
  } else {
    if (!final_path.empty()) g_unlink(final_path.c_str());
    dl.in_progress = false;
    dl.complete = false;
    dl.canceled = true;
    dl.current_speed = 0;
  }
  dl.updated_at_ms = g_get_real_time() / 1000;
  EmitUpdate(dl);
  job->finished.store(true, std::memory_order_release);
}

}  // namespace kajo_web
