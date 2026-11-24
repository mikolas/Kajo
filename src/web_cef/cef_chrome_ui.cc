#include "web_cef/cef_chrome_ui.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "include/wrapper/cef_helpers.h"

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
  if (accent.empty()) accent = "#0078d4";
  std::string font = s.ui_font_family.empty() ? "Fira Code" : s.ui_font_family;

  std::ostringstream css;
  css << ":root {\n"
      << "  --metro-accent: " << accent << ";\n"
      << "  --metro-theme-accent: " << s.accent_color << ";\n"
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
      << "  --metro-badge-size: " << s.badge_size_px << "px;\n"
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
  o << "\"show_tab_domains\":" << (s.show_tab_domains ? "true" : "false") << ",";
  o << "\"ac_visible\":" << (s.ac_visible ? "true" : "false") << ",";
  o << "\"ac_selected_idx\":" << s.ac_selected_idx << ",";
  o << "\"overlay_mode\":\"" << JsonEscape(s.overlay_mode) << "\",";
  o << "\"accent_color\":\"" << JsonEscape(s.accent_color) << "\",";
  o << "\"profile_accent\":\"" << JsonEscape(s.profile_accent) << "\",";
  o << "\"profile_id\":\"" << JsonEscape(s.profile_id) << "\",";
  o << "\"profile_name\":\"" << JsonEscape(s.profile_name) << "\",";
  o << "\"profile_icon\":\"" << JsonEscape(s.profile_icon) << "\",";
  o << "\"active_tab_id\":" << s.active_tab_id << ",";
  o << "\"active_url\":\"" << JsonEscape(s.active_url) << "\",";
  o << "\"active_title\":\"" << JsonEscape(s.active_title) << "\",";
  o << "\"active_host\":\"" << JsonEscape(s.active_host) << "\",";
  o << "\"can_go_back\":" << (s.can_go_back ? "true" : "false") << ",";
  o << "\"can_go_forward\":" << (s.can_go_forward ? "true" : "false") << ",";
  o << "\"is_loading\":" << (s.is_loading ? "true" : "false") << ",";
  o << "\"is_bookmarked\":" << (s.is_bookmarked ? "true" : "false") << ",";
  o << "\"zoom_pct\":" << s.zoom_pct << ",";
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

  o << "\"downloads\":[";
  for (size_t i = 0; i < s.downloads.size(); ++i) {
    const auto& d = s.downloads[i];
    if (i) o << ",";
    o << "{\"id\":" << d.id
      << ",\"filename\":\"" << JsonEscape(d.filename) << "\""
      << ",\"url\":\"" << JsonEscape(d.url) << "\""
      << ",\"full_path\":\"" << JsonEscape(d.full_path) << "\""
      << ",\"percent\":" << d.percent
      << ",\"received\":" << d.received_bytes
      << ",\"total\":" << d.total_bytes
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
    height: 34px;
    overflow: hidden;
    background: var(--metro-surface-0);
    border-bottom: 1px solid var(--metro-border);
  }
  .bar {
    height: 33px;
    display: flex;
    align-items: center;
    gap: 3px;
    padding: 0 5px;
    -webkit-app-region: drag;
  }
  .cluster {
    display: flex;
    align-items: center;
    gap: 2px;
    -webkit-app-region: no-drag;
    flex-shrink: 0;
  }
  .drag-spacer {
    flex: 0 1 16px;
    min-width: 8px;
    height: 33px;
    -webkit-app-region: drag;
  }
  .btn {
    height: 24px;
    min-width: 24px;
    padding: 0 6px;
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    color: var(--metro-fg);
    display: inline-flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    font-size: 12px;
    line-height: 1;
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
  .btn.close:hover {
    background: var(--metro-urgent);
    border-color: var(--metro-urgent);
    color: #ffffff;
  }
  .omnibar-wrap {
    flex: 1 1 auto;
    min-width: 180px;
    height: 24px;
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
    padding: 0 7px;
    height: 22px;
    display: inline-flex;
    align-items: center;
    font-size: 11px;
    color: var(--metro-fg-dim);
    border-right: 1px solid var(--metro-border);
    flex-shrink: 0;
  }
  .sec-badge.tls { color: var(--metro-success); }
  .sec-badge.warn { color: var(--metro-warning); }
  .sec-badge.kajo { color: var(--metro-accent); }
  #omnibar {
    flex: 1 1 auto;
    min-width: 0;
    height: 22px;
    background: transparent;
    border: none;
    padding: 0 8px;
    font-size: 12px;
    color: var(--metro-fg);
  }
  .pill {
    height: 22px;
    padding: 0 7px;
    background: transparent;
    border: none;
    border-left: 1px solid var(--metro-border);
    color: var(--metro-fg-dim);
    display: inline-flex;
    align-items: center;
    gap: 4px;
    cursor: pointer;
    font-size: 11px;
    flex-shrink: 0;
  }
  .pill:hover { background: var(--metro-surface-4); color: #ffffff; }
  .pill.shield-on { color: #ff8c38; }
  .pill.shield-paused { color: var(--metro-fg-dim); }
  .pill.star-on { color: #f5b942; }
  .hidden { display: none !important; }
  .profile-btn {
    gap: 5px;
    padding: 0 8px;
    font-size: 11px;
    letter-spacing: 0.4px;
  }
  .dot {
    width: 7px;
    height: 7px;
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
  }
  .load-bar.active { width: 72%; opacity: 1; }
</style>
</head>
<body>
<div id="loadBar" class="load-bar"></div>
<div class="bar">
  <div class="cluster">
    <button id="btnBack" class="btn" title="Back (Alt+Left)">←</button>
    <button id="btnFwd" class="btn" title="Forward (Alt+Right)">→</button>
    <button id="btnReload" class="btn" title="Reload / Stop (Ctrl+R)">⟳</button>
    <button id="btnRail" class="btn" title="Toggle Tab Rail (Ctrl+B)">◧</button>
  </div>

  <div class="drag-spacer"></div>

  <div id="omniWrap" class="omnibar-wrap">
    <span id="secBadge" class="sec-badge">❖</span>
    <input id="omnibar" type="text" spellcheck="false" autocomplete="off"
           placeholder="Search or enter URL (Ctrl+L · Ctrl+K Palette)">
    <button id="btnShield" class="pill shield-on" title="Adblock &amp; Trackers">
      <span>🛡</span><span id="blockedNum">0</span>
    </button>
    <button id="btnStar" class="pill" title="Bookmark Page (Ctrl+D)">☆</button>
  </div>

  <div class="drag-spacer"></div>

  <div class="cluster">
    <button id="btnMedia" class="btn hidden" title="Sniffed Media Streams">⬇ <span id="mediaCount">0</span></button>
    <button id="btnDl" class="btn hidden" title="Downloads">↓ <span id="dlCount">0</span></button>
    <button id="btnZoom" class="btn hidden" title="Reset Zoom (Ctrl+0)"><span id="zoomLabel">100%</span></button>
    <button id="btnProfile" class="btn profile-btn" title="Profiles &amp; Private Mode">
      <span id="profileDot" class="dot"></span>
      <span id="profileLabel">DEFAULT</span>
    </button>
    <button id="btnSettings" class="btn" title="Web Settings">⚙</button>
    <button id="btnFloat" class="btn" title="Float / Tile Window">🗗</button>
    <button id="btnClose" class="btn close" title="Close Browser">✕</button>
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

const omni = document.getElementById('omnibar');
const omniWrap = document.getElementById('omniWrap');
const btnBack = document.getElementById('btnBack');
const btnFwd = document.getElementById('btnFwd');
const btnReload = document.getElementById('btnReload');
const btnRail = document.getElementById('btnRail');
const secBadge = document.getElementById('secBadge');
const btnShield = document.getElementById('btnShield');
const blockedNum = document.getElementById('blockedNum');
const btnStar = document.getElementById('btnStar');
const btnMedia = document.getElementById('btnMedia');
const mediaCount = document.getElementById('mediaCount');
const btnDl = document.getElementById('btnDl');
const dlCount = document.getElementById('dlCount');
const btnZoom = document.getElementById('btnZoom');
const zoomLabel = document.getElementById('zoomLabel');
const btnProfile = document.getElementById('btnProfile');
const profileDot = document.getElementById('profileDot');
const profileLabel = document.getElementById('profileLabel');
const loadBar = document.getElementById('loadBar');

let lastState = null;
let acSelectedIdx = -1;

window.__kajoFocusOmnibar = () => {
  omni.focus();
  omni.select();
};

window.__kajoApply = (s) => {
  lastState = s;
  acSelectedIdx = typeof s.ac_selected_idx === 'number' ? s.ac_selected_idx : -1;
  btnBack.disabled = !s.can_go_back;
  btnFwd.disabled = !s.can_go_forward;
  btnReload.textContent = s.is_loading ? '✕' : '⟳';
  loadBar.classList.toggle('active', !!s.is_loading);
  btnRail.classList.toggle('active', s.rail_mode === 2);

  if (document.activeElement !== omni) {
    omni.value = s.active_url || '';
  }

  const url = s.active_url || '';
  secBadge.className = 'sec-badge';
  if (url.startsWith('https://')) {
    secBadge.textContent = '🔒 HTTPS';
    secBadge.classList.add('tls');
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

  btnStar.textContent = s.is_bookmarked ? '★' : '☆';
  btnStar.classList.toggle('star-on', !!s.is_bookmarked);

  const mc = (s.media_streams || []).length;
  btnMedia.classList.toggle('hidden', mc === 0);
  mediaCount.textContent = String(mc);

  const dc = (s.downloads || []).length;
  btnDl.classList.toggle('hidden', dc === 0);
  const activeDl = (s.downloads || []).filter(d => d.in_progress).length;
  dlCount.textContent = activeDl > 0 ? (activeDl + '/' + dc) : String(dc);

  btnZoom.classList.toggle('hidden', (s.zoom_pct || 100) === 100);
  zoomLabel.textContent = (s.zoom_pct || 100) + '%';

  profileDot.style.background = s.private_mode ? '#b146c2' : (s.profile_accent || s.accent_color || '#0078d4');
  profileLabel.textContent = s.private_mode ? 'PRIVATE' : (s.profile_name || 'DEFAULT');
};

omni.addEventListener('focus', () => {
  omniWrap.classList.add('focused');
  omni.select();
});
omni.addEventListener('blur', () => {
  omniWrap.classList.remove('focused');
  acSelectedIdx = -1;
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
    omni.blur();
  } else if (e.key === 'Escape') {
    e.preventDefault();
    acSelectedIdx = -1;
    if (lastState) omni.value = lastState.active_url || '';
    omni.blur();
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
btnStar.onclick = () => send('toggle_bookmark');
btnMedia.onclick = () => send('toggle_overlay', { mode: 'media' });
btnDl.onclick = () => send('toggle_overlay', { mode: 'downloads' });
btnZoom.onclick = () => send('zoom_reset');
btnProfile.onclick = () => send('toggle_overlay', { mode: 'profiles' });
document.getElementById('btnSettings').onclick = () => send('open_settings');
document.getElementById('btnFloat').onclick = () => send('toggle_float');
document.getElementById('btnClose').onclick = () => send('close_window');

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
  .rail.collapsed-strip .rail-head,
  .rail.collapsed-strip .tab-list,
  .rail.collapsed-strip .rail-foot { display: none; }
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
    height: 26px;
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    color: var(--metro-fg);
    cursor: pointer;
    font-size: 11px;
    text-align: left;
    padding: 0 8px;
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
    height: 24px;
    padding: 0 8px;
    font-size: 10px;
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
    gap: 7px;
    padding: 0 8px 0 11px;
    border-left: 3px solid transparent;
    cursor: pointer;
    height: 42px;
    overflow: hidden;
  }
  .rail.compact .tab-row {
    justify-content: flex-start;
    padding: 0 2px 0 11px;
    gap: 0;
  }
  .tab-row:hover { background: var(--metro-surface-3); }
  .tab-row.active {
    background: var(--metro-surface-3);
    border-left-color: var(--metro-accent);
  }
  .tab-row.suspended { opacity: 0.55; }
  .fav {
    width: 16px;
    height: 16px;
    flex-shrink: 0;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    font-size: 10px;
    color: var(--metro-accent);
    overflow: hidden;
  }
  .fav img { width: 14px; height: 14px; display: block; }
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
    font-size: 12px;
    line-height: 1.25;
    color: var(--metro-fg);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .tab-domain {
    font-size: 10px;
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
    width: 18px;
    height: 18px;
    background: transparent;
    border: none;
    color: var(--metro-fg-dim);
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    font-size: 11px;
    padding: 0;
  }
  .mini-btn:hover { background: var(--metro-surface-4); color: #ffffff; }
  .mini-btn.close:hover { background: var(--metro-urgent); color: #ffffff; }
  .tab-row:not(:hover) .mini-btn.close { opacity: 0.3; }
  .rail-foot {
    border-top: 1px solid var(--metro-border);
    padding: 5px 6px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 4px;
    font-size: 10px;
    color: var(--metro-fg-dim);
    flex-shrink: 0;
  }
  .mode-btn {
    height: 22px;
    padding: 0 6px;
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    color: var(--metro-fg-dim);
    cursor: pointer;
    font-size: 10px;
  }
  .mode-btn:hover { border-color: var(--metro-accent); color: #ffffff; }
</style>
</head>
<body>
<div id="railRoot" class="rail">
  <div class="rail-head">
    <button id="btnNewTab" class="newtab-btn" title="New Tab (Ctrl+T)">
      <span id="newTabLabel">+ NEW TAB</span>
      <span class="rail-foot-label">^T</span>
    </button>
  </div>
  <div id="tabList" class="tab-list"></div>
  <div class="rail-foot">
    <span id="tabCount" class="rail-foot-label">1 TAB</span>
    <button id="btnPalette" class="mode-btn rail-foot-label" title="Command Palette (Ctrl+K)">⌘K</button>
    <button id="btnMode" class="mode-btn" title="Cycle Rail Mode (Compact / Pinned / Autohide)">⇆</button>
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

const railRoot = document.getElementById('railRoot');
const tabList = document.getElementById('tabList');
const btnNewTab = document.getElementById('btnNewTab');
const newTabLabel = document.getElementById('newTabLabel');
const tabCount = document.getElementById('tabCount');

const btnMode = document.getElementById('btnMode');

let lastRailState = null;
railRoot.addEventListener('mouseenter', () => {
  send('rail_hover', { inside: true });
});
railRoot.addEventListener('mousemove', () => {
  if (lastRailState && lastRailState.rail_mode !== 2 && !lastRailState.rail_hover_open) {
    send('rail_hover', { inside: true });
  }
});
railRoot.addEventListener('mouseleave', () => {
  send('rail_hover', { inside: false });
});
window.addEventListener('blur', () => {
  if (lastRailState && lastRailState.rail_hover_open) {
    send('rail_hover', { inside: false });
  }
});

btnNewTab.onclick = () => send('new_tab');
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
    img.src = t.favicon_url;
    img.onerror = () => { box.textContent = (t.domain || 'K')[0].toUpperCase(); };
    box.appendChild(img);
    return box;
  }
  box.textContent = t.pinned ? '★' : ((t.domain || t.title || 'K')[0] || 'K').toUpperCase();
  return box;
}

window.__kajoApply = (s) => {
  lastRailState = s;
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
    tabList.innerHTML = '';
    return;
  }
  newTabLabel.textContent = isCompact ? '+' : '+ NEW TAB';
  tabCount.textContent = s.tabs.length + (s.tabs.length === 1 ? ' TAB' : ' TABS');

  const prevScroll = tabList.scrollTop;
  const collapsed = new Set(s.collapsed_groups || []);
  tabList.innerHTML = '';

  const renderTabs = (tabs, groupName) => {
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
      title.textContent = (t.pinned ? '📌 ' : '') + (t.title || t.domain || 'New Tab');
      txt.appendChild(title);
      if (s.show_tab_domains && t.domain) {
        const dom = document.createElement('div');
        dom.className = 'tab-domain';
        dom.textContent = t.domain + (t.suspended ? ' · zzz' : '');
        txt.appendChild(dom);
      }
      row.appendChild(txt);

      const act = document.createElement('div');
      act.className = 'tab-actions';
      const mb = document.createElement('button');
      mb.className = 'mini-btn';
      mb.textContent = t.muted ? '🔇' : '🔊';
      mb.style.opacity = (t.muted || t.audible) ? '1' : '0.45';
      mb.title = t.muted ? 'Unmute Tab' : 'Mute Tab';
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
      row.onauxclick = (e) => { if (e.button === 1) { e.preventDefault(); send('close_tab', { tab_id: t.id }); } };
      tabList.appendChild(row);
    }
  };

  const ungrouped = s.tabs.filter(t => !t.group);
  renderTabs(ungrouped, '');

  const seenGroups = [];
  for (const t of s.tabs) {
    if (t.group && !seenGroups.includes(t.group)) seenGroups.push(t.group);
  }
  for (const g of seenGroups) {
    renderTabs(s.tabs.filter(t => t.group === g), g);
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
    padding: 7px 10px;
    background: var(--metro-surface-0);
    border-bottom: 1px solid var(--metro-border);
    font-size: 11px;
    letter-spacing: 0.7px;
    text-transform: uppercase;
  }
  .hdr-title { color: #ffffff; font-weight: 600; }
  .xbtn {
    background: var(--metro-surface-2);
    border: 1px solid var(--metro-border);
    color: var(--metro-fg);
    padding: 2px 8px;
    cursor: pointer;
    font-size: 11px;
  }
  .xbtn:hover { background: var(--metro-urgent); border-color: var(--metro-urgent); }
  .search-box {
    width: 100%;
    padding: 8px 10px;
    background: #0d0d0d;
    border: none;
    border-bottom: 1px solid var(--metro-border-strong);
    color: #ffffff;
    font-size: 13px;
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
    padding: 7px 9px;
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
    font-size: 12px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .item-sub {
    color: var(--metro-fg-dim);
    font-size: 11px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .badge {
    padding: 2px 6px;
    font-size: 10px;
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
    font-size: 11px;
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
    padding: 5px 8px;
    background: #0d0d0d;
    border: 1px solid var(--metro-border-strong);
    color: #ffffff;
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
  .stat-num { font-size: 16px; color: #ffffff; font-weight: 600; }
  .stat-lbl { font-size: 10px; color: var(--metro-fg-dim); margin-top: 2px; }
  .prog {
    height: 4px;
    background: var(--metro-surface-0);
    margin-top: 4px;
    border: 1px solid var(--metro-border);
  }
  .prog > div { height: 100%; background: var(--metro-accent); }
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
const isAcRole = new URLSearchParams(location.search).get('role') === 'ac';
let curState = null;
let selectedIdx = 0;

window.__kajoOverlayStep = (delta) => {
  const items = root.querySelectorAll('.item');
  if (!items.length) return;
  selectedIdx = Math.max(0, Math.min(items.length - 1, selectedIdx + delta));
  items.forEach((el, i) => el.classList.toggle('sel', i === selectedIdx));
  items[selectedIdx].scrollIntoView({ block: 'nearest' });
};

window.__kajoOverlayFocus = () => {
  if (isAcRole) return;
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
  const modeChanged = !curState || curState.overlay_mode !== s.overlay_mode;
  curState = s;
  if (modeChanged) selectedIdx = 0;
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
    root.appendChild(renderHeader('⬇ SNIFFED MEDIA STREAMS'));
    const list = document.createElement('div');
    list.className = 'list';
    for (const m of (s.media_streams || [])) {
      const row = document.createElement('div');
      row.className = 'item';
      row.appendChild(makeMainBox(m.label || m.uri || '', (m.mime_type || '') + ' · ' + (m.uri || '')));
      const play = document.createElement('button');
      play.className = 'btn primary';
      play.textContent = '▶ kajo-player';
      play.onclick = (e) => { e.stopPropagation(); send('play_in_kajo_player', { uri: m.uri }); };
      const dl = document.createElement('button');
      dl.className = 'btn';
      dl.textContent = '⬇ Save';
      dl.onclick = (e) => { e.stopPropagation(); send('download_media_uri', { uri: m.uri }); };
      row.append(play, dl);
      list.appendChild(row);
    }
    root.appendChild(list);
  } else if (mode === 'downloads') {
    root.appendChild(renderHeader('↓ DOWNLOADS'));
    const list = document.createElement('div');
    list.className = 'list';
    for (const d of (s.downloads || [])) {
      const row = document.createElement('div');
      row.className = 'item';
      const st = d.complete ? 'COMPLETE' : (d.canceled ? 'CANCELED' : (d.percent >= 0 ? d.percent + '%' : 'ACTIVE'));
      const mainBox = makeMainBox(d.filename || '', st + ' · ' + (d.full_path || d.url || ''));
      const prog = document.createElement('div');
      prog.className = 'prog';
      const bar = document.createElement('div');
      bar.style.width = Math.max(0, Math.min(100, d.percent || 0)) + '%';
      prog.appendChild(bar);
      mainBox.appendChild(prog);
      row.appendChild(mainBox);
      if (d.in_progress) {
        const c = document.createElement('button');
        c.className = 'btn';
        c.textContent = 'Cancel';
        c.onclick = () => send('cancel_download', { id: d.id });
        row.appendChild(c);
      } else if (d.complete) {
        const o = document.createElement('button');
        o.className = 'btn primary';
        o.textContent = 'Open';
        o.onclick = () => send('open_download_folder', { path: d.full_path });
        row.appendChild(o);
      }
      list.appendChild(row);
    }
    root.appendChild(list);
    const act = document.createElement('div');
    act.className = 'act-row';
    const bAll = document.createElement('button');
    bAll.className = 'btn';
    bAll.textContent = 'Open kajo://downloads';
    bAll.onclick = () => send('open_kajo_page', { page: 'kajo://downloads' });
    act.appendChild(bAll);
    root.appendChild(act);
  } else if (mode === 'profiles') {
    root.appendChild(renderHeader('● PROFILES & PRIVATE MODE'));
    const list = document.createElement('div');
    list.className = 'list';
    for (const p of (s.profiles || [])) {
      const row = document.createElement('div');
      row.className = 'item' + (p.active ? ' sel' : '');
      const badge = document.createElement('span');
      badge.className = 'badge' + (p.active ? ' accent' : '');
      badge.textContent = p.active ? 'CURRENT' : 'LAUNCH';
      row.append(makeMainBox((p.icon || '') + ' ' + (p.name || ''), 'Profile ID: ' + (p.id || '')), badge);
      row.onclick = () => { if (!p.active) send('launch_profile', { id: p.id, private_mode: false }); };
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
    bAdd.onclick = () => { if (inp.value.trim()) send('create_profile', { name: inp.value.trim() }); };
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
  if (accent.empty()) accent = "#0078d4";
  std::ostringstream h;
  h << "<!doctype html><html><head><meta charset='utf-8'><title>" << HtmlEscape(title)
    << "</title><style>"
    << ":root{--accent:" << accent << ";--bg0:#111111;--bg1:#141414;--bg2:#181818;--bg3:#1e1e1e;--b:#2b2b2b;--fg:#ececec;--dim:#999999;--ok:#2ea043;--warn:#d97706;--err:#e81123;}"
    << "*{box-sizing:border-box;border-radius:0;}"
    << "html,body{margin:0;padding:0;background:var(--bg1);color:var(--fg);font-family:'Fira Code',monospace;font-size:13px;}"
    << ".nav{display:flex;gap:4px;padding:10px 24px;background:var(--bg0);border-bottom:1px solid var(--b);align-items:center;flex-wrap:wrap;}"
    << ".brand{color:var(--accent);font-weight:700;letter-spacing:1.2px;margin-right:16px;}"
    << ".nav a{color:var(--dim);text-decoration:none;padding:5px 10px;border:1px solid transparent;}"
    << ".nav a:hover{color:#fff;background:var(--bg2);border-color:var(--b);}"
    << ".nav a.act{color:#fff;border-color:var(--accent);background:var(--bg2);}"
    << ".wrap{max-width:1040px;margin:0 auto;padding:24px;}"
    << "h1{margin:0 0 16px 0;font-size:18px;letter-spacing:0.8px;text-transform:uppercase;border-left:4px solid var(--accent);padding-left:10px;}"
    << "h2{margin:20px 0 10px 0;font-size:13px;color:var(--dim);letter-spacing:0.8px;text-transform:uppercase;}"
    << ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:10px;}"
    << ".card{background:var(--bg2);border:1px solid var(--b);padding:12px;display:block;color:inherit;text-decoration:none;}"
    << ".card:hover{border-color:var(--accent);background:var(--bg3);}"
    << ".k{font-size:20px;font-weight:700;color:#fff;}"
    << ".lbl{font-size:11px;color:var(--dim);margin-top:4px;text-transform:uppercase;}"
    << "table{width:100%;border-collapse:collapse;background:var(--bg2);border:1px solid var(--b);}"
    << "th,td{padding:8px 10px;border-bottom:1px solid var(--b);text-align:left;vertical-align:middle;}"
    << "th{background:var(--bg0);color:var(--dim);font-size:11px;text-transform:uppercase;}"
    << ".btn{display:inline-block;padding:5px 10px;background:var(--bg3);border:1px solid #3c3c3c;color:#fff;text-decoration:none;cursor:pointer;font-family:inherit;font-size:11px;}"
    << ".btn:hover{border-color:var(--accent);}"
    << ".btn.pri{background:var(--accent);border-color:var(--accent);}"
    << ".btn.bad:hover{background:var(--err);border-color:var(--err);}"
    << "input[type=text]{padding:7px 10px;background:#0c0c0c;border:1px solid #3c3c3c;color:#fff;font-family:inherit;font-size:12px;}"
    << "input[type=text]:focus{border-color:var(--accent);outline:none;}"
    << "</style></head><body>"
    << "<div class='nav'><span class='brand'>❖ KAJO // CEF</span>"
    << "<a href='kajo://newtab' class='" << (active_nav == "newtab" ? "act" : "") << "'>NEWTAB</a>"
    << "<a href='kajo://bookmarks' class='" << (active_nav == "bookmarks" ? "act" : "") << "'>BOOKMARKS</a>"
    << "<a href='kajo://history' class='" << (active_nav == "history" ? "act" : "") << "'>HISTORY</a>"
    << "<a href='kajo://adblock' class='" << (active_nav == "adblock" ? "act" : "") << "'>ADBLOCK</a>"
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
      b << "<a class='card' href='" << HtmlEscape(bm.url) << "'><div>"
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
      b << "<tr><td><a style='color:#fff;text-decoration:none;' href='" << HtmlEscape(he.url) << "'>"
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
      b << "<tr><td><a style='color:#fff;' href='" << HtmlEscape(bm.url) << "'>"
        << HtmlEscape(bm.title) << "</a></td><td>" << HtmlEscape(bm.folder) << "</td><td style='color:var(--dim);'>"
        << HtmlEscape(bm.url) << "</td><td><a class='btn bad' href='kajo://bookmarks?action=delete&uuid="
        << HtmlEscape(bm.uuid) << "'>DELETE</a></td></tr>";
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
      b << "<tr><td><a style='color:#fff;' href='" << HtmlEscape(he.url) << "'>"
        << HtmlEscape(he.title.empty() ? he.url : he.title) << "</a></td><td style='color:var(--dim);'>"
        << HtmlEscape(he.url) << "</td><td>" << he.visit_count << "</td><td>"
        << "<a class='btn bad' href='kajo://history?action=delete&uri=" << HtmlEscape(he.url) << "'>×</a></td></tr>";
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
      << "<tr><td>Built-in Baseline (Ads &amp; YouTube Telemetry)<br><span style='color:var(--dim);font-size:11px;'>bundled offline fallback</span></td><td>"
      << s.baseline_rules << "</td><td>ACTIVE</td></tr>";
    for (const auto& fl : s.filter_lists) {
      const char* state_lbl = (fl.rule_count > 0)
                                  ? "LOADED"
                                  : (s.filter_update_busy ? "DOWNLOADING..." : "NOT LOADED (0)");
      b << "<tr><td>" << HtmlEscape(fl.name) << "<br><span style='color:var(--dim);font-size:11px;'>"
        << HtmlEscape(fl.url) << "</span></td><td>" << fl.rule_count << "</td><td>"
        << state_lbl << "</td></tr>";
    }
    b << "</table><h2>PAUSED / ALLOWLISTED HOSTS</h2><table><tr><th>Host</th><th>Action</th></tr>";
    for (const auto& h : s.allowlist_hosts) {
      b << "<tr><td>" << HtmlEscape(h) << "</td><td><a class='btn' href='kajo://adblock?action=unpause&host="
        << HtmlEscape(h) << "'>RESUME BLOCKING</a></td></tr>";
    }
    if (s.allowlist_hosts.empty()) {
      b << "<tr><td colspan='2' style='color:var(--dim);'>No paused hosts</td></tr>";
    }
    b << "</table>";
    *out_body = BuildKajoShellPage(s, "adblock", "Kajo — Adblock", b.str());
    return true;
  }

  if (page == "downloads") {
    std::ostringstream b;
    b << "<h1>DOWNLOADS (" << s.downloads.size() << ")</h1>"
      << "<div style='margin-bottom:12px;'><a class='btn' href='kajo://downloads?action=clear_done'>CLEAR FINISHED</a></div>"
      << "<table><tr><th>File</th><th>Progress</th><th>Size</th><th>Location</th></tr>";
    for (const auto& d : s.downloads) {
      b << "<tr><td>" << HtmlEscape(d.filename) << "</td><td>"
        << (d.complete ? "100% DONE" : (d.canceled ? "CANCELED" : std::to_string(std::max(0, d.percent)) + "%"))
        << "</td><td>" << FormatBytes(d.received_bytes) << " / " << FormatBytes(d.total_bytes)
        << "</td><td style='color:var(--dim);'>" << HtmlEscape(d.full_path) << "</td></tr>";
    }
    b << "</table>";
    *out_body = BuildKajoShellPage(s, "downloads", "Kajo — Downloads", b.str());
    return true;
  }

  if (page == "gpu") {
    std::ostringstream b;
    b << "<h1>GPU &amp; HARDWARE VIDEO PIPELINE</h1>"
      << "<div style='margin-bottom:16px;'><a class='btn pri' href='kajo://gpu?action=open_chrome_gpu'>OPEN RAW CHROME://GPU DIAGNOSTICS</a></div>"
      << "<table><tr><th>Subsystem</th><th>Configured State</th></tr>"
      << "<tr><td>Video Hardware Acceleration</td><td>" << (s.hw_accel_enabled ? "VA-API Zero-Copy (Vulkan/EGL ANGLE)" : "Disabled (--no-hw-accel)") << "</td></tr>"
      << "<tr><td>Widevine CDM Hint</td><td>" << (s.widevine_available ? HtmlEscape(s.widevine_path) : "Not found on system") << "</td></tr>"
      << "<tr><td>Native Wayland Ozone</td><td>Active (CEF Views Alloy Frameless Window)</td></tr>"
      << "<tr><td>User Agent</td><td style='color:var(--dim);'>" << HtmlEscape(s.user_agent) << "</td></tr>"
      << "</table>";
    *out_body = BuildKajoShellPage(s, "gpu", "Kajo — GPU & VA-API", b.str());
    return true;
  }

  /* Default: kajo://about */
  std::ostringstream b;
  b << "<h1>ABOUT KAJO BROWSER (CEF BACKEND)</h1>"
    << "<table><tr><th>Component</th><th>Details</th></tr>"
    << "<tr><td>Application</td><td>kajo-web-cef (Aero+Metro Alloy Views + Native Wayland)</td></tr>"
    << "<tr><td>Profile</td><td>" << HtmlEscape(s.profile_icon + " " + s.profile_name) << (s.private_mode ? " (Private)" : "") << "</td></tr>"
    << "<tr><td>Adblocker</td><td>IO-thread native ABP + Hosts + Cosmetic + YouTube Skip (" << s.adblock_rules << " rules)</td></tr>"
    << "<tr><td>DRM / Media</td><td>" << (s.widevine_available ? "Widevine CDM detected" : "Widevine CDM absent") << " + kajo-player handoff</td></tr>"
    << "</table>";
  *out_body = BuildKajoShellPage(s, "about", "Kajo — About", b.str());
  return true;
}

class MemoryResourceHandler : public CefResourceHandler {
public:
  MemoryResourceHandler(ChromeStateStore* store, bool is_chrome_ui)
      : store_(store), is_chrome_ui_(is_chrome_ui) {}

  bool Open(CefRefPtr<CefRequest> request,
            bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    handle_request = true;
    const std::string url = request->GetURL().ToString();
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
  ChromeStateStore* store_;
  bool is_chrome_ui_;
  std::string data_;
  std::string mime_ = "text/html";
  int status_code_ = 200;
  size_t offset_ = 0;

  IMPLEMENT_REFCOUNTING(MemoryResourceHandler);
};

class KajoSchemeHandlerFactory : public CefSchemeHandlerFactory {
public:
  KajoSchemeHandlerFactory(ChromeStateStore* store, bool is_chrome_ui)
      : store_(store), is_chrome_ui_(is_chrome_ui) {}

  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    return new MemoryResourceHandler(store_, is_chrome_ui_);
  }

private:
  ChromeStateStore* store_;
  bool is_chrome_ui_;

  IMPLEMENT_REFCOUNTING(KajoSchemeHandlerFactory);
};

void RegisterKajoSchemeHandlers(CefRefPtr<CefRequestContext> ctx,
                                ChromeStateStore* store) {
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
      CEF_SCHEME_OPTION_CORS_ENABLED |
      CEF_SCHEME_OPTION_FETCH_ENABLED);
}

}  // namespace kajo_web
