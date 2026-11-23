#include "web_schemes.h"
#include "../config/kajo_config_store.h"
#include <string.h>

static void
append_base_head(GString *h, const gchar *title, const gchar *accent)
{
    g_string_append_printf(h,
        "<!DOCTYPE html><html><head><meta charset='utf-8'><title>%s</title><style>"
        "*{box-sizing:border-box;border-radius:0!important;margin:0;padding:0;}"
        "body{background:#101010;color:#fff;font-family:'Fira Code',monospace;font-size:13px;padding:34px 40px;}"
        ".wrap{max-width:860px;margin:0 auto;}"
        "h1{font-size:18px;letter-spacing:1.4px;border-bottom:1px solid #2b2b2b;padding-bottom:10px;margin-bottom:18px;display:flex;justify-content:space-between;align-items:center;}"
        ".sub{font-size:10px;color:#888;font-weight:400;}"
        ".grid{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin:18px 0;}"
        ".card{background:#181818;border:1px solid #2b2b2b;border-top:2px solid %s;padding:12px;text-decoration:none;color:#fff;display:block;}"
        ".card:hover{background:#222;border-color:%s;}"
        ".k{font-size:9.5px;color:#888;letter-spacing:.6px;}"
        ".v{font-size:12px;font-weight:700;color:#00d8f0;margin-top:4px;}"
        ".ok{color:#4ce886;} .warn{color:#ffb84d;}"
        "table{width:100%%;border-collapse:collapse;margin-top:12px;}"
        "th,td{border:1px solid #2b2b2b;padding:8px 10px;text-align:left;font-size:11.5px;}"
        "th{background:#181818;color:#999;font-size:10px;letter-spacing:.6px;}"
        "td{background:#131313;}"
        "a.row{display:flex;justify-content:space-between;padding:8px 10px;background:#161616;border:1px solid #242424;margin-bottom:6px;color:#e0e0e0;text-decoration:none;}"
        "a.row:hover{background:#222;border-color:%s;color:#fff;}"
        "</style></head><body><div class='wrap'>",
        title, accent, accent, accent);
}

static void
render_newtab(GString *h, const KajoWebSchemeContext *sctx, const KajoWebCfg *wcfg, const gchar *accent)
{
    append_base_head(h, "KAJO WEB // SPEED DIAL", accent);
    g_string_append(h, "<h1><span>❖ KAJO WEB // SPEED DIAL</span><span class='sub'>HOVER EDGE FOR TABS • CTRL+K PALETTE</span></h1>");

    g_string_append(h, "<div class='grid'>"
        "<a class='card' href='https://github.com'><div class='k'>CODE</div><div class='v' style='color:#fff;'>GitHub</div></a>"
        "<a class='card' href='https://wiki.archlinux.org'><div class='k'>DOCS</div><div class='v' style='color:#fff;'>ArchWiki</div></a>"
        "<a class='card' href='https://developer.mozilla.org'><div class='k'>WEB</div><div class='v' style='color:#fff;'>MDN Docs</div></a>"
        "<a class='card' href='kajo://gpu'><div class='k'>DIAGNOSTICS</div><div class='v'>kajo://gpu</div></a>"
        "</div>");

    gboolean is_sw = (wcfg && wcfg->hardware_acceleration == KAJO_WEB_HW_NEVER) || (sctx && sctx->hw_disabled_override);
    const gchar *prof = (sctx && sctx->is_private) ? "PRIVATE" : ((sctx && sctx->profile_name) ? sctx->profile_name : "DEFAULT");
    guint rules = (sctx && sctx->adblock) ? web_adblock_get_rule_count(sctx->adblock) : 0;
    gboolean ab_on = (sctx && sctx->adblock) ? web_adblock_is_enabled(sctx->adblock) : TRUE;

    g_string_append_printf(h,
        "<div class='grid'>"
        "<div class='card'><div class='k'>PROFILE</div><div class='v'>%s%s</div></div>"
        "<div class='card'><div class='k'>RENDER PIPELINE</div><div class='v %s'>%s</div></div>"
        "<div class='card'><div class='k'>CONTENT BLOCKER</div><div class='v'>%s (%u RULES)</div></div>"
        "<div class='card'><div class='k'>ENGINE</div><div class='v'>WEBKITGTK %u.%u.%u</div></div>"
        "</div>",
        prof, (sctx && sctx->sync_enabled) ? " • ☁" : "",
        is_sw ? "warn" : "ok", is_sw ? "⚠ SW FALLBACK" : "SKIA + DMABUF",
        ab_on ? "ON" : "OFF", rules,
        webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());

    if (sctx && sctx->storage && !sctx->is_private) {
        g_autoptr(GPtrArray) bms = web_storage_list_bookmarks(sctx->storage, FALSE);
        if (bms && bms->len > 0) {
            g_string_append(h, "<div class='k' style='margin:16px 0 8px;'>BOOKMARKS</div>");
            guint lim = MIN(bms->len, 10);
            for (guint i = 0; i < lim; i++) {
                const KajoWebBookmark *bm = g_ptr_array_index(bms, i);
                g_autofree gchar *t = g_markup_escape_text(bm->title ? bm->title : bm->uri, -1);
                g_autofree gchar *u = g_markup_escape_text(bm->uri ? bm->uri : "", -1);
                g_string_append_printf(h, "<a class='row' href='%s'><span>★ %s</span><span class='sub'>%s</span></a>", u, t, u);
            }
        }
    }
    g_string_append(h, "</div></body></html>");
}

