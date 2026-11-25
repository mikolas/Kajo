#include "web_cef/cef_prefs_helper.h"
#include "web_cef/cef_ipc.h"
#include "web_cef/cef_media_dl.h"
#include "web_cef/cef_site_util.h"
#include "web_cef/cef_webapps.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <string>
#include <vector>

using namespace kajo_web;

static void test_log_severity_parsing(void) {
  g_assert_cmpint(ParseCefLogSeverity(nullptr), ==, LOGSEVERITY_FATAL);
  g_assert_cmpint(ParseCefLogSeverity(""), ==, LOGSEVERITY_FATAL);
  g_assert_cmpint(ParseCefLogSeverity("warn"), ==, LOGSEVERITY_WARNING);
  g_assert_cmpint(ParseCefLogSeverity("WARNING"), ==, LOGSEVERITY_WARNING);
  g_assert_cmpint(ParseCefLogSeverity("info"), ==, LOGSEVERITY_INFO);
  g_assert_cmpint(ParseCefLogSeverity("verbose"), ==, LOGSEVERITY_VERBOSE);
  g_assert_cmpint(ParseCefLogSeverity("error"), ==, LOGSEVERITY_ERROR);
  g_assert_cmpint(ParseCefLogSeverity("disable"), ==, LOGSEVERITY_DISABLE);
  g_assert_cmpint(ParseCefLogSeverity("off"), ==, LOGSEVERITY_DISABLE);
}

static void test_same_site_cookie_matching(void) {
  g_assert_true(IsSameSiteUrl("https://cdn.example.com/a.png", "https://www.example.com/"));
  g_assert_true(IsSameSiteUrl("https://news.bbc.co.uk/static.js", "https://www.bbc.co.uk/"));
  g_assert_false(IsSameSiteUrl("https://tracker.doubleclick.net/px", "https://www.example.com/"));
  g_assert_false(IsSameSiteUrl("https://evil.co.uk/px", "https://www.bbc.co.uk/"));
  g_assert_true(IsSameSiteUrl("https://api.example.com:8443/v1", "http://example.com:8080/"));
  g_assert_true(IsSameSiteUrl("kajo-chrome://ui/topbar.html", "https://example.com/"));
}

static void test_user_scripts_loader(void) {
  g_autofree gchar* tmp_dir = g_dir_make_tmp("kajo-cef-scripts-XXXXXX", nullptr);
  g_assert_nonnull(tmp_dir);

  g_autofree gchar* css_a = g_build_filename(tmp_dir, "01-dark.css", nullptr);
  g_autofree gchar* js_a = g_build_filename(tmp_dir, "02-helper.js", nullptr);
  g_autofree gchar* hidden = g_build_filename(tmp_dir, ".ignored.js", nullptr);
  g_file_set_contents(css_a, "body { color: #eee; }", -1, nullptr);
  g_file_set_contents(js_a, "window.__testInjected = 1;", -1, nullptr);
  g_file_set_contents(hidden, "ignored();", -1, nullptr);

  LoadedUserScripts res = LoadUserScriptsFromDir(tmp_dir);
  g_assert_nonnull(strstr(res.combined_css.c_str(), "color: #eee"));
  g_assert_cmpuint(res.js_files.size(), ==, 1);
  g_assert_cmpstr(res.js_files[0].first.c_str(), ==, "02-helper.js");

  g_unlink(css_a);
  g_unlink(js_a);
  g_unlink(hidden);
  g_rmdir(tmp_dir);
}

