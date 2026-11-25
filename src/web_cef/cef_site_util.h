#ifndef KAJO_WEB_CEF_SITE_UTIL_H
#define KAJO_WEB_CEF_SITE_UTIL_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace kajo_web {

inline std::string ToLowerAsciiCopy(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  for (unsigned char c : sv) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

inline bool IsIpLiteralHost(const std::string& host) {
  if (host.find(':') != std::string::npos) return true;
  bool all_digits_or_dots = !host.empty();
  for (unsigned char c : host) {
    if (!std::isdigit(c) && c != '.') {
      all_digits_or_dots = false;
      break;
    }
  }
  return all_digits_or_dots;
}

inline std::string RegistrableSiteHost(const std::string& raw_host) {
  std::string host = ToLowerAsciiCopy(raw_host);
  while (!host.empty() && host.back() == '.') host.pop_back();
  if (host.empty() || IsIpLiteralHost(host)) return host;

  std::vector<std::string> labels;
  size_t pos = 0;
  while (pos < host.size()) {
    size_t dot = host.find('.', pos);
    if (dot == std::string::npos) {
      labels.push_back(host.substr(pos));
      break;
    }
    if (dot > pos) labels.push_back(host.substr(pos, dot - pos));
    pos = dot + 1;
  }
  if (labels.size() <= 2) return host;

  static const char* kMultiTlds[] = {
      "co.uk", "org.uk", "ac.uk", "gov.uk",
      "com.au", "net.au", "org.au",
      "co.jp", "ne.jp", "or.jp",
      "com.br", "co.nz", "co.za", "co.kr",
      "com.cn", "com.tw", "com.hk",
      "co.in", "com.mx", "com.tr", "com.sg",
  };
  const std::string last2 = labels[labels.size() - 2] + "." + labels[labels.size() - 1];
  for (const char* tld : kMultiTlds) {
    if (last2 == tld && labels.size() >= 3) {
      return labels[labels.size() - 3] + "." + last2;
    }
  }
  return last2;
}

inline bool IsSameSiteHost(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return true;
  const std::string la = ToLowerAsciiCopy(a);
  const std::string lb = ToLowerAsciiCopy(b);
  if (la == lb) return true;
  return RegistrableSiteHost(la) == RegistrableSiteHost(lb);
}

inline bool IsLocalOrPrivateHost(const std::string& raw_host) {
  std::string host = ToLowerAsciiCopy(raw_host);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  while (!host.empty() && host.back() == '.') host.pop_back();
  if (host.empty()) return false;
  if (host == "localhost" || host == "::1") return true;
  if (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0) return true;
  if (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0) return true;
  if (host.rfind("127.", 0) == 0 || host.rfind("10.", 0) == 0 || host.rfind("192.168.", 0) == 0) {
    return true;
  }
  if (host.rfind("172.", 0) == 0) {
    size_t dot2 = host.find('.', 4);
    if (dot2 != std::string::npos && dot2 > 4) {
      bool all_digits = true;
      for (size_t i = 4; i < dot2; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(host[i]))) {
          all_digits = false;
          break;
        }
      }
      if (all_digits) {
        long second = std::strtol(host.substr(4, dot2 - 4).c_str(), nullptr, 10);
        if (second >= 16 && second <= 31) return true;
      }
    }
  }
  return false;
}

inline bool IsSafeHexColor(const std::string& s) {
  if (s.size() != 7 || s[0] != '#') return false;
  for (size_t i = 1; i < 7; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

inline std::string SanitizeHexColor(const std::string& s, const char* fallback = "#0078d4") {
  if (IsSafeHexColor(s)) return s;
  return fallback ? std::string(fallback) : std::string("#0078d4");
}

inline std::string SanitizeSafeHref(const std::string& url) {
  std::string trimmed = url;
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
    trimmed.erase(trimmed.begin());
  }
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
    trimmed.pop_back();
  }
  if (trimmed.empty()) return "#";
  const std::string low = ToLowerAsciiCopy(trimmed);
  if (low.rfind("http://", 0) == 0 ||
      low.rfind("https://", 0) == 0 ||
      low.rfind("kajo://", 0) == 0 ||
      low.rfind("file://", 0) == 0 ||
      low == "about:blank") {
    return trimmed;
  }
  return "#";
}

inline std::string DeriveOriginFaviconFallback(const std::string& url) {
  if (url.empty()) return "";
  for (unsigned char c : url) {
    if (c <= 0x20 || c == 0x7f) return "";
  }
  const std::string low = ToLowerAsciiCopy(url);
  std::string scheme;
  size_t pos = 0;
  if (low.rfind("https://", 0) == 0) {
    scheme = "https";
    pos = 8;
  } else if (low.rfind("http://", 0) == 0) {
    scheme = "http";
    pos = 7;
  } else {
    return "";
  }
  size_t end = url.find_first_of("/?#", pos);
  std::string auth = (end == std::string::npos) ? url.substr(pos) : url.substr(pos, end - pos);
  size_t at = auth.rfind('@');
  if (at != std::string::npos) auth = auth.substr(at + 1);
  if (auth.empty()) return "";
  if (auth.front() == '[') {
    size_t rb = auth.find(']');
    if (rb == std::string::npos || rb <= 1) return "";
    if (rb + 1 < auth.size()) {
      if (auth[rb + 1] != ':' || rb + 2 >= auth.size()) return "";
      for (size_t i = rb + 2; i < auth.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(auth[i]))) return "";
      }
    }
  } else {
    size_t colon = auth.find(':');
    if (colon != std::string::npos) {
      if (colon == 0) return "";
      if (colon + 1 >= auth.size()) {
        auth = auth.substr(0, colon);
      } else {
        for (size_t i = colon + 1; i < auth.size(); ++i) {
          if (!std::isdigit(static_cast<unsigned char>(auth[i]))) return "";
        }
      }
    }
  }
  return scheme + "://" + ToLowerAsciiCopy(auth) + "/favicon.ico";
}

