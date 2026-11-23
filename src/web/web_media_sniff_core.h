#ifndef KAJO_WEB_MEDIA_SNIFF_CORE_H
#define KAJO_WEB_MEDIA_SNIFF_CORE_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    KAJO_MEDIA_NONE = 0,
    KAJO_MEDIA_HLS,         /* .m3u8 / application/vnd.apple.mpegurl */
    KAJO_MEDIA_DASH,        /* .mpd  / application/dash+xml */
    KAJO_MEDIA_PROGRESSIVE, /* .mp4, .webm, video MIME >= 512 KB */
} KajoWebMediaKind;

typedef struct {
    KajoWebMediaKind kind;
    gchar           *uri;
    gchar           *label;      /* e.g. "1080p HLS (4.8 Mbps)" */
    gchar           *mime_type;
    gint             width;
    gint             height;
    gint64           bandwidth;  /* bits/sec or 0 */
    gint64           size_bytes; /* Content-Length or 0 */
} KajoWebMediaStream;

KajoWebMediaStream *web_media_stream_new(KajoWebMediaKind kind,
                                         const gchar     *uri,
                                         const gchar     *label,
                                         const gchar     *mime_type,
                                         gint             width,
                                         gint             height,
                                         gint64           bandwidth,
                                         gint64           size_bytes);
void                web_media_stream_free(gpointer ptr);

/* Classify a resource response (VDH-style: .m3u8, .mpd, or video MIME >= 512 KB) */
KajoWebMediaKind    web_media_classify_resource(const gchar *uri,
                                                const gchar *mime_type,
                                                gint64       content_length);

/* Parse HLS #EXT-X-STREAM-INF master playlist into KajoWebMediaStream* variants */
GPtrArray          *web_media_parse_hls_master(const gchar *manifest_text,
                                               const gchar *base_uri);

/* Parse DASH MPD <Representation ...> tags into KajoWebMediaStream* variants */
GPtrArray          *web_media_parse_dash_mpd(const gchar *mpd_text,
                                             const gchar *manifest_uri);

/* Deduplicated per-tab media stream list helper */
gboolean            web_media_list_add_unique(GPtrArray          *list,
                                              KajoWebMediaStream *stream);

G_END_DECLS

#endif /* KAJO_WEB_MEDIA_SNIFF_CORE_H */
