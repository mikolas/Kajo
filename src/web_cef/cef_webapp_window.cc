#include "web_cef/cef_webapp_window.h"

#include <glib.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "config/kajo_config_store.h"
#include "include/cef_app.h"
#include "include/cef_parser.h"
#include "include/wrapper/cef_helpers.h"
#include "web_cef/cef_clipboard.h"
#include "web_cef/cef_host_coord.h"
#include "web_cef/cef_prefs_helper.h"
#include "web_cef/cef_site_util.h"
#include "web_cef/cef_window.h"

namespace kajo_web {

namespace {

enum WebAppAccel {
  WA_ACCEL_RELOAD = 1,
  WA_ACCEL_HARD_RELOAD,
  WA_ACCEL_CLOSE,
  WA_ACCEL_FULLSCREEN,
  WA_ACCEL_DEVTOOLS,
  WA_ACCEL_BACK,
  WA_ACCEL_FORWARD,
  WA_ACCEL_ZOOM_IN,
  WA_ACCEL_ZOOM_OUT,
  WA_ACCEL_ZOOM_RESET,
};

class WebAppInfobarClient : public CefClient,
                            public CefLifeSpanHandler,
                            public CefKeyboardHandler,
                            public CefRequestHandler,
                            public CefContextMenuHandler,
                            public CefFocusHandler {
public:
  explicit WebAppInfobarClient(KajoWebAppWindow* owner, bool is_ctxmenu = false)
      : owner_(owner), is_ctxmenu_(is_ctxmenu) {}
  void Detach() { owner_ = nullptr; }

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
  CefRefPtr<CefFocusHandler> GetFocusHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    if (!owner_) return;
    if (is_ctxmenu_) owner_->OnCtxMenuBrowserCreated(browser);
    else owner_->OnInfobarBrowserCreated(browser);
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    if (owner_ && owner_->Router()) owner_->Router()->OnBeforeClose(browser);
  }
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                 TerminationStatus /*status*/,
                                 int /*error_code*/,
                                 const CefString& /*error_string*/) override {
    if (owner_ && owner_->Router()) owner_->Router()->OnRenderProcessTerminated(browser);
  }
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> /*request*/,
                      bool /*user_gesture*/,
                      bool /*is_redirect*/) override {
    if (owner_ && owner_->Router()) owner_->Router()->OnBeforeBrowse(browser, frame);
    return false;
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    if (owner_ && owner_->Router()) {
      return owner_->Router()->OnProcessMessageReceived(browser, frame, source_process, message);
    }
    return false;
  }
  bool OnPreKeyEvent(CefRefPtr<CefBrowser> /*browser*/,
                     const CefKeyEvent& event,
                     CefEventHandle /*os_event*/,
                     bool* /*is_keyboard_shortcut*/) override {
    if (owner_) return owner_->HandleShortcut(event);
    return false;
  }
  void OnBeforeContextMenu(CefRefPtr<CefBrowser> /*browser*/,
                           CefRefPtr<CefFrame> /*frame*/,
                           CefRefPtr<CefContextMenuParams> /*params*/,
                           CefRefPtr<CefMenuModel> model) override {
    if (model) model->Clear();
  }
  bool RunContextMenu(CefRefPtr<CefBrowser> /*browser*/,
                      CefRefPtr<CefFrame> /*frame*/,
                      CefRefPtr<CefContextMenuParams> /*params*/,
                      CefRefPtr<CefMenuModel> /*model*/,
                      CefRefPtr<CefRunContextMenuCallback> callback) override {
    if (callback) callback->Cancel();
    return true;
  }
  void OnGotFocus(CefRefPtr<CefBrowser> /*browser*/) override {
    if (!is_ctxmenu_ && owner_) owner_->OnInfobarGotFocus();
  }

private:
  KajoWebAppWindow* owner_;
  bool is_ctxmenu_ = false;
  IMPLEMENT_REFCOUNTING(WebAppInfobarClient);
};

class WebAppBrowserViewDelegate : public CefBrowserViewDelegate {
public:
  explicit WebAppBrowserViewDelegate(KajoWebAppWindow* owner) : owner_(owner) {}
  void Detach() { owner_ = nullptr; }

  cef_runtime_style_t GetBrowserRuntimeStyle() override {
    return CEF_RUNTIME_STYLE_ALLOY;
  }

  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    if (owner_) {
      CefSize s = owner_->GetPreferredInfobarSize();
      if (s.height > 0 && view && view->GetID() == 910) return s;
      if (view && view->GetID() == 911) return CefSize(100, 100);
    }
    return CefSize(800, 600);
  }

private:
  KajoWebAppWindow* owner_;
  IMPLEMENT_REFCOUNTING(WebAppBrowserViewDelegate);
};

}  // namespace

KajoWebAppWindow::KajoWebAppWindow(BrowserHostCoordinator* coordinator,
                                   const std::string& window_key,
                                   const WebAppEntry& app,
                                   const std::string& initial_url)
    : coord_(coordinator),
      window_key_(window_key),
      app_(app),
      initial_url_(initial_url.empty() ? app.start_url : initial_url),
      current_title_(app.name.empty() ? app.id : app.name),
      infobar_store_(std::make_shared<ChromeStateStore>()) {
  CefMessageRouterConfig rcfg;
  router_ = CefMessageRouterBrowserSide::Create(rcfg);
  router_->AddHandler(this, false);
}

KajoWebAppWindow::~KajoWebAppWindow() {
  closing_ = true;
  DismissContextMenu(false);
  ClearPermissionCallbacks();
  if (tab_client_) {
    tab_client_->DetachCallbacks();
    tab_client_ = nullptr;
  }
  if (infobar_client_) {
    static_cast<WebAppInfobarClient*>(infobar_client_.get())->Detach();
    infobar_client_ = nullptr;
  }
  if (ctxmenu_client_) {
    static_cast<WebAppInfobarClient*>(ctxmenu_client_.get())->Detach();
    ctxmenu_client_ = nullptr;
  }
  if (bv_delegate_) {
    static_cast<WebAppBrowserViewDelegate*>(bv_delegate_.get())->Detach();
    bv_delegate_ = nullptr;
  }
  if (router_) router_->RemoveHandler(this);
}

