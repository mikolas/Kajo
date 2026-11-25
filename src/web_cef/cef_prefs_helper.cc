#include "web_cef/cef_prefs_helper.h"

#include <glib.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "config/kajo_config_types.h"
#include "web_cef/cef_site_util.h"

namespace kajo_web {

namespace {

std::string ToLowerAscii(const std::string& s) {
  return ToLowerAsciiCopy(s);
}

bool IsHttpOrHttps(const std::string& url, size_t* out_scheme_end) {
  const std::string low = ToLowerAscii(url);
  if (low.rfind("https://", 0) == 0) {
    if (out_scheme_end) *out_scheme_end = 8;
    return true;
  }
  if (low.rfind("http://", 0) == 0) {
    if (out_scheme_end) *out_scheme_end = 7;
    return true;
  }
  return false;
}

std::string ExtractLowerHost(const std::string& url) {
  size_t pos = 0;
  if (!IsHttpOrHttps(url, &pos)) return "";
  size_t end = url.find_first_of("/?#", pos);
  std::string auth = (end == std::string::npos) ? url.substr(pos) : url.substr(pos, end - pos);
  size_t at = auth.rfind('@');
  if (at != std::string::npos) auth = auth.substr(at + 1);
  if (auth.empty()) return "";
  if (auth.front() == '[') {
    size_t close = auth.find(']');
    if (close != std::string::npos) return ToLowerAscii(auth.substr(1, close - 1));
  }
  size_t colon = auth.rfind(':');
  if (colon != std::string::npos) auth = auth.substr(0, colon);
  while (!auth.empty() && auth.back() == '.') auth.pop_back();
  return ToLowerAscii(auth);
}

}  // namespace

cef_log_severity_t ParseCefLogSeverity(const char* env_val) {
  if (!env_val || env_val[0] == '\0') return LOGSEVERITY_FATAL;
  const std::string v = ToLowerAscii(env_val);
  if (v == "verbose" || v == "debug") return LOGSEVERITY_VERBOSE;
  if (v == "info") return LOGSEVERITY_INFO;
  if (v == "warn" || v == "warning") return LOGSEVERITY_WARNING;
  if (v == "error") return LOGSEVERITY_ERROR;
  if (v == "disable" || v == "off" || v == "none") return LOGSEVERITY_DISABLE;
  return LOGSEVERITY_FATAL;
}

bool IsSameSiteUrl(const std::string& req_url, const std::string& first_party_url) {
  if (first_party_url.empty()) return true;
  const std::string req_host = ExtractLowerHost(req_url);
  const std::string fp_host = ExtractLowerHost(first_party_url);
  if (req_host.empty() || fp_host.empty()) return true;
  return IsSameSiteHost(req_host, fp_host);
}

LoadedUserScripts LoadUserScriptsFromDir(const std::string& scripts_dir) {
  LoadedUserScripts out;
  if (scripts_dir.empty()) return out;
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(scripts_dir, ec) || !fs::is_directory(scripts_dir, ec)) return out;

