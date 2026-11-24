#include "web_cef/cef_chrome_ui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#include "include/wrapper/cef_helpers.h"
#include "web_cef/cef_site_util.h"

namespace kajo_web {

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

std::string HtmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

static std::string UrlEncodeQuery(const std::string& v) {
  std::string out;
  out.reserve(v.size() + 8);
  for (unsigned char c : v) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

static std::string FormatBytes(int64_t bytes) {
  if (bytes <= 0) return "0 B";
  char buf[64];
  if (bytes >= 1024LL * 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
  } else if (bytes >= 1024LL * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024LL) {
    snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
  }
  return buf;
}

static void ExtractPathAndQuery(const std::string& url,
                                std::string* out_host_or_path,
                                std::string* out_query) {
  out_host_or_path->clear();
  out_query->clear();
  size_t sep = url.find("://");
  std::string rest = (sep != std::string::npos) ? url.substr(sep + 3) : url;
  size_t q = rest.find('?');
  if (q != std::string::npos) {
    *out_query = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }
  size_t hash = rest.find('#');
  if (hash != std::string::npos) {
    rest = rest.substr(0, hash);
  }
  while (!rest.empty() && rest.back() == '/') {
    rest.pop_back();
  }
  *out_host_or_path = rest;
}

void ChromeStateStore::Update(const ChromeStateSnapshot& snapshot) {
  std::lock_guard<std::mutex> lock(mu_);
  state_ = snapshot;
}

ChromeStateSnapshot ChromeStateStore::Snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_;
}

std::string ChromeStateStore::RenderThemeCss() const {
  ChromeStateSnapshot s = Snapshot();
  std::string accent = s.private_mode ? "#b146c2" :
                       (!s.profile_accent.empty() ? s.profile_accent : s.accent_color);
  accent = SanitizeHexColor(accent, "#0078d4");
  const std::string theme_accent = SanitizeHexColor(s.accent_color, "#0078d4");
  std::string font = s.ui_font_family.empty() ? "Fira Code" : s.ui_font_family;

  std::ostringstream css;
  css << ":root {\n"
      << "  --metro-accent: " << accent << ";\n"
      << "  --metro-theme-accent: " << theme_accent << ";\n"
      << "  --metro-surface-0: #111111;\n"
      << "  --metro-surface-1: #141414;\n"
      << "  --metro-surface-2: #181818;\n"
      << "  --metro-surface-3: #1a1a1a;\n"
      << "  --metro-surface-4: #222222;\n"
      << "  --metro-border: #2b2b2b;\n"
      << "  --metro-border-strong: #3c3c3c;\n"
      << "  --metro-fg: #ececec;\n"
      << "  --metro-fg-dim: #9a9a9a;\n"
      << "  --metro-success: #2ea043;\n"
      << "  --metro-warning: #d97706;\n"
      << "  --metro-urgent: #e81123;\n"
      << "  --metro-font: \"" << JsonEscape(font) << "\", \"Fira Code\", monospace;\n"
      << "  --metro-font-size: " << s.ui_font_size_px << "px;\n"
      << "  --metro-heading-size: " << s.heading_size_px << "px;\n"
      << "  --metro-sub-size: " << s.sub_size_px << "px;\n"
      << "  --metro-badge-size: " << s.badge_size_px << "px;\n"
      << "  --metro-topbar-h: " << s.topbar_height_px << "px;\n"
      << "  --metro-ctrl-h: " << s.control_height_px << "px;\n"
      << "  --metro-icon-size: " << s.icon_size_px << "px;\n"
      << "}\n"
      << "* { box-sizing: border-box; border-radius: 0 !important; }\n"
      << "html, body {\n"
      << "  margin: 0; padding: 0;\n"
      << "  background: var(--metro-surface-1);\n"
      << "  color: var(--metro-fg);\n"
      << "  font-family: var(--metro-font);\n"
      << "  font-size: var(--metro-font-size);\n"
      << "  font-variant-numeric: tabular-nums;\n"
      << "  user-select: none;\n"
      << "}\n"
      << "button, input, select {\n"
      << "  font-family: inherit;\n"
      << "  font-size: inherit;\n"
      << "  color: var(--metro-fg);\n"
      << "  border-radius: 0;\n"
      << "  outline: none;\n"
      << "}\n"
      << "::-webkit-scrollbar { width: 6px; height: 6px; }\n"
      << "::-webkit-scrollbar-track { background: var(--metro-surface-0); }\n"
      << "::-webkit-scrollbar-thumb { background: #333333; }\n"
      << "::-webkit-scrollbar-thumb:hover { background: var(--metro-accent); }\n";
  return css.str();
}

std::string ChromeStateStore::SnapshotJson() const {
  ChromeStateSnapshot s = Snapshot();
  std::ostringstream o;
  o << "{";
  o << "\"private_mode\":" << (s.private_mode ? "true" : "false") << ",";
  o << "\"is_maximized\":" << (s.is_maximized ? "true" : "false") << ",";
  o << "\"rail_mode\":" << s.rail_mode << ",";
  o << "\"rail_side\":" << s.rail_side << ",";
  o << "\"rail_hover_open\":" << (s.rail_hover_open ? "true" : "false") << ",";
  o << "\"rail_width_px\":" << s.rail_width_px << ",";
  o << "\"topbar_mode\":" << s.topbar_mode << ",";
  o << "\"topbar_hover_open\":" << (s.topbar_hover_open ? "true" : "false") << ",";
  o << "\"profile_border_accent\":" << (s.profile_border_accent ? "true" : "false") << ",";
  o << "\"show_tab_domains\":" << (s.show_tab_domains ? "true" : "false") << ",";
  o << "\"ac_visible\":" << (s.ac_visible ? "true" : "false") << ",";
  o << "\"ac_selected_idx\":" << s.ac_selected_idx << ",";
  o << "\"closed_tab_count\":" << s.closed_tab_count << ",";
  o << "\"overlay_mode\":\"" << JsonEscape(s.overlay_mode) << "\",";
  o << "\"accent_color\":\"" << JsonEscape(SanitizeHexColor(s.accent_color, "#0078d4")) << "\",";
  o << "\"profile_accent\":\"" << JsonEscape(SanitizeHexColor(s.profile_accent, "#0078d4")) << "\",";
  o << "\"profile_id\":\"" << JsonEscape(s.profile_id) << "\",";
  o << "\"profile_name\":\"" << JsonEscape(s.profile_name) << "\",";
  o << "\"profile_icon\":\"" << JsonEscape(s.profile_icon) << "\",";
  o << "\"ui_font_family\":\"" << JsonEscape(s.ui_font_family) << "\",";
  o << "\"ui_font_size_px\":" << s.ui_font_size_px << ",";
  o << "\"heading_size_px\":" << s.heading_size_px << ",";
  o << "\"sub_size_px\":" << s.sub_size_px << ",";
  o << "\"badge_size_px\":" << s.badge_size_px << ",";
  o << "\"topbar_height_px\":" << s.topbar_height_px << ",";
  o << "\"control_height_px\":" << s.control_height_px << ",";
  o << "\"icon_size_px\":" << s.icon_size_px << ",";
  o << "\"active_tab_id\":" << s.active_tab_id << ",";
  o << "\"active_url\":\"" << JsonEscape(s.active_url) << "\",";
  o << "\"active_title\":\"" << JsonEscape(s.active_title) << "\",";
  o << "\"active_host\":\"" << JsonEscape(s.active_host) << "\",";
  o << "\"can_go_back\":" << (s.can_go_back ? "true" : "false") << ",";
  o << "\"can_go_forward\":" << (s.can_go_forward ? "true" : "false") << ",";
  o << "\"is_loading\":" << (s.is_loading ? "true" : "false") << ",";
  o << "\"is_bookmarked\":" << (s.is_bookmarked ? "true" : "false") << ",";
  o << "\"zoom_pct\":" << s.zoom_pct << ",";
  o << "\"default_zoom_pct\":" << s.default_zoom_pct << ",";
  o << "\"adblock_enabled\":" << (s.adblock_enabled ? "true" : "false") << ",";
  o << "\"host_paused\":" << (s.host_paused ? "true" : "false") << ",";
  o << "\"tab_blocked\":" << s.tab_blocked << ",";
  o << "\"total_blocked\":" << s.total_blocked << ",";
  o << "\"baseline_rules\":" << s.baseline_rules << ",";
  o << "\"adblock_rules\":" << s.adblock_rules << ",";
  o << "\"paused_hosts\":" << s.paused_hosts << ",";
  o << "\"filter_update_busy\":" << (s.filter_update_busy ? "true" : "false") << ",";
  o << "\"filter_update_status\":\"" << JsonEscape(s.filter_update_status) << "\",";
  o << "\"hw_accel_enabled\":" << (s.hw_accel_enabled ? "true" : "false") << ",";
  o << "\"ffmpeg_available\":" << (s.ffmpeg_available ? "true" : "false") << ",";
  o << "\"yt_dlp_available\":" << (s.yt_dlp_available ? "true" : "false") << ",";
  o << "\"find_query\":\"" << JsonEscape(s.find_query) << "\",";
  o << "\"find_active\":" << s.find_active << ",";
  o << "\"find_count\":" << s.find_count << ",";
  o << "\"find_match_case\":" << (s.find_match_case ? "true" : "false") << ",";

  o << "\"tabs\":[";
  for (size_t i = 0; i < s.tabs.size(); ++i) {
    const auto& t = s.tabs[i];
    if (i) o << ",";
    o << "{\"id\":" << t.id
      << ",\"title\":\"" << JsonEscape(t.title) << "\""
      << ",\"url\":\"" << JsonEscape(t.url) << "\""
      << ",\"domain\":\"" << JsonEscape(t.domain) << "\""
      << ",\"group\":\"" << JsonEscape(t.group) << "\""
      << ",\"favicon_url\":\"" << JsonEscape(t.favicon_url) << "\""
      << ",\"pinned\":" << (t.pinned ? "true" : "false")
      << ",\"active\":" << (t.active ? "true" : "false")
      << ",\"loading\":" << (t.loading ? "true" : "false")
      << ",\"audible\":" << (t.audible ? "true" : "false")
      << ",\"muted\":" << (t.muted ? "true" : "false")
      << ",\"suspended\":" << (t.suspended ? "true" : "false")
      << ",\"blocked\":" << t.blocked_count << "}";
  }
  o << "],";

  o << "\"collapsed_groups\":[";
  for (size_t i = 0; i < s.collapsed_groups.size(); ++i) {
    if (i) o << ",";
    o << "\"" << JsonEscape(s.collapsed_groups[i]) << "\"";
  }
  o << "],";

  o << "\"downloads_unseen_complete\":" << (s.downloads_unseen_complete ? "true" : "false") << ",";
  o << "\"download_dir\":\"" << JsonEscape(s.download_dir) << "\",";
  o << "\"downloads_filter\":\"" << JsonEscape(s.downloads_filter) << "\",";
  o << "\"downloads_search_query\":\"" << JsonEscape(s.downloads_search_query) << "\",";
  o << "\"downloads\":[";
  for (size_t i = 0; i < s.downloads.size(); ++i) {
    const auto& d = s.downloads[i];
    if (i) o << ",";
    o << "{\"id\":" << d.id
      << ",\"db_id\":" << d.db_id
      << ",\"filename\":\"" << JsonEscape(d.filename) << "\""
      << ",\"url\":\"" << JsonEscape(d.url) << "\""
      << ",\"full_path\":\"" << JsonEscape(d.full_path) << "\""
      << ",\"percent\":" << d.percent
      << ",\"received\":" << d.received_bytes
      << ",\"total\":" << d.total_bytes
      << ",\"speed\":" << d.current_speed
      << ",\"updated_at_ms\":" << d.updated_at_ms
      << ",\"in_progress\":" << (d.in_progress ? "true" : "false")
      << ",\"complete\":" << (d.complete ? "true" : "false")
      << ",\"canceled\":" << (d.canceled ? "true" : "false")
      << ",\"paused\":" << (d.paused ? "true" : "false") << "}";
  }
  o << "],";

  o << "\"media_streams\":[";
  for (size_t i = 0; i < s.media_streams.size(); ++i) {
    const auto& m = s.media_streams[i];
    if (i) o << ",";
    o << "{\"kind\":\"" << JsonEscape(m.kind) << "\""
      << ",\"label\":\"" << JsonEscape(m.label) << "\""
      << ",\"uri\":\"" << JsonEscape(m.uri) << "\""
      << ",\"mime_type\":\"" << JsonEscape(m.mime_type) << "\""
      << ",\"size_bytes\":" << m.size_bytes << "}";
  }
  o << "],";

  o << "\"profiles\":[";
  for (size_t i = 0; i < s.profiles.size(); ++i) {
    const auto& p = s.profiles[i];
    if (i) o << ",";
    o << "{\"id\":\"" << JsonEscape(p.id) << "\""
      << ",\"name\":\"" << JsonEscape(p.name) << "\""
      << ",\"icon\":\"" << JsonEscape(p.icon) << "\""
      << ",\"accent_color\":\"" << JsonEscape(p.accent_color) << "\""
      << ",\"active\":" << (p.active ? "true" : "false") << "}";
  }
  o << "],";

  o << "\"autocomplete\":[";
  for (size_t i = 0; i < s.autocomplete.size(); ++i) {
    const auto& a = s.autocomplete[i];
    if (i) o << ",";
    o << "{\"kind\":\"" << JsonEscape(a.kind) << "\""
      << ",\"label\":\"" << JsonEscape(a.label) << "\""
      << ",\"sublabel\":\"" << JsonEscape(a.sublabel) << "\""
      << ",\"action\":\"" << JsonEscape(a.action) << "\""
      << ",\"payload\":\"" << JsonEscape(a.payload) << "\""
      << ",\"tab_id\":" << a.tab_id << "}";
  }
  o << "],";

  o << "\"palette_results\":[";
  for (size_t i = 0; i < s.palette_results.size(); ++i) {
    const auto& p = s.palette_results[i];
    if (i) o << ",";
    o << "{\"kind\":\"" << JsonEscape(p.kind) << "\""
      << ",\"label\":\"" << JsonEscape(p.label) << "\""
      << ",\"sublabel\":\"" << JsonEscape(p.sublabel) << "\""
      << ",\"action\":\"" << JsonEscape(p.action) << "\""
      << ",\"payload\":\"" << JsonEscape(p.payload) << "\""
      << ",\"tab_id\":" << p.tab_id << "}";
  }
  o << "],";

  o << "\"filter_lists\":[";
  for (size_t i = 0; i < s.filter_lists.size(); ++i) {
    const auto& f = s.filter_lists[i];
    if (i) o << ",";
    o << "{\"id\":\"" << JsonEscape(f.id) << "\""
      << ",\"name\":\"" << JsonEscape(f.name) << "\""
      << ",\"enabled\":" << (f.enabled ? "true" : "false")
      << ",\"rule_count\":" << f.rule_count << "}";
  }
  o << "],";

  o << "\"active_bookmark\":{"
    << "\"uuid\":\"" << JsonEscape(s.active_bookmark.uuid) << "\""
    << ",\"title\":\"" << JsonEscape(s.active_bookmark.title) << "\""
    << ",\"url\":\"" << JsonEscape(s.active_bookmark.url) << "\""
    << ",\"folder\":\"" << JsonEscape(s.active_bookmark.folder) << "\"},";

  o << "\"bookmark_folders\":[";
  for (size_t i = 0; i < s.bookmark_folders.size(); ++i) {
    if (i) o << ",";
    o << "\"" << JsonEscape(s.bookmark_folders[i]) << "\"";
  }
  o << "],";

  o << "\"active_permission\":{"
    << "\"req_id\":" << s.active_permission.req_id
    << ",\"origin\":\"" << JsonEscape(s.active_permission.origin) << "\""
    << ",\"label\":\"" << JsonEscape(s.active_permission.label) << "\""
    << ",\"perm_key\":\"" << JsonEscape(s.active_permission.perm_key) << "\"},";

  o << "\"saved_permissions\":[";
  for (size_t i = 0; i < s.saved_permissions.size(); ++i) {
    const auto& sp = s.saved_permissions[i];
    if (i) o << ",";
    o << "{\"origin\":\"" << JsonEscape(sp.origin) << "\""
      << ",\"perm_key\":\"" << JsonEscape(sp.perm_key) << "\""
      << ",\"label\":\"" << JsonEscape(sp.label) << "\""
      << ",\"allow\":" << (sp.allow ? "true" : "false") << "}";
  }
  o << "],";

  o << "\"install_draft\":{"
    << "\"name\":\"" << JsonEscape(s.install_draft.name) << "\""
    << ",\"start_url\":\"" << JsonEscape(s.install_draft.start_url) << "\""
    << ",\"scope\":\"" << JsonEscape(s.install_draft.scope) << "\""
    << ",\"theme_color\":\"" << JsonEscape(s.install_draft.theme_color) << "\""
    << ",\"icon_url\":\"" << JsonEscape(s.install_draft.icon_url) << "\""
    << ",\"manifest_url\":\"" << JsonEscape(s.install_draft.manifest_url) << "\""
    << ",\"already_installed\":" << (s.install_draft.already_installed ? "true" : "false")
    << ",\"existing_id\":\"" << JsonEscape(s.install_draft.existing_id) << "\"},";

  o << "\"webapps\":[";
  for (size_t i = 0; i < s.webapps.size(); ++i) {
    const auto& wa = s.webapps[i];
    if (i) o << ",";
    o << "{\"id\":\"" << JsonEscape(wa.id) << "\""
      << ",\"name\":\"" << JsonEscape(wa.name) << "\""
      << ",\"start_url\":\"" << JsonEscape(wa.start_url) << "\""
      << ",\"scope\":\"" << JsonEscape(wa.scope) << "\""
      << ",\"theme_color\":\"" << JsonEscape(wa.theme_color) << "\""
      << ",\"icon_url\":\"" << JsonEscape(wa.icon_url) << "\""
      << ",\"icon_path\":\"" << JsonEscape(wa.icon_path) << "\""
      << ",\"desktop_id\":\"" << JsonEscape(wa.desktop_id) << "\""
      << ",\"wayland_app_id\":\"" << JsonEscape(wa.wayland_app_id) << "\""
      << ",\"last_launched_epoch\":" << wa.last_launched_epoch << "}";
  }
  o << "]";

  o << "}";
  return o.str();
}

static const char* kTopbarHtml = R"KAJO(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<link rel="stylesheet" href="kajo-chrome://ui/theme.css">
<style>
  html, body {
    height: 100vh;
    overflow: hidden;
    background: transparent;
  }
  .bar-wrap {
    height: var(--metro-topbar-h);
    background: var(--metro-surface-0);
    border-bottom: 1px solid var(--metro-border);
    overflow: hidden;
  }
  .bar-wrap.collapsed {
    height: 6px;
    background: rgba(0, 0, 0, 0.01);
    border-bottom: none;
    border-top: 2px solid var(--metro-border-strong);
  }
  .bar-wrap.collapsed .bar { display: none; }
  .bar {
    height: calc(var(--metro-topbar-h) - 1px);
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 0 8px;
    -webkit-app-region: drag;
  }
  .cluster {
    display: flex;
    align-items: center;
    gap: 4px;
    -webkit-app-region: no-drag;
    flex-shrink: 0;
  }
  .drag-spacer {
    flex: 0 1 16px;
    min-width: 8px;
    height: calc(var(--metro-topbar-h) - 1px);
    -webkit-app-region: drag;
  }
  .btn {
    height: var(--metro-ctrl-h);
    min-width: var(--metro-ctrl-h);
    padding: 0 8px;
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    color: var(--metro-fg);
    display: inline-flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    font-size: var(--metro-sub-size);
    font-weight: 600;
    line-height: 1;
  }
  .btn svg, .pill svg {
    width: var(--metro-icon-size);
    height: var(--metro-icon-size);
    display: block;
    flex-shrink: 0;
  }
  .btn:hover:not(:disabled) {
    background: var(--metro-surface-4);
    border-color: var(--metro-accent);
  }
  .btn:disabled {
    opacity: 0.35;
    cursor: default;
  }
  .btn.active {
    border-color: var(--metro-accent);
    color: #ffffff;
  }
  .btn.sw-chip {
    border-color: var(--metro-warning);
    color: #ffb347;
    font-size: var(--metro-badge-size);
    letter-spacing: 0.4px;
    padding: 0 8px;
  }
  .toast-pill {
    height: var(--metro-ctrl-h);
    padding: 0 10px;
    display: inline-flex;
    align-items: center;
    background: var(--metro-surface-3);
    border: 1px solid var(--metro-accent);
    color: #ffffff;
    font-size: var(--metro-badge-size);
    font-weight: 600;
    letter-spacing: 0.3px;
    white-space: nowrap;
    flex-shrink: 0;
    -webkit-app-region: no-drag;
  }
  .btn.close:hover {
    background: var(--metro-urgent);
    border-color: var(--metro-urgent);
    color: #ffffff;
  }
  .omnibar-wrap {
    flex: 1 1 auto;
    min-width: 180px;
    height: var(--metro-ctrl-h);
    display: flex;
    align-items: center;
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border-strong);
    -webkit-app-region: no-drag;
  }
  .omnibar-wrap.focused {
    border-color: var(--metro-accent);
    background: #101010;
  }
  .sec-badge {
    padding: 0 8px;
    height: calc(var(--metro-ctrl-h) - 2px);
    display: inline-flex;
    align-items: center;
    font-size: var(--metro-badge-size);
    font-weight: 600;
    color: var(--metro-fg-dim);
    border-right: 1px solid var(--metro-border);
    flex-shrink: 0;
  }
  .sec-badge.tls { color: var(--metro-success); }
  .sec-badge.warn { color: var(--metro-warning); }
  .sec-badge.kajo { color: var(--metro-accent); }
  .omni-field {
    flex: 1 1 auto;
    min-width: 0;
    height: calc(var(--metro-ctrl-h) - 2px);
    position: relative;
    display: flex;
    align-items: center;
  }
  .omni-display {
    position: absolute;
    inset: 0;
    z-index: 1;
    pointer-events: none;
    display: flex;
    align-items: center;
    padding: 0 10px;
    font-size: var(--metro-font-size);
    white-space: nowrap;
    overflow: hidden;
  }
  .omni-host {
    color: #ffffff;
    font-weight: 600;
    flex-shrink: 0;
  }
  .omni-host.kajo-scheme { color: var(--metro-accent); }
  .omni-path {
    color: var(--metro-fg-dim);
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .omni-ph { color: var(--metro-fg-dim); }
  .omnibar-wrap.focused #omniDisplay { display: none; }
  #omnibar {
    width: 100%;
    height: calc(var(--metro-ctrl-h) - 2px);
    position: relative;
    z-index: 2;
    background: transparent;
    border: none;
    padding: 0 10px;
    font-size: var(--metro-font-size);
    color: var(--metro-fg);
  }
  .omnibar-wrap:not(.focused) #omnibar {
    color: transparent;
    caret-color: transparent;
  }
  .omnibar-wrap:not(.focused) #omnibar::placeholder { color: transparent; }
  .pill {
    height: calc(var(--metro-ctrl-h) - 2px);
    padding: 0 8px;
    background: transparent;
    border: none;
    border-left: 1px solid var(--metro-border);
    color: var(--metro-fg-dim);
    display: inline-flex;
    align-items: center;
    gap: 5px;
    cursor: pointer;
    font-size: var(--metro-badge-size);
    font-weight: 600;
    flex-shrink: 0;
  }
  .pill:hover { background: var(--metro-surface-4); color: #ffffff; }
  .pill.shield-on { color: #ff8c38; }
  .pill.shield-paused { color: var(--metro-fg-dim); }
  .pill.star-on { color: #f5b942; }
  .hidden { display: none !important; }
  .dl-btn {
    position: relative;
    gap: 5px;
    padding: 0 8px;
    overflow: hidden;
  }
  .dl-btn.dl-busy {
    border-color: var(--metro-accent);
    color: #ffffff;
  }
  .dl-dot {
    width: 6px;
    height: 6px;
    background: var(--metro-success);
    display: none;
  }
  .dl-btn.dl-done .dl-dot { display: inline-block; }
  .dl-prog {
    position: absolute;
    left: 0; right: 0; bottom: 0;
    height: 2px;
    background: var(--metro-border);
    display: none;
    pointer-events: none;
  }
  .dl-btn.dl-busy .dl-prog { display: block; }
  .dl-prog > span {
    display: block;
    height: 100%;
    background: var(--metro-accent);
    transition: width 120ms linear;
  }
  .profile-btn {
    gap: 6px;
    padding: 0 10px;
    font-size: var(--metro-sub-size);
    letter-spacing: 0.4px;
  }
  .dot {
    width: 8px;
    height: 8px;
    background: var(--metro-accent);
    display: inline-block;
  }
  .load-bar {
    position: fixed;
    left: 0; top: 0;
    height: 2px;
    width: 0;
    background: var(--metro-accent);
    transition: width 140ms linear, opacity 180ms ease;
    opacity: 0;
    pointer-events: none;
    z-index: 5;
  }
  .load-bar.active { width: 72%; opacity: 1; }
</style>
</head>
<body>
<div id="loadBar" class="load-bar"></div>
<div id="barWrap" class="bar-wrap">
  <div class="bar">
    <div class="cluster">
      <button id="btnBack" class="btn" title="Back (Alt+Left)" aria-label="Back"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="square" stroke-linejoin="miter"><path d="M13 8H3.5M7.5 4L3.5 8l4 4"/></svg></button>
      <button id="btnFwd" class="btn" title="Forward (Alt+Right)" aria-label="Forward"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="square" stroke-linejoin="miter"><path d="M3 8h9.5M8.5 4l4 4-4 4"/></svg></button>
      <button id="btnReload" class="btn" title="Reload / Stop (Ctrl+R · Shift+R Hard)" aria-label="Reload"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="square" stroke-linejoin="miter"><path d="M13 8a5 5 0 1 1-1.35-3.42"/><path d="M13 2.5v3h-3"/></svg></button>
      <button id="btnRail" class="btn" title="Toggle Tab Rail (Ctrl+B)" aria-label="Toggle Tab Rail"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="square" stroke-linejoin="miter"><rect x="2" y="2.5" width="12" height="11"/><path d="M6 2.5v11"/></svg></button>
    </div>

    <div class="drag-spacer"></div>

    <div id="omniWrap" class="omnibar-wrap">
      <span id="secBadge" class="sec-badge">❖</span>
      <div class="omni-field">
        <div id="omniDisplay" class="omni-display">
          <span id="omniHost" class="omni-host"></span><span id="omniPath" class="omni-path"></span><span id="omniPh" class="omni-ph hidden">Search or enter URL (Ctrl+L · Ctrl+K Palette)</span>
        </div>
        <input id="omnibar" type="text" spellcheck="false" autocomplete="off"
               placeholder="Search or enter URL (Ctrl+L · Ctrl+K Palette)">
      </div>
      <button id="btnShield" class="pill shield-on" title="Adblock &amp; Trackers">
        <svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="square" stroke-linejoin="miter"><path d="M8 2L3 4v4c0 3.2 2.1 5.1 5 6 2.9-.9 5-2.8 5-6V4L8 2z"/></svg><span id="blockedNum">0</span>
      </button>
      <button id="btnInstallApp" class="pill hidden" title="Install Site as Web App" aria-label="Install Web App"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="square" stroke-linejoin="miter"><rect x="2" y="2.5" width="9" height="9"/><path d="M5 13.5h9v-9"/><path d="M6.5 5.2v3.6M4.7 7h3.6"/></svg></button>
      <button id="btnStar" class="pill" title="Bookmark Page (Ctrl+D)" aria-label="Bookmark"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="miter"><polygon points="8,2.2 9.8,6 14,6.6 11,9.5 11.7,13.7 8,11.7 4.3,13.7 5,9.5 2,6.6 6.2,6"/></svg></button>
    </div>

    <div class="drag-spacer"></div>

    <div class="cluster">
      <span id="toastPill" class="toast-pill hidden"></span>
      <button id="btnSwRender" class="btn sw-chip hidden" title="Hardware GPU acceleration disabled — open kajo://gpu">⚠ SW RENDER</button>
      <button id="btnMedia" class="btn hidden" title="Sniffed Media Streams"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="square" stroke-linejoin="miter"><path d="M8 2.5v8M4.5 7.5L8 11l3.5-3.5M3 13.5h10"/></svg><span id="mediaCount" style="margin-left:4px;">0</span></button>
      <button id="btnDl" class="btn dl-btn" title="Downloads (Ctrl+J)" aria-label="Downloads">
        <svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="square" stroke-linejoin="miter"><path d="M8 2.5v8M4.5 7.5L8 11l3.5-3.5M3 13.5h10"/></svg><span id="dlCount"></span><span class="dl-dot"></span>
        <span class="dl-prog"><span id="dlProgBar" style="width:0%"></span></span>
      </button>
      <button id="btnZoom" class="btn hidden" title="Reset Zoom (Ctrl+0)"><span id="zoomLabel">100%</span></button>
      <button id="btnProfile" class="btn profile-btn" title="Profiles &amp; Private Mode (Ctrl+Shift+M)">
        <span id="profileDot" class="dot"></span>
        <span id="profileLabel">DEFAULT</span>
      </button>
      <button id="btnSettings" class="btn" title="Web Settings (Ctrl+,)" aria-label="Settings"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="square"><circle cx="8" cy="8" r="2.5"/><path d="M8 1.8v1.6M8 12.6v1.6M1.8 8h1.6M12.6 8h1.6M3.6 3.6l1.1 1.1M11.3 11.3l1.1 1.1M12.4 3.6l-1.1 1.1M4.7 11.3l-1.1 1.1"/></svg></button>
      <button id="btnFloat" class="btn" title="Float / Tile Window" aria-label="Float or Tile"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="square" stroke-linejoin="miter"><rect x="2.5" y="5.5" width="8" height="8"/><path d="M5.5 5.5v-3h8v8h-3"/></svg></button>
      <button id="btnClose" class="btn close" title="Close Browser" aria-label="Close"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="square"><path d="M3.5 3.5l9 9M12.5 3.5l-9 9"/></svg></button>
    </div>
  </div>
</div>

<script>
const send = (cmd, extra = {}) => {
  if (!window.cefQuery) return Promise.resolve(null);
  return new Promise((resolve) => {
    window.cefQuery({
      request: JSON.stringify({ cmd, ...extra }),
      onSuccess: (res) => { try { resolve(JSON.parse(res)); } catch { resolve(null); } },
      onFailure: () => resolve(null)
    });
  });
};

const barWrap = document.getElementById('barWrap');
const omni = document.getElementById('omnibar');
const omniWrap = document.getElementById('omniWrap');
const omniDisplay = document.getElementById('omniDisplay');
const omniHost = document.getElementById('omniHost');
const omniPath = document.getElementById('omniPath');
const omniPh = document.getElementById('omniPh');
const btnBack = document.getElementById('btnBack');
const btnFwd = document.getElementById('btnFwd');
const btnReload = document.getElementById('btnReload');
const btnRail = document.getElementById('btnRail');
const secBadge = document.getElementById('secBadge');
const btnShield = document.getElementById('btnShield');
const blockedNum = document.getElementById('blockedNum');
const btnInstallApp = document.getElementById('btnInstallApp');
const btnStar = document.getElementById('btnStar');
const btnSwRender = document.getElementById('btnSwRender');
const toastPill = document.getElementById('toastPill');
const btnMedia = document.getElementById('btnMedia');
const mediaCount = document.getElementById('mediaCount');
const btnDl = document.getElementById('btnDl');
const dlCount = document.getElementById('dlCount');
const dlProgBar = document.getElementById('dlProgBar');
const btnZoom = document.getElementById('btnZoom');
const zoomLabel = document.getElementById('zoomLabel');
const btnProfile = document.getElementById('btnProfile');
const profileDot = document.getElementById('profileDot');
const profileLabel = document.getElementById('profileLabel');
const loadBar = document.getElementById('loadBar');

let lastState = null;
let acSelectedIdx = -1;
let toastTimer = null;

window.__kajoToast = (msg) => {
  if (!toastPill) return;
  if (!msg) {
    toastPill.classList.add('hidden');
    return;
  }
  toastPill.textContent = msg;
  toastPill.classList.remove('hidden');
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    toastPill.classList.add('hidden');
    toastTimer = null;
  }, 1800);
};

function updateOmniDisplay(url) {
  omniHost.className = 'omni-host';
  omniHost.textContent = '';
  omniPath.textContent = '';
  omniPh.classList.add('hidden');
  if (!url || url === 'about:blank') {
    omniPh.classList.remove('hidden');
    return;
  }
  if (url.startsWith('view-source:')) {
    omniHost.classList.add('kajo-scheme');
    omniHost.textContent = 'view-source:';
    omniPath.textContent = url.slice('view-source:'.length);
    return;
  }
  if (url.startsWith('kajo://') || url.startsWith('chrome://')) {
    omniHost.classList.add('kajo-scheme');
    const idx = url.indexOf('/', url.indexOf('://') + 3);
    if (idx >= 0) {
      omniHost.textContent = url.slice(0, idx);
      omniPath.textContent = url.slice(idx);
    } else {
      omniHost.textContent = url;
    }
    return;
  }
  try {
    const u = new URL(url);
    if (u.host) {
      omniHost.textContent = u.host;
      const tail = (u.pathname === '/' && !u.search && !u.hash) ? '' : (u.pathname + u.search + u.hash);
      omniPath.textContent = tail;
      return;
    }
  } catch (_) {}
  omniHost.textContent = url;
}

window.__kajoFocusOmnibar = () => {
  omniWrap.classList.add('focused');
  omni.focus();
  omni.select();
};

barWrap.addEventListener('mouseenter', () => {
  if (lastState && lastState.topbar_mode === 1) send('topbar_hover', { inside: true });
});
barWrap.addEventListener('mousemove', () => {
  if (lastState && lastState.topbar_mode === 1 && !lastState.topbar_hover_open) {
    send('topbar_hover', { inside: true });
  }
});
barWrap.addEventListener('mouseleave', () => {
  if (lastState && lastState.topbar_mode === 1) send('topbar_hover', { inside: false });
});

const SVG_RELOAD = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="square" stroke-linejoin="miter"><path d="M13 8a5 5 0 1 1-1.35-3.42"/><path d="M13 2.5v3h-3"/></svg>';
const SVG_STOP = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="square"><path d="M3.5 3.5l9 9M12.5 3.5l-9 9"/></svg>';
const SVG_STAR_OUTLINE = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="miter"><polygon points="8,2.2 9.8,6 14,6.6 11,9.5 11.7,13.7 8,11.7 4.3,13.7 5,9.5 2,6.6 6.2,6"/></svg>';
const SVG_STAR_FILLED = '<svg viewBox="0 0 16 16" fill="currentColor" stroke="currentColor" stroke-width="1.2" stroke-linejoin="miter"><polygon points="8,2.2 9.8,6 14,6.6 11,9.5 11.7,13.7 8,11.7 4.3,13.7 5,9.5 2,6.6 6.2,6"/></svg>';
let lastIsLoading = null;
let lastIsBookmarked = null;

window.__kajoApply = (s) => {
  lastState = s;
  const ds = document.documentElement.style;
  const acc = s.private_mode ? '#b146c2' : (s.profile_accent || s.accent_color || '#0078d4');
  ds.setProperty('--metro-accent', acc);
  if (s.accent_color) ds.setProperty('--metro-theme-accent', s.accent_color);
  if (s.ui_font_family) ds.setProperty('--metro-font', '"' + s.ui_font_family + '", "Fira Code", monospace');
  if (s.ui_font_size_px) ds.setProperty('--metro-font-size', s.ui_font_size_px + 'px');
  if (s.heading_size_px) ds.setProperty('--metro-heading-size', s.heading_size_px + 'px');
  if (s.sub_size_px) ds.setProperty('--metro-sub-size', s.sub_size_px + 'px');
  if (s.badge_size_px) ds.setProperty('--metro-badge-size', s.badge_size_px + 'px');
  if (s.topbar_height_px) ds.setProperty('--metro-topbar-h', s.topbar_height_px + 'px');
  if (s.control_height_px) ds.setProperty('--metro-ctrl-h', s.control_height_px + 'px');
  if (s.icon_size_px) ds.setProperty('--metro-icon-size', s.icon_size_px + 'px');
  acSelectedIdx = typeof s.ac_selected_idx === 'number' ? s.ac_selected_idx : -1;
  const isCollapsed = (s.topbar_mode === 1 && !s.topbar_hover_open);
  barWrap.className = 'bar-wrap' + (isCollapsed ? ' collapsed' : '');

  btnBack.disabled = !s.can_go_back;
  btnFwd.disabled = !s.can_go_forward;
  if (lastIsLoading !== !!s.is_loading) {
    lastIsLoading = !!s.is_loading;
    btnReload.innerHTML = lastIsLoading ? SVG_STOP : SVG_RELOAD;
  }
  loadBar.classList.toggle('active', !!s.is_loading);
  btnRail.classList.toggle('active', s.rail_mode === 2);

  if (document.activeElement !== omni) {
    omniWrap.classList.remove('focused');
    omni.value = s.active_url || '';
    updateOmniDisplay(s.active_url || '');
  }

  const url = s.active_url || '';
  secBadge.className = 'sec-badge';
  if (url.startsWith('https://')) {
    secBadge.textContent = '🔒 HTTPS';
    secBadge.classList.add('tls');
  } else if (url.startsWith('view-source:')) {
    secBadge.textContent = '⟨/⟩ SRC';
    secBadge.classList.add('kajo');
  } else if (url.startsWith('kajo://')) {
    secBadge.textContent = '❖ KAJO';
    secBadge.classList.add('kajo');
  } else if (url.startsWith('http://')) {
    secBadge.textContent = 'ⓘ HTTP';
    secBadge.classList.add('warn');
  } else {
    secBadge.textContent = '❖';
  }

  blockedNum.textContent = String(s.tab_blocked || 0);
  btnShield.className = 'pill ' + ((s.adblock_enabled && !s.host_paused) ? 'shield-on' : 'shield-paused');
  btnShield.title = s.host_paused ? 'Protection paused on ' + (s.active_host || 'host') :
                    'Adblock active · ' + (s.tab_blocked || 0) + ' blocked on tab · ' + (s.total_blocked || 0) + ' session';

  if (lastIsBookmarked !== !!s.is_bookmarked) {
    lastIsBookmarked = !!s.is_bookmarked;
    btnStar.innerHTML = lastIsBookmarked ? SVG_STAR_FILLED : SVG_STAR_OUTLINE;
  }
  btnStar.classList.toggle('star-on', !!s.is_bookmarked);

  const canInstallApp = !!s.active_url && (s.active_url.startsWith('http://') || s.active_url.startsWith('https://'));
  btnInstallApp.classList.toggle('hidden', !canInstallApp);
  const appInstalled = !!(s.install_draft && s.install_draft.already_installed);
  btnInstallApp.classList.toggle('star-on', appInstalled);
  btnInstallApp.title = appInstalled
    ? ('Web App Installed: ' + (s.install_draft.name || '') + ' (Click to Launch or Manage)')
    : 'Install Site as Web App (Chromeless Window)';

  btnSwRender.classList.toggle('hidden', s.hw_accel_enabled !== false);

  const mc = (s.media_streams || []).length;
  btnMedia.classList.toggle('hidden', mc === 0);
  mediaCount.textContent = String(mc);

  const dls = s.downloads || [];
  const activeDls = dls.filter(d => d.in_progress);
  btnDl.classList.toggle('active', s.overlay_mode === 'downloads');
  btnDl.classList.toggle('dl-busy', activeDls.length > 0);
  btnDl.classList.toggle('dl-done', activeDls.length === 0 && !!s.downloads_unseen_complete);
  if (activeDls.length > 0) {
    let known = 0, sumPct = 0;
    for (const d of activeDls) {
      if (d.percent >= 0) { sumPct += Math.min(100, d.percent); ++known; }
    }
    const pct = known > 0 ? Math.round(sumPct / known) : -1;
    dlCount.textContent = pct >= 0 ? (pct + '%') : String(activeDls.length);
    dlProgBar.style.width = (pct >= 0 ? pct : 35) + '%';
    btnDl.title = 'Downloads active (' + activeDls.length + ') · Click or Ctrl+J';
  } else {
    dlCount.textContent = '';
    dlProgBar.style.width = '0%';
    btnDl.title = 'Downloads (' + dls.length + ' total) · Ctrl+J';
  }

  btnZoom.classList.toggle('hidden', (s.zoom_pct || 100) === (s.default_zoom_pct || 100));
  zoomLabel.textContent = (s.zoom_pct || 100) + '%';

  profileDot.style.background = s.private_mode ? '#b146c2' : (s.profile_accent || s.accent_color || '#0078d4');
  profileLabel.textContent = s.private_mode ? 'PRIVATE' : (s.profile_name || 'DEFAULT');
};

omni.addEventListener('focus', () => {
  omniWrap.classList.add('focused');
  send('topbar_focus', { focused: true });
  setTimeout(() => {
    if (document.activeElement === omni) omni.select();
  }, 0);
});
omni.addEventListener('blur', () => {
  omniWrap.classList.remove('focused');
  if (lastState) updateOmniDisplay(lastState.active_url || '');
  acSelectedIdx = -1;
  send('topbar_focus', { focused: false });
  setTimeout(() => {
    if (document.activeElement !== omni) {
      send('ac_close');
    }
  }, 150);
});
omni.addEventListener('input', () => {
  acSelectedIdx = -1;
  send('ac_query', { query: omni.value });
});
omni.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    e.preventDefault();
    if (acSelectedIdx >= 0) {
      send('ac_accept', { index: acSelectedIdx });
    } else {
      send('navigate', { input: omni.value });
    }
    acSelectedIdx = -1;
    omniWrap.classList.remove('focused');
    omni.blur();
    send('topbar_focus', { focused: false });
  } else if (e.key === 'Escape') {
    e.preventDefault();
    acSelectedIdx = -1;
    if (lastState) {
      omni.value = lastState.active_url || '';
      updateOmniDisplay(omni.value);
    }
    omniWrap.classList.remove('focused');
    omni.blur();
    send('topbar_focus', { focused: false });
    send('ac_close');
    send('focus_content');
  } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault();
    const list = (lastState && lastState.autocomplete) ? lastState.autocomplete : [];
    if (!list.length) return;
    if (e.key === 'ArrowDown') acSelectedIdx = (acSelectedIdx + 1) % list.length;
    else acSelectedIdx = (acSelectedIdx <= 0) ? (list.length - 1) : (acSelectedIdx - 1);
    send('ac_step', { index: acSelectedIdx });
  }
});