static void test_permission_store_and_labels(void) {
  g_autofree gchar* tmp_dir = g_dir_make_tmp("kajo-cef-perms-XXXXXX", nullptr);
  g_autofree gchar* path = g_build_filename(tmp_dir, "permissions.json", nullptr);

  PermissionStore st;
  const std::string key = PermissionKey(
      CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE, 0);
  const std::string lbl = FormatPermissionLabel(
      CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE, 0);
  g_assert_cmpstr(lbl.c_str(), ==, "Camera & Microphone");

  g_assert_cmpint(st.Lookup("https://meet.example.com/room", key), ==, 0);
  st.Set("https://meet.example.com/room", key, true);
  g_assert_true(st.Save(path));

  PermissionStore reloaded;
  g_assert_true(reloaded.Load(path));
  g_assert_cmpint(reloaded.Lookup("https://meet.example.com", key), ==, 1);
  g_assert_cmpint(reloaded.Lookup("https://other.example.com", key), ==, 0);

  const std::string nkey = PermissionKey(0, CEF_PERMISSION_TYPE_NOTIFICATIONS);
  const std::string nl_label = FormatPermissionLabel(0, CEF_PERMISSION_TYPE_NOTIFICATIONS);
  g_assert_cmpstr(nl_label.c_str(), ==, "Notifications");
  reloaded.Set("https://mail.example.com", nkey, false);
  g_assert_cmpint(reloaded.Lookup("https://mail.example.com/", nkey), ==, -1);

  auto list = reloaded.ListEntries();
  g_assert_cmpuint(list.size(), ==, 2);
  const std::string klbl = FormatPermissionKeyLabel(key);
  const std::string nlbl = FormatPermissionKeyLabel(nkey);
  g_assert_cmpstr(klbl.c_str(), ==, "Camera & Microphone");
  g_assert_cmpstr(nlbl.c_str(), ==, "Notifications");
  g_assert_true(reloaded.Remove("https://mail.example.com/", nkey));
  g_assert_cmpuint(reloaded.Size(), ==, 1);

  const std::string home = g_get_home_dir() ? g_get_home_dir() : "";
  const std::string e_empty = ExpandTildePath("");
  const std::string e_tilde = ExpandTildePath(" ~ ");
  const std::string e_dl = ExpandTildePath("~/Downloads");
  const std::string e_abs = ExpandTildePath("/tmp/dl");
  const std::string exp_dl = home.empty() ? "~/Downloads" : (home + "/Downloads");
  g_assert_cmpstr(e_empty.c_str(), ==, "");
  g_assert_cmpstr(e_tilde.c_str(), ==, home.c_str());
  g_assert_cmpstr(e_dl.c_str(), ==, exp_dl.c_str());
  g_assert_cmpstr(e_abs.c_str(), ==, "/tmp/dl");

  g_unlink(path);
  g_rmdir(tmp_dir);
}

static void test_profile_slug_and_hex_color(void) {
  g_assert_false(IsSafeProfileSlug("default"));
  g_assert_false(IsSafeProfileSlug(""));
  g_assert_false(IsSafeProfileSlug("../etc"));
  g_assert_false(IsSafeProfileSlug("Work Space"));
  g_assert_true(IsSafeProfileSlug("work-2"));

  cef_color_t c = ParseHexColorArgb("#0078d4", 0);
  g_assert_cmpuint(CefColorGetA(c), ==, 255);
  g_assert_cmpuint(CefColorGetR(c), ==, 0x00);
  g_assert_cmpuint(CefColorGetG(c), ==, 0x78);
  g_assert_cmpuint(CefColorGetB(c), ==, 0xd4);
  g_assert_cmpuint(ParseHexColorArgb("bad", 42), ==, 42);
}

