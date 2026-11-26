#include "web_cef/cef_media_dl.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
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

std::string ToLowerAscii(const std::string& in) {
  std::string out = in;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string TrimAscii(const std::string& in) {
  size_t a = 0;
  size_t b = in.size();
  while (a < b && (in[a] == ' ' || in[a] == '\t' || in[a] == '\r' || in[a] == '\n')) ++a;
  while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t' || in[b - 1] == '\r' || in[b - 1] == '\n')) --b;
  return in.substr(a, b - a);
}

int64_t ParseUnitBytes(double val, const std::string& unit) {
  if (val <= 0.0) return 0;
  const std::string u = ToLowerAscii(unit);
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

void RemoveTempFileSecurely(const std::string& path) {
  if (path.empty()) return;
  FILE* fp = std::fopen(path.c_str(), "r+b");
  if (fp) {
    if (std::fseek(fp, 0, SEEK_END) == 0) {
      long sz = std::ftell(fp);
      if (sz > 0 && sz < 1024L * 1024L) {
        std::rewind(fp);
        std::vector<char> zeros(static_cast<size_t>(sz), '\0');
        (void)std::fwrite(zeros.data(), 1, zeros.size(), fp);
        std::fflush(fp);
      }
    }
    std::fclose(fp);
  }
  g_unlink(path.c_str());
}

std::string ResolveRelativeHttpUrl(const std::string& base_url, const std::string& rel) {
  const std::string trimmed = TrimAscii(rel);
  if (trimmed.empty()) return "";
  if (trimmed.rfind("http://", 0) == 0 || trimmed.rfind("https://", 0) == 0) {
    return trimmed;
  }
  if (base_url.empty()) return trimmed;
  g_autoptr(GUri) base = g_uri_parse(base_url.c_str(), G_URI_FLAGS_NONE, nullptr);
  if (!base) return trimmed;
  g_autoptr(GUri) resolved = g_uri_parse_relative(base, trimmed.c_str(), G_URI_FLAGS_NONE, nullptr);
  if (!resolved) return trimmed;
  g_autofree gchar* s = g_uri_to_string(resolved);
  return s ? std::string(s) : trimmed;
}

std::string EncodeFetchQueryUrl(const std::string& prefix, const std::string& abs_url) {
  g_autofree gchar* esc = g_uri_escape_string(abs_url.c_str(), nullptr, FALSE);
  return prefix + (esc ? std::string(esc) : abs_url);
}

std::string ExtractUrlExtension(const std::string& url) {
  size_t end = url.find_first_of("?#");
  std::string path = (end == std::string::npos) ? url : url.substr(0, end);
  size_t slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= base.size()) return "";
  return ToLowerAscii(base.substr(dot + 1));
}

bool SendAllFd(int fd, const char* data, size_t len, const std::atomic<bool>* stop_flag) {
  size_t sent = 0;
  while (sent < len) {
    if (stop_flag && stop_flag->load(std::memory_order_acquire)) return false;
    ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool SendStringFd(int fd, const std::string& s, const std::atomic<bool>* stop_flag) {
  return SendAllFd(fd, s.data(), s.size(), stop_flag);
}

const char* HttpStatusReason(int status) {
  switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 416: return "Range Not Satisfiable";
    case 502: return "Bad Gateway";
    default: return "OK";
  }
}

std::string MapMimeToExt(const std::string& mime, const std::string& uri) {
  const std::string m = ToLowerAscii(mime);
  if (m.find("webm") != std::string::npos) return "webm";
  if (m.find("matroska") != std::string::npos) return "mkv";
  if (m.find("audio/mpeg") != std::string::npos) return "mp3";
  if (m.find("audio/mp4") != std::string::npos || m.find("m4a") != std::string::npos) return "m4a";
  if (m.find("ogg") != std::string::npos) return "ogg";
  if (m.find("quicktime") != std::string::npos) return "mov";
  std::string ext = ExtractUrlExtension(uri);
  if (ext == "webm" || ext == "mkv" || ext == "mov" || ext == "m4v" ||
      ext == "m4a" || ext == "mp3" || ext == "flv" || ext == "mp4") {
    return ext;
  }
  return "mp4";
}

bool LooksLikeManifestResponse(const std::string& url,
                               const std::map<std::string, std::string>& hdrs,
                               bool* out_is_hls,
                               bool* out_is_dash) {
  std::string ext = ExtractUrlExtension(url);
  std::string ct;
  for (const auto& kv : hdrs) {
    if (ToLowerAscii(kv.first) == "content-type") {
      ct = ToLowerAscii(kv.second);
      break;
    }
  }
  const bool hls = (ext == "m3u8" || ct.find("mpegurl") != std::string::npos);
  const bool dash = (ext == "mpd" || ct.find("dash+xml") != std::string::npos);
  if (out_is_hls) *out_is_hls = hls;
  if (out_is_dash) *out_is_dash = dash;
  return hls || dash;
}

std::string LastNonEmptyLine(const std::string& text) {
  std::string last;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    std::string t = TrimAscii(line);
    if (!t.empty()) last = t;
  }
  return last;
}

}  // namespace