btnBack.onclick = () => send('back');
btnFwd.onclick = () => send('forward');
btnReload.onclick = () => send('reload_or_stop');
btnRail.onclick = () => send('toggle_rail');
btnShield.onclick = () => send('toggle_overlay', { mode: 'adblock' });
btnInstallApp.onclick = () => send('open_install_app');
btnStar.onclick = () => send('toggle_bookmark');
btnSwRender.onclick = () => send('open_kajo_page', { page: 'kajo://gpu' });
btnMedia.onclick = () => send('toggle_overlay', { mode: 'media' });
btnDl.onclick = () => send('toggle_overlay', { mode: 'downloads' });
btnZoom.onclick = () => send('zoom_reset');
btnProfile.onclick = () => send('toggle_overlay', { mode: 'profiles' });
document.getElementById('btnSettings').onclick = () => send('open_settings');
document.getElementById('btnFloat').onclick = () => send('toggle_float');
document.getElementById('btnClose').onclick = () => send('close_window');

window.__kajoSetBlocked = (tabBlocked, totalBlocked) => {
  const tb = Number(tabBlocked) || 0;
  const tot = Number(totalBlocked) || 0;
  blockedNum.textContent = String(tb);
  if (lastState) {
    lastState.tab_blocked = tb;
    lastState.total_blocked = tot;
    if (!lastState.host_paused) {
      btnShield.title = 'Adblock active · ' + tb + ' blocked on tab · ' + tot + ' session';
    }
  }
};