static void test_site_util_and_ipc(void) {
  g_assert_true(IsLocalOrPrivateHost("localhost"));
  g_assert_true(IsLocalOrPrivateHost("app.localhost"));
  g_assert_true(IsLocalOrPrivateHost("nas.local"));
  g_assert_true(IsLocalOrPrivateHost("127.0.0.1"));
  g_assert_true(IsLocalOrPrivateHost("::1"));
  g_assert_true(IsLocalOrPrivateHost("[::1]"));
  g_assert_true(IsLocalOrPrivateHost("10.0.0.1"));
  g_assert_true(IsLocalOrPrivateHost("192.168.1.50"));
  g_assert_true(IsLocalOrPrivateHost("172.16.0.1"));
  g_assert_true(IsLocalOrPrivateHost("172.31.255.254"));
  g_assert_false(IsLocalOrPrivateHost("172.15.0.1"));
  g_assert_false(IsLocalOrPrivateHost("172.32.0.1"));
  g_assert_false(IsLocalOrPrivateHost("example.com"));

  const std::string href1 = SanitizeSafeHref("https://example.com/a?b=1");
  const std::string href2 = SanitizeSafeHref("kajo://downloads");
  const std::string href3 = SanitizeSafeHref("javascript:alert(1)");
  const std::string href4 = SanitizeSafeHref("data:text/html,<script>");
  g_assert_cmpstr(href1.c_str(), ==, "https://example.com/a?b=1");
  g_assert_cmpstr(href2.c_str(), ==, "kajo://downloads");
  g_assert_cmpstr(href3.c_str(), ==, "#");
  g_assert_cmpstr(href4.c_str(), ==, "#");

  const std::string col1 = SanitizeHexColor("#1a2B3c", "#0078d4");
  const std::string col2 = SanitizeHexColor("red;color:x", "#0078d4");
  g_assert_cmpstr(col1.c_str(), ==, "#1a2B3c");
  g_assert_cmpstr(col2.c_str(), ==, "#0078d4");

  g_autofree gchar* tmp_dir = g_dir_make_tmp("kajo-cef-ipc-XXXXXX", nullptr);
  g_autofree gchar* sock_path = g_build_filename(tmp_dir, "test.sock", nullptr);
  g_assert_false(TryForwardToRunningInstance(sock_path, {"https://a.example"}, 50));

  std::vector<std::string> got;
  GMutex mu;
  GCond cond;
  g_mutex_init(&mu);
  g_cond_init(&cond);
  bool received = false;

  ProfileIpcServer srv;
  g_assert_true(srv.Start(sock_path, [&](std::vector<std::string> urls) {
    g_mutex_lock(&mu);
    got = std::move(urls);
    received = true;
    g_cond_signal(&cond);
    g_mutex_unlock(&mu);
  }));

  g_assert_true(TryForwardToRunningInstance(
      sock_path, {"https://example.org", "https://kajo.local/test"}, 500));

  g_mutex_lock(&mu);
  const gint64 end_us = g_get_monotonic_time() + 1000000;
  while (!received) {
    if (!g_cond_wait_until(&cond, &mu, end_us)) break;
  }
  g_mutex_unlock(&mu);

  g_assert_true(received);
  g_assert_cmpuint(got.size(), ==, 2);
  g_assert_cmpstr(got[0].c_str(), ==, "https://example.org");
  g_assert_cmpstr(got[1].c_str(), ==, "https://kajo.local/test");

  srv.Stop();
  g_assert_false(TryForwardToRunningInstance(sock_path, {}, 50));
  g_cond_clear(&cond);
  g_mutex_clear(&mu);
  g_rmdir(tmp_dir);

  const std::string f1 = DeriveOriginFaviconFallback("https://github.com/org/repo?x=1");
  const std::string f2 = DeriveOriginFaviconFallback("http://localhost:8080/app");
  const std::string f3 = DeriveOriginFaviconFallback("https://user:pass@example.com/p");
  const std::string f4 = DeriveOriginFaviconFallback("kajo://newtab");
  const std::string f5 = DeriveOriginFaviconFallback("about:blank");
  g_assert_cmpstr(f1.c_str(), ==, "https://github.com/favicon.ico");
  g_assert_cmpstr(f2.c_str(), ==, "http://localhost:8080/favicon.ico");
  g_assert_cmpstr(f3.c_str(), ==, "https://example.com/favicon.ico");
  g_assert_cmpstr(f4.c_str(), ==, "");
  g_assert_cmpstr(f5.c_str(), ==, "");

  const ChromeUiMetrics m_min = ComputeChromeUiMetrics(4);
  const ChromeUiMetrics m_11 = ComputeChromeUiMetrics(11);
  const ChromeUiMetrics m_14 = ComputeChromeUiMetrics(14);
  const ChromeUiMetrics m_20 = ComputeChromeUiMetrics(20);
  const ChromeUiMetrics m_max = ComputeChromeUiMetrics(99);
  g_assert_cmpint(m_min.topbar_height_px, ==, m_11.topbar_height_px);
  g_assert_cmpint(m_max.topbar_height_px, ==, m_20.topbar_height_px);
  g_assert_cmpint(m_14.topbar_height_px, ==, 42);
  g_assert_cmpint(m_14.control_height_px, ==, 30);
  g_assert_cmpint(m_14.icon_size_px, ==, 16);
  g_assert_cmpint(m_14.infobar_height_px, ==, 44);
  g_assert_cmpint(m_14.findbar_height_px, ==, 50);
  g_assert_cmpint(m_14.compact_rail_width_px, ==, 48);
  g_assert_cmpint(m_20.topbar_height_px, >, m_14.topbar_height_px);
  g_assert_cmpint(m_14.topbar_height_px, >=, m_11.topbar_height_px);
}

