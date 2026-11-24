#include "web_schemes.h"
#include "../config/kajo_config_store.h"
#include <string.h>

static void
append_base_head(GString *h, const gchar *title, const gchar *accent)
{
    g_string_append_printf(h,
        "<!DOCTYPE html><html><head><meta charset='utf-8'><title>%s</title><style>"
        "*{box-sizing:border-box;border-radius:0!important;margin:0;padding:0;}"
        "body{background:#111111;color:#fff;font-family:'Fira Code',monospace;font-size:15px;padding:30px 38px;}"
        ".wrap{max-width:860px;margin:16px auto 0;}"
        "h1{font-size:20px;font-weight:800;letter-spacing:1.8px;border-bottom:1px solid #2b2b2b;padding-bottom:10px;margin-bottom:18px;display:flex;justify-content:space-between;align-items:center;gap:12px;}"
        ".sub{font-size:11.5px;color:#888;font-weight:400;letter-spacing:.6px;}"
        ".search{display:flex;border:1px solid #3a3a3a;background:#101010;margin-bottom:22px;}"
        ".search:focus-within{border-color:%s;}"
        ".search input{flex:1;background:transparent;border:0;color:#fff;font-family:'Fira Code',monospace;font-size:14px;padding:10px 12px;outline:none;}"
        ".search button{background:%s;border:0;color:#fff;font-family:'Fira Code',monospace;font-size:12px;font-weight:700;padding:0 18px;cursor:pointer;}"
        ".grid{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-bottom:20px;}"
        ".tile{background:#181818;border:1px solid #2b2b2b;border-top:2px solid %s;padding:16px;text-decoration:none;color:#fff;display:block;}"
        ".tile:hover{background:#222222;border-color:%s;}"
        ".ico{font-size:20px;margin-bottom:8px;}"
        ".nm{font-size:13.5px;font-weight:700;color:#fff;}"
        ".url{font-size:11.5px;color:#787878;margin-top:3px;}"
        ".tele-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-bottom:18px;}"
        ".tele{background:#161616;border:1px solid #242424;padding:10px 12px;}"
        ".k{font-size:11px;color:#787878;letter-spacing:.6px;}"
        ".v{font-size:13px;font-weight:700;color:#00d8f0;margin-top:3px;}"
        ".ok{color:#4ce886;} .warn{color:#ffb84d;}"
        "table{width:100%%;border-collapse:collapse;margin-top:12px;}"
        "th,td{border:1px solid #2b2b2b;padding:9px 11px;text-align:left;font-size:13px;}"
        "th{background:#181818;color:#a0a0a0;font-size:11px;letter-spacing:.6px;}"
        "td{background:#121212;}"
        ".list-row{display:flex;align-items:center;gap:8px;background:#161616;border:1px solid #242424;margin-bottom:6px;}"
        ".list-row:hover{background:#222;border-color:%s;}"
        "a.row{flex:1;min-width:0;display:flex;justify-content:space-between;gap:12px;padding:9px 10px;color:#e0e0e0;text-decoration:none;}"
        "a.row span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
        "a.row:hover{color:#fff;}"
        "a.del{padding:8px 11px;color:#888;text-decoration:none;border-left:1px solid #242424;font-weight:700;}"
        "a.del:hover{color:#ff5555;background:#2a1414;}"
        "a.btn-danger{display:inline-block;padding:6px 12px;border:1px solid #a4262c;color:#ff8080;font-size:11px;text-decoration:none;letter-spacing:.6px;}"
        "a.btn-danger:hover{background:#a4262c;color:#fff;}"
        "</style></head><body><div class='wrap'>",
        title, accent, accent, accent, accent, accent);
}

