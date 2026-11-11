/* comp_ipc.c — $XDG_RUNTIME_DIR/kajo-comp.sock IPC server, event broadcast,
 * inotify config watcher (50 ms debounce), and atomic CompConfig hot-reload (Phase 2d).
 */
#include "comp_server.h"

#include "comp_log.h"
#include "core/comp_schema_keys.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <talloc.h>
#include <unistd.h>
#include <wlr/types/wlr_output.h>

#define IPC_MAX_FRAME 65536

enum {
    SUB_WORKSPACES       = 1u << 0,
    SUB_WINDOWS          = 1u << 1,
    SUB_KEYBOARD_LAYOUTS = 1u << 2,
    SUB_OUTPUTS          = 1u << 3,
    SUB_TOUCH_GESTURES   = 1u << 4,
    SUB_SCREENSHOTS      = 1u << 5,
    SUB_ALL              = 0x3fu,
};

typedef struct {
    struct wl_list          link;   /* CompIpcState.clients */
    CompServer             *server;
    int                     fd;
    struct wl_event_source *ev;
    uint32_t                subs;
    char                    rbuf[IPC_MAX_FRAME];
    size_t                  rlen;
    char                   *wbuf;
    size_t                  wlen;
    size_t                  wcap;
    bool                    closing, in_dispatch;
} CompIpcClient;

struct CompIpcState {
    CompServer             *server;
    char                   *socket_path;
    int                     listen_fd;
    struct wl_event_source *listen_ev;
    struct wl_list          clients;        /* CompIpcClient.link */

    char                   *config_dir;
    int                     inotify_fd;
    int                     wd_root;
    int                     wd_confd;
    struct wl_event_source *inotify_ev;
    struct wl_event_source *debounce_timer;
};

/* ───────────────────────── Minimal JSON Builder ───────────────────────── */

typedef struct {
    void   *ctx;
    char   *buf;
    size_t  len;
    size_t  cap;
} JsonBuf;

static void
jbuf_init(JsonBuf *b, void *talloc_ctx)
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

static void
jbuf_puts(JsonBuf *b, const char *s)
{
    if (s)
        jbuf_append_raw(b, s, strlen(s));
}

__attribute__((format(printf, 2, 3)))
static void
jbuf_printf(JsonBuf *b, const char *fmt, ...)
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

static void
jbuf_str(JsonBuf *b, const char *s)
{
    if (!s)
        s = "";
    jbuf_puts(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  jbuf_puts(b, "\\\""); break;
        case '\\': jbuf_puts(b, "\\\\"); break;
        case '\b': jbuf_puts(b, "\\b");  break;
        case '\f': jbuf_puts(b, "\\f");  break;
        case '\n': jbuf_puts(b, "\\n");  break;
        case '\r': jbuf_puts(b, "\\r");  break;
        case '\t': jbuf_puts(b, "\\t");  break;
        default:
            if (c < 0x20)
                jbuf_printf(b, "\\u%04x", (unsigned)c);
            else
                jbuf_append_raw(b, (const char *)p, 1);
            break;
        }
    }
    jbuf_puts(b, "\"");
}

/* ───────────────────────── Minimal JSON Parser ───────────────────────── */

typedef enum {
    JV_NULL,
    JV_BOOL,
    JV_NUMBER,
    JV_STRING,
    JV_ARRAY,
    JV_OBJECT,
} JValKind;

typedef struct JVal JVal;

typedef struct {
    char *key;
    JVal *val;
} JMember;

struct JVal {
    JValKind kind;
    union {
        bool     b;
        double   num;
        char    *str;
        struct {
            JVal  **items;
            size_t  n;
        } arr;
        struct {
            JMember *members;
            size_t   n;
        } obj;
    } u;
};

typedef struct {
    void       *ctx;
    const char *p;
    int         depth;
} JParser;

static void
jp_skip_ws(JParser *jp)
{
    while (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\r' || *jp->p == '\n')
        jp->p++;
}

static JVal *jp_parse_value(JParser *jp);

static char *
jp_parse_raw_string(JParser *jp)
{
    if (*jp->p != '"')
        return NULL;
    jp->p++;
    size_t cap = 64, len = 0;
    char *out = talloc_array(jp->ctx, char, (unsigned)cap);
    if (!out)
        return NULL;

    while (*jp->p && *jp->p != '"') {
        unsigned char c = (unsigned char)*jp->p++;
        if (c == '\\') {
            unsigned char esc = (unsigned char)*jp->p;
            if (!esc)
                return NULL;
            jp->p++;
            switch (esc) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                unsigned code = 0;
                for (int i = 0; i < 4; i++) {
                    char hc = *jp->p;
                    if (!isxdigit((unsigned char)hc))
                        return NULL;
                    jp->p++;
                    code = (code << 4) | (unsigned)(isdigit((unsigned char)hc)
                                                        ? hc - '0'
                                                        : (tolower((unsigned char)hc) - 'a' + 10));
                }
                c = (code < 0x80 && code > 0) ? (unsigned char)code : '?';
                break;
            }
            default:
                return NULL;
            }
        }
        if (len + 2 > cap) {
            cap *= 2;
            char *nb = talloc_realloc(jp->ctx, out, char, (unsigned)cap);
            if (!nb)
                return NULL;
            out = nb;
        }
        out[len++] = (char)c;
    }
    if (*jp->p != '"')
        return NULL;
    jp->p++;
    out[len] = '\0';
    return out;
}