int KajoWebAppWindow::BrowserId() const {
  return content_browser_ ? content_browser_->GetIdentifier() : 0;
}

void KajoWebAppWindow::ShowWindow() {
  CEF_REQUIRE_UI_THREAD();
  if (window_) {
    FocusAndActivate();
    return;
  }
  PushInfobarState();
  window_ = CefWindow::CreateTopLevelWindow(this);
}

void KajoWebAppWindow::FocusAndActivate() {
  CEF_REQUIRE_UI_THREAD();
  if (!window_) return;
  window_->Show();
  window_->Activate();
  if (content_view_) content_view_->RequestFocus();
}

void KajoWebAppWindow::LoadUrl(const std::string& url) {
  CEF_REQUIRE_UI_THREAD();
  if (url.empty()) return;
  if (content_browser_ && content_browser_->GetMainFrame()) {
    content_browser_->GetMainFrame()->LoadURL(url);
  } else {
    initial_url_ = url;
  }
  FocusAndActivate();
}

void KajoWebAppWindow::UpdateAppMetadata(const WebAppEntry& app) {
  CEF_REQUIRE_UI_THREAD();
  app_ = app;
  UpdateWindowBorderAndTitle();
}

void KajoWebAppWindow::CloseWindow() {
  CEF_REQUIRE_UI_THREAD();
  if (window_) window_->Close();
}

void KajoWebAppWindow::OnInfobarBrowserCreated(CefRefPtr<CefBrowser> browser) {
  infobar_browser_ = browser;
  PushInfobarState();
}

void KajoWebAppWindow::OnCtxMenuBrowserCreated(CefRefPtr<CefBrowser> browser) {
  ctxmenu_browser_ = browser;
}

CefSize KajoWebAppWindow::GetPreferredInfobarSize() const {
  return CefSize(800, 44);
}

void KajoWebAppWindow::OnWindowCreated(CefRefPtr<CefWindow> window) {
  window_ = window;
  UpdateWindowBorderAndTitle();

  bv_delegate_ = new WebAppBrowserViewDelegate(this);
  tab_client_ = new CefTabClient(1, coord_ ? coord_->Adblock() : nullptr, this);
  infobar_client_ = new WebAppInfobarClient(this, false);
  ctxmenu_client_ = new WebAppInfobarClient(this, true);

  CefBrowserSettings bs;
  bs.background_color = CefColorSetARGB(255, 0x14, 0x14, 0x14);
  CefBrowserSettings trans_bs;
  trans_bs.background_color = CefColorSetARGB(0, 0, 0, 0);
  if (KajoConfigStore* cs = kajo_config_store_get_default()) {
    if (const KajoWebCfg* w = kajo_config_store_acquire_web(cs)) {
      bs.default_font_size = CLAMP(w->default_font_size_px, 10, 32);
      bs.default_fixed_font_size = CLAMP(w->mono_font_size_px, 10, 28);
      bs.minimum_font_size = CLAMP(w->min_font_size_px, 0, 24);
      bs.webgl = w->enable_webgl ? STATE_ENABLED : STATE_DISABLED;
      default_zoom_pct_ = CLAMP(w->default_zoom_pct, 50, 200);
      kajo_web_cfg_unref(w);
    }
  }

  CefRefPtr<CefRequestContext> rc = coord_ ? coord_->SharedRequestContext() : nullptr;
  content_view_ = CefBrowserView::CreateBrowserView(
      tab_client_, initial_url_, bs, nullptr, rc, bv_delegate_);
  infobar_view_ = CefBrowserView::CreateBrowserView(
      infobar_client_, "kajo-chrome://ui/overlay.html?role=perm", bs, nullptr, rc, bv_delegate_);
  if (infobar_view_) infobar_view_->SetID(910);
  ctxmenu_view_ = CefBrowserView::CreateBrowserView(
      ctxmenu_client_, "kajo-chrome://ui/ctxmenu.html", bs, nullptr, rc, bv_delegate_);
  if (ctxmenu_view_) ctxmenu_view_->SetID(911);

  CefBoxLayoutSettings vset;
  vset.horizontal = 0;
  vset.main_axis_alignment = CEF_AXIS_ALIGNMENT_START;
  vset.cross_axis_alignment = CEF_AXIS_ALIGNMENT_STRETCH;
  vset.inside_border_insets = {1, 1, 1, 1};
  CefRefPtr<CefBoxLayout> vlayout = window_->SetToBoxLayout(vset);
  window_->AddChildView(content_view_);
  vlayout->SetFlexForView(content_view_, 1);

  infobar_ctrl_ = window_->AddOverlayView(infobar_view_, CEF_DOCKING_MODE_CUSTOM, false);
  if (infobar_ctrl_) infobar_ctrl_->SetVisible(false);
  ctxmenu_ctrl_ = window_->AddOverlayView(ctxmenu_view_, CEF_DOCKING_MODE_CUSTOM, true);
  if (ctxmenu_ctrl_) ctxmenu_ctrl_->SetVisible(false);

  window_->SetAccelerator(WA_ACCEL_RELOAD, 'R', false, true, false, false);
  window_->SetAccelerator(WA_ACCEL_RELOAD, 0x74 /*F5*/, false, false, false, false);
  window_->SetAccelerator(WA_ACCEL_HARD_RELOAD, 'R', true, true, false, false);
  window_->SetAccelerator(WA_ACCEL_CLOSE, 'W', false, true, false, false);
  window_->SetAccelerator(WA_ACCEL_FULLSCREEN, 0x7A /*F11*/, false, false, false, false);
  window_->SetAccelerator(WA_ACCEL_DEVTOOLS, 0x7B /*F12*/, false, false, false, false);
  window_->SetAccelerator(WA_ACCEL_BACK, 0x25 /*VK_LEFT*/, false, false, true, false);
  window_->SetAccelerator(WA_ACCEL_FORWARD, 0x27 /*VK_RIGHT*/, false, false, true, false);
  window_->SetAccelerator(WA_ACCEL_ZOOM_IN, 0xBB /*VK_OEM_PLUS*/, false, true, false, false);
  window_->SetAccelerator(WA_ACCEL_ZOOM_IN, 0xBB /*VK_OEM_PLUS*/, true, true, false, false);
  window_->SetAccelerator(WA_ACCEL_ZOOM_IN, 0x6B /*VK_ADD*/, false, true, false, false);
  window_->SetAccelerator(WA_ACCEL_ZOOM_OUT, 0xBD /*VK_OEM_MINUS*/, false, true, false, false);
  window_->SetAccelerator(WA_ACCEL_ZOOM_OUT, 0xBD /*VK_OEM_MINUS*/, true, true, false, false);
  window_->SetAccelerator(WA_ACCEL_ZOOM_OUT, 0x6D /*VK_SUBTRACT*/, false, true, false, false);
  window_->SetAccelerator(WA_ACCEL_ZOOM_RESET, '0', false, true, false, false);
  window_->SetAccelerator(WA_ACCEL_ZOOM_RESET, 0x60 /*VK_NUMPAD0*/, false, true, false, false);

  window_->Show();
  if (content_view_) content_view_->RequestFocus();
  UpdateInfobarOverlayBounds();
}

