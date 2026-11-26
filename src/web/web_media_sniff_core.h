#ifndef KAJO_WEB_MEDIA_SNIFF_CORE_H
#define KAJO_WEB_MEDIA_SNIFF_CORE_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    KAJO_MEDIA_NONE = 0,
    KAJO_MEDIA_HLS,         /* .m3u8 / application/vnd.apple.mpegurl */
    KAJO_MEDIA_DASH,        /* .mpd  / application/dash+xml */
    KAJO_MEDIA_PROGRESSIVE, /* .mp4, .webm, video/audio >= threshold */
} KajoWebMediaKind;

typedef struct {
    KajoWebMediaKind kind;
    gchar           *uri;
    gchar           *label;        /* e.g. "1080p HLS (4.8 Mbps)" */
    gchar           *mime_type;
    gint             width;
    gint             height;
    gint64           bandwidth;    /* bits/sec or 0 */
    gint64           size_bytes;   /* Content-Length / Content-Range total or 0 */
    gchar           *audio_uri;    /* paired HLS audio rendition URI or NULL */
    gchar           *subtitle_uri; /* paired HLS subtitle URI or NULL */
    gchar           *audio_lang;   /* language tag (e.g. "en") or NULL */
    gchar           *codecs;       /* codec string or NULL */
    gchar           *group_id;     /* logical source manifest/config URI or NULL */
    gchar           *rep_id;       /* DASH <Representation id> or NULL */
    gdouble          duration_sec; /* parsed manifest duration or 0.0 */
    gboolean         has_drm;      /* Widevine/FairPlay/PlayReady/SAMPLE-AES */
    gboolean         is_audio_only;
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

/* Header & duration parsers */
gint64              web_media_parse_content_range_total(const gchar *content_range);
gchar              *web_media_parse_content_disposition_filename(const gchar *header);
gdouble             web_media_parse_iso8601_duration(const gchar *iso);

/* Classify a resource response (HLS, DASH, Vimeo config capture, or progressive media) */
gboolean            web_media_should_capture_manifest(const gchar *uri,
                                                      const gchar *mime_type);
KajoWebMediaKind    web_media_classify_resource_ex(const gchar *uri,
                                                   const gchar *mime_type,
                                                   gint64       content_length,
                                                   const gchar *content_range,
                                                   gboolean     page_has_manifest);
KajoWebMediaKind    web_media_classify_resource(const gchar *uri,
                                                const gchar *mime_type,
                                                gint64       content_length);

/* Parse HLS master/media playlist into KajoWebMediaStream* variants */
GPtrArray          *web_media_parse_hls_master(const gchar *manifest_text,
                                               const gchar *base_uri);

/* Parse DASH MPD <AdaptationSet>/<Representation> tags into KajoWebMediaStream* variants */
GPtrArray          *web_media_parse_dash_mpd(const gchar *mpd_text,
                                             const gchar *manifest_uri);

/* Parse Vimeo player config JSON into KajoWebMediaStream* variants */
GPtrArray          *web_media_parse_vimeo_config(const gchar *json_text,
                                                 const gchar *config_uri);

/* Deduplicated per-tab media stream list helper */
gboolean            web_media_list_add_unique(GPtrArray          *list,
                                              KajoWebMediaStream *stream);

G_END_DECLS

#endif /* KAJO_WEB_MEDIA_SNIFF_CORE_H */
