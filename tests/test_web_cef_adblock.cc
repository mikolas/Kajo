#include "web_cef/cef_adblock.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstring>
#include <string>

using kajo_web::CefAdblockEngine;
using kajo_web::ResourceTypeMask;

static void test_adblock_rules_and_blocking(void) {
  CefAdblockEngine engine;
  std::string rules =
      "! Comment line\n"
      "[Adblock Plus 2.0]\n"
      "||track3p.example^$third-party\n"
      "||ads.example.com^$script\n"
      "@@||ads.example.com/allowed.js$script\n"
      "0.0.0.0 tracker.evil.org\n"
      "127.0.0.1 telemetry.bad.io\n"
      "||banner.site.org^$domain=news.org|~trusted.news.org\n"
      "/adserver/pixel.gif$image\n"
      "youtube.com###player-ads\n"
      "###global-sponsored-banner\n";

  engine.SetEnabled(true);
  engine.LoadAdditionalRulesText(rules);

  /* Baseline rule blocks doubleclick */
  g_assert_true(engine.ShouldBlock("https://ad.doubleclick.net/pagead/view.js",
                                   "https://example.org/article",
                                   ResourceTypeMask::RT_SCRIPT));

  /* $third-party rule blocks third-party but skips first-party */
  g_assert_true(engine.ShouldBlock("https://cdn.track3p.example/ad.js",
                                   "https://example.org/article",
                                   ResourceTypeMask::RT_SCRIPT));
  g_assert_false(engine.ShouldBlock("https://cdn.track3p.example/static.js",
                                    "https://track3p.example/",
                                    ResourceTypeMask::RT_SCRIPT));

  /* $script resource type filter & exception rule */
  g_assert_true(engine.ShouldBlock("https://ads.example.com/banner.js",
                                   "https://site.org/",
                                   ResourceTypeMask::RT_SCRIPT));
  g_assert_false(engine.ShouldBlock("https://ads.example.com/banner.js",
                                    "https://site.org/",
                                    ResourceTypeMask::RT_IMAGE));
  g_assert_false(engine.ShouldBlock("https://ads.example.com/allowed.js",
                                    "https://site.org/",
                                    ResourceTypeMask::RT_SCRIPT));

  /* Hosts file entries */
  g_assert_true(engine.ShouldBlock("https://tracker.evil.org/collect",
                                   "https://site.org/",
                                   ResourceTypeMask::RT_XHR));
  g_assert_true(engine.ShouldBlock("https://telemetry.bad.io/ping",
                                   "https://site.org/",
                                   ResourceTypeMask::RT_XHR));

  /* $domain inclusion & negation */
  g_assert_true(engine.ShouldBlock("https://banner.site.org/a.js",
                                   "https://news.org/front",
                                   ResourceTypeMask::RT_SCRIPT));
  g_assert_false(engine.ShouldBlock("https://banner.site.org/a.js",
                                    "https://trusted.news.org/front",
                                    ResourceTypeMask::RT_SCRIPT));
  g_assert_false(engine.ShouldBlock("https://banner.site.org/a.js",
                                    "https://other.org/front",
                                    ResourceTypeMask::RT_SCRIPT));

  /* Path rule */
  g_assert_true(engine.ShouldBlock("https://cdn.example.net/adserver/pixel.gif",
                                   "https://example.net/",
                                   ResourceTypeMask::RT_IMAGE));

  /* Main frame navigation is never blocked directly */
  g_assert_false(engine.ShouldBlock("https://doubleclick.net/",
                                    "https://example.org/",
                                    ResourceTypeMask::RT_MAIN_FRAME));

  /* Internal schemes never blocked */
  g_assert_false(engine.ShouldBlock("kajo://newtab",
                                    "kajo://newtab",
                                    ResourceTypeMask::RT_ANY));
  g_assert_false(engine.ShouldBlock("kajo-chrome://ui/topbar.html",
                                    "kajo-chrome://ui/topbar.html",
                                    ResourceTypeMask::RT_ANY));
}