static JVal *
jp_parse_value(JParser *jp)
{
    if (jp->depth > 32)
        return NULL;
    jp_skip_ws(jp);
    char c = *jp->p;
    if (!c)
        return NULL;

    if (c == '"') {
        char *s = jp_parse_raw_string(jp);
        JVal *v = s ? talloc_zero(jp->ctx, JVal) : NULL;
        if (!v) return NULL;
        v->kind = JV_STRING;
        v->u.str = s;
        return v;
    }
    if (c == '{') {
        jp->p++;
        JVal *v = talloc_zero(jp->ctx, JVal);
        if (!v) return NULL;
        v->kind = JV_OBJECT;
        jp_skip_ws(jp);
        if (*jp->p == '}') { jp->p++; return v; }
        size_t cap = 8;
        v->u.obj.members = talloc_array(v, JMember, (unsigned)cap);
        if (!v->u.obj.members) return NULL;
        jp->depth++;
        for (;;) {
            jp_skip_ws(jp);
            char *k = jp_parse_raw_string(jp);
            if (!k) return NULL;
            jp_skip_ws(jp);
            if (*jp->p != ':') return NULL;
            jp->p++;
            JVal *child = jp_parse_value(jp);
            if (!child) return NULL;
            if (v->u.obj.n == cap) {
                cap *= 2;
                JMember *nm = talloc_realloc(v, v->u.obj.members, JMember, (unsigned)cap);
                if (!nm) return NULL;
                v->u.obj.members = nm;
            }
            v->u.obj.members[v->u.obj.n++] = (JMember){ .key = k, .val = child };
            jp_skip_ws(jp);
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == '}') { jp->p++; jp->depth--; return v; }
            return NULL;
        }
    }
    if (c == '[') {
        jp->p++;
        JVal *v = talloc_zero(jp->ctx, JVal);
        if (!v) return NULL;
        v->kind = JV_ARRAY;
        jp_skip_ws(jp);
        if (*jp->p == ']') { jp->p++; return v; }
        size_t cap = 8;
        v->u.arr.items = talloc_array(v, JVal *, (unsigned)cap);
        if (!v->u.arr.items) return NULL;
        jp->depth++;
        for (;;) {
            JVal *elem = jp_parse_value(jp);
            if (!elem) return NULL;
            if (v->u.arr.n == cap) {
                cap *= 2;
                JVal **ni = talloc_realloc(v, v->u.arr.items, JVal *, (unsigned)cap);
                if (!ni) return NULL;
                v->u.arr.items = ni;
            }
            v->u.arr.items[v->u.arr.n++] = elem;
            jp_skip_ws(jp);
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == ']') { jp->p++; jp->depth--; return v; }
            return NULL;
        }
    }
    if (strncmp(jp->p, "true", 4) == 0 || strncmp(jp->p, "false", 5) == 0) {
        bool bval = (*jp->p == 't');
        jp->p += bval ? 4 : 5;
        JVal *v = talloc_zero(jp->ctx, JVal);
        if (!v) return NULL;
        v->kind = JV_BOOL;
        v->u.b = bval;
        return v;
    }
    if (strncmp(jp->p, "null", 4) == 0) {
        jp->p += 4;
        JVal *v = talloc_zero(jp->ctx, JVal);
        if (v) v->kind = JV_NULL;
        return v;
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *end = NULL;
        errno = 0;
        double d = strtod(jp->p, &end);
        if (end == jp->p || errno != 0 || !isfinite(d)) return NULL;
        jp->p = end;
        JVal *v = talloc_zero(jp->ctx, JVal);
        if (!v) return NULL;
        v->kind = JV_NUMBER;
        v->u.num = d;
        return v;
    }
    return NULL;
}

static const JVal *
jobj_get(const JVal *obj, const char *key)
{
    if (!obj || obj->kind != JV_OBJECT || !key)
        return NULL;
    for (size_t i = 0; i < obj->u.obj.n; i++) {
        if (strcmp(obj->u.obj.members[i].key, key) == 0)
            return obj->u.obj.members[i].val;
    }
    return NULL;
}

static const char *
jobj_str(const JVal *obj, const char *key)
{
    const JVal *v = jobj_get(obj, key);
    return (v && v->kind == JV_STRING) ? v->u.str : NULL;
}

static bool
jobj_num(const JVal *obj, const char *key, double *out)
{
    const JVal *v = jobj_get(obj, key);
    if (!v || v->kind != JV_NUMBER)
        return false;
    *out = v->u.num;
    return true;
}

/* ───────────────────────── Payload Serializers ───────────────────────── */

static void
append_outputs_array(JsonBuf *b, const CompServer *s)
{
    jbuf_puts(b, "[");
    bool first = true;
    const CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        const struct wlr_output *wo = o->wlr_output;
        if (!wo)
            continue;
        if (!first)
            jbuf_puts(b, ",");
        first = false;
        jbuf_puts(b, "{\"name\":");
        jbuf_str(b, wo->name ? wo->name : "unknown");
        jbuf_puts(b, ",\"make\":");
        jbuf_str(b, wo->make ? wo->make : "");
        jbuf_puts(b, ",\"model\":");
        jbuf_str(b, wo->model ? wo->model : "");
        jbuf_printf(b, ",\"active\":%s,\"width\":%d,\"height\":%d,\"refresh_mhz\":%d,\"scale\":%.4g,\"transform\":",
                    wo->enabled ? "true" : "false",
                    wo->width, wo->height, wo->refresh, (double)wo->scale);
        jbuf_str(b, comp_transform_to_str((CompTransform)wo->transform));
        jbuf_puts(b, ",\"vrr\":");
        jbuf_str(b, comp_vrr_policy_to_str(o->vrr_policy));
        jbuf_puts(b, ",\"modes\":[");
        bool first_m = true;
        const struct wlr_output_mode *m;
        wl_list_for_each(m, &wo->modes, link) {
            if (!first_m)
                jbuf_puts(b, ",");
            first_m = false;
            jbuf_printf(b, "{\"width\":%d,\"height\":%d,\"refresh_mhz\":%d,\"preferred\":%s}",
                        m->width, m->height, m->refresh, m->preferred ? "true" : "false");
        }
        jbuf_puts(b, "]}");
    }
    jbuf_puts(b, "]");
}

static void
append_workspaces_array(JsonBuf *b, const CompServer *s)
{
    jbuf_puts(b, "[");
    size_t n_ws = s->layout ? comp_layout_workspace_count(s->layout) : 1;
    size_t active_ws = s->layout ? comp_layout_active_workspace(s->layout) : 0;

    /* Check urgency per workspace */
    size_t n_wins = comp_window_list_info(s, NULL, 0);
    CompWindowInfo *wins = n_wins > 0 ? talloc_array(b->ctx, CompWindowInfo, (unsigned)n_wins) : NULL;
    if (wins)
        comp_window_list_info(s, wins, n_wins);

    for (size_t i = 0; i < n_ws; i++) {
        if (i > 0)
            jbuf_puts(b, ",");
        size_t wcount = s->layout ? comp_layout_workspace_window_count(s->layout, i) : 0;
        bool urgent = false;
        for (size_t k = 0; k < n_wins && wins; k++) {
            if (wins[k].workspace_idx == i + 1 && wins[k].is_urgent) {
                urgent = true;
                break;
            }
        }
        jbuf_printf(b, "{\"idx\":%zu,\"id\":%zu,\"is_active\":%s,\"is_urgent\":%s,\"window_count\":%zu}",
                    i + 1, i + 1,
                    (i == active_ws) ? "true" : "false",
                    urgent ? "true" : "false",
                    wcount);
    }
    jbuf_puts(b, "]");
}