bool KajoWebAppWindow::CanClose(CefRefPtr<CefWindow> /*window*/) {
  return true;
}

void KajoWebAppWindow::OnWindowClosing(CefRefPtr<CefWindow> /*window*/) {
  closing_ = true;
  DismissContextMenu(false);
  ClearPermissionCallbacks();
  if (infobar_client_) {
    static_cast<WebAppInfobarClient*>(infobar_client_.get())->Detach();
  }
  infobar_client_ = nullptr;
  if (ctxmenu_client_) {
    static_cast<WebAppInfobarClient*>(ctxmenu_client_.get())->Detach();
  }
  ctxmenu_client_ = nullptr;
  if (bv_delegate_) {
    static_cast<WebAppBrowserViewDelegate*>(bv_delegate_.get())->Detach();
  }
  bv_delegate_ = nullptr;
  if (infobar_view_) {
    if (auto b = infobar_view_->GetBrowser()) router_->OnBeforeClose(b);
  }
  if (ctxmenu_view_) {
    if (auto b = ctxmenu_view_->GetBrowser()) router_->OnBeforeClose(b);
  }
  if (tab_client_) tab_client_->DetachCallbacks();
  if (coord_ && coord_->SharedRequestContext()) {
    if (CefRefPtr<CefCookieManager> cm =
            coord_->SharedRequestContext()->GetCookieManager(nullptr)) {
      cm->FlushStore(nullptr);
    }
  }
}

void KajoWebAppWindow::OnWindowDestroyed(CefRefPtr<CefWindow> /*window*/) {
  window_ = nullptr;
  content_view_ = nullptr;
  infobar_view_ = nullptr;
  ctxmenu_view_ = nullptr;
  infobar_ctrl_ = nullptr;
  ctxmenu_ctrl_ = nullptr;
  content_browser_ = nullptr;
  infobar_browser_ = nullptr;
  ctxmenu_browser_ = nullptr;
  ctxmenu_cb_ = nullptr;
  ctxmenu_open_ = false;
  ctxmenu_ready_ = false;
  tab_client_ = nullptr;
  router_ = nullptr;
  if (coord_) coord_->OnWebAppWindowDestroyed(window_key_);
}

void KajoWebAppWindow::OnWindowActivationChanged(CefRefPtr<CefWindow> /*window*/, bool active) {
  if (!active && !closing_ && ctxmenu_open_) {
    DismissContextMenu(false);
  }
}

void KajoWebAppWindow::OnWindowBoundsChanged(CefRefPtr<CefWindow> /*window*/,
                                             const CefRect& /*new_bounds*/) {
  if (ctxmenu_open_) DismissContextMenu(false);
  UpdateInfobarOverlayBounds();
}

CefRect KajoWebAppWindow::GetInitialBounds(CefRefPtr<CefWindow> /*window*/) {
  return CefRect(0, 0, 1280, 840);
}

bool KajoWebAppWindow::GetLinuxWindowProperties(CefRefPtr<CefWindow> /*window*/,
                                                CefLinuxWindowProperties& properties) {
  const std::string wayland_id =
      !app_.wayland_app_id.empty()
          ? app_.wayland_app_id
          : WebAppWaylandAppId(app_.profile_id, app_.id);
  CefString(&properties.wayland_app_id) = wayland_id;
  CefString(&properties.wm_class_class) = wayland_id;
  CefString(&properties.wm_class_name) = app_.id.empty() ? "kajo-webapp" : app_.id;
  return true;
}

bool KajoWebAppWindow::HandleShortcut(const CefKeyEvent& event) {
  if (event.type != KEYEVENT_RAWKEYDOWN) return false;
  const bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
  const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
  const bool alt = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;
  const int vk = event.windows_key_code;

  if (vk == 0x1B /*ESC*/ && ctxmenu_open_) {
    DismissContextMenu(true);
    return true;
  }

  if (ctrl && !alt) {
    if (const ZoomShortcutAction zact = ClassifyZoomShortcutKey(
            vk, static_cast<int>(event.character), static_cast<int>(event.unmodified_character));
        zact != ZoomShortcutAction::kNone) {
      ApplyZoomDelta(zact);
      return true;
    }
  }
  if (ctrl && vk == 'W') return OnAccelerator(window_, WA_ACCEL_CLOSE);
  if (ctrl && shift && vk == 'R') return OnAccelerator(window_, WA_ACCEL_HARD_RELOAD);
  if ((ctrl && vk == 'R') || vk == 0x74) return OnAccelerator(window_, WA_ACCEL_RELOAD);
  if (vk == 0x7A) return OnAccelerator(window_, WA_ACCEL_FULLSCREEN);
  if (vk == 0x7B) return OnAccelerator(window_, WA_ACCEL_DEVTOOLS);
  if (alt && vk == 0x25) return OnAccelerator(window_, WA_ACCEL_BACK);
  if (alt && vk == 0x27) return OnAccelerator(window_, WA_ACCEL_FORWARD);
  return false;
}