static void test_adblock_cosmetic_and_allowlist(void) {
  g_autofree gchar* tmp_dir = g_dir_make_tmp("kajo-web-cef-test-XXXXXX", nullptr);
  g_assert_nonnull(tmp_dir);

  CefAdblockEngine engine;
  engine.SetEnabled(true);
  engine.SetDirectories(tmp_dir, tmp_dir);
  engine.LoadAdditionalRulesText(
      "youtube.com##.ytd-ad-slot-renderer\n"
      "example.org##.ad-box\n"
      "###universal-ad-box\n");

  std::string yt_css = engine.GetCosmeticCssForHost("www.youtube.com");
  g_assert_nonnull(strstr(yt_css.c_str(), ".ytd-ad-slot-renderer"));
  g_assert_nonnull(strstr(yt_css.c_str(), "#universal-ad-box"));
  g_assert_null(strstr(yt_css.c_str(), ".ad-box"));

  /* Per-host pause persists across ReloadFromDisk */
  g_assert_false(engine.IsHostPaused("https://www.youtube.com/watch?v=123"));
  bool paused = engine.ToggleHostPaused("https://www.youtube.com/watch?v=123");
  g_assert_true(paused);
  g_assert_true(engine.IsHostPaused("https://www.youtube.com/watch?v=123"));
  g_assert_false(engine.ShouldBlock("https://googleads.g.doubleclick.net/pagead/id",
                                    "https://www.youtube.com/watch?v=123",
                                    ResourceTypeMask::RT_XHR));
  g_assert_true(engine.GetCosmeticCssForHost("www.youtube.com").empty());

  CefAdblockEngine reloaded;
  reloaded.SetEnabled(true);
  reloaded.SetDirectories(tmp_dir, tmp_dir);
  reloaded.ReloadFromDisk();
  g_assert_true(reloaded.IsHostPaused("https://m.youtube.com/"));
  g_assert_cmpuint(reloaded.GetPausedHostCount(), >=, 1);
  g_assert_cmpuint(reloaded.GetPausedHosts().size(), >=, 1);
  g_assert_cmpuint(reloaded.GetRuleCountForList("easylist"), ==, 0);
  g_assert_true(reloaded.HasMissingOrStaleLists(24));

  g_assert_true(reloaded.WriteListFileOnly("easylist", "||adtest.invalid^\n||banner.invalid^\n"));
  reloaded.ReloadFromDisk();
  g_assert_cmpuint(reloaded.GetRuleCountForList("easylist"), ==, 2);
  g_assert_cmpuint(reloaded.GetBaselineRuleCount(), >, 0);
  g_assert_true(reloaded.ShouldBlock("https://adtest.invalid/x.js",
                                     "https://example.com/",
                                     ResourceTypeMask::RT_SCRIPT));

  /* Clean temp files */
  g_autofree gchar* ab_dir = g_build_filename(tmp_dir, "adblock", nullptr);
  g_autofree gchar* el_file = g_build_filename(ab_dir, "easylist.txt", nullptr);
  g_unlink(el_file);
  g_rmdir(ab_dir);
  g_autofree gchar* allow_path = g_build_filename(tmp_dir, "adblock-allowlist.txt", nullptr);
  g_unlink(allow_path);
  g_rmdir(tmp_dir);
}

static void test_adblock_tab_counters(void) {
  CefAdblockEngine engine;
  engine.RecordBlockedForTab(101);
  engine.RecordBlockedForTab(101);
  engine.RecordBlockedForTab(202);

  g_assert_cmpint(engine.GetBlockedCountForTab(101), ==, 2);
  g_assert_cmpint(engine.GetBlockedCountForTab(202), ==, 1);
  g_assert_cmpint(engine.GetTotalBlockedCount(), ==, 3);

  engine.ResetTabBlockedCount(101);
  g_assert_cmpint(engine.GetBlockedCountForTab(101), ==, 0);
  g_assert_cmpint(engine.GetTotalBlockedCount(), ==, 3);
}

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/web_cef/adblock/rules_and_blocking", test_adblock_rules_and_blocking);
  g_test_add_func("/web_cef/adblock/cosmetic_and_allowlist", test_adblock_cosmetic_and_allowlist);
  g_test_add_func("/web_cef/adblock/tab_counters", test_adblock_tab_counters);
  return g_test_run();
}