  std::vector<fs::path> css_paths;
  std::vector<fs::path> js_paths;
  for (const auto& entry : fs::directory_iterator(scripts_dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    const std::string low = ToLowerAscii(name);
    if (low.size() > 4 && low.compare(low.size() - 4, 4, ".css") == 0) {
      css_paths.push_back(entry.path());
    } else if (low.size() > 3 && low.compare(low.size() - 3, 3, ".js") == 0) {
      js_paths.push_back(entry.path());
    }
  }
  std::sort(css_paths.begin(), css_paths.end());
  std::sort(js_paths.begin(), js_paths.end());

  constexpr size_t kMaxFileBytes = 512U * 1024U;
  constexpr size_t kMaxFiles = 64U;

  auto read_capped = [](const fs::path& p, std::string* dst) -> bool {
    std::error_code sec;
    auto sz = fs::file_size(p, sec);
    if (sec || sz == 0 || sz > kMaxFileBytes) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *dst = ss.str();
    return !dst->empty();
  };

  size_t count = 0;
  for (const auto& p : css_paths) {
    if (count >= kMaxFiles) break;
    std::string body;
    if (read_capped(p, &body)) {
      if (!out.combined_css.empty()) out.combined_css.push_back('\n');
      out.combined_css += body;
      ++count;
    }
  }
  for (const auto& p : js_paths) {
    if (count >= kMaxFiles) break;
    std::string body;
    if (read_capped(p, &body)) {
      out.js_files.emplace_back(p.filename().string(), std::move(body));
      ++count;
    }
  }
  return out;
}

std::string PermissionStore::MakeKey(const std::string& origin, const std::string& perm_key) {
  return NormalizeOrigin(origin) + "|" + perm_key;
}

bool PermissionStore::Load(const std::string& filepath) {
  entries_.clear();
  if (filepath.empty() || !g_file_test(filepath.c_str(), G_FILE_TEST_EXISTS)) return false;
  g_autofree gchar* data = nullptr;
  gsize len = 0;
  if (!g_file_get_contents(filepath.c_str(), &data, &len, nullptr) || !data) return false;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, data, static_cast<gssize>(len), nullptr)) return false;
  JsonNode* root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return false;
  JsonObject* obj = json_node_get_object(root);

  JsonObjectIter it;
  const gchar* k = nullptr;
  JsonNode* v = nullptr;
  json_object_iter_init(&it, obj);
  while (json_object_iter_next(&it, &k, &v)) {
    if (!k || !v || !JSON_NODE_HOLDS_VALUE(v)) continue;
    const gchar* val = json_node_get_string(v);
    if (!val) continue;
    if (g_strcmp0(val, "allow") == 0) entries_[k] = true;
    else if (g_strcmp0(val, "deny") == 0) entries_[k] = false;
  }
  return true;
}

bool PermissionStore::Save(const std::string& filepath) const {
  if (filepath.empty()) return false;
  g_autofree gchar* dir = g_path_get_dirname(filepath.c_str());
  if (dir) g_mkdir_with_parents(dir, 0700);

  g_autoptr(JsonBuilder) jb = json_builder_new();
  json_builder_begin_object(jb);
  for (const auto& kv : entries_) {
    json_builder_set_member_name(jb, kv.first.c_str());
    json_builder_add_string_value(jb, kv.second ? "allow" : "deny");
  }
  json_builder_end_object(jb);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  g_autoptr(JsonNode) root = json_builder_get_root(jb);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, TRUE);
  gsize out_len = 0;
  g_autofree gchar* json = json_generator_to_data(gen, &out_len);
  if (!json) return false;
  return g_file_set_contents(filepath.c_str(), json, static_cast<gssize>(out_len), nullptr) != FALSE;
}

int PermissionStore::Lookup(const std::string& origin, const std::string& perm_key) const {
  const std::string key = MakeKey(origin, perm_key);
  auto it = entries_.find(key);
  if (it == entries_.end()) return 0;
  return it->second ? 1 : -1;
}

void PermissionStore::Set(const std::string& origin, const std::string& perm_key, bool allow) {
  entries_[MakeKey(origin, perm_key)] = allow;
}

bool PermissionStore::Remove(const std::string& origin, const std::string& perm_key) {
  return entries_.erase(MakeKey(origin, perm_key)) > 0;
}

std::vector<PermissionStore::Entry> PermissionStore::ListEntries() const {
  std::vector<Entry> out;
  out.reserve(entries_.size());
  for (const auto& kv : entries_) {
    size_t bar = kv.first.rfind('|');
    if (bar == std::string::npos) continue;
    Entry e;
    e.origin = kv.first.substr(0, bar);
    e.perm_key = kv.first.substr(bar + 1);
    e.allow = kv.second;
    out.push_back(std::move(e));
  }
  return out;
}