bool KajoWebAppWindow::OnAccelerator(CefRefPtr<CefWindow> /*window*/, int command_id) {
  switch (command_id) {
    case WA_ACCEL_RELOAD:
      if (content_browser_) content_browser_->Reload();
      return true;
    case WA_ACCEL_HARD_RELOAD:
      if (content_browser_) content_browser_->ReloadIgnoreCache();
      return true;
    case WA_ACCEL_CLOSE:
      CloseWindow();
      return true;
    case WA_ACCEL_FULLSCREEN:
      if (window_) {
        fullscreen_ = !fullscreen_;
        window_->SetFullscreen(fullscreen_);
        UpdateWindowBorderAndTitle();
        UpdateInfobarOverlayBounds();
      }
      return true;
    case WA_ACCEL_DEVTOOLS:
      if (content_browser_ && content_browser_->GetHost()) {
        CefWindowInfo wi;
        CefBrowserSettings bs;
        content_browser_->GetHost()->ShowDevTools(wi, nullptr, bs, CefPoint());
      }
      return true;
    case WA_ACCEL_BACK:
      if (content_browser_ && content_browser_->CanGoBack()) content_browser_->GoBack();
      return true;
    case WA_ACCEL_FORWARD:
      if (content_browser_ && content_browser_->CanGoForward()) content_browser_->GoForward();
      return true;
    case WA_ACCEL_ZOOM_IN:
      ApplyZoomDelta(ZoomShortcutAction::kZoomIn);
      return true;
    case WA_ACCEL_ZOOM_OUT:
      ApplyZoomDelta(ZoomShortcutAction::kZoomOut);
      return true;
    case WA_ACCEL_ZOOM_RESET:
      ApplyZoomDelta(ZoomShortcutAction::kZoomReset);
      return true;
    default:
      return false;
  }
}

void KajoWebAppWindow::ApplyZoomDelta(ZoomShortcutAction act) {
  if (!content_browser_ || !content_browser_->GetHost()) return;
  double cur = content_browser_->GetHost()->GetZoomLevel();
  if (act == ZoomShortcutAction::kZoomReset) {
    cur = ZoomPercentToCefLevel(default_zoom_pct_);
  } else if (act == ZoomShortcutAction::kZoomIn) {
    cur = std::min(5.0, cur + 0.5);
  } else if (act == ZoomShortcutAction::kZoomOut) {
    cur = std::max(-5.0, cur - 0.5);
  }
  content_browser_->GetHost()->SetZoomLevel(cur);
}

void KajoWebAppWindow::UpdateWindowBorderAndTitle() {
  if (!window_) return;
  if (fullscreen_) {
    window_->SetBackgroundColor(CefColorSetARGB(255, 0, 0, 0));
  } else {
    window_->SetBackgroundColor(
        ParseHexColorArgb(app_.theme_color, CefColorSetARGB(255, 0x00, 0x78, 0xd4)));
  }
  const std::string app_label = app_.name.empty() ? app_.id : app_.name;
  if (current_title_.empty() || current_title_ == app_label) {
    window_->SetTitle(app_label);
  } else {
    window_->SetTitle(current_title_ + " — " + app_label);
  }
}

void KajoWebAppWindow::UpdateInfobarOverlayBounds() {
  if (!window_) return;
  CefRect wb = window_->GetBounds();
  if (ctxmenu_ctrl_) {
    if (!ctxmenu_open_ || wb.width < 100 || wb.height < 100) {
      ctxmenu_ctrl_->SetVisible(false);
    } else {
      UpdateContextMenuBounds(ctxmenu_w_, ctxmenu_h_);
    }
  }
  if (!infobar_ctrl_) return;
  if (pending_permissions_.empty() || fullscreen_) {
    infobar_ctrl_->SetVisible(false);
    return;
  }
  if (wb.width < 200 || wb.height < 120) return;
  const int w = std::max(240, wb.width - 2);
  infobar_ctrl_->SetBounds(CefRect(1, 1, w, 44));
  infobar_ctrl_->SetVisible(true);
}

void KajoWebAppWindow::PushInfobarState() {
  ChromeStateSnapshot snap;
  snap.profile_id = app_.profile_id;
  snap.profile_name = app_.name;
  snap.accent_color = SanitizeHexColor(app_.theme_color, "#0078d4");
  if (!pending_permissions_.empty()) {
    const auto& p = pending_permissions_.front();
    snap.active_permission.req_id = p.req_id;
    snap.active_permission.origin = p.origin;
    snap.active_permission.label = p.label;
    snap.active_permission.perm_key = p.perm_key;
  }
  if (infobar_store_) {
    infobar_store_->Update(snap);
  }
  if (infobar_browser_ && infobar_browser_->GetMainFrame() && infobar_store_) {
    const std::string js = "window.__kajoApply && window.__kajoApply(" +
                           infobar_store_->SnapshotJson() + ");";
    infobar_browser_->GetMainFrame()->ExecuteJavaScript(js, "kajo-chrome://ui/", 0);
  }
}

void KajoWebAppWindow::DispatchNotificationEvent(const std::string& js_notif_id,
                                                   const std::string& event_type) {
  CEF_REQUIRE_UI_THREAD();
  if (closing_ || js_notif_id.empty() || !content_browser_ || !content_browser_->GetMainFrame()) {
    return;
  }
  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("kajo-notify-event");
  CefRefPtr<CefListValue> args = msg->GetArgumentList();
  args->SetString(0, js_notif_id);
  args->SetString(1, event_type);
  content_browser_->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
}