static void test_webapps_core_and_manifest(void) {
  const std::string id1 = SanitizeWebAppId("Apple Music");
  const std::string id2 = SanitizeWebAppId("  Microsoft Teams (Work)! ");
  const std::string id3 = SanitizeWebAppId("///");
  g_assert_cmpstr(id1.c_str(), ==, "apple-music");
  g_assert_cmpstr(id2.c_str(), ==, "microsoft-teams-work");
  g_assert_cmpstr(id3.c_str(), ==, "webapp");

  const std::string wid = WebAppWaylandAppId("Work", "Apple Music");
  const std::string did = WebAppDesktopId("default", "teams");
  g_assert_cmpstr(wid.c_str(), ==, "kajo-webapp-work-apple-music");
  g_assert_cmpstr(did.c_str(), ==, "kajo-webapp-default-teams.desktop");

  const std::string orig = DeriveOriginFromUrl("https://music.apple.com/us/browse");
  const std::string res_rel = ResolveUrlAgainstBase("https://web.whatsapp.com/app/", "../icons/wa.png");
  g_assert_cmpstr(orig.c_str(), ==, "https://music.apple.com/");
  g_assert_cmpstr(res_rel.c_str(), ==, "https://web.whatsapp.com/icons/wa.png");

  const std::string manifest = R"JSON({
    "name": "Apple Music",
    "short_name": "Music",
    "start_url": "/us/home?l=en",
    "scope": "/",
    "theme_color": "#f43",
    "display": "standalone",
    "icons": [
      { "src": "/img/icon-64.png", "sizes": "64x64", "type": "image/png" },
      { "src": "/img/icon-512.png", "sizes": "512x512", "type": "image/png" }
    ]
  })JSON";
  WebManifestParseResult parsed =
      ParseWebManifestJson(manifest, "https://music.apple.com/manifest.webmanifest", "https://music.apple.com/");
  g_assert_true(parsed.valid);
  g_assert_cmpstr(parsed.name.c_str(), ==, "Apple Music");
  g_assert_cmpstr(parsed.start_url.c_str(), ==, "https://music.apple.com/us/home?l=en");
  g_assert_cmpstr(parsed.scope.c_str(), ==, "https://music.apple.com/");
  g_assert_cmpstr(parsed.theme_color.c_str(), ==, "#ff4433");
  g_assert_cmpstr(parsed.best_icon_url.c_str(), ==, "https://music.apple.com/img/icon-512.png");

  WebAppEntry teams;
  teams.id = "microsoft-teams";
  teams.name = "Microsoft Teams";
  teams.profile_id = "work";
  teams.start_url = "https://teams.microsoft.com/v2/";
  teams.scope = "https://teams.microsoft.com/";
  teams.theme_color = "#5b5fc7";

  g_assert_true(IsUrlInWebAppScope(teams, "https://teams.microsoft.com/v2/#/chats"));
  g_assert_false(IsUrlInWebAppScope(teams, "https://github.com/org/repo"));
  g_assert_true(IsLikelyAuthFlowUrl("https://login.microsoftonline.com/common/oauth2/authorize"));
  g_assert_true(IsLikelyAuthFlowUrl("https://idmsa.apple.com/IDMSWebAuth/signin"));
  g_assert_false(IsLikelyAuthFlowUrl("https://example.com/blog/post"));

  g_autofree gchar* tmp_dir = g_dir_make_tmp("kajo-cef-webapps-XXXXXX", nullptr);
  g_autofree gchar* store_path = g_build_filename(tmp_dir, "webapps.json", nullptr);
  WebAppStore store;
  store.Upsert(teams);
  const std::string uniq = store.MakeUniqueId("microsoft-teams");
  g_assert_cmpstr(uniq.c_str(), ==, "microsoft-teams-2");
  g_assert_true(store.TouchLaunched("microsoft-teams"));
  g_assert_true(store.Save(store_path));

  WebAppStore reloaded;
  g_assert_true(reloaded.Load(store_path));
  g_assert_cmpuint(reloaded.Size(), ==, 1);
  auto match = reloaded.FindBestMatchForUrl("https://teams.microsoft.com/v2/");
  g_assert_true(match.has_value());
  g_assert_cmpstr(match->desktop_id.c_str(), ==, "kajo-webapp-work-microsoft-teams.desktop");

  std::string out_icon, out_desktop;
  g_assert_true(InstallWebAppDesktopFilesWithDirs(
      *match, "/usr/local/bin/kajo-web", {}, "", tmp_dir, &out_icon, &out_desktop));
  g_assert_true(g_file_test(out_icon.c_str(), G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(out_desktop.c_str(), G_FILE_TEST_EXISTS));
  g_assert_nonnull(strstr(out_icon.c_str(), ".svg"));
  RemoveWebAppDesktopFilesWithDirs(*match, tmp_dir);
  g_assert_false(g_file_test(out_icon.c_str(), G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(out_desktop.c_str(), G_FILE_TEST_EXISTS));

  std::vector<uint8_t> fake_png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0};
  std::string png_icon, png_desktop;
  g_assert_true(InstallWebAppDesktopFilesWithDirs(
      *match, "/usr/local/bin/kajo-web", fake_png, "image/png", tmp_dir, &png_icon, &png_desktop));
  g_assert_nonnull(strstr(png_icon.c_str(), ".png"));
  g_assert_true(g_file_test(png_icon.c_str(), G_FILE_TEST_EXISTS));
  RemoveWebAppDesktopFilesWithDirs(*match, tmp_dir);

  g_unlink(store_path);
  g_autofree gchar* apps_d = g_build_filename(tmp_dir, "applications", nullptr);
  g_autofree gchar* d1 = g_build_filename(tmp_dir, "icons/hicolor/scalable/apps", nullptr);
  g_autofree gchar* d2 = g_build_filename(tmp_dir, "icons/hicolor/256x256/apps", nullptr);
  g_autofree gchar* d3 = g_build_filename(tmp_dir, "icons/hicolor/scalable", nullptr);
  g_autofree gchar* d4 = g_build_filename(tmp_dir, "icons/hicolor/256x256", nullptr);
  g_autofree gchar* d5 = g_build_filename(tmp_dir, "icons/hicolor", nullptr);
  g_autofree gchar* d6 = g_build_filename(tmp_dir, "icons", nullptr);
  g_rmdir(apps_d);
  g_rmdir(d1);
  g_rmdir(d2);
  g_rmdir(d3);
  g_rmdir(d4);
  g_rmdir(d5);
  g_rmdir(d6);
  g_rmdir(tmp_dir);
}

