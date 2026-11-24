#include "web_cef/cef_webapps.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <sstream>

#include "web_cef/cef_site_util.h"

namespace kajo_web {

namespace {

std::string SafeProfileSlug(const std::string& profile_id) {
  std::string slug = SanitizeWebAppId(profile_id.empty() ? "default" : profile_id);
  if (slug.empty()) slug = "default";
  return slug;
}

std::string NormalizeHexColorToken(const std::string& raw) {
  std::string s = raw;
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  if (s.size() == 4 && s[0] == '#') {
    bool ok = true;
    for (size_t i = 1; i < 4; ++i) {
      if (!std::isxdigit(static_cast<unsigned char>(s[i]))) ok = false;
    }
    if (ok) {
      std::string expanded = "#";
      for (size_t i = 1; i < 4; ++i) {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        expanded.push_back(c);
        expanded.push_back(c);
      }
      return expanded;
    }
  }
  return SanitizeHexColor(s, "#0078d4");
}

int EffectivePort(GUri* uri) {
  if (!uri) return -1;
  int port = g_uri_get_port(uri);
  if (port > 0) return port;
  const char* scheme = g_uri_get_scheme(uri);
  if (!scheme) return -1;
  std::string low = ToLowerAsciiCopy(scheme);
  if (low == "https") return 443;
  if (low == "http") return 80;
  return -1;
}

std::string NormalizedPath(GUri* uri) {
  if (!uri) return "/";
  const char* p = g_uri_get_path(uri);
  if (!p || p[0] == '\0') return "/";
  return std::string(p);
}

int ParseMaxIconSize(const char* sizes_str) {
  if (!sizes_str || sizes_str[0] == '\0') return 0;
  std::string low = ToLowerAsciiCopy(sizes_str);
  if (low.find("any") != std::string::npos) return 512;
  int best = 0;
  size_t i = 0;
  while (i < low.size()) {
    while (i < low.size() && !std::isdigit(static_cast<unsigned char>(low[i]))) ++i;
    if (i >= low.size()) break;
    int w = 0;
    while (i < low.size() && std::isdigit(static_cast<unsigned char>(low[i]))) {
      w = w * 10 + (low[i] - '0');
      ++i;
    }
    if (w > best) best = w;
    if (i < low.size() && low[i] == 'x') {
      ++i;
      int h = 0;
      while (i < low.size() && std::isdigit(static_cast<unsigned char>(low[i]))) {
        h = h * 10 + (low[i] - '0');
        ++i;
      }
      if (h > best) best = h;
    }
  }
  return best;
}

bool LooksLikePng(const std::vector<uint8_t>& bytes) {
  return bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' &&
         bytes[2] == 'N' && bytes[3] == 'G' && bytes[4] == 0x0d &&
         bytes[5] == 0x0a && bytes[6] == 0x1a && bytes[7] == 0x0a;
}

bool LooksLikeSvg(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 16) return false;
  size_t limit = std::min<size_t>(bytes.size(), 512);
  std::string head(reinterpret_cast<const char*>(bytes.data()), limit);
  std::string low = ToLowerAsciiCopy(head);
  return low.find("<svg") != std::string::npos;
}

std::string DesktopQuoteArg(const std::string& arg) {
  std::string out = "\"";
  for (char c : arg) {
    if (c == '"' || c == '\\' || c == '$' || c == '`') {
      out.push_back('\\');
    }
    if (c == '%') {
      out += "%%";
      continue;
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string XmlEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string EffectiveXdgDataHome(const std::string& custom_xdg_data_home) {
  if (!custom_xdg_data_home.empty()) return custom_xdg_data_home;
  const char* env = g_get_user_data_dir();
  return env ? std::string(env) : std::string("/tmp");
}

}  // namespace

std::string SanitizeWebAppId(const std::string& raw) {
  std::string out;
  bool prev_dash = true;
  for (unsigned char c : raw) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      prev_dash = false;
    } else if (!prev_dash && (c == '-' || c == '_' || c == ' ' || c == '.')) {
      out.push_back('-');
      prev_dash = true;
    }
    if (out.size() >= 48) break;
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.empty()) out = "webapp";
  return out;
}

std::string WebAppWaylandAppId(const std::string& profile_id, const std::string& app_id) {
  return "kajo-webapp-" + SafeProfileSlug(profile_id) + "-" + SanitizeWebAppId(app_id);
}

std::string WebAppDesktopId(const std::string& profile_id, const std::string& app_id) {
  return WebAppWaylandAppId(profile_id, app_id) + ".desktop";
}

std::string DeriveOriginFromUrl(const std::string& url) {
  if (url.empty()) return "";
  g_autoptr(GUri) uri = g_uri_parse(url.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  if (!uri) return "";
  const char* scheme = g_uri_get_scheme(uri);
  const char* host = g_uri_get_host(uri);
  if (!scheme || !host || host[0] == '\0') return "";
  std::string low_scheme = ToLowerAsciiCopy(scheme);
  if (low_scheme != "http" && low_scheme != "https") return "";
  std::string low_host = ToLowerAsciiCopy(host);
  if (low_host.find(':') != std::string::npos && low_host.front() != '[') {
    low_host = "[" + low_host + "]";
  }
  int port = g_uri_get_port(uri);
  std::string origin = low_scheme + "://" + low_host;
  if (port > 0 &&
      !((low_scheme == "https" && port == 443) || (low_scheme == "http" && port == 80))) {
    origin += ":" + std::to_string(port);
  }
  origin += "/";
  return origin;
}

std::string ResolveUrlAgainstBase(const std::string& base_url, const std::string& ref) {
  if (ref.empty()) return base_url;
  g_autoptr(GUri) base = g_uri_parse(base_url.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  if (!base) {
    g_autoptr(GUri) direct = g_uri_parse(ref.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
    if (!direct) return "";
    g_autofree gchar* s = g_uri_to_string(direct);
    return s ? std::string(s) : "";
  }
  g_autoptr(GUri) resolved =
      g_uri_parse_relative(base, ref.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  if (!resolved) return "";
  g_autofree gchar* out = g_uri_to_string(resolved);
  return out ? std::string(out) : "";
}

bool IsUrlInWebAppScope(const WebAppEntry& app, const std::string& target_url) {
  if (target_url.empty()) return true;
  std::string low = ToLowerAsciiCopy(target_url);
  if (low == "about:blank" || low.rfind("blob:", 0) == 0 || low.rfind("data:", 0) == 0) {
    return true;
  }

  std::string scope_url = !app.scope.empty() ? app.scope : DeriveOriginFromUrl(app.start_url);
  if (scope_url.empty()) return false;

  g_autoptr(GUri) scope_uri = g_uri_parse(scope_url.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  g_autoptr(GUri) tgt_uri = g_uri_parse(target_url.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  if (!scope_uri || !tgt_uri) return false;

  const char* s_scheme = g_uri_get_scheme(scope_uri);
  const char* t_scheme = g_uri_get_scheme(tgt_uri);
  const char* s_host = g_uri_get_host(scope_uri);
  const char* t_host = g_uri_get_host(tgt_uri);
  if (!s_scheme || !t_scheme || !s_host || !t_host) return false;

  std::string ls = ToLowerAsciiCopy(s_scheme);
  std::string lt = ToLowerAsciiCopy(t_scheme);
  if (ls != lt) return false;

  std::string lh_s = ToLowerAsciiCopy(s_host);
  std::string lh_t = ToLowerAsciiCopy(t_host);
  if (lh_s != lh_t) return false;

  if (EffectivePort(scope_uri) != EffectivePort(tgt_uri)) return false;

  std::string s_path = NormalizedPath(scope_uri);
  std::string t_path = NormalizedPath(tgt_uri);
  if (s_path == "/") return true;
  if (t_path.rfind(s_path, 0) != 0) return false;
  if (s_path.back() == '/' || t_path.size() == s_path.size()) return true;
  return t_path[s_path.size()] == '/';
}

bool IsLikelyAuthFlowUrl(const std::string& url) {
  if (url.empty()) return false;
  g_autoptr(GUri) uri = g_uri_parse(url.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  if (!uri) return false;
  const char* scheme = g_uri_get_scheme(uri);
  const char* host_c = g_uri_get_host(uri);
  if (!scheme || !host_c) return false;
  std::string lscheme = ToLowerAsciiCopy(scheme);
  if (lscheme != "https" && lscheme != "http") return false;

  std::string host = ToLowerAsciiCopy(host_c);
  std::string path = ToLowerAsciiCopy(NormalizedPath(uri));
  const char* q_c = g_uri_get_query(uri);
  std::string query = q_c ? ToLowerAsciiCopy(q_c) : "";

  static const char* kExactAuthHosts[] = {
      "login.microsoftonline.com",
      "login.live.com",
      "login.microsoft.com",
      "account.live.com",
      "aadcdn.msauth.net",
      "idmsa.apple.com",
      "appleid.apple.com",
      "iforgot.apple.com",
      "accounts.google.com",
      "accounts.youtube.com",
      "login.yahoo.com",
      "login.salesforce.com",
      "id.atlassian.com",
      "auth.atlassian.com",
      "appleid.cdn-apple.com",
  };
  for (const char* h : kExactAuthHosts) {
    if (host == h) return true;
  }

  static const char* kAuthHostSuffixes[] = {
      ".auth0.com",
      ".okta.com",
      ".oktapreview.com",
      ".duosecurity.com",
      ".onelogin.com",
      ".pingidentity.com",
      ".msauth.net",
      ".msftauth.net",
  };
  for (const char* suf : kAuthHostSuffixes) {
    size_t slen = std::strlen(suf);
    if (host.size() >= slen && host.compare(host.size() - slen, slen, suf) == 0) {
      return true;
    }
  }

  static const char* kAuthHostPrefixes[] = {
      "login.", "sso.", "auth.", "accounts.", "oauth.", "idp.", "signin.",
  };
  for (const char* pre : kAuthHostPrefixes) {
    if (host.rfind(pre, 0) == 0) return true;
  }

  if (path.find("/oauth") != std::string::npos || path.find("/saml") != std::string::npos ||
      path.find("/oidc") != std::string::npos) {
    return true;
  }
  if (!query.empty() &&
      (query.find("redirect_uri=") != std::string::npos ||
       query.find("samlrequest=") != std::string::npos ||
       (query.find("client_id=") != std::string::npos &&
        query.find("response_type=") != std::string::npos))) {
    return true;
  }
  return false;
}

WebManifestParseResult ParseWebManifestJson(const std::string& json_text,
                                            const std::string& manifest_url,
                                            const std::string& page_url) {
  WebManifestParseResult res;
  if (json_text.empty()) return res;

  g_autoptr(GError) err = nullptr;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json_text.data(), static_cast<gssize>(json_text.size()),
                                  &err)) {
    return res;
  }
  JsonNode* root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return res;
  JsonObject* obj = json_node_get_object(root);

  const std::string base = !manifest_url.empty() ? manifest_url : page_url;
  res.valid = true;

  if (json_object_has_member(obj, "name")) {
    const char* s = json_object_get_string_member(obj, "name");
    if (s) res.name = s;
  }
  if (json_object_has_member(obj, "short_name")) {
    const char* s = json_object_get_string_member(obj, "short_name");
    if (s) res.short_name = s;
  }
  if (res.name.empty() && !res.short_name.empty()) res.name = res.short_name;

  if (json_object_has_member(obj, "start_url")) {
    const char* s = json_object_get_string_member(obj, "start_url");
    if (s && s[0] != '\0') {
      res.start_url = ResolveUrlAgainstBase(base, s);
    }
  }
  if (res.start_url.empty()) res.start_url = page_url;

  if (json_object_has_member(obj, "scope")) {
    const char* s = json_object_get_string_member(obj, "scope");
    if (s && s[0] != '\0') {
      std::string resolved_scope = ResolveUrlAgainstBase(base, s);
      if (!resolved_scope.empty()) res.scope = resolved_scope;
    }
  }
  if (res.scope.empty()) res.scope = DeriveOriginFromUrl(res.start_url);

  if (json_object_has_member(obj, "theme_color")) {
    const char* s = json_object_get_string_member(obj, "theme_color");
    if (s && s[0] != '\0') res.theme_color = NormalizeHexColorToken(s);
  }
  if (json_object_has_member(obj, "display")) {
    const char* s = json_object_get_string_member(obj, "display");
    if (s) res.display = s;
  }

  if (json_object_has_member(obj, "icons")) {
    JsonNode* icons_node = json_object_get_member(obj, "icons");
    if (icons_node && JSON_NODE_HOLDS_ARRAY(icons_node)) {
      JsonArray* arr = json_node_get_array(icons_node);
      guint len = json_array_get_length(arr);
      int best_score = -1;
      for (guint i = 0; i < len; ++i) {
        JsonNode* item = json_array_get_element(arr, i);
        if (!item || !JSON_NODE_HOLDS_OBJECT(item)) continue;
        JsonObject* io = json_node_get_object(item);
        const char* src = json_object_has_member(io, "src")
                              ? json_object_get_string_member(io, "src")
                              : nullptr;
        if (!src || src[0] == '\0') continue;
        std::string resolved_icon = ResolveUrlAgainstBase(base, src);
        if (resolved_icon.empty()) continue;

        const char* sizes = json_object_has_member(io, "sizes")
                                ? json_object_get_string_member(io, "sizes")
                                : nullptr;
        const char* type = json_object_has_member(io, "type")
                               ? json_object_get_string_member(io, "type")
                               : nullptr;
        const char* purpose = json_object_has_member(io, "purpose")
                                  ? json_object_get_string_member(io, "purpose")
                                  : nullptr;

        int sz = ParseMaxIconSize(sizes);
        int score = 0;
        std::string ltype = type ? ToLowerAsciiCopy(type) : "";
        std::string lsrc = ToLowerAsciiCopy(resolved_icon);
        if (ltype.find("svg") != std::string::npos || lsrc.find(".svg") != std::string::npos) {
          score += 900;
        } else if (ltype.find("png") != std::string::npos ||
                   lsrc.find(".png") != std::string::npos) {
          score += 500 + std::min(sz, 512);
        } else {
          score += 150 + std::min(sz, 256);
        }
        if (purpose && ToLowerAsciiCopy(purpose).find("any") != std::string::npos) {
          score += 40;
        } else if (purpose && ToLowerAsciiCopy(purpose) == "maskable") {
          score -= 20;
        }
        if (score > best_score) {
          best_score = score;
          res.best_icon_url = resolved_icon;
        }
      }
    }
  }

  return res;
}

std::string BuildFallbackMonogramSvg(const std::string& name, const std::string& accent_hex) {
  std::string accent = SanitizeHexColor(accent_hex, "#0078d4");
  std::string monogram;
  for (unsigned char c : name) {
    if (std::isalnum(c)) {
      monogram.push_back(static_cast<char>(std::toupper(c)));
      break;
    }
  }
  if (monogram.empty()) monogram = "W";

  std::ostringstream os;
  os << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"256\" height=\"256\" "
        "viewBox=\"0 0 256 256\">\n"
     << "  <rect x=\"0\" y=\"0\" width=\"256\" height=\"256\" fill=\"#141414\"/>\n"
     << "  <rect x=\"0\" y=\"0\" width=\"24\" height=\"256\" fill=\"" << XmlEscape(accent)
     << "\"/>\n"
     << "  <rect x=\"2\" y=\"2\" width=\"252\" height=\"252\" fill=\"none\" stroke=\""
     << XmlEscape(accent) << "\" stroke-width=\"4\"/>\n"
     << "  <text x=\"140\" y=\"164\" text-anchor=\"middle\" "
        "font-family=\"Fira Code, monospace\" font-size=\"124\" font-weight=\"700\" "
        "fill=\"#ffffff\">"
     << XmlEscape(monogram) << "</text>\n"
     << "</svg>\n";
  return os.str();
}

bool WebAppStore::Load(const std::string& json_path) {
  std::lock_guard<std::mutex> lock(mu_);
  apps_.clear();
  if (json_path.empty() || !g_file_test(json_path.c_str(), G_FILE_TEST_EXISTS)) return false;

  g_autofree gchar* contents = nullptr;
  gsize len = 0;
  if (!g_file_get_contents(json_path.c_str(), &contents, &len, nullptr) || !contents) {
    return false;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, contents, static_cast<gssize>(len), nullptr)) {
    return false;
  }
  JsonNode* root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return false;
  JsonObject* obj = json_node_get_object(root);
  if (!json_object_has_member(obj, "apps")) return true;
  JsonNode* apps_node = json_object_get_member(obj, "apps");
  if (!apps_node || !JSON_NODE_HOLDS_ARRAY(apps_node)) return true;

  JsonArray* arr = json_node_get_array(apps_node);
  guint count = json_array_get_length(arr);
  for (guint i = 0; i < count; ++i) {
    JsonNode* item = json_array_get_element(arr, i);
    if (!item || !JSON_NODE_HOLDS_OBJECT(item)) continue;
    JsonObject* ao = json_node_get_object(item);
    WebAppEntry e;
    auto get_str = [&](const char* k) -> std::string {
      if (!json_object_has_member(ao, k)) return "";
      const char* v = json_object_get_string_member(ao, k);
      return v ? std::string(v) : "";
    };
    e.id = SanitizeWebAppId(get_str("id"));
    e.name = get_str("name");
    e.start_url = get_str("start_url");
    e.scope = get_str("scope");
    e.theme_color = SanitizeHexColor(get_str("theme_color"), "#0078d4");
    e.icon_url = get_str("icon_url");
    e.icon_path = get_str("icon_path");
    e.profile_id = get_str("profile_id");
    if (e.profile_id.empty()) e.profile_id = "default";
    e.desktop_id = get_str("desktop_id");
    if (e.desktop_id.empty()) e.desktop_id = WebAppDesktopId(e.profile_id, e.id);
    e.wayland_app_id = get_str("wayland_app_id");
    if (e.wayland_app_id.empty()) e.wayland_app_id = WebAppWaylandAppId(e.profile_id, e.id);
    if (json_object_has_member(ao, "created_epoch")) {
      e.created_epoch = json_object_get_int_member(ao, "created_epoch");
    }
    if (json_object_has_member(ao, "last_launched_epoch")) {
      e.last_launched_epoch = json_object_get_int_member(ao, "last_launched_epoch");
    }
    if (!e.start_url.empty()) {
      if (e.scope.empty()) e.scope = DeriveOriginFromUrl(e.start_url);
      if (e.name.empty()) e.name = e.id;
      apps_.push_back(std::move(e));
    }
  }
  return true;
}

bool WebAppStore::Save(const std::string& json_path) const {
  if (json_path.empty()) return false;
  std::lock_guard<std::mutex> lock(mu_);

  g_autofree gchar* dir = g_path_get_dirname(json_path.c_str());
  if (dir && g_mkdir_with_parents(dir, 0700) != 0) return false;

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "apps");
  json_builder_begin_array(b);
  for (const auto& e : apps_) {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_string_value(b, e.id.c_str());
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, e.name.c_str());
    json_builder_set_member_name(b, "start_url");
    json_builder_add_string_value(b, e.start_url.c_str());
    json_builder_set_member_name(b, "scope");
    json_builder_add_string_value(b, e.scope.c_str());
    json_builder_set_member_name(b, "theme_color");
    json_builder_add_string_value(b, e.theme_color.c_str());
    json_builder_set_member_name(b, "icon_url");
    json_builder_add_string_value(b, e.icon_url.c_str());
    json_builder_set_member_name(b, "icon_path");
    json_builder_add_string_value(b, e.icon_path.c_str());
    json_builder_set_member_name(b, "desktop_id");
    json_builder_add_string_value(b, e.desktop_id.c_str());
    json_builder_set_member_name(b, "wayland_app_id");
    json_builder_add_string_value(b, e.wayland_app_id.c_str());
    json_builder_set_member_name(b, "profile_id");
    json_builder_add_string_value(b, e.profile_id.c_str());
    json_builder_set_member_name(b, "created_epoch");
    json_builder_add_int_value(b, e.created_epoch);
    json_builder_set_member_name(b, "last_launched_epoch");
    json_builder_add_int_value(b, e.last_launched_epoch);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  g_autofree gchar* text = json_generator_to_data(gen, nullptr);
  if (!text) return false;
  return g_file_set_contents(json_path.c_str(), text, -1, nullptr) == TRUE;
}

void WebAppStore::Upsert(WebAppEntry entry) {
  std::lock_guard<std::mutex> lock(mu_);
  entry.id = SanitizeWebAppId(entry.id.empty() ? entry.name : entry.id);
  if (entry.profile_id.empty()) entry.profile_id = "default";
  if (entry.scope.empty()) entry.scope = DeriveOriginFromUrl(entry.start_url);
  entry.theme_color = SanitizeHexColor(entry.theme_color, "#0078d4");
  entry.desktop_id = WebAppDesktopId(entry.profile_id, entry.id);
  entry.wayland_app_id = WebAppWaylandAppId(entry.profile_id, entry.id);
  if (entry.created_epoch <= 0) entry.created_epoch = static_cast<int64_t>(std::time(nullptr));

  for (auto& existing : apps_) {
    if (existing.id == entry.id) {
      existing = std::move(entry);
      return;
    }
  }
  apps_.push_back(std::move(entry));
}

bool WebAppStore::Remove(const std::string& id) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::string safe = SanitizeWebAppId(id);
  for (auto it = apps_.begin(); it != apps_.end(); ++it) {
    if (it->id == safe || it->id == id) {
      apps_.erase(it);
      return true;
    }
  }
  return false;
}

bool WebAppStore::TouchLaunched(const std::string& id) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::string safe = SanitizeWebAppId(id);
  for (auto& e : apps_) {
    if (e.id == safe || e.id == id) {
      e.last_launched_epoch = static_cast<int64_t>(std::time(nullptr));
      return true;
    }
  }
  return false;
}

std::optional<WebAppEntry> WebAppStore::FindById(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const std::string safe = SanitizeWebAppId(id);
  for (const auto& e : apps_) {
    if (e.id == id || e.id == safe) return e;
  }
  return std::nullopt;
}

std::optional<WebAppEntry> WebAppStore::FindBestMatchForUrl(const std::string& url) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::optional<WebAppEntry> best;
  size_t best_len = 0;
  for (const auto& e : apps_) {
    if (IsUrlInWebAppScope(e, url)) {
      if (!best.has_value() || e.scope.size() >= best_len) {
        best = e;
        best_len = e.scope.size();
      }
    }
  }
  return best;
}

std::vector<WebAppEntry> WebAppStore::List() const {
  std::lock_guard<std::mutex> lock(mu_);
  return apps_;
}

std::string WebAppStore::MakeUniqueId(const std::string& preferred) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::string base = SanitizeWebAppId(preferred);
  auto exists = [&](const std::string& candidate) {
    for (const auto& e : apps_) {
      if (e.id == candidate) return true;
    }
    return false;
  };
  if (!exists(base)) return base;
  for (int i = 2; i < 1000; ++i) {
    std::string suffix = "-" + std::to_string(i);
    std::string cand = base.substr(0, 48 - std::min<size_t>(base.size(), suffix.size())) + suffix;
    if (!exists(cand)) return cand;
  }
  return base;
}

