#include "web_cef/cef_notify.h"

#include <gio/gio.h>

#include <utility>

namespace kajo_web {

struct WebNotificationBridge::Impl : public std::enable_shared_from_this<Impl> {
  explicit Impl(ClickCallback click_cb, CloseCallback close_cb)
      : on_click(std::move(click_cb)), on_close(std::move(close_cb)) {}

  ClickCallback on_click;
  CloseCallback on_close;

  std::mutex mu;
  std::atomic<bool> started{false};
  std::atomic<bool> stopping{false};
  std::thread worker;

  GMainContext* ctx = nullptr;
  GMainLoop* loop = nullptr;
  GDBusConnection* conn = nullptr;
  GCancellable* cancellable = nullptr;
  guint action_sub_id = 0;
  guint closed_sub_id = 0;

  std::unordered_map<uint32_t, WebNotificationTarget> active_by_dbus_id;
  std::unordered_map<std::string, uint32_t> dbus_id_by_js_id;
  /* Supports "tag" replacement semantics on (origin, tag). */
  std::unordered_map<std::string, uint32_t> dbus_id_by_tag_key;
  std::vector<WebNotificationRequest> queued_sends;
};

namespace {

struct PendingSend {
  std::weak_ptr<WebNotificationBridge::Impl> weak_impl;
  WebNotificationRequest req;
};

struct PendingClose {
  std::weak_ptr<WebNotificationBridge::Impl> weak_impl;
  std::string js_notif_id;
};

struct NotifyCallData {
  std::weak_ptr<WebNotificationBridge::Impl> weak_impl;
  WebNotificationTarget target;
  std::string tag_key;
};

void OnActionInvoked(GDBusConnection* /*conn*/,
                     const gchar* /*sender_name*/,
                     const gchar* /*object_path*/,
                     const gchar* /*interface_name*/,
                     const gchar* /*signal_name*/,
                     GVariant* parameters,
                     gpointer user_data) {
  auto* weak = static_cast<std::weak_ptr<WebNotificationBridge::Impl>*>(user_data);
  if (!weak) return;
  auto impl = weak->lock();
  if (!impl || impl->stopping.load()) return;

  if (!parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)"))) return;
  guint32 nid = 0;
  const gchar* action_key = nullptr;
  g_variant_get(parameters, "(u&s)", &nid, &action_key);

  WebNotificationTarget target;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    auto it = impl->active_by_dbus_id.find(nid);
    if (it != impl->active_by_dbus_id.end()) {
      target = it->second;
      found = true;
    }
  }
  if (found && impl->on_click) {
    impl->on_click(target);
  }
}

void OnNotificationClosed(GDBusConnection* /*conn*/,
                          const gchar* /*sender_name*/,
                          const gchar* /*object_path*/,
                          const gchar* /*interface_name*/,
                          const gchar* /*signal_name*/,
                          GVariant* parameters,
                          gpointer user_data) {
  auto* weak = static_cast<std::weak_ptr<WebNotificationBridge::Impl>*>(user_data);
  if (!weak) return;
  auto impl = weak->lock();
  if (!impl || impl->stopping.load()) return;

  if (!parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uu)"))) return;
  guint32 nid = 0;
  guint32 reason = 0;
  g_variant_get(parameters, "(uu)", &nid, &reason);

  WebNotificationTarget target;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    auto it = impl->active_by_dbus_id.find(nid);
    if (it != impl->active_by_dbus_id.end()) {
      target = it->second;
      found = true;
      if (!target.js_notif_id.empty()) {
        impl->dbus_id_by_js_id.erase(target.js_notif_id);
      }
      for (auto tit = impl->dbus_id_by_tag_key.begin();
           tit != impl->dbus_id_by_tag_key.end();) {
        if (tit->second == nid) {
          tit = impl->dbus_id_by_tag_key.erase(tit);
        } else {
          ++tit;
        }
      }
      impl->active_by_dbus_id.erase(it);
    }
  }
  if (found && impl->on_close) {
    impl->on_close(target, reason);
  }
}

void FreeWeakPtr(gpointer data) {
  delete static_cast<std::weak_ptr<WebNotificationBridge::Impl>*>(data);
}

void OnNotifyReply(GObject* source, GAsyncResult* res, gpointer user_data) {
  std::unique_ptr<NotifyCallData> data(static_cast<NotifyCallData*>(user_data));
  g_autoptr(GError) err = nullptr;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
  if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;

  auto impl = data->weak_impl.lock();
  if (!impl || impl->stopping.load()) return;

  if (!reply || err || !g_variant_is_of_type(reply, G_VARIANT_TYPE("(u)"))) {
    return;
  }

  guint32 nid = 0;
  g_variant_get(reply, "(u)", &nid);
  if (nid == 0) return;

  std::lock_guard<std::mutex> lock(impl->mu);
  impl->active_by_dbus_id[nid] = data->target;
  if (!data->target.js_notif_id.empty()) {
    impl->dbus_id_by_js_id[data->target.js_notif_id] = nid;
  }
  if (!data->tag_key.empty()) {
    impl->dbus_id_by_tag_key[data->tag_key] = nid;
  }
}

