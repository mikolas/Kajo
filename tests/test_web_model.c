#include "../src/web/web_model.h"
#include "../src/web/web_sync_merge.h"
#include "../src/web/web_media_sniff_core.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* 1. Autohide Rail FSM (entry, exit, timeout, wait-then-input, hold-open, focus-loss) */
static void
test_rail_fsm_transitions(void)
{
    KajoWebRailFsm fsm;
    web_rail_fsm_init(&fsm);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_COLLAPSED);

    /* Hover edge strip -> starts reveal timer */
    g_assert_cmpint(web_rail_fsm_trigger_enter(&fsm), ==, KAJO_RAIL_ACT_START_REVEAL_TIMER);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_REVEAL_WAIT);

    /* Leave before reveal timer fires -> cancels timer and collapses */
    g_assert_cmpint(web_rail_fsm_trigger_leave(&fsm), ==, KAJO_RAIL_ACT_CANCEL_REVEAL_TIMER);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_COLLAPSED);

    /* Stale reveal timeout after leave -> swallowed */
    g_assert_cmpint(web_rail_fsm_reveal_timeout(&fsm), ==, KAJO_RAIL_ACT_NONE);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_COLLAPSED);

    /* Hover again + wait -> expands (pointer_on_rail set so synthetic trigger leave doesn't collapse!) */
    g_assert_cmpint(web_rail_fsm_trigger_enter(&fsm), ==, KAJO_RAIL_ACT_START_REVEAL_TIMER);
    g_assert_cmpint(web_rail_fsm_reveal_timeout(&fsm), ==, KAJO_RAIL_ACT_EXPAND);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_EXPANDED);

    /* Synthetic leave on trigger when rail overlays it -> MUST stay expanded! */
    g_assert_cmpint(web_rail_fsm_trigger_leave(&fsm), ==, KAJO_RAIL_ACT_NONE);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_EXPANDED);

    /* Explicit rail enter -> stays expanded */
    g_assert_cmpint(web_rail_fsm_rail_enter(&fsm), ==, KAJO_RAIL_ACT_NONE);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_EXPANDED);

    /* Hold open (e.g. context menu / drag) prevents hide timer on leave */
    web_rail_fsm_hold(&fsm);
    g_assert_cmpint(web_rail_fsm_rail_leave(&fsm), ==, KAJO_RAIL_ACT_NONE);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_EXPANDED);

    /* Release hold while pointer is away -> starts hide timer */
    g_assert_cmpint(web_rail_fsm_release(&fsm), ==, KAJO_RAIL_ACT_START_HIDE_TIMER);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_HIDE_WAIT);

    /* Re-enter during hide grace -> cancels hide timer */
    g_assert_cmpint(web_rail_fsm_rail_enter(&fsm), ==, KAJO_RAIL_ACT_CANCEL_HIDE_TIMER);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_EXPANDED);

    /* Leave and wait -> collapses */
    g_assert_cmpint(web_rail_fsm_rail_leave(&fsm), ==, KAJO_RAIL_ACT_START_HIDE_TIMER);
    g_assert_cmpint(web_rail_fsm_hide_timeout(&fsm), ==, KAJO_RAIL_ACT_COLLAPSE);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_COLLAPSED);

    /* Keyboard reveal + Escape / focus-loss immediate collapse */
    g_assert_cmpint(web_rail_fsm_keyboard_reveal(&fsm), ==, KAJO_RAIL_ACT_EXPAND);
    g_assert_cmpint(web_rail_fsm_escape(&fsm), ==, KAJO_RAIL_ACT_COLLAPSE);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_COLLAPSED);

    g_assert_cmpint(web_rail_fsm_keyboard_reveal(&fsm), ==, KAJO_RAIL_ACT_EXPAND);
    g_assert_cmpint(web_rail_fsm_focus_lost(&fsm), ==, KAJO_RAIL_ACT_COLLAPSE);
    g_assert_cmpint(fsm.state, ==, KAJO_RAIL_FSM_COLLAPSED);
}

