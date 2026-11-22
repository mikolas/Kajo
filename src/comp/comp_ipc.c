/* comp_ipc.c — $XDG_RUNTIME_DIR/kajo-comp.sock IPC server, client framing,
 * EventStream delivery, and request/action dispatch (Phase 2d).
 */
#include "comp_ipc_private.h"

#include "comp_log.h"
#include "core/comp_schema_keys.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <talloc.h>
#include <unistd.h>
#include <wlr/types/wlr_output.h>

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
    if (!v || v->kind != JV_NUMBER || !isfinite(v->u.num))
        return false;
    *out = v->u.num;
    return true;
}

static bool
jobj_win_id(const JVal *obj, const char *key, CompWinId *out)
{
    double d = 0.0;
    /* Max exact integer in IEEE-754 double is 2^53 (9007199254740992.0), which
     * covers 32-bit slot + 21-bit generation without float-cast UB. */
    if (!jobj_num(obj, key, &d) || d <= 0.0 || d > 9007199254740992.0)
        return false;
    *out = (CompWinId)d;
    return true;
}

static bool
jobj_ws_idx(const JVal *obj, const char *key, size_t *out)
{
    double d = 0.0;
    if (!jobj_num(obj, key, &d) || d < 1.0 || d > 4096.0)
        return false;
    *out = (size_t)d;
    return true;
}

static bool
jobj_i32_bounded(const JVal *obj, const char *key, int lo, int hi, int *out)
{
    double d = 0.0;
    if (!jobj_num(obj, key, &d) || d < (double)lo || d > (double)hi)
        return false;
    *out = (int)d;
    return true;
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
    comp_ipc_jbuf_init(&b, tmp);
    comp_ipc_jbuf_puts(&b, "{\"error\":");
    comp_ipc_jbuf_str(&b, msg ? msg : "error");
    comp_ipc_jbuf_puts(&b, "}\n");
    client_queue_line(c, b.buf);
    talloc_free(tmp);
}

/* ───────────────────────── Event Broadcast ───────────────────────── */

bool
comp_ipc_has_subscribers(const CompServer *s, uint32_t sub_bit)
{
    if (!s || !s->ipc || wl_list_empty(&s->ipc->clients))
        return false;
    const CompIpcClient *c;
    wl_list_for_each(c, &s->ipc->clients, link) {
        if (!c->closing && (c->subs & sub_bit))
            return true;
    }
    return false;
}

