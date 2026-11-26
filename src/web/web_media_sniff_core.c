#include "web_media_sniff_core.h"

#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

#define KAJO_MEDIA_MIN_PROGRESSIVE_BYTES (512 * 1024)
#define KAJO_MEDIA_MIN_PROGRESSIVE_WITH_MANIFEST_BYTES (20 * 1024 * 1024)
#define KAJO_WEB_MAX_MEDIA_STREAMS 64

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
    g_free(s->audio_uri);
    g_free(s->subtitle_uri);
    g_free(s->audio_lang);
    g_free(s->codecs);
    g_free(s->group_id);
    g_free(s->rep_id);
    g_free(s);
}

static gchar *
mime_base_lower(const gchar *mime_type)
{
    if (!mime_type || !*mime_type)
        return g_strdup("");
    const char *semi = strchr(mime_type, ';');
    g_autofree gchar *raw = semi ? g_strndup(mime_type, (gsize)(semi - mime_type))
                                 : g_strdup(mime_type);
    g_strstrip(raw);
    gchar *low = g_ascii_strdown(raw, -1);
    return low;
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

static gboolean
is_segment_like_uri(const gchar *uri)
{
    if (!uri)
        return FALSE;
    if (uri_path_has_ext(uri, ".m4s") ||
        uri_path_has_ext(uri, ".ts") ||
        uri_path_has_ext(uri, ".m4f") ||
        uri_path_has_ext(uri, ".m2ts") ||
        uri_path_has_ext(uri, ".cmfv") ||
        uri_path_has_ext(uri, ".cmfa")) {
        return TRUE;
    }

    const char *q = strpbrk(uri, "?#");
    gsize path_len = q ? (gsize)(q - uri) : strlen(uri);
    g_autofree gchar *path = g_ascii_strdown(uri, (gssize)path_len);
    if (g_str_has_suffix(path, "/init.mp4") ||
        g_str_has_suffix(path, "/init.m4v") ||
        g_str_has_suffix(path, "/init.m4a")) {
        return TRUE;
    }
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    if (g_str_has_suffix(base, ".mp4") &&
        (g_str_has_prefix(base, "seg-") ||
         g_str_has_prefix(base, "seg_") ||
         g_str_has_prefix(base, "chunk-") ||
         g_str_has_prefix(base, "chunk_") ||
         g_str_has_prefix(base, "fragment-") ||
         g_str_has_prefix(base, "frag-"))) {
        return TRUE;
    }

    if (q && *q == '?') {
        g_autofree gchar *qlow = g_ascii_strdown(q, -1);
        if (strstr(qlow, "?range=") || strstr(qlow, "&range=") ||
            strstr(qlow, "?sq=") || strstr(qlow, "&sq=")) {
            return TRUE;
        }
    }
    return FALSE;
}

gint64
web_media_parse_content_range_total(const gchar *content_range)
{
    if (!content_range || !*content_range)
        return 0;
    const char *slash = strchr(content_range, '/');
    if (!slash || !slash[1])
        return 0;
    g_autofree gchar *tail = g_strstrip(g_strdup(slash + 1));
    if (!*tail || strcmp(tail, "*") == 0)
        return 0;
    char *endp = NULL;
    gint64 val = g_ascii_strtoll(tail, &endp, 10);
    if (!endp || endp == tail || val <= 0)
        return 0;
    return val;
}

gchar *
web_media_parse_content_disposition_filename(const gchar *header)
{
    if (!header || !*header)
        return NULL;

    g_autofree gchar *low = g_ascii_strdown(header, -1);
    g_autofree gchar *raw = NULL;

    /* Prefer RFC 5987 filename*=charset'lang'value */
    const char *star = strstr(low, "filename*");
    if (star) {
        const char *orig_star = header + (star - low);
        const char *eq = strchr(orig_star, '=');
        if (eq) {
            eq++;
            while (*eq == ' ' || *eq == '\t')
                eq++;
            gboolean quoted = (*eq == '"' || *eq == '\'');
            char qch = quoted ? *eq++ : '\0';
            const char *end = eq;
            while (*end && (quoted ? (*end != qch) : (*end != ';')))
                end++;
            g_autofree gchar *val = g_strndup(eq, (gsize)(end - eq));
            g_strstrip(val);
            const char *q1 = strchr(val, '\'');
            const char *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;
            const char *enc = q2 ? (q2 + 1) : val;
            gchar *unesc = g_uri_unescape_string(enc, NULL);
            raw = unesc ? unesc : g_strdup(enc);
        }
    }

    if (!raw || !*raw) {
        g_clear_pointer(&raw, g_free);
        const char *fn = strstr(low, "filename=");
        if (fn) {
            const char *p = header + (fn - low) + 9;
            while (*p == ' ' || *p == '\t')
                p++;
            gboolean quoted = (*p == '"' || *p == '\'');
            char qch = quoted ? *p++ : '\0';
            const char *end = p;
            while (*end && (quoted ? (*end != qch) : (*end != ';')))
                end++;
            raw = g_strndup(p, (gsize)(end - p));
            g_strstrip(raw);
        }
    }

    if (!raw || !*raw)
        return NULL;
    gchar *base = g_path_get_basename(raw);
    if (!base || strcmp(base, ".") == 0 || strcmp(base, "..") == 0 || strcmp(base, "/") == 0) {
        g_free(base);
        return NULL;
    }
    return base;
}

gdouble
web_media_parse_iso8601_duration(const gchar *iso)
{
    if (!iso || iso[0] != 'P')
        return 0.0;
    const char *p = iso + 1;
    gdouble total = 0.0;
    gboolean in_time = FALSE;
    while (*p) {
        if (*p == 'T') {
            in_time = TRUE;
            p++;
            continue;
        }
        char *endp = NULL;
        double v = g_ascii_strtod(p, &endp);
        if (!endp || endp == p || !*endp)
            break;
        char unit = (char)g_ascii_toupper(*endp);
        if (!in_time && unit == 'D')
            total += v * 86400.0;
        else if (in_time && unit == 'H')
            total += v * 3600.0;
        else if (in_time && unit == 'M')
            total += v * 60.0;
        else if (in_time && unit == 'S')
            total += v;
        p = endp + 1;
    }
    return total > 0.0 ? total : 0.0;
}

static gboolean
is_hls_resource(const gchar *uri, const gchar *mime_low)
{
    if (mime_low && (strcmp(mime_low, "application/vnd.apple.mpegurl") == 0 ||
                     strcmp(mime_low, "application/x-mpegurl") == 0 ||
                     strcmp(mime_low, "audio/mpegurl") == 0 ||
                     strstr(mime_low, "mpegurl") != NULL)) {
        return TRUE;
    }
    if (uri_path_has_ext(uri, ".m3u8"))
        return TRUE;
    const char *q = strpbrk(uri, "?#");
    gsize plen = q ? (gsize)(q - uri) : strlen(uri);
    g_autofree gchar *path = g_strndup(uri, plen);
    return strstr(path, "/api/playlist/master/") != NULL;
}

static gboolean
is_dash_resource(const gchar *uri, const gchar *mime_low)
{
    if (mime_low && strcmp(mime_low, "application/dash+xml") == 0)
        return TRUE;
    return uri_path_has_ext(uri, ".mpd");
}

static gboolean
is_vimeo_config_url(const gchar *uri)
{
    if (!uri)
        return FALSE;
    g_autoptr(GUri) u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    if (!u)
        return FALSE;
    const char *host = g_uri_get_host(u);
    const char *path = g_uri_get_path(u);
    return host && path &&
           g_ascii_strcasecmp(host, "player.vimeo.com") == 0 &&
           g_str_has_suffix(path, "/config");
}

gboolean
web_media_should_capture_manifest(const gchar *uri, const gchar *mime_type)
{
    if (!uri || (!g_str_has_prefix(uri, "http://") && !g_str_has_prefix(uri, "https://")))
        return FALSE;
    g_autofree gchar *mlow = mime_base_lower(mime_type);
    return is_hls_resource(uri, mlow) ||
           is_dash_resource(uri, mlow) ||
           is_vimeo_config_url(uri);
}

KajoWebMediaKind
web_media_classify_resource_ex(const gchar *uri,
                               const gchar *mime_type,
                               gint64       content_length,
                               const gchar *content_range,
                               gboolean     page_has_manifest)
{
    if (!uri || (!g_str_has_prefix(uri, "http://") && !g_str_has_prefix(uri, "https://")))
        return KAJO_MEDIA_NONE;

    g_autofree gchar *mlow = mime_base_lower(mime_type);

    if (is_hls_resource(uri, mlow))
        return KAJO_MEDIA_HLS;
    if (is_dash_resource(uri, mlow))
        return KAJO_MEDIA_DASH;

    if (is_segment_like_uri(uri))
        return KAJO_MEDIA_NONE;

    gboolean is_video_mime = (*mlow != '\0') &&
                             g_str_has_prefix(mlow, "video/") &&
                             strcmp(mlow, "video/mp2t") != 0;
    gboolean is_audio_mime = (*mlow != '\0') &&
                             (strcmp(mlow, "audio/mpeg") == 0 ||
                              strcmp(mlow, "audio/mp4") == 0 ||
                              strcmp(mlow, "audio/aac") == 0 ||
                              strcmp(mlow, "audio/webm") == 0 ||
                              strcmp(mlow, "audio/ogg") == 0);
    gboolean is_media_ext = uri_path_has_ext(uri, ".mp4") ||
                            uri_path_has_ext(uri, ".webm") ||
                            uri_path_has_ext(uri, ".mkv") ||
                            uri_path_has_ext(uri, ".mov") ||
                            uri_path_has_ext(uri, ".m4v") ||
                            uri_path_has_ext(uri, ".flv") ||
                            uri_path_has_ext(uri, ".m4a") ||
                            uri_path_has_ext(uri, ".mp3");

    if (!is_video_mime && !is_audio_mime && !is_media_ext)
        return KAJO_MEDIA_NONE;

    gint64 total = web_media_parse_content_range_total(content_range);
    if (total <= 0)
        total = content_length;

    const gint64 min_bytes = page_has_manifest
                                 ? KAJO_MEDIA_MIN_PROGRESSIVE_WITH_MANIFEST_BYTES
                                 : KAJO_MEDIA_MIN_PROGRESSIVE_BYTES;
    if (page_has_manifest) {
        return (total >= min_bytes) ? KAJO_MEDIA_PROGRESSIVE : KAJO_MEDIA_NONE;
    }
    return (total <= 0 || total >= min_bytes) ? KAJO_MEDIA_PROGRESSIVE : KAJO_MEDIA_NONE;
}

KajoWebMediaKind
web_media_classify_resource(const gchar *uri,
                            const gchar *mime_type,
                            gint64       content_length)
{
    return web_media_classify_resource_ex(uri, mime_type, content_length, NULL, FALSE);
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

/* Extract attribute value from HLS tag attribute list (handles quoted and unquoted). */
static gchar *
hls_attr_dup(const char *attrs, const char *key)
{
    if (!attrs || !key)
        return NULL;
    size_t klen = strlen(key);
    const char *p = attrs;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p)
            break;
        const char *eq = strchr(p, '=');
        if (!eq)
            break;
        gboolean match = ((size_t)(eq - p) == klen && g_ascii_strncasecmp(p, key, klen) == 0);
        const char *vstart = eq + 1;
        const char *vend = vstart;
        gboolean quoted = (*vstart == '"');
        if (quoted) {
            vstart++;
            vend = strchr(vstart, '"');
            if (!vend)
                vend = vstart + strlen(vstart);
        } else {
            while (*vend && *vend != ',')
                vend++;
        }
        if (match)
            return g_strndup(vstart, (gsize)(vend - vstart));
        p = (*vend == '"') ? (vend + 1) : vend;
    }
    return NULL;
}

