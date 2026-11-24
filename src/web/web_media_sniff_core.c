#include "web_media_sniff_core.h"
#include <string.h>
#include <stdlib.h>

#define KAJO_MEDIA_MIN_PROGRESSIVE_BYTES (512 * 1024)

KajoWebMediaStream *
web_media_stream_new(KajoWebMediaKind kind,
                     const gchar     *uri,
                     const gchar     *label,
                     const gchar     *mime_type,
                     gint             width,
                     gint             height,
                     gint64           bandwidth,
                     gint64           size_bytes)
{
    KajoWebMediaStream *s = g_new0(KajoWebMediaStream, 1);
    s->kind = kind;
    s->uri = g_strdup(uri ? uri : "");
    s->label = g_strdup(label ? label : "");
    s->mime_type = g_strdup(mime_type ? mime_type : "");
    s->width = width;
    s->height = height;
    s->bandwidth = bandwidth;
    s->size_bytes = size_bytes;
    return s;
}

void
web_media_stream_free(gpointer ptr)
{
    KajoWebMediaStream *s = ptr;
    if (!s)
        return;
    g_free(s->uri);
    g_free(s->label);
    g_free(s->mime_type);
    g_free(s);
}

static gboolean
uri_path_has_ext(const gchar *uri, const gchar *ext)
{
    if (!uri || !ext)
        return FALSE;
    const char *q = strpbrk(uri, "?#");
    gsize len = q ? (gsize)(q - uri) : strlen(uri);
    gsize ext_len = strlen(ext);
    if (len < ext_len)
        return FALSE;
    return g_ascii_strncasecmp(uri + len - ext_len, ext, ext_len) == 0;
}

KajoWebMediaKind
web_media_classify_resource(const gchar *uri,
                            const gchar *mime_type,
                            gint64       content_length)
{
    if (!uri || (!g_str_has_prefix(uri, "http://") && !g_str_has_prefix(uri, "https://")))
        return KAJO_MEDIA_NONE;

    if ((mime_type && (g_ascii_strcasecmp(mime_type, "application/vnd.apple.mpegurl") == 0 ||
                       g_ascii_strcasecmp(mime_type, "application/x-mpegurl") == 0 ||
                       g_ascii_strcasecmp(mime_type, "audio/mpegurl") == 0)) ||
        uri_path_has_ext(uri, ".m3u8")) {
        return KAJO_MEDIA_HLS;
    }

    if ((mime_type && g_ascii_strcasecmp(mime_type, "application/dash+xml") == 0) ||
        uri_path_has_ext(uri, ".mpd")) {
        return KAJO_MEDIA_DASH;
    }

    gboolean is_video_mime = mime_type && g_str_has_prefix(mime_type, "video/") &&
                             g_ascii_strcasecmp(mime_type, "video/mp2t") != 0;
    gboolean is_video_ext = uri_path_has_ext(uri, ".mp4") || uri_path_has_ext(uri, ".webm");
    if ((is_video_mime || is_video_ext) &&
        (content_length <= 0 || content_length >= KAJO_MEDIA_MIN_PROGRESSIVE_BYTES)) {
        return KAJO_MEDIA_PROGRESSIVE;
    }

    return KAJO_MEDIA_NONE;
}

static gchar *
resolve_relative_uri(const gchar *base_uri, const gchar *rel)
{
    if (!rel || !*rel)
        return g_strdup("");
    if (g_str_has_prefix(rel, "http://") || g_str_has_prefix(rel, "https://"))
        return g_strdup(rel);
    if (!base_uri || !*base_uri)
        return g_strdup(rel);
    g_autoptr(GUri) base = g_uri_parse(base_uri, G_URI_FLAGS_NONE, NULL);
    if (!base)
        return g_strdup(rel);
    g_autoptr(GUri) resolved = g_uri_parse_relative(base, rel, G_URI_FLAGS_NONE, NULL);
    return resolved ? g_uri_to_string(resolved) : g_strdup(rel);
}