static void
append_windows_array(JsonBuf *b, const CompServer *s)
{
    jbuf_puts(b, "[");
    size_t n_wins = comp_window_list_info(s, NULL, 0);
    CompWindowInfo *wins = n_wins > 0 ? talloc_array(b->ctx, CompWindowInfo, (unsigned)n_wins) : NULL;
    if (wins) {
        n_wins = comp_window_list_info(s, wins, n_wins);
        for (size_t i = 0; i < n_wins; i++) {
            if (i > 0)
                jbuf_puts(b, ",");
            jbuf_printf(b, "{\"id\":%llu,\"title\":", (unsigned long long)wins[i].id);
            jbuf_str(b, wins[i].title);
            jbuf_puts(b, ",\"app_id\":");
            jbuf_str(b, wins[i].app_id);
            jbuf_printf(b, ",\"workspace_idx\":%zu,\"is_focused\":%s,\"is_floating\":%s,\"is_fullscreen\":%s,\"is_urgent\":%s}",
                        wins[i].workspace_idx,
                        wins[i].is_focused ? "true" : "false",
                        wins[i].is_floating ? "true" : "false",
                        wins[i].is_fullscreen ? "true" : "false",
                        wins[i].is_urgent ? "true" : "false");
        }
    }
    jbuf_puts(b, "]");
}

static void
append_keyboard_layouts_fields(JsonBuf *b, const CompServer *s)
{
    const char *names[16];
    uint32_t cur = 0;
    size_t n = comp_input_get_keyboard_layouts(s, names, 16, &cur);
    if (n > 16)
        n = 16;
    jbuf_puts(b, "\"names\":[");
    for (size_t i = 0; i < n; i++) {
        if (i > 0)
            jbuf_puts(b, ",");
        jbuf_str(b, names[i]);
    }
    jbuf_printf(b, "],\"current_idx\":%u", cur);
}

/* ───────────────────────── Client Write Queue ───────────────────────── */

static int handle_client_fd(int fd, uint32_t mask, void *data);

