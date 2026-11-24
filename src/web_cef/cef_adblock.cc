#include "cef_adblock.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace kajo_web {
namespace {

const char *kBuiltinBaselineRules = R"ABP(
! Kajo Web baseline ad & telemetry filters (active before external lists sync)
||doubleclick.net^
||googlesyndication.com^
||googleadservices.com^
||adservice.google.com^
||adservice.google.fi^
||pagead2.googlesyndication.com^
||tpc.googlesyndication.com^
||google-analytics.com^
||googletagmanager.com^
||connect.facebook.net/signals/$third-party
||scorecardresearch.com^
||amazon-adsystem.com^
||adnxs.com^
||taboola.com^
||outbrain.com^
||criteo.com^
||rubiconproject.com^
||pubmatic.com^
||openx.net^
||casalemedia.com^
||youtube.com/pagead/
||youtube.com/api/stats/ads
||youtube.com/get_midroll_info
||youtube.com/ptracking
||youtube.com/api/stats/qoe?adformat=$xhr
youtube.com##ytd-ad-slot-renderer
youtube.com##ytd-display-ad-renderer
youtube.com##ytd-promoted-sparkles-web-renderer
youtube.com##ytd-companion-slot-renderer
youtube.com###player-ads
youtube.com###masthead-ad
youtube.com##.ytp-ad-overlay-container
)ABP";

std::string ToLowerAscii(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  for (unsigned char c : sv) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

std::string TrimAscii(std::string_view sv) {
  size_t a = 0;
  while (a < sv.size() && std::isspace(static_cast<unsigned char>(sv[a])))
    ++a;
  size_t b = sv.size();
  while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1])))
    --b;
  return std::string(sv.substr(a, b - a));
}

bool SplitHostAndRest(std::string_view url, std::string &out_host,
                      std::string &out_rest) {
  auto pos = url.find("://");
  if (pos == std::string_view::npos)
    return false;
  auto scheme = ToLowerAscii(url.substr(0, pos));
  if (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss")
    return false;
  std::string_view after = url.substr(pos + 3);
  auto slash = after.find('/');
  auto q = after.find('?');
  auto end_auth = std::min(slash == std::string_view::npos ? after.size() : slash,
                           q == std::string_view::npos ? after.size() : q);
  std::string_view auth = after.substr(0, end_auth);
  if (auto at = auth.rfind('@'); at != std::string_view::npos)
    auth = auth.substr(at + 1);
  if (!auth.empty() && auth.front() == '[') {
    auto close = auth.find(']');
    if (close != std::string_view::npos)
      auth = auth.substr(0, close + 1);
  } else if (auto colon = auth.rfind(':'); colon != std::string_view::npos) {
    auth = auth.substr(0, colon);
  }
  while (!auth.empty() && auth.back() == '.')
    auth.remove_suffix(1);
  if (auth.empty())
    return false;
  out_host = ToLowerAscii(auth);
  out_rest = end_auth < after.size() ? ToLowerAscii(after.substr(end_auth)) : "/";
  return true;
}

bool HostMatchesOrSubdomain(const std::string &host,
                            const std::string &target) {
  if (host == target)
    return true;
  if (host.size() <= target.size() + 1)
    return false;
  size_t off = host.size() - target.size();
  return host[off - 1] == '.' && host.compare(off, target.size(), target) == 0;
}

bool HasDomainInSet(const std::string &host,
                    const std::unordered_set<std::string> &set) {
  if (host.empty() || set.empty())
    return false;
  if (set.find(host) != set.end())
    return true;
  size_t pos = 0;
  while ((pos = host.find('.', pos)) != std::string::npos) {
    ++pos;
    if (pos < host.size() && set.find(host.substr(pos)) != set.end())
      return true;
  }
  return false;
}

bool IsSameSite(const std::string &a, const std::string &b) {
  if (a.empty() || b.empty())
    return true;
  if (a == b)
    return true;
  if (HostMatchesOrSubdomain(a, b) || HostMatchesOrSubdomain(b, a))
    return true;
  auto last2 = [](const std::string &h) -> std::string_view {
    size_t d1 = h.rfind('.');
    if (d1 == std::string::npos || d1 == 0)
      return h;
    size_t d2 = h.rfind('.', d1 - 1);
    if (d2 == std::string::npos)
      return h;
    return std::string_view(h).substr(d2 + 1);
  };
  return last2(a) == last2(b);
}

bool IsPlainDomainString(std::string_view sv) {
  if (sv.empty() || sv.size() > 253)
    return false;
  for (unsigned char c : sv) {
    if (!(std::isalnum(c) || c == '.' || c == '-'))
      return false;
  }
  return sv.find('.') != std::string_view::npos;
}

bool WildcardMatch(std::string_view text, std::string_view pat) {
  if (pat.empty())
    return true;
  if (pat.find('*') == std::string_view::npos &&
      pat.find('^') == std::string_view::npos) {
    return text.find(pat) != std::string_view::npos;
  }
  // Split by '*' and check ordered chunks.
  size_t cur = 0;
  size_t p_idx = 0;
  while (p_idx < pat.size()) {
    size_t star = pat.find('*', p_idx);
    std::string_view seg = (star == std::string_view::npos)
                               ? pat.substr(p_idx)
                               : pat.substr(p_idx, star - p_idx);
    if (!seg.empty()) {
      bool sep_end = false;
      if (seg.back() == '^') {
        sep_end = true;
        seg.remove_suffix(1);
      }
      if (!seg.empty()) {
        size_t found = text.find(seg, cur);
        if (found == std::string_view::npos)
          return false;
        size_t end_pos = found + seg.size();
        if (sep_end && end_pos < text.size()) {
          unsigned char ch = static_cast<unsigned char>(text[end_pos]);
          if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '%')
            return false;
        }
        cur = end_pos;
      }
    }
    if (star == std::string_view::npos)
      break;
    p_idx = star + 1;
  }
  return true;
}