struct MediaDownloadManager::Job {
  uint32_t id = 0;
  std::atomic<bool> canceled{false};
  std::atomic<bool> finished{false};
  std::mutex proc_mu;
  GSubprocess* proc = nullptr;
  std::weak_ptr<MediaLoopbackProxy> proxy;
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
  for (const char* ext : {".m3u8", ".mpd", ".mp4", ".webm", ".mkv", ".ts", ".m4a", ".mp3"}) {
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

std::string StripUrlFragment(const std::string& uri) {
  size_t hash = uri.find('#');
  return (hash == std::string::npos) ? uri : uri.substr(0, hash);
}

std::string Base64UrlEncode(const std::string& raw) {
  if (raw.empty()) return "";
  g_autofree gchar* b64 = g_base64_encode(
      reinterpret_cast<const guchar*>(raw.data()), raw.size());
  if (!b64) return "";
  std::string out(b64);
  for (char& c : out) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
  }
  while (!out.empty() && out.back() == '=') out.pop_back();
  return out;
}

std::string Base64UrlDecode(const std::string& encoded) {
  if (encoded.empty()) return "";
  std::string b64 = encoded;
  for (char& c : b64) {
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
  }
  while (b64.size() % 4 != 0) b64.push_back('=');
  gsize out_len = 0;
  g_autofree guchar* raw = g_base64_decode(b64.c_str(), &out_len);
  if (!raw || out_len == 0) return "";
  return std::string(reinterpret_cast<const char*>(raw), out_len);
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

std::vector<std::string> BuildFfmpegJobArgs(const FfmpegJobSpec& spec) {
  std::vector<std::string> args = {
      spec.ffmpeg_bin.empty() ? "ffmpeg" : spec.ffmpeg_bin,
      "-y",
      "-nostdin",
      "-hide_banner",
      "-loglevel",
      "error",
      "-progress",
      "pipe:1",
      "-protocol_whitelist",
      "file,http,https,tcp,tls,crypto,data",
  };

  auto append_input_opts = [&](const std::string& url, const std::string& fmt_override) {
    if (!fmt_override.empty()) {
      args.emplace_back("-f");
      args.push_back(fmt_override);
      if (fmt_override == "hls") {
        args.emplace_back("-allowed_extensions");
        args.emplace_back("ALL");
        args.emplace_back("-allowed_segment_extensions");
        args.emplace_back("ALL");
        args.emplace_back("-extension_picky");
        args.emplace_back("0");
      } else if (fmt_override == "dash") {
        args.emplace_back("-allowed_extensions");
        args.emplace_back("ALL");
      }
    }
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
      if (spec.proxied) {
        args.emplace_back("-http_persistent");
        args.emplace_back("0");
      } else {
        if (!spec.user_agent.empty()) {
          args.emplace_back("-user_agent");
          args.push_back(StripCrLf(spec.user_agent));
        }
        if (!spec.headers_arg.empty()) {
          args.emplace_back("-headers");
          args.push_back(spec.headers_arg);
        }
      }
    }
  };

  append_input_opts(spec.video_uri, spec.input_format);
  args.emplace_back("-i");
  args.push_back(spec.video_uri);

  int next_idx = 1;
  int audio_in_idx = -1;
  if (!spec.audio_uri.empty() && spec.audio_uri != spec.video_uri) {
    const std::string afmt = (spec.input_format == "hls") ? "hls" : "";
    append_input_opts(spec.audio_uri, afmt);
    args.emplace_back("-i");
    args.push_back(spec.audio_uri);
    audio_in_idx = next_idx++;
  }

  int sub_in_idx = -1;
  if (!spec.subtitle_uri.empty() && !spec.audio_only) {
    const std::string sfmt = (spec.subtitle_uri.find(".m3u8") != std::string::npos ||
                              spec.input_format == "hls")
                                 ? "hls"
                                 : "";
    append_input_opts(spec.subtitle_uri, sfmt);
    args.emplace_back("-i");
    args.push_back(spec.subtitle_uri);
    sub_in_idx = next_idx++;
  }

  if (spec.audio_only) {
    if (audio_in_idx >= 0) {
      args.emplace_back("-map");
      args.push_back(std::to_string(audio_in_idx) + ":a:0?");
    } else if (!spec.dash_rep_id.empty()) {
      args.emplace_back("-map");
      args.push_back("0:m:id:" + spec.dash_rep_id + "?");
    } else {
      args.emplace_back("-map");
      args.emplace_back("0:a:0?");
    }
    args.emplace_back("-vn");
  } else {
    if (!spec.dash_rep_id.empty() && spec.input_format == "dash") {
      args.emplace_back("-map");
      args.push_back("0:m:id:" + spec.dash_rep_id + "?");
      args.emplace_back("-map");
      args.emplace_back("0:v:0?");
    } else {
      args.emplace_back("-map");
      args.emplace_back("0:v:0?");
    }
    if (audio_in_idx >= 0) {
      args.emplace_back("-map");
      args.push_back(std::to_string(audio_in_idx) + ":a:0?");
    } else {
      args.emplace_back("-map");
      args.emplace_back("0:a:0?");
    }
    if (sub_in_idx >= 0) {
      args.emplace_back("-map");
      args.push_back(std::to_string(sub_in_idx) + ":s:0?");
    }
  }

  args.emplace_back("-c");
  args.emplace_back("copy");
  if (sub_in_idx >= 0 && !spec.mkv_fallback && !spec.audio_only) {
    args.emplace_back("-c:s");
    args.emplace_back("mov_text");
  }
  args.emplace_back("-avoid_negative_ts");
  args.emplace_back("make_zero");
  if (!spec.mkv_fallback && !spec.audio_only) {
    args.emplace_back("-movflags");
    args.emplace_back("+faststart");
  }
  args.push_back(spec.out_path);
  return args;
}

std::string RewriteHlsManifestForProxy(const std::string& m3u8,
                                       const std::string& base_url,
                                       const std::string& proxy_fetch_prefix) {
  std::istringstream iss(m3u8);
  std::ostringstream out;
  std::string line;
  bool first = true;

  auto rewrite_uri_attr = [&](std::string s) -> std::string {
    size_t pos = 0;
    while ((pos = s.find("URI=\"", pos)) != std::string::npos) {
      size_t val_start = pos + 5;
      size_t val_end = s.find('"', val_start);
      if (val_end == std::string::npos) break;
      std::string orig = s.substr(val_start, val_end - val_start);
      if (!orig.empty() && orig.rfind("data:", 0) != 0 &&
          orig.rfind(proxy_fetch_prefix, 0) != 0) {
        std::string abs = ResolveRelativeHttpUrl(base_url, orig);
        std::string repl = EncodeFetchQueryUrl(proxy_fetch_prefix, abs);
        s.replace(val_start, val_end - val_start, repl);
        pos = val_start + repl.size() + 1;
      } else {
        pos = val_end + 1;
      }
    }
    return s;
  };

  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!first) out << '\n';
    first = false;

    std::string trimmed = TrimAscii(line);
    if (trimmed.empty()) {
      continue;
    }
    if (trimmed[0] == '#') {
      out << rewrite_uri_attr(trimmed);
    } else {
      if (trimmed.rfind(proxy_fetch_prefix, 0) == 0) {
        out << trimmed;
      } else {
        std::string abs = ResolveRelativeHttpUrl(base_url, trimmed);
        out << EncodeFetchQueryUrl(proxy_fetch_prefix, abs);
      }
    }
  }
  if (!m3u8.empty() && m3u8.back() == '\n') out << '\n';
  return out.str();
}