bool KajoWebAppWindow::OnQuery(CefRefPtr<CefBrowser> /*browser*/,
                               CefRefPtr<CefFrame> frame,
                               int64_t /*query_id*/,
                               const CefString& request,
                               bool /*persistent*/,
                               CefRefPtr<Callback> callback) {
  const std::string url = frame ? frame->GetURL().ToString() : "";
  if (url.rfind("kajo-chrome://", 0) != 0) {
    callback->Failure(403, "Forbidden");
    return true;
  }
  const std::string req_str = request.ToString();
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, req_str.data(), static_cast<gssize>(req_str.size()),
                                  nullptr)) {
    callback->Failure(400, "Invalid JSON");
    return true;
  }
  JsonNode* root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    callback->Failure(400, "Invalid payload");
    return true;
  }
  JsonObject* obj = json_node_get_object(root);
  const char* cmd_c = json_object_has_member(obj, "cmd")
                          ? json_object_get_string_member(obj, "cmd")
                          : nullptr;
  std::string cmd = cmd_c ? std::string(cmd_c) : "";

  if (cmd == "get_state") {
    callback->Success(infobar_store_->SnapshotJson());
    return true;
  }
  if (cmd == "ctx_ready") {
    ctxmenu_ready_ = true;
    callback->Success("{}");
    return true;
  }
  if (cmd == "ctx_resize") {
    const uint64_t seq = json_object_has_member(obj, "seq")
                             ? static_cast<uint64_t>(json_object_get_int_member(obj, "seq"))
                             : 0;
    const int w = json_object_has_member(obj, "w")
                      ? static_cast<int>(json_object_get_int_member(obj, "w"))
                      : 0;
    const int h = json_object_has_member(obj, "h")
                      ? static_cast<int>(json_object_get_int_member(obj, "h"))
                      : 0;
    if (ctxmenu_open_ && (seq == 0 || seq == ctxmenu_seq_)) {
      UpdateContextMenuBounds(w, h);
    }
    callback->Success("{}");
    return true;
  }
  if (cmd == "ctx_cancel") {
    const uint64_t seq = json_object_has_member(obj, "seq")
                             ? static_cast<uint64_t>(json_object_get_int_member(obj, "seq"))
                             : 0;
    if (seq == 0 || seq == ctxmenu_seq_) {
      DismissContextMenu(true);
    }
    callback->Success("{}");
    return true;
  }
  if (cmd == "ctx_select") {
    auto get_bool = [&](const char* k) -> bool {
      return json_object_has_member(obj, k) && json_object_get_boolean_member(obj, k);
    };
    const uint64_t seq = json_object_has_member(obj, "seq")
                             ? static_cast<uint64_t>(json_object_get_int_member(obj, "seq"))
                             : 0;
    if (seq == 0 || seq == ctxmenu_seq_) {
      const int command_id = json_object_has_member(obj, "command_id")
                                 ? static_cast<int>(json_object_get_int_member(obj, "command_id"))
                                 : 0;
      uint32_t flags = EVENTFLAG_NONE;
      if (get_bool("shift")) flags |= EVENTFLAG_SHIFT_DOWN;
      if (get_bool("ctrl")) flags |= EVENTFLAG_CONTROL_DOWN;
      if (get_bool("alt")) flags |= EVENTFLAG_ALT_DOWN;
      if (get_bool("middle")) flags |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
      CompleteContextMenu(command_id, static_cast<cef_event_flags_t>(flags));
    }
    callback->Success("{}");
    return true;
  }
  if (cmd == "perm_respond") {
    uint64_t rid = json_object_has_member(obj, "req_id")
                       ? static_cast<uint64_t>(json_object_get_int_member(obj, "req_id"))
                       : 0;
    const char* dec_c = json_object_has_member(obj, "decision")
                            ? json_object_get_string_member(obj, "decision")
                            : nullptr;
    std::string dec = dec_c ? std::string(dec_c) : "dismiss";
    const bool allow = (dec == "once" || dec == "always");
    const bool remember = (dec == "always" || dec == "deny");
    RespondToPermission(rid, allow, remember);
    callback->Success("{\"ok\":true}");
    return true;
  }
  callback->Success("{}");
  return true;
}

void KajoWebAppWindow::RespondToPermission(uint64_t req_id, bool allow, bool remember) {
  for (auto it = pending_permissions_.begin(); it != pending_permissions_.end(); ++it) {
    if (it->req_id == req_id) {
      PendingPermission req = std::move(*it);
      pending_permissions_.erase(it);
      ResolvePermission(req, allow, remember);
      UpdateInfobarOverlayBounds();
      PushInfobarState();
      return;
    }
  }
}

void KajoWebAppWindow::ResolvePermission(PendingPermission& req, bool allow, bool remember) {
  if (remember && coord_ && coord_->SharedPermissions() && !coord_->Config().private_mode) {
    coord_->SharedPermissions()->Set(req.origin, req.perm_key, allow);
    coord_->SaveSharedPermissions();
  }
  if (req.media_cb) {
    if (allow) req.media_cb->Continue(req.media_perms);
    else req.media_cb->Cancel();
    req.media_cb = nullptr;
  }
  if (req.prompt_cb) {
    req.prompt_cb->Continue(allow ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY);
    req.prompt_cb = nullptr;
  }
  if (req.notify_js_req_id >= 0) {
    if (allow && !remember && coord_) {
      coord_->GrantSessionNotification(req.notify_origin);
    }
    DeliverNotifyPermResult(req.notify_js_req_id, req.notify_origin, allow ? "granted" : "denied");
    req.notify_js_req_id = -1;
  }
}

void KajoWebAppWindow::ClearPermissionCallbacks() {
  for (auto& req : pending_permissions_) {
    ResolvePermission(req, false, false);
  }
  pending_permissions_.clear();
}

void KajoWebAppWindow::DeliverNotifyPermResult(int js_req_id,
                                               const std::string& origin,
                                               const std::string& perm) {
  if (!content_browser_ || !content_browser_->GetMainFrame()) return;
  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("kajo-notify-perm-result");
  CefRefPtr<CefListValue> args = msg->GetArgumentList();
  args->SetInt(0, js_req_id);
  args->SetString(1, origin);
  args->SetString(2, perm);
  content_browser_->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
}

void KajoWebAppWindow::OnTabBrowserCreated(uint64_t /*tab_id*/, CefRefPtr<CefBrowser> browser) {
  content_browser_ = browser;
  if (browser && browser->GetHost() && default_zoom_pct_ != 100 &&
      std::abs(browser->GetHost()->GetZoomLevel()) < 1e-6) {
    browser->GetHost()->SetZoomLevel(ZoomPercentToCefLevel(default_zoom_pct_));
  }
}

void KajoWebAppWindow::OnTabBrowserClosing(uint64_t /*tab_id*/) {
  DismissContextMenu(false);
  content_browser_ = nullptr;
  if (!closing_ && window_) window_->Close();
}

