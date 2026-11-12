/* comp_test_client — minimal Wayland client used by the kajo-comp tests.
 *
 * Usage: comp-test-client globals IFACE...   exit 0 iff every IFACE is advertised
 *        comp-test-client churn N            N sequential connect/roundtrip/disconnect cycles
 *        comp-test-client windows N          map N xdg_toplevels (+ decoration + popup + fullscreen)
 * Exit status: 0 ok, 1 missing global / protocol error, 2 usage, 3 cannot connect.
 */
#include "ext-session-lock-v1-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

typedef struct {
    char **want;
    int    n_want;
    int   *seen;
} GlobalsCheck;

static void
on_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    (void)reg; (void)name; (void)version;
    GlobalsCheck *c = data;
    for (int i = 0; i < c->n_want; i++)
        if (strcmp(c->want[i], iface) == 0)
            c->seen[i] = 1;
}

static void
on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener k_registry = { on_global, on_global_remove };

static int
check_globals(char **want, int n)
{
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) {
        fprintf(stderr, "comp-test-client: cannot connect\n");
        return 3;
    }
    GlobalsCheck c = { want, n, calloc((size_t)n, sizeof(int)) };
    if (!c.seen) {
        wl_display_disconnect(d);
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &k_registry, &c);
    int rc = wl_display_roundtrip(d) < 0 ? 1 : 0;
    for (int i = 0; i < n && rc == 0; i++) {
        if (!c.seen[i]) {
            fprintf(stderr, "comp-test-client: missing global %s\n", want[i]);
            rc = 1;
        }
    }
    wl_registry_destroy(reg);
    wl_display_disconnect(d);
    free(c.seen);
    return rc;
}

static int
churn(int n)
{
    for (int i = 0; i < n; i++) {
        struct wl_display *d = wl_display_connect(NULL);
        if (!d)
            return 3;
        struct wl_registry *reg = wl_display_get_registry(d);
        int rc = wl_display_roundtrip(d);
        wl_registry_destroy(reg);
        wl_display_disconnect(d);
        if (rc < 0)
            return 1;
    }
    return 0;
}

/* ── Phase 2b window & popup test ── */

typedef struct {
    struct wl_compositor                     *compositor;
    struct xdg_wm_base                       *wm_base;
    struct zxdg_decoration_manager_v1        *dec_mgr;
    struct wp_single_pixel_buffer_manager_v1 *spb_mgr;
    struct wp_viewporter                     *viewporter;
} WinEnv;

typedef struct {
    struct wl_surface                 *surface;
    struct xdg_surface                *xdg_surface;
    struct xdg_toplevel               *toplevel;
    struct zxdg_toplevel_decoration_v1 *dec;
    struct wp_viewport                *viewport;
    int32_t                            cfg_w;
    int32_t                            cfg_h;
    uint32_t                           dec_mode;
    bool                               configured;
} TestWin;

typedef struct {
    struct wl_surface  *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup   *popup;
    bool                configured;
} TestPopup;

static void
wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener k_wm_base_listener = { wm_base_ping };

static void
on_win_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
    WinEnv *e = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        e->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        e->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, ver < 6 ? ver : 6);
        xdg_wm_base_add_listener(e->wm_base, &k_wm_base_listener, e);
    } else if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
        e->dec_mgr = wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
        e->spb_mgr = wl_registry_bind(reg, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        e->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    }
}

static const struct wl_registry_listener k_win_reg_listener = { on_win_global, on_global_remove };

static void
xdg_surf_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    TestWin *w = data;
    xdg_surface_ack_configure(xs, serial);
    w->configured = true;
}

static const struct xdg_surface_listener k_xdg_surf_listener = { xdg_surf_configure };

static void
toplevel_configure(void *data, struct xdg_toplevel *tl, int32_t width, int32_t height,
                   struct wl_array *states)
{
    (void)tl; (void)states;
    TestWin *w = data;
    w->cfg_w = width;
    w->cfg_h = height;
}

static void
toplevel_close(void *data, struct xdg_toplevel *tl)
{
    (void)data; (void)tl;
}

static void
toplevel_configure_bounds(void *data, struct xdg_toplevel *tl, int32_t w, int32_t h)
{
    (void)data; (void)tl; (void)w; (void)h;
}