send('get_state').then(s => { if (s) window.__kajoApply(s); });
</script>
</body>
</html>
)KAJO";

static const char* kRailHtml = R"KAJO(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<link rel="stylesheet" href="kajo-chrome://ui/theme.css">
<style>
  html, body {
    height: 100vh;
    overflow: hidden;
    background: transparent;
  }
  .rail {
    height: 100vh;
    display: flex;
    flex-direction: column;
    border-right: 1px solid var(--metro-border);
    background: var(--metro-surface-0);
  }
  .rail.side-right {
    border-right: none;
    border-left: 1px solid var(--metro-border);
  }
  .rail.overlay-elevated {
    border-right: 1px solid var(--metro-accent);
  }
  .rail.side-right.overlay-elevated {
    border-right: none;
    border-left: 1px solid var(--metro-accent);
  }
  .rail.collapsed-strip {
    background: rgba(0, 0, 0, 0.01);
    border: none;
    border-left: 2px solid var(--metro-border-strong);
  }
  .rail.side-right.collapsed-strip {
    border-left: none;
    border-right: 2px solid var(--metro-border-strong);
  }
  .edge-ticks {
    display: none;
    position: fixed;
    top: 8px;
    bottom: 8px;
    left: 0;
    width: 3px;
    flex-direction: column;
    gap: 3px;
    pointer-events: none;
    overflow: hidden;
  }
  .rail.side-right .edge-ticks {
    left: auto;
    right: 0;
  }
  .rail.collapsed-strip .edge-ticks { display: flex; }
  .edge-tick {
    width: 3px;
    height: 11px;
    background: #555555;
    flex-shrink: 0;
  }
  .edge-tick.active {
    background: var(--metro-accent);
    height: 16px;
  }
  .edge-tick.suspended { background: #2d2d2d; }
  .rail.collapsed-strip .rail-head,
  .rail.collapsed-strip .pinned-grid,
  .rail.collapsed-strip .tab-list,
  .rail.collapsed-strip .rail-foot { display: none; }
  .pinned-grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 3px;
    padding: 5px 6px;
    border-bottom: 1px solid var(--metro-border);
    background: var(--metro-surface-1);
    flex-shrink: 0;
  }
  .pinned-grid.hidden { display: none; }
  .rail.compact .pinned-grid {
    grid-template-columns: 1fr;
    padding: 4px;
  }
  .pin-tile {
    position: relative;
    height: calc(var(--metro-ctrl-h) + 8px);
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
  }
  .pin-tile:hover { background: var(--metro-surface-3); border-color: var(--metro-border-strong); }
  .pin-tile.active {
    background: var(--metro-surface-3);
    border-color: var(--metro-accent);
    box-shadow: inset 0 -2px 0 var(--metro-accent);
  }
  .pin-tile.suspended { opacity: 0.72; }
  .pin-tile.drop-above { border-left: 2px solid var(--metro-accent); }
  .pin-tile.drop-below { border-right: 2px solid var(--metro-accent); }
  .pin-mute {
    position: absolute;
    right: 1px;
    bottom: 0;
    font-size: var(--metro-badge-size);
    line-height: 1;
  }
  .tab-row.drop-above { box-shadow: inset 0 2px 0 var(--metro-accent); }
  .tab-row.drop-below { box-shadow: inset 0 -2px 0 var(--metro-accent); }
  .ctx-menu {
    position: fixed;
    z-index: 50;
    min-width: 204px;
    max-width: 260px;
    background: var(--metro-surface-1);
    border: 1px solid var(--metro-accent);
    padding: 3px 0;
    box-shadow: 0 4px 16px rgba(0, 0, 0, 0.7);
  }
  .ctx-menu.hidden { display: none; }
  .ctx-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    width: 100%;
    padding: 6px 10px;
    background: transparent;
    border: none;
    color: var(--metro-fg);
    font-size: var(--metro-sub-size);
    text-align: left;
    cursor: pointer;
  }
  .ctx-item:hover:not(:disabled) {
    background: var(--metro-surface-4);
    color: #ffffff;
  }
  .ctx-item:disabled { opacity: 0.35; cursor: default; }
  .ctx-sep { height: 1px; background: var(--metro-border); margin: 3px 0; }
  .ctx-sub {
    padding: 4px 8px 6px 10px;
    border-top: 1px solid var(--metro-border);
    background: var(--metro-surface-0);
  }
  .ctx-sub-lbl {
    font-size: var(--metro-badge-size);
    color: var(--metro-fg-dim);
    text-transform: uppercase;
    letter-spacing: 0.6px;
    margin-bottom: 4px;
  }
  .ctx-chips {
    display: flex;
    flex-wrap: wrap;
    gap: 3px;
    margin-bottom: 4px;
  }
  .ctx-chip {
    padding: 2px 6px;
    font-size: var(--metro-badge-size);
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border-strong);
    color: var(--metro-fg);
    cursor: pointer;
  }
  .ctx-chip.active, .ctx-chip:hover {
    border-color: var(--metro-accent);
    color: #ffffff;
  }
  .ctx-grp-row {
    display: flex;
    gap: 3px;
  }
  .ctx-grp-inp {
    flex: 1;
    min-width: 0;
    padding: 2px 5px;
    background: #0d0d0d;
    border: 1px solid var(--metro-border-strong);
    color: #ffffff;
    font-size: var(--metro-badge-size);
  }
  .rail-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 6px;
    gap: 4px;
    border-bottom: 1px solid var(--metro-border);
    flex-shrink: 0;
  }
  .newtab-btn {
    flex: 1;
    min-height: var(--metro-ctrl-h);
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    color: var(--metro-fg);
    cursor: pointer;
    font-size: var(--metro-sub-size);
    font-weight: 600;
    text-align: left;
    padding: 0 10px;
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .newtab-btn:hover { border-color: var(--metro-accent); background: var(--metro-surface-4); }
  .rail.compact .rail-head { padding: 6px 4px; justify-content: center; }
  .rail.compact .newtab-btn { justify-content: center; padding: 0; text-align: center; }
  .tab-list {
    flex: 1 1 auto;
    overflow-y: auto;
    overflow-x: hidden;
    padding: 4px 0;
  }
  .group-hdr {
    display: flex;
    align-items: center;
    justify-content: space-between;
    min-height: calc(var(--metro-badge-size) + 15px);
    height: auto;
    padding: 3px 8px;
    font-size: var(--metro-badge-size);
    font-weight: 700;
    letter-spacing: 0.8px;
    color: var(--metro-fg-dim);
    background: var(--metro-surface-1);
    border-top: 1px solid var(--metro-border);
    border-bottom: 1px solid var(--metro-border);
    cursor: pointer;
    text-transform: uppercase;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .group-hdr:hover { color: var(--metro-fg); }
  .tab-row {
    position: relative;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 5px 8px 5px 11px;
    border-left: 3px solid transparent;
    cursor: pointer;
    min-height: calc(var(--metro-font-size) + var(--metro-badge-size) + 21px);
    height: auto;
    overflow: hidden;
  }
  .rail.compact .tab-row {
    justify-content: flex-start;
    padding: 4px 2px 4px 11px;
    gap: 0;
  }
  .tab-row:hover { background: var(--metro-surface-3); }
  .tab-row.active {
    background: var(--metro-surface-3);
    border-left-color: var(--metro-accent);
  }
  .tab-row.suspended { opacity: 0.78; }
  .tab-row.suspended .tab-title { color: var(--metro-fg-dim); }
  .tab-row.suspended .fav, .pin-tile.suspended .fav {
    filter: grayscale(100%);
    opacity: 0.65;
    border-style: dashed;
  }
  .fav {
    width: calc(var(--metro-icon-size) + 4px);
    height: calc(var(--metro-icon-size) + 4px);
    flex-shrink: 0;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    font-size: var(--metro-badge-size);
    color: var(--metro-accent);
    overflow: hidden;
  }
  .fav img { width: var(--metro-icon-size); height: var(--metro-icon-size); display: block; }
  .tab-text {
    flex: 1 1 auto;
    min-width: 0;
    display: flex;
    flex-direction: column;
  }
  .rail.compact .tab-text,
  .rail.compact .tab-actions,
  .rail.compact .rail-foot-label { display: none; }
  .tab-title {
    font-size: var(--metro-font-size);
    line-height: 1.25;
    color: var(--metro-fg);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .tab-domain {
    font-size: var(--metro-badge-size);
    line-height: 1.2;
    color: var(--metro-fg-dim);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .tab-actions {
    display: flex;
    align-items: center;
    gap: 2px;
    flex-shrink: 0;
  }
  .mini-btn {
    width: calc(var(--metro-sub-size) + 10px);
    height: calc(var(--metro-sub-size) + 10px);
    background: transparent;
    border: none;
    color: var(--metro-fg-dim);
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    font-size: var(--metro-sub-size);
    padding: 0;
  }
  .mini-btn:hover { background: var(--metro-surface-4); color: #ffffff; }
  .mini-btn.close:hover { background: var(--metro-urgent); color: #ffffff; }
  .tab-row:not(:hover) .mini-btn.close { opacity: 0.3; }
  .rail-foot {
    border-top: 1px solid var(--metro-border);
    padding: 6px 8px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 4px;
    font-size: var(--metro-badge-size);
    color: var(--metro-fg-dim);
    flex-shrink: 0;
  }
  .mode-btn {
    min-height: calc(var(--metro-ctrl-h) - 4px);
    height: auto;
    padding: 0 8px;
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    color: var(--metro-fg-dim);
    cursor: pointer;
    font-size: var(--metro-badge-size);
  }
  .mode-btn:hover { border-color: var(--metro-accent); color: #ffffff; }
</style>
</head>
<body>
<div id="railRoot" class="rail">
  <div id="edgeTicks" class="edge-ticks"></div>
  <div class="rail-head">
    <button id="btnNewTab" class="newtab-btn" title="New Tab (Ctrl+T)">
      <span id="newTabLabel">+ NEW TAB</span>
      <span class="rail-foot-label">^T</span>
    </button>
  </div>
  <div id="pinnedGrid" class="pinned-grid hidden"></div>
  <div id="tabList" class="tab-list"></div>
  <div class="rail-foot">
    <span id="tabCount" class="rail-foot-label">1 TAB</span>
    <div style="display:flex;gap:3px;align-items:center;">
      <button id="btnReopen" class="mode-btn rail-foot-label" title="Reopen Closed Tab (Ctrl+Shift+T)" disabled>↺</button>
      <button id="btnPalette" class="mode-btn rail-foot-label" title="Command Palette (Ctrl+K)">⌘K</button>
      <button id="btnMode" class="mode-btn" title="Cycle Rail Mode (Compact / Pinned / Autohide)">⇆</button>
    </div>
  </div>
</div>
<div id="tabCtxMenu" class="ctx-menu hidden"></div>

<script>
const send = (cmd, extra = {}) => {
  if (!window.cefQuery) return Promise.resolve(null);
  return new Promise((resolve) => {
    window.cefQuery({
      request: JSON.stringify({ cmd, ...extra }),
      onSuccess: (res) => { try { resolve(JSON.parse(res)); } catch { resolve(null); } },
      onFailure: () => resolve(null)
    });
  });
};

const railRoot = document.getElementById('railRoot');
const edgeTicks = document.getElementById('edgeTicks');
const pinnedGrid = document.getElementById('pinnedGrid');
const tabList = document.getElementById('tabList');
const btnNewTab = document.getElementById('btnNewTab');
const newTabLabel = document.getElementById('newTabLabel');
const tabCount = document.getElementById('tabCount');
const btnReopen = document.getElementById('btnReopen');
const btnMode = document.getElementById('btnMode');
const tabCtxMenu = document.getElementById('tabCtxMenu');

let lastRailState = null;
let ctxTabId = null;
let dragTabId = null;
let holdSent = false;

function setRailHold(hold) {
  if (holdSent === hold) return;
  holdSent = hold;
  send('rail_hold', { hold });
}

window.__kajoCloseCtxMenu = () => {
  if (ctxTabId === null && tabCtxMenu.classList.contains('hidden')) return;
  ctxTabId = null;
  tabCtxMenu.classList.add('hidden');
  tabCtxMenu.innerHTML = '';
  if (!dragTabId) setRailHold(false);
};

railRoot.addEventListener('mouseenter', () => {
  send('rail_hover', { inside: true });
});
railRoot.addEventListener('mousemove', () => {
  if (lastRailState && lastRailState.rail_mode !== 2 && !lastRailState.rail_hover_open) {
    send('rail_hover', { inside: true });
  }
});
railRoot.addEventListener('mouseleave', () => {
  if (ctxTabId === null && !dragTabId) {
    send('rail_hover', { inside: false });
  }
});
window.addEventListener('blur', () => {
  window.__kajoCloseCtxMenu();
  if (lastRailState && lastRailState.rail_hover_open) {
    send('rail_hover', { inside: false });
  }
});
document.addEventListener('mousedown', (e) => {
  if (ctxTabId !== null && !tabCtxMenu.contains(e.target)) {
    window.__kajoCloseCtxMenu();
  }
});
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && ctxTabId !== null) {
    e.preventDefault();
    window.__kajoCloseCtxMenu();
  }
});

btnNewTab.onclick = () => send('new_tab');
btnReopen.onclick = () => send('reopen_tab');
document.getElementById('btnPalette').onclick = () => send('toggle_overlay', { mode: 'palette' });
btnMode.onclick = () => send('cycle_rail_mode');

function makeFavicon(t) {
  const box = document.createElement('span');
  box.className = 'fav';
  if (t.loading) {
    box.textContent = '◌';
    return box;
  }
  if (t.favicon_url) {
    const img = document.createElement('img');
    img.referrerPolicy = 'no-referrer';
    img.loading = 'lazy';
    img.src = t.favicon_url;
    img.onerror = () => {
      box.textContent = t.pinned ? '★' : ((t.domain || t.title || 'K')[0] || 'K').toUpperCase();
    };
    box.appendChild(img);
    return box;
  }
  box.textContent = t.pinned ? '★' : ((t.domain || t.title || 'K')[0] || 'K').toUpperCase();
  return box;
}

function openTabCtxMenu(t, x, y) {
  if (!lastRailState) return;
  ctxTabId = t.id;
  setRailHold(true);
  tabCtxMenu.innerHTML = '';

  const addItem = (label, disabled, onClick) => {
    const b = document.createElement('button');
    b.className = 'ctx-item';
    b.textContent = label;
    b.disabled = !!disabled;
    b.onclick = (e) => {
      e.stopPropagation();
      window.__kajoCloseCtxMenu();
      onClick();
    };
    tabCtxMenu.appendChild(b);
  };

  addItem(t.pinned ? 'Unpin Tab' : 'Pin Tab', false, () => send('toggle_pin', { tab_id: t.id }));
  addItem(t.muted ? 'Unmute Tab' : 'Mute Tab', false, () => send('toggle_mute', { tab_id: t.id }));
  addItem('Duplicate Tab', false, () => send('duplicate_tab', { tab_id: t.id }));
  addItem('Suspend Tab', t.active || t.suspended, () => send('suspend_tab', { tab_id: t.id }));

  const sub = document.createElement('div');
  sub.className = 'ctx-sub';
  const subLbl = document.createElement('div');
  subLbl.className = 'ctx-sub-lbl';
  subLbl.textContent = 'Move to Group';
  sub.appendChild(subLbl);

  const groups = [];
  for (const cand of (lastRailState.tabs || [])) {
    if (cand.group && !groups.includes(cand.group)) groups.push(cand.group);
  }
  const chips = document.createElement('div');
  chips.className = 'ctx-chips';
  const addChip = (lbl, grpVal, isAct) => {
    const ch = document.createElement('button');
    ch.className = 'ctx-chip' + (isAct ? ' active' : '');
    ch.textContent = lbl;
    ch.onclick = (e) => {
      e.stopPropagation();
      window.__kajoCloseCtxMenu();
      send('set_tab_group', { tab_id: t.id, group: grpVal });
    };
    chips.appendChild(ch);
  };
  if (t.group) addChip('None', '', false);
  for (const g of groups) addChip(g, g, t.group === g);
  if (groups.length || t.group) sub.appendChild(chips);

  const bNewGrp = document.createElement('button');
  bNewGrp.className = 'ctx-chip';
  bNewGrp.style.width = '100%';
  bNewGrp.textContent = '+ New Group...';
  bNewGrp.onclick = (e) => {
    e.stopPropagation();
    window.__kajoCloseCtxMenu();
    send('open_group_prompt', { tab_id: t.id });
  };
  sub.appendChild(bNewGrp);
  tabCtxMenu.appendChild(sub);

  const sep = document.createElement('div');
  sep.className = 'ctx-sep';
  tabCtxMenu.appendChild(sep);

  addItem('Close Tab', false, () => send('close_tab', { tab_id: t.id }));
  addItem('Close Other Tabs', (lastRailState.tabs || []).length <= 1, () => send('close_other_tabs', { tab_id: t.id }));

  tabCtxMenu.classList.remove('hidden');
  const mw = tabCtxMenu.offsetWidth || 190;
  const mh = tabCtxMenu.offsetHeight || 220;
  const px = Math.max(4, Math.min(x, window.innerWidth - mw - 4));
  const py = Math.max(4, Math.min(y, window.innerHeight - mh - 4));
  tabCtxMenu.style.left = px + 'px';
  tabCtxMenu.style.top = py + 'px';
}

function attachDragHandlers(el, t) {
  el.draggable = true;
  el.addEventListener('dragstart', (e) => {
    dragTabId = t.id;
    setRailHold(true);
    if (e.dataTransfer) {
      e.dataTransfer.effectAllowed = 'move';
      e.dataTransfer.setData('text/plain', String(t.id));
    }
  });
  el.addEventListener('dragover', (e) => {
    if (dragTabId === null || dragTabId === t.id) return;
    e.preventDefault();
    const r = el.getBoundingClientRect();
    const after = t.pinned ? (e.clientX > r.left + r.width / 2) : (e.clientY > r.top + r.height / 2);
    el.classList.toggle('drop-above', !after);
    el.classList.toggle('drop-below', after);
  });
  el.addEventListener('dragleave', () => {
    el.classList.remove('drop-above', 'drop-below');
  });
  el.addEventListener('drop', (e) => {
    e.preventDefault();
    const after = el.classList.contains('drop-below');
    el.classList.remove('drop-above', 'drop-below');
    const srcId = dragTabId;
    dragTabId = null;
    if (ctxTabId === null) setRailHold(false);
    if (srcId !== null && srcId !== t.id) {
      send('move_tab', { tab_id: srcId, target_id: t.id, after, group: t.group || '' });
    }
  });
  el.addEventListener('dragend', () => {
    dragTabId = null;
    for (const d of document.querySelectorAll('.drop-above, .drop-below')) {
      d.classList.remove('drop-above', 'drop-below');
    }
    if (ctxTabId === null) setRailHold(false);
  });
}

let lastRailSig = '';

window.__kajoApply = (s) => {
  lastRailState = s;
  const tabsSig = (s.tabs || []).map(t =>
    [t.id, t.title || '', t.url || '', t.domain || '', t.group || '', t.favicon_url || '',
     t.pinned ? 1 : 0, t.active ? 1 : 0, t.loading ? 1 : 0, t.audible ? 1 : 0, t.muted ? 1 : 0, t.suspended ? 1 : 0].join('\x1f')
  ).join('\x1e');
  const railSig = [
    s.private_mode ? 1 : 0, s.profile_accent || '', s.accent_color || '',
    s.ui_font_family || '', s.ui_font_size_px || 0, s.heading_size_px || 0,
    s.sub_size_px || 0, s.badge_size_px || 0, s.topbar_height_px || 0,
    s.control_height_px || 0, s.icon_size_px || 0,
    s.rail_mode, s.rail_side, s.rail_hover_open ? 1 : 0,
    s.closed_tab_count || 0, s.show_tab_domains ? 1 : 0,
    (s.collapsed_groups || []).join(','), tabsSig
  ].join('|');
  if (railSig === lastRailSig) return;
  lastRailSig = railSig;

  const ds = document.documentElement.style;
  const acc = s.private_mode ? '#b146c2' : (s.profile_accent || s.accent_color || '#0078d4');
  ds.setProperty('--metro-accent', acc);
  if (s.accent_color) ds.setProperty('--metro-theme-accent', s.accent_color);
  if (s.ui_font_family) ds.setProperty('--metro-font', '"' + s.ui_font_family + '", "Fira Code", monospace');
  if (s.ui_font_size_px) ds.setProperty('--metro-font-size', s.ui_font_size_px + 'px');
  if (s.heading_size_px) ds.setProperty('--metro-heading-size', s.heading_size_px + 'px');
  if (s.sub_size_px) ds.setProperty('--metro-sub-size', s.sub_size_px + 'px');
  if (s.badge_size_px) ds.setProperty('--metro-badge-size', s.badge_size_px + 'px');
  if (s.topbar_height_px) ds.setProperty('--metro-topbar-h', s.topbar_height_px + 'px');
  if (s.control_height_px) ds.setProperty('--metro-ctrl-h', s.control_height_px + 'px');
  if (s.icon_size_px) ds.setProperty('--metro-icon-size', s.icon_size_px + 'px');
  const isCompact = (s.rail_mode === 1 && !s.rail_hover_open);
  const isAutohideCollapsed = (s.rail_mode === 0 && !s.rail_hover_open);
  const isOverlay = (s.rail_hover_open && (s.rail_mode === 0 || s.rail_mode === 1));
  railRoot.className = 'rail' +
    (s.rail_side === 1 ? ' side-right' : '') +
    (isCompact ? ' compact' : '') +
    (isOverlay ? ' overlay-elevated' : '') +
    (isAutohideCollapsed ? ' collapsed-strip' : '');
  btnMode.title = 'Rail Mode: ' + (s.rail_mode === 0 ? 'AUTOHIDE' : (s.rail_mode === 1 ? 'COMPACT' : 'PINNED')) + ' (click to cycle)';

  if (isAutohideCollapsed) {
    edgeTicks.innerHTML = '';
    for (const t of (s.tabs || []).slice(0, 48)) {
      const tk = document.createElement('div');
      tk.className = 'edge-tick' + (t.active ? ' active' : (t.suspended ? ' suspended' : ''));
      edgeTicks.appendChild(tk);
    }
    pinnedGrid.innerHTML = '';
    pinnedGrid.classList.add('hidden');
    tabList.innerHTML = '';
    return;
  }

  newTabLabel.textContent = isCompact ? '+' : '+ NEW TAB';
  tabCount.textContent = s.tabs.length + (s.tabs.length === 1 ? ' TAB' : ' TABS');
  btnReopen.disabled = !(s.closed_tab_count > 0);

  if (ctxTabId !== null && !(s.tabs || []).some(t => t.id === ctxTabId)) {
    window.__kajoCloseCtxMenu();
  }

  const pinnedTabs = (s.tabs || []).filter(t => t.pinned);
  pinnedGrid.innerHTML = '';
  pinnedGrid.classList.toggle('hidden', pinnedTabs.length === 0);
  for (const t of pinnedTabs) {
    const tile = document.createElement('div');
    tile.className = 'pin-tile' + (t.active ? ' active' : '') + (t.suspended ? ' suspended' : '');
    tile.title = (t.title || t.url) + (t.url ? '\n' + t.url : '');
    tile.appendChild(makeFavicon(t));
    if (t.muted || t.audible) {
      const pm = document.createElement('span');
      pm.className = 'pin-mute';
      pm.textContent = t.muted ? '🔇' : '🔊';
      tile.appendChild(pm);
    }
    tile.onclick = () => send('activate_tab', { tab_id: t.id });
    /* Pinned tabs ignore middle-click close */
    tile.onauxclick = (e) => { if (e.button === 1) e.preventDefault(); };
    tile.oncontextmenu = (e) => { e.preventDefault(); openTabCtxMenu(t, e.clientX, e.clientY); };
    attachDragHandlers(tile, t);
    pinnedGrid.appendChild(tile);
  }

  const prevScroll = tabList.scrollTop;
  const collapsed = new Set(s.collapsed_groups || []);
  tabList.innerHTML = '';

  const renderTabs = (tabs, groupName) => {
    if (!tabs.length) return;
    if (groupName) {
      const hdr = document.createElement('div');
      hdr.className = 'group-hdr';
      hdr.textContent = (collapsed.has(groupName) ? '▸ ' : '▾ ') + groupName;
      hdr.onclick = () => send('toggle_group', { group: groupName });
      tabList.appendChild(hdr);
      if (collapsed.has(groupName)) return;
    }
    for (const t of tabs) {
      const row = document.createElement('div');
      row.className = 'tab-row' + (t.active ? ' active' : '') + (t.suspended ? ' suspended' : '');
      row.title = (t.title || t.url) + (t.url ? '\n' + t.url : '');
      row.appendChild(makeFavicon(t));

      const txt = document.createElement('div');
      txt.className = 'tab-text';
      const title = document.createElement('div');
      title.className = 'tab-title';
      title.textContent = (t.title || t.domain || 'New Tab');
      txt.appendChild(title);
      if (s.show_tab_domains && (t.domain || t.suspended)) {
        const dom = document.createElement('div');
        dom.className = 'tab-domain';
        dom.textContent = (t.domain || t.url || '') + (t.suspended ? ' · zzz' : '');
        txt.appendChild(dom);
      }
      row.appendChild(txt);

      const act = document.createElement('div');
      act.className = 'tab-actions';
      const mb = document.createElement('button');
      mb.className = 'mini-btn';
      mb.textContent = t.muted ? '🔇' : '🔊';
      mb.style.opacity = (t.muted || t.audible) ? '1' : '0.45';
      mb.title = t.muted ? 'Unmute Tab (Ctrl+M)' : 'Mute Tab (Ctrl+M)';
      mb.onclick = (e) => { e.stopPropagation(); send('toggle_mute', { tab_id: t.id }); };
      act.appendChild(mb);
      const pb = document.createElement('button');
      pb.className = 'mini-btn';
      pb.textContent = t.pinned ? '◆' : '◇';
      pb.title = t.pinned ? 'Unpin Tab' : 'Pin Tab';
      pb.onclick = (e) => { e.stopPropagation(); send('toggle_pin', { tab_id: t.id }); };
      act.appendChild(pb);

      const cb = document.createElement('button');
      cb.className = 'mini-btn close';
      cb.textContent = '×';
      cb.title = 'Close Tab';
      cb.onclick = (e) => { e.stopPropagation(); send('close_tab', { tab_id: t.id }); };
      act.appendChild(cb);
      row.appendChild(act);

      row.onclick = () => send('activate_tab', { tab_id: t.id });
      row.onauxclick = (e) => { if (e.button === 1) { e.preventDefault(); if (!t.pinned) send('close_tab', { tab_id: t.id }); } };
      row.oncontextmenu = (e) => { e.preventDefault(); openTabCtxMenu(t, e.clientX, e.clientY); };
      attachDragHandlers(row, t);
      tabList.appendChild(row);
    }
  };

  const unpinned = (s.tabs || []).filter(t => !t.pinned);
  renderTabs(unpinned.filter(t => !t.group), '');

  const seenGroups = [];
  for (const t of unpinned) {
    if (t.group && !seenGroups.includes(t.group)) seenGroups.push(t.group);
  }
  for (const g of seenGroups) {
    renderTabs(unpinned.filter(t => t.group === g), g);
  }
  tabList.scrollTop = prevScroll;
};

send('get_state').then(s => { if (s) window.__kajoApply(s); });
</script>
</body>
</html>
)KAJO";

static const char* kOverlayHtml = R"KAJO(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<link rel="stylesheet" href="kajo-chrome://ui/theme.css">
<style>
  html, body {
    height: 100vh;
    overflow: hidden;
    background: var(--metro-surface-1);
    border: 1px solid var(--metro-accent);
  }
  .panel {
    height: 100vh;
    display: flex;
    flex-direction: column;
    background: var(--metro-surface-1);
  }
  .hdr {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 12px;
    background: var(--metro-surface-0);
    border-bottom: 1px solid var(--metro-border);
    font-size: var(--metro-heading-size);
    letter-spacing: 0.7px;
    text-transform: uppercase;
  }
  .hdr-title { color: #ffffff; font-weight: 600; }
  .xbtn {
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    color: var(--metro-fg);
    padding: 4px 10px;
    cursor: pointer;
    font-size: var(--metro-badge-size);
  }
  .xbtn:hover { background: var(--metro-urgent); border-color: var(--metro-urgent); }
  .search-box {
    width: 100%;
    padding: 9px 12px;
    background: #0d0d0d;
    border: none;
    border-bottom: 1px solid var(--metro-border-strong);
    color: #ffffff;
    font-size: var(--metro-font-size);
  }
  .list {
    flex: 1 1 auto;
    overflow-y: auto;
    padding: 4px;
  }
  .item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 8px 10px;
    border-left: 3px solid transparent;
    cursor: pointer;
    border-bottom: 1px solid #1e1e1e;
  }
  .item:hover, .item.sel {
    background: var(--metro-surface-3);
    border-left-color: var(--metro-accent);
  }
  .item-main { min-width: 0; flex: 1; }
  .item-title {
    color: #ffffff;
    font-size: var(--metro-font-size);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .item-sub {
    color: var(--metro-fg-dim);
    font-size: var(--metro-sub-size);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .badge {
    padding: 2px 6px;
    font-size: var(--metro-badge-size);
    border: 1px solid var(--metro-border-strong);
    background: var(--metro-surface-2);
    color: var(--metro-fg-dim);
    text-transform: uppercase;
    flex-shrink: 0;
  }
  .badge.accent { border-color: var(--metro-accent); color: #ffffff; }
  .act-row {
    display: flex;
    gap: 6px;
    padding: 8px 10px;
    border-top: 1px solid var(--metro-border);
    background: var(--metro-surface-0);
    flex-wrap: wrap;
  }
  .btn {
    padding: 5px 10px;
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border-strong);
    color: var(--metro-fg);
    cursor: pointer;
    font-size: var(--metro-sub-size);
  }
  .btn:hover { background: var(--metro-surface-4); border-color: var(--metro-accent); }
  .btn.primary { background: var(--metro-accent); border-color: var(--metro-accent); color: #ffffff; }
  .find-bar {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 6px 8px;
    height: 100vh;
  }
  .find-input {
    flex: 1;
    padding: 6px 10px;
    background: #0d0d0d;
    border: 1px solid var(--metro-border-strong);
    color: #ffffff;
    font-size: var(--metro-font-size);
  }
  .find-input:focus { border-color: var(--metro-accent); }
  .stat-grid {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 6px;
    padding: 10px;
  }
  .stat-card {
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    padding: 8px;
  }
  .stat-num { font-size: calc(var(--metro-font-size) + 4px); color: #ffffff; font-weight: 600; }
  .stat-lbl { font-size: var(--metro-badge-size); color: var(--metro-fg-dim); margin-top: 2px; }
  .prog {
    height: 4px;
    background: var(--metro-surface-0);
    margin-top: 4px;
    border: 1px solid var(--metro-border);
  }
  .prog > div { height: 100%; background: var(--metro-accent); }
  .perm-bar {
    height: 100vh;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 0 10px;
    background: var(--metro-surface-0);
    border-left: 4px solid var(--metro-warning);
  }
  .perm-msg {
    flex: 1;
    min-width: 0;
    font-size: var(--metro-font-size);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .perm-msg strong { color: #ffffff; }
  .form-body {
    padding: 12px;
    display: flex;
    flex-direction: column;
    gap: 9px;
    flex: 1;
  }
  .form-lbl {
    font-size: var(--metro-badge-size);
    color: var(--metro-fg-dim);
    text-transform: uppercase;
    letter-spacing: 0.6px;
    margin-bottom: 3px;
  }
  .form-inp {
    width: 100%;
    padding: 6px 10px;
    background: #0d0d0d;
    border: 1px solid var(--metro-border-strong);
    color: #ffffff;
    font-size: var(--metro-font-size);
  }
  .form-inp:focus { border-color: var(--metro-accent); }
  .btn.bad:hover { background: var(--metro-urgent); border-color: var(--metro-urgent); color: #ffffff; }
</style>
</head>
<body>
<div id="root" class="panel"></div>
<script>
const send = (cmd, extra = {}) => {
  if (!window.cefQuery) return Promise.resolve(null);
  return new Promise((resolve) => {
    window.cefQuery({
      request: JSON.stringify({ cmd, ...extra }),
      onSuccess: (res) => { try { resolve(JSON.parse(res)); } catch { resolve(null); } },
      onFailure: () => resolve(null)
    });
  });
};

const root = document.getElementById('root');
const roleParam = new URLSearchParams(location.search).get('role');
const isAcRole = roleParam === 'ac';
const isPermRole = roleParam === 'perm';
let curState = null;
let selectedIdx = 0;
let editingProfileId = '';
let confirmDeleteProfileId = '';

window.__kajoOverlayStep = (delta) => {
  const items = root.querySelectorAll('.item');
  if (!items.length) return;
  selectedIdx = Math.max(0, Math.min(items.length - 1, selectedIdx + delta));
  items.forEach((el, i) => el.classList.toggle('sel', i === selectedIdx));
  items[selectedIdx].scrollIntoView({ block: 'nearest' });
};

window.__kajoOverlayFocus = () => {
  if (isAcRole || isPermRole) return;
  const inp = root.querySelector('input');
  if (inp) { inp.focus(); inp.select(); }
};

function renderHeader(title) {
  const hdr = document.createElement('div');
  hdr.className = 'hdr';
  const t = document.createElement('span');
  t.className = 'hdr-title';
  t.textContent = title;
  const x = document.createElement('button');
  x.className = 'xbtn';
  x.textContent = 'ESC';
  x.onclick = () => send(isAcRole ? 'ac_close' : 'close_overlay');
  hdr.append(t, x);
  return hdr;
}

window.__kajoApply = (s) => {
  const ds = document.documentElement.style;
  const acc = s.private_mode ? '#b146c2' : (s.profile_accent || s.accent_color || '#0078d4');
  ds.setProperty('--metro-accent', acc);
  if (s.accent_color) ds.setProperty('--metro-theme-accent', s.accent_color);
  if (s.ui_font_family) ds.setProperty('--metro-font', '"' + s.ui_font_family + '", "Fira Code", monospace');
  if (s.ui_font_size_px) ds.setProperty('--metro-font-size', s.ui_font_size_px + 'px');
  if (s.heading_size_px) ds.setProperty('--metro-heading-size', s.heading_size_px + 'px');
  if (s.sub_size_px) ds.setProperty('--metro-sub-size', s.sub_size_px + 'px');
  if (s.badge_size_px) ds.setProperty('--metro-badge-size', s.badge_size_px + 'px');
  if (s.topbar_height_px) ds.setProperty('--metro-topbar-h', s.topbar_height_px + 'px');
  if (s.control_height_px) ds.setProperty('--metro-ctrl-h', s.control_height_px + 'px');
  if (s.icon_size_px) ds.setProperty('--metro-icon-size', s.icon_size_px + 'px');
  if (isPermRole) {
    curState = s;
    root.innerHTML = '';
    const ap = s.active_permission || {};
    if (!ap.req_id) return;
    const bar = document.createElement('div');
    bar.className = 'perm-bar';
    const msg = document.createElement('div');
    msg.className = 'perm-msg';
    const s1 = document.createElement('strong');
    s1.textContent = ap.origin || 'Site';
    const mid = document.createTextNode(' requests ');
    const s2 = document.createElement('strong');
    s2.textContent = ap.label || 'Permission';
    msg.append('⚠ ', s1, mid, s2);

    const bOnce = document.createElement('button');
    bOnce.className = 'btn';
    bOnce.textContent = 'Allow Once';
    bOnce.onclick = () => send('perm_respond', { req_id: ap.req_id, decision: 'once' });

    const bAlways = document.createElement('button');
    bAlways.className = 'btn primary';
    bAlways.textContent = 'Always Allow';
    bAlways.onclick = () => send('perm_respond', { req_id: ap.req_id, decision: 'always' });

    const bDeny = document.createElement('button');
    bDeny.className = 'btn bad';
    bDeny.textContent = 'Deny';
    bDeny.onclick = () => send('perm_respond', { req_id: ap.req_id, decision: 'deny' });

    const bClose = document.createElement('button');
    bClose.className = 'xbtn';
    bClose.textContent = '✕';
    bClose.onclick = () => send('perm_respond', { req_id: ap.req_id, decision: 'dismiss' });

    bar.append(msg, bOnce, bAlways, bDeny, bClose);
    root.appendChild(bar);
    return;
  }

  const modeChanged = !curState || curState.overlay_mode !== s.overlay_mode;
  curState = s;
  if (modeChanged) {
    selectedIdx = 0;
    editingProfileId = '';
    confirmDeleteProfileId = '';
  }
  const mode = isAcRole ? 'autocomplete' : (s.overlay_mode || '');

  if (mode === 'find') {
    if (!modeChanged && root.querySelector('#findInput')) {
      const cnt = root.querySelector('#findCount');
      if (cnt) cnt.textContent = s.find_count > 0 ? (s.find_active + '/' + s.find_count) : '0/0';
      const mc = root.querySelector('#findCase');
      if (mc) mc.classList.toggle('primary', !!s.find_match_case);
      return;
    }
    root.innerHTML = '';
    const bar = document.createElement('div');
    bar.className = 'find-bar';
    const inp = document.createElement('input');
    inp.id = 'findInput';
    inp.className = 'find-input';
    inp.placeholder = 'Find in page...';
    inp.value = s.find_query || '';
    const cnt = document.createElement('span');
    cnt.id = 'findCount';
    cnt.className = 'badge';
    cnt.textContent = s.find_count > 0 ? (s.find_active + '/' + s.find_count) : '0/0';
    const prev = document.createElement('button');
    prev.className = 'btn'; prev.textContent = '▲';
    prev.onclick = () => send('find_step', { forward: false });
    const next = document.createElement('button');
    next.className = 'btn'; next.textContent = '▼';
    next.onclick = () => send('find_step', { forward: true });
    const mc = document.createElement('button');
    mc.id = 'findCase';
    mc.className = 'btn' + (s.find_match_case ? ' primary' : '');
    mc.textContent = 'Aa';
    mc.onclick = () => send('find_toggle_case');
    const close = document.createElement('button');
    close.className = 'xbtn'; close.textContent = '✕';
    close.onclick = () => send('close_overlay');

    inp.addEventListener('input', () => send('find_query', { query: inp.value, forward: true }));
    inp.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); send('find_step', { forward: !e.shiftKey }); }
      else if (e.key === 'Escape') { e.preventDefault(); send('close_overlay'); }
    });
    bar.append(inp, cnt, prev, next, mc, close);
    root.appendChild(bar);
    setTimeout(() => { inp.focus(); inp.select(); }, 10);
    return;
  }

  if (mode === 'palette' || mode === 'autocomplete') {
    const itemsData = mode === 'palette' ? (s.palette_results || []) : (s.autocomplete || []);
    let listEl = root.querySelector('#listEl');
    let searchEl = root.querySelector('#searchEl');
    if (modeChanged || !listEl) {
      root.innerHTML = '';
      if (mode === 'palette') {
        root.appendChild(renderHeader('⌘ COMMAND & TAB PALETTE'));
        searchEl = document.createElement('input');
        searchEl.id = 'searchEl';
        searchEl.className = 'search-box';
        searchEl.placeholder = 'Type command, open tab, bookmark or URL...';
        searchEl.addEventListener('input', () => send('palette_query', { query: searchEl.value }));
        searchEl.addEventListener('keydown', (e) => {
          if (e.key === 'ArrowDown') { e.preventDefault(); window.__kajoOverlayStep(1); }
          else if (e.key === 'ArrowUp') { e.preventDefault(); window.__kajoOverlayStep(-1); }
          else if (e.key === 'Escape') { e.preventDefault(); send('close_overlay'); }
          else if (e.key === 'Enter') {
            e.preventDefault();
            const cur = (curState.palette_results || [])[selectedIdx];
            if (cur) send('run_suggestion', { action: cur.action, payload: cur.payload, tab_id: cur.tab_id });
          }
        });
        root.appendChild(searchEl);
        setTimeout(() => searchEl.focus(), 10);
      } else {
        root.appendChild(renderHeader('OMNIBAR SUGGESTIONS'));
      }
      listEl = document.createElement('div');
      listEl.id = 'listEl';
      listEl.className = 'list';
      root.appendChild(listEl);
    }

    listEl.innerHTML = '';
    const activeIdx = (mode === 'autocomplete') ? (s.ac_selected_idx ?? -1) : selectedIdx;
    if (mode === 'palette' && selectedIdx >= itemsData.length) selectedIdx = 0;
    itemsData.forEach((it, idx) => {
      const row = document.createElement('div');
      row.className = 'item' + (idx === activeIdx ? ' sel' : '');
      const m = document.createElement('div');
      m.className = 'item-main';
      const t = document.createElement('div');
      t.className = 'item-title';
      t.textContent = it.label || it.payload;
      const sub = document.createElement('div');
      sub.className = 'item-sub';
      sub.textContent = it.sublabel || '';
      m.append(t, sub);
      const b = document.createElement('span');
      b.className = 'badge' + (it.kind === 'tab' ? ' accent' : '');
      b.textContent = it.kind;
      row.append(m, b);
      row.onmousedown = (e) => {
        e.preventDefault();
        send('run_suggestion', { action: it.action, payload: it.payload, tab_id: it.tab_id });
      };
      listEl.appendChild(row);
    });
    return;
  }

  root.innerHTML = '';
  const makeMainBox = (titleTxt, subTxt) => {
    const m = document.createElement('div');
    m.className = 'item-main';
    const t = document.createElement('div');
    t.className = 'item-title';
    t.textContent = titleTxt;
    const sub = document.createElement('div');
    sub.className = 'item-sub';
    sub.textContent = subTxt;
    m.append(t, sub);
    return m;
  };
  const makeStatCard = (val, lblTxt) => {
    const c = document.createElement('div');
    c.className = 'stat-card';
    const n = document.createElement('div');
    n.className = 'stat-num';
    n.textContent = String(val);
    const l = document.createElement('div');
    l.className = 'stat-lbl';
    l.textContent = lblTxt;
    c.append(n, l);
    return c;
  };

  if (mode === 'adblock') {
    root.appendChild(renderHeader('🛡 SHIELD & CONTENT BLOCKER'));
    const grid = document.createElement('div');
    grid.className = 'stat-grid';
    grid.append(
      makeStatCard(s.tab_blocked || 0, 'TAB BLOCKED'),
      makeStatCard(s.total_blocked || 0, 'SESSION BLOCKED'),
      makeStatCard(s.adblock_rules || 0, 'ACTIVE RULES')
    );
    root.appendChild(grid);

    const list = document.createElement('div');
    list.className = 'list';
    for (const fl of (s.filter_lists || [])) {
      const row = document.createElement('div');
      row.className = 'item';
      const badge = document.createElement('span');
      badge.className = 'badge' + (fl.rule_count > 0 ? ' accent' : '');
      badge.textContent = fl.rule_count > 0 ? 'LOADED' : (s.filter_update_busy ? 'SYNC...' : 'EMPTY');
      row.append(makeMainBox(fl.name || '', (fl.rule_count || 0) + ' rules'), badge);
      row.onclick = () => send('open_kajo_page', { page: 'kajo://adblock' });
      list.appendChild(row);
    }
    root.appendChild(list);

    const act = document.createElement('div');
    act.className = 'act-row';
    const bPause = document.createElement('button');
    bPause.className = 'btn ' + (s.host_paused ? 'primary' : '');
    bPause.textContent = s.host_paused ? ('Resume on ' + (s.active_host || 'Host')) : ('Pause on ' + (s.active_host || 'Host'));
    bPause.disabled = !s.active_host;
    bPause.onclick = () => send('toggle_host_adblock');

    const bUpdate = document.createElement('button');
    bUpdate.className = 'btn';
    bUpdate.textContent = s.filter_update_busy ? 'Updating...' : 'Update Lists';
    bUpdate.disabled = !!s.filter_update_busy;
    bUpdate.onclick = () => send('update_filter_lists');

    const bDash = document.createElement('button');
    bDash.className = 'btn';
    bDash.textContent = 'Dashboard (kajo://adblock)';
    bDash.onclick = () => send('open_kajo_page', { page: 'kajo://adblock' });
    act.append(bPause, bUpdate, bDash);
    root.appendChild(act);
  } else if (mode === 'media') {
    root.appendChild(renderHeader('⬇ MEDIA STREAMS & DOWNLOADER'));
    const bar = document.createElement('div');
    bar.className = 'act-row';
    if (s.yt_dlp_available && s.active_url &&
        (s.active_url.startsWith('http://') || s.active_url.startsWith('https://'))) {
      const bYt = document.createElement('button');
      bYt.className = 'btn primary';
      bYt.textContent = '⬇ Download Page Video (yt-dlp)';
      bYt.onclick = () => send('download_page_ytdlp');
      bar.appendChild(bYt);
    } else {
      const note = document.createElement('span');
      note.className = 'item-sub';
      note.style.marginRight = 'auto';
      note.textContent = s.ffmpeg_available
        ? 'HLS/DASH streams remux via ffmpeg · Install yt-dlp for full site extraction'
        : 'ffmpeg not found in PATH — only direct MP4/WebM downloads available';
      bar.appendChild(note);
    }
    root.appendChild(bar);

    const list = document.createElement('div');
    list.className = 'list';
    const streams = s.media_streams || [];
    if (!streams.length) {
      const empty = document.createElement('div');
      empty.className = 'stat-card';
      empty.style.cssText = 'margin:8px;padding:14px;';
      const t = document.createElement('div');
      t.className = 'item-title';
      t.textContent = 'No media streams sniffed on this tab yet';
      const sub = document.createElement('div');
      sub.className = 'item-sub';
      sub.style.marginTop = '4px';
      sub.textContent = 'Start video or audio playback to capture HLS (.m3u8), DASH (.mpd), or direct streams.';
      empty.append(t, sub);
      list.appendChild(empty);
    }
    for (const m of streams) {
      const row = document.createElement('div');
      row.className = 'item';
      row.appendChild(makeMainBox(m.label || m.uri || '', (m.mime_type || m.kind || '') + ' · ' + (m.uri || '')));
      const play = document.createElement('button');
      play.className = 'btn';
      play.textContent = '▶ kajo-player';
      play.onclick = (e) => { e.stopPropagation(); send('play_in_kajo_player', { uri: m.uri }); };
      const isStream = (m.kind === 'HLS' || m.kind === 'DASH');
      const dl = document.createElement('button');
      dl.className = 'btn primary';
      dl.textContent = isStream ? '⬇ Save MP4' : '⬇ Save';
      if (isStream && !s.ffmpeg_available) {
        dl.disabled = true;
        dl.title = 'ffmpeg not found in PATH';
      }
      dl.onclick = (e) => { e.stopPropagation(); send('download_media_uri', { uri: m.uri }); };
      row.append(play, dl);
      list.appendChild(row);
    }
    root.appendChild(list);
  } else if (mode === 'downloads') {
    const fmtBytes = (b) => {
      if (!b || b <= 0) return '0 B';
      if (b >= 1073741824) return (b / 1073741824).toFixed(2) + ' GB';
      if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
      if (b >= 1024) return (b / 1024).toFixed(1) + ' KB';
      return b + ' B';
    };
    const dls = s.downloads || [];
    root.appendChild(renderHeader('↓ DOWNLOADS (' + dls.length + ')'));
    const list = document.createElement('div');
    list.className = 'list';
    if (!dls.length) {
      const empty = document.createElement('div');
      empty.className = 'stat-card';
      empty.style.cssText = 'margin:8px;padding:14px;';
      const t = document.createElement('div');
      t.className = 'item-title';
      t.textContent = 'No downloads yet';
      const sub = document.createElement('div');
      sub.className = 'item-sub';
      sub.style.marginTop = '4px';
      sub.textContent = 'Save folder: ' + (s.download_dir || '~/Downloads');
      empty.append(t, sub);
      list.appendChild(empty);
    }
    for (const d of dls) {
      const row = document.createElement('div');
      row.className = 'item';
      row.style.cursor = 'default';
      let st = 'ACTIVE';
      if (d.complete) st = 'DONE';
      else if (d.canceled) st = 'CANCELED';
      else if (d.paused) st = 'PAUSED';
      else if (d.percent >= 0) st = d.percent + '%';
      const sz = fmtBytes(d.received) + (d.total > 0 ? (' / ' + fmtBytes(d.total)) : '');
      const spd = (d.in_progress && !d.paused && d.speed > 0) ? (' · ' + fmtBytes(d.speed) + '/s') : '';
      const mainBox = makeMainBox(d.filename || d.url || 'download', st + ' · ' + sz + spd);
      const prog = document.createElement('div');
      prog.className = 'prog';
      const bar = document.createElement('div');
      const w = d.complete ? 100 : Math.max(0, Math.min(100, d.percent || 0));
      bar.style.width = w + '%';
      if (d.complete) bar.style.background = 'var(--metro-success)';
      else if (d.canceled) bar.style.background = 'var(--metro-urgent)';
      else if (d.paused) bar.style.background = 'var(--metro-warning)';
      prog.appendChild(bar);
      mainBox.appendChild(prog);
      row.appendChild(mainBox);

      if (d.in_progress) {
        if (d.paused) {
          const r = document.createElement('button');
          r.className = 'btn primary'; r.textContent = '▶'; r.title = 'Resume';
          r.onclick = () => send('resume_download', { id: d.id });
          row.appendChild(r);
        } else {
          const p = document.createElement('button');
          p.className = 'btn'; p.textContent = '⏸'; p.title = 'Pause';
          p.onclick = () => send('pause_download', { id: d.id });
          row.appendChild(p);
        }
        const c = document.createElement('button');
        c.className = 'btn bad'; c.textContent = '✕'; c.title = 'Cancel';
        c.onclick = () => send('cancel_download', { id: d.id });
        row.appendChild(c);
      } else if (d.complete) {
        const o = document.createElement('button');
        o.className = 'btn primary'; o.textContent = 'Open'; o.title = 'Open file';
        o.onclick = () => send('open_download_file', { path: d.full_path });
        const f = document.createElement('button');
        f.className = 'btn'; f.textContent = '📁'; f.title = 'Show in kajo-files';
        f.onclick = () => send('open_download_folder', { path: d.full_path });
        const rm = document.createElement('button');
        rm.className = 'btn bad'; rm.textContent = '✕'; rm.title = 'Remove from list';
        rm.onclick = () => send('remove_download', { id: d.id, db_id: d.db_id || 0 });
        row.append(o, f, rm);
      } else {
        if (d.url) {
          const rt = document.createElement('button');
          rt.className = 'btn primary'; rt.textContent = '↻'; rt.title = 'Retry download';
          rt.onclick = () => send('retry_download', { url: d.url });
          row.appendChild(rt);
        }
        const rm = document.createElement('button');
        rm.className = 'btn bad'; rm.textContent = '✕'; rm.title = 'Remove from list';
        rm.onclick = () => send('remove_download', { id: d.id, db_id: d.db_id || 0 });
        row.appendChild(rm);
      }
      list.appendChild(row);
    }
    root.appendChild(list);
    const act = document.createElement('div');
    act.className = 'act-row';
    const bAll = document.createElement('button');
    bAll.className = 'btn primary';
    bAll.textContent = 'kajo://downloads';
    bAll.onclick = () => send('open_kajo_page', { page: 'kajo://downloads' });
    const bDir = document.createElement('button');
    bDir.className = 'btn';
    bDir.textContent = '📁 Folder';
    bDir.onclick = () => send('open_download_folder', { path: '' });
    const bClr = document.createElement('button');
    bClr.className = 'btn';
    bClr.textContent = 'Clear Done';
    bClr.disabled = !dls.some(d => d.complete || d.canceled);
    bClr.onclick = () => send('clear_finished_downloads');
    const bSet = document.createElement('button');
    bSet.className = 'btn';
    bSet.textContent = '⚙';
    bSet.title = 'Download Settings';
    bSet.onclick = () => send('open_settings');
    act.append(bAll, bDir, bClr, bSet);
    root.appendChild(act);
  } else if (mode === 'bookmark') {
    if (!modeChanged && root.querySelector('#bmTitle')) return;
    const bm = s.active_bookmark || {};
    root.appendChild(renderHeader('★ BOOKMARK PAGE'));
    const body = document.createElement('div');
    body.className = 'form-body';

    const tLbl = document.createElement('div');
    tLbl.className = 'form-lbl';
    tLbl.textContent = 'Title';
    const tInp = document.createElement('input');
    tInp.id = 'bmTitle';
    tInp.className = 'form-inp';
    tInp.value = bm.title || s.active_title || s.active_url || '';

    const fLbl = document.createElement('div');
    fLbl.className = 'form-lbl';
    fLbl.textContent = 'Folder';
    const fInp = document.createElement('input');
    fInp.id = 'bmFolder';
    fInp.className = 'form-inp';
    fInp.setAttribute('list', 'bmFoldersList');
    fInp.placeholder = 'Optional folder (e.g. Work, Reading)';
    fInp.value = bm.folder || '';
    const dl = document.createElement('datalist');
    dl.id = 'bmFoldersList';
    for (const fl of (s.bookmark_folders || [])) {
      const opt = document.createElement('option');
      opt.value = fl;
      dl.appendChild(opt);
    }

    const save = () => send('save_bookmark', {
      uuid: bm.uuid || '',
      title: tInp.value.trim() || s.active_url || '',
      folder: fInp.value.trim()
    });
    for (const inp of [tInp, fInp]) {
      inp.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { e.preventDefault(); save(); }
        else if (e.key === 'Escape') { e.preventDefault(); send('close_overlay'); }
      });
    }

    body.append(tLbl, tInp, fLbl, fInp, dl);
    root.appendChild(body);

    const act = document.createElement('div');
    act.className = 'act-row';
    const bSave = document.createElement('button');
    bSave.className = 'btn primary';
    bSave.textContent = 'Save Bookmark';
    bSave.onclick = save;
    const bDel = document.createElement('button');
    bDel.className = 'btn bad';
    bDel.textContent = 'Remove';
    bDel.disabled = !bm.uuid;
    bDel.onclick = () => send('delete_bookmark', { uuid: bm.uuid || '' });
    act.append(bSave, bDel);
    root.appendChild(act);
    setTimeout(() => { tInp.focus(); tInp.select(); }, 10);
  } else if (mode === 'new_group') {
    if (!modeChanged && root.querySelector('#grpNameInp')) return;
    root.appendChild(renderHeader('▤ ASSIGN TAB GROUP'));
    const body = document.createElement('div');
    body.className = 'form-body';
    const gLbl = document.createElement('div');
    gLbl.className = 'form-lbl';
    gLbl.textContent = 'Group Name';
    const gInp = document.createElement('input');
    gInp.id = 'grpNameInp';
    gInp.className = 'form-inp';
    gInp.placeholder = 'e.g. WORK, RESEARCH, MEDIA';
    const applyGrp = (val) => send('set_tab_group', { tab_id: 0, group: val });
    gInp.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); if (gInp.value.trim()) applyGrp(gInp.value.trim()); }
      else if (e.key === 'Escape') { e.preventDefault(); send('close_overlay'); }
    });
    body.append(gLbl, gInp);
    root.appendChild(body);
    const act = document.createElement('div');
    act.className = 'act-row';
    const bSet = document.createElement('button');
    bSet.className = 'btn primary';
    bSet.textContent = 'Assign Group';
    bSet.onclick = () => { if (gInp.value.trim()) applyGrp(gInp.value.trim()); };
    const bClear = document.createElement('button');
    bClear.className = 'btn';
    bClear.textContent = 'Ungroup';
    bClear.onclick = () => applyGrp('');
    act.append(bSet, bClear);
    root.appendChild(act);
    setTimeout(() => { gInp.focus(); gInp.select(); }, 10);
  } else if (mode === 'install_app') {
    const d = s.install_draft || {};
    const existingInp = root.querySelector('#appNameInp');
    if (!modeChanged && existingInp) {
      if (d.name && !existingInp.dataset.dirty) existingInp.value = d.name;
      const scInp = root.querySelector('#appScopeInp');
      if (d.scope && scInp && !scInp.dataset.dirty) scInp.value = d.scope;
      const thInp = root.querySelector('#appThemeInp');
      const sw = root.querySelector('#appThemeSwatch');
      if (d.theme_color && thInp && !thInp.dataset.dirty) {
        thInp.value = d.theme_color;
        if (sw) sw.style.background = d.theme_color;
      }
      const icInp = root.querySelector('#appIconInp');
      if (d.icon_url && icInp && !icInp.dataset.dirty) icInp.value = d.icon_url;
      return;
    }
    root.appendChild(renderHeader(d.already_installed ? '⊞ MANAGE WEB APP' : '⊞ INSTALL WEB APP'));
    const body = document.createElement('div');
    body.className = 'form-body';
    const mkField = (id, label, val, ph) => {
      const lbl = document.createElement('div');
      lbl.className = 'form-lbl';
      lbl.textContent = label;
      const inp = document.createElement('input');
      inp.id = id;
      inp.className = 'form-inp';
      inp.value = val || '';
      inp.placeholder = ph || '';
      inp.addEventListener('input', () => { inp.dataset.dirty = '1'; });
      body.append(lbl, inp);
      return inp;
    };
    const nameInp = mkField('appNameInp', 'Application Name', d.name || s.active_title || s.active_host, 'e.g. Apple Music');
    const urlInp = mkField('appUrlInp', 'Start URL', d.start_url || s.active_url, 'https://...');
    const scopeInp = mkField('appScopeInp', 'App Scope Prefix', d.scope || '', 'https://example.com/');

    const thLbl = document.createElement('div');
    thLbl.className = 'form-lbl';
    thLbl.textContent = 'Window Accent Color (#rrggbb)';
    const thRow = document.createElement('div');
    thRow.style.cssText = 'display:flex;gap:6px;align-items:center;';
    const sw = document.createElement('span');
    sw.id = 'appThemeSwatch';
    sw.style.cssText = 'width:26px;height:26px;border:1px solid #3c3c3c;display:inline-block;flex-shrink:0;';
    sw.style.background = d.theme_color || '#0078d4';
    const thInp = document.createElement('input');
    thInp.id = 'appThemeInp';
    thInp.className = 'form-inp';
    thInp.style.flex = '1';
    thInp.value = d.theme_color || '#0078d4';
    thInp.placeholder = '#0078d4';
    thInp.addEventListener('input', () => {
      thInp.dataset.dirty = '1';
      if (/^#[0-9a-fA-F]{6}$/.test(thInp.value.trim())) sw.style.background = thInp.value.trim();
    });
    thRow.append(sw, thInp);
    body.append(thLbl, thRow);

    const iconInp = mkField('appIconInp', 'Icon URL (optional — monogram generated if empty)', d.icon_url || '', 'https://.../icon.png');
    root.appendChild(body);

    const submit = () => send('install_webapp', {
      name: nameInp.value.trim() || s.active_host || 'Web App',
      start_url: urlInp.value.trim() || s.active_url || '',
      scope: scopeInp.value.trim(),
      theme_color: thInp.value.trim() || '#0078d4',
      icon_url: iconInp.value.trim()
    });
    for (const inp of [nameInp, urlInp, scopeInp, thInp, iconInp]) {
      inp.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { e.preventDefault(); submit(); }
        else if (e.key === 'Escape') { e.preventDefault(); send('close_overlay'); }
      });
    }

    const act = document.createElement('div');
    act.className = 'act-row';
    const bSave = document.createElement('button');
    bSave.className = 'btn primary';
    bSave.textContent = d.already_installed ? 'Update & Launch' : 'Install App';
    bSave.onclick = submit;
    act.appendChild(bSave);
    if (d.already_installed && d.existing_id) {
      const bLaunch = document.createElement('button');
      bLaunch.className = 'btn';
      bLaunch.textContent = 'Launch';
      bLaunch.onclick = () => send('launch_webapp', { id: d.existing_id });
      const bDel = document.createElement('button');
      bDel.className = 'btn bad';
      bDel.textContent = 'Uninstall';
      bDel.onclick = () => send('uninstall_webapp', { id: d.existing_id });
      act.append(bLaunch, bDel);
    }
    const bCancel = document.createElement('button');
    bCancel.className = 'btn';
    bCancel.textContent = 'Cancel';
    bCancel.onclick = () => send('close_overlay');
    act.appendChild(bCancel);
    root.appendChild(act);
    setTimeout(() => { nameInp.focus(); nameInp.select(); }, 10);
  } else if (mode === 'profiles') {
    root.appendChild(renderHeader('● PROFILES & PRIVATE MODE'));
    const list = document.createElement('div');
    list.className = 'list';
    for (const p of (s.profiles || [])) {
      const row = document.createElement('div');
      row.className = 'item' + (p.active ? ' sel' : '');
      if (editingProfileId === p.id) {
        const rinp = document.createElement('input');
        rinp.className = 'form-inp';
        rinp.style.cssText = 'flex:1;padding:3px 6px;';
        rinp.value = p.name || '';
        const commitRename = () => {
          const v = rinp.value.trim();
          editingProfileId = '';
          if (v) send('rename_profile', { id: p.id, name: v });
          else window.__kajoApply(curState);
        };
        rinp.addEventListener('keydown', (e) => {
          e.stopPropagation();
          if (e.key === 'Enter') { e.preventDefault(); commitRename(); }
          else if (e.key === 'Escape') { e.preventDefault(); editingProfileId = ''; window.__kajoApply(curState); }
        });
        const bOk = document.createElement('button');
        bOk.className = 'btn primary';
        bOk.textContent = '✓';
        bOk.onclick = (e) => { e.stopPropagation(); commitRename(); };
        const bCancel = document.createElement('button');
        bCancel.className = 'btn';
        bCancel.textContent = '✕';
        bCancel.onclick = (e) => { e.stopPropagation(); editingProfileId = ''; window.__kajoApply(curState); };
        row.append(rinp, bOk, bCancel);
        setTimeout(() => { rinp.focus(); rinp.select(); }, 10);
      } else {
        const badge = document.createElement('span');
        badge.className = 'badge' + (p.active ? ' accent' : '');
        badge.textContent = p.active ? 'CURRENT' : 'LAUNCH';
        row.append(makeMainBox((p.icon || '') + ' ' + (p.name || ''), 'Profile ID: ' + (p.id || '')), badge);
        const bEdit = document.createElement('button');
        bEdit.className = 'btn';
        bEdit.textContent = '✎';
        bEdit.title = 'Rename Profile';
        bEdit.onclick = (e) => {
          e.stopPropagation();
          confirmDeleteProfileId = '';
          editingProfileId = p.id;
          window.__kajoApply(curState);
        };
        row.appendChild(bEdit);
        if (p.id !== 'default' && !p.active) {
          const bDel = document.createElement('button');
          bDel.className = 'btn bad';
          bDel.textContent = (confirmDeleteProfileId === p.id) ? 'DEL?' : '🗑';
          bDel.title = 'Delete Profile';
          bDel.onclick = (e) => {
            e.stopPropagation();
            if (confirmDeleteProfileId === p.id) {
              confirmDeleteProfileId = '';
              send('delete_profile', { id: p.id });
            } else {
              confirmDeleteProfileId = p.id;
              window.__kajoApply(curState);
            }
          };
          row.appendChild(bDel);
        }
        row.onclick = () => { if (!p.active) send('launch_profile', { id: p.id, private_mode: false }); };
      }
      list.appendChild(row);
    }
    root.appendChild(list);
    const act = document.createElement('div');
    act.className = 'act-row';
    const bPriv = document.createElement('button');
    bPriv.className = 'btn primary';
    bPriv.textContent = '🕶 Open Private Window';
    bPriv.onclick = () => send('launch_profile', { id: s.profile_id || 'default', private_mode: true });
    const inp = document.createElement('input');
    inp.placeholder = 'New profile name...';
    inp.style.cssText = 'flex:1;padding:4px 8px;background:#0d0d0d;border:1px solid var(--metro-border-strong);';
    const bAdd = document.createElement('button');
    bAdd.className = 'btn';
    bAdd.textContent = '+ Create';
    const doCreate = () => {
      if (inp.value.trim()) send('create_profile', { name: inp.value.trim() });
    };
    bAdd.onclick = doCreate;
    inp.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); doCreate(); }
    });
    act.append(bPriv, inp, bAdd);
    root.appendChild(act);
  }
};