bool MatchRule(const NetRule &r, const std::string &host,
               const std::string &host_and_rest, const std::string &fp_host,
               bool is_third_party, uint32_t rt_mask) {
  if ((r.type_mask & rt_mask) == 0)
    return false;
  if (r.third_party_req == 1 && !is_third_party)
    return false;
  if (r.third_party_req == -1 && is_third_party)
    return false;

  if (!r.domain_anchor.empty() && !HostMatchesOrSubdomain(host, r.domain_anchor))
    return false;

  if (!r.exclude_domains.empty()) {
    for (const auto &ed : r.exclude_domains) {
      if (HostMatchesOrSubdomain(fp_host, ed))
        return false;
    }
  }
  if (!r.include_domains.empty()) {
    bool ok = false;
    for (const auto &id : r.include_domains) {
      if (HostMatchesOrSubdomain(fp_host, id)) {
        ok = true;
        break;
      }
    }
    if (!ok)
      return false;
  }

  if (r.pattern.empty())
    return true;
  if (!r.domain_anchor.empty()) {
    size_t pos = host_and_rest.find(r.domain_anchor);
    if (pos == std::string::npos)
      return false;
    std::string_view tail = std::string_view(host_and_rest).substr(pos + r.domain_anchor.size());
    return WildcardMatch(tail, r.pattern);
  }
  return WildcardMatch(host_and_rest, r.pattern);
}

