#ifndef KAJO_WEB_CEF_CLIPBOARD_H
#define KAJO_WEB_CEF_CLIPBOARD_H

#include <functional>
#include <string>

#include "include/cef_browser.h"
#include "include/cef_request_context.h"

namespace kajo_web {

/* Copies plain text to the Wayland/Chromium clipboard using the target browser's
 * main frame (no external wl-copy dependency required). */
void CopyTextViaBrowser(CefRefPtr<CefBrowser> browser, const std::string& text);

/* Fetches image_url (supports http://, https://, and data:image/...;base64,...)
 * through the profile's CefRequestContext (inheriting cookies/auth) and writes
 * a normalized image/png ClipboardItem into the Wayland/Chromium clipboard. */
void CopyImageViaBrowser(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefRequestContext> ctx,
                         const std::string& image_url,
                         std::function<void(bool ok)> on_done);

}  // namespace kajo_web

#endif  // KAJO_WEB_CEF_CLIPBOARD_H