static void test_ipc_app_forwarding(void) {
  g_autofree gchar* tmp_dir = g_dir_make_tmp("kajo-cef-ipc-app-XXXXXX", nullptr);
  g_autofree gchar* sock_path = g_build_filename(tmp_dir, "app.sock", nullptr);

  IpcOpenRequest got;
  GMutex mu;
  GCond cond;
  g_mutex_init(&mu);
  g_cond_init(&cond);
  bool received = false;

  ProfileIpcServer srv;
  g_assert_true(srv.StartEx(sock_path, [&](IpcOpenRequest req) {
    g_mutex_lock(&mu);
    got = std::move(req);
    received = true;
    g_cond_signal(&cond);
    g_mutex_unlock(&mu);
  }));

  IpcOpenRequest send_req;
  send_req.app_id = "apple-music";
  send_req.app_url = "https://music.apple.com/us/browse";
  send_req.urls = {"https://example.org"};
  g_assert_true(TryForwardAppToRunningInstance(sock_path, send_req, 500));

  g_mutex_lock(&mu);
  const gint64 end_us = g_get_monotonic_time() + 1000000;
  while (!received) {
    if (!g_cond_wait_until(&cond, &mu, end_us)) break;
  }
  g_mutex_unlock(&mu);

  g_assert_true(received);
  g_assert_cmpstr(got.app_id.c_str(), ==, "apple-music");
  g_assert_cmpstr(got.app_url.c_str(), ==, "https://music.apple.com/us/browse");
  g_assert_cmpuint(got.urls.size(), ==, 1);
  g_assert_cmpstr(got.urls[0].c_str(), ==, "https://example.org");

  srv.Stop();
  g_cond_clear(&cond);
  g_mutex_clear(&mu);
  g_rmdir(tmp_dir);
}