std::string RewriteDashManifestForProxy(const std::string& mpd,
                                        const std::string& base_url,
                                        const std::string& proxy_rel_prefix,
                                        const std::string& proxy_fetch_prefix) {
  std::string s = mpd;
  /* Rewrite existing <BaseURL>http(s)://...</BaseURL> elements */
  size_t pos = 0;
  bool has_top_base_url = false;
  while ((pos = s.find("<BaseURL", pos)) != std::string::npos) {
    size_t open_end = s.find('>', pos);
    size_t close_pos = (open_end == std::string::npos) ? std::string::npos : s.find("</BaseURL>", open_end);
    if (open_end == std::string::npos || close_pos == std::string::npos) break;
    std::string val = TrimAscii(s.substr(open_end + 1, close_pos - open_end - 1));
    if (val.rfind("http://", 0) == 0 || val.rfind("https://", 0) == 0) {
      std::string repl;
      if (!val.empty() && val.back() == '/') {
        repl = proxy_rel_prefix + Base64UrlEncode(val) + "/";
      } else {
        repl = EncodeFetchQueryUrl(proxy_fetch_prefix, val);
      }
      s.replace(open_end + 1, close_pos - open_end - 1, repl);
      has_top_base_url = true;
      pos = open_end + 1 + repl.size();
    } else {
      pos = close_pos + 10;
    }
  }

  /* Inject a <BaseURL> right after <MPD ...> if no <BaseURL> was rewritten so
   * ffmpeg's DASH demuxer resolves relative SegmentTemplate/SegmentList paths
   * through /<token>/rel/<b64>/... */
  if (!has_top_base_url && !base_url.empty()) {
    size_t mpd_start = s.find("<MPD");
    size_t mpd_gt = (mpd_start != std::string::npos) ? s.find('>', mpd_start) : std::string::npos;
    if (mpd_gt != std::string::npos) {
      std::string base_dir = StripUrlFragment(base_url);
      size_t q = base_dir.find('?');
      if (q != std::string::npos) base_dir.resize(q);
      size_t slash = base_dir.find_last_of('/');
      if (slash != std::string::npos) base_dir.resize(slash + 1);
      std::string injected = "<BaseURL>" + proxy_rel_prefix + Base64UrlEncode(base_dir) + "/</BaseURL>";
      s.insert(mpd_gt + 1, injected);
    }
  }
  return s;
}

std::string FormatNetscapeCookieLine(const std::string& domain,
                                     bool include_subdomains,
                                     const std::string& path,
                                     bool secure,
                                     int64_t expires_unix,
                                     const std::string& name,
                                     const std::string& value) {
  std::ostringstream oss;
  std::string dom = StripCrLf(domain);
  if (include_subdomains && !dom.empty() && dom.front() != '.') {
    dom = "." + dom;
  }
  oss << dom << '\t'
      << (include_subdomains ? "TRUE" : "FALSE") << '\t'
      << (path.empty() ? "/" : StripCrLf(path)) << '\t'
      << (secure ? "TRUE" : "FALSE") << '\t'
      << std::max<int64_t>(0, expires_unix) << '\t'
      << StripCrLf(name) << '\t'
      << StripCrLf(value);
  return oss.str();
}

std::vector<YtDlpFormatOption> ParseYtDlpInfoJson(const std::string& json_text,
                                                  std::string* out_title) {
  std::vector<YtDlpFormatOption> out;
  if (json_text.empty()) return out;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json_text.data(), static_cast<gssize>(json_text.size()), nullptr)) {
    return out;
  }
  JsonNode* root_node = json_parser_get_root(parser);
  if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) return out;
  JsonObject* root = json_node_get_object(root_node);

  if (out_title && json_object_has_member(root, "title")) {
    const gchar* t = json_object_get_string_member_with_default(root, "title", "");
    if (t) *out_title = t;
  }
  const double dur = json_object_get_double_member_with_default(root, "duration", 0.0);

  if (!json_object_has_member(root, "formats")) return out;
  JsonArray* fmts = json_object_get_array_member(root, "formats");
  if (!fmts) return out;

  const guint n = json_array_get_length(fmts);
  std::unordered_set<std::string> seen;
  for (guint i = 0; i < n; ++i) {
    JsonObject* f = json_array_get_object_element(fmts, i);
    if (!f) continue;
    const gchar* fid = json_object_get_string_member_with_default(f, "format_id", "");
    const gchar* ext = json_object_get_string_member_with_default(f, "ext", "");
    const gchar* vcodec = json_object_get_string_member_with_default(f, "vcodec", "none");
    const gchar* acodec = json_object_get_string_member_with_default(f, "acodec", "none");
    const gchar* note = json_object_get_string_member_with_default(f, "format_note", "");
    if (!fid || !fid[0]) continue;
    if (g_strcmp0(ext, "mhtml") == 0) continue; /* storyboard thumbnails */

    const bool has_v = (vcodec && g_strcmp0(vcodec, "none") != 0);
    const bool has_a = (acodec && g_strcmp0(acodec, "none") != 0);
    if (!has_v && !has_a) continue;

    YtDlpFormatOption opt;
    opt.format_id = fid;
    opt.ext = ext ? ext : "";
    opt.width = static_cast<int>(json_object_get_int_member_with_default(f, "width", 0));
    opt.height = static_cast<int>(json_object_get_int_member_with_default(f, "height", 0));
    double tbr = json_object_get_double_member_with_default(f, "tbr", 0.0);
    opt.bandwidth = tbr > 0.0 ? static_cast<int64_t>(tbr * 1000.0) : 0;
    opt.filesize = static_cast<int64_t>(json_object_get_int_member_with_default(f, "filesize", 0));
    if (opt.filesize <= 0) {
      opt.filesize = static_cast<int64_t>(
          json_object_get_int_member_with_default(f, "filesize_approx", 0));
    }
    opt.duration_sec = dur > 0.0 ? dur : 0.0;
    opt.audio_only = (!has_v && has_a);

    std::ostringstream lbl;
    if (opt.audio_only) {
      lbl << "Audio";
      if (!opt.ext.empty()) lbl << " (" << opt.ext << ")";
      if (tbr > 0.0) lbl << " · " << static_cast<int>(tbr + 0.5) << " kbps";
    } else if (opt.height > 0) {
      lbl << opt.height << "p";
      if (!opt.ext.empty()) lbl << " " << ToLowerAscii(opt.ext);
      if (note && note[0]) lbl << " · " << note;
      if (!has_a) lbl << " (+auto audio)";
    } else {
      lbl << "Format " << opt.format_id;
      if (!opt.ext.empty()) lbl << " (" << opt.ext << ")";
    }
    opt.label = lbl.str();
    if (!seen.insert(opt.format_id).second) continue;
    out.push_back(std::move(opt));
  }

  std::sort(out.begin(), out.end(), [](const YtDlpFormatOption& a, const YtDlpFormatOption& b) {
    if (a.audio_only != b.audio_only) return !a.audio_only;
    if (a.height != b.height) return a.height > b.height;
    return a.bandwidth > b.bandwidth;
  });
  if (out.size() > 24) out.resize(24);
  return out;
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