/* 2. Tab, Group, Pin, MRU, Undo-Close & Suspension Eligibility Model */
static void
test_tab_model_and_suspension(void)
{
    g_autoptr(KajoWebModel) m = web_model_new();
    const gint64 t0 = 1000 * G_USEC_PER_SEC;

    KajoWebTabMeta *a = web_model_add_tab(m, "https://a.example", "A", NULL, FALSE, t0);
    KajoWebTabMeta *b = web_model_add_tab(m, "https://b.example", "B", "WORK", FALSE, t0 + 1000);
    KajoWebTabMeta *c = web_model_add_tab(m, "https://c.example", "C", NULL, TRUE, t0 + 2000);

    guint64 id_a = a->id, id_b = b->id, id_c = c->id;
    g_assert_cmpuint(m->tabs->len, ==, 3);

    /* Activate A -> B -> C; MRU order is [C, B, A] */
    g_assert_true(web_model_activate(m, id_a, t0 + 10));
    g_assert_true(web_model_activate(m, id_b, t0 + 20));
    g_assert_true(web_model_activate(m, id_c, t0 + 30));
    g_assert_cmpuint(m->active_id, ==, id_c);
    g_assert_cmpuint(web_model_cycle_mru(m, TRUE), ==, id_b);

    /* Suspension guards: active, pinned, audible, capture_active are NEVER suspended */
    const gint64 later = t0 + 40LL * 60LL * G_USEC_PER_SEC;
    g_assert_false(web_model_can_suspend(m, web_model_find_tab(m, id_c), 30, later)); /* active + pinned */
    g_assert_true(web_model_can_suspend(m, web_model_find_tab(m, id_a), 30, later));  /* idle 40m > 30m */
    g_assert_false(web_model_can_suspend(m, web_model_find_tab(m, id_a), 0, later));  /* disabled */

    web_model_find_tab(m, id_a)->audible = TRUE;
    g_assert_false(web_model_can_suspend(m, web_model_find_tab(m, id_a), 30, later));
    web_model_find_tab(m, id_a)->audible = FALSE;
    web_model_find_tab(m, id_a)->capture_active = TRUE;
    g_assert_false(web_model_can_suspend(m, web_model_find_tab(m, id_a), 30, later));

    /* Close B and reopen via Ctrl+Shift+T LIFO stack */
    g_assert_true(web_model_close_tab(m, id_b));
    g_assert_cmpuint(m->tabs->len, ==, 2);
    KajoWebTabMeta *reopened = web_model_reopen_closed(m, later);
    g_assert_nonnull(reopened);
    g_assert_cmpstr(reopened->uri, ==, "https://b.example");
    g_assert_cmpstr(reopened->group, ==, "WORK");
    g_assert_cmpuint(m->tabs->len, ==, 3);

    /* Group collapse state */
    g_assert_false(web_model_is_group_collapsed(m, "WORK"));
    web_model_set_group_collapsed(m, "WORK", TRUE);
    g_assert_true(web_model_is_group_collapsed(m, "WORK"));
}

/* 3. Omnibar URL vs Bang vs Search Resolution */
static void
test_omnibar_classifier(void)
{
    g_autofree gchar *u1 = web_model_resolve_input("kajo://gpu", KAJO_WEB_SEARCH_DUCKDUCKGO, NULL);
    g_assert_cmpstr(u1, ==, "kajo://gpu");

    g_autofree gchar *u2 = web_model_resolve_input("webkit.org/blog", KAJO_WEB_SEARCH_DUCKDUCKGO, NULL);
    g_assert_cmpstr(u2, ==, "https://webkit.org/blog");

    g_autofree gchar *u3 = web_model_resolve_input("localhost:8080/api", KAJO_WEB_SEARCH_DUCKDUCKGO, NULL);
    g_assert_cmpstr(u3, ==, "http://localhost:8080/api");

    g_autofree gchar *u4 = web_model_resolve_input("!g gtk4 webkit", KAJO_WEB_SEARCH_DUCKDUCKGO, NULL);
    g_assert_true(g_str_has_prefix(u4, "https://www.google.com/search?q=gtk4%20webkit"));

    g_autofree gchar *u5 = web_model_resolve_input("wayland dmabuf", KAJO_WEB_SEARCH_KAGI, NULL);
    g_assert_true(g_str_has_prefix(u5, "https://kagi.com/search?q=wayland%20dmabuf"));

    g_autofree gchar *u6 = web_model_resolve_input("hello world", KAJO_WEB_SEARCH_CUSTOM, "https://searx.local/search?q=%s");
    g_assert_cmpstr(u6, ==, "https://searx.local/search?q=hello%20world");

    g_autofree gchar *h1 = web_model_extract_host("https://docs.gtk.org/gtk4/index.html");
    g_assert_cmpstr(h1, ==, "docs.gtk.org");
    g_autofree gchar *h2 = web_model_extract_host("kajo://newtab");
    g_assert_cmpstr(h2, ==, "kajo://newtab");
}

