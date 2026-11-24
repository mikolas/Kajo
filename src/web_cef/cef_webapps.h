#ifndef KAJO_WEB_CEF_WEBAPPS_H
#define KAJO_WEB_CEF_WEBAPPS_H

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace kajo_web {

struct WebAppEntry {
  std::string id;
  std::string name;
  std::string start_url;
  std::string scope;
  std::string theme_color = "#0078d4";
  std::string icon_url;
  std::string icon_path;
  std::string desktop_id;
  std::string wayland_app_id;
  std::string profile_id = "default";
  int64_t created_epoch = 0;
  int64_t last_launched_epoch = 0;
};

struct WebManifestParseResult {
  bool valid = false;
  std::string name;
  std::string short_name;
  std::string start_url;
  std::string scope;
  std::string theme_color;
  std::string best_icon_url;
  std::string display;
};

/* Sanitize a web app display name or slug into a safe [a-z0-9-] identifier. */
std::string SanitizeWebAppId(const std::string& raw);

/* Build deterministic Wayland app_id and .desktop filename for (profile, id). */
std::string WebAppWaylandAppId(const std::string& profile_id, const std::string& app_id);
std::string WebAppDesktopId(const std::string& profile_id, const std::string& app_id);

/* URL normalization / scope helpers. */
std::string DeriveOriginFromUrl(const std::string& url);
std::string ResolveUrlAgainstBase(const std::string& base_url, const std::string& ref);
bool IsUrlInWebAppScope(const WebAppEntry& app, const std::string& target_url);
bool IsLikelyAuthFlowUrl(const std::string& url);

/* Parse a Web App Manifest JSON document (manifest.webmanifest / manifest.json). */
WebManifestParseResult ParseWebManifestJson(const std::string& json_text,
                                            const std::string& manifest_url,
                                            const std::string& page_url);

/* Generate an Aero+Metro geometric SVG monogram icon for fallback when no PNG/SVG icon is cached. */
std::string BuildFallbackMonogramSvg(const std::string& name, const std::string& accent_hex);

/* Persistent per-profile registry (~/.local/share/kajo/web-cef/<profile>/webapps.json). */
class WebAppStore {
public:
  bool Load(const std::string& json_path);
  bool Save(const std::string& json_path) const;

  void Upsert(WebAppEntry entry);
  bool Remove(const std::string& id);
  bool TouchLaunched(const std::string& id);

  std::optional<WebAppEntry> FindById(const std::string& id) const;
  std::optional<WebAppEntry> FindBestMatchForUrl(const std::string& url) const;
  std::vector<WebAppEntry> List() const;
  std::string MakeUniqueId(const std::string& preferred) const;
  size_t Size() const;

private:
  mutable std::mutex mu_;
  std::vector<WebAppEntry> apps_;
};

/* Install / remove XDG desktop entry & hicolor icon under custom or XDG data dirs. */
bool InstallWebAppDesktopFilesWithDirs(const WebAppEntry& app,
                                       const std::string& exe_path,
                                       const std::vector<uint8_t>& icon_bytes,
                                       const std::string& icon_content_type,
                                       const std::string& xdg_data_home,
                                       std::string* out_icon_path,
                                       std::string* out_desktop_path);

bool InstallWebAppDesktopFiles(const WebAppEntry& app,
                               const std::string& exe_path,
                               const std::vector<uint8_t>& icon_bytes,
                               const std::string& icon_content_type,
                               std::string* out_icon_path,
                               std::string* out_desktop_path);

void RemoveWebAppDesktopFilesWithDirs(const WebAppEntry& app, const std::string& xdg_data_home);
void RemoveWebAppDesktopFiles(const WebAppEntry& app);

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_WEBAPPS_H