document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') {
    e.preventDefault();
    send('close_overlay');
  }
});

send('get_state').then(s => { if (s) window.__kajoApply(s); });
</script>
</body>
</html>
)KAJO";

bool ChromeStateStore::RenderChromeAsset(const std::string& url,
                                         std::string* out_body,
                                         std::string* out_mime) const {
  std::string path, query;
  ExtractPathAndQuery(url, &path, &query);
  if (path == "ui/theme.css") {
    *out_mime = "text/css";
    *out_body = RenderThemeCss();
    return true;
  }
  if (path == "ui/topbar.html") {
    *out_mime = "text/html";
    *out_body = kTopbarHtml;
    return true;
  }
  if (path == "ui/rail.html") {
    *out_mime = "text/html";
    *out_body = kRailHtml;
    return true;
  }
  if (path == "ui/overlay.html") {
    *out_mime = "text/html";
    *out_body = kOverlayHtml;
    return true;
  }
  if (path == "ui/state.json") {
    *out_mime = "application/json";
    *out_body = SnapshotJson();
    return true;
  }
  return false;
}

static std::string BuildKajoShellPage(const ChromeStateSnapshot& s,
                                      const std::string& active_nav,
                                      const std::string& title,
                                      const std::string& body_html) {
  std::string accent = s.private_mode ? "#b146c2" :
                       (!s.profile_accent.empty() ? s.profile_accent : s.accent_color);
  accent = SanitizeHexColor(accent, "#0078d4");
  const std::string font = s.ui_font_family.empty() ? "Fira Code" : s.ui_font_family;
  std::ostringstream h;
  h << "<!doctype html><html><head><meta charset='utf-8'><title>" << HtmlEscape(title)
    << "</title><style>"
    << ":root{--accent:" << accent << ";--bg0:#111111;--bg1:#141414;--bg2:#181818;--bg3:#1e1e1e;--b:#2b2b2b;--fg:#ececec;--dim:#999999;--ok:#2ea043;--warn:#d97706;--err:#e81123;"
    << "--font:\"" << JsonEscape(font) << "\",\"Fira Code\",monospace;"
    << "--fs:" << s.ui_font_size_px << "px;"
    << "--fs-head:" << s.heading_size_px << "px;"
    << "--fs-sub:" << s.sub_size_px << "px;"
    << "--fs-badge:" << s.badge_size_px << "px;}"
    << "*{box-sizing:border-box;border-radius:0;}"
    << "html,body{margin:0;padding:0;background:var(--bg1);color:var(--fg);font-family:var(--font);font-size:var(--fs);}"
    << ".nav{display:flex;gap:4px;padding:10px 24px;background:var(--bg0);border-bottom:1px solid var(--b);align-items:center;flex-wrap:wrap;}"
    << ".brand{color:var(--accent);font-weight:700;letter-spacing:1.2px;margin-right:16px;font-size:var(--fs-head);}"
    << ".nav a{color:var(--dim);text-decoration:none;padding:5px 10px;border:1px solid transparent;font-size:var(--fs-sub);}"
    << ".nav a:hover{color:#fff;background:var(--bg2);border-color:var(--b);}"
    << ".nav a.act{color:#fff;border-color:var(--accent);background:var(--bg2);}"
    << ".wrap{max-width:1040px;margin:0 auto;padding:24px;}"
    << "h1{margin:0 0 16px 0;font-size:calc(var(--fs-head) + 5px);letter-spacing:0.8px;text-transform:uppercase;border-left:4px solid var(--accent);padding-left:10px;}"
    << "h2{margin:20px 0 10px 0;font-size:var(--fs-head);color:var(--dim);letter-spacing:0.8px;text-transform:uppercase;}"
    << ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:10px;}"
    << ".card{background:var(--bg2);border:1px solid var(--b);padding:12px;display:block;color:inherit;text-decoration:none;}"
    << ".card:hover{border-color:var(--accent);background:var(--bg3);}"
    << ".k{font-size:calc(var(--fs) + 6px);font-weight:700;color:#fff;}"
    << ".lbl{font-size:var(--fs-badge);color:var(--dim);margin-top:4px;text-transform:uppercase;}"
    << "table{width:100%;border-collapse:collapse;background:var(--bg2);border:1px solid var(--b);}"
    << "th,td{padding:8px 10px;border-bottom:1px solid var(--b);text-align:left;vertical-align:middle;}"
    << "th{background:var(--bg0);color:var(--dim);font-size:var(--fs-badge);text-transform:uppercase;}"
    << ".btn{display:inline-block;padding:6px 12px;background:var(--bg3);border:1px solid #3c3c3c;color:#fff;text-decoration:none;cursor:pointer;font-family:inherit;font-size:var(--fs-sub);}"
    << ".btn:hover{border-color:var(--accent);}"
    << ".btn.pri{background:var(--accent);border-color:var(--accent);}"
    << ".btn.bad:hover{background:var(--err);border-color:var(--err);}"
    << "input[type=text]{padding:7px 10px;background:#0c0c0c;border:1px solid #3c3c3c;color:#fff;font-family:inherit;font-size:var(--fs);}"
    << "input[type=text]:focus{border-color:var(--accent);outline:none;}"
    << "</style></head><body>"
    << "<div class='nav'><span class='brand'>❖ KAJO // WEB</span>"
    << "<a href='kajo://newtab' class='" << (active_nav == "newtab" ? "act" : "") << "'>NEWTAB</a>"
    << "<a href='kajo://bookmarks' class='" << (active_nav == "bookmarks" ? "act" : "") << "'>BOOKMARKS</a>"
    << "<a href='kajo://history' class='" << (active_nav == "history" ? "act" : "") << "'>HISTORY</a>"
    << "<a href='kajo://adblock' class='" << (active_nav == "adblock" ? "act" : "") << "'>ADBLOCK</a>"
    << "<a href='kajo://permissions' class='" << (active_nav == "permissions" ? "act" : "") << "'>PERMISSIONS</a>"
    << "<a href='kajo://apps' class='" << (active_nav == "apps" ? "act" : "") << "'>APPS</a>"
    << "<a href='kajo://downloads' class='" << (active_nav == "downloads" ? "act" : "") << "'>DOWNLOADS</a>"
    << "<a href='kajo://gpu' class='" << (active_nav == "gpu" ? "act" : "") << "'>GPU &amp; VA-API</a>"
    << "<a href='kajo://about' class='" << (active_nav == "about" ? "act" : "") << "'>ABOUT</a>"
    << "</div><div class='wrap'>" << body_html << "</div></body></html>";
  return h.str();
}