static void
toplevel_wm_capabilities(void *data, struct xdg_toplevel *tl, struct wl_array *caps)
{
    (void)data; (void)tl; (void)caps;
}

static const struct xdg_toplevel_listener k_toplevel_listener = {
    toplevel_configure,
    toplevel_close,
    toplevel_configure_bounds,
    toplevel_wm_capabilities,
};

static void
dec_configure(void *data, struct zxdg_toplevel_decoration_v1 *dec, uint32_t mode)
{
    (void)dec;
    TestWin *w = data;
    w->dec_mode = mode;
}

static const struct zxdg_toplevel_decoration_v1_listener k_dec_listener = { dec_configure };

static void
popup_xdg_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    TestPopup *p = data;
    xdg_surface_ack_configure(xs, serial);
    p->configured = true;
}

static const struct xdg_surface_listener k_popup_xdg_listener = { popup_xdg_configure };

static void
popup_configure(void *data, struct xdg_popup *popup, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)data; (void)popup; (void)x; (void)y; (void)w; (void)h;
}

static void
popup_done(void *data, struct xdg_popup *popup)
{
    (void)data; (void)popup;
}

static void
popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token)
{
    (void)data; (void)popup; (void)token;
}

static const struct xdg_popup_listener k_popup_listener = {
    popup_configure,
    popup_done,
    popup_repositioned,
};

