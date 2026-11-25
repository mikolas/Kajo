#include "web_cef/cef_app.h"

#include <gio/gio.h>
#include <glib.h>

#include <cstring>
#include <string>

#include "config/kajo_config_store.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_task.h"
#include "include/cef_version.h"
#include "include/wrapper/cef_helpers.h"
#include "web_cef/cef_prefs_helper.h"
#include "web_cef/cef_webapps.h"

namespace kajo_web {

namespace {

const char* kWidevineSearchPaths[] = {
    "/usr/lib/cef/WidevineCdm",
    "/usr/lib/chromium/WidevineCdm",
    "/opt/google/chrome/WidevineCdm",
    "/opt/thorium-browser/WidevineCdm",
};

const char* kYtAdSkipScript = R"JS(
(() => {
  if (window.__kajoYtAdSkip && !window.__kajoYtAdSkip.stopped) return;
  let stopped = false;
  let skipTimer = 0, obs = null, bootObs = null, bootStop = 0;
  let curPlayer = null, savedMute = null, inAd = false;
  const getPlayer = () =>
    (curPlayer && curPlayer.isConnected)
      ? curPlayer
      : (document.getElementById('movie_player') || document.querySelector('.html5-video-player'));
  const restorePlayer = () => {
    const p = getPlayer();
    const v = p ? p.querySelector('video') : null;
    if (v) {
      if (v.playbackRate !== 1) v.playbackRate = 1;
      if (savedMute !== null) { v.muted = savedMute; savedMute = null; }
    } else {
      savedMute = null;
    }
    inAd = false;
    if (skipTimer) { clearInterval(skipTimer); skipTimer = 0; }
  };
  const trySkip = () => {
    if (stopped) { restorePlayer(); return; }
    const p = getPlayer();
    if (!p) { restorePlayer(); return; }
    const ad = p.classList.contains('ad-showing') || p.classList.contains('ad-interrupting');
    if (!ad) { if (inAd || skipTimer) restorePlayer(); return; }
    const v = p.querySelector('video');
    if (!inAd) { inAd = true; if (v && savedMute === null) savedMute = !!v.muted; }
    if (v) {
      v.muted = true;
      if (v.readyState >= 2 && Number.isFinite(v.duration) && v.duration > 0 && v.duration <= 180) {
        try { v.playbackRate = 16; } catch (_) {}
      }
    }
    const b = p.querySelector('.ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-skip-ad-button, .ytp-ad-overlay-close-button');
    if (b) b.click();
  };
  const stopBoot = () => {
    if (bootObs) { bootObs.disconnect(); bootObs = null; }
    if (bootStop) { clearTimeout(bootStop); bootStop = 0; }
  };
  const onClass = () => {
    if (stopped || !curPlayer) return;
    const ad = curPlayer.classList.contains('ad-showing') || curPlayer.classList.contains('ad-interrupting');
    if (ad) {
      trySkip();
      if (!skipTimer) skipTimer = setInterval(trySkip, 250);
    } else if (inAd || skipTimer) {
      restorePlayer();
    }
  };
  const attach = () => {
    if (stopped) return false;
    const p = document.getElementById('movie_player') || document.querySelector('.html5-video-player');
    if (!p) return false;
    if (p !== curPlayer) {
      if (obs) obs.disconnect();
      if (inAd || skipTimer) restorePlayer();
      curPlayer = p;
      obs = new MutationObserver(onClass);
      obs.observe(p, { attributes: true, attributeFilter: ['class'], subtree: false });
      onClass();
    }
    stopBoot();
    return true;
  };
  const boot = () => {
    if (stopped) return;
    if (attach()) return;
    if (location.pathname.indexOf('/watch') === 0 || location.pathname.indexOf('/shorts') === 0) {
      if (document.body && !bootObs) {
        bootObs = new MutationObserver(() => attach());
        bootObs.observe(document.body, { childList: true, subtree: true });
        bootStop = setTimeout(stopBoot, 3000);
      }
    }
  };
  const onNavFinish = () => setTimeout(boot, 0);
  const onDataUpdated = () => attach();
  const stop = () => {
    stopped = true;
    stopBoot();
    if (obs) { obs.disconnect(); obs = null; }
    restorePlayer();
    curPlayer = null;
    document.removeEventListener('yt-navigate-finish', onNavFinish);
    document.removeEventListener('yt-page-data-updated', onDataUpdated);
    document.removeEventListener('DOMContentLoaded', boot);
  };
  window.__kajoYtAdSkip = { stopped: false, stop };
  window.__kajoYtSkipInstalled = true;
  document.addEventListener('yt-navigate-finish', onNavFinish);
  document.addEventListener('yt-page-data-updated', onDataUpdated);
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();
)JS";

void AppendCommaFeature(CefRefPtr<CefCommandLine> command_line,
                        const char* switch_name,
                        const std::string& add_csv) {
  std::string cur = command_line->GetSwitchValue(switch_name).ToString();
  size_t pos = 0;
  while (pos < add_csv.size()) {
    size_t comma = add_csv.find(',', pos);
    std::string tok = (comma == std::string::npos)
                          ? add_csv.substr(pos)
                          : add_csv.substr(pos, comma - pos);
    pos = (comma == std::string::npos) ? add_csv.size() : comma + 1;
    if (tok.empty()) continue;
    bool present = false;
    size_t cpos = 0;
    while (cpos < cur.size()) {
      size_t c = cur.find(',', cpos);
      std::string existing = (c == std::string::npos)
                                 ? cur.substr(cpos)
                                 : cur.substr(cpos, c - cpos);
      cpos = (c == std::string::npos) ? cur.size() : c + 1;
      if (existing == tok) {
        present = true;
        break;
      }
    }
    if (!present) {
      if (!cur.empty()) cur.push_back(',');
      cur += tok;
    }
  }
  if (!cur.empty()) {
    command_line->AppendSwitchWithValue(switch_name, cur);
  }
}

}  // namespace

std::string DetectSystemWidevinePath() {
  for (const char* dir : kWidevineSearchPaths) {
    g_autofree gchar* manifest = g_build_filename(dir, "manifest.json", nullptr);
    g_autofree gchar* lib_new =
        g_build_filename(dir, "_platform_specific", "linux_x64", "libwidevinecdm.so", nullptr);
    g_autofree gchar* lib_old = g_build_filename(dir, "libwidevinecdm.so", nullptr);
    if (g_file_test(manifest, G_FILE_TEST_EXISTS) &&
        (g_file_test(lib_new, G_FILE_TEST_EXISTS) || g_file_test(lib_old, G_FILE_TEST_EXISTS))) {
      return dir;
    }
  }
  return "";
}

void EnsureWidevineComponentHint(const std::string& root_cache_dir,
                                 const std::string& widevine_dir) {
  if (root_cache_dir.empty() || widevine_dir.empty()) return;
  g_autofree gchar* hint_dir =
      g_build_filename(root_cache_dir.c_str(), "WidevineCdm", nullptr);
  g_autofree gchar* hint_path =
      g_build_filename(hint_dir, "latest-component-updated-widevine-cdm", nullptr);
  if (g_file_test(hint_path, G_FILE_TEST_EXISTS)) return;
  g_mkdir_with_parents(hint_dir, 0700);
  std::string body = "{\"Path\":\"" + JsonEscape(widevine_dir) + "\"}";
  g_file_set_contents(hint_path, body.c_str(), static_cast<gssize>(body.size()), nullptr);
}

KajoCefApp::KajoCefApp(const LaunchConfig& cfg) : cfg_(cfg) {}

void KajoCefApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  RegisterKajoCustomSchemes(registrar);
}

void KajoCefApp::OnBeforeCommandLineProcessing(const CefString& process_type,
                                               CefRefPtr<CefCommandLine> command_line) {
  if (!command_line) return;

  if (!command_line->HasSwitch("ozone-platform-hint")) {
    command_line->AppendSwitchWithValue("ozone-platform-hint", "auto");
  }

  /* Respect kajo_config hardware acceleration & startup flags in browser process */
  bool hw_accel = cfg_.hw_accel_enabled;
  if (process_type.empty()) {
    KajoConfigStore* cs = kajo_config_store_get_default();
    if (cs) {
      const KajoWebCfg* wcfg = kajo_config_store_acquire_web(cs);
      if (wcfg) {
        if (wcfg->hardware_acceleration == KAJO_WEB_HW_NEVER) {
          hw_accel = false;
          cfg_.hw_accel_enabled = false;
        }
        if (wcfg->user_agent_preset == KAJO_WEB_UA_SAFARI) {
          command_line->AppendSwitchWithValue(
              "user-agent",
              "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
              "(KHTML, like Gecko) Version/18.2 Safari/605.1.15");
        } else if (wcfg->user_agent_preset == KAJO_WEB_UA_CHROME_LINUX) {
          const std::string ua =
              "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/" + std::to_string(CHROME_VERSION_MAJOR) +
              ".0.0.0 Safari/537.36";
          command_line->AppendSwitchWithValue("user-agent", ua);
        } else if (wcfg->user_agent_preset == KAJO_WEB_UA_CUSTOM &&
                   wcfg->user_agent_custom && wcfg->user_agent_custom[0] != '\0') {
          command_line->AppendSwitchWithValue("user-agent", wcfg->user_agent_custom);
        }
        if (!command_line->HasSwitch("autoplay-policy")) {
          const char* ap = "user-gesture-required";
          if (wcfg->autoplay_policy == KAJO_WEB_AUTOPLAY_ALLOW) {
            ap = "no-user-gesture-required";
          } else if (wcfg->autoplay_policy == KAJO_WEB_AUTOPLAY_DENY) {
            ap = "document-user-activation-required";
          }
          command_line->AppendSwitchWithValue("autoplay-policy", ap);
        }
        if (!wcfg->smooth_scrolling && !command_line->HasSwitch("disable-smooth-scrolling")) {
          command_line->AppendSwitch("disable-smooth-scrolling");
        }
        kajo_web_cfg_unref(wcfg);
      }
    }
  }

  if (hw_accel) {
    if (!command_line->HasSwitch("use-gl")) {
      command_line->AppendSwitchWithValue("use-gl", "angle");
    }
    if (!command_line->HasSwitch("use-angle")) {
      command_line->AppendSwitchWithValue("use-angle", "gl-egl");
    }
    if (!command_line->HasSwitch("use-vulkan")) {
      command_line->AppendSwitchWithValue("use-vulkan", "disabled");
    }
    if (!command_line->HasSwitch("ignore-gpu-blocklist")) {
      command_line->AppendSwitch("ignore-gpu-blocklist");
    }
    if (!command_line->HasSwitch("enable-zero-copy")) {
      command_line->AppendSwitch("enable-zero-copy");
    }
    if (!command_line->HasSwitch("disable-gpu-driver-bug-workaround")) {
      command_line->AppendSwitch("disable-gpu-driver-bug-workaround");
    }
    AppendCommaFeature(
        command_line,
        "enable-features",
        "AcceleratedVideoDecoder,AcceleratedVideoEncoder,"
        "AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL,"
        "VaapiIgnoreDriverChecks,WaylandUnscaledTouchpadScrolling");
    AppendCommaFeature(command_line, "disable-features", "Vulkan");
  } else {
    command_line->AppendSwitch("disable-gpu");
    command_line->AppendSwitch("disable-gpu-compositing");
  }
}

namespace {

class AppUiTask : public CefTask {
public:
  explicit AppUiTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override {
    if (fn_) fn_();
  }

private:
  std::function<void()> fn_;
  IMPLEMENT_REFCOUNTING(AppUiTask);
};

const char* kWebNotifyShimScript = R"JS(
(() => {
  if (window.__kajoNotifyShimInstalled) return;
  window.__kajoNotifyShimInstalled = true;
  if (typeof window.__kajoNotifyPermState !== 'string') {
    window.__kajoNotifyPermState = 'default';
  }
  const activeNotifs = new Map();
  let seq = 1;

  class KajoNotification extends EventTarget {
    constructor(title, options = {}) {
      super();
      if (arguments.length === 0) {
        throw new TypeError("Failed to construct 'Notification': 1 argument required.");
      }
      this.title = String(title);
      this.body = options && options.body != null ? String(options.body) : '';
      this.tag = options && options.tag != null ? String(options.tag) : '';
      this.icon = options && options.icon != null ? String(options.icon) : '';
      this.data = options && 'data' in options ? options.data : null;
      this.silent = !!(options && options.silent);
      this.dir = (options && options.dir) || 'auto';
      this.lang = (options && options.lang) || '';
      this.onclick = null;
      this.onclose = null;
      this.onerror = null;
      this.onshow = null;
      this.__kajoId = 'n_' + (++seq) + '_' + Date.now();

      if (window.__kajoNotifyPermState !== 'granted') {
        queueMicrotask(() => {
          const ev = new Event('error');
          if (typeof this.onerror === 'function') { try { this.onerror(ev); } catch (_) {} }
          this.dispatchEvent(ev);
        });
        return;
      }

      activeNotifs.set(this.__kajoId, this);
      if (typeof window.__kajoNotifyShow === 'function') {
        window.__kajoNotifyShow(this.title, this.body, this.tag, this.__kajoId);
      }
      queueMicrotask(() => {
        const ev = new Event('show');
        if (typeof this.onshow === 'function') { try { this.onshow(ev); } catch (_) {} }
        this.dispatchEvent(ev);
      });
    }

    close() {
      if (!activeNotifs.has(this.__kajoId)) return;
      activeNotifs.delete(this.__kajoId);
      if (typeof window.__kajoNotifyClose === 'function') {
        window.__kajoNotifyClose(this.__kajoId);
      }
      const ev = new Event('close');
      if (typeof this.onclose === 'function') { try { this.onclose(ev); } catch (_) {} }
      this.dispatchEvent(ev);
    }

    static get permission() {
      return window.__kajoNotifyPermState || 'default';
    }

    static get maxActions() {
      return 2;
    }

    static requestPermission(deprecatedCallback) {
      if (typeof window.__kajoNotifyReqPerm !== 'function') {
        const res = window.__kajoNotifyPermState || 'default';
        if (typeof deprecatedCallback === 'function') {
          try { deprecatedCallback(res); } catch (_) {}
        }
        return Promise.resolve(res);
      }
      return window.__kajoNotifyReqPerm(deprecatedCallback);
    }
  }

  window.__kajoOnNotifyEvent = (jsId, type) => {
    const inst = activeNotifs.get(jsId);
    if (!inst) return;
    const ev = new Event(type);
    if (type === 'click') {
      if (typeof inst.onclick === 'function') { try { inst.onclick(ev); } catch (_) {} }
      inst.dispatchEvent(ev);
    } else if (type === 'close') {
      activeNotifs.delete(jsId);
      if (typeof inst.onclose === 'function') { try { inst.onclose(ev); } catch (_) {} }
      inst.dispatchEvent(ev);
    }
  };

  try {
    Object.defineProperty(window, 'Notification', {
      value: KajoNotification,
      writable: true,
      configurable: true
    });
  } catch (_) {
    window.Notification = KajoNotification;
  }

  if (window.ServiceWorkerRegistration && window.ServiceWorkerRegistration.prototype) {
    const swProto = window.ServiceWorkerRegistration.prototype;
    swProto.showNotification = function(title, options = {}) {
      if (window.__kajoNotifyPermState !== 'granted') {
        return Promise.reject(new DOMException('Notification permission not granted', 'NotAllowedError'));
      }
      const jsId = 'sw_' + (++seq) + '_' + Date.now();
      const body = options && options.body != null ? String(options.body) : '';
      const tag = options && options.tag != null ? String(options.tag) : '';
      if (typeof window.__kajoNotifyShow === 'function') {
        window.__kajoNotifyShow(String(title || ''), body, tag, jsId);
      }
      return Promise.resolve();
    };
  }

  if (typeof window.__kajoNotifySyncReq === 'function') {
    window.__kajoNotifySyncReq();
  }
})();
)JS";

const char* kWebAppProbeScript = R"JS(
(() => {
  if (typeof window.__kajoSendWebAppProbe !== 'function') return;
  const pageUrl = location.href || '';
  const manifestEl = document.querySelector('link[rel~="manifest" i]');
  const manifestUrl = (manifestEl && manifestEl.href) ? manifestEl.href : '';
  let iconUrl = '';
  const icons = Array.from(document.querySelectorAll('link[rel~="icon" i], link[rel="apple-touch-icon"]'));
  for (const el of icons) {
    if (el && el.href) {
      iconUrl = el.href;
      if (el.rel && el.rel.toLowerCase().includes('apple-touch-icon')) break;
    }
  }
  const themeEl = document.querySelector('meta[name="theme-color" i]');
  const themeColor = (themeEl && themeEl.content) ? themeEl.content.trim() : '';
  const title = (document.title || '').trim();
  window.__kajoSendWebAppProbe(pageUrl, manifestUrl, iconUrl, title, themeColor);
})();
)JS";

