#ifndef KAJO_WEB_CEF_MEDIA_NET_H
#define KAJO_WEB_CEF_MEDIA_NET_H

#include <functional>
#include <map>
#include <string>

#include "include/cef_request_context.h"
#include "web_cef/cef_media_dl.h"

namespace kajo_web {

/* Creates a MediaFetchStreamFn backed by Chromium/CEF's CefURLRequest on the
 * given CefRequestContext (inheriting profile cookies, HttpOnly auth, TLS/HTTP2
 * fingerprint, and the tab's captured Referer/Origin/User-Agent/Authorization headers).
 * Safe to invoke synchronously from any background/proxy worker thread; never blocks TID_UI. */
MediaFetchStreamFn CreateCefMediaFetcher(CefRefPtr<CefRequestContext> ctx,
                                         std::map<std::string, std::string> captured_headers,
                                         std::string referer,
                                         std::string user_agent);

/* Exports cookies matching page_url (including HttpOnly cookies) from ctx's
 * CefCookieManager into an ephemeral 0600 Netscape cookie file for yt-dlp.
 * Invokes cb on TID_UI with the temp path (or "" if no cookies / unavailable).
 * Caller is responsible for unlinking the temp file when done. */
void ExportNetscapeCookiesAsync(CefRefPtr<CefRequestContext> ctx,
                                const std::string& page_url,
                                std::function<void(std::string temp_cookie_file)> cb);

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_MEDIA_NET_H