static int
test_windows(int n)
{
    if (n <= 0 || n > 64)
        return 2;

    struct wl_display *d = wl_display_connect(NULL);
    if (!d)
        return 3;

    WinEnv env = {0};
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &k_win_reg_listener, &env);
    if (wl_display_roundtrip(d) < 0 || !env.compositor || !env.wm_base ||
        !env.dec_mgr || !env.spb_mgr) {
        fprintf(stderr, "comp-test-client: missing window globals\n");
        wl_registry_destroy(reg);
        wl_display_disconnect(d);
        return 1;
    }

    TestWin *wins = calloc((size_t)n, sizeof(TestWin));
    if (!wins)
        return 1;

    /* Create N toplevels (window[n-1] is a child dialog of window[0] when n >= 2) */
    for (int i = 0; i < n; i++) {
        wins[i].surface = wl_compositor_create_surface(env.compositor);
        wins[i].xdg_surface = xdg_wm_base_get_xdg_surface(env.wm_base, wins[i].surface);
        xdg_surface_add_listener(wins[i].xdg_surface, &k_xdg_surf_listener, &wins[i]);
        wins[i].toplevel = xdg_surface_get_toplevel(wins[i].xdg_surface);
        xdg_toplevel_add_listener(wins[i].toplevel, &k_toplevel_listener, &wins[i]);
        xdg_toplevel_set_app_id(wins[i].toplevel, "org.kajo.TestClient");
        xdg_toplevel_set_title(wins[i].toplevel, "Kajo Test Window");
        if (i == n - 1 && n >= 2)
            xdg_toplevel_set_parent(wins[i].toplevel, wins[0].toplevel);

        wins[i].dec = zxdg_decoration_manager_v1_get_toplevel_decoration(env.dec_mgr, wins[i].toplevel);
        zxdg_toplevel_decoration_v1_add_listener(wins[i].dec, &k_dec_listener, &wins[i]);
        zxdg_toplevel_decoration_v1_set_mode(wins[i].dec, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

        wl_surface_commit(wins[i].surface);
    }

    /* Roundtrip for initial configure + server-side decoration mode */
    if (wl_display_roundtrip(d) < 0)
        return 1;

    struct wl_buffer *buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        env.spb_mgr, 0x111111ffu, 0x111111ffu, 0x111111ffu, 0xffffffffu);

    for (int i = 0; i < n; i++) {
        if (!wins[i].configured ||
            wins[i].dec_mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
            fprintf(stderr, "comp-test-client: window %d not initially configured (dec_mode=%u)\n",
                    i, wins[i].dec_mode);
            return 1;
        }
        wl_surface_attach(wins[i].surface, buf, 0, 0);
        wl_surface_damage_buffer(wins[i].surface, 0, 0, 1, 1);
        wl_surface_commit(wins[i].surface);
    }

    /* Roundtrip so compositor maps all windows, runs CompLayout + CompMotion, and sends size configures */
    if (wl_display_roundtrip(d) < 0 || wl_display_roundtrip(d) < 0)
        return 1;

    for (int i = 0; i < n; i++) {
        if (wins[i].cfg_w <= 0 || wins[i].cfg_h <= 0) {
            fprintf(stderr, "comp-test-client: window %d did not receive positive configure size (%dx%d)\n",
                    i, wins[i].cfg_w, wins[i].cfg_h);
            return 1;
        }
    }

    /* Exercise fullscreen + maximize + popup on wins[0] */
    xdg_toplevel_set_fullscreen(wins[0].toplevel, NULL);
    xdg_toplevel_set_maximized(wins[0].toplevel);

    struct xdg_positioner *pos = xdg_wm_base_create_positioner(env.wm_base);
    xdg_positioner_set_size(pos, 120, 80);
    xdg_positioner_set_anchor_rect(pos, 10, 10, 50, 20);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);

    TestPopup pop = {0};
    pop.surface = wl_compositor_create_surface(env.compositor);
    pop.xdg_surface = xdg_wm_base_get_xdg_surface(env.wm_base, pop.surface);
    xdg_surface_add_listener(pop.xdg_surface, &k_popup_xdg_listener, &pop);
    pop.popup = xdg_surface_get_popup(pop.xdg_surface, wins[0].xdg_surface, pos);
    xdg_popup_add_listener(pop.popup, &k_popup_listener, &pop);
    xdg_positioner_destroy(pos);
    wl_surface_commit(pop.surface);

    if (wl_display_roundtrip(d) < 0 || !pop.configured) {
        fprintf(stderr, "comp-test-client: popup did not configure\n");
        return 1;
    }

    xdg_popup_destroy(pop.popup);
    xdg_surface_destroy(pop.xdg_surface);
    wl_surface_destroy(pop.surface);

    xdg_toplevel_unset_fullscreen(wins[0].toplevel);
    xdg_toplevel_unset_maximized(wins[0].toplevel);
    if (wl_display_roundtrip(d) < 0)
        return 1;

    for (int i = n - 1; i >= 0; i--) {
        zxdg_toplevel_decoration_v1_destroy(wins[i].dec);
        xdg_toplevel_destroy(wins[i].toplevel);
        xdg_surface_destroy(wins[i].xdg_surface);
        wl_surface_destroy(wins[i].surface);
    }
    wl_buffer_destroy(buf);
    wl_display_roundtrip(d);

    if (env.viewporter)
        wp_viewporter_destroy(env.viewporter);
    wp_single_pixel_buffer_manager_v1_destroy(env.spb_mgr);
    zxdg_decoration_manager_v1_destroy(env.dec_mgr);
    xdg_wm_base_destroy(env.wm_base);
    wl_compositor_destroy(env.compositor);
    wl_registry_destroy(reg);
    wl_display_disconnect(d);
    free(wins);
    return 0;
}

/* ── Phase 2c layer-shell & session-lock test ── */

typedef struct {
    struct wl_compositor                     *compositor;
    struct wl_output                         *output;
    struct xdg_wm_base                       *wm_base;
    struct zwlr_layer_shell_v1               *layer_shell;
    struct ext_session_lock_manager_v1       *lock_mgr;
    struct wp_single_pixel_buffer_manager_v1 *spb_mgr;
    struct wp_viewporter                     *viewporter;
} ShellEnv;

typedef struct {
    uint32_t width;
    uint32_t height;
    bool     configured;
} LayerState;

typedef struct {
    uint32_t width;
    uint32_t height;
    bool     locked;
    bool     finished;
    bool     surf_configured;
} LockState;

