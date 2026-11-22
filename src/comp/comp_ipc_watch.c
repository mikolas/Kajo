/* comp_ipc_watch.c — IPC JSON builder/serializers, event emitters & inotify config watcher. */
#include "comp_ipc_private.h"

#include "comp_log.h"
#include "core/comp_schema_keys.h"

#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <talloc.h>
#include <unistd.h>
#include <wlr/types/wlr_output.h>

/* ───────────────────────── Minimal JSON Builder ───────────────────────── */

void
comp_ipc_jbuf_init(JsonBuf *b, void *talloc_ctx)
{
    b->ctx = talloc_ctx;
    b->cap = 512;
    b->len = 0;
    b->buf = talloc_zero_array(talloc_ctx, char, (unsigned)b->cap);
}

static bool
jbuf_reserve(JsonBuf *b, size_t extra)
{
    if (!b->buf)
        return false;
    if (b->len + extra + 1 <= b->cap)
        return true;
    size_t ncap = b->cap * 2;
    while (ncap < b->len + extra + 1)
        ncap *= 2;
    char *nb = talloc_realloc(b->ctx, b->buf, char, (unsigned)ncap);
    if (!nb)
        return false;
    b->buf = nb;
    b->cap = ncap;
    return true;
}

static void
jbuf_append_raw(JsonBuf *b, const char *s, size_t n)
{
    if (!jbuf_reserve(b, n))
        return;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

void
comp_ipc_jbuf_puts(JsonBuf *b, const char *s)
{
    if (s)
        jbuf_append_raw(b, s, strlen(s));
}

__attribute__((format(printf, 2, 3)))
void
comp_ipc_jbuf_printf(JsonBuf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need > 0 && jbuf_reserve(b, (size_t)need)) {
        vsnprintf(b->buf + b->len, (size_t)need + 1, fmt, ap2);
        b->len += (size_t)need;
    }
    va_end(ap2);
}

void
comp_ipc_jbuf_str(JsonBuf *b, const char *s)
{
    if (!s)
        s = "";
    comp_ipc_jbuf_puts(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  comp_ipc_jbuf_puts(b, "\\\""); break;
        case '\\': comp_ipc_jbuf_puts(b, "\\\\"); break;
        case '\b': comp_ipc_jbuf_puts(b, "\\b");  break;
        case '\f': comp_ipc_jbuf_puts(b, "\\f");  break;
        case '\n': comp_ipc_jbuf_puts(b, "\\n");  break;
        case '\r': comp_ipc_jbuf_puts(b, "\\r");  break;
        case '\t': comp_ipc_jbuf_puts(b, "\\t");  break;
        default:
            if (c < 0x20)
                comp_ipc_jbuf_printf(b, "\\u%04x", (unsigned)c);
            else
                jbuf_append_raw(b, (const char *)p, 1);
            break;
        }
    }
    comp_ipc_jbuf_puts(b, "\"");
}

/* ───────────────────────── Payload Serializers ───────────────────────── */

void
comp_ipc_append_outputs_array(JsonBuf *b, const CompServer *s)
{
    comp_ipc_jbuf_puts(b, "[");
    bool first = true;
    const CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        const struct wlr_output *wo = o->wlr_output;
        if (!wo)
            continue;
        if (!first)
            comp_ipc_jbuf_puts(b, ",");
        first = false;
        comp_ipc_jbuf_puts(b, "{\"name\":");
        comp_ipc_jbuf_str(b, wo->name ? wo->name : "unknown");
        comp_ipc_jbuf_puts(b, ",\"make\":");
        comp_ipc_jbuf_str(b, wo->make ? wo->make : "");
        comp_ipc_jbuf_puts(b, ",\"model\":");
        comp_ipc_jbuf_str(b, wo->model ? wo->model : "");
        comp_ipc_jbuf_printf(b, ",\"active\":%s,\"width\":%d,\"height\":%d,\"refresh_mhz\":%d,\"scale\":%.4g,\"transform\":",
                             wo->enabled ? "true" : "false",
                             wo->width, wo->height, wo->refresh, (double)wo->scale);
        comp_ipc_jbuf_str(b, comp_transform_to_str((CompTransform)wo->transform));
        comp_ipc_jbuf_puts(b, ",\"vrr\":");
        comp_ipc_jbuf_str(b, comp_vrr_policy_to_str(o->vrr_policy));
        comp_ipc_jbuf_puts(b, ",\"modes\":[");
        bool first_m = true;
        const struct wlr_output_mode *m;
        wl_list_for_each(m, &wo->modes, link) {
            if (!first_m)
                comp_ipc_jbuf_puts(b, ",");
            first_m = false;
            comp_ipc_jbuf_printf(b, "{\"width\":%d,\"height\":%d,\"refresh_mhz\":%d,\"preferred\":%s}",
                                 m->width, m->height, m->refresh, m->preferred ? "true" : "false");
        }
        comp_ipc_jbuf_puts(b, "]}");
    }
    comp_ipc_jbuf_puts(b, "]");
}