GPtrArray *
web_media_parse_hls_master(const gchar *manifest_text, const gchar *base_uri)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(web_media_stream_free);
    if (!manifest_text || !*manifest_text)
        return out;

    g_auto(GStrv) lines = g_strsplit(manifest_text, "\n", -1);
    gboolean expect_uri = FALSE;
    gint w = 0, h = 0;
    gint64 bw = 0;

    for (gsize i = 0; lines && lines[i]; i++) {
        g_autofree gchar *line = g_strstrip(g_strdup(lines[i]));
        if (!*line)
            continue;

        if (g_str_has_prefix(line, "#EXT-X-STREAM-INF:")) {
            expect_uri = TRUE;
            w = 0; h = 0; bw = 0;
            const char *bp = strstr(line, "BANDWIDTH=");
            if (bp)
                bw = g_ascii_strtoll(bp + 10, NULL, 10);
            const char *rp = strstr(line, "RESOLUTION=");
            if (rp) {
                char *endp = NULL;
                w = (gint)strtol(rp + 11, &endp, 10);
                if (endp && (*endp == 'x' || *endp == 'X'))
                    h = (gint)strtol(endp + 1, NULL, 10);
            }
            continue;
        }

        if (expect_uri && line[0] != '#') {
            expect_uri = FALSE;
            g_autofree gchar *full = resolve_relative_uri(base_uri, line);
            g_autofree gchar *lbl = NULL;
            if (h > 0 && bw > 0)
                lbl = g_strdup_printf("%dp HLS (%.1f Mbps)", h, (double)bw / 1000000.0);
            else if (h > 0)
                lbl = g_strdup_printf("%dp HLS", h);
            else if (bw > 0)
                lbl = g_strdup_printf("HLS (%.1f Mbps)", (double)bw / 1000000.0);
            else
                lbl = g_strdup("HLS Stream");

            web_media_list_add_unique(out, web_media_stream_new(
                KAJO_MEDIA_HLS, full, lbl, "application/vnd.apple.mpegurl",
                w, h, bw, 0));
        }
    }
    return out;
}

static gboolean
extract_xml_int_attr(const char *tag, const char *attr, gint64 *out_val)
{
    const char *p = strstr(tag, attr);
    if (!p)
        return FALSE;
    p += strlen(attr);
    if (*p != '=' || (p[1] != '"' && p[1] != '\''))
        return FALSE;
    *out_val = g_ascii_strtoll(p + 2, NULL, 10);
    return TRUE;
}

GPtrArray *
web_media_parse_dash_mpd(const gchar *mpd_text, const gchar *manifest_uri)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(web_media_stream_free);
    if (!mpd_text || !*mpd_text)
        return out;

    const char *cur = mpd_text;
    while ((cur = strstr(cur, "<Representation")) != NULL) {
        const char *end = strchr(cur, '>');
        if (!end)
            break;
        g_autofree gchar *tag = g_strndup(cur, (gsize)(end - cur + 1));
        gint64 w = 0, h = 0, bw = 0;
        extract_xml_int_attr(tag, "width", &w);
        extract_xml_int_attr(tag, "height", &h);
        extract_xml_int_attr(tag, "bandwidth", &bw);

        if (h > 0 || bw > 0) {
            g_autofree gchar *lbl = (h > 0)
                ? g_strdup_printf("%dp DASH (%.1f Mbps)", (int)h, (double)bw / 1000000.0)
                : g_strdup_printf("DASH Audio/Stream (%.0f kbps)", (double)bw / 1000.0);
            g_autofree gchar *key_uri = g_strdup_printf("%s#h=%d&bw=%" G_GINT64_FORMAT,
                                                        manifest_uri ? manifest_uri : "",
                                                        (int)h, bw);
            web_media_list_add_unique(out, web_media_stream_new(
                KAJO_MEDIA_DASH, key_uri, lbl, "application/dash+xml",
                (gint)w, (gint)h, bw, 0));
        }
        cur = end + 1;
    }
    return out;
}

#define KAJO_WEB_MAX_MEDIA_STREAMS 32

/* Strip transient byte-range parameters (e.g. ?range=0-1024 / &range=...) so
 * repeated DASH segment requests for the same stream collapse to one entry. */
static gchar *
media_uri_dedup_key(const gchar *uri)
{
    if (!uri)
        return g_strdup("");
    const gchar *q = strchr(uri, '?');
    if (!q)
        return g_strdup(uri);

    GString *out = g_string_new_len(uri, (gssize)(q - uri));
    g_auto(GStrv) parts = g_strsplit(q + 1, "&", -1);
    gboolean first = TRUE;
    for (int i = 0; parts && parts[i]; i++) {
        if (g_str_has_prefix(parts[i], "range="))
            continue;
        g_string_append_c(out, first ? '?' : '&');
        g_string_append(out, parts[i]);
        first = FALSE;
    }
    return g_string_free(out, FALSE);
}

gboolean
web_media_list_add_unique(GPtrArray *list, KajoWebMediaStream *stream)
{
    if (!list || !stream)
        return FALSE;
    if (list->len >= KAJO_WEB_MAX_MEDIA_STREAMS) {
        web_media_stream_free(stream);
        return FALSE;
    }
    g_autofree gchar *key = media_uri_dedup_key(stream->uri);
    for (guint i = 0; i < list->len; i++) {
        const KajoWebMediaStream *existing = g_ptr_array_index(list, i);
        g_autofree gchar *ekey = media_uri_dedup_key(existing->uri);
        if (g_strcmp0(ekey, key) == 0) {
            web_media_stream_free(stream);
            return FALSE;
        }
    }
    g_ptr_array_add(list, stream);
    return TRUE;
}