void KajoWebAppWindow::OnTabAddressChanged(uint64_t /*tab_id*/, const std::string& /*url*/) {}

void KajoWebAppWindow::OnTabTitleChanged(uint64_t /*tab_id*/, const std::string& title) {
  current_title_ = title;
  UpdateWindowBorderAndTitle();
}

void KajoWebAppWindow::OnTabFullscreenChanged(uint64_t /*tab_id*/, bool fullscreen) {
  fullscreen_ = fullscreen;
  UpdateWindowBorderAndTitle();
  UpdateInfobarOverlayBounds();
}

void KajoWebAppWindow::OnTabOpenUrl(uint64_t /*from_tab_id*/,
                                    const std::string& url,
                                    bool /*foreground*/) {
  if (url.empty()) return;
  if (IsUrlInWebAppScope(app_, url) || IsLikelyAuthFlowUrl(url)) {
    if (content_browser_ && content_browser_->GetMainFrame()) {
      content_browser_->GetMainFrame()->LoadURL(url);
    }
    return;
  }
  if (coord_) coord_->OpenBrowserUrls({url});
}

void KajoWebAppWindow::OnTabRequestClose(uint64_t /*tab_id*/) {
  CloseWindow();
}

bool KajoWebAppWindow::OnTabShortcut(uint64_t /*tab_id*/, const CefKeyEvent& event) {
  return HandleShortcut(event);
}

void KajoWebAppWindow::OnInfobarGotFocus() {
  if (ctxmenu_open_) DismissContextMenu(false);
}

void KajoWebAppWindow::UpdateContextMenuBounds(int menu_w, int menu_h) {
  if (!window_ || !ctxmenu_ctrl_ || !ctxmenu_open_) return;
  CefRect wb = window_->GetBounds();
  if (wb.width < 100 || wb.height < 100) return;
  const int max_allowed_w = std::max(160, std::min(360, wb.width - 12));
  ctxmenu_w_ = std::clamp(menu_w, 160, max_allowed_w);
  ctxmenu_h_ = std::max(32, menu_h);
  const ContextMenuBounds b = ComputeContextMenuBounds(
      ctxmenu_anchor_x_, ctxmenu_anchor_y_, ctxmenu_w_, ctxmenu_h_, wb.width, wb.height, 6);
  const int clamped_h = std::min(ctxmenu_h_, b.max_h);
  ctxmenu_ctrl_->SetBounds(CefRect(b.x, b.y, ctxmenu_w_, clamped_h));
  ctxmenu_ctrl_->SetVisible(true);
}

void KajoWebAppWindow::OnTabGotFocus(uint64_t /*tab_id*/) {
  if (ctxmenu_open_) DismissContextMenu(false);
}

void KajoWebAppWindow::DismissContextMenu(bool restore_focus) {
  CefRefPtr<CefRunContextMenuCallback> cb = std::move(ctxmenu_cb_);
  ctxmenu_cb_ = nullptr;
  const bool was_open = ctxmenu_open_;
  if (was_open) ++ctxmenu_seq_;
  ctxmenu_open_ = false;
  if (was_open && ctxmenu_browser_) {
    if (CefRefPtr<CefFrame> mf = ctxmenu_browser_->GetMainFrame()) {
      mf->ExecuteJavaScript("window.__kajoClearCtxMenu && window.__kajoClearCtxMenu();",
                            mf->GetURL(), 0);
    }
  }
  if (ctxmenu_ctrl_) ctxmenu_ctrl_->SetVisible(false);
  if (was_open && restore_focus && !closing_ && content_view_) {
    content_view_->RequestFocus();
  }
  if (cb) cb->Cancel();
}

void KajoWebAppWindow::CompleteContextMenu(int command_id, cef_event_flags_t event_flags) {
  CefRefPtr<CefRunContextMenuCallback> cb = std::move(ctxmenu_cb_);
  ctxmenu_cb_ = nullptr;
  ++ctxmenu_seq_;
  ctxmenu_open_ = false;
  if (ctxmenu_browser_) {
    if (CefRefPtr<CefFrame> mf = ctxmenu_browser_->GetMainFrame()) {
      mf->ExecuteJavaScript("window.__kajoClearCtxMenu && window.__kajoClearCtxMenu();",
                            mf->GetURL(), 0);
    }
  }
  if (ctxmenu_ctrl_) ctxmenu_ctrl_->SetVisible(false);
  if (!closing_ && content_view_) {
    content_view_->RequestFocus();
  }
  if (cb) cb->Continue(command_id, event_flags);
}

bool KajoWebAppWindow::OnTabRunContextMenu(
    uint64_t /*tab_id*/,
    int x,
    int y,
    const std::vector<ChromeContextMenuItem>& items,
    CefRefPtr<CefRunContextMenuCallback> callback) {
  if (!window_ || !content_view_ || !ctxmenu_ctrl_ || !ctxmenu_browser_ || !ctxmenu_ready_ ||
      closing_ || items.empty()) {
    return false;
  }
  if (ctxmenu_open_ || ctxmenu_cb_) {
    DismissContextMenu(false);
  }
  CefRefPtr<CefFrame> mf = ctxmenu_browser_->GetMainFrame();
  if (!mf) return false;

  CefPoint pt(x, y);
  content_view_->ConvertPointToWindow(pt);
  CefRect wb = window_->GetBounds();
  if (wb.width < 100 || wb.height < 100) return false;

  ctxmenu_cb_ = std::move(callback);
  ctxmenu_open_ = true;
  ++ctxmenu_seq_;
  ctxmenu_anchor_x_ = pt.x;
  ctxmenu_anchor_y_ = pt.y;

  std::string font = "Fira Code";
  int font_px = 14;
  int sub_px = 12;
  int badge_px = 11;
  if (KajoConfigStore* cs = kajo_config_store_get_default()) {
    if (const KajoThemeCfg* t = kajo_config_store_acquire_theme(cs)) {
      if (t->ui_font_family && t->ui_font_family[0] != '\0') font = t->ui_font_family;
      font_px = CLAMP(t->ui_font_size_px, 11, 20);
      sub_px = CLAMP(t->font_subtitle_size_px, 10, 18);
      badge_px = CLAMP(t->font_badge_size_px, 9, 16);
      kajo_theme_cfg_unref(t);
    }
  }
  const ContextMenuSize sz = EstimateContextMenuSize(items, sub_px, false);
  UpdateContextMenuBounds(sz.width, sz.height);
  if (ctxmenu_view_) ctxmenu_view_->RequestFocus();

  const std::string accent = SanitizeHexColor(app_.theme_color, "#0078d4");
  const std::string payload = SerializeContextMenuPayloadJson(
      ctxmenu_seq_, items, accent, font, font_px, sub_px, badge_px);
  const std::string js = "window.__kajoOpenCtxMenu && window.__kajoOpenCtxMenu(" + payload + ");";
  mf->ExecuteJavaScript(js, mf->GetURL(), 0);
  return true;
}