static void
client_queue_line(CompIpcClient *c, const char *json_line)
{
    if (!c || c->fd < 0 || c->closing || !json_line)
        return;
    size_t len = strlen(json_line);
    bool add_nl = (len == 0 || json_line[len - 1] != '\n');
    size_t total = len + (add_nl ? 1 : 0);

    if (c->wlen == 0) {
        ssize_t wr = send(c->fd, json_line, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (wr == (ssize_t)len) {
            if (!add_nl)
                return;
            ssize_t wr2 = send(c->fd, "\n", 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (wr2 == 1)
                return;
            json_line = "\n";
            len = 0;
            total = 1;
        } else if (wr > 0) {
            json_line += wr;
            len -= (size_t)wr;
            total = len + (add_nl ? 1 : 0);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            c->closing = true;
            return;
        }
    }

    if (c->wlen + total > 1024 * 1024) {
        c->closing = true;
        return;
    }
    if (c->wlen + total > c->wcap) {
        size_t ncap = c->wcap ? c->wcap * 2 : 4096;
        while (ncap < c->wlen + total)
            ncap *= 2;
        char *nb = talloc_realloc(c, c->wbuf, char, (unsigned)ncap);
        if (!nb)
            return;
        c->wbuf = nb;
        c->wcap = ncap;
    }
    if (len > 0) {
        memcpy(c->wbuf + c->wlen, json_line, len);
        c->wlen += len;
    }
    if (add_nl)
        c->wbuf[c->wlen++] = '\n';

    if (c->ev)
        wl_event_source_fd_update(c->ev, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
}

static void
client_reply_ok(CompIpcClient *c)
{
    client_queue_line(c, "{\"ok\":null}\n");
}

static void
client_reply_error(CompIpcClient *c, const char *msg)
{
    void *tmp = talloc_new(c);
    JsonBuf b;
    jbuf_init(&b, tmp);
    jbuf_puts(&b, "{\"error\":");
    jbuf_str(&b, msg ? msg : "error");
    jbuf_puts(&b, "}\n");
    client_queue_line(c, b.buf);
    talloc_free(tmp);
}

/* ───────────────────────── Event Broadcast ───────────────────────── */

static void
broadcast_event(CompServer *s, uint32_t sub_bit, const char *line)
{
    if (!s || !s->ipc || !line)
        return;
    CompIpcClient *c, *tmp;
    wl_list_for_each_safe(c, tmp, &s->ipc->clients, link) {
        if (c->subs & sub_bit) {
            client_queue_line(c, line);
            if (c->closing && !c->in_dispatch)
                talloc_free(c);
        }
    }
}

static void
build_workspaces_event(JsonBuf *b, const CompServer *s)
{
    b->len = 0;
    jbuf_puts(b, "{\"event\":\"" COMP_IPC_EVENT_WORKSPACES_CHANGED "\",\"workspaces\":");
    append_workspaces_array(b, s);
    jbuf_puts(b, "}\n");
}

static void
build_windows_event(JsonBuf *b, const CompServer *s)
{
    b->len = 0;
    jbuf_puts(b, "{\"event\":\"" COMP_IPC_EVENT_WINDOWS_CHANGED "\",\"windows\":");
    append_windows_array(b, s);
    jbuf_puts(b, "}\n");
}

static void
build_keyboard_layouts_event(JsonBuf *b, const CompServer *s)
{
    b->len = 0;
    jbuf_puts(b, "{\"event\":\"" COMP_IPC_EVENT_KEYBOARD_LAYOUTS_CHANGED "\",");
    append_keyboard_layouts_fields(b, s);
    jbuf_puts(b, "}\n");
}

static void
build_outputs_event(JsonBuf *b, const CompServer *s)
{
    b->len = 0;
    jbuf_puts(b, "{\"event\":\"" COMP_IPC_EVENT_OUTPUTS_CHANGED "\",\"outputs\":");
    append_outputs_array(b, s);
    jbuf_puts(b, "}\n");
}

static void
emit_built_event(CompServer *s, uint32_t sub_bit, void (*builder)(JsonBuf *, const CompServer *))
{
    if (!s || !s->ipc || wl_list_empty(&s->ipc->clients))
        return;
    void *tmp = talloc_new(s->ipc);
    JsonBuf b;
    jbuf_init(&b, tmp);
    builder(&b, s);
    broadcast_event(s, sub_bit, b.buf);
    talloc_free(tmp);
}

void comp_ipc_emit_workspaces(CompServer *s)       { emit_built_event(s, SUB_WORKSPACES, build_workspaces_event); }
void comp_ipc_emit_windows(CompServer *s)          { emit_built_event(s, SUB_WINDOWS, build_windows_event); }
void comp_ipc_emit_keyboard_layouts(CompServer *s) { emit_built_event(s, SUB_KEYBOARD_LAYOUTS, build_keyboard_layouts_event); }
void comp_ipc_emit_outputs(CompServer *s)          { emit_built_event(s, SUB_OUTPUTS, build_outputs_event); }

void
comp_ipc_emit_focus(CompServer *s, CompWinId focused_id)
{
    if (!s || !s->ipc || wl_list_empty(&s->ipc->clients))
        return;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"event\":\"" COMP_IPC_EVENT_WINDOW_FOCUS_CHANGED "\",\"id\":%llu}\n",
             (unsigned long long)focused_id);
    broadcast_event(s, SUB_WINDOWS, buf);
}

void
comp_ipc_emit_touch_bezel_swipe(CompServer *s, const char *edge, double x_ratio)
{
    if (!s || !s->ipc || wl_list_empty(&s->ipc->clients))
        return;
    void *tmp = talloc_new(s->ipc);
    JsonBuf b;
    jbuf_init(&b, tmp);
    jbuf_puts(&b, "{\"event\":\"" COMP_IPC_EVENT_TOUCH_BEZEL_SWIPE "\",\"edge\":");
    jbuf_str(&b, edge ? edge : COMP_IPC_EDGE_BOTTOM);
    jbuf_printf(&b, ",\"x_ratio\":%.4f}\n", x_ratio);
    broadcast_event(s, SUB_TOUCH_GESTURES, b.buf);
    talloc_free(tmp);
}

void
comp_ipc_emit_screenshot_saved(CompServer *s, const char *path, bool copied)
{
    if (!s || !s->ipc || wl_list_empty(&s->ipc->clients))
        return;
    void *tmp = talloc_new(s->ipc);
    JsonBuf b;
    jbuf_init(&b, tmp);
    jbuf_puts(&b, "{\"event\":\"" COMP_IPC_EVENT_SCREENSHOT_SAVED "\",\"path\":");
    jbuf_str(&b, path ? path : "");
    jbuf_printf(&b, ",\"copied\":%s}\n", copied ? "true" : "false");
    broadcast_event(s, SUB_SCREENSHOTS, b.buf);
    talloc_free(tmp);
}

/* ───────────────────────── Request Dispatch ───────────────────────── */

typedef enum {
    IPC_ACT_FOCUS_WINDOW = 0,
    IPC_ACT_FOCUS_APP,
    IPC_ACT_SWITCH_WORKSPACE,
    IPC_ACT_SWITCHER_CONFIRM,
    IPC_ACT_SWITCHER_CANCEL,
    IPC_ACT_SWITCH_KEYBOARD_LAYOUT,
    IPC_ACT_SET_OUTPUT_TRANSFORM,
    IPC_ACT_SET_OUTPUT_CONFIG,
    IPC_ACT_SCREENSHOT_CONFIRM,
    IPC_ACT_SCREENSHOT_CANCEL,
} CompIpcExtraAction;

static const struct { const char *name; CompIpcExtraAction act; } k_ipc_extra_actions[] = {
    { COMP_IPC_ACT_FOCUS_WINDOW,           IPC_ACT_FOCUS_WINDOW },
    { COMP_IPC_ACT_FOCUS_APP,              IPC_ACT_FOCUS_APP },
    { COMP_IPC_ACT_SWITCH_WORKSPACE,       IPC_ACT_SWITCH_WORKSPACE },
    { COMP_IPC_ACT_SWITCHER_CONFIRM,       IPC_ACT_SWITCHER_CONFIRM },
    { COMP_IPC_ACT_SWITCHER_CANCEL,        IPC_ACT_SWITCHER_CANCEL },
    { COMP_IPC_ACT_SWITCH_KEYBOARD_LAYOUT, IPC_ACT_SWITCH_KEYBOARD_LAYOUT },
    { COMP_IPC_ACT_SET_OUTPUT_TRANSFORM,   IPC_ACT_SET_OUTPUT_TRANSFORM },
    { COMP_IPC_ACT_SET_OUTPUT_CONFIG,      IPC_ACT_SET_OUTPUT_CONFIG },
    { COMP_IPC_ACT_SCREENSHOT_CONFIRM,     IPC_ACT_SCREENSHOT_CONFIRM },
    { COMP_IPC_ACT_SCREENSHOT_CANCEL,      IPC_ACT_SCREENSHOT_CANCEL },
};

static bool
ipc_extra_action_from_name(const char *name, CompIpcExtraAction *out)
{
    for (size_t i = 0; i < sizeof(k_ipc_extra_actions) / sizeof(k_ipc_extra_actions[0]); i++) {
        if (strcmp(k_ipc_extra_actions[i].name, name) == 0) {
            *out = k_ipc_extra_actions[i].act;
            return true;
        }
    }
    return false;
}

static bool
handle_bind_kind_action_cmd(CompIpcClient *c, const JVal *req, CompBindKind kind)
{
    CompServer *s = c->server;
    switch (kind) {
    case COMP_BIND_FULLSCREEN_WINDOW:
        if (comp_layout_toggle_fullscreen(s->layout)) {
            comp_window_relayout(s);
            comp_ipc_emit_windows(s);
        }
        client_reply_ok(c);
        return true;

    case COMP_BIND_CLOSE_WINDOW: {
        double id_num = 0.0;
        CompWinId id = jobj_num(req, COMP_IPC_KEY_ID, &id_num) && id_num > 0.0 ? (CompWinId)id_num : 0;
        comp_window_close_id(s, id);
        client_reply_ok(c);
        return true;
    }

    case COMP_BIND_SET_COLUMN_WIDTH:
    case COMP_BIND_SET_WINDOW_HEIGHT: {
        const char *delta_s = jobj_str(req, COMP_KEY_DELTA);
        double d = 0.0;
        CompDeltaMode mode = COMP_DELTA_REL_PROP;
        if (!delta_s || !comp_binds_parse_delta(delta_s, &d, &mode)) {
            client_reply_error(c, "invalid or missing 'delta'");
            return true;
        }
        if (kind == COMP_BIND_SET_COLUMN_WIDTH)
            comp_window_resize_column(s, d, mode);
        else
            comp_window_resize_height(s, d, mode);
        client_reply_ok(c);
        return true;
    }

    case COMP_BIND_NEXT_WINDOW:
    case COMP_BIND_PREVIOUS_WINDOW: {
        const char *scope_s  = jobj_str(req, COMP_KEY_SCOPE);
        const char *filter_s = jobj_str(req, COMP_KEY_FILTER);
        const JVal *hold_v   = jobj_get(req, COMP_IPC_KEY_HOLD);
        bool hold = (hold_v && hold_v->kind == JV_BOOL && hold_v->u.b);
        CompSwitchScope scope = COMP_SWITCH_SCOPE_OUTPUT;
        if (scope_s)
            (void)comp_switch_scope_from_str(scope_s, &scope);
        bool same_app = (filter_s && strcmp(filter_s, COMP_VAL_APP_ID) == 0);
        comp_switcher_step(s, kind == COMP_BIND_NEXT_WINDOW, scope, same_app, hold ? COMP_MOD_ALT : 0);
        client_reply_ok(c);
        return true;
    }

    case COMP_BIND_SPAWN: {
        const JVal *cmd_arr = jobj_get(req, COMP_IPC_KEY_COMMAND);
        if (!cmd_arr || cmd_arr->kind != JV_ARRAY || cmd_arr->u.arr.n == 0) {
            client_reply_error(c, "spawn requires non-empty 'command' array");
            return true;
        }
        const char **argv = talloc_zero_array(c, const char *, (unsigned)(cmd_arr->u.arr.n + 1));
        if (!argv) {
            client_reply_error(c, "out of memory");
            return true;
        }
        for (size_t i = 0; i < cmd_arr->u.arr.n; i++) {
            const JVal *e = cmd_arr->u.arr.items[i];
            if (!e || e->kind != JV_STRING || (i == 0 && !*e->u.str)) {
                talloc_free(argv);
                client_reply_error(c, "spawn command elements must be strings");
                return true;
            }
            argv[i] = e->u.str;
        }
        if (!s->opts.greeter_mode)
            comp_spawn(s, argv);
        talloc_free(argv);
        client_reply_ok(c);
        return true;
    }

    case COMP_BIND_RELOAD_CONFIG:
        if (!comp_server_reload_config(s))
            client_reply_error(c, "config reload failed (syntax error; previous config retained)");
        else
            client_reply_ok(c);
        return true;

    case COMP_BIND_BREAK_SHORTCUTS_INHIBIT:
        comp_protocols_break_shortcuts_inhibit(s);
        client_reply_ok(c);
        return true;

    case COMP_BIND_SCREENSHOT:
        if (!comp_screenshot_interactive(s)) client_reply_error(c, "screenshot failed");
        else client_reply_ok(c);
        return true;

    case COMP_BIND_SCREENSHOT_WINDOW:
        if (!comp_screenshot_window(s)) client_reply_error(c, "screenshot-window failed (no focused window)");
        else client_reply_ok(c);
        return true;

    case COMP_BIND_SCREENSHOT_SCREEN:
        if (!comp_screenshot_screen(s)) client_reply_error(c, "screenshot-screen failed");
        else client_reply_ok(c);
        return true;

    case COMP_BIND_QUIT:
        client_reply_ok(c);
        if (!s->opts.greeter_mode)
            comp_server_terminate(s, 0);
        return true;

    case COMP_BIND_LAYOUT:
    default:
        return false;
    }
}

static void
handle_action_cmd(CompIpcClient *c, const JVal *req)
{
    CompServer *s = c->server;
    const char *name = jobj_str(req, COMP_IPC_KEY_NAME);
    if (!name || !*name) {
        client_reply_error(c, "action requires a 'name' field");
        return;
    }

    CompAction layout_act;
    if (comp_action_from_name(name, &layout_act)) {
        if (layout_act == COMP_ACT_TOGGLE_WINDOW_FLOATING) {
            CompBind b = { .kind = COMP_BIND_LAYOUT, .layout_action = layout_act };
            comp_window_handle_bind(s, &b);
        } else if (comp_layout_apply(s->layout, layout_act)) {
            comp_window_relayout(s);
            comp_ipc_emit_workspaces(s);
            comp_ipc_emit_windows(s);
        }
        client_reply_ok(c);
        return;
    }

    CompBindKind bind_kind;
    if (comp_bind_kind_from_name(name, &bind_kind) &&
        handle_bind_kind_action_cmd(c, req, bind_kind)) {
        return;
    }

    CompIpcExtraAction extra;
    if (!ipc_extra_action_from_name(name, &extra)) {
        client_reply_error(c, "unknown action name");
        return;
    }

    switch (extra) {
    case IPC_ACT_FOCUS_WINDOW: {
        double id_num = 0.0;
        if (!jobj_num(req, COMP_IPC_KEY_ID, &id_num) || id_num <= 0.0) {
            client_reply_error(c, "focus-window requires numeric 'id'");
            return;
        }
        comp_window_focus_id(s, (CompWinId)id_num);
        client_reply_ok(c);
        return;
    }
    case IPC_ACT_FOCUS_APP: {
        const char *app_id = jobj_str(req, COMP_IPC_KEY_APP_ID);
        if (!app_id || !*app_id) {
            client_reply_error(c, "focus-app requires 'app_id'");
            return;
        }
        comp_window_focus_app(s, app_id);
        client_reply_ok(c);
        return;
    }
    case IPC_ACT_SWITCH_WORKSPACE: {
        double idx_num = 0.0;
        if (!jobj_num(req, COMP_IPC_KEY_IDX, &idx_num) || idx_num < 1.0) {
            client_reply_error(c, "switch-workspace requires 'idx' >= 1");
            return;
        }
        if (comp_layout_focus_workspace_index(s->layout, (size_t)idx_num - 1)) {
            comp_window_relayout(s);
            comp_ipc_emit_workspaces(s);
            comp_ipc_emit_windows(s);
        }
        client_reply_ok(c);
        return;
    }
    case IPC_ACT_SWITCHER_CONFIRM: comp_switcher_confirm(s); client_reply_ok(c); return;
    case IPC_ACT_SWITCHER_CANCEL:  comp_switcher_cancel(s);  client_reply_ok(c); return;
    case IPC_ACT_SWITCH_KEYBOARD_LAYOUT: {
        double idx_num = -1.0;
        int idx = jobj_num(req, COMP_IPC_KEY_IDX, &idx_num) ? (int)idx_num : -1;
        comp_input_switch_keyboard_layout(s, idx);
        client_reply_ok(c);
        return;
    }
    case IPC_ACT_SET_OUTPUT_TRANSFORM: {
        CompTransform tf = COMP_TRANSFORM_NORMAL;
        bool tf_ok = comp_transform_from_str(jobj_str(req, COMP_KEY_TRANSFORM), &tf) &&
                     tf <= COMP_TRANSFORM_270;
        if (!tf_ok || !comp_output_configure(s, jobj_str(req, COMP_IPC_KEY_OUTPUT), NULL, -1.0, (int)tf, -1))
            client_reply_error(c, !tf_ok ? "invalid 'transform' (expected normal, 90, 180, or 270)"
                                         : "failed to configure output transform");
        else
            client_reply_ok(c);
        return;
    }
    case IPC_ACT_SET_OUTPUT_CONFIG: {
        const char *tf_str  = jobj_str(req, COMP_KEY_TRANSFORM);
        const char *vrr_str = jobj_str(req, COMP_KEY_VRR);
        double scale = -1.0;
        jobj_num(req, COMP_KEY_SCALE, &scale);
        CompTransform tf_val = COMP_TRANSFORM_NORMAL;
        CompVrrPolicy vrr_val = COMP_VRR_OFF;
        bool tf_ok  = !tf_str || (comp_transform_from_str(tf_str, &tf_val) && tf_val <= COMP_TRANSFORM_270);
        bool vrr_ok = !vrr_str || comp_vrr_policy_from_str(vrr_str, &vrr_val);
        if (!tf_ok || !vrr_ok ||
            !comp_output_configure(s, jobj_str(req, COMP_IPC_KEY_OUTPUT),
                                   jobj_str(req, COMP_KEY_MODE), scale,
                                   tf_str ? (int)tf_val : -1,
                                   vrr_str ? (int)vrr_val : -1))
            client_reply_error(c, "output configuration rejected");
        else
            client_reply_ok(c);
        return;
    }
    case IPC_ACT_SCREENSHOT_CONFIRM: comp_screenshot_confirm(s); client_reply_ok(c); return;
    case IPC_ACT_SCREENSHOT_CANCEL:  comp_screenshot_cancel(s);  client_reply_ok(c); return;
    }
}

typedef enum {
    IPC_CMD_GET_OUTPUTS = 0,
    IPC_CMD_GET_WORKSPACES,
    IPC_CMD_GET_WINDOWS,
    IPC_CMD_GET_KEYBOARD_LAYOUTS,
    IPC_CMD_DEBUG_MEM,
    IPC_CMD_SUBSCRIBE,
    IPC_CMD_ACTION,
} CompIpcCmd;

static const struct { const char *name; CompIpcCmd cmd; } k_ipc_cmds[] = {
    { COMP_IPC_CMD_GET_OUTPUTS,          IPC_CMD_GET_OUTPUTS },
    { COMP_IPC_CMD_GET_WORKSPACES,       IPC_CMD_GET_WORKSPACES },
    { COMP_IPC_CMD_GET_WINDOWS,          IPC_CMD_GET_WINDOWS },
    { COMP_IPC_CMD_GET_KEYBOARD_LAYOUTS, IPC_CMD_GET_KEYBOARD_LAYOUTS },
    { COMP_IPC_CMD_DEBUG_MEM,            IPC_CMD_DEBUG_MEM },
    { COMP_IPC_CMD_SUBSCRIBE,            IPC_CMD_SUBSCRIBE },
    { COMP_IPC_CMD_ACTION,               IPC_CMD_ACTION },
};

static bool
ipc_cmd_from_str(const char *s, CompIpcCmd *out)
{
    for (size_t i = 0; i < sizeof(k_ipc_cmds) / sizeof(k_ipc_cmds[0]); i++) {
        if (strcmp(k_ipc_cmds[i].name, s) == 0) {
            *out = k_ipc_cmds[i].cmd;
            return true;
        }
    }
    return false;
}

static const struct { const char *name; uint32_t bit; } k_ipc_sub_topics[] = {
    { COMP_IPC_SUB_WORKSPACES,       SUB_WORKSPACES },
    { COMP_IPC_SUB_WINDOWS,          SUB_WINDOWS },
    { COMP_IPC_SUB_KEYBOARD_LAYOUTS, SUB_KEYBOARD_LAYOUTS },
    { COMP_IPC_SUB_OUTPUTS,          SUB_OUTPUTS },
    { COMP_IPC_SUB_TOUCH_GESTURES,   SUB_TOUCH_GESTURES },
    { COMP_IPC_SUB_SCREENSHOTS,      SUB_SCREENSHOTS },
};

static void
handle_subscribe_cmd(CompIpcClient *c, const JVal *req, JsonBuf *b)
{
    CompServer *s = c->server;
    uint32_t mask = 0;
    const JVal *ev_arr = jobj_get(req, COMP_IPC_KEY_EVENTS);
    if (!ev_arr || ev_arr->kind != JV_ARRAY || ev_arr->u.arr.n == 0) {
        mask = SUB_ALL;
    } else {
        for (size_t i = 0; i < ev_arr->u.arr.n; i++) {
            const JVal *e = ev_arr->u.arr.items[i];
            if (!e || e->kind != JV_STRING)
                continue;
            for (size_t k = 0; k < sizeof(k_ipc_sub_topics) / sizeof(k_ipc_sub_topics[0]); k++) {
                if (strcmp(e->u.str, k_ipc_sub_topics[k].name) == 0) {
                    mask |= k_ipc_sub_topics[k].bit;
                    break;
                }
            }
        }
    }
    c->subs |= mask;
    client_reply_ok(c);

    /* Initial snapshot events per spec §5.3 */
    if (mask & SUB_WORKSPACES) {
        build_workspaces_event(b, s);
        client_queue_line(c, b->buf);
    }
    if (mask & SUB_WINDOWS) {
        build_windows_event(b, s);
        client_queue_line(c, b->buf);

        CompWinId fid = s->layout ? comp_layout_focused_window(s->layout) : 0;
        b->len = 0;
        jbuf_printf(b, "{\"event\":\"" COMP_IPC_EVENT_WINDOW_FOCUS_CHANGED "\",\"id\":%llu}\n",
                    (unsigned long long)fid);
        client_queue_line(c, b->buf);
    }
    if (mask & SUB_KEYBOARD_LAYOUTS) {
        build_keyboard_layouts_event(b, s);
        client_queue_line(c, b->buf);
    }
    if (mask & SUB_OUTPUTS) {
        build_outputs_event(b, s);
        client_queue_line(c, b->buf);
    }
}

static void
handle_client_line(CompIpcClient *c, const char *line)
{
    CompServer *s = c->server;
    void *tmp = talloc_new(c);
    JParser jp = { .ctx = tmp, .p = line };
    JVal *req = jp_parse_value(&jp);
    jp_skip_ws(&jp);
    if (!req || req->kind != JV_OBJECT || *jp.p != '\0') {
        client_reply_error(c, "invalid JSON request");
        talloc_free(tmp);
        return;
    }

    const char *cmd_str = jobj_str(req, COMP_IPC_KEY_CMD);
    if (!cmd_str) {
        client_reply_error(c, "missing 'cmd' field");
        talloc_free(tmp);
        return;
    }

    CompIpcCmd cmd;
    if (!ipc_cmd_from_str(cmd_str, &cmd)) {
        client_reply_error(c, "unknown cmd");
        talloc_free(tmp);
        return;
    }

    JsonBuf b;
    jbuf_init(&b, tmp);

    switch (cmd) {
    case IPC_CMD_GET_OUTPUTS:
        jbuf_puts(&b, "{\"ok\":{\"outputs\":");
        append_outputs_array(&b, s);
        jbuf_puts(&b, "}}\n");
        client_queue_line(c, b.buf);
        break;
    case IPC_CMD_GET_WORKSPACES:
        jbuf_puts(&b, "{\"ok\":{\"workspaces\":");
        append_workspaces_array(&b, s);
        jbuf_puts(&b, "}}\n");
        client_queue_line(c, b.buf);
        break;
    case IPC_CMD_GET_WINDOWS:
        jbuf_puts(&b, "{\"ok\":{\"windows\":");
        append_windows_array(&b, s);
        jbuf_puts(&b, "}}\n");
        client_queue_line(c, b.buf);
        break;
    case IPC_CMD_GET_KEYBOARD_LAYOUTS:
        jbuf_puts(&b, "{\"ok\":{");
        append_keyboard_layouts_fields(&b, s);
        jbuf_puts(&b, "}}\n");
        client_queue_line(c, b.buf);
        break;
    case IPC_CMD_DEBUG_MEM: {
        char *rep = NULL;
        size_t rep_len = 0;
        FILE *fp = open_memstream(&rep, &rep_len);
        if (fp) { talloc_report_full(s, fp); fclose(fp); }
        jbuf_puts(&b, "{\"ok\":{\"report\":");
        jbuf_str(&b, rep ? rep : "");
        jbuf_puts(&b, "}}\n");
        free(rep);
        client_queue_line(c, b.buf);
        break;
    }
    case IPC_CMD_SUBSCRIBE: handle_subscribe_cmd(c, req, &b); break;
    case IPC_CMD_ACTION:    handle_action_cmd(c, req);        break;
    }

    talloc_free(tmp);
}

static int
client_destructor(CompIpcClient *c)
{
    if (c->ev) {
        wl_event_source_remove(c->ev);
        c->ev = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    wl_list_remove(&c->link);
    return 0;
}

static int
handle_client_fd(int fd, uint32_t mask, void *data)
{
    CompIpcClient *c = data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        talloc_free(c);
        return 0;
    }
    if ((mask & WL_EVENT_WRITABLE) && c->wlen > 0) {
        ssize_t wr = send(fd, c->wbuf, c->wlen, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (wr > 0) {
            if ((size_t)wr < c->wlen)
                memmove(c->wbuf, c->wbuf + wr, c->wlen - (size_t)wr);
            c->wlen -= (size_t)wr;
        } else if (wr < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            talloc_free(c);
            return 0;
        }
        if (c->wlen == 0 && c->ev)
            wl_event_source_fd_update(c->ev, WL_EVENT_READABLE);
    }
    if (mask & WL_EVENT_READABLE) {
        if (c->rlen >= sizeof(c->rbuf) - 1) {
            client_reply_error(c, "frame exceeds 64 KB limit");
            talloc_free(c);
            return 0;
        }
        ssize_t rd = recv(fd, c->rbuf + c->rlen, sizeof(c->rbuf) - 1 - c->rlen, MSG_DONTWAIT);
        if (rd == 0) {
            talloc_free(c);
            return 0;
        }
        if (rd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                talloc_free(c);
            return 0;
        }
        c->rlen += (size_t)rd;
        c->rbuf[c->rlen] = '\0';

        char *start = c->rbuf;
        char *nl = NULL;
        c->in_dispatch = true;
        while (!c->closing && (nl = memchr(start, '\n', (size_t)(c->rbuf + c->rlen - start))) != NULL) {
            *nl = '\0';
            if (nl > start)
                handle_client_line(c, start);
            start = nl + 1;
        }
        c->in_dispatch = false;
        if (c->closing) {
            talloc_free(c);
            return 0;
        }
        size_t rem = (size_t)(c->rbuf + c->rlen - start);
        if (rem > 0 && start != c->rbuf)
            memmove(c->rbuf, start, rem);
        c->rlen = rem;
    }
    return 0;
}

static void
set_nonblock_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int fdflags = fcntl(fd, F_GETFD, 0);
    if (fdflags >= 0)
        (void)fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
}

static int
handle_ipc_accept(int fd, uint32_t mask, void *data)
{
    (void)mask;
    CompServer *s = data;
    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        set_nonblock_cloexec(cfd);
        CompIpcClient *c = talloc_zero(s->ipc, CompIpcClient);
        if (!c) {
            close(cfd);
            continue;
        }
        talloc_set_name_const(c, "CompIpcClient");
        c->server = s;
        c->fd     = cfd;
        wl_list_insert(&s->ipc->clients, &c->link);
        talloc_set_destructor(c, client_destructor);
        c->ev = wl_event_loop_add_fd(s->loop, cfd, WL_EVENT_READABLE, handle_client_fd, c);
        if (!c->ev) {
            talloc_free(c);
            continue;
        }
    }
    return 0;
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
                if (nl > 5 && strcmp(ev->name + nl - 5, COMP_CFG_EXT_TOML) == 0)
                    trigger = true;
                else if ((ev->mask & IN_ISDIR) || strcmp(ev->name, COMP_CFG_DIR_CONFD) == 0) {
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

static int
ipc_state_destructor(struct CompIpcState *ipc)
{
    CompIpcClient *c, *tmp;
    wl_list_for_each_safe(c, tmp, &ipc->clients, link) {
        talloc_free(c);
    }
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
    if (ipc->listen_ev) {
        wl_event_source_remove(ipc->listen_ev);
        ipc->listen_ev = NULL;
    }
    if (ipc->listen_fd >= 0) {
        close(ipc->listen_fd);
        ipc->listen_fd = -1;
    }
    if (ipc->socket_path && *ipc->socket_path)
        unlink(ipc->socket_path);
    return 0;
}

bool
comp_ipc_init(CompServer *s)
{
    if (!s)
        return false;

    struct CompIpcState *ipc = talloc_zero(s, struct CompIpcState);
    COMP_WARN_RETURN(ipc != NULL, false);
    talloc_set_name_const(ipc, "CompIpcState");
    ipc->server     = s;
    ipc->listen_fd  = -1;
    ipc->inotify_fd = -1;
    ipc->wd_root    = -1;
    ipc->wd_confd   = -1;
    wl_list_init(&ipc->clients);
    talloc_set_destructor(ipc, ipc_state_destructor);
    s->ipc = ipc;

    ipc->config_dir = comp_server_resolve_config_dir(ipc, &s->opts);

    /* Watch config directory via inotify on wl_event_loop */
    if (ipc->config_dir && !s->opts.greeter_mode) {
        int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ifd >= 0) {
            ipc->inotify_fd = ifd;
            ipc->wd_root = inotify_add_watch(ifd, ipc->config_dir,
                                             IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_ATTRIB);
            refresh_confd_watch(ipc);
            ipc->debounce_timer = wl_event_loop_add_timer(s->loop, handle_debounce_timer, s);
            ipc->inotify_ev = wl_event_loop_add_fd(s->loop, ifd, WL_EVENT_READABLE, handle_inotify_fd, s);
        }
    }

    if (s->opts.greeter_mode)
        return true;

    const char *env_sock = getenv(COMP_IPC_ENV_SOCKET);
    const char *xdg_run  = getenv("XDG_RUNTIME_DIR");
    if (env_sock && *env_sock)
        ipc->socket_path = talloc_strdup(ipc, env_sock);
    else if (xdg_run && *xdg_run)
        ipc->socket_path = talloc_asprintf(ipc, "%s/%s", xdg_run, COMP_IPC_SOCK_NAME);
    else
        ipc->socket_path = talloc_asprintf(ipc, "/tmp/kajo-comp-%d.sock", (int)getuid());

    if (!ipc->socket_path)
        return false;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(ipc->socket_path) >= sizeof(addr.sun_path)) {
        wlr_log(WLR_ERROR, "IPC socket path too long: %s", ipc->socket_path);
        return false;
    }
    strncpy(addr.sun_path, ipc->socket_path, sizeof(addr.sun_path) - 1);
    unlink(ipc->socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        wlr_log(WLR_ERROR, "could not create IPC socket: %s", strerror(errno));
        return false;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
        wlr_log(WLR_ERROR, "could not bind/listen on %s: %s", ipc->socket_path, strerror(errno));
        close(fd);
        return false;
    }

    ipc->listen_fd = fd;
    ipc->listen_ev = wl_event_loop_add_fd(s->loop, fd, WL_EVENT_READABLE, handle_ipc_accept, s);
    if (!ipc->listen_ev) {
        close(fd);
        ipc->listen_fd = -1;
        return false;
    }

    setenv(COMP_IPC_ENV_SOCKET, ipc->socket_path, 1);
    wlr_log(WLR_INFO, "kajo-comp IPC listening on %s", ipc->socket_path);
    return true;
}

void
comp_ipc_finish(CompServer *s)
{
    if (!s || !s->ipc)
        return;
    talloc_free(s->ipc);
    s->ipc = NULL;
}

void
comp_ipc_fuzz_bytes(CompServer *s, const uint8_t *data, size_t len)
{
    if (!s || !data || len == 0 || len >= IPC_MAX_FRAME)
        return;
    CompIpcClient *c = talloc_zero(s, CompIpcClient);
    if (!c)
        return;
    c->server = s;
    c->fd     = -1;
    wl_list_init(&c->link);
    memcpy(c->rbuf, data, len);
    c->rlen = len;
    c->rbuf[c->rlen] = '\0';

    char *start = c->rbuf;
    char *nl = NULL;
    while ((nl = memchr(start, '\n', (size_t)(c->rbuf + c->rlen - start))) != NULL) {
        *nl = '\0';
        if (nl > start)
            handle_client_line(c, start);
        start = nl + 1;
    }
    if (start < c->rbuf + c->rlen)
        handle_client_line(c, start);
    talloc_free(c);
}