static void test_zoom_shortcut_classifier_and_levels(void) {
  /* US OEM keys */
  g_assert_true(ClassifyZoomShortcutKey(0xBB, '=', '=') == ZoomShortcutAction::kZoomIn);
  g_assert_true(ClassifyZoomShortcutKey(0xBD, '-', '-') == ZoomShortcutAction::kZoomOut);
  g_assert_true(ClassifyZoomShortcutKey('0', '0', '0') == ZoomShortcutAction::kZoomReset);
  /* Nordic/European '+' key (VK may differ, character/unmodified_character is '+') */
  g_assert_true(ClassifyZoomShortcutKey(0x6B, 0, 0) == ZoomShortcutAction::kZoomIn);
  g_assert_true(ClassifyZoomShortcutKey(0, '+', '+') == ZoomShortcutAction::kZoomIn);
  g_assert_true(ClassifyZoomShortcutKey(0, '-', '-') == ZoomShortcutAction::kZoomOut);
  /* Numpad */
  g_assert_true(ClassifyZoomShortcutKey(0x6D, 0, 0) == ZoomShortcutAction::kZoomOut);
  g_assert_true(ClassifyZoomShortcutKey(0x60, 0, 0) == ZoomShortcutAction::kZoomReset);
  g_assert_true(ClassifyZoomShortcutKey('A', 'a', 'a') == ZoomShortcutAction::kNone);

  g_assert_cmpfloat(ZoomPercentToCefLevel(100), ==, 0.0);
  g_assert_cmpfloat(ZoomPercentToCefLevel(120), >, 0.99);
  g_assert_cmpfloat(ZoomPercentToCefLevel(120), <, 1.01);
  g_assert_cmpfloat(ZoomPercentToCefLevel(80), <, 0.0);
}