/* ── MediaLoopbackProxy ─────────────────────────────────────────────────────────────────── */

MediaLoopbackProxy::MediaLoopbackProxy(MediaFetchStreamFn fetch_fn)
    : fetch_fn_(std::move(fetch_fn)) {}

MediaLoopbackProxy::~MediaLoopbackProxy() {
  Stop();
}

std::string MediaLoopbackProxy::ProxyFetchPrefix() const {
  if (port_ == 0 || token_.empty()) return "";
  return "http://127.0.0.1:" + std::to_string(port_) + "/" + token_ + "/fetch?u=";
}

std::string MediaLoopbackProxy::ProxyRelPrefix() const {
  if (port_ == 0 || token_.empty()) return "";
  return "http://127.0.0.1:" + std::to_string(port_) + "/" + token_ + "/rel/";
}

std::string MediaLoopbackProxy::WrapUrl(const std::string& target_url) const {
  if (target_url.empty()) return "";
  return EncodeFetchQueryUrl(ProxyFetchPrefix(), target_url);
}

bool MediaLoopbackProxy::Start() {
  if (listen_fd_ >= 0 || !fetch_fn_) return false;
  stopped_.store(false, std::memory_order_release);

  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  int yes = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 32) != 0) {
    ::close(fd);
    return false;
  }
  socklen_t alen = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
    ::close(fd);
    return false;
  }

  port_ = ntohs(addr.sin_port);
  g_autofree gchar* tok = g_strdup_printf("%08x%08x%08x%08x",
                                          g_random_int(), g_random_int(),
                                          g_random_int(), g_random_int());
  token_ = tok ? tok : "kajo";
  listen_fd_ = fd;
  accept_thread_ = std::thread([this]() { AcceptLoop(); });
  return true;
}

void MediaLoopbackProxy::Stop() {
  stopped_.store(true, std::memory_order_release);
  int lfd = listen_fd_;
  listen_fd_ = -1;
  if (lfd >= 0) {
    ::shutdown(lfd, SHUT_RDWR);
    ::close(lfd);
  }
  {
    std::lock_guard<std::mutex> lock(fds_mu_);
    for (int cfd : active_fds_) {
      if (cfd >= 0) ::shutdown(cfd, SHUT_RDWR);
    }
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  std::vector<std::unique_ptr<WorkerEntry>> rem;
  {
    std::lock_guard<std::mutex> lock(workers_mu_);
    rem.swap(workers_);
  }
  for (auto& w : rem) {
    if (w && w->th.joinable()) w->th.join();
  }
}

void MediaLoopbackProxy::PruneFinishedWorkersLocked() {
  for (auto it = workers_.begin(); it != workers_.end();) {
    if (*it && (*it)->done.load(std::memory_order_acquire)) {
      if ((*it)->th.joinable()) (*it)->th.join();
      it = workers_.erase(it);
    } else {
      ++it;
    }
  }
}

void MediaLoopbackProxy::AcceptLoop() {
  while (!stopped_.load(std::memory_order_acquire) && listen_fd_ >= 0) {
    pollfd pfd{listen_fd_, POLLIN, 0};
    int pr = ::poll(&pfd, 1, 100);
    if (pr <= 0) continue;

    sockaddr_in cli{};
    socklen_t clen = sizeof(cli);
    int cfd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &clen, SOCK_CLOEXEC);
    if (cfd < 0) continue;

    {
      std::lock_guard<std::mutex> lock(workers_mu_);
      PruneFinishedWorkersLocked();
      if (workers_.size() >= 24) {
        ::close(cfd);
        continue;
      }
      auto w = std::make_unique<WorkerEntry>();
      WorkerEntry* raw_w = w.get();
      w->th = std::thread([this, cfd, raw_w]() {
        HandleClient(cfd);
        raw_w->done.store(true, std::memory_order_release);
      });
      workers_.push_back(std::move(w));
    }
  }
}