gboolean DoSendOnWorker(gpointer user_data) {
  std::unique_ptr<PendingSend> pending(static_cast<PendingSend*>(user_data));
  auto impl = pending->weak_impl.lock();
  if (!impl || impl->stopping.load() || !impl->conn) return G_SOURCE_REMOVE;

  const auto& r = pending->req;
  std::string tag_key;
  uint32_t replaces_id = 0;
  if (!r.tag.empty()) {
    tag_key = r.origin + "\n" + r.tag;
    std::lock_guard<std::mutex> lock(impl->mu);
    auto it = impl->dbus_id_by_tag_key.find(tag_key);
    if (it != impl->dbus_id_by_tag_key.end()) replaces_id = it->second;
  }

  const std::string app_name = !r.app_name.empty() ? r.app_name : "Kajo Web";
  const std::string app_icon = !r.icon_path.empty() ? r.icon_path : "org.kajo.Web";
  const std::string summary = !r.title.empty() ? r.title : app_name;
  const std::string desktop_entry =
      !r.desktop_entry.empty() ? r.desktop_entry : "org.kajo.Web";

  GVariantBuilder actions_b;
  g_variant_builder_init(&actions_b, G_VARIANT_TYPE("as"));
  g_variant_builder_add(&actions_b, "s", "default");
  g_variant_builder_add(&actions_b, "s", "Open");

  GVariantBuilder hints_b;
  g_variant_builder_init(&hints_b, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&hints_b, "{sv}", "desktop-entry",
                        g_variant_new_string(desktop_entry.c_str()));
  if (!r.icon_path.empty() && r.icon_path[0] == '/') {
    g_variant_builder_add(&hints_b, "{sv}", "image-path",
                          g_variant_new_string(r.icon_path.c_str()));
  }

  GVariant* params = g_variant_new("(susss@as@a{sv}i)",
                                   app_name.c_str(),
                                   replaces_id,
                                   app_icon.c_str(),
                                   summary.c_str(),
                                   r.body.c_str(),
                                   g_variant_builder_end(&actions_b),
                                   g_variant_builder_end(&hints_b),
                                   -1);

  auto* call_data = new NotifyCallData();
  call_data->weak_impl = impl;
  call_data->target.browser_id = r.browser_id;
  call_data->target.tab_id = r.tab_id;
  call_data->target.app_id = r.app_id;
  call_data->target.js_notif_id = r.js_notif_id;
  call_data->tag_key = std::move(tag_key);

  g_dbus_connection_call(impl->conn,
                         "org.freedesktop.Notifications",
                         "/org/freedesktop/Notifications",
                         "org.freedesktop.Notifications",
                         "Notify",
                         params,
                         G_VARIANT_TYPE("(u)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         5000,
                         impl->cancellable,
                         OnNotifyReply,
                         call_data);
  return G_SOURCE_REMOVE;
}

gboolean DoCloseOnWorker(gpointer user_data) {
  std::unique_ptr<PendingClose> pending(static_cast<PendingClose*>(user_data));
  auto impl = pending->weak_impl.lock();
  if (!impl || impl->stopping.load() || !impl->conn) return G_SOURCE_REMOVE;

  uint32_t nid = 0;
  {
    std::lock_guard<std::mutex> lock(impl->mu);
    auto it = impl->dbus_id_by_js_id.find(pending->js_notif_id);
    if (it != impl->dbus_id_by_js_id.end()) {
      nid = it->second;
      impl->dbus_id_by_js_id.erase(it);
      impl->active_by_dbus_id.erase(nid);
    }
  }
  if (nid != 0) {
    g_dbus_connection_call(impl->conn,
                           "org.freedesktop.Notifications",
                           "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications",
                           "CloseNotification",
                           g_variant_new("(u)", nid),
                           nullptr,
                           G_DBUS_CALL_FLAGS_NONE,
                           3000,
                           impl->cancellable,
                           nullptr,
                           nullptr);
  }
  return G_SOURCE_REMOVE;
}

gboolean QuitLoopOnWorker(gpointer user_data) {
  auto* loop = static_cast<GMainLoop*>(user_data);
  if (loop) g_main_loop_quit(loop);
  return G_SOURCE_REMOVE;
}

}  // namespace

WebNotificationBridge::WebNotificationBridge(ClickCallback on_click, CloseCallback on_close)
    : impl_(std::make_shared<Impl>(std::move(on_click), std::move(on_close))) {}

WebNotificationBridge::~WebNotificationBridge() {
  Stop();
}