static void test_media_dl_helpers_and_ffmpeg(void) {
  const std::string fn1 = SanitizeMediaFilename("My: Video / Test?.mp4", "vid");
  const std::string fn2 = SanitizeMediaFilename("https://cdn.example.com/hls/master.m3u8?x=1", "stream");
  const std::string fn3 = SanitizeMediaFilename("///", "fallback");
  g_assert_cmpstr(fn1.c_str(), ==, "My_ Video _ Test");
  g_assert_cmpstr(fn2.c_str(), ==, "master");
  g_assert_cmpstr(fn3.c_str(), ==, "fallback");

  const std::string hdr = BuildFfmpegHeaderArg(
      "https://site.example/watch\r\nX-Injected: bad",
      "sid=abc\n");
  g_assert_cmpstr(hdr.c_str(), ==, "Referer: https://site.example/watchX-Injected: bad\r\nCookie: sid=abc\r\n");

  const auto mp4_args = BuildFfmpegRemuxArgs(
      "/usr/bin/ffmpeg", "https://cdn.example/master.m3u8", "/tmp/out.mp4",
      hdr, "TestUA/1.0", false);
  bool has_faststart = false, has_bsf = false, has_headers = false;
  for (const auto& a : mp4_args) {
    if (a == "+faststart") has_faststart = true;
    if (a == "aac_adtstoasc") has_bsf = true;
    if (a == "-headers") has_headers = true;
  }
  g_assert_true(has_faststart && has_bsf && has_headers);

  const auto mkv_args = BuildFfmpegRemuxArgs(
      "/usr/bin/ffmpeg", "/tmp/local.m3u8", "/tmp/out.mkv",
      hdr, "TestUA/1.0", true);
  bool mkv_has_bsf = false, mkv_has_headers = false;
  for (const auto& a : mkv_args) {
    if (a == "aac_adtstoasc") mkv_has_bsf = true;
    if (a == "-headers") mkv_has_headers = true;
  }
  g_assert_false(mkv_has_bsf);
  g_assert_false(mkv_has_headers); /* local path skips HTTP -headers */

  MediaProgressSample sample;
  g_assert_true(ParseFfmpegProgressLine("out_time_us=1500000", &sample));
  g_assert_cmpint(sample.out_time_us, ==, 1500000);
  g_assert_true(ParseFfmpegProgressLine("total_size=524288", &sample));
  g_assert_cmpint(sample.total_size, ==, 524288);
  g_assert_true(ParseFfmpegProgressLine("speed=2.5x", &sample));
  g_assert_cmpfloat(sample.speed_x, >, 2.4);
  g_assert_true(ParseFfmpegProgressLine("progress=end", &sample));
  g_assert_true(sample.progress_end);

  int pct = 0;
  int64_t spd = 0, tot = 0;
  g_assert_true(ParseYtDlpProgressLine(
      "[download]  45.2% of ~  100.00MiB at    2.50MiB/s ETA 00:22", &pct, &spd, &tot));
  g_assert_cmpint(pct, ==, 45);
  g_assert_cmpint(tot, >, 100000000LL);
  g_assert_cmpint(spd, >, 2000000LL);

  g_autofree gchar* tmp_dir = g_dir_make_tmp("kajo-cef-dl-XXXXXX", nullptr);
  g_assert_nonnull(tmp_dir);
  g_autofree gchar* existing = g_build_filename(tmp_dir, "clip.mp4", nullptr);
  g_file_set_contents(existing, "x", 1, nullptr);
  const std::string uniq = BuildUniqueMediaPath(tmp_dir, "clip", "mp4");
  g_assert_true(uniq.find("clip-1.mp4") != std::string::npos);

  if (IsFfmpegAvailable()) {
    g_autofree gchar* m3u8_path = g_build_filename(tmp_dir, "tiny.m3u8", nullptr);
    g_autofree gchar* seg_tmpl = g_build_filename(tmp_dir, "tiny%d.ts", nullptr);
    g_autofree gchar* seg0_path = g_build_filename(tmp_dir, "tiny0.ts", nullptr);
    const gchar* gen_argv[] = {
        "ffmpeg", "-y", "-nostdin", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "color=c=black:s=64x64:d=0.4:r=25",
        "-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo",
        "-t", "0.4", "-pix_fmt", "yuv420p", "-c:v", "libx264", "-c:a", "aac",
        "-hls_time", "1", "-hls_list_size", "0",
        "-hls_segment_filename", seg_tmpl, m3u8_path, nullptr};
    gint wait_status = 1;
    const gboolean spawned = g_spawn_sync(
        nullptr, const_cast<gchar**>(gen_argv), nullptr, G_SPAWN_SEARCH_PATH,
        nullptr, nullptr, nullptr, nullptr, &wait_status, nullptr);
    g_assert_true(spawned);
    g_assert_true(g_spawn_check_wait_status(wait_status, nullptr));
    g_assert_true(g_file_test(m3u8_path, G_FILE_TEST_EXISTS));

    GMutex mu;
    GCond cond;
    g_mutex_init(&mu);
    g_cond_init(&cond);
    bool done = false;
    ChromeDownloadView final_view;

    MediaDownloadManager mgr([&](const ChromeDownloadView& v) {
      g_mutex_lock(&mu);
      final_view = v;
      if (v.complete || v.canceled) {
        done = true;
        g_cond_signal(&cond);
      }
      g_mutex_unlock(&mu);
    });
    const uint32_t jid = mgr.StartFfmpegStream(
        m3u8_path, "Synthetic Test Clip", tmp_dir, "", "", "");
    g_assert_cmpuint(jid, >, 0);

    g_mutex_lock(&mu);
    const gint64 deadline = g_get_monotonic_time() + 10000000LL;
    while (!done) {
      if (!g_cond_wait_until(&cond, &mu, deadline)) break;
    }
    g_mutex_unlock(&mu);

    g_assert_true(done);
    g_assert_true(final_view.complete);
    g_assert_false(final_view.canceled);
    g_assert_true(g_file_test(final_view.full_path.c_str(), G_FILE_TEST_EXISTS));
    g_assert_cmpint(final_view.total_bytes, >, 0);
    if (!final_view.full_path.empty()) g_unlink(final_view.full_path.c_str());

    g_cond_clear(&cond);
    g_mutex_clear(&mu);
    g_unlink(m3u8_path);
    g_unlink(seg0_path);
  }
  g_unlink(existing);
  g_rmdir(tmp_dir);
}