typedef struct {
    gchar   *type;
    gchar   *group_id;
    gchar   *language;
    gchar   *name;
    gchar   *uri;
    gboolean is_default;
} HlsMediaGroup;

static void
hls_media_group_free(gpointer p)
{
    HlsMediaGroup *m = p;
    if (!m)
        return;
    g_free(m->type);
    g_free(m->group_id);
    g_free(m->language);
    g_free(m->name);
    g_free(m->uri);
    g_free(m);
}

static const HlsMediaGroup *
find_hls_group(GPtrArray *groups, const char *type, const char *group_id)
{
    if (!groups || !type || !group_id)
        return NULL;
    const HlsMediaGroup *fallback = NULL;
    for (guint i = 0; i < groups->len; i++) {
        const HlsMediaGroup *g = g_ptr_array_index(groups, i);
        if (g_strcmp0(g->type, type) == 0 && g_strcmp0(g->group_id, group_id) == 0 && g->uri && *g->uri) {
            if (g->is_default)
                return g;
            if (!fallback)
                fallback = g;
        }
    }
    return fallback;
}

static gint
cmp_media_variant_desc(gconstpointer a, gconstpointer b)
{
    const KajoWebMediaStream *sa = *(const KajoWebMediaStream *const *)a;
    const KajoWebMediaStream *sb = *(const KajoWebMediaStream *const *)b;
    if (sa->is_audio_only != sb->is_audio_only)
        return sa->is_audio_only ? 1 : -1;
    if (sa->height != sb->height)
        return (sb->height > sa->height) ? 1 : -1;
    if (sa->bandwidth != sb->bandwidth)
        return (sb->bandwidth > sa->bandwidth) ? 1 : -1;
    return 0;
}