void MediaLoopbackProxy::HandleClient(int client_fd) {
  {
    std::lock_guard<std::mutex> lock(fds_mu_);
    active_fds_.insert(client_fd);
  }
  auto cleanup = [this, client_fd]() {
    {
      std::lock_guard<std::mutex> lock(fds_mu_);
      active_fds_.erase(client_fd);
    }
    ::close(client_fd);
  };

  std::string raw_req;
  raw_req.reserve(2048);
  char buf[4096];
  size_t hdr_end = std::string::npos;
  while (raw_req.size() < 65536 && !stopped_.load(std::memory_order_acquire)) {
    pollfd pfd{client_fd, POLLIN, 0};
    int pr = ::poll(&pfd, 1, 100);
    if (pr == 0) continue;
    if (pr < 0) {
      cleanup();
      return;
    }
    ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      cleanup();
      return;
    }
    raw_req.append(buf, static_cast<size_t>(n));
    hdr_end = raw_req.find("\r\n\r\n");
    if (hdr_end != std::string::npos) break;
  }
  if (hdr_end == std::string::npos) {
    SendStringFd(client_fd, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", &stopped_);
    cleanup();
    return;
  }

  std::istringstream iss(raw_req.substr(0, hdr_end));
  std::string method, target_path, proto;
  iss >> method >> target_path >> proto;
  if ((method != "GET" && method != "HEAD") || target_path.empty()) {
    SendStringFd(client_fd, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", &stopped_);
    cleanup();
    return;
  }

  std::map<std::string, std::string> fwd_headers;
  std::string hline;
  std::getline(iss, hline); /* consume CRLF after request line */
  while (std::getline(iss, hline)) {
    if (!hline.empty() && hline.back() == '\r') hline.pop_back();
    size_t col = hline.find(':');
    if (col == std::string::npos) continue;
    std::string k = ToLowerAscii(TrimAscii(hline.substr(0, col)));
    std::string v = TrimAscii(hline.substr(col + 1));
    if (k == "range" && !v.empty()) {
      fwd_headers["Range"] = v;
    }
  }

  const std::string token_prefix = "/" + token_ + "/";
  if (target_path.rfind(token_prefix, 0) != 0) {
    SendStringFd(client_fd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", &stopped_);
    cleanup();
    return;
  }

  const std::string sub = target_path.substr(token_prefix.size());
  std::string target_url;
  if (sub.rfind("fetch?", 0) == 0) {
    std::string query = sub.substr(6);
    size_t upos = query.find("u=");
    if (upos != std::string::npos) {
      std::string enc = query.substr(upos + 2);
      size_t amp = enc.find('&');
      if (amp != std::string::npos) enc.resize(amp);
      g_autofree gchar* unesc = g_uri_unescape_string(enc.c_str(), nullptr);
      if (unesc) target_url = unesc;
    }
  } else if (sub.rfind("rel/", 0) == 0) {
    std::string rest = sub.substr(4);
    size_t sl = rest.find('/');
    if (sl != std::string::npos) {
      std::string base = Base64UrlDecode(rest.substr(0, sl));
      std::string rel = rest.substr(sl + 1);
      target_url = ResolveRelativeHttpUrl(base, rel);
    }
  }

  if (target_url.rfind("http://", 0) != 0 && target_url.rfind("https://", 0) != 0) {
    SendStringFd(client_fd, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", &stopped_);
    cleanup();
    return;
  }

  bool headers_arrived = false;
  bool is_manifest = false;
  bool is_hls = false;
  bool is_dash = false;
  int resp_status = 502;
  std::map<std::string, std::string> resp_hdrs;
  std::string manifest_buf;
  bool manifest_overflow = false;
  bool sent_stream_hdr = false;
  bool write_failed = false;

  auto on_headers = [&](int status, const std::map<std::string, std::string>& hdrs) {
    headers_arrived = true;
    resp_status = status > 0 ? status : 502;
    resp_hdrs = hdrs;
    if (method != "HEAD" && status >= 200 && status < 300) {
      is_manifest = LooksLikeManifestResponse(target_url, hdrs, &is_hls, &is_dash);
    }
    if (!is_manifest) {
      std::ostringstream hdr;
      hdr << "HTTP/1.1 " << resp_status << " " << HttpStatusReason(resp_status) << "\r\n";
      for (const auto& kv : hdrs) {
        const std::string lk = ToLowerAscii(kv.first);
        if (lk == "content-type" || lk == "content-range" ||
            lk == "accept-ranges" || lk == "content-length") {
          hdr << kv.first << ": " << StripCrLf(kv.second) << "\r\n";
        }
      }
      hdr << "Connection: close\r\n\r\n";
      if (!SendStringFd(client_fd, hdr.str(), &stopped_)) {
        write_failed = true;
      }
      sent_stream_hdr = true;
    }
  };

  auto on_chunk = [&](const char* data, size_t len) -> bool {
    if (stopped_.load(std::memory_order_acquire) || write_failed) return false;
    if (!data || len == 0) return true;
    if (is_manifest) {
      if (manifest_buf.size() + len > 4U * 1024U * 1024U) {
        manifest_overflow = true;
        return false;
      }
      manifest_buf.append(data, len);
      return true;
    }
    if (!SendAllFd(client_fd, data, len, &stopped_)) {
      write_failed = true;
      return false;
    }
    return true;
  };

  auto on_done = [&](bool ok, const std::string& err) {
    if (stopped_.load(std::memory_order_acquire)) return;
    if (is_manifest) {
      if (!ok || manifest_overflow) {
        std::string msg = manifest_overflow ? "Manifest too large" : (err.empty() ? "Upstream error" : err);
        std::ostringstream hdr;
        hdr << "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nContent-Length: "
            << msg.size() << "\r\nConnection: close\r\n\r\n" << msg;
        SendStringFd(client_fd, hdr.str(), &stopped_);
        return;
      }
      std::string rewritten;
      std::string ct = "application/octet-stream";
      if (is_hls) {
        rewritten = RewriteHlsManifestForProxy(manifest_buf, target_url, ProxyFetchPrefix());
        ct = "application/vnd.apple.mpegurl";
      } else {
        rewritten = RewriteDashManifestForProxy(
            manifest_buf, target_url, ProxyRelPrefix(), ProxyFetchPrefix());
        ct = "application/dash+xml";
      }
      std::ostringstream hdr;
      hdr << "HTTP/1.1 " << resp_status << " OK\r\n"
          << "Content-Type: " << ct << "\r\n"
          << "Content-Length: " << rewritten.size() << "\r\n"
          << "Connection: close\r\n\r\n";
      if (SendStringFd(client_fd, hdr.str(), &stopped_)) {
        SendAllFd(client_fd, rewritten.data(), rewritten.size(), &stopped_);
      }
    } else if (!headers_arrived && !sent_stream_hdr) {
      std::string msg = err.empty() ? "Upstream request failed" : err;
      std::ostringstream hdr;
      hdr << "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nContent-Length: "
          << msg.size() << "\r\nConnection: close\r\n\r\n" << msg;
      SendStringFd(client_fd, hdr.str(), &stopped_);
    }
  };

  fetch_fn_(target_url, method, fwd_headers, &stopped_,
            std::move(on_headers), std::move(on_chunk), std::move(on_done));
  cleanup();
}

/* ── MediaDownloadManager ──────────────────────────────────────────────────────────────── */

MediaDownloadManager::MediaDownloadManager(ProgressCb cb) : cb_(std::move(cb)) {}

MediaDownloadManager::~MediaDownloadManager() {
  CancelAll();
}

uint32_t MediaDownloadManager::AllocateIdLocked() {
  uint32_t id = next_id_++;
  if (next_id_ < 0x80000001u) next_id_ = 0x80000001u;
  return id;
}

std::string MediaDownloadManager::ResolveDownloadDir(const std::string& requested) const {
  std::string dir = requested;
  if (dir.empty()) {
    const gchar* dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    dir = dl ? dl : g_get_home_dir();
  }
  g_mkdir_with_parents(dir.c_str(), 0755);
  return dir;
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
  std::shared_ptr<MediaLoopbackProxy> proxy;
  {
    std::lock_guard<std::mutex> plock(job->proc_mu);
    if (job->proc) g_subprocess_force_exit(job->proc);
    proxy = job->proxy.lock();
  }
  if (proxy) proxy->Stop();
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
    std::shared_ptr<MediaLoopbackProxy> proxy;
    {
      std::lock_guard<std::mutex> plock(kv.second->proc_mu);
      if (kv.second->proc) g_subprocess_force_exit(kv.second->proc);
      proxy = kv.second->proxy.lock();
    }
    if (proxy) proxy->Stop();
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
  job->id = AllocateIdLocked();

  std::string dir = ResolveDownloadDir(download_dir);
  std::string stem = SanitizeMediaFilename(suggested_title, "");
  if (stem.empty()) stem = SanitizeMediaFilename(uri, "stream");

  FfmpegJobSpec spec;
  spec.ffmpeg_bin = ffmpeg_path;
  spec.video_uri = StripUrlFragment(uri);
  spec.headers_arg = BuildFfmpegHeaderArg(referer, cookie_header);
  spec.user_agent = user_agent;
  spec.proxied = false;

  jobs_[job->id] = job;
  job->worker = std::thread([this, job, uri, dir, stem, spec]() mutable {
    RunFfmpegJob(job, uri, dir, stem, 0.0, std::move(spec), nullptr);
  });
  return job->id;
}

uint32_t MediaDownloadManager::StartProxiedStream(const ProxiedStreamRequest& req,
                                                  MediaFetchStreamFn fetcher) {
  g_autofree gchar* ffmpeg_path = g_find_program_in_path("ffmpeg");
  if (!ffmpeg_path || req.video_uri.empty() || !fetcher) return 0;

  auto proxy = std::make_shared<MediaLoopbackProxy>(std::move(fetcher));
  if (!proxy->Start()) return 0;

  std::lock_guard<std::mutex> lock(mu_);
  ReapFinishedLocked();
  auto job = std::make_shared<Job>();
  job->id = AllocateIdLocked();
  job->proxy = proxy;

  std::string dir = ResolveDownloadDir(req.download_dir);
  std::string stem = SanitizeMediaFilename(req.suggested_title, "");
  if (stem.empty()) stem = SanitizeMediaFilename(req.video_uri, "stream");

  FfmpegJobSpec spec;
  spec.ffmpeg_bin = ffmpeg_path;
  spec.video_uri = proxy->WrapUrl(StripUrlFragment(req.video_uri));
  if (!req.audio_uri.empty()) {
    spec.audio_uri = proxy->WrapUrl(StripUrlFragment(req.audio_uri));
  }
  if (!req.subtitle_uri.empty()) {
    spec.subtitle_uri = proxy->WrapUrl(StripUrlFragment(req.subtitle_uri));
  }
  if (req.kind == "HLS") spec.input_format = "hls";
  else if (req.kind == "DASH") spec.input_format = "dash";
  spec.dash_rep_id = req.dash_rep_id;
  spec.audio_only = req.audio_only;
  spec.proxied = true;

  jobs_[job->id] = job;
  std::string display_url = req.video_uri;
  double dur = req.duration_sec;
  job->worker = std::thread([this, job, display_url, dir, stem, dur, spec, proxy]() mutable {
    RunFfmpegJob(job, display_url, dir, stem, dur, std::move(spec), std::move(proxy));
  });
  return job->id;
}

void MediaDownloadManager::RunFfmpegJob(const std::shared_ptr<Job>& job,
                                        const std::string& display_url,
                                        const std::string& dir,
                                        const std::string& stem,
                                        double duration_sec,
                                        FfmpegJobSpec spec,
                                        std::shared_ptr<MediaLoopbackProxy> proxy) {
  const std::string primary_ext = spec.audio_only ? "m4a" : "mp4";
  std::string out_path = BuildUniqueMediaPath(dir, stem, primary_ext);
  g_autofree gchar* bname = g_path_get_basename(out_path.c_str());

  ChromeDownloadView dl;
  dl.id = job->id;
  dl.url = display_url;
  dl.full_path = out_path;
  dl.filename = bname ? bname : (stem + "." + primary_ext);
  dl.percent = 0;
  dl.updated_at_ms = g_get_real_time() / 1000;
  dl.in_progress = true;
  EmitUpdate(dl);

  std::string last_stderr;
  auto run_once = [&](const std::string& target_path, bool mkv_fallback) -> bool {
    last_stderr.clear();
    spec.out_path = target_path;
    spec.mkv_fallback = mkv_fallback;
    std::vector<std::string> args = spec.proxied
        ? BuildFfmpegJobArgs(spec)
        : BuildFfmpegRemuxArgs(spec.ffmpeg_bin, spec.video_uri, target_path,
                               spec.headers_arg, spec.user_agent, mkv_fallback);
    std::vector<const gchar*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_PIPE));
    if (spec.proxied) {
      g_subprocess_launcher_unsetenv(launcher, "http_proxy");
      g_subprocess_launcher_unsetenv(launcher, "HTTP_PROXY");
      g_subprocess_launcher_unsetenv(launcher, "https_proxy");
      g_subprocess_launcher_unsetenv(launcher, "HTTPS_PROXY");
    }
    g_autoptr(GError) err = nullptr;
    GSubprocess* proc = g_subprocess_launcher_spawnv(launcher, argv.data(), &err);
    if (!proc) {
      if (err && err->message) last_stderr = err->message;
      return false;
    }
    {
      std::lock_guard<std::mutex> plock(job->proc_mu);
      job->proc = proc;
    }

    std::string stderr_buf;
    std::thread stderr_thread;
    if (GInputStream* err_pipe = g_subprocess_get_stderr_pipe(proc)) {
      g_object_ref(err_pipe);
      stderr_thread = std::thread([err_pipe, &stderr_buf]() {
        char buf[1024];
        for (;;) {
          gssize r = g_input_stream_read(err_pipe, buf, sizeof(buf), nullptr, nullptr);
          if (r <= 0) break;
          stderr_buf.append(buf, static_cast<size_t>(r));
          if (stderr_buf.size() > 8192) {
            stderr_buf.erase(0, stderr_buf.size() - 4096);
          }
        }
        g_object_unref(err_pipe);
      });
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
            if (duration_sec > 0.0 && sample.out_time_us > 0) {
              double pct = 100.0 * (static_cast<double>(sample.out_time_us) / (duration_sec * 1e6));
              dl.percent = std::clamp(static_cast<int>(pct + 0.5), 0, 99);
            } else {
              dl.percent = -1;
            }
            dl.updated_at_ms = g_get_real_time() / 1000;
            EmitUpdate(dl);
          }
        }
      }
    }

    gboolean ok = g_subprocess_wait_check(proc, nullptr, nullptr);
    if (stderr_thread.joinable()) stderr_thread.join();
    last_stderr = LastNonEmptyLine(stderr_buf);
    {
      std::lock_guard<std::mutex> plock(job->proc_mu);
      g_clear_object(&job->proc);
    }
    return ok != FALSE;
  };

  bool ok = run_once(out_path, false);
  if (!ok && !job->canceled.load(std::memory_order_acquire) && !spec.audio_only) {
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

  if (proxy) proxy->Stop();

  const int64_t final_bytes = FileSizeBytes(out_path);
  const bool keep_partial = (!ok && final_bytes > 65536 && dl.percent >= 5);
  if (ok && !job->canceled.load(std::memory_order_acquire) && final_bytes > 0) {
    dl.in_progress = false;
    dl.complete = true;
    dl.canceled = false;
    dl.percent = 100;
    dl.received_bytes = final_bytes;
    dl.total_bytes = final_bytes;
    dl.current_speed = 0;
    dl.error_message.clear();
  } else {
    if (!keep_partial) g_unlink(out_path.c_str());
    dl.in_progress = false;
    dl.complete = false;
    dl.canceled = true;
    dl.current_speed = 0;
    if (job->canceled.load(std::memory_order_acquire)) {
      dl.error_message = keep_partial ? "Canceled (partial file kept)" : "Canceled by user";
    } else if (!last_stderr.empty()) {
      dl.error_message = last_stderr;
    } else if (final_bytes == 0) {
      dl.error_message = "ffmpeg produced empty output (check stream encryption or network)";
    } else {
      dl.error_message = "ffmpeg exited with error";
    }
  }
  dl.updated_at_ms = g_get_real_time() / 1000;
  EmitUpdate(dl);
  job->finished.store(true, std::memory_order_release);
}