class KajoNotifyV8Handler : public CefV8Handler {
public:
  KajoNotifyV8Handler(KajoCefApp* app, CefRefPtr<CefBrowser> browser)
      : app_(app), browser_(std::move(browser)) {}

  bool Execute(const CefString& name,
               CefRefPtr<CefV8Value> /*object*/,
               const CefV8ValueList& arguments,
               CefRefPtr<CefV8Value>& retval,
               CefString& /*exception*/) override {
    if (!app_ || !browser_) return false;
    CefRefPtr<CefV8Context> ctx = CefV8Context::GetCurrentContext();
    CefRefPtr<CefFrame> frame = ctx ? ctx->GetFrame() : browser_->GetMainFrame();
    if (!frame) return false;

    const std::string fn = name.ToString();
    const std::string url = frame->GetURL().ToString();
    const std::string origin = DeriveOriginFromUrl(url);

    if (fn == "__kajoNotifySyncReq") {
      CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("kajo-notify-sync-req");
      frame->SendProcessMessage(PID_BROWSER, msg);
      retval = CefV8Value::CreateUndefined();
      return true;
    }

    if (fn == "__kajoNotifyShow") {
      const std::string title =
          (arguments.size() > 0 && arguments[0]->IsString()) ? arguments[0]->GetStringValue().ToString() : "";
      const std::string body =
          (arguments.size() > 1 && arguments[1]->IsString()) ? arguments[1]->GetStringValue().ToString() : "";
      const std::string tag =
          (arguments.size() > 2 && arguments[2]->IsString()) ? arguments[2]->GetStringValue().ToString() : "";
      const std::string js_id =
          (arguments.size() > 3 && arguments[3]->IsString()) ? arguments[3]->GetStringValue().ToString() : "";
      CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("kajo-notify-show");
      CefRefPtr<CefListValue> args = msg->GetArgumentList();
      args->SetString(0, origin);
      args->SetString(1, title);
      args->SetString(2, body);
      args->SetString(3, tag);
      args->SetString(4, js_id);
      frame->SendProcessMessage(PID_BROWSER, msg);
      retval = CefV8Value::CreateUndefined();
      return true;
    }

    if (fn == "__kajoNotifyClose") {
      const std::string js_id =
          (arguments.size() > 0 && arguments[0]->IsString()) ? arguments[0]->GetStringValue().ToString() : "";
      CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("kajo-notify-close");
      CefRefPtr<CefListValue> args = msg->GetArgumentList();
      args->SetString(0, js_id);
      frame->SendProcessMessage(PID_BROWSER, msg);
      retval = CefV8Value::CreateUndefined();
      return true;
    }

    if (fn == "__kajoNotifyReqPerm") {
      CefRefPtr<CefV8Value> legacy_cb;
      if (arguments.size() > 0 && arguments[0]->IsFunction()) {
        legacy_cb = arguments[0];
      }
      CefRefPtr<CefV8Value> promise = CefV8Value::CreatePromise();
      const int req_id = app_->RegisterPendingNotifyPerm(ctx, promise, legacy_cb);
      CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("kajo-notify-req-perm");
      CefRefPtr<CefListValue> args = msg->GetArgumentList();
      args->SetString(0, origin);
      args->SetInt(1, req_id);
      frame->SendProcessMessage(PID_BROWSER, msg);
      retval = promise;
      return true;
    }

    if (fn == "__kajoSendWebAppProbe") {
      CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("kajo-webapp-probe-result");
      CefRefPtr<CefListValue> args = msg->GetArgumentList();
      for (size_t i = 0; i < 5; ++i) {
        std::string s = (i < arguments.size() && arguments[i]->IsString())
                            ? arguments[i]->GetStringValue().ToString()
                            : "";
        args->SetString(i, s);
      }
      frame->SendProcessMessage(PID_BROWSER, msg);
      retval = CefV8Value::CreateUndefined();
      return true;
    }
    return false;
  }

private:
  KajoCefApp* app_;
  CefRefPtr<CefBrowser> browser_;
  IMPLEMENT_REFCOUNTING(KajoNotifyV8Handler);
};

}  // namespace

void KajoCefApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();

  adblock_ = std::make_shared<CefAdblockEngine>();
  adblock_->SetDirectories(cfg_.cache_dir, cfg_.data_dir);
  adblock_->ReloadFromDisk();

  store_ = std::make_shared<ChromeStateStore>();

  g_autofree gchar* perm_p = g_build_filename(cfg_.data_dir.c_str(), "permissions.json", nullptr);
  if (perm_p) {
    permission_store_path_ = perm_p;
    if (!cfg_.private_mode) permission_store_.Load(permission_store_path_);
  }
  g_autofree gchar* apps_p = g_build_filename(cfg_.data_dir.c_str(), "webapps.json", nullptr);
  if (apps_p) {
    webapps_store_path_ = apps_p;
    webapp_store_.Load(webapps_store_path_);
  }

  request_context_ = CefRequestContext::GetGlobalContext();
  RegisterKajoSchemeHandlers(request_context_, store_);
  if (request_context_) {
    bool is_light_surface = false;
    int color_scheme = KAJO_WEB_SCHEME_FOLLOW_KAJO;
    std::string accent_hex = "#0078d4";
    if (KajoConfigStore* cs = kajo_config_store_get_default()) {
      if (const KajoThemeCfg* t = kajo_config_store_acquire_theme(cs)) {
        if (t->accent_color && t->accent_color[0] != '\0') accent_hex = t->accent_color;
        is_light_surface = IsLightSurfaceHex(t->surface_base ? t->surface_base : "#141414");
        kajo_theme_cfg_unref(t);
      }
      if (const KajoWebCfg* w = kajo_config_store_acquire_web(cs)) {
        color_scheme = w->color_scheme;
        kajo_web_cfg_unref(w);
      }
    }
    const cef_color_variant_t variant =
        ResolveWebColorSchemeVariant(color_scheme, is_light_surface);
    const cef_color_t tint = ParseHexColorArgb(
        cfg_.private_mode ? "#b146c2" : accent_hex,
        CefColorSetARGB(255, 0x00, 0x78, 0xd4));
    request_context_->SetChromeColorScheme(variant, tint);
  }

  CefRefPtr<KajoCefApp> self(this);
  notifications_ = std::make_unique<WebNotificationBridge>(
      [self](const WebNotificationTarget& target) {
        CefPostTask(TID_UI, new AppUiTask([self, target]() {
          self->HandleNotificationClickOnUi(target);
        }));
      },
      [self](const WebNotificationTarget& target, uint32_t /*reason*/) {
        CefPostTask(TID_UI, new AppUiTask([self, target]() {
          self->HandleNotificationCloseOnUi(target);
        }));
      });
  notifications_->Start();

  if (!cfg_.private_mode) {
    ipc_server_ = std::make_unique<ProfileIpcServer>();
    const std::string subdir =
        (cfg_.profile_id.empty() || cfg_.profile_id == "default") ? "default" : cfg_.profile_id;
    ipc_server_->StartEx(ProfileIpcSocketPath(subdir), [self](IpcOpenRequest req) {
      CefPostTask(TID_UI, new AppUiTask([self, req = std::move(req)]() {
        self->HandleIpcOpen(req);
      }));
    });
  }

  if (!cfg_.app_id.empty()) {
    OpenOrFocusWebApp(cfg_.app_id, cfg_.app_url);
    return;
  }
  if (!cfg_.app_url.empty()) {
    LaunchAdHocWebApp(cfg_.app_url, "");
    return;
  }

  main_window_ = new KajoBrowserWindow(this, cfg_, adblock_, store_, request_context_);
  main_window_->ShowWindow();
}

