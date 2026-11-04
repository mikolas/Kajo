#include <glib.h>
#include <gio/gio.h>
#include "../src/player/player_queue.h"

static void
test_format_time(void)
{
    g_autofree char *t0 = kajo_player_format_time(0);
    g_assert_cmpstr(t0, ==, "00:00");

    g_autofree char *tneg = kajo_player_format_time(-5000000);
    g_assert_cmpstr(tneg, ==, "00:00");

    g_autofree char *t9 = kajo_player_format_time(9 * G_USEC_PER_SEC);
    g_assert_cmpstr(t9, ==, "00:09");

    g_autofree char *t252 = kajo_player_format_time(252 * G_USEC_PER_SEC);
    g_assert_cmpstr(t252, ==, "04:12");

    g_autofree char *t6138 = kajo_player_format_time(6138LL * G_USEC_PER_SEC);
    g_assert_cmpstr(t6138, ==, "01:42:18");
}

static void
test_format_size(void)
{
    g_autofree char *s0 = kajo_player_format_size(512);
    g_assert_cmpstr(s0, ==, "512 B");

    g_autofree char *s_kb = kajo_player_format_size(2048);
    g_assert_cmpstr(s_kb, ==, "2.0 KB");

    g_autofree char *s_mb = kajo_player_format_size(10 * 1024 * 1024);
    g_assert_cmpstr(s_mb, ==, "10.0 MB");

    g_autofree char *s_gb = kajo_player_format_size(2ULL * 1024 * 1024 * 1024);
    g_assert_cmpstr(s_gb, ==, "2.00 GB");
}

static void
test_supported_media_detection(void)
{
    g_assert_true(kajo_player_is_supported_media("video/x-matroska", "movie.mkv"));
    g_assert_true(kajo_player_is_supported_media("video/mp4", "clip.mp4"));
    g_assert_true(kajo_player_is_supported_media("audio/flac", "track.flac"));
    g_assert_true(kajo_player_is_supported_media(NULL, "demo.webm"));
    g_assert_true(kajo_player_is_supported_media("application/ogg", "song.ogg"));

    g_assert_false(kajo_player_is_supported_media("text/plain", "notes.txt"));
    g_assert_false(kajo_player_is_supported_media("image/png", "photo.png"));

    g_assert_false(kajo_player_is_audio_only("video/mp4", "clip.mp4"));
    g_assert_false(kajo_player_is_audio_only(NULL, "movie.mkv"));
    g_assert_true(kajo_player_is_audio_only("audio/mpeg", "song.mp3"));
    g_assert_true(kajo_player_is_audio_only(NULL, "album.flac"));
}

static void
test_media_track_lifecycle(void)
{
    g_autoptr(GFile) file = g_file_new_for_path("/tmp/sample_video.mkv");
    KajoMediaTrack *track = kajo_media_track_new(file, "sample_video.mkv", "video/x-matroska", 1048576);
    g_assert_nonnull(track);
    g_assert_cmpstr(track->display_name, ==, "sample_video.mkv");
    g_assert_cmpstr(track->content_type, ==, "video/x-matroska");
    g_assert_cmpuint(track->size, ==, 1048576);
    g_assert_false(track->is_audio_only);
    kajo_media_track_free(track);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/player/format_time", test_format_time);
    g_test_add_func("/player/format_size", test_format_size);
    g_test_add_func("/player/media_detection", test_supported_media_detection);
    g_test_add_func("/player/track_lifecycle", test_media_track_lifecycle);

    return g_test_run();
}