static void
on_shell_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
    ShellEnv *e = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        e->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && !e->output) {
        e->output = wl_registry_bind(reg, name, &wl_output_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        e->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, ver < 6 ? ver : 6);
        xdg_wm_base_add_listener(e->wm_base, &k_wm_base_listener, e);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        e->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
        e->lock_mgr = wl_registry_bind(reg, name, &ext_session_lock_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
        e->spb_mgr = wl_registry_bind(reg, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        e->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    }
}

static const struct wl_registry_listener k_shell_reg_listener = { on_shell_global, on_global_remove };

static void
layer_surf_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
                     uint32_t width, uint32_t height)
{
    LayerState *st = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    st->width = width;
    st->height = height;
    st->configured = true;
}

static void
layer_surf_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
    (void)data; (void)ls;
}

static const struct zwlr_layer_surface_v1_listener k_layer_surf_listener = {
    layer_surf_configure,
    layer_surf_closed,
};

static void
lock_locked(void *data, struct ext_session_lock_v1 *lock)
{
    (void)lock;
    LockState *st = data;
    st->locked = true;
}

static void
lock_finished(void *data, struct ext_session_lock_v1 *lock)
{
    (void)lock;
    LockState *st = data;
    st->finished = true;
}

static const struct ext_session_lock_v1_listener k_lock_listener = {
    lock_locked,
    lock_finished,
};

static void
lock_surf_configure(void *data, struct ext_session_lock_surface_v1 *ls, uint32_t serial,
                    uint32_t width, uint32_t height)
{
    LockState *st = data;
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    st->width = width;
    st->height = height;
    st->surf_configured = true;
}

static const struct ext_session_lock_surface_v1_listener k_lock_surf_listener = {
    lock_surf_configure,
};