struct ChromeUiMetrics {
  int topbar_height_px;
  int control_height_px;
  int icon_size_px;
  int infobar_height_px;
  int findbar_height_px;
  int compact_rail_width_px;
};

inline ChromeUiMetrics ComputeChromeUiMetrics(int ui_font_size_px) {
  const int fs = std::clamp(ui_font_size_px, 11, 20);
  ChromeUiMetrics m{};
  m.topbar_height_px = std::max(40, fs * 2 + 14);
  m.control_height_px = m.topbar_height_px - 12;
  m.icon_size_px = std::clamp(fs + 2, 14, 22);
  m.infobar_height_px = std::max(42, m.topbar_height_px + 2);
  m.findbar_height_px = std::max(48, m.topbar_height_px + 8);
  m.compact_rail_width_px = std::max(48, m.control_height_px + 18);
  return m;
}

enum class ZoomShortcutAction {
  kNone = 0,
  kZoomIn,
  kZoomOut,
  kZoomReset,
};

inline ZoomShortcutAction ClassifyZoomShortcutKey(int vk, int ch, int unmod_ch) {
  if (vk == 0xBB /*VK_OEM_PLUS*/ || vk == 0x6B /*VK_ADD*/ ||
      ch == '+' || ch == '=' || unmod_ch == '+' || unmod_ch == '=') {
    return ZoomShortcutAction::kZoomIn;
  }
  if (vk == 0xBD /*VK_OEM_MINUS*/ || vk == 0x6D /*VK_SUBTRACT*/ ||
      ch == '-' || unmod_ch == '-') {
    return ZoomShortcutAction::kZoomOut;
  }
  if (vk == '0' || vk == 0x60 /*VK_NUMPAD0*/ ||
      ch == '0' || unmod_ch == '0') {
    return ZoomShortcutAction::kZoomReset;
  }
  return ZoomShortcutAction::kNone;
}

inline double ZoomPercentToCefLevel(int zoom_pct) {
  if (zoom_pct <= 0 || zoom_pct == 100) return 0.0;
  return std::log(static_cast<double>(zoom_pct) / 100.0) / std::log(1.2);
}

inline std::string StripMenuMnemonic(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '&') {
      if (i + 1 < raw.size() && raw[i + 1] == '&') {
        out.push_back('&');
        ++i;
      } else if (i + 1 < raw.size() &&
                 !std::isspace(static_cast<unsigned char>(raw[i + 1])) &&
                 static_cast<unsigned char>(raw[i + 1]) >= 0x20) {
        continue;
      } else {
        out.push_back('&');
      }
    } else {
      out.push_back(raw[i]);
    }
  }
  return out;
}

inline std::string FormatMenuAccelerator(int key_code, bool shift, bool ctrl, bool alt) {
  if (key_code <= 0) return "";
  std::string key;
  if ((key_code >= 'A' && key_code <= 'Z') || (key_code >= '0' && key_code <= '9')) {
    key.push_back(static_cast<char>(key_code));
  } else if (key_code >= 0x70 && key_code <= 0x7B) {
    key = "F" + std::to_string(key_code - 0x70 + 1);
  } else {
    switch (key_code) {
      case 0x08: key = "Bksp"; break;
      case 0x09: key = "Tab"; break;
      case 0x0D: key = "Enter"; break;
      case 0x1B: key = "Esc"; break;
      case 0x20: key = "Space"; break;
      case 0x25: key = "←"; break;
      case 0x26: key = "↑"; break;
      case 0x27: key = "→"; break;
      case 0x28: key = "↓"; break;
      case 0x2E: key = "Del"; break;
      case 0xBB: key = "+"; break;
      case 0xBD: key = "-"; break;
      default: return "";
    }
  }
  std::string out;
  if (ctrl) out += "Ctrl+";
  if (alt) out += "Alt+";
  if (shift) out += "Shift+";
  out += key;
  return out;
}

struct ContextMenuBounds {
  int x = 0;
  int y = 0;
  int max_h = 80;
};

inline ContextMenuBounds ComputeContextMenuBounds(int anchor_x,
                                                  int anchor_y,
                                                  int menu_w,
                                                  int menu_h,
                                                  int viewport_w,
                                                  int viewport_h,
                                                  int margin = 6) {
  ContextMenuBounds b;
  const int safe_w = std::max(1, menu_w);
  const int safe_h = std::max(1, menu_h);
  const int m = std::max(0, margin);
  b.max_h = std::max(80, viewport_h - 2 * m);

  if (anchor_x + safe_w + m <= viewport_w) {
    b.x = anchor_x;
  } else if (anchor_x - safe_w >= m) {
    b.x = anchor_x - safe_w;
  } else {
    const int max_x = std::max(m, viewport_w - safe_w - m);
    b.x = std::clamp(anchor_x, m, max_x);
  }

  const int eff_h = std::min(safe_h, b.max_h);
  if (anchor_y + eff_h + m <= viewport_h) {
    b.y = anchor_y;
  } else if (anchor_y - eff_h >= m) {
    b.y = anchor_y - eff_h;
  } else {
    const int max_y = std::max(m, viewport_h - eff_h - m);
    b.y = std::clamp(anchor_y, m, max_y);
  }
  return b;
}

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_SITE_UTIL_H