bool ParseOptions(std::string_view opts_sv, NetRule &rule) {
  uint32_t include_mask = 0;
  uint32_t exclude_mask = 0;
  size_t pos = 0;
  while (pos < opts_sv.size()) {
    size_t comma = opts_sv.find(',', pos);
    std::string_view tok = (comma == std::string_view::npos)
                               ? opts_sv.substr(pos)
                               : opts_sv.substr(pos, comma - pos);
    pos = (comma == std::string_view::npos) ? opts_sv.size() : comma + 1;
    std::string opt = ToLowerAscii(TrimAscii(tok));
    if (opt.empty())
      continue;
    if (opt == "third-party" || opt == "3p") {
      rule.third_party_req = 1;
    } else if (opt == "~third-party" || opt == "~3p" || opt == "first-party" || opt == "1p") {
      rule.third_party_req = -1;
    } else if (opt.rfind("domain=", 0) == 0) {
      std::string_view doms = std::string_view(opt).substr(7);
      size_t dp = 0;
      while (dp < doms.size()) {
        size_t bar = doms.find('|', dp);
        std::string_view d = (bar == std::string_view::npos)
                                 ? doms.substr(dp)
                                 : doms.substr(dp, bar - dp);
        dp = (bar == std::string_view::npos) ? doms.size() : bar + 1;
        if (d.empty())
          continue;
        if (d.front() == '~')
          rule.exclude_domains.emplace_back(d.substr(1));
        else
          rule.include_domains.emplace_back(d);
      }
    } else if (opt == "script") {
      include_mask |= RT_SCRIPT;
    } else if (opt == "~script") {
      exclude_mask |= RT_SCRIPT;
    } else if (opt == "image") {
      include_mask |= RT_IMAGE;
    } else if (opt == "~image") {
      exclude_mask |= RT_IMAGE;
    } else if (opt == "stylesheet" || opt == "css") {
      include_mask |= RT_STYLESHEET;
    } else if (opt == "~stylesheet") {
      exclude_mask |= RT_STYLESHEET;
    } else if (opt == "xmlhttprequest" || opt == "xhr" || opt == "fetch") {
      include_mask |= RT_XHR;
    } else if (opt == "~xmlhttprequest" || opt == "~xhr") {
      exclude_mask |= RT_XHR;
    } else if (opt == "subdocument" || opt == "frame") {
      include_mask |= RT_SUBDOCUMENT;
    } else if (opt == "~subdocument") {
      exclude_mask |= RT_SUBDOCUMENT;
    } else if (opt == "font") {
      include_mask |= RT_FONT;
    } else if (opt == "media") {
      include_mask |= RT_MEDIA;
    } else if (opt == "other") {
      include_mask |= RT_OTHER;
    } else if (opt == "important" || opt == "match-case") {
      // Supported implicitly.
    } else if (opt.rfind("redirect=", 0) == 0 ||
               opt.rfind("csp=", 0) == 0 ||
               opt.rfind("removeparam=", 0) == 0 ||
               opt == "badfilter" || opt == "popup" || opt == "document") {
      // Unsupported advanced modifiers -> skip rule cleanly.
      return false;
    }
  }
  if (include_mask != 0)
    rule.type_mask = include_mask & ~exclude_mask;
  else if (exclude_mask != 0)
    rule.type_mask = RT_ANY & ~exclude_mask;
  return rule.type_mask != 0;
}

bool HasUnsupportedProceduralCss(std::string_view sel) {
  return sel.find(":has-text(") != std::string_view::npos ||
         sel.find(":-abp-") != std::string_view::npos ||
         sel.find(":upward(") != std::string_view::npos ||
         sel.find(":style(") != std::string_view::npos ||
         sel.find(":xpath(") != std::string_view::npos ||
         sel.find(":matches-path(") != std::string_view::npos;
}

} // namespace

