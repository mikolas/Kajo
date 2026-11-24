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
  bool Load(const std::string& filepath);
  bool Save(const std::string& filepath) const;

  /* Returns 1 if allowed, -1 if denied, 0 if not stored (ask). */
  int Lookup(const std::string& origin, const std::string& perm_key) const;
  void Set(const std::string& origin, const std::string& perm_key, bool allow);
  void Clear();
  size_t Size() const { return entries_.size(); }

private:
  static std::string MakeKey(const std::string& origin, const std::string& perm_key);
  std::map<std::string, bool> entries_;
};

/* Human-readable permission label & persistence key from CEF bitmasks. */
std::string FormatPermissionLabel(uint32_t media_perms, uint32_t perm_types);
std::string PermissionKey(uint32_t media_perms, uint32_t perm_types);

/* True when |id| is a non-default slug safe for profile deletion ([a-z0-9_-]+). */
bool IsSafeProfileSlug(const std::string& id);

/* Parses #rrggbb hex string into opaque ARGB cef_color_t, or |fallback|. */
cef_color_t ParseHexColorArgb(const std::string& hex, cef_color_t fallback);

/* Extracts normalized http/https origin ("https://host[:port]") or full string. */
std::string NormalizeOrigin(const std::string& url_or_origin);

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_PREFS_HELPER_H