size_t WebAppStore::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return apps_.size();
}

bool InstallWebAppDesktopFilesWithDirs(const WebAppEntry& app,
                                       const std::string& exe_path,
                                       const std::vector<uint8_t>& icon_bytes,
                                       const std::string& icon_content_type,
                                       const std::string& xdg_data_home,
                                       std::string* out_icon_path,
                                       std::string* out_desktop_path) {
  const std::string data_home = EffectiveXdgDataHome(xdg_data_home);
  const std::string wayland_id =
      app.wayland_app_id.empty() ? WebAppWaylandAppId(app.profile_id, app.id) : app.wayland_app_id;
  const std::string desktop_id =
      app.desktop_id.empty() ? WebAppDesktopId(app.profile_id, app.id) : app.desktop_id;

  bool is_png = LooksLikePng(icon_bytes);
  bool is_svg = !is_png && (LooksLikeSvg(icon_bytes) ||
                            ToLowerAsciiCopy(icon_content_type).find("svg") != std::string::npos);

  std::string icon_rel_dir = is_png ? "icons/hicolor/256x256/apps" : "icons/hicolor/scalable/apps";
  std::string icon_ext = is_png ? ".png" : ".svg";

  g_autofree gchar* icon_dir = g_build_filename(data_home.c_str(), icon_rel_dir.c_str(), nullptr);
  if (g_mkdir_with_parents(icon_dir, 0755) != 0) return false;
  g_autofree gchar* icon_path =
      g_build_filename(icon_dir, (wayland_id + icon_ext).c_str(), nullptr);

  if (is_png) {
    if (!g_file_set_contents(icon_path, reinterpret_cast<const char*>(icon_bytes.data()),
                             static_cast<gssize>(icon_bytes.size()), nullptr)) {
      return false;
    }
  } else if (is_svg && !icon_bytes.empty()) {
    if (!g_file_set_contents(icon_path, reinterpret_cast<const char*>(icon_bytes.data()),
                             static_cast<gssize>(icon_bytes.size()), nullptr)) {
      return false;
    }
  } else {
    std::string svg = BuildFallbackMonogramSvg(app.name, app.theme_color);
    if (!g_file_set_contents(icon_path, svg.data(), static_cast<gssize>(svg.size()), nullptr)) {
      return false;
    }
  }

  g_autofree gchar* apps_dir = g_build_filename(data_home.c_str(), "applications", nullptr);
  if (g_mkdir_with_parents(apps_dir, 0755) != 0) return false;
  g_autofree gchar* desktop_path = g_build_filename(apps_dir, desktop_id.c_str(), nullptr);

  std::string exec_bin = !exe_path.empty() ? exe_path : "kajo-web";
  std::string exec_line = DesktopQuoteArg(exec_bin) + " --profile " +
                          DesktopQuoteArg(app.profile_id.empty() ? "default" : app.profile_id) +
                          " --app-id " + DesktopQuoteArg(app.id);

  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "Type", "Application");
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "Version", "1.5");
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "Name",
                        app.name.empty() ? app.id.c_str() : app.name.c_str());
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "Comment", app.start_url.c_str());
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "Exec", exec_line.c_str());
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "Icon", icon_path);
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "StartupWMClass", wayland_id.c_str());
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "Categories", "Network;WebBrowser;");
  g_key_file_set_boolean(kf, G_KEY_FILE_DESKTOP_GROUP, "Terminal", FALSE);
  g_key_file_set_boolean(kf, G_KEY_FILE_DESKTOP_GROUP, "StartupNotify", TRUE);
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "X-Kajo-WebApp-Id", app.id.c_str());
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "X-Kajo-WebApp-Profile",
                        app.profile_id.empty() ? "default" : app.profile_id.c_str());
  g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, "X-Kajo-WebApp-Url", app.start_url.c_str());

  gsize kf_len = 0;
  g_autofree gchar* kf_data = g_key_file_to_data(kf, &kf_len, nullptr);
  if (!kf_data || !g_file_set_contents(desktop_path, kf_data, static_cast<gssize>(kf_len), nullptr)) {
    return false;
  }

  if (out_icon_path) *out_icon_path = icon_path;
  if (out_desktop_path) *out_desktop_path = desktop_path;
  return true;
}