void PermissionStore::Clear() {
  entries_.clear();
}

std::string FormatPermissionLabel(uint32_t media_perms, uint32_t perm_types) {
  std::vector<std::string> parts;
  auto add = [&parts](const char* s) {
    if (std::find(parts.begin(), parts.end(), s) == parts.end()) parts.emplace_back(s);
  };

  if (media_perms & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE) add("Camera");
  if (media_perms & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE) add("Microphone");
  if (media_perms & (CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE |
                     CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE)) {
    add("Screen Sharing");
  }

  if (perm_types & CEF_PERMISSION_TYPE_CAMERA_STREAM) add("Camera");
  if (perm_types & CEF_PERMISSION_TYPE_MIC_STREAM) add("Microphone");
  if (perm_types & CEF_PERMISSION_TYPE_CAMERA_PAN_TILT_ZOOM) add("Camera Pan/Tilt/Zoom");
  if (perm_types & CEF_PERMISSION_TYPE_NOTIFICATIONS) add("Notifications");
  if (perm_types & CEF_PERMISSION_TYPE_GEOLOCATION) add("Location");
  if (perm_types & CEF_PERMISSION_TYPE_CLIPBOARD) add("Clipboard");
  if (perm_types & CEF_PERMISSION_TYPE_MIDI_SYSEX) add("MIDI Devices");
  if (perm_types & CEF_PERMISSION_TYPE_IDLE_DETECTION) add("Idle Detection");
  if (perm_types & CEF_PERMISSION_TYPE_LOCAL_FONTS) add("Local Fonts");
  if (perm_types & CEF_PERMISSION_TYPE_MULTIPLE_DOWNLOADS) add("Multiple Downloads");
  if (perm_types & CEF_PERMISSION_TYPE_POINTER_LOCK) add("Pointer Lock");
  if (perm_types & CEF_PERMISSION_TYPE_KEYBOARD_LOCK) add("Keyboard Lock");
  if (perm_types & CEF_PERMISSION_TYPE_PROTECTED_MEDIA_IDENTIFIER) add("Protected Media");

  if (parts.empty()) return "Site Permission";
  std::string out = parts[0];
  for (size_t i = 1; i < parts.size(); ++i) {
    out += (i + 1 == parts.size()) ? " & " : ", ";
    out += parts[i];
  }
  return out;
}

std::string PermissionKey(uint32_t media_perms, uint32_t perm_types) {
  std::ostringstream o;
  o << "m" << media_perms << "_p" << perm_types;
  return o.str();
}

std::string FormatPermissionKeyLabel(const std::string& perm_key) {
  if (perm_key.size() < 4 || perm_key[0] != 'm') return "Site Permission";
  size_t sep = perm_key.find("_p");
  if (sep == std::string::npos) return "Site Permission";
  uint32_t media_perms = static_cast<uint32_t>(std::strtoul(perm_key.c_str() + 1, nullptr, 10));
  uint32_t perm_types = static_cast<uint32_t>(std::strtoul(perm_key.c_str() + sep + 2, nullptr, 10));
  return FormatPermissionLabel(media_perms, perm_types);
}

std::string ExpandTildePath(const std::string& raw) {
  size_t start = 0;
  while (start < raw.size() && std::isspace(static_cast<unsigned char>(raw[start]))) ++start;
  size_t end = raw.size();
  while (end > start && std::isspace(static_cast<unsigned char>(raw[end - 1]))) --end;
  std::string s = raw.substr(start, end - start);
  if (s.empty()) return "";
  const gchar* home = g_get_home_dir();
  if (!home) return s;
  if (s == "~") return std::string(home);
  if (s.rfind("~/", 0) == 0) {
    g_autofree gchar* joined = g_build_filename(home, s.c_str() + 2, nullptr);
    return joined ? std::string(joined) : s;
  }
  return s;
}