const std::vector<FilterListSpec> &GetDefaultFilterListSpecs() {
  static const std::vector<FilterListSpec> kSpecs = {
      {"easylist", "EasyList", "https://easylist.to/easylist/easylist.txt"},
      {"easyprivacy", "EasyPrivacy",
       "https://easylist.to/easylist/easyprivacy.txt"},
      {"regional-fi", "EasyList Finland",
       "https://raw.githubusercontent.com/finnish-easylist-addition/"
       "finnish-easylist-addition/gh-pages/Finland_adb.txt"},
      {"nordic", "Nordic Filters",
       "https://raw.githubusercontent.com/DandelionSprout/adfilt/master/"
       "NorwegianExperimentalList%20alternate%20versions/"
       "NordicFiltersABP-Inclusion.txt"},
      {"cookies", "Fanboy Cookie Notices",
       "https://secure.fanboy.co.nz/fanboy-cookiemonster.txt"},
  };
  return kSpecs;
}

std::string CefAdblockEngine::ExtractHost(const std::string &url) {
  std::string h, rest;
  if (SplitHostAndRest(url, h, rest))
    return h;
  return "";
}

size_t CefAdblockEngine::ParseFilterTextInto(const std::string &text,
                                             FilterSnapshot &snap) {
  std::istringstream iss(text);
  std::string line;
  size_t added = 0;

  while (std::getline(iss, line)) {
    std::string trimmed = TrimAscii(line);
    if (trimmed.empty() || trimmed[0] == '!' || trimmed[0] == '[')
      continue;
    // Standard comment lines in hosts files start with '# ' or standalone '#'
    if (trimmed[0] == '#' &&
        (trimmed.size() == 1 || (trimmed[1] != '#' && trimmed[1] != '@' &&
                                 trimmed[1] != '?'))) {
      continue;
    }

    // Cosmetic hiding: [domains]##selector
    auto hh = trimmed.find("##");
    if (hh != std::string::npos) {
      std::string_view prefix(trimmed.data(), hh);
      std::string_view sel(trimmed.data() + hh + 2, trimmed.size() - hh - 2);
      if (sel.empty() || sel.front() == '+' || HasUnsupportedProceduralCss(sel))
        continue;
      CosmeticRule cr;
      cr.selector = TrimAscii(sel);
      if (cr.selector.empty() || cr.selector.find('{') != std::string::npos)
        continue;
      if (prefix.empty()) {
        if (snap.global_cosmetic.size() < 3000) {
          snap.global_cosmetic.push_back(std::move(cr));
          ++added;
        }
      } else {
        std::vector<std::string> inc;
        size_t p = 0;
        std::string low_p = ToLowerAscii(prefix);
        while (p < low_p.size()) {
          size_t c = low_p.find(',', p);
          std::string_view tok = (c == std::string::npos)
                                     ? std::string_view(low_p).substr(p)
                                     : std::string_view(low_p).substr(p, c - p);
          p = (c == std::string::npos) ? low_p.size() : c + 1;
          std::string t = TrimAscii(tok);
          if (t.empty())
            continue;
          if (t[0] == '~')
            cr.exclude_domains.push_back(t.substr(1));
          else
            inc.push_back(std::move(t));
        }
        if (inc.empty()) {
          if (snap.global_cosmetic.size() < 3000) {
            snap.global_cosmetic.push_back(std::move(cr));
            ++added;
          }
        } else {
          for (const auto &d : inc) {
            auto &vec = snap.domain_cosmetic[d];
            if (vec.size() < 128)
              vec.push_back(cr);
          }
          ++added;
        }
      }
      continue;
    }

    // Skip unsupported cosmetic/scriptlet exceptions
    if (trimmed.find("#@#") != std::string::npos ||
        trimmed.find("#?#") != std::string::npos) {
      continue;
    }

    // Hosts-file line: 0.0.0.0 domain.com [domain2 ...] or 127.0.0.1 domain.com
    if (trimmed.rfind("0.0.0.0 ", 0) == 0 || trimmed.rfind("127.0.0.1 ", 0) == 0) {
      std::istringstream hs(trimmed);
      std::string ip, h;
      hs >> ip;
      while (hs >> h) {
        if (h[0] == '#')
          break;
        std::string lh = ToLowerAscii(h);
        if (lh != "localhost" && IsPlainDomainString(lh)) {
          snap.block_domains.insert(std::move(lh));
          ++added;
        }
      }
      continue;
    }

    bool is_exc = false;
    std::string_view body = trimmed;
    if (body.rfind("@@", 0) == 0) {
      is_exc = true;
      body.remove_prefix(2);
    }

    std::string_view opts_sv;
    if (auto dol = body.rfind('$'); dol != std::string_view::npos) {
      opts_sv = body.substr(dol + 1);
      body = body.substr(0, dol);
    }
    if (body.empty())
      continue;
    // Skip regex literals (/.../) in initial native matcher cleanly.
    if (body.size() >= 2 && body.front() == '/' && body.back() == '/')
      continue;

    NetRule rule;
    rule.is_exception = is_exc;
    if (!opts_sv.empty() && !ParseOptions(opts_sv, rule))
      continue;

    if (body.rfind("||", 0) == 0) {
      std::string_view rest = body.substr(2);
      size_t sep = rest.find_first_of("^/*");
      std::string dom = ToLowerAscii(
          sep == std::string_view::npos ? rest : rest.substr(0, sep));
      std::string pat =
          sep == std::string_view::npos ? "" : ToLowerAscii(rest.substr(sep));
      if (pat == "^")
        pat.clear();
      else if (!pat.empty() && pat.front() == '^')
        pat.erase(pat.begin());

      if (!IsPlainDomainString(dom))
        continue;

      if (pat.empty() && rule.third_party_req == 0 && rule.type_mask == RT_ANY &&
          rule.include_domains.empty() && rule.exclude_domains.empty()) {
        if (is_exc)
          snap.exc_domains.insert(std::move(dom));
        else
          snap.block_domains.insert(std::move(dom));
        ++added;
      } else {
        rule.domain_anchor = dom;
        rule.pattern = std::move(pat);
        if (is_exc)
          snap.exc_rules.push_back(std::move(rule));
        else
          snap.domain_rules[std::move(dom)].push_back(std::move(rule));
        ++added;
      }
      continue;
    }

    if (body.rfind("|http://", 0) == 0 || body.rfind("|https://", 0) == 0) {
      body.remove_prefix(1);
    }
    std::string pat = ToLowerAscii(body);
    if (pat.size() < 4)
      continue;
    rule.pattern = std::move(pat);
    if (is_exc)
      snap.exc_rules.push_back(std::move(rule));
    else if (rule.include_domains.size() == 1)
      snap.domain_rules[rule.include_domains[0]].push_back(std::move(rule));
    else
      snap.generic_rules.push_back(std::move(rule));
    ++added;
  }

  snap.total_rules += added;
  return added;
}

