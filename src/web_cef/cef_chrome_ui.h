#ifndef KAJO_WEB_CEF_CHROME_UI_H
#define KAJO_WEB_CEF_CHROME_UI_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "include/cef_scheme.h"

namespace kajo_web {

struct ChromeTabView {
  uint64_t id = 0;
  std::string title;
  std::string url;
  std::string domain;
  std::string group;
  std::string favicon_url;
  bool pinned = false;
  bool active = false;
  bool loading = false;
  bool audible = false;
  bool muted = false;
  bool suspended = false;
  int64_t blocked_count = 0;
};

struct ChromeDownloadView {
  uint32_t id = 0;
  std::string filename;
  std::string url;
  std::string full_path;
  int percent = 0;
  int64_t received_bytes = 0;
  int64_t total_bytes = 0;
  bool in_progress = false;
  bool complete = false;
  bool canceled = false;
  bool paused = false;
};

struct ChromeMediaView {
  std::string kind;
  std::string label;
  std::string uri;
  std::string mime_type;
  int64_t size_bytes = 0;
};

struct ChromeBookmarkView {
  std::string uuid;
  std::string title;
  std::string url;
  std::string folder;
};

struct ChromeHistoryView {
  std::string title;
  std::string url;
  int visit_count = 0;
  int64_t last_visit_ms = 0;
};

struct ChromeProfileView {
  std::string id;
  std::string name;
  std::string icon;
  std::string accent_color;
  bool active = false;
};

struct ChromeSuggestionView {
  std::string kind; /* "tab", "bookmark", "history", "search", "command" */
  std::string label;
  std::string sublabel;
  std::string action;
  std::string payload;
  uint64_t tab_id = 0;
};

struct ChromeFilterListView {
  std::string id;
  std::string name;
  std::string url;
  bool enabled = true;
  size_t rule_count = 0;
};

struct ChromePermissionView {
  uint64_t req_id = 0;
  std::string origin;
  std::string label;
  std::string perm_key;
};

struct ChromeStateSnapshot {
  /* Window & layout state */
  bool private_mode = false;
  bool is_maximized = false;
  int rail_mode = 2; /* 0=autohide, 1=compact, 2=pinned */
  int rail_side = 0; /* 0=left, 1=right */
  bool rail_hover_open = false;
  int rail_width_px = 240;
  int topbar_mode = 0; /* 0=always, 1=autohide */
  bool topbar_hover_open = false;
  bool profile_border_accent = true;
  bool show_tab_domains = true;
  bool ac_visible = false;
  int ac_selected_idx = -1;
  int closed_tab_count = 0;
  std::string overlay_mode; /* "", "palette", "find", "adblock", "media", "downloads", "profiles", "bookmark" */

  /* Theme tokens */
  std::string accent_color = "#0078d4";
  std::string profile_accent = "#0078d4";
  std::string profile_id = "default";
  std::string profile_name = "DEFAULT";
  std::string profile_icon = "🌐";
  std::string ui_font_family = "Fira Code";
  int ui_font_size_px = 13;
  int badge_size_px = 11;

  /* Active tab */
  uint64_t active_tab_id = 0;
  std::string active_url = "kajo://newtab";
  std::string active_title = "New Tab";
  std::string active_host;
  bool can_go_back = false;
  bool can_go_forward = false;
  bool is_loading = false;
  bool is_bookmarked = false;
  int zoom_pct = 100;

  /* Adblock & HW accel telemetry */
  bool adblock_enabled = true;
  bool host_paused = false;
  int64_t tab_blocked = 0;
  int64_t total_blocked = 0;
  size_t baseline_rules = 0;
  size_t adblock_rules = 0;
  size_t paused_hosts = 0;
  bool filter_update_busy = false;
  std::string filter_update_status;
  bool hw_accel_enabled = true;
  bool widevine_available = false;
  std::string widevine_path;
  std::string user_agent;

  /* Find state */
  std::string find_query;
  int find_active = 0;
  int find_count = 0;
  bool find_match_case = false;

  /* Collections */
  std::vector<ChromeTabView> tabs;
  std::vector<std::string> collapsed_groups;
  std::vector<ChromeDownloadView> downloads;
  std::vector<ChromeMediaView> media_streams;
  std::vector<ChromeBookmarkView> bookmarks;
  ChromeBookmarkView active_bookmark;
  std::vector<std::string> bookmark_folders;
  ChromePermissionView active_permission;
  std::vector<ChromeHistoryView> recent_history;
  std::vector<ChromeHistoryView> history_query_results;
  std::string history_search_query;
  std::vector<ChromeProfileView> profiles;
  std::vector<ChromeSuggestionView> autocomplete;
  std::vector<ChromeSuggestionView> palette_results;
  std::vector<std::string> allowlist_hosts;
  std::vector<ChromeFilterListView> filter_lists;
};

/* Thread-safe store shared between TID_UI (writers) and TID_IO (scheme readers). */
class ChromeStateStore {
public:
  void Update(const ChromeStateSnapshot& snapshot);
  ChromeStateSnapshot Snapshot() const;
  std::string SnapshotJson() const;
  std::string RenderThemeCss() const;
  bool RenderKajoPage(const std::string& url,
                      std::string* out_body,
                      std::string* out_mime) const;
  bool RenderChromeAsset(const std::string& url,
                         std::string* out_body,
                         std::string* out_mime) const;

private:
  mutable std::mutex mu_;
  ChromeStateSnapshot state_;
};

std::string JsonEscape(const std::string& s);
std::string HtmlEscape(const std::string& s);

/* Registers kajo-chrome:// and kajo:// scheme handlers on a request context. */
void RegisterKajoSchemeHandlers(CefRefPtr<CefRequestContext> ctx,
                                ChromeStateStore* store);

/* Builds custom scheme privileges for CefApp::OnRegisterCustomSchemes. */
void RegisterKajoCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar);

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_CHROME_UI_H
