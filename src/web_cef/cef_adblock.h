#ifndef KAJO_CEF_ADBLOCK_H
#define KAJO_CEF_ADBLOCK_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kajo_web {

enum ResourceTypeMask : uint32_t {
  RT_OTHER = 1u << 0,
  RT_SCRIPT = 1u << 1,
  RT_IMAGE = 1u << 2,
  RT_STYLESHEET = 1u << 3,
  RT_XHR = 1u << 4,
  RT_SUBDOCUMENT = 1u << 5,
  RT_FONT = 1u << 6,
  RT_MEDIA = 1u << 7,
  RT_MAIN_FRAME = 1u << 8,
  RT_ANY = 0xFFFFFFFFu,
};

struct FilterListSpec {
  const char *slug;
  const char *title;
  const char *url;
};

const std::vector<FilterListSpec> &GetDefaultFilterListSpecs();

struct NetRule {
  std::string pattern;         // lowercase match substring (empty for domain-only rules)
  std::string domain_anchor;   // lowercase domain if anchored with ||
  bool domain_suffix_only = false;
  bool is_exception = false;
  int third_party_req = 0;     // 0=any, 1=third-party only, -1=first-party only
  uint32_t type_mask = RT_ANY;
  std::vector<std::string> include_domains;
  std::vector<std::string> exclude_domains;
};

struct CosmeticRule {
  std::string selector;
  std::vector<std::string> exclude_domains;
};

struct FilterSnapshot {
  std::unordered_set<std::string> block_domains;
  std::unordered_set<std::string> exc_domains;
  std::unordered_map<std::string, std::vector<NetRule>> domain_rules;
  std::vector<NetRule> generic_rules;
  std::vector<NetRule> exc_rules;
  std::vector<CosmeticRule> global_cosmetic;
  std::unordered_map<std::string, std::vector<CosmeticRule>> domain_cosmetic;
  std::unordered_map<std::string, size_t> list_rule_counts;
  size_t baseline_rules = 0;
  size_t total_rules = 0;
  size_t loaded_lists = 0;
};

class CefAdblockEngine {
public:
  CefAdblockEngine();

  void SetEnabled(bool enabled);
  bool IsEnabled() const;

  void SetDirectories(const std::string &cache_dir, const std::string &data_dir);
  void ReloadFromDisk(const std::string &custom_rules_path = "");
  bool WriteListFileOnly(const std::string &slug, const std::string &content);
  bool SaveListAndReload(const std::string &slug, const std::string &content,
                         const std::string &custom_rules_path = "");
  bool HasMissingOrStaleLists(int max_age_hours = 24) const;
  void LoadAdditionalRulesText(const std::string &text);

  // Thread-safe IO-thread lookup. Never blocks main/top-level navigations.
  bool ShouldBlock(const std::string &url, const std::string &first_party_url,
                   uint32_t resource_type_mask) const;

  // Returns ready-to-inject CSS rule string for host (empty if disabled or paused).
  std::string GetCosmeticCssForHost(const std::string &host) const;

  // Per-host allowlist persisted in <data_dir>/adblock-allowlist.txt.
  bool IsHostPaused(const std::string &host) const;
  bool ToggleHostPaused(const std::string &host);
  size_t GetPausedHostCount() const;
  std::vector<std::string> GetPausedHosts() const;

  // Telemetry counters (real blocks only).
  size_t GetRuleCount() const;
  size_t GetBaselineRuleCount() const;
  size_t GetRuleCountForList(const std::string &slug) const;
  size_t GetLoadedListCount() const;
  void RecordBlockedForTab(int browser_id);
  void ResetTabBlockedCount(int browser_id);
  uint64_t GetBlockedCountForTab(int browser_id) const;
  uint64_t GetTotalBlockedCount() const;

  // Exposed for unit tests and direct parsing.
  static size_t ParseFilterTextInto(const std::string &text, FilterSnapshot &snap);
  static std::string ExtractHost(const std::string &url);

private:
  void LoadAllowlist();
  void SaveAllowlistLocked() const;
  std::shared_ptr<const FilterSnapshot> GetSnapshot() const;

  mutable std::mutex cfg_mu_;
  bool enabled_ = true;
  std::string cache_dir_;
  std::string data_dir_;
  std::unordered_set<std::string> paused_hosts_;

  mutable std::mutex snap_mu_;
  std::shared_ptr<const FilterSnapshot> snapshot_;

  mutable std::mutex count_mu_;
  std::unordered_map<int, uint64_t> blocked_per_tab_;
  uint64_t total_blocked_ = 0;
};

} // namespace kajo_web

#endif // KAJO_CEF_ADBLOCK_H