CefAdblockEngine::CefAdblockEngine() {
  auto snap = std::make_shared<FilterSnapshot>();
  snap->baseline_rules = ParseFilterTextInto(kBuiltinBaselineRules, *snap);
  snap->loaded_lists = 1;
  snapshot_ = std::move(snap);
}

void CefAdblockEngine::SetEnabled(bool enabled) {
  std::lock_guard<std::mutex> lk(cfg_mu_);
  enabled_ = enabled;
}

bool CefAdblockEngine::IsEnabled() const {
  std::lock_guard<std::mutex> lk(cfg_mu_);
  return enabled_;
}

void CefAdblockEngine::SetDirectories(const std::string &cache_dir,
                                      const std::string &data_dir) {
  {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    cache_dir_ = cache_dir;
    data_dir_ = data_dir;
  }
  LoadAllowlist();
}

void CefAdblockEngine::LoadAllowlist() {
  std::string dir;
  {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    dir = data_dir_;
    paused_hosts_.clear();
  }
  if (dir.empty())
    return;
  fs::path p = fs::path(dir) / "adblock-allowlist.txt";
  std::ifstream in(p);
  if (!in.is_open())
    return;
  std::unordered_set<std::string> hosts;
  std::string line;
  while (std::getline(in, line)) {
    std::string h = ToLowerAscii(TrimAscii(line));
    if (!h.empty())
      hosts.insert(std::move(h));
  }
  std::lock_guard<std::mutex> lk(cfg_mu_);
  paused_hosts_ = std::move(hosts);
}