static gboolean
line_indicates_hls_drm(const char *line)
{
    if (!g_str_has_prefix(line, "#EXT-X-KEY:") &&
        !g_str_has_prefix(line, "#EXT-X-SESSION-KEY:")) {
        return FALSE;
    }
    g_autofree gchar *method = hls_attr_dup(strchr(line, ':') + 1, "METHOD");
    g_autofree gchar *keyfmt = hls_attr_dup(strchr(line, ':') + 1, "KEYFORMAT");
    if (method && (g_ascii_strcasecmp(method, "SAMPLE-AES") == 0 ||
                   g_ascii_strcasecmp(method, "SAMPLE-AES-CTR") == 0 ||
                   g_ascii_strcasecmp(method, "SAMPLE-AES-CENC") == 0)) {
        return TRUE;
    }
    if (keyfmt && *keyfmt && g_ascii_strcasecmp(keyfmt, "identity") != 0)
        return TRUE;
    return FALSE;
}

GPtrArray *
web_media_parse_hls_master(const gchar *manifest_text, const gchar *base_uri)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(web_media_stream_free);
    if (!manifest_text || !*manifest_text)
        return out;

    g_autoptr(GPtrArray) media_groups = g_ptr_array_new_with_free_func(hls_media_group_free);
    g_auto(GStrv) lines = g_strsplit(manifest_text, "\n", -1);
    gboolean has_drm = FALSE;
    gdouble extinf_total_sec = 0.0;

    for (gsize i = 0; lines && lines[i]; i++) {
        g_autofree gchar *line = g_strstrip(g_strdup(lines[i]));
        if (!*line)
            continue;
        if (line_indicates_hls_drm(line))
            has_drm = TRUE;
        if (g_str_has_prefix(line, "#EXTINF:")) {
            double sec = g_ascii_strtod(line + 8, NULL);
            if (sec > 0.0)
                extinf_total_sec += sec;
        }
        if (g_str_has_prefix(line, "#EXT-X-MEDIA:")) {
            const char *attrs = line + strlen("#EXT-X-MEDIA:");
            g_autofree gchar *uri_rel = hls_attr_dup(attrs, "URI");
            if (!uri_rel || !*uri_rel)
                continue;
            HlsMediaGroup *m = g_new0(HlsMediaGroup, 1);
            m->type = hls_attr_dup(attrs, "TYPE");
            m->group_id = hls_attr_dup(attrs, "GROUP-ID");
            m->language = hls_attr_dup(attrs, "LANGUAGE");
            m->name = hls_attr_dup(attrs, "NAME");
            m->uri = resolve_relative_uri(base_uri, uri_rel);
            g_autofree gchar *def = hls_attr_dup(attrs, "DEFAULT");
            m->is_default = (def && g_ascii_strcasecmp(def, "YES") == 0);
            g_ptr_array_add(media_groups, m);
        }
    }

    gboolean expect_uri = FALSE;
    gint w = 0, h = 0;
    gint64 bw = 0;
    g_autofree gchar *codecs = NULL;
    g_autofree gchar *audio_grp = NULL;
    g_autofree gchar *sub_grp = NULL;

    for (gsize i = 0; lines && lines[i]; i++) {
        g_autofree gchar *line = g_strstrip(g_strdup(lines[i]));
        if (!*line)
            continue;

        if (g_str_has_prefix(line, "#EXT-X-STREAM-INF:")) {
            expect_uri = TRUE;
            w = 0;
            h = 0;
            bw = 0;
            g_clear_pointer(&codecs, g_free);
            g_clear_pointer(&audio_grp, g_free);
            g_clear_pointer(&sub_grp, g_free);

            const char *attrs = line + strlen("#EXT-X-STREAM-INF:");
            g_autofree gchar *bw_s = hls_attr_dup(attrs, "BANDWIDTH");
            if (!bw_s)
                bw_s = hls_attr_dup(attrs, "AVERAGE-BANDWIDTH");
            if (bw_s)
                bw = g_ascii_strtoll(bw_s, NULL, 10);

            g_autofree gchar *res_s = hls_attr_dup(attrs, "RESOLUTION");
            if (res_s) {
                char *endp = NULL;
                w = (gint)strtol(res_s, &endp, 10);
                if (endp && (*endp == 'x' || *endp == 'X'))
                    h = (gint)strtol(endp + 1, NULL, 10);
            }
            codecs = hls_attr_dup(attrs, "CODECS");
            audio_grp = hls_attr_dup(attrs, "AUDIO");
            sub_grp = hls_attr_dup(attrs, "SUBTITLES");
            continue;
        }

        if (expect_uri && line[0] != '#') {
            expect_uri = FALSE;
            g_autofree gchar *full = resolve_relative_uri(base_uri, line);
            const HlsMediaGroup *ag = audio_grp ? find_hls_group(media_groups, "AUDIO", audio_grp) : NULL;
            const HlsMediaGroup *sg = sub_grp ? find_hls_group(media_groups, "SUBTITLES", sub_grp) : NULL;

            GString *lbl = g_string_new(NULL);
            if (h > 0 && bw > 0)
                g_string_printf(lbl, "%dp HLS (%.1f Mbps)", h, (double)bw / 1000000.0);
            else if (h > 0)
                g_string_printf(lbl, "%dp HLS", h);
            else if (bw > 0)
                g_string_printf(lbl, "HLS (%.1f Mbps)", (double)bw / 1000000.0);
            else
                g_string_assign(lbl, "HLS Stream");
            if (ag && ag->language && *ag->language)
                g_string_append_printf(lbl, " · %s", ag->language);
            if (has_drm)
                g_string_append(lbl, " [DRM]");

            KajoWebMediaStream *s = web_media_stream_new(
                KAJO_MEDIA_HLS, full, lbl->str, "application/vnd.apple.mpegurl",
                w, h, bw, 0);
            g_string_free(lbl, TRUE);
            s->audio_uri = ag ? g_strdup(ag->uri) : NULL;
            s->audio_lang = (ag && ag->language) ? g_strdup(ag->language) : NULL;
            s->subtitle_uri = sg ? g_strdup(sg->uri) : NULL;
            s->codecs = g_strdup(codecs);
            s->group_id = g_strdup(base_uri ? base_uri : full);
            s->has_drm = has_drm;
            web_media_list_add_unique(out, s);
        }
    }

    g_clear_pointer(&codecs, g_free);
    g_clear_pointer(&audio_grp, g_free);
    g_clear_pointer(&sub_grp, g_free);

    /* Emit standalone audio renditions if any are present alongside master */
    for (guint i = 0; out->len > 0 && i < media_groups->len; i++) {
        const HlsMediaGroup *g = g_ptr_array_index(media_groups, i);
        if (g_strcmp0(g->type, "AUDIO") == 0 && g->uri && *g->uri) {
            g_autofree gchar *lbl = g_strdup_printf(
                "Audio HLS%s%s%s",
                (g->name && *g->name) ? " · " : "",
                (g->name && *g->name) ? g->name : "",
                (g->language && *g->language && g_strcmp0(g->name, g->language) != 0)
                    ? g->language : "");
            KajoWebMediaStream *as = web_media_stream_new(
                KAJO_MEDIA_HLS, g->uri, lbl, "application/vnd.apple.mpegurl",
                0, 0, 0, 0);
            as->is_audio_only = TRUE;
            as->audio_lang = g_strdup(g->language);
            as->group_id = g_strdup(base_uri ? base_uri : g->uri);
            as->has_drm = has_drm;
            web_media_list_add_unique(out, as);
        }
    }

    /* Single-variant HLS media playlist (#EXTINF segments, no #EXT-X-STREAM-INF) */
    if (out->len == 0 && extinf_total_sec > 0.0 && base_uri && *base_uri) {
        gint mins = (gint)(extinf_total_sec / 60.0);
        gint secs = (gint)extinf_total_sec % 60;
        g_autofree gchar *lbl = g_strdup_printf("HLS Stream (%d:%02d)%s",
                                                mins, secs, has_drm ? " [DRM]" : "");
        KajoWebMediaStream *s = web_media_stream_new(
            KAJO_MEDIA_HLS, base_uri, lbl, "application/vnd.apple.mpegurl",
            0, 0, 0, 0);
        s->group_id = g_strdup(base_uri);
        s->duration_sec = extinf_total_sec;
        s->has_drm = has_drm;
        web_media_list_add_unique(out, s);
    }

    if (out->len > 1)
        g_ptr_array_sort(out, cmp_media_variant_desc);
    return out;
}

