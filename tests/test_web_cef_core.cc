#include "web_cef/cef_prefs_helper.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <string>

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
  g_assert_cmpstr(FormatPermissionLabel(0, CEF_PERMISSION_TYPE_NOTIFICATIONS).c_str(),
                  ==, "Notifications");
  reloaded.Set("https://mail.example.com", nkey, false);
  g_assert_cmpint(reloaded.Lookup("https://mail.example.com/", nkey), ==, -1);

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

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/web_cef/core/log_severity", test_log_severity_parsing);
  g_test_add_func("/web_cef/core/same_site", test_same_site_cookie_matching);
  g_test_add_func("/web_cef/core/user_scripts", test_user_scripts_loader);
  g_test_add_func("/web_cef/core/permissions", test_permission_store_and_labels);
  g_test_add_func("/web_cef/core/slug_and_color", test_profile_slug_and_hex_color);
  return g_test_run();
}