void CefAdblockEngine::SaveAllowlistLocked() const {
  if (data_dir_.empty())
    return;
  std::error_code ec;
  fs::create_directories(data_dir_, ec);
  fs::path p = fs::path(data_dir_) / "adblock-allowlist.txt";
  std::ofstream out(p, std::ios::trunc);
  if (!out.is_open())
    return;
  for (const auto &h : paused_hosts_) {
    out << h << "\n";
  }
}

void CefAdblockEngine::ReloadFromDisk(const std::string &custom_rules_path) {
  auto snap = std::make_shared<FilterSnapshot>();
  snap->baseline_rules = ParseFilterTextInto(kBuiltinBaselineRules, *snap);
  snap->loaded_lists = 1;

  std::string cdir;
  {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    cdir = cache_dir_;
  }

  if (!cdir.empty()) {
    fs::path ab_dir = fs::path(cdir) / "adblock";
    for (const auto &spec : GetDefaultFilterListSpecs()) {
      fs::path f = ab_dir / (std::string(spec.slug) + ".txt");
      std::ifstream in(f);
      if (in.is_open()) {
        std::ostringstream ss;
        ss << in.rdbuf();
        size_t n = ParseFilterTextInto(ss.str(), *snap);
        snap->list_rule_counts[spec.slug] = n;
        if (n > 0)
          ++snap->loaded_lists;
      }
    }
  }

  if (!custom_rules_path.empty()) {
    std::ifstream in(custom_rules_path);
    if (in.is_open()) {
      std::ostringstream ss;
      ss << in.rdbuf();
      if (ParseFilterTextInto(ss.str(), *snap) > 0)
        ++snap->loaded_lists;
    }
  }

  std::lock_guard<std::mutex> lk(snap_mu_);
  snapshot_ = std::move(snap);
}

bool CefAdblockEngine::WriteListFileOnly(const std::string &slug,
                                         const std::string &content) {
  std::string cdir;
  {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    cdir = cache_dir_;
  }
  if (cdir.empty())
    return false;
  std::error_code ec;
  fs::path ab_dir = fs::path(cdir) / "adblock";
  fs::create_directories(ab_dir, ec);
  fs::path tmp = ab_dir / (slug + ".txt.tmp");
  fs::path dst = ab_dir / (slug + ".txt");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
      return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out.good())
      return false;
  }
  fs::rename(tmp, dst, ec);
  return !ec;
}

bool CefAdblockEngine::SaveListAndReload(const std::string &slug,
                                           const std::string &content,
                                           const std::string &custom_rules_path) {
  if (!WriteListFileOnly(slug, content))
    return false;
  ReloadFromDisk(custom_rules_path);
  return true;
}

bool CefAdblockEngine::HasMissingOrStaleLists(int max_age_hours) const {
  std::string cdir;
  {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    cdir = cache_dir_;
  }
  if (cdir.empty())
    return false;
  fs::path ab_dir = fs::path(cdir) / "adblock";
  const auto now = fs::file_time_type::clock::now();
  const auto max_age = std::chrono::hours(std::max(1, max_age_hours));
  for (const auto &spec : GetDefaultFilterListSpecs()) {
    fs::path f = ab_dir / (std::string(spec.slug) + ".txt");
    std::error_code ec;
    if (!fs::exists(f, ec) || fs::file_size(f, ec) == 0 || ec) {
      return true;
    }
    auto mtime = fs::last_write_time(f, ec);
    if (ec || (now - mtime) > max_age) {
      return true;
    }
  }
  return false;
}