static void test_context_menu_helpers(void) {
  g_assert_cmpstr(StripMenuMnemonic("&Back").c_str(), ==, "Back");
  g_assert_cmpstr(StripMenuMnemonic("Save &As...").c_str(), ==, "Save As...");
  g_assert_cmpstr(StripMenuMnemonic("A && B").c_str(), ==, "A & B");
  g_assert_cmpstr(StripMenuMnemonic("Cut & Paste").c_str(), ==, "Cut & Paste");
  g_assert_cmpstr(StripMenuMnemonic("Plain").c_str(), ==, "Plain");
  g_assert_cmpstr(StripMenuMnemonic("End&").c_str(), ==, "End&");

  g_assert_cmpstr(FormatMenuAccelerator('C', false, true, false).c_str(), ==, "Ctrl+C");
  g_assert_cmpstr(FormatMenuAccelerator('I', true, true, false).c_str(), ==, "Ctrl+Shift+I");
  g_assert_cmpstr(FormatMenuAccelerator(0x25, false, false, true).c_str(), ==, "Alt+←");
  g_assert_cmpstr(FormatMenuAccelerator(0x74, false, false, false).c_str(), ==, "F5");
  g_assert_cmpstr(FormatMenuAccelerator('0', false, true, false).c_str(), ==, "Ctrl+0");
  g_assert_cmpstr(FormatMenuAccelerator(0, false, true, false).c_str(), ==, "");

  const ContextMenuBounds normal = ComputeContextMenuBounds(100, 120, 220, 300, 1280, 720);
  g_assert_cmpint(normal.x, ==, 100);
  g_assert_cmpint(normal.y, ==, 120);
  g_assert_cmpint(normal.max_h, ==, 708);

  const ContextMenuBounds flipped = ComputeContextMenuBounds(1200, 680, 220, 300, 1280, 720);
  g_assert_cmpint(flipped.x, ==, 980);
  g_assert_cmpint(flipped.y, ==, 380);

  const ContextMenuBounds tiny = ComputeContextMenuBounds(10, 10, 300, 400, 200, 150);
  g_assert_cmpint(tiny.x, ==, 6);
  g_assert_cmpint(tiny.y, ==, 6);
  g_assert_cmpint(tiny.max_h, ==, 138);

  const ContextMenuDimensions dim_root = EstimateContextMenuDimensions(6, 2, 20, 12, false);
  g_assert_cmpint(dim_root.width, >=, 216);
  g_assert_cmpint(dim_root.width, <=, 340);
  g_assert_cmpint(dim_root.height, ==, 8 + 6 * 27 + 2 * 7);

  const ContextMenuDimensions dim_sub = EstimateContextMenuDimensions(3, 0, 10, 12, true);
  g_assert_cmpint(dim_sub.width, ==, 216);
  g_assert_cmpint(dim_sub.height, >, 8 + 3 * 27);
}

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/web_cef/core/log_severity", test_log_severity_parsing);
  g_test_add_func("/web_cef/core/same_site", test_same_site_cookie_matching);
  g_test_add_func("/web_cef/core/user_scripts", test_user_scripts_loader);
  g_test_add_func("/web_cef/core/permissions", test_permission_store_and_labels);
  g_test_add_func("/web_cef/core/slug_and_color", test_profile_slug_and_hex_color);
  g_test_add_func("/web_cef/core/site_util_and_ipc", test_site_util_and_ipc);
  g_test_add_func("/web_cef/core/webapps", test_webapps_core_and_manifest);
  g_test_add_func("/web_cef/core/ipc_app_forwarding", test_ipc_app_forwarding);
  g_test_add_func("/web_cef/core/zoom_shortcuts", test_zoom_shortcut_classifier_and_levels);
  g_test_add_func("/web_cef/core/media_dl", test_media_dl_helpers_and_ffmpeg);
  g_test_add_func("/web_cef/core/context_menu_helpers", test_context_menu_helpers);
  return g_test_run();
}