void
comp_ipc_append_workspaces_array(JsonBuf *b, const CompServer *s)
{
    comp_ipc_jbuf_puts(b, "[");
    size_t n_ws = s->layout ? comp_layout_workspace_count(s->layout) : 1;
    size_t active_ws = s->layout ? comp_layout_active_workspace(s->layout) : 0;

    /* Check urgency per workspace */
    size_t n_wins = comp_window_list_info(s, NULL, 0);
    CompWindowInfo *wins = n_wins > 0 ? talloc_array(b->ctx, CompWindowInfo, (unsigned)n_wins) : NULL;
    if (wins)
        comp_window_list_info(s, wins, n_wins);

    for (size_t i = 0; i < n_ws; i++) {
        if (i > 0)
            comp_ipc_jbuf_puts(b, ",");
        size_t wcount = s->layout ? comp_layout_workspace_window_count(s->layout, i) : 0;
        bool urgent = false;
        for (size_t k = 0; k < n_wins && wins; k++) {
            if (wins[k].workspace_idx == i + 1 && wins[k].is_urgent) {
                urgent = true;
                break;
            }
        }
        comp_ipc_jbuf_printf(b, "{\"idx\":%zu,\"id\":%zu,\"is_active\":%s,\"is_urgent\":%s,\"window_count\":%zu}",
                             i + 1, i + 1,
                             (i == active_ws) ? "true" : "false",
                             urgent ? "true" : "false",
                             wcount);
    }
    comp_ipc_jbuf_puts(b, "]");
}

void
comp_ipc_append_windows_array(JsonBuf *b, const CompServer *s)
{
    comp_ipc_jbuf_puts(b, "[");
    size_t n_wins = comp_window_list_info(s, NULL, 0);
    CompWindowInfo *wins = n_wins > 0 ? talloc_array(b->ctx, CompWindowInfo, (unsigned)n_wins) : NULL;
    if (wins) {
        n_wins = comp_window_list_info(s, wins, n_wins);
        for (size_t i = 0; i < n_wins; i++) {
            if (i > 0)
                comp_ipc_jbuf_puts(b, ",");
            comp_ipc_jbuf_printf(b, "{\"id\":%llu,\"title\":", (unsigned long long)wins[i].id);
            comp_ipc_jbuf_str(b, wins[i].title);
            comp_ipc_jbuf_puts(b, ",\"app_id\":");
            comp_ipc_jbuf_str(b, wins[i].app_id);
            comp_ipc_jbuf_printf(b, ",\"workspace_idx\":%zu,\"is_focused\":%s,\"is_floating\":%s,\"is_fullscreen\":%s,\"is_urgent\":%s}",
                                 wins[i].workspace_idx,
                                 wins[i].is_focused ? "true" : "false",
                                 wins[i].is_floating ? "true" : "false",
                                 wins[i].is_fullscreen ? "true" : "false",
                                 wins[i].is_urgent ? "true" : "false");
        }
    }
    comp_ipc_jbuf_puts(b, "]");
}

void
comp_ipc_append_keyboard_layouts_fields(JsonBuf *b, const CompServer *s)
{
    const char *names[16];
    uint32_t cur = 0;
    size_t n = comp_input_get_keyboard_layouts(s, names, 16, &cur);
    if (n > 16)
        n = 16;
    comp_ipc_jbuf_puts(b, "\"names\":[");
    for (size_t i = 0; i < n; i++) {
        if (i > 0)
            comp_ipc_jbuf_puts(b, ",");
        comp_ipc_jbuf_str(b, names[i]);
    }
    comp_ipc_jbuf_printf(b, "],\"current_idx\":%u", cur);
}