bool ChromeStateStore::RenderKajoPage(const std::string& url,
                                      std::string* out_body,
                                      std::string* out_mime) const {
  ChromeStateSnapshot s = Snapshot();
  std::string page, query;
  ExtractPathAndQuery(url, &page, &query);
  if (page.empty()) page = "newtab";
  *out_mime = "text/html";

  if (page == "newtab") {
    std::ostringstream b;
    b << "<h1>" << (s.private_mode ? "PRIVATE SESSION" : "KAJO WORKBENCH") << "</h1>"
      << "<form action='kajo://newtab' method='get' style='display:flex;gap:6px;margin-bottom:18px;'>"
      << "<input type='hidden' name='action' value='search'>"
      << "<input type='text' name='q' placeholder='Search the web or open URL...' style='flex:1;' autofocus>"
      << "<button class='btn pri' type='submit'>GO</button></form>"
      << "<div class='grid' style='margin-bottom:18px;'>"
      << "<div class='card'><div class='k'>" << s.total_blocked << "</div><div class='lbl'>TRACKERS BLOCKED</div></div>"
      << "<div class='card'><div class='k'>" << s.adblock_rules << "</div><div class='lbl'>ACTIVE FILTER RULES</div></div>"
      << "<div class='card'><div class='k'>" << s.bookmarks.size() << "</div><div class='lbl'>BOOKMARKS</div></div>"
      << "<div class='card'><div class='k'>" << (s.hw_accel_enabled ? "VA-API" : "CPU") << "</div><div class='lbl'>VIDEO ENGINE</div></div>"
      << "</div><h2>PINNED BOOKMARKS</h2><div class='grid'>";
    size_t count = std::min<size_t>(s.bookmarks.size(), 8);
    for (size_t i = 0; i < count; ++i) {
      const auto& bm = s.bookmarks[i];
      b << "<a class='card' href='" << HtmlEscape(SanitizeSafeHref(bm.url)) << "'><div>"
        << HtmlEscape(bm.title.empty() ? bm.url : bm.title)
        << "</div><div class='lbl'>" << HtmlEscape(bm.url) << "</div></a>";
    }
    if (count == 0) {
      b << "<div class='card'><div class='lbl'>No bookmarks yet — press Ctrl+D or ☆ to save a page</div></div>";
    }
    b << "</div><h2>RECENT VISITS</h2><table><tr><th>Title</th><th>URL</th><th>Visits</th></tr>";
    size_t hcnt = std::min<size_t>(s.recent_history.size(), 8);
    for (size_t i = 0; i < hcnt; ++i) {
      const auto& he = s.recent_history[i];
      b << "<tr><td><a style='color:#fff;text-decoration:none;' href='" << HtmlEscape(SanitizeSafeHref(he.url)) << "'>"
        << HtmlEscape(he.title.empty() ? he.url : he.title) << "</a></td><td style='color:var(--dim);'>"
        << HtmlEscape(he.url) << "</td><td>" << he.visit_count << "</td></tr>";
    }
    b << "</table>";
    *out_body = BuildKajoShellPage(s, "newtab", "Kajo — New Tab", b.str());
    return true;
  }

  if (page == "bookmarks") {
    std::ostringstream b;
    b << "<h1>BOOKMARKS (" << s.bookmarks.size() << ")</h1>"
      << "<table><tr><th>Title</th><th>Folder</th><th>URL</th><th>Actions</th></tr>";
    for (const auto& bm : s.bookmarks) {
      b << "<tr><td><a style='color:#fff;' href='" << HtmlEscape(SanitizeSafeHref(bm.url)) << "'>"
        << HtmlEscape(bm.title) << "</a></td><td>" << HtmlEscape(bm.folder) << "</td><td style='color:var(--dim);'>"
        << HtmlEscape(bm.url) << "</td><td><a class='btn bad' href='kajo://bookmarks?action=delete&uuid="
        << UrlEncodeQuery(bm.uuid) << "'>DELETE</a></td></tr>";
    }
    b << "</table>";
    *out_body = BuildKajoShellPage(s, "bookmarks", "Kajo — Bookmarks", b.str());
    return true;
  }

  if (page == "history") {
    const auto& list = s.history_search_query.empty() ? s.recent_history : s.history_query_results;
    std::ostringstream b;
    b << "<h1>HISTORY</h1>"
      << "<div style='display:flex;gap:8px;margin-bottom:14px;'>"
      << "<form action='kajo://history' method='get' style='display:flex;gap:6px;flex:1;'>"
      << "<input type='hidden' name='action' value='search'>"
      << "<input type='text' name='q' value='" << HtmlEscape(s.history_search_query) << "' placeholder='Filter history...' style='flex:1;'>"
      << "<button class='btn pri' type='submit'>SEARCH</button></form>"
      << "<a class='btn bad' href='kajo://history?action=clear'>CLEAR ALL HISTORY</a></div>"
      << "<table><tr><th>Title</th><th>URL</th><th>Visits</th><th>Remove</th></tr>";
    for (const auto& he : list) {
      b << "<tr><td><a style='color:#fff;' href='" << HtmlEscape(SanitizeSafeHref(he.url)) << "'>"
        << HtmlEscape(he.title.empty() ? he.url : he.title) << "</a></td><td style='color:var(--dim);'>"
        << HtmlEscape(he.url) << "</td><td>" << he.visit_count << "</td><td>"
        << "<a class='btn bad' href='kajo://history?action=delete&uri=" << UrlEncodeQuery(he.url) << "'>×</a></td></tr>";
    }
    b << "</table>";
    *out_body = BuildKajoShellPage(s, "history", "Kajo — History", b.str());
    return true;
  }

  if (page == "adblock") {
    std::ostringstream b;
    b << "<h1>ADBLOCK &amp; COSMETIC ENGINE</h1>"
      << "<div style='display:flex;gap:8px;margin-bottom:16px;'>"
      << "<a class='btn " << (s.adblock_enabled ? "pri" : "") << "' href='kajo://adblock?action=toggle_global'>"
      << (s.adblock_enabled ? "ENGINE: ENABLED (CLICK TO DISABLE)" : "ENGINE: DISABLED (CLICK TO ENABLE)") << "</a>"
      << "<a class='btn' href='kajo://adblock?action=update_lists'>"
      << (s.filter_update_busy ? "UPDATING LISTS..." : "UPDATE FILTER LISTS NOW") << "</a></div>";
    if (!s.filter_update_status.empty()) {
      b << "<div class='card' style='margin-bottom:14px;'>STATUS: " << HtmlEscape(s.filter_update_status) << "</div>";
    }
    b << "<div class='grid' style='margin-bottom:16px;'>"
      << "<div class='card'><div class='k'>" << s.adblock_rules << "</div><div class='lbl'>TOTAL RULES LOADED</div></div>"
      << "<div class='card'><div class='k'>" << s.total_blocked << "</div><div class='lbl'>SESSION REQUESTS BLOCKED</div></div>"
      << "<div class='card'><div class='k'>" << s.allowlist_hosts.size() << "</div><div class='lbl'>PAUSED HOSTS</div></div></div>"
      << "<h2>FILTER LISTS</h2><table><tr><th>List</th><th>Rules</th><th>State</th></tr>"
      << "<tr><td>Built-in Baseline (Ads &amp; YouTube Telemetry)<br><span style='color:var(--dim);font-size:var(--fs-badge);'>bundled offline fallback</span></td><td>"
      << s.baseline_rules << "</td><td>ACTIVE</td></tr>";
    for (const auto& fl : s.filter_lists) {
      const char* state_lbl = (fl.rule_count > 0)
                                  ? "LOADED"
                                  : (s.filter_update_busy ? "DOWNLOADING..." : "NOT LOADED (0)");
      b << "<tr><td>" << HtmlEscape(fl.name) << "<br><span style='color:var(--dim);font-size:var(--fs-badge);'>"
        << HtmlEscape(fl.url) << "</span></td><td>" << fl.rule_count << "</td><td>"
        << state_lbl << "</td></tr>";
    }
    b << "</table><h2>PAUSED / ALLOWLISTED HOSTS</h2><table><tr><th>Host</th><th>Action</th></tr>";
    for (const auto& h : s.allowlist_hosts) {
      b << "<tr><td>" << HtmlEscape(h) << "</td><td><a class='btn' href='kajo://adblock?action=unpause&host="
        << UrlEncodeQuery(h) << "'>RESUME BLOCKING</a></td></tr>";
    }
    if (s.allowlist_hosts.empty()) {
      b << "<tr><td colspan='2' style='color:var(--dim);'>No paused hosts</td></tr>";
    }
    b << "</table>";
    *out_body = BuildKajoShellPage(s, "adblock", "Kajo — Adblock", b.str());
    return true;
  }

  if (page == "downloads") {
    auto contains_ci = [](const std::string& hay, const std::string& needle) -> bool {
      if (needle.empty()) return true;
      auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
      std::string hl(hay.size(), '\0'), nl(needle.size(), '\0');
      std::transform(hay.begin(), hay.end(), hl.begin(), lower);
      std::transform(needle.begin(), needle.end(), nl.begin(), lower);
      return hl.find(nl) != std::string::npos;
    };
    size_t active_cnt = 0, done_cnt = 0;
    for (const auto& d : s.downloads) {
      if (d.in_progress) ++active_cnt;
      else if (d.complete) ++done_cnt;
    }
    std::ostringstream b;
    b << "<h1>DOWNLOADS</h1>"
      << "<div class='grid' style='margin-bottom:14px;'>"
      << "<div class='card'><div id='dlStatTotal' class='k'>" << s.downloads.size() << "</div><div class='lbl'>TOTAL ITEMS</div></div>"
      << "<div class='card'><div id='dlStatActive' class='k'>" << active_cnt << "</div><div class='lbl'>ACTIVE TRANSFERS</div></div>"
      << "<div class='card'><div id='dlStatDone' class='k'>" << done_cnt << "</div><div class='lbl'>COMPLETED</div></div>"
      << "<div class='card'><div class='k' style='font-size:var(--fs-head);word-break:break-all;'>"
      << HtmlEscape(s.download_dir.empty() ? "~/Downloads" : s.download_dir) << "</div><div class='lbl'>SAVE DIRECTORY</div></div>"
      << "</div>"
      << "<div style='display:flex;gap:8px;margin-bottom:14px;flex-wrap:wrap;align-items:center;'>"
      << "<form action='kajo://downloads' method='get' style='display:flex;gap:6px;flex:1;min-width:220px;'>"
      << "<input type='hidden' name='action' value='search'>"
      << "<input type='text' name='q' value='" << HtmlEscape(s.downloads_search_query) << "' placeholder='Filter downloads by file or URL...' style='flex:1;'>"
      << "<button class='btn pri' type='submit'>FILTER</button></form>"
      << "<a class='btn " << (s.downloads_filter.empty() ? "pri" : "") << "' href='kajo://downloads?action=filter&f=all'>ALL</a>"
      << "<a class='btn " << (s.downloads_filter == "active" ? "pri" : "") << "' href='kajo://downloads?action=filter&f=active'>ACTIVE</a>"
      << "<a class='btn " << (s.downloads_filter == "completed" ? "pri" : "") << "' href='kajo://downloads?action=filter&f=completed'>COMPLETED</a>"
      << "<a class='btn' href='kajo://downloads?action=open_folder'>📁 OPEN FOLDER</a>"
      << "<a class='btn bad' href='kajo://downloads?action=clear_done'>CLEAR FINISHED</a>"
      << "</div>"
      << "<div style='border:1px solid var(--b);background:var(--bg2);'>"
      << "<table style='border:none;'><thead><tr><th>File</th><th>Progress</th><th>Size / Speed</th><th>Actions</th></tr></thead>"
      << "<tbody id='dlBody'>";
    size_t shown = 0;
    for (const auto& d : s.downloads) {
      if (s.downloads_filter == "active" && !d.in_progress) continue;
      if (s.downloads_filter == "completed" && !d.complete) continue;
      if (!s.downloads_search_query.empty() &&
          !contains_ci(d.filename, s.downloads_search_query) &&
          !contains_ci(d.url, s.downloads_search_query)) {
        continue;
      }
      ++shown;
      const int pct = d.complete ? 100 : std::clamp(d.percent, 0, 100);
      const char* bar_col = d.complete ? "var(--ok)" : (d.canceled ? "var(--err)" : (d.paused ? "var(--warn)" : "var(--accent)"));
      const std::string st_txt = d.complete ? "DONE" : (d.canceled ? "CANCELED" : (d.paused ? "PAUSED" : (d.percent >= 0 ? std::to_string(d.percent) + "%" : "ACTIVE")));
      b << "<tr>"
        << "<td><div style='color:#fff;font-weight:600;'>" << HtmlEscape(d.filename.empty() ? "download" : d.filename)
        << "</div><div style='color:var(--dim);font-size:var(--fs-badge);word-break:break-all;'>"
        << HtmlEscape(d.full_path.empty() ? d.url : d.full_path) << "</div></td>"
        << "<td style='min-width:150px;'><div>" << st_txt << "</div>"
        << "<div style='height:4px;background:var(--bg0);border:1px solid var(--b);margin-top:4px;'>"
        << "<div style='height:100%;width:" << pct << "%;background:" << bar_col << ";'></div></div></td>"
        << "<td>" << FormatBytes(d.received_bytes)
        << (d.total_bytes > 0 ? (" / " + FormatBytes(d.total_bytes)) : "")
        << (d.in_progress && !d.paused && d.current_speed > 0 ? ("<div style='color:var(--dim);font-size:var(--fs-badge);'>" + FormatBytes(d.current_speed) + "/s</div>") : "")
        << "</td><td style='white-space:nowrap;'>";
      if (d.in_progress) {
        if (d.paused) {
          b << "<a class='btn pri' href='kajo://downloads?action=resume&id=" << d.id << "'>▶ RESUME</a> ";
        } else {
          b << "<a class='btn' href='kajo://downloads?action=pause&id=" << d.id << "'>⏸ PAUSE</a> ";
        }
        b << "<a class='btn bad' href='kajo://downloads?action=cancel&id=" << d.id << "'>✕ CANCEL</a>";
      } else if (d.complete) {
        b << "<a class='btn pri' href='kajo://downloads?action=open_file&path=" << UrlEncodeQuery(d.full_path) << "'>OPEN</a> "
          << "<a class='btn' href='kajo://downloads?action=open_folder&path=" << UrlEncodeQuery(d.full_path) << "'>📁</a> "
          << "<a class='btn bad' href='kajo://downloads?action=remove&id=" << d.id << "&db_id=" << d.db_id << "'>×</a>";
      } else {
        if (!d.url.empty()) {
          b << "<a class='btn pri' href='kajo://downloads?action=retry&url=" << UrlEncodeQuery(d.url) << "'>↻ RETRY</a> ";
        }
        b << "<a class='btn bad' href='kajo://downloads?action=remove&id=" << d.id << "&db_id=" << d.db_id << "'>×</a>";
      }
      b << "</td></tr>";
    }
    if (shown == 0) {
      b << "<tr><td colspan='4' style='color:var(--dim);padding:16px;'>No matching downloads</td></tr>";
    }
    b << "</tbody></table></div>"
      << "<script>"
      << "window.__kajoDlUpdate = (s) => {"
      << "  const dls = s.downloads || [];"
      << "  const tEl = document.getElementById('dlStatTotal');"
      << "  const aEl = document.getElementById('dlStatActive');"
      << "  const cEl = document.getElementById('dlStatDone');"
      << "  if (tEl) tEl.textContent = String(dls.length);"
      << "  if (aEl) aEl.textContent = String(dls.filter(d => d.in_progress).length);"
      << "  if (cEl) cEl.textContent = String(dls.filter(d => d.complete).length);"
      << "  const tb = document.getElementById('dlBody'); if (!tb) return;"
      << "  const fmt = (b) => { if (!b || b <= 0) return '0 B'; if (b >= 1073741824) return (b/1073741824).toFixed(2)+' GB'; if (b >= 1048576) return (b/1048576).toFixed(1)+' MB'; if (b >= 1024) return (b/1024).toFixed(1)+' KB'; return b+' B'; };"
      << "  const f = s.downloads_filter || ''; const q = (s.downloads_search_query || '').toLowerCase();"
      << "  const vis = dls.filter(d => (!f || (f==='active' && d.in_progress) || (f==='completed' && d.complete)) && (!q || (d.filename||'').toLowerCase().includes(q) || (d.url||'').toLowerCase().includes(q)));"
      << "  tb.innerHTML = '';"
      << "  if (!vis.length) { const tr = document.createElement('tr'); const td = document.createElement('td'); td.colSpan = 4; td.style.cssText = 'color:var(--dim);padding:16px;'; td.textContent = 'No matching downloads'; tr.appendChild(td); tb.appendChild(tr); return; }"
      << "  for (const d of vis) {"
      << "    const tr = document.createElement('tr');"
      << "    const td1 = document.createElement('td');"
      << "    const fn = document.createElement('div'); fn.style.cssText = 'color:#fff;font-weight:600;'; fn.textContent = d.filename || 'download';"
      << "    const fp = document.createElement('div'); fp.style.cssText = 'color:var(--dim);font-size:var(--fs-badge);word-break:break-all;'; fp.textContent = d.full_path || d.url || '';"
      << "    td1.append(fn, fp);"
      << "    const td2 = document.createElement('td'); td2.style.minWidth = '150px';"
      << "    const st = d.complete ? 'DONE' : (d.canceled ? 'CANCELED' : (d.paused ? 'PAUSED' : (d.percent >= 0 ? d.percent + '%' : 'ACTIVE')));"
      << "    const stDiv = document.createElement('div'); stDiv.textContent = st;"
      << "    const pb = document.createElement('div'); pb.style.cssText = 'height:4px;background:var(--bg0);border:1px solid var(--b);margin-top:4px;';"
      << "    const pbf = document.createElement('div'); const pct = d.complete ? 100 : Math.max(0, Math.min(100, d.percent || 0));"
      << "    pbf.style.cssText = 'height:100%;width:' + pct + '%;background:' + (d.complete ? 'var(--ok)' : (d.canceled ? 'var(--err)' : (d.paused ? 'var(--warn)' : 'var(--accent)'))) + ';';"
      << "    pb.appendChild(pbf); td2.append(stDiv, pb);"
      << "    const td3 = document.createElement('td'); td3.textContent = fmt(d.received) + (d.total > 0 ? (' / ' + fmt(d.total)) : '');"
      << "    if (d.in_progress && !d.paused && d.speed > 0) { const sp = document.createElement('div'); sp.style.cssText = 'color:var(--dim);font-size:var(--fs-badge);'; sp.textContent = fmt(d.speed) + '/s'; td3.appendChild(sp); }"
      << "    const td4 = document.createElement('td'); td4.style.whiteSpace = 'nowrap';"
      << "    const mkA = (cls, txt, href) => { const a = document.createElement('a'); a.className = 'btn ' + cls; a.textContent = txt; a.href = href; a.style.marginRight = '4px'; return a; };"
      << "    if (d.in_progress) { td4.appendChild(d.paused ? mkA('pri','▶ RESUME','kajo://downloads?action=resume&id='+d.id) : mkA('','⏸ PAUSE','kajo://downloads?action=pause&id='+d.id)); td4.appendChild(mkA('bad','✕ CANCEL','kajo://downloads?action=cancel&id='+d.id)); }"
      << "    else if (d.complete) { td4.appendChild(mkA('pri','OPEN','kajo://downloads?action=open_file&path='+encodeURIComponent(d.full_path||''))); td4.appendChild(mkA('','📁','kajo://downloads?action=open_folder&path='+encodeURIComponent(d.full_path||''))); td4.appendChild(mkA('bad','×','kajo://downloads?action=remove&id='+d.id+'&db_id='+(d.db_id||0))); }"
      << "    else { if (d.url) td4.appendChild(mkA('pri','↻ RETRY','kajo://downloads?action=retry&url='+encodeURIComponent(d.url))); td4.appendChild(mkA('bad','×','kajo://downloads?action=remove&id='+d.id+'&db_id='+(d.db_id||0))); }"
      << "    tr.append(td1, td2, td3, td4); tb.appendChild(tr);"
      << "  }"
      << "};"
      << "</script>";
    *out_body = BuildKajoShellPage(s, "downloads", "Kajo — Downloads", b.str());
    return true;
  }

  if (page == "permissions") {
    std::ostringstream b;
    b << "<h1>SITE PERMISSIONS (" << s.saved_permissions.size() << ")</h1>"
      << "<div style='margin-bottom:14px;display:flex;gap:8px;align-items:center;'>"
      << "<a class='btn bad' href='kajo://permissions?action=clear_all'>CLEAR ALL PERMISSIONS</a>"
      << "<span style='color:var(--dim);font-size:var(--fs-sub);'>Stored in current profile permissions.json (private sessions never persist)</span>"
      << "</div>"
      << "<table><tr><th>Origin</th><th>Capability</th><th>Decision</th><th>Action</th></tr>";
    for (const auto& sp : s.saved_permissions) {
      b << "<tr><td style='color:#fff;'>" << HtmlEscape(sp.origin) << "</td>"
        << "<td>" << HtmlEscape(sp.label) << "</td>"
        << "<td style='color:" << (sp.allow ? "var(--ok)" : "var(--err)") << ";font-weight:700;'>"
        << (sp.allow ? "ALLOW" : "DENY") << "</td>"
        << "<td><a class='btn bad' href='kajo://permissions?action=revoke&origin="
        << UrlEncodeQuery(sp.origin) << "&key=" << UrlEncodeQuery(sp.perm_key) << "'>REVOKE</a></td></tr>";
    }
    if (s.saved_permissions.empty()) {
      b << "<tr><td colspan='4' style='color:var(--dim);padding:16px;'>No saved site permission grants or denials</td></tr>";
    }
    b << "</table>";
    *out_body = BuildKajoShellPage(s, "permissions", "Kajo — Permissions", b.str());
    return true;
  }

  if (page == "apps") {
    std::ostringstream b;
    b << "<h1>INSTALLED WEB APPS (" << s.webapps.size() << ")</h1>"
      << "<p style='color:var(--dim);font-size:var(--fs-sub);margin:0 0 14px 0;'>"
      << "Web Apps share this profile's cookies, login sessions, adblock &amp; permissions while running in dedicated chromeless Wayland windows and registering into ~/.local/share/applications/."
      << "</p>"
      << "<form action='kajo://apps' method='get' class='card' style='display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:18px;'>"
      << "<input type='hidden' name='action' value='install'>"
      << "<input type='text' name='url' placeholder='App URL (e.g. https://music.apple.com)' style='flex:2;min-width:240px;' required>"
      << "<input type='text' name='name' placeholder='App Name (optional)' style='flex:1;min-width:140px;'>"
      << "<input type='text' name='theme_color' placeholder='#0078d4' value='#0078d4' style='width:96px;'>"
      << "<button class='btn pri' type='submit'>⊞ INSTALL WEB APP</button></form>"
      << "<table><tr><th>App</th><th>Start URL &amp; Scope</th><th>Accent</th><th>Desktop ID</th><th>Actions</th></tr>";
    for (const auto& wa : s.webapps) {
      b << "<tr>"
        << "<td style='color:#fff;font-weight:700;'>" << HtmlEscape(wa.name) << "</td>"
        << "<td style='word-break:break-all;'><div><a href='" << HtmlEscape(wa.start_url)
        << "'>" << HtmlEscape(wa.start_url) << "</a></div>"
        << "<div style='color:var(--dim);font-size:var(--fs-badge);'>Scope: "
        << HtmlEscape(wa.scope) << "</div></td>"
        << "<td><span style='display:inline-block;width:12px;height:12px;vertical-align:middle;margin-right:6px;background:"
        << HtmlEscape(SanitizeHexColor(wa.theme_color, "#0078d4")) << ";'></span>"
        << HtmlEscape(wa.theme_color) << "</td>"
        << "<td style='color:var(--dim);font-size:var(--fs-badge);'>"
        << HtmlEscape(wa.wayland_app_id) << "</td>"
        << "<td style='white-space:nowrap;'>"
        << "<a class='btn pri' href='kajo://apps?action=launch&id="
        << UrlEncodeQuery(wa.id) << "'>▶ LAUNCH</a> "
        << "<a class='btn bad' href='kajo://apps?action=uninstall&id="
        << UrlEncodeQuery(wa.id) << "'>✕ UNINSTALL</a></td></tr>";
    }
    if (s.webapps.empty()) {
      b << "<tr><td colspan='5' style='color:var(--dim);padding:16px;'>"
        << "No Web Apps installed in this profile yet. Click ⊞ in the address bar on any site or use the form above."
        << "</td></tr>";
    }
    b << "</table>";
    *out_body = BuildKajoShellPage(s, "apps", "Kajo — Web Apps", b.str());
    return true;
  }

  if (page == "crashed") {
    auto get_q = [&](const char* k) -> std::string {
      std::string key = std::string(k) + "=";
      size_t pos = 0;
      while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = (amp == std::string::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
        if (pair.rfind(key, 0) == 0) {
          std::string raw = pair.substr(key.size());
          std::string dec;
          for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '+') dec.push_back(' ');
            else if (raw[i] == '%' && i + 2 < raw.size()) {
              char hex[3] = {raw[i + 1], raw[i + 2], 0};
              char* endp = nullptr;
              long v = strtol(hex, &endp, 16);
              if (endp == hex + 2) {
                dec.push_back(static_cast<char>(v));
                i += 2;
                continue;
              }
            } else {
              dec.push_back(raw[i]);
            }
          }
          return dec;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
      }
      return "";
    };
    const std::string reason = get_q("reason");
    const std::string crashed_url = get_q("url");
    std::ostringstream b;
    b << "<div class='card' style='border-left:4px solid var(--err);padding:20px;'>"
      << "<h1 style='border-left-color:var(--err);'>TAB RENDERER TERMINATED</h1>"
      << "<p style='color:var(--dim);margin:8px 0;'>Reason: <strong style='color:#fff;'>"
      << HtmlEscape(reason.empty() ? "Renderer exited unexpectedly" : reason) << "</strong></p>"
      << "<p style='color:var(--dim);word-break:break-all;margin:8px 0 18px 0;'>URL: "
      << HtmlEscape(crashed_url.empty() ? s.active_url : crashed_url) << "</p>"
      << "<div style='display:flex;gap:8px;'>"
      << "<a class='btn pri' href='kajo://crashed?action=reload'>↻ RELOAD TAB</a>"
      << "<a class='btn bad' href='kajo://crashed?action=close'>CLOSE TAB</a>"
      << "</div></div>";
    *out_body = BuildKajoShellPage(s, "", "Kajo — Tab Crashed", b.str());
    return true;
  }

  if (page == "gpu") {
    std::ostringstream b;
    b << "<h1>GPU &amp; HARDWARE VIDEO PIPELINE</h1>"
      << "<div style='margin-bottom:16px;'><a class='btn pri' href='kajo://gpu?action=open_chrome_gpu'>OPEN RAW CHROME://GPU DIAGNOSTICS</a></div>"
      << "<table><tr><th>Subsystem</th><th>Configured State</th></tr>"
      << "<tr><td>Video Hardware Acceleration</td><td>" << (s.hw_accel_enabled ? "VA-API Zero-Copy (GL/EGL ANGLE · Vulkan Disabled)" : "Disabled (Software Compositing)") << "</td></tr>"
      << "<tr><td>Widevine CDM Hint</td><td>" << (s.widevine_available ? HtmlEscape(s.widevine_path) : "Not found on system") << "</td></tr>"
      << "<tr><td>Native Wayland Ozone</td><td>Active (CEF Views Alloy Frameless Window)</td></tr>"
      << "<tr><td>User Agent</td><td style='color:var(--dim);'>" << HtmlEscape(s.user_agent) << "</td></tr>"
      << "</table>";
    *out_body = BuildKajoShellPage(s, "gpu", "Kajo — GPU & VA-API", b.str());
    return true;
  }

  /* Default: kajo://about */
  std::ostringstream b;
  b << "<h1>ABOUT KAJO WEB</h1>"
    << "<table><tr><th>Component</th><th>Details</th></tr>"
    << "<tr><td>Application</td><td>kajo-web (Aero+Metro Chromium/CEF Alloy Views + Native Wayland)</td></tr>"
    << "<tr><td>Profile</td><td>" << HtmlEscape(s.profile_icon + " " + s.profile_name) << (s.private_mode ? " (Private)" : "") << "</td></tr>"
    << "<tr><td>Adblocker</td><td>IO-thread native ABP + Hosts + Cosmetic + YouTube Skip (" << s.adblock_rules << " rules)</td></tr>"
    << "<tr><td>DRM / Media</td><td>" << (s.widevine_available ? "Widevine CDM detected" : "Widevine CDM absent") << " + kajo-player handoff</td></tr>"
    << "</table>";
  *out_body = BuildKajoShellPage(s, "about", "Kajo — About", b.str());
  return true;
}