void CefAdblockEngine::LoadAdditionalRulesText(const std::string &text) {
  auto next = std::make_shared<FilterSnapshot>();
  if (auto cur = GetSnapshot()) {
    *next = *cur;
  }
  if (ParseFilterTextInto(text, *next) > 0) {
    ++next->loaded_lists;
  }
  std::lock_guard<std::mutex> lk(snap_mu_);
  snapshot_ = std::move(next);
}

std::shared_ptr<const FilterSnapshot> CefAdblockEngine::GetSnapshot() const {
  std::lock_guard<std::mutex> lk(snap_mu_);
  return snapshot_;
}

bool CefAdblockEngine::ShouldBlock(const std::string &url,
                                   const std::string &first_party_url,
                                   uint32_t resource_type_mask) const {
  if (resource_type_mask & RT_MAIN_FRAME)
    return false;

  std::string host, rest;
  if (!SplitHostAndRest(url, host, rest))
    return false;

  std::string fp_host = ExtractHost(first_party_url);
  if (fp_host.empty())
    fp_host = host;

  {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    if (!enabled_)
      return false;
    if (HasDomainInSet(fp_host, paused_hosts_))
      return false;
  }

  auto snap = GetSnapshot();
  if (!snap)
    return false;

  bool is_3p = !IsSameSite(host, fp_host);
  std::string host_and_rest = host + rest;

  if (HasDomainInSet(host, snap->exc_domains))
    return false;
  for (const auto &er : snap->exc_rules) {
    if (MatchRule(er, host, host_and_rest, fp_host, is_3p, resource_type_mask))
      return false;
  }

  if (HasDomainInSet(host, snap->block_domains))
    return true;

  auto check_bucket = [&](const std::string &key) -> bool {
    auto it = snap->domain_rules.find(key);
    if (it == snap->domain_rules.end())
      return false;
    for (const auto &r : it->second) {
      if (MatchRule(r, host, host_and_rest, fp_host, is_3p, resource_type_mask))
        return true;
    }
    return false;
  };

  if (check_bucket(host))
    return true;
  size_t pos = 0;
  while ((pos = host.find('.', pos)) != std::string::npos) {
    ++pos;
    if (pos < host.size() && check_bucket(host.substr(pos)))
      return true;
  }
  if (!fp_host.empty() && fp_host != host) {
    if (check_bucket(fp_host))
      return true;
    pos = 0;
    while ((pos = fp_host.find('.', pos)) != std::string::npos) {
      ++pos;
      if (pos < fp_host.size() && check_bucket(fp_host.substr(pos)))
        return true;
    }
  }

  for (const auto &r : snap->generic_rules) {
    if (MatchRule(r, host, host_and_rest, fp_host, is_3p, resource_type_mask))
      return true;
  }
  return false;
}

static std::string NormalizeHostArg(const std::string &host_or_url) {
  std::string h;
  if (host_or_url.find("://") != std::string::npos) {
    h = CefAdblockEngine::ExtractHost(host_or_url);
  } else {
    h = ToLowerAscii(TrimAscii(host_or_url));
  }
  if (h.rfind("www.", 0) == 0 && h.find('.', 4) != std::string::npos) {
    h = h.substr(4);
  }
  return h;
}