/* ───────────────────────── Event Builders & Emitters ───────────────────────── */

void
comp_ipc_build_workspaces_event(JsonBuf *b, const CompServer *s)
{
    b->len = 0;
    comp_ipc_jbuf_puts(b, "{\"event\":\"" COMP_IPC_EVENT_WORKSPACES_CHANGED "\",\"workspaces\":");
    comp_ipc_append_workspaces_array(b, s);
    comp_ipc_jbuf_puts(b, "}\n");
}

void
comp_ipc_build_windows_event(JsonBuf *b, const CompServer *s)
{
    b->len = 0;
    comp_ipc_jbuf_puts(b, "{\"event\":\"" COMP_IPC_EVENT_WINDOWS_CHANGED "\",\"windows\":");
    comp_ipc_append_windows_array(b, s);
    comp_ipc_jbuf_puts(b, "}\n");
}

void
comp_ipc_build_keyboard_layouts_event(JsonBuf *b, const CompServer *s)
{
    b->len = 0;
    comp_ipc_jbuf_puts(b, "{\"event\":\"" COMP_IPC_EVENT_KEYBOARD_LAYOUTS_CHANGED "\",");
    comp_ipc_append_keyboard_layouts_fields(b, s);
    comp_ipc_jbuf_puts(b, "}\n");
}

void
comp_ipc_build_outputs_event(JsonBuf *b, const CompServer *s)
{
    b->len = 0;
    comp_ipc_jbuf_puts(b, "{\"event\":\"" COMP_IPC_EVENT_OUTPUTS_CHANGED "\",\"outputs\":");
    comp_ipc_append_outputs_array(b, s);
    comp_ipc_jbuf_puts(b, "}\n");
}

static void
emit_built_event(CompServer *s, uint32_t sub_bit, void (*builder)(JsonBuf *, const CompServer *))
{
    if (!comp_ipc_has_subscribers(s, sub_bit))
        return;
    void *tmp = talloc_new(s->ipc);
    JsonBuf b;
    comp_ipc_jbuf_init(&b, tmp);
    builder(&b, s);
    comp_ipc_broadcast_event(s, sub_bit, b.buf);
    talloc_free(tmp);
}

void comp_ipc_emit_workspaces(CompServer *s)       { emit_built_event(s, SUB_WORKSPACES, comp_ipc_build_workspaces_event); }
void comp_ipc_emit_windows(CompServer *s)          { emit_built_event(s, SUB_WINDOWS, comp_ipc_build_windows_event); }
void comp_ipc_emit_keyboard_layouts(CompServer *s) { emit_built_event(s, SUB_KEYBOARD_LAYOUTS, comp_ipc_build_keyboard_layouts_event); }
void comp_ipc_emit_outputs(CompServer *s)          { emit_built_event(s, SUB_OUTPUTS, comp_ipc_build_outputs_event); }

void
comp_ipc_emit_focus(CompServer *s, CompWinId focused_id)
{
    if (!comp_ipc_has_subscribers(s, SUB_WINDOWS))
        return;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"event\":\"" COMP_IPC_EVENT_WINDOW_FOCUS_CHANGED "\",\"id\":%llu}\n",
             (unsigned long long)focused_id);
    comp_ipc_broadcast_event(s, SUB_WINDOWS, buf);
}

void
comp_ipc_emit_touch_bezel_swipe(CompServer *s, const char *edge, double x_ratio)
{
    if (!comp_ipc_has_subscribers(s, SUB_TOUCH_GESTURES))
        return;
    void *tmp = talloc_new(s->ipc);
    JsonBuf b;
    comp_ipc_jbuf_init(&b, tmp);
    comp_ipc_jbuf_puts(&b, "{\"event\":\"" COMP_IPC_EVENT_TOUCH_BEZEL_SWIPE "\",\"edge\":");
    comp_ipc_jbuf_str(&b, edge ? edge : COMP_IPC_EDGE_BOTTOM);
    comp_ipc_jbuf_printf(&b, ",\"x_ratio\":%.4f}\n", x_ratio);
    comp_ipc_broadcast_event(s, SUB_TOUCH_GESTURES, b.buf);
    talloc_free(tmp);
}