void KajoCefApp::SaveSharedPermissions() {
  if (!cfg_.private_mode && !permission_store_path_.empty()) {
    permission_store_.Save(permission_store_path_);
  }
}

void KajoCefApp::GrantSessionNotification(const std::string& origin) {
  const std::string norm = NormalizeOrigin(origin);
  if (!norm.empty()) session_notify_granted_.insert(norm);
}

bool KajoCefApp::HasSessionNotificationGrant(const std::string& origin) const {
  const std::string norm = NormalizeOrigin(origin);
  return !norm.empty() && session_notify_granted_.count(norm) > 0;
}

void KajoCefApp::HandleIpcOpen(const IpcOpenRequest& req) {
  CEF_REQUIRE_UI_THREAD();
  if (shutting_down_) return;
  if (!req.app_id.empty()) {
    OpenOrFocusWebApp(req.app_id, req.app_url);
    return;
  }
  if (!req.app_url.empty()) {
    LaunchAdHocWebApp(req.app_url, "");
    return;
  }
  OpenBrowserUrls(req.urls);
}

void KajoCefApp::OpenBrowserUrls(const std::vector<std::string>& urls) {
  CEF_REQUIRE_UI_THREAD();
  if (shutting_down_) return;
  if (!main_window_) {
    LaunchConfig bcfg = cfg_;
    bcfg.app_id.clear();
    bcfg.app_url.clear();
    bcfg.initial_urls = urls;
    main_window_ = new KajoBrowserWindow(this, bcfg, adblock_, store_, request_context_);
    main_window_->ShowWindow();
    return;
  }
  main_window_->HandleExternalOpen(urls);
}