static int
test_shell(void)
{
    struct wl_display *d = wl_display_connect(NULL);
    if (!d)
        return 3;

    ShellEnv env = {0};
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &k_shell_reg_listener, &env);
    if (wl_display_roundtrip(d) < 0 || !env.compositor || !env.output ||
        !env.wm_base || !env.layer_shell || !env.lock_mgr || !env.spb_mgr || !env.viewporter) {
        fprintf(stderr, "comp-test-client: missing shell globals\n");
        return 1;
    }

    struct wl_buffer *buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        env.spb_mgr, 0x111111ffu, 0x111111ffu, 0x111111ffu, 0xffffffffu);

    /* 1. Map a tiled window and record its full-height configure */
    TestWin win = {0};
    win.surface = wl_compositor_create_surface(env.compositor);
    win.xdg_surface = xdg_wm_base_get_xdg_surface(env.wm_base, win.surface);
    xdg_surface_add_listener(win.xdg_surface, &k_xdg_surf_listener, &win);
    win.toplevel = xdg_surface_get_toplevel(win.xdg_surface);
    xdg_toplevel_add_listener(win.toplevel, &k_toplevel_listener, &win);
    wl_surface_commit(win.surface);
    if (wl_display_roundtrip(d) < 0 || !win.configured)
        return 1;
    wl_surface_attach(win.surface, buf, 0, 0);
    wl_surface_damage_buffer(win.surface, 0, 0, 1, 1);
    wl_surface_commit(win.surface);
    if (wl_display_roundtrip(d) < 0 || wl_display_roundtrip(d) < 0)
        return 1;
    int32_t base_h = win.cfg_h;
    if (base_h <= 0) {
        fprintf(stderr, "comp-test-client: invalid initial window height %d\n", base_h);
        return 1;
    }

    /* 2. Map a top panel layer surface with exclusive_zone = 36 */
    LayerState lst = {0};
    struct wl_surface *lsurf_wl = wl_compositor_create_surface(env.compositor);
    struct zwlr_layer_surface_v1 *lsurf = zwlr_layer_shell_v1_get_layer_surface(
        env.layer_shell, lsurf_wl, env.output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "kajo-panel");
    zwlr_layer_surface_v1_add_listener(lsurf, &k_layer_surf_listener, &lst);
    zwlr_layer_surface_v1_set_size(lsurf, 0, 36);
    zwlr_layer_surface_v1_set_anchor(lsurf,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(lsurf, 36);
    wl_surface_commit(lsurf_wl);
    if (wl_display_roundtrip(d) < 0 || !lst.configured || lst.height != 36) {
        fprintf(stderr, "comp-test-client: layer surface failed initial configure (h=%u)\n", lst.height);
        return 1;
    }
    wl_surface_attach(lsurf_wl, buf, 0, 0);
    wl_surface_damage_buffer(lsurf_wl, 0, 0, 1, 1);
    wl_surface_commit(lsurf_wl);
    if (wl_display_roundtrip(d) < 0 || wl_display_roundtrip(d) < 0)
        return 1;

    if (win.cfg_h != base_h - 36) {
        fprintf(stderr, "comp-test-client: exclusive_zone=36 did not shrink window height (%d -> %d)\n",
                base_h, win.cfg_h);
        return 1;
    }

    /* 3. Lock session via ext-session-lock-v1, attach lock surface buffer, then unlock */
    LockState lock_st = {0};
    struct ext_session_lock_v1 *lock = ext_session_lock_manager_v1_lock(env.lock_mgr);
    ext_session_lock_v1_add_listener(lock, &k_lock_listener, &lock_st);
    struct wl_surface *lock_wl = wl_compositor_create_surface(env.compositor);
    struct ext_session_lock_surface_v1 *lock_surf =
        ext_session_lock_v1_get_lock_surface(lock, lock_wl, env.output);
    ext_session_lock_surface_v1_add_listener(lock_surf, &k_lock_surf_listener, &lock_st);
    if (wl_display_roundtrip(d) < 0 || !lock_st.locked || !lock_st.surf_configured ||
        lock_st.width == 0 || lock_st.height == 0) {
        fprintf(stderr, "comp-test-client: session lock failed (locked=%d surf_cfg=%d %ux%u)\n",
                lock_st.locked, lock_st.surf_configured, lock_st.width, lock_st.height);
        return 1;
    }
    struct wp_viewport *lock_vp = wp_viewporter_get_viewport(env.viewporter, lock_wl);
    wp_viewport_set_destination(lock_vp, (int32_t)lock_st.width, (int32_t)lock_st.height);
    wl_surface_attach(lock_wl, buf, 0, 0);
    wl_surface_damage_buffer(lock_wl, 0, 0, 1, 1);
    wl_surface_commit(lock_wl);
    if (wl_display_roundtrip(d) < 0)
        return 1;

    wp_viewport_destroy(lock_vp);
    ext_session_lock_surface_v1_destroy(lock_surf);
    wl_surface_destroy(lock_wl);
    ext_session_lock_v1_unlock_and_destroy(lock);
    if (wl_display_roundtrip(d) < 0)
        return 1;

    zwlr_layer_surface_v1_destroy(lsurf);
    wl_surface_destroy(lsurf_wl);
    xdg_toplevel_destroy(win.toplevel);
    xdg_surface_destroy(win.xdg_surface);
    wl_surface_destroy(win.surface);
    wl_buffer_destroy(buf);
    wl_display_roundtrip(d);

    wp_viewporter_destroy(env.viewporter);
    wp_single_pixel_buffer_manager_v1_destroy(env.spb_mgr);
    ext_session_lock_manager_v1_destroy(env.lock_mgr);
    zwlr_layer_shell_v1_destroy(env.layer_shell);
    xdg_wm_base_destroy(env.wm_base);
    wl_output_destroy(env.output);
    wl_compositor_destroy(env.compositor);
    wl_registry_destroy(reg);
    wl_display_disconnect(d);
    return 0;
}