void
comp_ipc_emit_screenshot_saved(CompServer *s, const char *path, bool copied)
{
    if (!comp_ipc_has_subscribers(s, SUB_SCREENSHOTS))
        return;
    void *tmp = talloc_new(s->ipc);
    JsonBuf b;
    comp_ipc_jbuf_init(&b, tmp);
    comp_ipc_jbuf_puts(&b, "{\"event\":\"" COMP_IPC_EVENT_SCREENSHOT_SAVED "\",\"path\":");
    comp_ipc_jbuf_str(&b, path ? path : "");
    comp_ipc_jbuf_printf(&b, ",\"copied\":%s}\n", copied ? "true" : "false");
    comp_ipc_broadcast_event(s, SUB_SCREENSHOTS, b.buf);
    talloc_free(tmp);
}

/* ───────────────────────── Config Inotify ───────────────────────── */

static int
handle_debounce_timer(void *data)
{
    CompServer *s = data;
    wlr_log(WLR_INFO, "hot-reloading compositor configuration");
    comp_server_reload_config(s);
    return 0;
}

static void
refresh_confd_watch(struct CompIpcState *ipc)
{
    if (!ipc || ipc->inotify_fd < 0 || !ipc->config_dir)
        return;
    DIR *d = opendir(ipc->config_dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", ipc->config_dir, de->d_name);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
            int wd = inotify_add_watch(ipc->inotify_fd, sub,
                                       IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_ATTRIB);
            if (strcmp(de->d_name, COMP_CFG_DIR_CONFD) == 0)
                ipc->wd_confd = wd;
        }
    }
    closedir(d);
}

static bool
is_comp_root_toml(const char *name)
{
    return strcmp(name, COMP_CFG_FILE_THEME) == 0 ||
           strcmp(name, COMP_CFG_FILE_SHELL) == 0 ||
           strcmp(name, COMP_CFG_FILE_COMP) == 0 ||
           strcmp(name, COMP_CFG_FILE_OUTPUTS) == 0 ||
           strcmp(name, COMP_CFG_FILE_BINDS) == 0 ||
           strcmp(name, COMP_CFG_FILE_RULES) == 0;
}

static int
handle_inotify_fd(int fd, uint32_t mask, void *data)
{
    (void)mask;
    CompServer *s = data;
    struct CompIpcState *ipc = s->ipc;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool trigger = false;

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        const char *p = buf;
        while (p < buf + n) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            if (ev->len > 0) {
                size_t nl = strlen(ev->name);
                if (nl > 5 && strcmp(ev->name + nl - 5, COMP_CFG_EXT_TOML) == 0) {
                    if (!ipc || ev->wd != ipc->wd_root || is_comp_root_toml(ev->name))
                        trigger = true;
                } else if ((ev->mask & IN_ISDIR) || strcmp(ev->name, COMP_CFG_DIR_CONFD) == 0) {
                    refresh_confd_watch(ipc);
                    trigger = true;
                }
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }

    if (trigger && ipc && ipc->debounce_timer)
        wl_event_source_timer_update(ipc->debounce_timer, 50);
    return 0;
}

void
comp_ipc_watch_init(CompServer *s, struct CompIpcState *ipc)
{
    if (!s || !ipc || !ipc->config_dir || s->opts.greeter_mode)
        return;
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0)
        return;
    ipc->inotify_fd = ifd;
    ipc->wd_root = inotify_add_watch(ifd, ipc->config_dir,
                                     IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_ATTRIB);
    refresh_confd_watch(ipc);
    ipc->debounce_timer = wl_event_loop_add_timer(s->loop, handle_debounce_timer, s);
    ipc->inotify_ev = wl_event_loop_add_fd(s->loop, ifd, WL_EVENT_READABLE, handle_inotify_fd, s);
}

void
comp_ipc_watch_finish(struct CompIpcState *ipc)
{
    if (!ipc)
        return;
    if (ipc->debounce_timer) {
        wl_event_source_remove(ipc->debounce_timer);
        ipc->debounce_timer = NULL;
    }
    if (ipc->inotify_ev) {
        wl_event_source_remove(ipc->inotify_ev);
        ipc->inotify_ev = NULL;
    }
    if (ipc->inotify_fd >= 0) {
        close(ipc->inotify_fd);
        ipc->inotify_fd = -1;
    }
}