void KajoCefApp::OpenOrFocusWebApp(const std::string& app_id, const std::string& override_url) {
  CEF_REQUIRE_UI_THREAD();
  if (shutting_down_) return;
  auto found = webapp_store_.FindById(app_id);
  if (!found.has_value()) {
    if (!override_url.empty()) {
      LaunchAdHocWebApp(override_url, app_id);
    } else if (!main_window_) {
      OpenBrowserUrls({});
    }
    return;
  }

  WebAppEntry entry = *found;
  webapp_store_.TouchLaunched(entry.id);
  if (!cfg_.private_mode && !webapps_store_path_.empty()) {
    webapp_store_.Save(webapps_store_path_);
  }
  NotifyWebAppsChanged();

  auto it = app_windows_.find(entry.id);
  if (it != app_windows_.end() && it->second) {
    if (!override_url.empty()) it->second->LoadUrl(override_url);
    else it->second->FocusAndActivate();
    return;
  }

  CefRefPtr<KajoWebAppWindow> win =
      new KajoWebAppWindow(this, entry.id, entry, override_url);
  app_windows_[entry.id] = win;
  win->ShowWindow();
}

void KajoCefApp::LaunchAdHocWebApp(const std::string& url, const std::string& title) {
  CEF_REQUIRE_UI_THREAD();
  if (shutting_down_ || url.empty()) return;
  if (auto matched = webapp_store_.FindBestMatchForUrl(url)) {
    OpenOrFocusWebApp(matched->id, url);
    return;
  }
  std::string target = url;
  if (target.rfind("http://", 0) != 0 && target.rfind("https://", 0) != 0) {
    target = "https://" + target;
  }
  g_autofree gchar* host = web_model_extract_host(target.c_str());
  const std::string base_name = !title.empty() ? title : (host ? std::string(host) : "Web App");
  const std::string slug = SanitizeWebAppId(base_name);
  const std::string key = "adhoc-" + slug + "-" + std::to_string(next_adhoc_id_++);

  WebAppEntry e;
  e.id = slug;
  e.name = base_name;
  e.start_url = target;
  e.scope = DeriveOriginFromUrl(target);
  e.theme_color = "#0078d4";
  e.profile_id = cfg_.profile_id.empty() ? "default" : cfg_.profile_id;
  e.wayland_app_id = WebAppWaylandAppId(e.profile_id, e.id);

  CefRefPtr<KajoWebAppWindow> win = new KajoWebAppWindow(this, key, e, target);
  app_windows_[key] = win;
  win->ShowWindow();
}