void WebNotificationBridge::Start() {
  bool expected = false;
  if (!impl_->started.compare_exchange_strong(expected, true)) return;
  impl_->stopping.store(false);

  std::weak_ptr<Impl> weak_impl = impl_;
  impl_->worker = std::thread([weak_impl]() {
    auto impl = weak_impl.lock();
    if (!impl || impl->stopping.load()) return;

    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GMainLoop* loop = g_main_loop_new(ctx, FALSE);
    GCancellable* cancel = g_cancellable_new();

    g_autoptr(GError) bus_err = nullptr;
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, cancel, &bus_err);

    guint action_id = 0;
    guint closed_id = 0;
    if (conn) {
      action_id = g_dbus_connection_signal_subscribe(
          conn,
          "org.freedesktop.Notifications",
          "org.freedesktop.Notifications",
          "ActionInvoked",
          "/org/freedesktop/Notifications",
          nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE,
          OnActionInvoked,
          new std::weak_ptr<Impl>(impl),
          FreeWeakPtr);
      closed_id = g_dbus_connection_signal_subscribe(
          conn,
          "org.freedesktop.Notifications",
          "org.freedesktop.Notifications",
          "NotificationClosed",
          "/org/freedesktop/Notifications",
          nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE,
          OnNotificationClosed,
          new std::weak_ptr<Impl>(impl),
          FreeWeakPtr);
    }

    std::vector<WebNotificationRequest> pending_drain;
    {
      std::lock_guard<std::mutex> lock(impl->mu);
      impl->ctx = ctx;
      impl->loop = loop;
      impl->conn = conn;
      impl->cancellable = cancel;
      impl->action_sub_id = action_id;
      impl->closed_sub_id = closed_id;
      pending_drain.swap(impl->queued_sends);
    }

    if (!impl->stopping.load()) {
      for (auto& queued : pending_drain) {
        auto* p = new PendingSend{impl, std::move(queued)};
        g_main_context_invoke_full(ctx, G_PRIORITY_DEFAULT, DoSendOnWorker, p, nullptr);
      }
      impl.reset(); /* Drop extra ref inside run loop so destructor can clear if needed. */
      g_main_loop_run(loop);
    } else {
      impl.reset();
    }

    g_main_context_pop_thread_default(ctx);

    auto impl_cleanup = weak_impl.lock();
    if (impl_cleanup) {
      std::lock_guard<std::mutex> lock(impl_cleanup->mu);
      if (impl_cleanup->conn) {
        if (impl_cleanup->action_sub_id) {
          g_dbus_connection_signal_unsubscribe(impl_cleanup->conn, impl_cleanup->action_sub_id);
          impl_cleanup->action_sub_id = 0;
        }
        if (impl_cleanup->closed_sub_id) {
          g_dbus_connection_signal_unsubscribe(impl_cleanup->conn, impl_cleanup->closed_sub_id);
          impl_cleanup->closed_sub_id = 0;
        }
        g_clear_object(&impl_cleanup->conn);
      }
      g_clear_object(&impl_cleanup->cancellable);
      g_clear_pointer(&impl_cleanup->loop, g_main_loop_unref);
      g_clear_pointer(&impl_cleanup->ctx, g_main_context_unref);
    } else {
      if (conn) {
        if (action_id) g_dbus_connection_signal_unsubscribe(conn, action_id);
        if (closed_id) g_dbus_connection_signal_unsubscribe(conn, closed_id);
        g_object_unref(conn);
      }
      g_object_unref(cancel);
      g_main_loop_unref(loop);
      g_main_context_unref(ctx);
    }
  });
}

void WebNotificationBridge::Stop() {
  impl_->stopping.store(true);
  if (!impl_->started.load()) return;

  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->cancellable) g_cancellable_cancel(impl_->cancellable);
    if (impl_->ctx && impl_->loop) {
      g_main_context_invoke_full(impl_->ctx, G_PRIORITY_HIGH, QuitLoopOnWorker,
                                 g_main_loop_ref(impl_->loop),
                                 reinterpret_cast<GDestroyNotify>(g_main_loop_unref));
    }
  }

  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->started.store(false);
}

void WebNotificationBridge::SendNotification(WebNotificationRequest req) {
  if (impl_->stopping.load()) return;
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (!impl_->ctx || !impl_->conn) {
    if (impl_->started.load() && impl_->queued_sends.size() < 64) {
      impl_->queued_sends.push_back(std::move(req));
    }
    return;
  }
  auto* pending = new PendingSend{impl_, std::move(req)};
  g_main_context_invoke_full(impl_->ctx, G_PRIORITY_DEFAULT, DoSendOnWorker, pending, nullptr);
}

void WebNotificationBridge::CloseByJsId(const std::string& js_notif_id) {
  if (js_notif_id.empty() || impl_->stopping.load()) return;
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (!impl_->ctx || !impl_->conn) return;
  auto* pending = new PendingClose{impl_, js_notif_id};
  g_main_context_invoke_full(impl_->ctx, G_PRIORITY_DEFAULT, DoCloseOnWorker, pending, nullptr);
}

}  // namespace kajo_web
