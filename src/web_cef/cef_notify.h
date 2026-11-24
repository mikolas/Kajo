#ifndef KAJO_WEB_CEF_NOTIFY_H
#define KAJO_WEB_CEF_NOTIFY_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace kajo_web {

struct WebNotificationRequest {
  std::string origin;
  std::string title;
  std::string body;
  std::string tag;
  std::string desktop_entry;   /* e.g. "kajo-webapp-default-teams" or "org.kajo.Web" */
  std::string app_name;        /* e.g. "Microsoft Teams" or "Kajo Web" */
  std::string icon_path;       /* local icon path or themed name; falls back to "org.kajo.Web" */
  int browser_id = 0;
  uint64_t tab_id = 0;
  std::string app_id;          /* non-empty if triggered inside a WebApp window */
  std::string js_notif_id;     /* renderer correlation token for click/close events */
};

struct WebNotificationTarget {
  int browser_id = 0;
  uint64_t tab_id = 0;
  std::string app_id;
  std::string js_notif_id;
};

/* Async org.freedesktop.Notifications D-Bus bridge running on its own GLib
 * GMainContext worker thread so signal delivery never depends on Ozone's pump
 * and never blocks Chromium's TID_UI thread. */
class WebNotificationBridge {
public:
  using ClickCallback = std::function<void(const WebNotificationTarget&)>;
  using CloseCallback = std::function<void(const WebNotificationTarget&, uint32_t reason)>;

  WebNotificationBridge(ClickCallback on_click, CloseCallback on_close);
  ~WebNotificationBridge();

  WebNotificationBridge(const WebNotificationBridge&) = delete;
  WebNotificationBridge& operator=(const WebNotificationBridge&) = delete;

  void Start();
  void Stop();

  /* Safe to call from any thread (including TID_UI). */
  void SendNotification(WebNotificationRequest req);
  void CloseByJsId(const std::string& js_notif_id);

  struct Impl;

private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_NOTIFY_H