static void
render_gpu(GString *h, const KajoWebSchemeContext *sctx, const KajoWebCfg *wcfg, const gchar *accent)
{
    append_base_head(h, "KAJO WEB // GPU TELEMETRY", accent);
    gboolean is_sw = (wcfg && wcfg->hardware_acceleration == KAJO_WEB_HW_NEVER) || (sctx && sctx->hw_disabled_override);
    const gchar *pol = is_sw ? "NEVER (Software Fallback)"
        : ((wcfg && wcfg->hardware_acceleration == KAJO_WEB_HW_ON_DEMAND) ? "ON-DEMAND (Skia + DMA-BUF)" : "ALWAYS (Skia + DMA-BUF)");
    gboolean has_ffmpeg = g_find_program_in_path("ffmpeg") != NULL;
    gboolean has_ytdlp  = g_find_program_in_path("yt-dlp") != NULL;

    g_string_append_printf(h,
        "<h1><span>⚡ kajo://gpu — GRAPHICS &amp; RUNTIME TELEMETRY</span><span class='sub'>WEBKITGTK %u.%u.%u</span></h1>"
        "<table><thead><tr><th>SUBSYSTEM</th><th>STATUS</th><th>DETAILS</th></tr></thead><tbody>"
        "<tr><td>WebKitGTK Runtime</td><td class='ok'>%u.%u.%u</td><td>GTK4 + libsoup3</td></tr>"
        "<tr><td>Hardware Acceleration</td><td class='%s'>%s</td><td>%s</td></tr>"
        "<tr><td>WebGL &amp; GPU Process</td><td class='%s'>%s</td><td>enable_webgl=%s, gpu_process=%s</td></tr>"
        "<tr><td>Sandbox</td><td class='ok'>MANDATORY ENFORCED</td><td>bubblewrap + xdg-dbus-proxy + seccomp</td></tr>"
        "<tr><td>Memory Limit</td><td class='ok'>%d MB</td><td>WebKitMemoryPressureSettings (idle suspend=%dm)</td></tr>"
        "<tr><td>Media Downloader</td><td class='ok'>ffmpeg: %s • yt-dlp: %s</td><td>Tier 1 HTML5 + Tier 2 HLS/DASH sniffer + Tier 3 yt-dlp</td></tr>"
        "</tbody></table></div></body></html>",
        webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version(),
        webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version(),
        is_sw ? "warn" : "ok", pol,
        is_sw ? "WEBKIT_DISABLE_DMABUF_RENDERER=1" : "Skia 2D + Skia Web Process Compositor + GBM DMA-BUF",
        (wcfg && wcfg->enable_webgl) ? "ok" : "warn",
        (wcfg && wcfg->enable_webgl) ? "ENABLED" : "DISABLED",
        (wcfg && wcfg->enable_webgl) ? "true" : "false",
        (wcfg && wcfg->gpu_process_webgl) ? "true" : "false",
        wcfg ? wcfg->memory_limit_mb : 2048,
        wcfg ? wcfg->suspend_idle_min : 30,
        has_ffmpeg ? "FOUND" : "MISSING",
        has_ytdlp ? "FOUND" : "OPTIONAL (NOT INSTALLED)");
}

