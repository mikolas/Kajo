#ifndef KAJO_WEB_DOWNLOADS_H
#define KAJO_WEB_DOWNLOADS_H

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include "web_media_sniff_core.h"
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

typedef struct _KajoWebDownloads KajoWebDownloads;

typedef void (*KajoWebDownloadsChangedCb)(KajoWebDownloads *dl, guint active_count, guint total_count, gpointer ud);

KajoWebDownloads *web_downloads_new(KajoWebDownloadsChangedCb on_changed, gpointer user_data);
void              web_downloads_free(KajoWebDownloads *dl);

/* Tier 1: Direct WebKitDownload */
void              web_downloads_track_webkit(KajoWebDownloads *dl,
                                             WebKitDownload   *wk_dl,
                                             const KajoWebCfg *cfg);

/* Tier 2: VDH-style sniffed HLS (.m3u8) / DASH (.mpd) remux via system ffmpeg */
void              web_downloads_start_ffmpeg_stream(KajoWebDownloads         *dl,
                                                    const KajoWebMediaStream *stream,
                                                    const gchar              *page_title,
                                                    const gchar              *referer_uri,
                                                    WebKitNetworkSession     *session,
                                                    const KajoWebCfg         *cfg);

/* Tier 3: Optional yt-dlp + ffmpeg for JS-obfuscated YouTube/social pages */
void              web_downloads_start_ytdlp(KajoWebDownloads   *dl,
                                            const gchar        *page_uri,
                                            KajoWebDownloadFmt  fmt,
                                            const KajoWebCfg   *cfg);

/* Direct handoff to kajo-player (libmpv) */
void              web_downloads_open_in_kajo_player(const gchar *uri_or_path);

/* Build popovers */
GtkWidget        *web_downloads_build_popover_content(KajoWebDownloads *dl);
gboolean          web_downloads_has_ffmpeg(void);
gboolean          web_downloads_has_ytdlp(void);

G_END_DECLS

#endif /* KAJO_WEB_DOWNLOADS_H */