class MemoryResourceHandler : public CefResourceHandler {
public:
  MemoryResourceHandler(std::shared_ptr<ChromeStateStore> store, bool is_chrome_ui)
      : store_(std::move(store)), is_chrome_ui_(is_chrome_ui) {}

  bool Open(CefRefPtr<CefRequest> request,
            bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    handle_request = true;
    const std::string url = request->GetURL().ToString();
    if (!store_) {
      status_code_ = 503;
      mime_ = "text/plain";
      data_ = "Unavailable";
      offset_ = 0;
      return true;
    }
    bool ok = is_chrome_ui_
                  ? store_->RenderChromeAsset(url, &data_, &mime_)
                  : store_->RenderKajoPage(url, &data_, &mime_);
    if (!ok) {
      status_code_ = 404;
      mime_ = "text/plain";
      data_ = "Not found: " + url;
    } else {
      status_code_ = 200;
    }
    offset_ = 0;
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirectUrl) override {
    response->SetStatus(status_code_);
    response->SetMimeType(mime_);
    response->SetCharset("utf-8");
    CefResponse::HeaderMap hdrs;
    hdrs.emplace("Cache-Control", "no-store");
    hdrs.emplace("X-Content-Type-Options", "nosniff");
    if (is_chrome_ui_) {
      hdrs.emplace("Content-Security-Policy",
                   "default-src 'self' 'unsafe-inline'; "
                   "img-src * data: blob:; "
                   "style-src 'self' 'unsafe-inline'; "
                   "script-src 'self' 'unsafe-inline';");
    } else {
      hdrs.emplace("Content-Security-Policy",
                   "default-src 'none'; "
                   "style-src 'unsafe-inline'; "
                   "script-src 'unsafe-inline'; "
                   "img-src data:; "
                   "form-action kajo:;");
    }
    response->SetHeaderMap(hdrs);
    response_length = static_cast<int64_t>(data_.size());
  }