static void
render_list_page(GString *h, const gchar *kind, const KajoWebSchemeContext *sctx, const gchar *accent)
{
    gboolean is_hist = g_strcmp0(kind, "history") == 0;
    append_base_head(h, is_hist ? "KAJO WEB // HISTORY" : "KAJO WEB // BOOKMARKS", accent);
    g_string_append_printf(h, "<h1><span>%s</span><span class='sub'>PROFILE: %s</span></h1>",
                           is_hist ? "↺ HISTORY" : "★ BOOKMARKS",
                           (sctx && sctx->profile_name) ? sctx->profile_name : "DEFAULT");

    if (!sctx || !sctx->storage || sctx->is_private) {
        g_string_append(h, "<p class='sub'>No persistent storage in private mode.</p></div></body></html>");
        return;
    }

    if (is_hist) {
        g_autoptr(GPtrArray) rows = web_storage_search_history(sctx->storage, NULL, 100);
        for (guint i = 0; i < rows->len; i++) {
            const KajoWebHistoryEntry *e = g_ptr_array_index(rows, i);
            g_autofree gchar *t = g_markup_escape_text(e->title && *e->title ? e->title : e->uri, -1);
            g_autofree gchar *u = g_markup_escape_text(e->uri, -1);
            g_string_append_printf(h, "<a class='row' href='%s'><span>%s</span><span class='sub'>%s (%dx)</span></a>",
                                   u, t, u, e->visit_count);
        }
        if (rows->len == 0)
            g_string_append(h, "<p class='sub'>History is empty.</p>");
    } else {
        g_autoptr(GPtrArray) rows = web_storage_list_bookmarks(sctx->storage, FALSE);
        for (guint i = 0; i < rows->len; i++) {
            const KajoWebBookmark *b = g_ptr_array_index(rows, i);
            g_autofree gchar *t = g_markup_escape_text(b->title && *b->title ? b->title : b->uri, -1);
            g_autofree gchar *u = g_markup_escape_text(b->uri, -1);
            g_string_append_printf(h, "<a class='row' href='%s'><span>★ %s</span><span class='sub'>%s</span></a>",
                                   u, t, u);
        }
        if (rows->len == 0)
            g_string_append(h, "<p class='sub'>No bookmarks saved yet (press Ctrl+D on any page).</p>");
    }
    g_string_append(h, "</div></body></html>");
}

gchar *
web_schemes_render_page(const gchar                *uri,
                        const KajoWebSchemeContext *sctx,
                        const KajoWebCfg           *wcfg,
                        const KajoThemeCfg         *tcfg)
{
    const gchar *accent = (sctx && sctx->profile_accent && *sctx->profile_accent)
        ? sctx->profile_accent
        : ((tcfg && tcfg->accent_color && *tcfg->accent_color) ? tcfg->accent_color : "#0078d4");

    GString *h = g_string_new(NULL);
    if (uri && g_str_has_prefix(uri, "kajo://gpu"))
        render_gpu(h, sctx, wcfg, accent);
    else if (uri && g_str_has_prefix(uri, "kajo://history"))
        render_list_page(h, "history", sctx, accent);
    else if (uri && g_str_has_prefix(uri, "kajo://bookmarks"))
        render_list_page(h, "bookmarks", sctx, accent);
    else
        render_newtab(h, sctx, wcfg, accent);
    return g_string_free(h, FALSE);
}

static void
on_kajo_scheme_request(WebKitURISchemeRequest *request, gpointer user_data)
{
    KajoWebSchemeContext *sctx = user_data;
    const gchar *uri = webkit_uri_scheme_request_get_uri(request);
    g_autoptr(KajoWebCfgRef) wcfg = kajo_config_store_acquire_web(NULL);
    g_autoptr(KajoThemeCfgRef) tcfg = kajo_config_store_acquire_theme(NULL);

    g_autofree gchar *html = web_schemes_render_page(uri, sctx, wcfg, tcfg);
    gsize len = strlen(html);
    g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_data(g_steal_pointer(&html), (gssize)len, g_free);
    webkit_uri_scheme_request_finish(request, stream, (gint64)len, "text/html; charset=utf-8");
}

void
web_schemes_register(WebKitWebContext *ctx, KajoWebSchemeContext *sctx)
{
    if (!ctx)
        return;
    webkit_web_context_register_uri_scheme(ctx, "kajo", on_kajo_scheme_request, sctx, NULL);
    WebKitSecurityManager *sm = webkit_web_context_get_security_manager(ctx);
    if (sm)
        webkit_security_manager_register_uri_scheme_as_secure(sm, "kajo");
}