bool KajoCefApp::UninstallWebApp(const std::string& app_id) {
  CEF_REQUIRE_UI_THREAD();
  auto found = webapp_store_.FindById(app_id);
  if (!found.has_value()) return false;

  auto wit = app_windows_.find(found->id);
  if (wit != app_windows_.end() && wit->second) {
    wit->second->CloseWindow();
  }
  webapp_store_.Remove(found->id);
  if (!cfg_.private_mode && !webapps_store_path_.empty()) {
    webapp_store_.Save(webapps_store_path_);
  }
  RemoveWebAppDesktopFiles(*found);
  NotifyWebAppsChanged();
  return true;
}

void KajoCefApp::NotifyWebAppsChanged() {
  CEF_REQUIRE_UI_THREAD();
  if (main_window_) main_window_->RefreshWebAppsState();
}

void KajoCefApp::OnMainWindowDestroyed() {
  CEF_REQUIRE_UI_THREAD();
  main_window_ = nullptr;
  MaybeQuitIfNoWindows();
}

void KajoCefApp::OnWebAppWindowDestroyed(const std::string& window_key) {
  CEF_REQUIRE_UI_THREAD();
  app_windows_.erase(window_key);
  MaybeQuitIfNoWindows();
}

void KajoCefApp::MaybeQuitIfNoWindows() {
  if (!shutting_down_ && !main_window_ && app_windows_.empty()) {
    CefQuitMessageLoop();
  }
}