bool InstallWebAppDesktopFiles(const WebAppEntry& app,
                               const std::string& exe_path,
                               const std::vector<uint8_t>& icon_bytes,
                               const std::string& icon_content_type,
                               std::string* out_icon_path,
                               std::string* out_desktop_path) {
  return InstallWebAppDesktopFilesWithDirs(app, exe_path, icon_bytes, icon_content_type, "",
                                           out_icon_path, out_desktop_path);
}

void RemoveWebAppDesktopFilesWithDirs(const WebAppEntry& app, const std::string& xdg_data_home) {
  const std::string data_home = EffectiveXdgDataHome(xdg_data_home);
  const std::string wayland_id =
      app.wayland_app_id.empty() ? WebAppWaylandAppId(app.profile_id, app.id) : app.wayland_app_id;
  const std::string desktop_id =
      app.desktop_id.empty() ? WebAppDesktopId(app.profile_id, app.id) : app.desktop_id;

  g_autofree gchar* desktop_path =
      g_build_filename(data_home.c_str(), "applications", desktop_id.c_str(), nullptr);
  g_unlink(desktop_path);

  if (!app.icon_path.empty()) g_unlink(app.icon_path.c_str());

  g_autofree gchar* png_path = g_build_filename(data_home.c_str(), "icons/hicolor/256x256/apps",
                                                (wayland_id + ".png").c_str(), nullptr);
  g_unlink(png_path);
  g_autofree gchar* svg_path = g_build_filename(data_home.c_str(), "icons/hicolor/scalable/apps",
                                                (wayland_id + ".svg").c_str(), nullptr);
  g_unlink(svg_path);
}

void RemoveWebAppDesktopFiles(const WebAppEntry& app) {
  RemoveWebAppDesktopFilesWithDirs(app, "");
}

}  // namespace kajo_web