static int
test_hold_windows(int n, const char *ready_file)
{
    if (n <= 0 || n > 16 || !ready_file)
        return 2;

    struct wl_display *d = wl_display_connect(NULL);
    if (!d)
        return 3;

    WinEnv env = {0};
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &k_win_reg_listener, &env);
    if (wl_display_roundtrip(d) < 0 || !env.compositor || !env.wm_base ||
        !env.dec_mgr || !env.spb_mgr || !env.viewporter) {
        return 1;
    }

    TestWin *wins = calloc((size_t)n, sizeof(TestWin));
    if (!wins)
        return 1;

    struct wl_buffer *buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        env.spb_mgr, 0x224488ffu, 0x3366ccffu, 0x4488ffffu, 0xffffffffu);

    for (int i = 0; i < n; i++) {
        char title[64];
        snprintf(title, sizeof(title), "Kajo Test Window %d", i + 1);
        wins[i].surface = wl_compositor_create_surface(env.compositor);
        wins[i].viewport = wp_viewporter_get_viewport(env.viewporter, wins[i].surface);
        wins[i].xdg_surface = xdg_wm_base_get_xdg_surface(env.wm_base, wins[i].surface);
        xdg_surface_add_listener(wins[i].xdg_surface, &k_xdg_surf_listener, &wins[i]);
        wins[i].toplevel = xdg_surface_get_toplevel(wins[i].xdg_surface);
        xdg_toplevel_add_listener(wins[i].toplevel, &k_toplevel_listener, &wins[i]);
        xdg_toplevel_set_app_id(wins[i].toplevel, "org.kajo.TestClient");
        xdg_toplevel_set_title(wins[i].toplevel, title);
        wins[i].dec = zxdg_decoration_manager_v1_get_toplevel_decoration(env.dec_mgr, wins[i].toplevel);
        zxdg_toplevel_decoration_v1_add_listener(wins[i].dec, &k_dec_listener, &wins[i]);
        wl_surface_commit(wins[i].surface);
    }

    if (wl_display_roundtrip(d) < 0)
        return 1;

    for (int i = 0; i < n; i++) {
        int32_t w = wins[i].cfg_w > 0 ? wins[i].cfg_w : 400;
        int32_t h = wins[i].cfg_h > 0 ? wins[i].cfg_h : 300;
        wp_viewport_set_destination(wins[i].viewport, w, h);
        wl_surface_attach(wins[i].surface, buf, 0, 0);
        wl_surface_damage_buffer(wins[i].surface, 0, 0, 1, 1);
        wl_surface_commit(wins[i].surface);
    }

    if (wl_display_roundtrip(d) < 0 || wl_display_roundtrip(d) < 0)
        return 1;

    for (int i = 0; i < n; i++) {
        if (wins[i].cfg_w > 0 && wins[i].cfg_h > 0) {
            wp_viewport_set_destination(wins[i].viewport, wins[i].cfg_w, wins[i].cfg_h);
            wl_surface_commit(wins[i].surface);
        }
    }
    if (wl_display_roundtrip(d) < 0)
        return 1;

    FILE *fp = fopen(ready_file, "w");
    if (fp) {
        fputs("ready\n", fp);
        fclose(fp);
    }

    while (access(ready_file, F_OK) == 0) {
        if (wl_display_dispatch_pending(d) < 0 || wl_display_flush(d) < 0)
            break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000L };
        nanosleep(&ts, NULL);
    }

    for (int i = n - 1; i >= 0; i--) {
        wp_viewport_destroy(wins[i].viewport);
        zxdg_toplevel_decoration_v1_destroy(wins[i].dec);
        xdg_toplevel_destroy(wins[i].toplevel);
        xdg_surface_destroy(wins[i].xdg_surface);
        wl_surface_destroy(wins[i].surface);
    }
    wl_buffer_destroy(buf);
    wl_display_roundtrip(d);

    wp_viewporter_destroy(env.viewporter);
    wp_single_pixel_buffer_manager_v1_destroy(env.spb_mgr);
    zxdg_decoration_manager_v1_destroy(env.dec_mgr);
    xdg_wm_base_destroy(env.wm_base);
    wl_compositor_destroy(env.compositor);
    wl_registry_destroy(reg);
    wl_display_disconnect(d);
    free(wins);
    return 0;
}

typedef struct {
    struct wl_compositor                     *compositor;
    struct wl_seat                           *seat;
    struct xdg_wm_base                       *wm_base;
    struct wp_single_pixel_buffer_manager_v1 *spb_mgr;
    struct wp_viewporter                     *viewporter;
    struct zwp_virtual_keyboard_manager_v1   *vkbd_mgr;
    struct zwp_input_method_manager_v2       *im_mgr;
} RogueEnv;

static void
on_rogue_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
    RogueEnv *e = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        e->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        e->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        e->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, ver < 6 ? ver : 6);
        xdg_wm_base_add_listener(e->wm_base, &k_wm_base_listener, e);
    } else if (strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
        e->spb_mgr = wl_registry_bind(reg, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        e->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    } else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        e->vkbd_mgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (strcmp(iface, zwp_input_method_manager_v2_interface.name) == 0) {
        e->im_mgr = wl_registry_bind(reg, name, &zwp_input_method_manager_v2_interface, 1);
    }
}