std::string CefAdblockEngine::GetCosmeticCssForHost(const std::string &host) const {
  std::string lh = NormalizeHostArg(host);
  if (lh.empty())
    return "";
  {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    if (!enabled_ || HasDomainInSet(lh, paused_hosts_))
      return "";
  }
  auto snap = GetSnapshot();
  if (!snap)
    return "";

  std::vector<std::string> selectors;
  auto collect_vec = [&](const std::vector<CosmeticRule> &vec) {
    for (const auto &cr : vec) {
      bool excluded = false;
      for (const auto &ed : cr.exclude_domains) {
        if (HostMatchesOrSubdomain(lh, ed)) {
          excluded = true;
          break;
        }
      }
      if (!excluded)
        selectors.push_back(cr.selector);
    }
  };

  if (auto it = snap->domain_cosmetic.find(lh); it != snap->domain_cosmetic.end())
    collect_vec(it->second);
  size_t pos = 0;
  while ((pos = lh.find('.', pos)) != std::string::npos) {
    ++pos;
    if (pos < lh.size()) {
      if (auto it = snap->domain_cosmetic.find(lh.substr(pos));
          it != snap->domain_cosmetic.end()) {
        collect_vec(it->second);
      }
    }
  }
  collect_vec(snap->global_cosmetic);

  if (selectors.empty())
    return "";

  std::ostringstream css;
  size_t limit = std::min<size_t>(selectors.size(), 1500);
  for (size_t i = 0; i < limit; ++i) {
    if (i > 0)
      css << ",";
    css << selectors[i];
  }
  css << "{display:none!important;}";
  return css.str();
}

bool CefAdblockEngine::IsHostPaused(const std::string &host) const {
  std::string lh = NormalizeHostArg(host);
  if (lh.empty())
    return false;
  std::lock_guard<std::mutex> lk(cfg_mu_);
  return HasDomainInSet(lh, paused_hosts_);
}

bool CefAdblockEngine::ToggleHostPaused(const std::string &host) {
  std::string lh = NormalizeHostArg(host);
  if (lh.empty())
    return false;
  std::lock_guard<std::mutex> lk(cfg_mu_);
  bool now_paused = false;
  if (paused_hosts_.find(lh) != paused_hosts_.end()) {
    paused_hosts_.erase(lh);
    now_paused = false;
  } else {
    paused_hosts_.insert(std::move(lh));
    now_paused = true;
  }
  SaveAllowlistLocked();
  return now_paused;
}

size_t CefAdblockEngine::GetPausedHostCount() const {
  std::lock_guard<std::mutex> lk(cfg_mu_);
  return paused_hosts_.size();
}

std::vector<std::string> CefAdblockEngine::GetPausedHosts() const {
  std::lock_guard<std::mutex> lk(cfg_mu_);
  std::vector<std::string> out(paused_hosts_.begin(), paused_hosts_.end());
  std::sort(out.begin(), out.end());
  return out;
}

size_t CefAdblockEngine::GetRuleCount() const {
  auto s = GetSnapshot();
  return s ? s->total_rules : 0;
}

size_t CefAdblockEngine::GetBaselineRuleCount() const {
  auto s = GetSnapshot();
  return s ? s->baseline_rules : 0;
}

size_t CefAdblockEngine::GetRuleCountForList(const std::string &slug) const {
  auto s = GetSnapshot();
  if (!s)
    return 0;
  auto it = s->list_rule_counts.find(slug);
  return it != s->list_rule_counts.end() ? it->second : 0;
}

size_t CefAdblockEngine::GetLoadedListCount() const {
  auto s = GetSnapshot();
  return s ? s->loaded_lists : 0;
}

void CefAdblockEngine::RecordBlockedForTab(int browser_id) {
  std::lock_guard<std::mutex> lk(count_mu_);
  ++blocked_per_tab_[browser_id];
  ++total_blocked_;
}

void CefAdblockEngine::ResetTabBlockedCount(int browser_id) {
  std::lock_guard<std::mutex> lk(count_mu_);
  blocked_per_tab_[browser_id] = 0;
}

uint64_t CefAdblockEngine::GetBlockedCountForTab(int browser_id) const {
  std::lock_guard<std::mutex> lk(count_mu_);
  auto it = blocked_per_tab_.find(browser_id);
  return it != blocked_per_tab_.end() ? it->second : 0;
}

uint64_t CefAdblockEngine::GetTotalBlockedCount() const {
  std::lock_guard<std::mutex> lk(count_mu_);
  return total_blocked_;
}

} // namespace kajo_web