static gchar *
extract_xml_str_attr(const char *tag, const char *attr)
{
    if (!tag || !attr)
        return NULL;
    size_t alen = strlen(attr);
    const char *p = tag;
    while ((p = strstr(p, attr)) != NULL) {
        if (p > tag && (g_ascii_isalnum(p[-1]) || p[-1] == '_' || p[-1] == '-')) {
            p += alen;
            continue;
        }
        const char *q = p + alen;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q != '=' || (q[1] != '"' && q[1] != '\'')) {
            p += alen;
            continue;
        }
        char quote = q[1];
        const char *vstart = q + 2;
        const char *vend = strchr(vstart, quote);
        if (!vend)
            return NULL;
        return g_strndup(vstart, (gsize)(vend - vstart));
    }
    return NULL;
}

static gboolean
extract_xml_int_attr(const char *tag, const char *attr, gint64 *out_val)
{
    g_autofree gchar *s = extract_xml_str_attr(tag, attr);
    if (!s || !*s)
        return FALSE;
    *out_val = g_ascii_strtoll(s, NULL, 10);
    return TRUE;
}

static gboolean
block_has_drm_protection(const char *block, gsize len)
{
    if (!block || len == 0)
        return FALSE;
    g_autofree gchar *low = g_ascii_strdown(block, (gssize)len);
    if (strstr(low, "<contentprotection") == NULL)
        return FALSE;
    return strstr(low, "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed") != NULL || /* Widevine */
           strstr(low, "9a04f079-9840-4286-ab92-e65be0885f95") != NULL || /* PlayReady */
           strstr(low, "94ce86fb-07ff-4f43-adb8-93d2fa968ca2") != NULL || /* FairPlay */
           strstr(low, "widevine") != NULL ||
           strstr(low, "playready") != NULL ||
           strstr(low, "fairplay") != NULL ||
           strstr(low, "cenc:default_kid") != NULL;
}