static const struct wl_registry_listener k_rogue_reg_listener = { on_rogue_global, on_global_remove };

static int
test_rogue(const char *ready_file)
{
    if (!ready_file)
        return 2;

    struct wl_display *d = wl_display_connect(NULL);
    if (!d)
        return 3;

    RogueEnv env = {0};
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &k_rogue_reg_listener, &env);
    if (wl_display_roundtrip(d) < 0 || !env.compositor || !env.seat || !env.wm_base ||
        !env.spb_mgr || !env.viewporter || !env.vkbd_mgr || !env.im_mgr) {
        return 1;
    }

    struct wl_buffer *buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        env.spb_mgr, 0x112233ffu, 0x445566ffu, 0x778899ffu, 0xffffffffu);

    /* 1. Rapid rogue surface churn: 25 toplevels created and immediately destroyed mid-flight */
    for (int i = 0; i < 25; i++) {
        struct wl_surface *s = wl_compositor_create_surface(env.compositor);
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(env.wm_base, s);
        struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
        xdg_toplevel_set_app_id(top, "org.kajo.RogueChurn");
        xdg_toplevel_set_title(top, "Transient");
        if (i % 2 == 0)
            wl_surface_commit(s);
        xdg_toplevel_destroy(top);
        xdg_surface_destroy(xs);
        wl_surface_destroy(s);
    }
    if (wl_display_roundtrip(d) < 0)
        return 1;

    /* 2. Virtual keyboard + input-method v2 exercise */
    struct zwp_virtual_keyboard_v1 *vkbd =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(env.vkbd_mgr, env.seat);
    struct zwp_input_method_v2 *im =
        zwp_input_method_manager_v2_get_input_method(env.im_mgr, env.seat);

    struct xkb_context *xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *kmap = xctx ? xkb_keymap_new_from_names(xctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    char *kmap_str = kmap ? xkb_keymap_get_as_string(kmap, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    if (kmap_str) {
        size_t klen = strlen(kmap_str) + 1;
        char tmpl[] = "/tmp/kajo-vkbd-XXXXXX";
        int kfd = mkstemp(tmpl);
        if (kfd >= 0) {
            unlink(tmpl);
            if (write(kfd, kmap_str, klen) == (ssize_t)klen) {
                zwp_virtual_keyboard_v1_keymap(vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, kfd, (uint32_t)klen);
                zwp_virtual_keyboard_v1_modifiers(vkbd, 0, 0, 0, 0);
                zwp_virtual_keyboard_v1_key(vkbd, 10, 30 /* KEY_A */, WL_KEYBOARD_KEY_STATE_PRESSED);
                zwp_virtual_keyboard_v1_key(vkbd, 20, 30 /* KEY_A */, WL_KEYBOARD_KEY_STATE_RELEASED);
            }
            close(kfd);
        }
        free(kmap_str);
    }
    if (kmap) xkb_keymap_unref(kmap);
    if (xctx) xkb_context_unref(xctx);

    zwp_input_method_v2_commit_string(im, "hei");
    zwp_input_method_v2_commit(im, 1);

    /* 3. Map two persistent windows:
     *    - win[0]: app_id="firefox", starts title="Browser" (tiled), then mutates to "Picture-in-Picture"
     *    - win[1]: app_id="org.kajo.Oversized", sets a 8192-byte title with UTF-8 'ä' (0xC3 0xA4) at 511..512 */
    TestWin wins[2] = {0};
    for (int i = 0; i < 2; i++) {
        wins[i].surface = wl_compositor_create_surface(env.compositor);
        wins[i].viewport = wp_viewporter_get_viewport(env.viewporter, wins[i].surface);
        wins[i].xdg_surface = xdg_wm_base_get_xdg_surface(env.wm_base, wins[i].surface);
        xdg_surface_add_listener(wins[i].xdg_surface, &k_xdg_surf_listener, &wins[i]);
        wins[i].toplevel = xdg_surface_get_toplevel(wins[i].xdg_surface);
        xdg_toplevel_add_listener(wins[i].toplevel, &k_toplevel_listener, &wins[i]);
        if (i == 0) {
            xdg_toplevel_set_app_id(wins[i].toplevel, "firefox");
            xdg_toplevel_set_title(wins[i].toplevel, "Browser");
        } else {
            xdg_toplevel_set_app_id(wins[i].toplevel, "org.kajo.Oversized");
            xdg_toplevel_set_title(wins[i].toplevel, "Short");
        }
        wl_surface_commit(wins[i].surface);
    }
    if (wl_display_roundtrip(d) < 0)
        return 1;

    for (int i = 0; i < 2; i++) {
        wp_viewport_set_destination(wins[i].viewport, 400, 300);
        wl_surface_attach(wins[i].surface, buf, 0, 0);
        wl_surface_damage_buffer(wins[i].surface, 0, 0, 1, 1);
        wl_surface_commit(wins[i].surface);
    }
    if (wl_display_roundtrip(d) < 0 || wl_display_roundtrip(d) < 0)
        return 1;

    /* Mutate win[0] title to trigger dynamic floating rule, and win[1] to 2048-byte string */
    xdg_toplevel_set_title(wins[0].toplevel, "Picture-in-Picture");
    xdg_toplevel_move(wins[0].toplevel, env.seat, 1u);
    xdg_toplevel_resize(wins[0].toplevel, env.seat, 1u, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
    char *huge = malloc(2049);
    if (!huge)
        return 1;
    memset(huge, 'X', 2048);
    /* Place 2-byte UTF-8 'ä' (0xC3 0xA4) straddling index 511..512 */
    huge[511] = (char)0xC3;
    huge[512] = (char)0xA4;
    huge[2048] = '\0';
    xdg_toplevel_set_title(wins[1].toplevel, huge);
    free(huge);

    if (wl_display_roundtrip(d) < 0)
        return 1;

    FILE *fp = fopen(ready_file, "w");
    if (fp) {
        fputs("ready\n", fp);
        fclose(fp);
    }

    while (access(ready_file, F_OK) == 0) {
        if (wl_display_dispatch_pending(d) < 0 || wl_display_flush(d) < 0)
            break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000L };
        nanosleep(&ts, NULL);
    }

    for (int i = 1; i >= 0; i--) {
        wp_viewport_destroy(wins[i].viewport);
        xdg_toplevel_destroy(wins[i].toplevel);
        xdg_surface_destroy(wins[i].xdg_surface);
        wl_surface_destroy(wins[i].surface);
    }
    zwp_input_method_v2_destroy(im);
    zwp_virtual_keyboard_v1_destroy(vkbd);
    wl_buffer_destroy(buf);
    wl_display_roundtrip(d);

    zwp_input_method_manager_v2_destroy(env.im_mgr);
    zwp_virtual_keyboard_manager_v1_destroy(env.vkbd_mgr);
    wp_viewporter_destroy(env.viewporter);
    wp_single_pixel_buffer_manager_v1_destroy(env.spb_mgr);
    xdg_wm_base_destroy(env.wm_base);
    wl_seat_destroy(env.seat);
    wl_compositor_destroy(env.compositor);
    wl_registry_destroy(reg);
    wl_display_disconnect(d);
    return 0;
}

int
main(int argc, char *argv[])
{
    if (argc >= 3 && strcmp(argv[1], "globals") == 0)
        return check_globals(&argv[2], argc - 2);
    if (argc == 3 && strcmp(argv[1], "churn") == 0)
        return churn(atoi(argv[2]));
    if (argc == 3 && strcmp(argv[1], "windows") == 0)
        return test_windows(atoi(argv[2]));
    if (argc == 4 && strcmp(argv[1], "hold") == 0)
        return test_hold_windows(atoi(argv[2]), argv[3]);
    if (argc == 3 && strcmp(argv[1], "rogue") == 0)
        return test_rogue(argv[2]);
    if (argc == 2 && strcmp(argv[1], "shell") == 0)
        return test_shell();
    fprintf(stderr, "usage: %s globals IFACE... | churn N | windows N | hold N FILE | rogue FILE | shell\n", argv[0]);
    return 2;
}
