#ifndef KAJO_WEB_CEF_PREFS_HELPER_H
#define KAJO_WEB_CEF_PREFS_HELPER_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "include/internal/cef_types.h"

namespace kajo_web {

/* Parses KAJO_CEF_LOG ("verbose", "info", "warn", "error", "fatal", "disable").
 * Defaults to LOGSEVERITY_FATAL so harmless upstream Chromium GPU probe noise
 * does not spam stderr unless requested. */
cef_log_severity_t ParseCefLogSeverity(const char* env_val);

/* Returns true if |req_url| and |first_party_url| share the same registrable site
 * (or if either URL is not http/https). */
bool IsSameSiteUrl(const std::string& req_url, const std::string& first_party_url);

struct LoadedUserScripts {
  std::string combined_css;
  std::vector<std::pair<std::string, std::string>> js_files;
};

/* Loads sorted *.css and *.js user files from |scripts_dir|. */
LoadedUserScripts LoadUserScriptsFromDir(const std::string& scripts_dir);

/* Persists site permission grants/denials in JSON ("origin|perm_key": "allow"|"deny"). */
class PermissionStore {
public:
  struct Entry {
    std::string origin;
    std::string perm_key;
    bool allow = false;
  };

  bool Load(const std::string& filepath);
  bool Save(const std::string& filepath) const;

  /* Returns 1 if allowed, -1 if denied, 0 if not stored (ask). */
  int Lookup(const std::string& origin, const std::string& perm_key) const;
  void Set(const std::string& origin, const std::string& perm_key, bool allow);
  bool Remove(const std::string& origin, const std::string& perm_key);
  std::vector<Entry> ListEntries() const;
  void Clear();
  size_t Size() const { return entries_.size(); }

private:
  static std::string MakeKey(const std::string& origin, const std::string& perm_key);
  std::map<std::string, bool> entries_;
};

/* Human-readable permission label & persistence key from CEF bitmasks. */
std::string FormatPermissionLabel(uint32_t media_perms, uint32_t perm_types);
std::string FormatPermissionKeyLabel(const std::string& perm_key);
std::string PermissionKey(uint32_t media_perms, uint32_t perm_types);

/* Expands leading ~/ or ~ and trims whitespace. */
std::string ExpandTildePath(const std::string& raw);

/* True when |id| is a non-default slug safe for profile deletion ([a-z0-9_-]+). */
bool IsSafeProfileSlug(const std::string& id);

/* Parses #rrggbb hex string into opaque ARGB cef_color_t, or |fallback|. */
cef_color_t ParseHexColorArgb(const std::string& hex, cef_color_t fallback);

/* Default CefBrowserSettings::background_color for web content browsers.
 * Zero (alpha=0) lets Chromium use the W3C default white canvas on light-only
 * pages with transparent html/body and dark canvas when a page opts into
 * `color-scheme: dark`, avoiding dark text on forced dark #141414 canvases. */
cef_color_t ContentBrowserDefaultBackgroundColor();

/* Returns true when |hex| (#rrggbb) has relative luminance > 0.5 (light surface).
 * Falls back to dark (#141414 -> false) on empty/invalid input. */
bool IsLightSurfaceHex(const std::string& hex);

/* Maps KajoWebColorScheme + Kajo theme lightness to CEF color scheme variant. */
cef_color_variant_t ResolveWebColorSchemeVariant(int color_scheme,
                                                 bool kajo_theme_is_light);

enum class TabColorSchemeOverride : int {
  AUTO = 0,
  LIGHT = 1,
  DARK = 2,
};

/* Cycles per-tab prefers-color-scheme override. First click from AUTO jumps to
 * the opposite of |default_is_dark| so the user gets an immediate change:
 *   default_is_dark=true : AUTO -> LIGHT -> DARK -> AUTO
 *   default_is_dark=false: AUTO -> DARK -> LIGHT -> AUTO */
TabColorSchemeOverride NextTabColorSchemeOverride(TabColorSchemeOverride cur,
                                                  bool default_is_dark);

/* Value for CDP Emulation.setEmulatedMedia prefers-color-scheme ("", "light", "dark"). */
const char* TabColorSchemeEmulatedMediaValue(TabColorSchemeOverride ov);

/* Short uppercase label ("AUTO", "LIGHT", "DARK") for topbar toast/status. */
const char* TabColorSchemeOverrideLabel(TabColorSchemeOverride ov);

/* Extracts normalized http/https origin ("https://host[:port]") or full string. */
std::string NormalizeOrigin(const std::string& url_or_origin);

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_PREFS_HELPER_H