uint32_t MediaDownloadManager::StartProgressiveDownload(const ProgressiveDownloadRequest& req,
                                                        MediaFetchStreamFn fetcher) {
  if (req.uri.empty() || !fetcher) return 0;

  std::lock_guard<std::mutex> lock(mu_);
  ReapFinishedLocked();
  auto job = std::make_shared<Job>();
  job->id = AllocateIdLocked();

  std::string dir = ResolveDownloadDir(req.download_dir);
  std::string ext = MapMimeToExt(req.mime_type, req.uri);
  std::string stem = !req.suggested_filename.empty()
                         ? SanitizeMediaFilename(req.suggested_filename, "")
                         : SanitizeMediaFilename(req.suggested_title, "");
  if (stem.empty()) stem = SanitizeMediaFilename(req.uri, "video");
  std::string out_path = BuildUniqueMediaPath(dir, stem, ext);

  jobs_[job->id] = job;
  job->worker = std::thread([this, job, req, dir, out_path, fetcher = std::move(fetcher)]() mutable {
    RunProgressiveWorker(job, std::move(req), std::move(dir), std::move(out_path), std::move(fetcher));
  });
  return job->id;
}

void MediaDownloadManager::RunProgressiveWorker(const std::shared_ptr<Job>& job,
                                                ProgressiveDownloadRequest req,
                                                std::string dir G_GNUC_UNUSED,
                                                std::string out_path,
                                                MediaFetchStreamFn fetcher) {
  g_autofree gchar* bname = g_path_get_basename(out_path.c_str());
  ChromeDownloadView dl;
  dl.id = job->id;
  dl.url = req.uri;
  dl.full_path = out_path;
  dl.filename = bname ? bname : "video.mp4";
  dl.percent = 0;
  dl.total_bytes = req.expected_size;
  dl.updated_at_ms = g_get_real_time() / 1000;
  dl.in_progress = true;
  EmitUpdate(dl);

  FILE* fp = std::fopen(out_path.c_str(), "wb");
  if (!fp) {
    dl.in_progress = false;
    dl.canceled = true;
    dl.error_message = "Cannot create output file";
    dl.updated_at_ms = g_get_real_time() / 1000;
    EmitUpdate(dl);
    job->finished.store(true, std::memory_order_release);
    return;
  }

  int http_status = 0;
  int64_t received = 0;
  int64_t total = req.expected_size;
  bool fetch_ok = false;
  std::string fetch_err;
  bool write_err = false;
  const gint64 start_us = g_get_monotonic_time();
  gint64 last_emit_us = start_us;

  std::map<std::string, std::string> empty_hdrs;
  fetcher(
      req.uri, "GET", empty_hdrs, &job->canceled,
      [&](int status, const std::map<std::string, std::string>& hdrs) {
        http_status = status;
        for (const auto& kv : hdrs) {
          if (ToLowerAscii(kv.first) == "content-length") {
            int64_t cl = std::strtoll(kv.second.c_str(), nullptr, 10);
            if (cl > 0) total = cl;
          }
        }
      },
      [&](const char* data, size_t len) -> bool {
        if (job->canceled.load(std::memory_order_acquire) || http_status < 200 || http_status >= 300) {
          return false;
        }
        if (data && len > 0) {
          if (std::fwrite(data, 1, len, fp) != len) {
            write_err = true;
            return false;
          }
          received += static_cast<int64_t>(len);
          gint64 now_us = g_get_monotonic_time();
          if (now_us - last_emit_us >= 200000) {
            last_emit_us = now_us;
            double elapsed_s = std::max(0.1, static_cast<double>(now_us - start_us) / 1e6);
            dl.received_bytes = received;
            dl.total_bytes = total;
            dl.current_speed = static_cast<int64_t>(static_cast<double>(received) / elapsed_s);
            dl.percent = (total > 0) ? std::clamp(static_cast<int>((received * 100) / total), 0, 99) : -1;
            dl.updated_at_ms = g_get_real_time() / 1000;
            EmitUpdate(dl);
          }
        }
        return true;
      },
      [&](bool ok, const std::string& err) {
        fetch_ok = ok;
        fetch_err = err;
      });

  std::fclose(fp);

  const int64_t final_bytes = FileSizeBytes(out_path);
  const bool success = fetch_ok && !write_err && !job->canceled.load(std::memory_order_acquire) &&
                       http_status >= 200 && http_status < 300 && final_bytes > 0;
  if (success) {
    dl.in_progress = false;
    dl.complete = true;
    dl.canceled = false;
    dl.percent = 100;
    dl.received_bytes = final_bytes;
    dl.total_bytes = final_bytes;
    dl.current_speed = 0;
    dl.error_message.clear();
  } else {
    g_unlink(out_path.c_str());
    dl.in_progress = false;
    dl.complete = false;
    dl.canceled = true;
    dl.current_speed = 0;
    if (job->canceled.load(std::memory_order_acquire)) {
      dl.error_message = "Canceled by user";
    } else if (write_err) {
      dl.error_message = "Disk write failed";
    } else if (http_status >= 400) {
      dl.error_message = "HTTP error " + std::to_string(http_status);
    } else if (!fetch_err.empty()) {
      dl.error_message = fetch_err;
    } else {
      dl.error_message = "Download failed";
    }
  }
  dl.updated_at_ms = g_get_real_time() / 1000;
  EmitUpdate(dl);
  job->finished.store(true, std::memory_order_release);
}