static void
render_newtab(GString *h, const KajoWebSchemeContext *sctx, const KajoWebCfg *wcfg, const gchar *accent)
{
    append_base_head(h, "KAJO WEB // SPEED DIAL", accent);
    g_string_append(h,
        "<h1><span>❖ KAJO WEB // SPEED DIAL</span>"
        "<span class='sub'>HOVER EDGE FOR TABS • CTRL+K PALETTE</span></h1>");

    g_string_append(h,
        "<form class='search' action='kajo://search' method='GET'>"
        "<input name='q' placeholder='Search or enter URL (press Ctrl+L for omnibar bangs)...' autocomplete='off'/>"
        "<button type='submit'>GO</button></form>");

    g_string_append(h,
        "<div class='grid'>"
        "<a class='tile' href='https://github.com'><div class='ico'>⌘</div><div class='nm'>GitHub</div><div class='url'>github.com</div></a>"
        "<a class='tile' href='https://wiki.archlinux.org'><div class='ico'>📘</div><div class='nm'>ArchWiki</div><div class='url'>wiki.archlinux.org</div></a>"
        "<a class='tile' href='https://developer.mozilla.org'><div class='ico'>🌐</div><div class='nm'>MDN Docs</div><div class='url'>developer.mozilla.org</div></a>"
        "<a class='tile' href='kajo://gpu'><div class='ico'>⚡</div><div class='nm'>GPU Telemetry</div><div class='url'>kajo://gpu</div></a>"
        "</div>");

    gboolean is_sw = (wcfg && wcfg->hardware_acceleration == KAJO_WEB_HW_NEVER) || (sctx && sctx->hw_disabled_override);
    const gchar *prof = (sctx && sctx->is_private) ? "PRIVATE" : ((sctx && sctx->profile_name) ? sctx->profile_name : "DEFAULT");
    gboolean ab_on = (sctx && sctx->adblock) ? web_adblock_is_enabled(sctx->adblock) : FALSE;
    gboolean ab_ready = (sctx && sctx->adblock) ? web_adblock_is_ready(sctx->adblock) : FALSE;
    const gchar *ab_status = (sctx && sctx->adblock) ? web_adblock_get_status_text(sctx->adblock) : "IDLE";

    g_string_append_printf(h,
        "<div class='tele-grid'>"
        "<div class='tele'><div class='k'>PROFILE</div><div class='v'>%s%s</div></div>"
        "<div class='tele'><div class='k'>RENDER PIPELINE</div><div class='v %s'>%s</div></div>"
        "<div class='tele'><div class='k'>CONTENT BLOCKER</div><div class='v %s'>%s</div></div>"
        "<div class='tele'><div class='k'>ENGINE</div><div class='v'>WEBKITGTK %u.%u.%u</div></div>"
        "</div>",
        prof, (sctx && sctx->sync_enabled) ? " • ☁" : "",
        is_sw ? "warn" : "ok", is_sw ? "⚠ SW FALLBACK" : "SKIA + DMABUF",
        (ab_on && ab_ready) ? "ok" : "warn", ab_status,
        webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());

    if (sctx && sctx->storage && !sctx->is_private) {
        g_autoptr(GPtrArray) bms = web_storage_list_bookmarks(sctx->storage, FALSE);
        if (bms && bms->len > 0) {
            g_string_append(h, "<div class='k' style='margin:14px 0 8px;'>BOOKMARKS</div>");
            guint lim = MIN(bms->len, 10);
            for (guint i = 0; i < lim; i++) {
                const KajoWebBookmark *bm = g_ptr_array_index(bms, i);
                g_autofree gchar *t = g_markup_escape_text(bm->title ? bm->title : bm->uri, -1);
                g_autofree gchar *u = g_markup_escape_text(bm->uri ? bm->uri : "", -1);
                g_string_append_printf(h,
                    "<div class='list-row'><a class='row' href='%s'><span>★ %s</span><span class='sub'>%s</span></a></div>",
                    u, t, u);
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
    const gchar *pol = is_sw ? "NEVER (Software Fallback)" : "ALWAYS (Skia + DMA-BUF)";
    const gchar *gsk = g_getenv("GSK_RENDERER");
    const gchar *skia_thr = g_getenv("WEBKIT_SKIA_GPU_PAINTING_THREADS");
    g_autofree gchar *ffp = g_find_program_in_path("ffmpeg");
    g_autofree gchar *ytp = g_find_program_in_path("yt-dlp");

    gboolean gpu_webgl = (wcfg && wcfg->gpu_process_webgl);
    g_string_append_printf(h,
        "<h1><span>⚡ kajo://gpu — GRAPHICS &amp; RUNTIME TELEMETRY</span><span class='sub'>WEBKITGTK %u.%u.%u</span></h1>"
        "<table><thead><tr><th>SUBSYSTEM</th><th>STATUS</th><th>DETAILS</th></tr></thead><tbody>"
        "<tr><td>WebKitGTK Runtime</td><td class='ok'>%u.%u.%u</td><td>GTK4 + libsoup3 + WEB_BROWSER Cache Model</td></tr>"
        "<tr><td>Hardware Acceleration</td><td class='%s'>%s</td><td>%s</td></tr>"
        "<tr><td>GSK &amp; Skia Paint</td><td class='ok'>GSK=%s • THREADS=%s</td><td>UseGPUProcessForWebGL=%s • Skia GPU Compositor</td></tr>"
        "<tr><td>WebGL</td><td class='%s'>%s</td><td>enable_webgl=%s</td></tr>"
        "<tr><td>Content Blocker</td><td class='ok'>%s</td><td>WebKitUserContentFilterStore compiled bytecode (%u rules)</td></tr>"
        "<tr><td>Sandbox</td><td class='ok'>MANDATORY ENFORCED</td><td>bubblewrap + xdg-dbus-proxy + seccomp</td></tr>"
        "<tr><td>Memory Limit</td><td class='ok'>%d MB</td><td>WebKitMemoryPressureSettings (idle suspend=%dm)</td></tr>"
        "<tr><td>Media Downloader</td><td class='ok'>ffmpeg: %s • yt-dlp: %s</td><td>Tier 1 HTML5 + Tier 2 HLS/DASH sniffer + Tier 3 yt-dlp</td></tr>"
        "</tbody></table></div></body></html>",
        webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version(),
        webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version(),
        is_sw ? "warn" : "ok", pol,
        is_sw ? "WEBKIT_DISABLE_DMABUF_RENDERER=1" : "Skia 2D + Skia Web Process Compositor + GBM DMA-BUF",
        (gsk && *gsk) ? gsk : "auto",
        (skia_thr && *skia_thr) ? skia_thr : "default",
        gpu_webgl ? "ON" : "OFF",
        (wcfg && wcfg->enable_webgl) ? "ok" : "warn",
        (wcfg && wcfg->enable_webgl) ? "ENABLED" : "DISABLED",
        (wcfg && wcfg->enable_webgl) ? "true" : "false",
        (sctx && sctx->adblock) ? web_adblock_get_status_text(sctx->adblock) : "ACTIVE",
        (sctx && sctx->adblock) ? web_adblock_get_rule_count(sctx->adblock) : 0,
        wcfg ? wcfg->memory_limit_mb : 2048,
        wcfg ? wcfg->suspend_idle_min : 30,
        ffp ? "FOUND" : "MISSING",
        ytp ? "FOUND" : "OPTIONAL (NOT INSTALLED)");
}

static void
render_list_page(GString *h, const gchar *kind, const KajoWebSchemeContext *sctx, const gchar *accent)
{
    gboolean is_hist = g_strcmp0(kind, "history") == 0;
    const gchar *tok = (sctx && sctx->action_token[0]) ? sctx->action_token : "";
    g_autofree gchar *esc_tok = g_uri_escape_string(tok, NULL, TRUE);

    append_base_head(h, is_hist ? "KAJO WEB // HISTORY" : "KAJO WEB // BOOKMARKS", accent);
    g_string_append_printf(h,
        "<h1><span>%s</span><span style='display:flex;align-items:center;gap:12px;'>"
        "<span class='sub'>PROFILE: %s</span>",
        is_hist ? "↺ HISTORY" : "★ BOOKMARKS",
        (sctx && sctx->profile_name) ? sctx->profile_name : "DEFAULT");

    if (is_hist && sctx && sctx->storage && !sctx->is_private) {
        g_string_append_printf(h,
            "<a class='btn-danger' href='kajo://action/clear-history?tok=%s'>CLEAR HISTORY</a>",
            esc_tok);
    }
    g_string_append(h, "</span></h1>");

    if (!sctx || !sctx->storage || sctx->is_private) {
        g_string_append(h, "<p class='sub'>No persistent storage in private mode.</p></div></body></html>");
        return;
    }

    g_string_append(h,
        "<div class='search' style='margin-bottom:14px;'>"
        "<input id='flt' placeholder='Filter entries by title or URL...' oninput=\""
        "var q=this.value.toLowerCase();document.querySelectorAll('.list-row').forEach(function(r){"
        "r.style.display=r.textContent.toLowerCase().indexOf(q)>=0?'flex':'none';});\"/>"
        "</div>");

    if (is_hist) {
        g_autoptr(GPtrArray) rows = web_storage_search_history(sctx->storage, NULL, 300);
        for (guint i = 0; i < rows->len; i++) {
            const KajoWebHistoryEntry *e = g_ptr_array_index(rows, i);
            g_autofree gchar *t = g_markup_escape_text(e->title && *e->title ? e->title : e->uri, -1);
            g_autofree gchar *u = g_markup_escape_text(e->uri, -1);
            g_autofree gchar *eu = g_uri_escape_string(e->uri, NULL, TRUE);
            g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(e->last_visit_ms / 1000);
            g_autofree gchar *ts = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M") : g_strdup("");
            g_string_append_printf(h,
                "<div class='list-row'>"
                "<a class='row' href='%s'><span>%s</span><span class='sub'>%s • %s (%dx)</span></a>"
                "<a class='del' href='kajo://action/delete-history?tok=%s&amp;uri=%s' title='Remove entry'>✕</a>"
                "</div>",
                u, t, ts, u, e->visit_count, esc_tok, eu);
        }
        if (rows->len == 0)
            g_string_append(h, "<p class='sub'>History is empty.</p>");
    } else {
        g_autoptr(GPtrArray) rows = web_storage_list_bookmarks(sctx->storage, FALSE);
        for (guint i = 0; i < rows->len; i++) {
            const KajoWebBookmark *b = g_ptr_array_index(rows, i);
            g_autofree gchar *t = g_markup_escape_text(b->title && *b->title ? b->title : b->uri, -1);
            g_autofree gchar *u = g_markup_escape_text(b->uri, -1);
            g_autofree gchar *eu = g_uri_escape_string(b->uri, NULL, TRUE);
            g_string_append_printf(h,
                "<div class='list-row'>"
                "<a class='row' href='%s'><span>★ %s</span><span class='sub'>%s</span></a>"
                "<a class='del' href='kajo://action/delete-bookmark?tok=%s&amp;uri=%s' title='Remove bookmark'>✕</a>"
                "</div>",
                u, t, u, esc_tok, eu);
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