GPtrArray *
web_media_parse_dash_mpd(const gchar *mpd_text, const gchar *manifest_uri)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(web_media_stream_free);
    if (!mpd_text || !*mpd_text)
        return out;

    gdouble mpd_dur_sec = 0.0;
    const char *mpd_tag = strstr(mpd_text, "<MPD");
    if (mpd_tag) {
        const char *mpd_end = strchr(mpd_tag, '>');
        if (mpd_end) {
            g_autofree gchar *hdr = g_strndup(mpd_tag, (gsize)(mpd_end - mpd_tag + 1));
            g_autofree gchar *dur_str = extract_xml_str_attr(hdr, "mediaPresentationDuration");
            if (dur_str)
                mpd_dur_sec = web_media_parse_iso8601_duration(dur_str);
        }
    }

    const char *cur = mpd_text;
    const char *adapt = strstr(cur, "<AdaptationSet");
    if (!adapt) {
        /* Fallback if malformed test XML omits <AdaptationSet> */
        adapt = mpd_text;
    }

    while (adapt && *adapt) {
        const char *next_adapt = NULL;
        const char *block_end = NULL;
        g_autofree gchar *as_mime = NULL;
        g_autofree gchar *as_type = NULL;
        g_autofree gchar *as_lang = NULL;
        g_autofree gchar *as_codecs = NULL;
        gboolean as_drm = FALSE;

        if (strncmp(adapt, "<AdaptationSet", 14) == 0) {
            const char *tag_end = strchr(adapt, '>');
            if (!tag_end)
                break;
            g_autofree gchar *as_tag = g_strndup(adapt, (gsize)(tag_end - adapt + 1));
            as_mime = extract_xml_str_attr(as_tag, "mimeType");
            as_type = extract_xml_str_attr(as_tag, "contentType");
            as_lang = extract_xml_str_attr(as_tag, "lang");
            as_codecs = extract_xml_str_attr(as_tag, "codecs");

            const char *close_as = strstr(tag_end + 1, "</AdaptationSet>");
            next_adapt = strstr(tag_end + 1, "<AdaptationSet");
            if (close_as && (!next_adapt || close_as < next_adapt))
                block_end = close_as;
            else if (next_adapt)
                block_end = next_adapt;
            else
                block_end = adapt + strlen(adapt);

            as_drm = block_has_drm_protection(tag_end + 1, (gsize)(block_end - (tag_end + 1)));
            cur = tag_end + 1;
        } else {
            block_end = mpd_text + strlen(mpd_text);
            as_drm = block_has_drm_protection(mpd_text, (gsize)(block_end - mpd_text));
            cur = mpd_text;
        }

        const gboolean is_text_track =
            (as_type && g_ascii_strcasecmp(as_type, "text") == 0) ||
            (as_mime && g_str_has_prefix(as_mime, "text/"));

        if (!is_text_track) {
            while ((cur = strstr(cur, "<Representation")) != NULL && cur < block_end) {
                const char *end = strchr(cur, '>');
                if (!end || end > block_end)
                    break;
                g_autofree gchar *tag = g_strndup(cur, (gsize)(end - cur + 1));
                gint64 w = 0, h = 0, bw = 0;
                extract_xml_int_attr(tag, "width", &w);
                extract_xml_int_attr(tag, "height", &h);
                extract_xml_int_attr(tag, "bandwidth", &bw);
                g_autofree gchar *rep_id = extract_xml_str_attr(tag, "id");
                g_autofree gchar *rep_mime = extract_xml_str_attr(tag, "mimeType");
                g_autofree gchar *rep_codecs = extract_xml_str_attr(tag, "codecs");

                const char *eff_mime = rep_mime ? rep_mime : as_mime;
                const char *eff_codecs = rep_codecs ? rep_codecs : as_codecs;
                const gboolean audio_only =
                    (as_type && g_ascii_strcasecmp(as_type, "audio") == 0) ||
                    (eff_mime && g_str_has_prefix(eff_mime, "audio/")) ||
                    (h == 0 && bw > 0 && !(as_type && g_ascii_strcasecmp(as_type, "video") == 0));

                gboolean rep_drm = as_drm;
                if (!rep_drm && end > cur && end[-1] != '/') {
                    const char *rep_close = strstr(end + 1, "</Representation>");
                    if (rep_close && rep_close <= block_end) {
                        rep_drm = block_has_drm_protection(end + 1, (gsize)(rep_close - (end + 1)));
                    }
                }

                if (h > 0 || bw > 0) {
                    GString *lbl = g_string_new(NULL);
                    if (h > 0)
                        g_string_printf(lbl, "%dp DASH (%.1f Mbps)", (int)h, (double)bw / 1000000.0);
                    else
                        g_string_printf(lbl, "DASH Audio (%.0f kbps)", (double)bw / 1000.0);
                    if (as_lang && *as_lang)
                        g_string_append_printf(lbl, " · %s", as_lang);
                    if (rep_drm)
                        g_string_append(lbl, " [DRM]");

                    g_autofree gchar *key_uri = (rep_id && *rep_id)
                        ? g_strdup_printf("%s#id=%s", manifest_uri ? manifest_uri : "", rep_id)
                        : g_strdup_printf("%s#h=%d&bw=%" G_GINT64_FORMAT,
                                          manifest_uri ? manifest_uri : "", (int)h, bw);

                    KajoWebMediaStream *s = web_media_stream_new(
                        KAJO_MEDIA_DASH, key_uri, lbl->str, "application/dash+xml",
                        (gint)w, (gint)h, bw, 0);
                    g_string_free(lbl, TRUE);
                    s->rep_id = g_strdup(rep_id);
                    s->codecs = g_strdup(eff_codecs);
                    s->audio_lang = g_strdup(as_lang);
                    s->group_id = g_strdup(manifest_uri);
                    s->duration_sec = mpd_dur_sec;
                    s->has_drm = rep_drm;
                    s->is_audio_only = audio_only;
                    web_media_list_add_unique(out, s);
                }
                cur = end + 1;
            }
        }
        adapt = next_adapt;
    }

    if (out->len > 1)
        g_ptr_array_sort(out, cmp_media_variant_desc);
    return out;
}

