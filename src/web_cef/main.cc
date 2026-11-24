#include "web_cef/cef_app.h"

#include <gio/gio.h>
#include <glib.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "config/kajo_config_store.h"
#include "include/cef_app.h"
#include "web/web_model.h"
#include "web_cef/cef_prefs_helper.h"

namespace {

std::string SanitizeSlug(const char* raw) {
  g_autofree gchar* slug = web_profile_sanitize_id(raw ? raw : "default");
  return slug ? std::string(slug) : std::string("default");
}

void MigrateLegacyCacheProfileIfNeeded(const std::string& cache_dir,
                                       const std::string& profile_dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (fs::exists(profile_dir, ec)) return;
  if (!fs::exists(fs::path(cache_dir) / "Default" / "Cookies", ec)) return;
  if (fs::exists(fs::path(cache_dir) / "SingletonLock", ec)) return;

  fs::create_directories(profile_dir, ec);
  if (ec) return;
  fs::permissions(profile_dir, fs::perms::owner_all, fs::perm_options::replace, ec);

  for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name == "adblock" || name == "LOCK" || name.rfind("Singleton", 0) == 0 ||
        (name.size() > 4 && name.compare(name.size() - 4, 4, ".pma") == 0)) {
      continue;
    }
    const fs::path dst = fs::path(profile_dir) / name;
    std::error_code mv_ec;
    fs::rename(entry.path(), dst, mv_ec);
    if (mv_ec) {
      std::error_code cp_ec;
      fs::copy(entry.path(), dst,
               fs::copy_options::recursive | fs::copy_options::skip_symlinks, cp_ec);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  CefMainArgs main_args(argc, argv);

  kajo_web::LaunchConfig cfg;
  cfg.exe_path = (argc > 0 && argv[0]) ? argv[0] : "kajo-web-cef";

  bool is_sub_process = false;
  for (int i = 1; i < argc; ++i) {
    if (strncmp(argv[i], "--type=", 7) == 0) {
      is_sub_process = true;
      break;
    }
  }

  std::string profile_dir;
  if (!is_sub_process) {
    bool explicit_profile = false;
    for (int i = 1; i < argc; ++i) {
      const char* a = argv[i];
      if (strcmp(a, "--private") == 0 || strcmp(a, "-p") == 0) {
        cfg.private_mode = true;
      } else if (strcmp(a, "--no-hw-accel") == 0) {
        cfg.hw_accel_enabled = false;
      } else if (strcmp(a, "--no-adblock-sync") == 0) {
        cfg.auto_adblock_sync = false;
      } else if (strcmp(a, "--profile") == 0 && i + 1 < argc) {
        cfg.profile_id = SanitizeSlug(argv[++i]);
        explicit_profile = true;
      } else if (strncmp(a, "--profile=", 10) == 0) {
        cfg.profile_id = SanitizeSlug(a + 10);
        explicit_profile = true;
      } else if (a[0] != '-') {
        cfg.initial_urls.emplace_back(a);
      }
    }

    const gchar* data_base = g_get_user_data_dir();
    const gchar* cache_base = g_get_user_cache_dir();

    g_autofree gchar* reg_dir = g_build_filename(data_base, "kajo", "web", nullptr);
    g_autofree gchar* reg_path = g_build_filename(reg_dir, "profiles.toml", nullptr);
    KajoWebProfileRegistry* reg = web_profile_registry_new();
    if (reg) {
      web_profile_registry_load(reg, reg_path);
      if (!explicit_profile && reg->last_profile_id && reg->last_profile_id[0] != '\0') {
        cfg.profile_id = SanitizeSlug(reg->last_profile_id);
      }
      if (!cfg.private_mode) {
        const std::string resolved =
            cfg.profile_id.empty() ? std::string("default") : cfg.profile_id;
        if (g_strcmp0(reg->last_profile_id, resolved.c_str()) != 0) {
          g_free(reg->last_profile_id);
          reg->last_profile_id = g_strdup(resolved.c_str());
          g_mkdir_with_parents(reg_dir, 0700);
          web_profile_registry_save(reg, reg_path, nullptr);
        }
      }
      web_profile_registry_free(reg);
    }

    const std::string subdir =
        cfg.profile_id.empty() || cfg.profile_id == "default"
            ? "default"
            : cfg.profile_id;

    g_autofree gchar* pdata = g_build_filename(data_base, "kajo", "web-cef", subdir.c_str(), nullptr);
    g_autofree gchar* pcache = g_build_filename(cache_base, "kajo", "web-cef", subdir.c_str(), nullptr);
    g_mkdir_with_parents(pdata, 0700);
    g_mkdir_with_parents(pcache, 0700);
    cfg.data_dir = pdata;
    cfg.cache_dir = pcache;

    if (!cfg.private_mode) {
      g_autofree gchar* pprof = g_build_filename(pdata, "profile", nullptr);
      profile_dir = pprof;
      MigrateLegacyCacheProfileIfNeeded(cfg.cache_dir, profile_dir);
      g_mkdir_with_parents(profile_dir.c_str(), 0700);
    }

    cfg.widevine_path = kajo_web::DetectSystemWidevinePath();
    cfg.widevine_available = !cfg.widevine_path.empty();
    if (cfg.widevine_available && !cfg.private_mode) {
      kajo_web::EnsureWidevineComponentHint(profile_dir, cfg.widevine_path);
    }
    if (auto* cs = kajo_config_store_get_default()) {
      if (const KajoWebCfg* wcfg = kajo_config_store_acquire_web(cs)) {
        if (wcfg->user_agent_preset == KAJO_WEB_UA_SAFARI) {
          cfg.user_agent =
              "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
              "(KHTML, like Gecko) Version/18.2 Safari/605.1.15";
        } else if (wcfg->user_agent_preset == KAJO_WEB_UA_CUSTOM &&
                   wcfg->user_agent_custom && wcfg->user_agent_custom[0] != '\0') {
          cfg.user_agent = wcfg->user_agent_custom;
        } else {
          cfg.user_agent = "Chromium / CEF 156 (Kajo Native Wayland)";
        }
        kajo_web_cfg_unref(wcfg);
      }
    }
  }

  std::string priv_root_dir;
  if (cfg.private_mode && !is_sub_process) {
    g_autofree gchar* priv = g_dir_make_tmp("kajo-web-cef-priv-XXXXXX", nullptr);
    if (priv) {
      priv_root_dir = priv;
      if (cfg.widevine_available) {
        kajo_web::EnsureWidevineComponentHint(priv_root_dir, cfg.widevine_path);
      }
    }
  }

  CefRefPtr<kajo_web::KajoCefApp> app(new kajo_web::KajoCefApp(cfg));
  int exit_code = CefExecuteProcess(main_args, app, nullptr);
  if (exit_code >= 0) {
    return exit_code;
  }

  CefSettings settings;
  settings.no_sandbox = false;
  CefString(&settings.browser_subprocess_path) = "";
  CefString(&settings.resources_dir_path) = "/usr/lib/cef";
  CefString(&settings.locales_dir_path) = "/usr/lib/cef/locales";
  CefString(&settings.framework_dir_path) = "/usr/lib/cef";
  CefString(&settings.root_cache_path) =
      priv_root_dir.empty() ? (profile_dir.empty() ? cfg.cache_dir : profile_dir) : priv_root_dir;
  CefString(&settings.cache_path) = cfg.private_mode ? "" : profile_dir;
  settings.persist_session_cookies = !cfg.private_mode;
  settings.log_severity = kajo_web::ParseCefLogSeverity(getenv("KAJO_CEF_LOG"));

  /* Enable CDP remote debugging when explicitly requested via env */
  if (const char* port_env = getenv("KAJO_CEF_DEBUG_PORT")) {
    int port = atoi(port_env);
    if (port > 0) settings.remote_debugging_port = port;
  }

  if (!CefInitialize(main_args, settings, app, nullptr)) {
    app->ShutdownRelease();
    app = nullptr;
    if (!priv_root_dir.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(priv_root_dir, ec);
    }
    return 1;
  }

  CefRunMessageLoop();
  app->ShutdownRelease();
  app = nullptr;
  CefShutdown();
  if (!priv_root_dir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(priv_root_dir, ec);
  }
  return 0;
}