void
comp_ipc_broadcast_event(CompServer *s, uint32_t sub_bit, const char *line)
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
    IPC_ACT_POWER_ON_MONITORS,
    IPC_ACT_POWER_OFF_MONITORS,
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
    { COMP_IPC_ACT_POWER_ON_MONITORS,      IPC_ACT_POWER_ON_MONITORS },
    { COMP_IPC_ACT_POWER_OFF_MONITORS,     IPC_ACT_POWER_OFF_MONITORS },
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
        CompWinId id = 0;
        (void)jobj_win_id(req, COMP_IPC_KEY_ID, &id);
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
        if (!s->opts.greeter_mode && c->fd >= 0)
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
        if (!s->opts.greeter_mode && c->fd >= 0 && s->display)
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
        CompWinId id = 0;
        if (!jobj_win_id(req, COMP_IPC_KEY_ID, &id)) {
            client_reply_error(c, "focus-window requires numeric 'id'");
            return;
        }
        comp_window_focus_id(s, id);
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
        size_t idx = 0;
        if (!jobj_ws_idx(req, COMP_IPC_KEY_IDX, &idx)) {
            client_reply_error(c, "switch-workspace requires 'idx' >= 1");
            return;
        }
        if (comp_layout_focus_workspace_index(s->layout, idx - 1)) {
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
        int idx = -1;
        (void)jobj_i32_bounded(req, COMP_IPC_KEY_IDX, -1, 64, &idx);
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
    case IPC_ACT_POWER_ON_MONITORS:
        comp_output_apply_config(s);
        comp_server_schedule_frames(s);
        client_reply_ok(c);
        return;
    case IPC_ACT_POWER_OFF_MONITORS: {
        CompOutput *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (!o->wlr_output || !o->wlr_output->enabled)
                continue;
            struct wlr_output_state st;
            wlr_output_state_init(&st);
            wlr_output_state_set_enabled(&st, false);
            if (wlr_output_test_state(o->wlr_output, &st))
                wlr_output_commit_state(o->wlr_output, &st);
            wlr_output_state_finish(&st);
        }
        client_reply_ok(c);
        return;
    }
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
        comp_ipc_build_workspaces_event(b, s);
        client_queue_line(c, b->buf);
    }
    if (mask & SUB_WINDOWS) {
        comp_ipc_build_windows_event(b, s);
        client_queue_line(c, b->buf);

        CompWinId fid = s->layout ? comp_layout_focused_window(s->layout) : 0;
        b->len = 0;
        comp_ipc_jbuf_printf(b, "{\"event\":\"" COMP_IPC_EVENT_WINDOW_FOCUS_CHANGED "\",\"id\":%llu}\n",
                             (unsigned long long)fid);
        client_queue_line(c, b->buf);
    }
    if (mask & SUB_KEYBOARD_LAYOUTS) {
        comp_ipc_build_keyboard_layouts_event(b, s);
        client_queue_line(c, b->buf);
    }
    if (mask & SUB_OUTPUTS) {
        comp_ipc_build_outputs_event(b, s);
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
    comp_ipc_jbuf_init(&b, tmp);

    switch (cmd) {
    case IPC_CMD_GET_OUTPUTS:
        comp_ipc_jbuf_puts(&b, "{\"ok\":{\"outputs\":");
        comp_ipc_append_outputs_array(&b, s);
        comp_ipc_jbuf_puts(&b, "}}\n");
        client_queue_line(c, b.buf);
        break;
    case IPC_CMD_GET_WORKSPACES:
        comp_ipc_jbuf_puts(&b, "{\"ok\":{\"workspaces\":");
        comp_ipc_append_workspaces_array(&b, s);
        comp_ipc_jbuf_puts(&b, "}}\n");
        client_queue_line(c, b.buf);
        break;
    case IPC_CMD_GET_WINDOWS:
        comp_ipc_jbuf_puts(&b, "{\"ok\":{\"windows\":");
        comp_ipc_append_windows_array(&b, s);
        comp_ipc_jbuf_puts(&b, "}}\n");
        client_queue_line(c, b.buf);
        break;
    case IPC_CMD_GET_KEYBOARD_LAYOUTS:
        comp_ipc_jbuf_puts(&b, "{\"ok\":{");
        comp_ipc_append_keyboard_layouts_fields(&b, s);
        comp_ipc_jbuf_puts(&b, "}}\n");
        client_queue_line(c, b.buf);
        break;
    case IPC_CMD_DEBUG_MEM: {
        char *rep = NULL;
        size_t rep_len = 0;
        FILE *fp = open_memstream(&rep, &rep_len);
        if (fp) { talloc_report_full(s, fp); fclose(fp); }
        comp_ipc_jbuf_puts(&b, "{\"ok\":{\"report\":");
        comp_ipc_jbuf_str(&b, rep ? rep : "");
        comp_ipc_jbuf_puts(&b, "}}\n");
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
    if (c->server && c->server->ipc && c->server->ipc->n_clients > 0)
        c->server->ipc->n_clients--;
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
        if (!s->ipc || s->ipc->n_clients >= IPC_MAX_CLIENTS) {
            close(cfd);
            continue;
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
        s->ipc->n_clients++;
        talloc_set_destructor(c, client_destructor);
        c->ev = wl_event_loop_add_fd(s->loop, cfd, WL_EVENT_READABLE, handle_client_fd, c);
        if (!c->ev) {
            talloc_free(c);
            continue;
        }
    }
    return 0;
}

/* ───────────────────────── IPC Socket Lifecycle ───────────────────────── */

static int
ipc_state_destructor(struct CompIpcState *ipc)
{
    CompIpcClient *c, *tmp;
    wl_list_for_each_safe(c, tmp, &ipc->clients, link) {
        talloc_free(c);
    }
    comp_ipc_watch_finish(ipc);
    if (ipc->listen_ev) {
        wl_event_source_remove(ipc->listen_ev);
        ipc->listen_ev = NULL;
    }
    if (ipc->listen_fd >= 0) {
        close(ipc->listen_fd);
        ipc->listen_fd = -1;
    }
    if (ipc->socket_path && *ipc->socket_path && ipc->has_socket_stat) {
        struct stat st;
        if (stat(ipc->socket_path, &st) == 0 &&
            st.st_dev == ipc->socket_dev && st.st_ino == ipc->socket_ino) {
            unlink(ipc->socket_path);
        }
        ipc->has_socket_stat = false;
    }
    return 0;
}

static bool
bind_ipc_socket(struct CompIpcState *ipc)
{
    if (!ipc || !ipc->server || !ipc->socket_path || !*ipc->socket_path)
        return false;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(ipc->socket_path) >= sizeof(addr.sun_path)) {
        wlr_log(WLR_ERROR, "IPC socket path too long: %s", ipc->socket_path);
        return false;
    }
    strncpy(addr.sun_path, ipc->socket_path, sizeof(addr.sun_path) - 1);

    struct stat existing_st;
    if (stat(ipc->socket_path, &existing_st) == 0) {
        bool is_self = (ipc->listen_fd >= 0 && ipc->has_socket_stat &&
                        existing_st.st_dev == ipc->socket_dev &&
                        existing_st.st_ino == ipc->socket_ino);
        if (!is_self && S_ISSOCK(existing_st.st_mode)) {
            int probe_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (probe_fd >= 0) {
                int rc = connect(probe_fd, (struct sockaddr *)&addr, sizeof(addr));
                int err = errno;
                close(probe_fd);
                if (rc == 0 || err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS) {
                    wlr_log(WLR_ERROR,
                            "IPC socket %s is already active in another instance; refusing to unlink",
                            ipc->socket_path);
                    return false;
                }
            }
        }
        unlink(ipc->socket_path);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        wlr_log(WLR_ERROR, "could not create IPC socket: %s", strerror(errno));
        return false;
    }
    (void)fchmod(fd, S_IRUSR | S_IWUSR);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
        wlr_log(WLR_ERROR, "could not bind/listen on %s: %s", ipc->socket_path, strerror(errno));
        close(fd);
        return false;
    }
    (void)chmod(ipc->socket_path, S_IRUSR | S_IWUSR);

    struct stat bound_st;
    if (stat(ipc->socket_path, &bound_st) == 0) {
        ipc->socket_dev      = bound_st.st_dev;
        ipc->socket_ino      = bound_st.st_ino;
        ipc->has_socket_stat = true;
    } else {
        ipc->has_socket_stat = false;
    }

    if (ipc->listen_ev) {
        wl_event_source_remove(ipc->listen_ev);
        ipc->listen_ev = NULL;
    }
    if (ipc->listen_fd >= 0) {
        close(ipc->listen_fd);
        ipc->listen_fd = -1;
    }

    ipc->listen_fd = fd;
    ipc->listen_ev = wl_event_loop_add_fd(ipc->server->loop, fd, WL_EVENT_READABLE, handle_ipc_accept, ipc->server);
    if (!ipc->listen_ev) {
        close(fd);
        ipc->listen_fd       = -1;
        ipc->has_socket_stat = false;
        return false;
    }
    return true;
}