uint32_t MediaDownloadManager::StartYtDlp(const std::string& page_url,
                                          const std::string& suggested_title,
                                          const std::string& download_dir,
                                          const std::string& user_agent,
                                          const std::string& referer,
                                          const std::string& cookies_file,
                                          const std::string& format_id,
                                          bool audio_only) {
  g_autofree gchar* ytdlp_path = g_find_program_in_path("yt-dlp");
  if (!ytdlp_path || page_url.empty()) {
    RemoveTempFileSecurely(cookies_file);
    return 0;
  }

  std::lock_guard<std::mutex> lock(mu_);
  ReapFinishedLocked();
  auto job = std::make_shared<Job>();
  job->id = AllocateIdLocked();

  std::string dir = ResolveDownloadDir(download_dir);
  std::string stem = SanitizeMediaFilename(suggested_title, "");
  if (stem.empty()) stem = SanitizeMediaFilename(page_url, "video");

  jobs_[job->id] = job;
  job->worker = std::thread([this, job, page_url, dir, stem, user_agent,
                             referer, cookies_file, format_id, audio_only]() {
    RunYtDlpWorker(job, page_url, dir, stem, user_agent, referer, cookies_file, format_id, audio_only);
  });
  return job->id;
}

void MediaDownloadManager::RunYtDlpWorker(const std::shared_ptr<Job>& job,
                                          std::string page_url,
                                          std::string dir,
                                          std::string stem,
                                          std::string user_agent,
                                          std::string referer,
                                          std::string cookies_file,
                                          std::string format_id,
                                          bool audio_only) {
  g_autofree gchar* ytdlp_bin = g_find_program_in_path("yt-dlp");
  if (!ytdlp_bin) {
    RemoveTempFileSecurely(cookies_file);
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
  if (!format_id.empty()) {
    args.emplace_back("-f");
    args.push_back(audio_only ? format_id : (format_id + "+bestaudio/best"));
  } else if (audio_only) {
    args.emplace_back("-f");
    args.emplace_back("bestaudio/best");
  }
  if (!user_agent.empty()) {
    args.emplace_back("--user-agent");
    args.push_back(StripCrLf(user_agent));
  }
  if (!referer.empty()) {
    args.emplace_back("--referer");
    args.push_back(StripCrLf(referer));
  }
  if (!cookies_file.empty()) {
    args.emplace_back("--cookies");
    args.push_back(cookies_file);
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
  std::string last_err;
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
        if (line.rfind("ERROR:", 0) == 0) last_err = line;
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
  } else if (err && err->message) {
    last_err = err->message;
  }

  RemoveTempFileSecurely(cookies_file);

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
    dl.error_message.clear();
  } else {
    if (!final_path.empty()) g_unlink(final_path.c_str());
    dl.in_progress = false;
    dl.complete = false;
    dl.canceled = true;
    dl.current_speed = 0;
    dl.error_message = !last_err.empty() ? last_err
                                         : (job->canceled.load(std::memory_order_acquire)
                                                ? "Canceled by user"
                                                : "yt-dlp failed");
  }
  dl.updated_at_ms = g_get_real_time() / 1000;
  EmitUpdate(dl);
  job->finished.store(true, std::memory_order_release);
}

