#include "web_cef/cef_app.h"

#include <gio/gio.h>
#include <glib.h>

#include <cstring>
#include <string>

#include "config/kajo_config_store.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_helpers.h"

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
  if (window.__kajoYtAdSkip) return;
  window.__kajoYtAdSkip = true;
  window.__kajoYtSkipInstalled = true;
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
    if (!curPlayer) return;
    const ad = curPlayer.classList.contains('ad-showing') || curPlayer.classList.contains('ad-interrupting');
    if (ad) {
      trySkip();
      if (!skipTimer) skipTimer = setInterval(trySkip, 250);
    } else if (inAd || skipTimer) {
      restorePlayer();
    }
  };
  const attach = () => {
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
    if (attach()) return;
    if (location.pathname.indexOf('/watch') === 0 || location.pathname.indexOf('/shorts') === 0) {
      if (document.body && !bootObs) {
        bootObs = new MutationObserver(() => attach());
        bootObs.observe(document.body, { childList: true, subtree: true });
        bootStop = setTimeout(stopBoot, 3000);
      }
    }
  };
  document.addEventListener('yt-navigate-finish', () => setTimeout(boot, 0));
  document.addEventListener('yt-page-data-updated', () => attach());
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
  g_autofree gchar* hint_path =
      g_build_filename(root_cache_dir.c_str(), "latest-component-updated-widevine-cdm", nullptr);
  if (g_file_test(hint_path, G_FILE_TEST_EXISTS)) return;
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

void KajoCefApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();

  adblock_ = std::make_shared<CefAdblockEngine>();
  adblock_->SetDirectories(cfg_.cache_dir, cfg_.data_dir);
  adblock_->ReloadFromDisk();

  store_ = std::make_shared<ChromeStateStore>();

  CefRefPtr<CefRequestContext> ctx = CefRequestContext::GetGlobalContext();
  RegisterKajoSchemeHandlers(ctx, store_);

  main_window_ = new KajoBrowserWindow(cfg_, adblock_, store_, ctx);
  main_window_->ShowWindow();
}

void KajoCefApp::ShutdownRelease() {
  main_window_ = nullptr;
}

void KajoCefApp::OnWebKitInitialized() {
  CefMessageRouterConfig rc;
  renderer_router_ = CefMessageRouterRendererSide::Create(rc);
}

void KajoCefApp::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefRefPtr<CefV8Context> context) {
  if (!renderer_router_ || !frame) return;
  const std::string url = frame->GetURL().ToString();
  if (url.rfind("kajo-chrome://", 0) == 0) {
    renderer_router_->OnContextCreated(browser, frame, context);
  }
}

void KajoCefApp::OnContextReleased(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefV8Context> context) {
  if (!renderer_router_) return;
  renderer_router_->OnContextReleased(browser, frame, context);
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
  if (message->GetName() == "kajo-apply-cosmetic" && frame && frame->IsMain()) {
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
    }
    return true;
  }
  return false;
}

}  // namespace kajo_web