bool KajoCefApp::FocusNotificationTarget(const WebNotificationTarget& target) {
  CEF_REQUIRE_UI_THREAD();
  if (shutting_down_) return false;
  if (!target.app_id.empty()) {
    auto it = app_windows_.find(target.app_id);
    if (it != app_windows_.end() && it->second) {
      it->second->FocusAndActivate();
      return true;
    }
    if (webapp_store_.FindById(target.app_id).has_value()) {
      OpenOrFocusWebApp(target.app_id);
      return true;
    }
  }
  if (main_window_) {
    main_window_->FocusTabById(target.tab_id);
    return true;
  }
  return false;
}

void KajoCefApp::HandleNotificationClickOnUi(const WebNotificationTarget& target) {
  CEF_REQUIRE_UI_THREAD();
  if (shutting_down_) return;
  FocusNotificationTarget(target);
  if (!target.app_id.empty()) {
    auto it = app_windows_.find(target.app_id);
    if (it != app_windows_.end() && it->second) {
      it->second->DispatchNotificationEvent(target.js_notif_id, "click");
    }
  } else if (main_window_) {
    main_window_->DispatchNotificationEvent(
        target.browser_id, target.tab_id, target.js_notif_id, "click");
  }
}

void KajoCefApp::HandleNotificationCloseOnUi(const WebNotificationTarget& target) {
  CEF_REQUIRE_UI_THREAD();
  if (shutting_down_) return;
  if (!target.app_id.empty()) {
    auto it = app_windows_.find(target.app_id);
    if (it != app_windows_.end() && it->second) {
      it->second->DispatchNotificationEvent(target.js_notif_id, "close");
    }
  } else if (main_window_) {
    main_window_->DispatchNotificationEvent(
        target.browser_id, target.tab_id, target.js_notif_id, "close");
  }
}

void KajoCefApp::ShutdownRelease() {
  shutting_down_ = true;
  if (ipc_server_) {
    ipc_server_->Stop();
    ipc_server_.reset();
  }
  if (notifications_) {
    notifications_->Stop();
    notifications_.reset();
  }
  app_windows_.clear();
  main_window_ = nullptr;
  if (request_context_) {
    request_context_->ClearSchemeHandlerFactories();
    request_context_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(perm_mu_);
    pending_renderer_perms_.clear();
  }
}

int KajoCefApp::RegisterPendingNotifyPerm(CefRefPtr<CefV8Context> ctx,
                                            CefRefPtr<CefV8Value> promise,
                                            CefRefPtr<CefV8Value> legacy_cb) {
  std::lock_guard<std::mutex> lock(perm_mu_);
  int id = next_renderer_perm_req_id_++;
  pending_renderer_perms_[id] = {std::move(ctx), std::move(promise), std::move(legacy_cb)};
  return id;
}

void KajoCefApp::OnWebKitInitialized() {
  CefMessageRouterConfig rc;
  renderer_router_ = CefMessageRouterRendererSide::Create(rc);
}

void KajoCefApp::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefRefPtr<CefV8Context> context) {
  if (!frame) return;
  const std::string url = frame->GetURL().ToString();
  if (renderer_router_ && url.rfind("kajo-chrome://", 0) == 0) {
    renderer_router_->OnContextCreated(browser, frame, context);
    return;
  }
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return;

  CefRefPtr<CefV8Value> global = context ? context->GetGlobal() : nullptr;
  if (!global || !global->IsValid()) return;

  CefRefPtr<CefV8Handler> handler = new KajoNotifyV8Handler(this, browser);
  auto bind_fn = [&](const char* fn_name) {
    CefRefPtr<CefV8Value> fn = CefV8Value::CreateFunction(fn_name, handler);
    global->SetValue(fn_name, fn, V8_PROPERTY_ATTRIBUTE_NONE);
  };
  bind_fn("__kajoNotifyShow");
  bind_fn("__kajoNotifyClose");
  bind_fn("__kajoNotifyReqPerm");
  bind_fn("__kajoNotifySyncReq");
  bind_fn("__kajoSendWebAppProbe");
  frame->ExecuteJavaScript(kWebNotifyShimScript, url, 0);
}