void MediaDownloadManager::ProbeYtDlpAsync(const std::string& page_url,
                                           const std::string& user_agent,
                                           const std::string& referer,
                                           const std::string& cookies_file,
                                           YtDlpProbeCb cb) {
  g_autofree gchar* ytdlp_path = g_find_program_in_path("yt-dlp");
  if (!ytdlp_path || page_url.empty()) {
    RemoveTempFileSecurely(cookies_file);
    if (cb) cb(false, "", {}, "yt-dlp not found in PATH");
    return;
  }

  std::lock_guard<std::mutex> lock(mu_);
  ReapFinishedLocked();
  auto job = std::make_shared<Job>();
  job->id = AllocateIdLocked();
  jobs_[job->id] = job;

  job->worker = std::thread([job, page_url, user_agent, referer, cookies_file, cb = std::move(cb)]() mutable {
    g_autofree gchar* ytdlp_bin = g_find_program_in_path("yt-dlp");
    if (!ytdlp_bin || job->canceled.load(std::memory_order_acquire)) {
      RemoveTempFileSecurely(cookies_file);
      if (cb) cb(false, "", {}, "yt-dlp probe canceled");
      job->finished.store(true, std::memory_order_release);
      return;
    }

    std::vector<std::string> args = {ytdlp_bin, "-J", "--no-playlist", "--no-warnings"};
    if (!user_agent.empty()) {
      args.emplace_back("--user-agent");
      args.push_back(StripCrLf(user_agent));
    }
    if (!referer.empty()) {
      args.emplace_back("--referer");
      args.push_back(StripCrLf(referer));
    }
    if (!cookies_file.empty()) {
      args.emplace_back("--cookies");
      args.push_back(cookies_file);
    }
    args.push_back(page_url);

    std::vector<const gchar*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_PIPE));
    g_autoptr(GError) err = nullptr;
    GSubprocess* proc = g_subprocess_launcher_spawnv(launcher, argv.data(), &err);
    std::string stdout_s;
    std::string stderr_s;
    bool ok = false;
    if (proc) {
      {
        std::lock_guard<std::mutex> plock(job->proc_mu);
        job->proc = proc;
      }
      g_autofree gchar* out_c = nullptr;
      g_autofree gchar* err_c = nullptr;
      g_autoptr(GError) comm_err = nullptr;
      ok = g_subprocess_communicate_utf8(proc, nullptr, nullptr, &out_c, &err_c, &comm_err) != FALSE;
      ok = ok && g_subprocess_get_successful(proc);
      if (out_c) stdout_s = out_c;
      if (err_c) stderr_s = err_c;
      {
        std::lock_guard<std::mutex> plock(job->proc_mu);
        g_clear_object(&job->proc);
      }
    } else if (err && err->message) {
      stderr_s = err->message;
    }

    RemoveTempFileSecurely(cookies_file);

    if (!job->canceled.load(std::memory_order_acquire)) {
      if (!ok || stdout_s.empty()) {
        std::string msg = LastNonEmptyLine(stderr_s);
        if (cb) cb(false, "", {}, msg.empty() ? "yt-dlp probe failed" : msg);
      } else {
        std::string title;
        auto formats = ParseYtDlpInfoJson(stdout_s, &title);
        if (cb) cb(!formats.empty(), std::move(title), std::move(formats),
                   formats.empty() ? "No downloadable formats reported by yt-dlp" : "");
      }
    }
    job->finished.store(true, std::memory_order_release);
  });
}

}  // namespace kajo_web