void
comp_ipc_ensure_socket(CompServer *s)
{
    if (!s || !s->ipc || s->opts.greeter_mode || !s->ipc->socket_path)
        return;
    struct stat path_st;
    if (s->ipc->listen_fd >= 0 && s->ipc->has_socket_stat &&
        stat(s->ipc->socket_path, &path_st) == 0 &&
        S_ISSOCK(path_st.st_mode) &&
        path_st.st_dev == s->ipc->socket_dev &&
        path_st.st_ino == s->ipc->socket_ino) {
        return;
    }
    if (bind_ipc_socket(s->ipc))
        wlr_log(WLR_INFO, "kajo-comp IPC socket restored at %s", s->ipc->socket_path);
}

const char *
comp_ipc_socket_path(const CompServer *s)
{
    if (!s || !s->ipc || s->ipc->listen_fd < 0 || !s->ipc->has_socket_stat)
        return NULL;
    return s->ipc->socket_path;
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
    comp_ipc_watch_init(s, ipc);

    if (s->opts.greeter_mode)
        return true;

    const char *env_sock = getenv(COMP_IPC_ENV_SOCKET);
    const char *xdg_run  = getenv("XDG_RUNTIME_DIR");
    if (env_sock && *env_sock) {
        ipc->socket_path = talloc_strdup(ipc, env_sock);
    } else if (s->nested_at_startup) {
        if (xdg_run && *xdg_run)
            ipc->socket_path = talloc_asprintf(ipc, "%s/kajo-comp-%d.sock", xdg_run, (int)getpid());
        else
            ipc->socket_path = talloc_asprintf(ipc, "/tmp/kajo-comp-%d-%d.sock", (int)getuid(), (int)getpid());
    } else if (xdg_run && *xdg_run) {
        ipc->socket_path = talloc_asprintf(ipc, "%s/%s", xdg_run, COMP_IPC_SOCK_NAME);
    } else {
        ipc->socket_path = talloc_asprintf(ipc, "/tmp/kajo-comp-%d.sock", (int)getuid());
    }

    if (!ipc->socket_path)
        return false;

    if (!bind_ipc_socket(ipc))
        return false;

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