void KajoCefApp::OnContextReleased(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefV8Context> context) {
  if (renderer_router_) {
    renderer_router_->OnContextReleased(browser, frame, context);
  }
  if (context) {
    std::lock_guard<std::mutex> lock(perm_mu_);
    for (auto it = pending_renderer_perms_.begin(); it != pending_renderer_perms_.end();) {
      if (it->second.context && it->second.context->IsSame(context)) {
        it = pending_renderer_perms_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

bool KajoCefApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                          CefRefPtr<CefFrame> frame,
                                          CefProcessId source_process,
                                          CefRefPtr<CefProcessMessage> message) {
  if (!message) return false;
  if (renderer_router_ &&
      renderer_router_->OnProcessMessageReceived(browser, frame, source_process, message)) {
    return true;
  }
  const std::string name = message->GetName().ToString();
  if (name == "kajo-notify-perm-sync" && frame && frame->IsMain()) {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (args && args->GetSize() >= 2) {
      const std::string perm = args->GetString(1).ToString();
      const std::string safe_perm =
          (perm == "granted" || perm == "denied") ? perm : "default";
      frame->ExecuteJavaScript(
          "window.__kajoNotifyPermState = '" + safe_perm + "';",
          frame->GetURL(), 0);
    }
    return true;
  }
  if (name == "kajo-notify-perm-result" && frame && frame->IsMain()) {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (args && args->GetSize() >= 3) {
      const int req_id = args->GetInt(0);
      const std::string perm = args->GetString(2).ToString();
      const std::string safe_perm =
          (perm == "granted" || perm == "denied") ? perm : "default";
      frame->ExecuteJavaScript(
          "window.__kajoNotifyPermState = '" + safe_perm + "';",
          frame->GetURL(), 0);
      PendingRendererNotifyPerm pending;
      bool found = false;
      {
        std::lock_guard<std::mutex> lock(perm_mu_);
        auto it = pending_renderer_perms_.find(req_id);
        if (it != pending_renderer_perms_.end()) {
          pending = std::move(it->second);
          pending_renderer_perms_.erase(it);
          found = true;
        }
      }
      if (found && pending.context && pending.context->IsValid() && pending.context->Enter()) {
        CefRefPtr<CefV8Value> val = CefV8Value::CreateString(safe_perm);
        if (pending.promise && pending.promise->IsValid()) {
          pending.promise->ResolvePromise(val);
        }
        if (pending.legacy_cb && pending.legacy_cb->IsValid() &&
            pending.legacy_cb->IsFunction()) {
          CefV8ValueList cb_args{val};
          pending.legacy_cb->ExecuteFunction(nullptr, cb_args);
        }
        pending.context->Exit();
      }
    }
    return true;
  }
  if (name == "kajo-notify-event" && frame && frame->IsMain()) {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (args && args->GetSize() >= 2) {
      const std::string js_id = args->GetString(0).ToString();
      const std::string ev = args->GetString(1).ToString();
      const std::string js = "if (window.__kajoOnNotifyEvent) window.__kajoOnNotifyEvent(\"" +
                             JsonEscape(js_id) + "\", \"" + JsonEscape(ev) + "\");";
      frame->ExecuteJavaScript(js, frame->GetURL(), 0);
    }
    return true;
  }
  if (name == "kajo-probe-webapp" && frame && frame->IsMain()) {
    frame->ExecuteJavaScript(kWebAppProbeScript, frame->GetURL(), 0);
    return true;
  }
  if (name == "kajo-apply-cosmetic" && frame && frame->IsMain()) {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    const std::string css = args->GetSize() > 0 ? args->GetString(0).ToString() : "";
    const bool yt_skip = args->GetSize() > 1 ? args->GetBool(1) : false;

    std::string js =
        "(() => {"
        "  let el = document.getElementById('kajo-adblock-cosmetic');"
        "  const css = \"" + JsonEscape(css) + "\";"
        "  if (!css) { if (el) el.remove(); return; }"
        "  if (!el) {"
        "    el = document.createElement('style');"
        "    el.id = 'kajo-adblock-cosmetic';"
        "    (document.head || document.documentElement).appendChild(el);"
        "  }"
        "  el.textContent = css;"
        "})();";
    frame->ExecuteJavaScript(js, frame->GetURL(), 0);

    if (yt_skip) {
      frame->ExecuteJavaScript(kYtAdSkipScript, frame->GetURL(), 0);
    } else {
      frame->ExecuteJavaScript(
          "if (window.__kajoYtAdSkip && window.__kajoYtAdSkip.stop) {"
          "  window.__kajoYtAdSkip.stop();"
          "  delete window.__kajoYtAdSkip;"
          "  window.__kajoYtSkipInstalled = false;"
          "}",
          frame->GetURL(), 0);
    }
    return true;
  }
  return false;
}

}  // namespace kajo_web