static const gchar *
json_obj_get_str_safe(JsonObject *obj, const gchar *member)
{
    if (!obj || !json_object_has_member(obj, member))
        return NULL;
    JsonNode *n = json_object_get_member(obj, member);
    if (!n || JSON_NODE_HOLDS_NULL(n))
        return NULL;
    return json_node_get_string(n);
}

static void
extract_vimeo_cdn_manifest(JsonObject      *files,
                           const gchar     *section_name,
                           KajoWebMediaKind kind,
                           const gchar     *mime,
                           const gchar     *label_base,
                           const gchar     *config_uri,
                           gdouble          duration_sec,
                           GPtrArray       *out)
{
    if (!files || !json_object_has_member(files, section_name))
        return;
    JsonObject *sec = json_object_get_object_member(files, section_name);
    if (!sec || !json_object_has_member(sec, "cdns"))
        return;
    JsonObject *cdns = json_object_get_object_member(sec, "cdns");
    if (!cdns)
        return;

    const gchar *def_cdn = json_obj_get_str_safe(sec, "default_cdn");
    JsonObject *cdn = (def_cdn && json_object_has_member(cdns, def_cdn))
                          ? json_object_get_object_member(cdns, def_cdn)
                          : NULL;
    if (!cdn) {
        g_autoptr(GList) names = json_object_get_members(cdns);
        if (names && names->data)
            cdn = json_object_get_object_member(cdns, (const gchar *)names->data);
    }
    const gchar *url = json_obj_get_str_safe(cdn, "url");
    if (url && (g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://"))) {
        KajoWebMediaStream *s = web_media_stream_new(kind, url, label_base, mime, 0, 0, 0, 0);
        s->group_id = g_strdup(config_uri);
        s->duration_sec = duration_sec;
        web_media_list_add_unique(out, s);
    }
}

GPtrArray *
web_media_parse_vimeo_config(const gchar *json_text, const gchar *config_uri)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(web_media_stream_free);
    if (!json_text || !*json_text)
        return out;

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_text, -1, NULL))
        return out;

    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node))
        return out;
    JsonObject *root = json_node_get_object(root_node);

    gdouble duration_sec = 0.0;
    const gchar *title = NULL;
    if (json_object_has_member(root, "video")) {
        JsonObject *vid = json_object_get_object_member(root, "video");
        if (vid) {
            title = json_obj_get_str_safe(vid, "title");
            if (json_object_has_member(vid, "duration"))
                duration_sec = (gdouble)json_object_get_double_member(vid, "duration");
        }
    }

    if (!json_object_has_member(root, "request"))
        return out;
    JsonObject *req = json_object_get_object_member(root, "request");
    if (!req || !json_object_has_member(req, "files"))
        return out;
    JsonObject *files = json_object_get_object_member(req, "files");
    if (!files)
        return out;

    g_autofree gchar *hls_lbl = title && *title
        ? g_strdup_printf("%s · HLS Master", title)
        : g_strdup("Vimeo HLS Master");
    g_autofree gchar *dash_lbl = title && *title
        ? g_strdup_printf("%s · DASH Master", title)
        : g_strdup("Vimeo DASH Master");

    extract_vimeo_cdn_manifest(files, "hls", KAJO_MEDIA_HLS, "application/vnd.apple.mpegurl",
                               hls_lbl, config_uri, duration_sec, out);
    extract_vimeo_cdn_manifest(files, "dash", KAJO_MEDIA_DASH, "application/dash+xml",
                               dash_lbl, config_uri, duration_sec, out);

    if (json_object_has_member(files, "progressive")) {
        JsonArray *prog = json_object_get_array_member(files, "progressive");
        guint n = prog ? json_array_get_length(prog) : 0;
        for (guint i = 0; i < n; i++) {
            JsonObject *item = json_array_get_object_element(prog, i);
            const gchar *url = json_obj_get_str_safe(item, "url");
            if (!url || (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")))
                continue;
            gint w = (gint)json_object_get_int_member_with_default(item, "width", 0);
            gint h = (gint)json_object_get_int_member_with_default(item, "height", 0);
            const gchar *mime = json_obj_get_str_safe(item, "mime");
            const gchar *qual = json_obj_get_str_safe(item, "quality");
            g_autofree gchar *lbl = h > 0
                ? g_strdup_printf("%dp MP4 (%s)", (int)h, qual ? qual : "Vimeo")
                : g_strdup_printf("MP4 (%s)", qual ? qual : "Vimeo");
            KajoWebMediaStream *s = web_media_stream_new(
                KAJO_MEDIA_PROGRESSIVE, url, lbl, mime ? mime : "video/mp4",
                w, h, 0, 0);
            s->group_id = g_strdup(config_uri);
            s->duration_sec = duration_sec;
            web_media_list_add_unique(out, s);
        }
    }

    if (out->len > 1)
        g_ptr_array_sort(out, cmp_media_variant_desc);
    return out;
}

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