bool IsSafeProfileSlug(const std::string& id) {
  if (id.empty() || id == "default") return false;
  for (unsigned char c : id) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      return false;
    }
  }
  return true;
}

cef_color_t ParseHexColorArgb(const std::string& hex, cef_color_t fallback) {
  if (hex.size() != 7 || hex[0] != '#') return fallback;
  auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  int vals[6];
  for (int i = 0; i < 6; ++i) {
    vals[i] = hex_val(hex[static_cast<size_t>(i + 1)]);
    if (vals[i] < 0) return fallback;
  }
  const uint8_t r = static_cast<uint8_t>((vals[0] << 4) | vals[1]);
  const uint8_t g = static_cast<uint8_t>((vals[2] << 4) | vals[3]);
  const uint8_t b = static_cast<uint8_t>((vals[4] << 4) | vals[5]);
  return CefColorSetARGB(255, r, g, b);
}

cef_color_t ContentBrowserDefaultBackgroundColor() {
  return 0;
}

bool IsLightSurfaceHex(const std::string& hex) {
  const cef_color_t surf = ParseHexColorArgb(
      hex, CefColorSetARGB(255, 0x14, 0x14, 0x14));
  const double lum = (0.2126 * CefColorGetR(surf) +
                      0.7152 * CefColorGetG(surf) +
                      0.0722 * CefColorGetB(surf)) / 255.0;
  return lum > 0.5;
}

cef_color_variant_t ResolveWebColorSchemeVariant(int color_scheme,
                                                 bool kajo_theme_is_light) {
  if (color_scheme == KAJO_WEB_SCHEME_LIGHT ||
      (color_scheme == KAJO_WEB_SCHEME_FOLLOW_KAJO && kajo_theme_is_light)) {
    return CEF_COLOR_VARIANT_LIGHT;
  }
  return CEF_COLOR_VARIANT_DARK;
}

TabColorSchemeOverride NextTabColorSchemeOverride(TabColorSchemeOverride cur,
                                                  bool default_is_dark) {
  if (default_is_dark) {
    switch (cur) {
      case TabColorSchemeOverride::AUTO: return TabColorSchemeOverride::LIGHT;
      case TabColorSchemeOverride::LIGHT: return TabColorSchemeOverride::DARK;
      case TabColorSchemeOverride::DARK: return TabColorSchemeOverride::AUTO;
    }
  } else {
    switch (cur) {
      case TabColorSchemeOverride::AUTO: return TabColorSchemeOverride::DARK;
      case TabColorSchemeOverride::DARK: return TabColorSchemeOverride::LIGHT;
      case TabColorSchemeOverride::LIGHT: return TabColorSchemeOverride::AUTO;
    }
  }
  return TabColorSchemeOverride::AUTO;
}

const char* TabColorSchemeEmulatedMediaValue(TabColorSchemeOverride ov) {
  switch (ov) {
    case TabColorSchemeOverride::LIGHT: return "light";
    case TabColorSchemeOverride::DARK: return "dark";
    case TabColorSchemeOverride::AUTO: return "";
  }
  return "";
}

const char* TabColorSchemeOverrideLabel(TabColorSchemeOverride ov) {
  switch (ov) {
    case TabColorSchemeOverride::LIGHT: return "LIGHT";
    case TabColorSchemeOverride::DARK: return "DARK";
    case TabColorSchemeOverride::AUTO: return "AUTO";
  }
  return "AUTO";
}

std::string NormalizeOrigin(const std::string& url_or_origin) {
  size_t pos = 0;
  if (!IsHttpOrHttps(url_or_origin, &pos)) return ToLowerAscii(url_or_origin);
  size_t slash = url_or_origin.find_first_of("/?#", pos);
  std::string origin = (slash == std::string::npos) ? url_or_origin : url_or_origin.substr(0, slash);
  while (!origin.empty() && origin.back() == '/') origin.pop_back();
  return ToLowerAscii(origin);
}

}  // namespace kajo_web