void KajoWebAppWindow::OnTabContextMenuDismissed(uint64_t /*tab_id*/) {
  if (!ctxmenu_open_) return;
  ctxmenu_cb_ = nullptr;
  ++ctxmenu_seq_;
  ctxmenu_open_ = false;
  if (ctxmenu_browser_) {
    if (CefRefPtr<CefFrame> mf = ctxmenu_browser_->GetMainFrame()) {
      mf->ExecuteJavaScript("window.__kajoClearCtxMenu && window.__kajoClearCtxMenu();",
                            mf->GetURL(), 0);
    }
  }
  if (ctxmenu_ctrl_) ctxmenu_ctrl_->SetVisible(false);
}

std::string KajoWebAppWindow::GetSuggestedDownloadPath(const std::string& suggested_name,
                                                       bool* out_ask) {
  const bool ask = force_ask_next_download_;
  force_ask_next_download_ = false;
  if (out_ask) *out_ask = ask;
  const char* dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  std::string dir = dl ? std::string(dl) : (std::string(g_get_home_dir()) + "/Downloads");
  g_mkdir_with_parents(dir.c_str(), 0755);
  std::string base = suggested_name.empty() ? "download" : suggested_name;
  for (char& c : base) {
    if (c == '/' || c == '\\') c = '_';
  }
  g_autofree gchar* full = g_build_filename(dir.c_str(), base.c_str(), nullptr);
  return full ? std::string(full) : "";
}

bool KajoWebAppWindow::OnTabContextCommand(uint64_t /*tab_id*/,
                                           int command_id,
                                           const std::string& target_url) {
  switch (command_id) {
    case KAJO_CTX_OPEN_LINK_TAB:
    case KAJO_CTX_OPEN_IMAGE_TAB:
    case KAJO_CTX_OPEN_MEDIA_TAB:
      if (!target_url.empty() && coord_) {
        coord_->OpenBrowserUrls({target_url});
        return true;
      }
      return false;
    case KAJO_CTX_COPY_LINK_URL:
    case KAJO_CTX_COPY_IMAGE_URL:
    case KAJO_CTX_COPY_MEDIA_URL:
      if (target_url.empty() || !content_browser_) return false;
      clipboard_auto_allow_until_us_ = g_get_monotonic_time() + 2000000;
      if (content_view_) content_view_->RequestFocus();
      CopyTextViaBrowser(content_browser_, target_url);
      return true;
    case KAJO_CTX_COPY_IMAGE: {
      if (target_url.empty() || !content_browser_ || !coord_) return false;
      clipboard_auto_allow_until_us_ = g_get_monotonic_time() + 4000000;
      if (content_view_) content_view_->RequestFocus();
      CopyImageViaBrowser(content_browser_, coord_->SharedRequestContext(), target_url, nullptr);
      return true;
    }
    case KAJO_CTX_SAVE_LINK_AS:
    case KAJO_CTX_SAVE_IMAGE_AS:
    case KAJO_CTX_SAVE_MEDIA_AS:
      if (target_url.empty() || !content_browser_ || !content_browser_->GetHost()) return false;
      force_ask_next_download_ = true;
      content_browser_->GetHost()->StartDownload(target_url);
      return true;
    case KAJO_CTX_PLAY_IN_PLAYER:
      if (target_url.rfind("http://", 0) == 0 || target_url.rfind("https://", 0) == 0 ||
          target_url.rfind("file://", 0) == 0) {
        const gchar* argv[] = {"kajo-player", "--", target_url.c_str(), nullptr};
        g_spawn_async(nullptr, const_cast<gchar**>(argv), nullptr,
                      G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr);
        return true;
      }
      return false;
    case KAJO_CTX_VIEW_SOURCE:
      if (!target_url.empty() && target_url.rfind("view-source:", 0) != 0 && coord_) {
        coord_->OpenBrowserUrls({"view-source:" + target_url});
        return true;
      }
      return false;
    default:
      return false;
  }
}

void KajoWebAppWindow::OnTabCrashed(uint64_t /*tab_id*/,
                                    int /*status*/,
                                    int /*error_code*/,
                                    const std::string& /*error_string*/) {
  if (content_browser_) content_browser_->Reload();
}

bool KajoWebAppWindow::OnTabMediaPermissionRequest(
    uint64_t /*tab_id*/,
    const std::string& origin,
    uint32_t perms,
    CefRefPtr<CefMediaAccessCallback> callback) {
  if (!callback || closing_) return false;
  const std::string key = PermissionKey(perms, 0);
  const int decision = (coord_ && coord_->SharedPermissions() && !coord_->Config().private_mode)
                           ? coord_->SharedPermissions()->Lookup(origin, key)
                           : 0;
  if (decision == 1) {
    callback->Continue(perms);
    return true;
  }
  if (decision == -1) {
    callback->Cancel();
    return true;
  }
  PendingPermission req;
  req.req_id = next_perm_req_id_++;
  req.origin = NormalizeOrigin(origin);
  req.label = FormatPermissionLabel(perms, 0);
  req.perm_key = key;
  req.media_perms = perms;
  req.media_cb = callback;
  pending_permissions_.push_back(std::move(req));
  UpdateInfobarOverlayBounds();
  PushInfobarState();
  return true;
}