/* 4. Profile Registry (profiles.toml) Persistence & Sanitization */
static void
test_profile_registry_roundtrip(void)
{
    g_autofree gchar *tmpdir = g_dir_make_tmp("kajo-web-prof-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(tmpdir, "profiles.toml", NULL);

    g_autoptr(KajoWebProfileRegistry) reg = web_profile_registry_new();
    g_assert_cmpuint(reg->profiles->len, ==, 1); /* default profile */

    KajoWebProfileInfo *work = web_profile_registry_add(reg, "Work Dev!", "💼", "#00b294");
    g_assert_nonnull(work);
    g_assert_cmpstr(work->id, ==, "work-dev");
    work->sync_enabled = TRUE;
    g_free(work->google_email);
    work->google_email = g_strdup("dev@example.com");
    g_free(reg->last_profile_id);
    reg->last_profile_id = g_strdup("work-dev");

    g_autoptr(GError) err = NULL;
    g_assert_true(web_profile_registry_save(reg, path, &err));
    g_assert_no_error(err);

    g_autoptr(KajoWebProfileRegistry) loaded = web_profile_registry_new();
    g_assert_true(web_profile_registry_load(loaded, path));
    g_assert_cmpuint(loaded->profiles->len, ==, 2);
    g_assert_cmpstr(loaded->last_profile_id, ==, "work-dev");

    KajoWebProfileInfo *lw = web_profile_registry_find(loaded, "work-dev");
    g_assert_nonnull(lw);
    g_assert_cmpstr(lw->name, ==, "WORK DEV!");
    g_assert_cmpstr(lw->accent_color, ==, "#00b294");
    g_assert_cmpstr(lw->google_email, ==, "dev@example.com");
    g_assert_true(lw->sync_enabled);

    /* Cannot remove default profile, can remove work-dev */
    g_assert_false(web_profile_registry_remove(loaded, "default"));
    g_assert_true(web_profile_registry_remove(loaded, "work-dev"));
    g_assert_cmpuint(loaded->profiles->len, ==, 1);

    g_unlink(path);
    g_rmdir(tmpdir);
}

/* 5. CRDT Bookmark Merge & AES-256-GCM E2EE Round-Trip */
static void
test_sync_crdt_and_e2ee(void)
{
    GPtrArray *local = g_ptr_array_new_with_free_func(web_bookmark_free);
    GPtrArray *remote = g_ptr_array_new_with_free_func(web_bookmark_free);

    /* Same UUID: remote has newer title */
    g_ptr_array_add(local, web_bookmark_new("u1", "Old Title", "https://a.org", "DEV", 1000, FALSE));
    g_ptr_array_add(remote, web_bookmark_new("u1", "New Title", "https://a.org", "DEV", 2000, FALSE));

    /* Tombstone conflict: local deleted later than remote edit -> stays deleted */
    g_ptr_array_add(local, web_bookmark_new("u2", "Gone", "https://b.org", "", 3000, TRUE));
    g_ptr_array_add(remote, web_bookmark_new("u2", "Edited", "https://b.org", "", 2500, FALSE));

    /* Remote-only bookmark */
    g_ptr_array_add(remote, web_bookmark_new("u3", "Remote Only", "https://c.org", "NEWS", 1500, FALSE));

    GPtrArray *merged = web_sync_merge_bookmarks(local, remote);
    g_assert_cmpuint(merged->len, ==, 3);

    gboolean saw_u1 = FALSE, saw_u2 = FALSE, saw_u3 = FALSE;
    for (guint i = 0; i < merged->len; i++) {
        KajoWebBookmark *b = g_ptr_array_index(merged, i);
        if (g_strcmp0(b->uuid, "u1") == 0) {
            g_assert_cmpstr(b->title, ==, "New Title");
            g_assert_false(b->deleted);
            saw_u1 = TRUE;
        } else if (g_strcmp0(b->uuid, "u2") == 0) {
            g_assert_true(b->deleted);
            saw_u2 = TRUE;
        } else if (g_strcmp0(b->uuid, "u3") == 0) {
            g_assert_cmpstr(b->uri, ==, "https://c.org");
            saw_u3 = TRUE;
        }
    }
    g_assert_true(saw_u1 && saw_u2 && saw_u3);

    /* JSON round-trip */
    g_autofree gchar *json = web_sync_bookmarks_to_json(merged);
    g_autoptr(GError) err = NULL;
    GPtrArray *parsed = web_sync_bookmarks_from_json(json, &err);
    g_assert_no_error(err);
    g_assert_nonnull(parsed);
    g_assert_cmpuint(parsed->len, ==, 3);

    /* E2EE AES-256-GCM encrypt/decrypt + wrong passphrase rejection */
    g_autofree gchar *env = web_sync_encrypt_payload(json, "hunter2-kajo-secret", &err);
    g_assert_no_error(err);
    g_assert_nonnull(env);
    g_assert_null(strstr(env, "New Title"));

    g_autofree gchar *dec = web_sync_decrypt_payload(env, "hunter2-kajo-secret", &err);
    g_assert_no_error(err);
    g_assert_cmpstr(dec, ==, json);

    g_autoptr(GError) bad_err = NULL;
    g_autofree gchar *bad = web_sync_decrypt_payload(env, "wrong-passphrase", &bad_err);
    g_assert_null(bad);
    g_assert_nonnull(bad_err);

    g_ptr_array_unref(parsed);
    g_ptr_array_unref(merged);
    g_ptr_array_unref(remote);
    g_ptr_array_unref(local);
}

/* 6. VDH-Style Media Stream Sniffer (HLS / DASH / Progressive + Deduplication) */
static void
test_media_sniffer_core(void)
{
    g_assert_cmpint(web_media_classify_resource("https://cdn.example/live/master.m3u8?tok=1", NULL, 0),
                    ==, KAJO_MEDIA_HLS);
    g_assert_cmpint(web_media_classify_resource("https://cdn.example/vod/stream.mpd", NULL, 0),
                    ==, KAJO_MEDIA_DASH);
    g_assert_cmpint(web_media_classify_resource("https://cdn.example/clip.mp4", "video/mp4", 1024 * 1024),
                    ==, KAJO_MEDIA_PROGRESSIVE);
    /* Tiny video (<512KB) ignored to avoid UI animation spam */
    g_assert_cmpint(web_media_classify_resource("https://cdn.example/spinner.mp4", "video/mp4", 64 * 1024),
                    ==, KAJO_MEDIA_NONE);
    g_assert_cmpint(web_media_classify_resource("https://cdn.example/style.css", "text/css", 999999),
                    ==, KAJO_MEDIA_NONE);

    /* HLS master playlist parsing */
    const gchar *hls =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=4800000,RESOLUTION=1920x1080\n"
        "variants/1080p.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=1280x720\n"
        "https://cdn.example/abs/720p.m3u8\n";
    GPtrArray *hv = web_media_parse_hls_master(hls, "https://cdn.example/live/master.m3u8");
    g_assert_cmpuint(hv->len, ==, 2);
    KajoWebMediaStream *s0 = g_ptr_array_index(hv, 0);
    g_assert_cmpint(s0->width, ==, 1920);
    g_assert_cmpint(s0->height, ==, 1080);
    g_assert_cmpstr(s0->uri, ==, "https://cdn.example/live/variants/1080p.m3u8");
    KajoWebMediaStream *s1 = g_ptr_array_index(hv, 1);
    g_assert_cmpint(s1->height, ==, 720);
    g_assert_cmpstr(s1->uri, ==, "https://cdn.example/abs/720p.m3u8");

    /* DASH MPD parsing */
    const gchar *mpd =
        "<MPD><Period><AdaptationSet mimeType=\"video/mp4\">"
        "<Representation id=\"1\" width=\"1920\" height=\"1080\" bandwidth=\"5200000\"/>"
        "<Representation id=\"2\" width=\"854\" height=\"480\" bandwidth=\"1200000\"/>"
        "</AdaptationSet></Period></MPD>";
    GPtrArray *dv = web_media_parse_dash_mpd(mpd, "https://cdn.example/vod/stream.mpd");
    g_assert_cmpuint(dv->len, ==, 2);
    KajoWebMediaStream *d0 = g_ptr_array_index(dv, 0);
    g_assert_cmpint(d0->kind, ==, KAJO_MEDIA_DASH);
    g_assert_cmpint(d0->height, ==, 1080);

    /* Deduplication */
    GPtrArray *dedup = g_ptr_array_new_with_free_func(web_media_stream_free);
    g_assert_true(web_media_list_add_unique(dedup,
        web_media_stream_new(KAJO_MEDIA_HLS, "https://x/a.m3u8", "A", NULL, 0, 0, 0, 0)));
    g_assert_false(web_media_list_add_unique(dedup,
        web_media_stream_new(KAJO_MEDIA_HLS, "https://x/a.m3u8", "A dup", NULL, 0, 0, 0, 0)));
    g_assert_cmpuint(dedup->len, ==, 1);

    g_ptr_array_unref(dedup);
    g_ptr_array_unref(dv);
    g_ptr_array_unref(hv);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/web/rail-fsm", test_rail_fsm_transitions);
    g_test_add_func("/web/tab-model-and-suspension", test_tab_model_and_suspension);
    g_test_add_func("/web/omnibar-classifier", test_omnibar_classifier);
    g_test_add_func("/web/profile-registry-roundtrip", test_profile_registry_roundtrip);
    g_test_add_func("/web/sync-crdt-and-e2ee", test_sync_crdt_and_e2ee);
    g_test_add_func("/web/media-sniffer-core", test_media_sniffer_core);
    return g_test_run();
}