  bool Read(void* data_out,
            int bytes_to_read,
            int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    if (offset_ >= data_.size()) {
      bytes_read = 0;
      return false;
    }
    size_t avail = data_.size() - offset_;
    size_t take = std::min(avail, static_cast<size_t>(bytes_to_read));
    memcpy(data_out, data_.data() + offset_, take);
    offset_ += take;
    bytes_read = static_cast<int>(take);
    return true;
  }

  void Cancel() override {}

private:
  std::shared_ptr<ChromeStateStore> store_;
  bool is_chrome_ui_;
  std::string data_;
  std::string mime_ = "text/html";
  int status_code_ = 200;
  size_t offset_ = 0;

  IMPLEMENT_REFCOUNTING(MemoryResourceHandler);
};

class KajoSchemeHandlerFactory : public CefSchemeHandlerFactory {
public:
  KajoSchemeHandlerFactory(std::shared_ptr<ChromeStateStore> store, bool is_chrome_ui)
      : store_(std::move(store)), is_chrome_ui_(is_chrome_ui) {}

  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    return new MemoryResourceHandler(store_, is_chrome_ui_);
  }

private:
  std::shared_ptr<ChromeStateStore> store_;
  bool is_chrome_ui_;

  IMPLEMENT_REFCOUNTING(KajoSchemeHandlerFactory);
};

void RegisterKajoSchemeHandlers(CefRefPtr<CefRequestContext> ctx,
                                std::shared_ptr<ChromeStateStore> store) {
  if (!ctx || !store) return;
  ctx->RegisterSchemeHandlerFactory("kajo-chrome", "", new KajoSchemeHandlerFactory(store, true));
  ctx->RegisterSchemeHandlerFactory("kajo", "", new KajoSchemeHandlerFactory(store, false));
}

void RegisterKajoCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  registrar->AddCustomScheme(
      "kajo-chrome",
      CEF_SCHEME_OPTION_STANDARD |
      CEF_SCHEME_OPTION_SECURE |
      CEF_SCHEME_OPTION_CORS_ENABLED |
      CEF_SCHEME_OPTION_FETCH_ENABLED |
      CEF_SCHEME_OPTION_DISPLAY_ISOLATED);
  registrar->AddCustomScheme(
      "kajo",
      CEF_SCHEME_OPTION_STANDARD |
      CEF_SCHEME_OPTION_SECURE |
      CEF_SCHEME_OPTION_DISPLAY_ISOLATED);
}

}  // namespace kajo_web