bool KajoWebAppWindow::OnTabPermissionPrompt(
    uint64_t /*tab_id*/,
    uint64_t prompt_id,
    const std::string& origin,
    uint32_t perms,
    CefRefPtr<CefPermissionPromptCallback> callback) {
  if (!callback || closing_) return false;
  if ((perms & CEF_PERMISSION_TYPE_CLIPBOARD) != 0 &&
      g_get_monotonic_time() <= clipboard_auto_allow_until_us_) {
    callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
    return true;
  }
  const std::string key = PermissionKey(0, perms);
  const int decision = (coord_ && coord_->SharedPermissions() && !coord_->Config().private_mode)
                           ? coord_->SharedPermissions()->Lookup(origin, key)
                           : 0;
  if (decision == 1) {
    callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
    return true;
  }
  if (decision == -1) {
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return true;
  }
  PendingPermission req;
  req.req_id = next_perm_req_id_++;
  req.prompt_id = prompt_id;
  req.origin = NormalizeOrigin(origin);
  req.label = FormatPermissionLabel(0, perms);
  req.perm_key = key;
  req.prompt_cb = callback;
  pending_permissions_.push_back(std::move(req));
  UpdateInfobarOverlayBounds();
  PushInfobarState();
  return true;
}

void KajoWebAppWindow::OnTabPermissionDismiss(uint64_t /*tab_id*/, uint64_t prompt_id) {
  bool removed = false;
  for (auto it = pending_permissions_.begin(); it != pending_permissions_.end();) {
    if (it->prompt_id == prompt_id) {
      it = pending_permissions_.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  if (removed) {
    UpdateInfobarOverlayBounds();
    PushInfobarState();
  }
}

bool KajoWebAppWindow::ShouldAllowNativePopup(uint64_t /*tab_id*/,
                                              const std::string& url,
                                              cef_window_open_disposition_t disposition) {
  return disposition == CEF_WOD_NEW_POPUP || IsLikelyAuthFlowUrl(url) ||
         IsUrlInWebAppScope(app_, url);
}

bool KajoWebAppWindow::OnTabShouldInterceptNavigation(uint64_t /*tab_id*/,
                                                      const std::string& url,
                                                      bool is_redirect,
                                                      bool user_gesture) {
  if (!user_gesture || is_redirect) return false;
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return false;
  if (IsUrlInWebAppScope(app_, url) || IsLikelyAuthFlowUrl(url)) return false;
  if (content_browser_ && content_browser_->GetMainFrame()) {
    const std::string cur = content_browser_->GetMainFrame()->GetURL().ToString();
    const std::string cur_origin = DeriveOriginFromUrl(cur);
    const std::string tgt_origin = DeriveOriginFromUrl(url);
    if (!cur_origin.empty() && cur_origin == tgt_origin) return false;
  }
  if (coord_) coord_->OpenBrowserUrls({url});
  return true;
}

void KajoWebAppWindow::OnTabWebNotifyShow(uint64_t /*tab_id*/,
                                            const std::string& origin,
                                            const std::string& title,
                                            const std::string& body,
                                            const std::string& tag,
                                            const std::string& js_notif_id) {
  if (!coord_ || !coord_->Notifications() || closing_) return;
  const std::string key = PermissionKey(0, CEF_PERMISSION_TYPE_NOTIFICATIONS);
  int decision = (coord_->SharedPermissions() && !coord_->Config().private_mode)
                     ? coord_->SharedPermissions()->Lookup(origin, key)
                     : 0;
  if (decision == 0 && coord_->HasSessionNotificationGrant(origin)) decision = 1;
  if (decision != 1) return;

  WebNotificationRequest req;
  req.origin = NormalizeOrigin(origin);
  req.title = title;
  req.body = body;
  req.tag = tag;
  req.app_name = !app_.name.empty() ? app_.name : "Kajo Web App";
  req.desktop_entry =
      !app_.wayland_app_id.empty() ? app_.wayland_app_id
                                   : WebAppWaylandAppId(app_.profile_id, app_.id);
  req.icon_path = !app_.icon_path.empty() ? app_.icon_path : "org.kajo.Web";
  req.browser_id = BrowserId();
  req.app_id = window_key_;
  req.js_notif_id = js_notif_id;
  coord_->Notifications()->SendNotification(std::move(req));
}

void KajoWebAppWindow::OnTabWebNotifyClose(uint64_t /*tab_id*/,
                                             const std::string& js_notif_id) {
  if (coord_ && coord_->Notifications()) {
    coord_->Notifications()->CloseByJsId(js_notif_id);
  }
}

void KajoWebAppWindow::OnTabWebNotifyRequestPerm(uint64_t /*tab_id*/,
                                                   const std::string& origin,
                                                   int req_id) {
  if (closing_) return;
  const std::string norm = NormalizeOrigin(origin);
  const std::string key = PermissionKey(0, CEF_PERMISSION_TYPE_NOTIFICATIONS);
  const int decision = (coord_ && coord_->SharedPermissions() && !coord_->Config().private_mode)
                           ? coord_->SharedPermissions()->Lookup(norm, key)
                           : 0;
  if (decision == 1) {
    DeliverNotifyPermResult(req_id, norm, "granted");
    return;
  }
  if (decision == -1) {
    DeliverNotifyPermResult(req_id, norm, "denied");
    return;
  }

  PendingPermission req;
  req.req_id = next_perm_req_id_++;
  req.origin = norm;
  req.label = FormatPermissionLabel(0, CEF_PERMISSION_TYPE_NOTIFICATIONS);
  req.perm_key = key;
  req.notify_js_req_id = req_id;
  req.notify_origin = norm;
  pending_permissions_.push_back(std::move(req));
  UpdateInfobarOverlayBounds();
  PushInfobarState();
}

std::string KajoWebAppWindow::QueryTabWebNotifyPermission(uint64_t /*tab_id*/,
                                                          const std::string& origin) {
  if (!coord_) return "default";
  if (coord_->HasSessionNotificationGrant(origin)) return "granted";
  if (!coord_->SharedPermissions() || coord_->Config().private_mode) return "default";
  const int d = coord_->SharedPermissions()->Lookup(
      origin, PermissionKey(0, CEF_PERMISSION_TYPE_NOTIFICATIONS));
  if (d == 1) return "granted";
  if (d == -1) return "denied";
  return "default";
}

}  // namespace kajo_web
