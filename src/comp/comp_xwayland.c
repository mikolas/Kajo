/* comp_xwayland.c — on-demand XWayland server, XWM window lifecycle, and
 * unscaled workarea management (spec §2.5.5, Phase 2c).
 */
#include "comp_server.h"

#include "comp_log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <talloc.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/xwayland/xwayland.h>

typedef struct {
    struct wl_list               link;         /* CompServer.xwayland_wins */
    CompServer                  *server;
    struct wlr_xwayland_surface *xsurf;
    struct CompWindow           *win;          /* non-NULL for managed (!override_redirect) windows */
    struct wlr_scene_tree       *scene_tree;   /* used only for override_redirect surfaces */
    void                        *assoc_ctx;    /* talloc ctx for map/unmap listeners */
    bool                         mapped;
} CompXWaylandWin;

void
comp_xwayland_update_workarea(CompServer *s)
{
    if (!s || !s->xwayland || !s->xwayland->xwm)
        return;

    CompRect area = {0};
    comp_server_working_area(s, &area);

    /* XWayland in upstream wlroots receives logical wl_output coordinates, so
     * _NET_WORKAREA must be advertised in logical coordinates to match xsurf->x/y. */
    struct wlr_box box = {
        .x      = (int)lround(area.x),
        .y      = (int)lround(area.y),
        .width  = (int)lround(area.w),
        .height = (int)lround(area.h),
    };
    if (box.width < 1)  box.width = 1;
    if (box.height < 1) box.height = 1;
    wlr_xwayland_set_workareas(s->xwayland, &box, 1);

    bool force_zero = (!s->config || s->config->xwayland_force_zero_scaling);
    double scale = 1.0;
    struct wlr_output *out = comp_server_primary_output(s);
    if (force_zero && out && out->scale > 1.0f)
        scale = (double)out->scale;

    int dpi = (int)lround(96.0 * scale);
    if (dpi != s->xwayland_last_dpi && s->xwayland->display_name) {
        s->xwayland_last_dpi = dpi;
        const char *no_env = getenv("KAJO_COMP_NO_DBUS_ENV");
        if (!no_env || no_env[0] == '\0' || no_env[0] == '0') {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "printf 'Xft.dpi: %d\\n' %d | xrdb -merge 2>/dev/null", dpi, dpi);
            const char * const argv[] = { "sh", "-c", cmd, NULL };
            (void)comp_spawn(s, argv);
        }
    }
}

static int
xwayland_win_destructor(CompXWaylandWin *xw)
{
    wl_list_remove(&xw->link);
    if (xw->win) {
        talloc_free(xw->win);
        xw->win = NULL;
    }
    if (xw->server->scene && xw->scene_tree) {
        wlr_scene_node_destroy(&xw->scene_tree->node);
        xw->scene_tree = NULL;
    }
    return 0;
}

static void
handle_xsurf_map(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    CompServer *s = xw->server;
    (void)data;

    if (xw->mapped || !xw->xsurf->surface)
        return;
    xw->mapped = true;

    if (!xw->xsurf->override_redirect) {
        if (!xw->win)
            xw->win = comp_window_create_xwayland(s, xw->xsurf);
        if (xw->win)
            comp_window_map_xwayland(xw->win);
        return;
    }

    xw->scene_tree = wlr_scene_subsurface_tree_create(s->tree_top, xw->xsurf->surface);
    if (xw->scene_tree) {
        wlr_scene_node_set_position(&xw->scene_tree->node, xw->xsurf->x, xw->xsurf->y);
        wlr_scene_node_raise_to_top(&xw->scene_tree->node);
    }

    if (!s->locked && wlr_xwayland_surface_override_redirect_wants_focus(xw->xsurf)) {
        wlr_xwayland_surface_activate(xw->xsurf, true);
        comp_server_keyboard_enter(s, xw->xsurf->surface);
    }
    comp_server_schedule_frames(s);
}

static void
handle_xsurf_unmap(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    CompServer *s = xw->server;
    (void)data;

    if (!xw->mapped)
        return;
    xw->mapped = false;
    if (xw->win) {
        comp_window_unmap_xwayland(xw->win);
        return;
    }
    if (s->scene && xw->scene_tree) {
        wlr_scene_node_destroy(&xw->scene_tree->node);
        xw->scene_tree = NULL;
    }
    comp_server_restore_keyboard_focus(s);
    comp_server_schedule_frames(s);
}

static void
handle_xsurf_associate(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    (void)data;
    talloc_free(xw->assoc_ctx);
    xw->assoc_ctx = talloc_named_const(xw, 0, "CompXWaylandWin assoc");
    if (!xw->assoc_ctx || !xw->xsurf->surface)
        return;
    comp_listen_ctx(xw->assoc_ctx, xw, &xw->xsurf->surface->events.map, handle_xsurf_map);
    comp_listen_ctx(xw->assoc_ctx, xw, &xw->xsurf->surface->events.unmap, handle_xsurf_unmap);
}

static void
handle_xsurf_dissociate(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    (void)data;
    handle_xsurf_unmap(xw, NULL);
    talloc_free(xw->assoc_ctx);
    xw->assoc_ctx = NULL;
}

static void
handle_xsurf_request_configure(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    struct wlr_xwayland_surface_configure_event *ev = data;
    if (!ev)
        return;
    if (xw->mapped && xw->win && !xw->xsurf->override_redirect) {
        comp_window_relayout(xw->server);
        return;
    }
    wlr_xwayland_surface_configure(xw->xsurf, ev->x, ev->y, ev->width, ev->height);
    if (xw->scene_tree)
        wlr_scene_node_set_position(&xw->scene_tree->node, ev->x, ev->y);
}

static void
handle_xsurf_set_geometry(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    (void)data;
    if (xw->scene_tree && xw->xsurf->override_redirect)
        wlr_scene_node_set_position(&xw->scene_tree->node, xw->xsurf->x, xw->xsurf->y);
}

static void
handle_xsurf_request_activate(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    (void)data;
    if (!xw->mapped || xw->server->locked)
        return;
    if (xw->win) {
        comp_window_focus(xw->server, xw->win);
        return;
    }
    wlr_xwayland_surface_activate(xw->xsurf, true);
}

static void
handle_xsurf_request_fullscreen(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    (void)data;
    if (xw->win)
        comp_window_set_xwayland_fullscreen(xw->win, xw->xsurf->fullscreen);
}

static void
handle_xsurf_request_maximize(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    (void)data;
    if (xw->win) {
        comp_window_set_xwayland_maximized(
            xw->win, xw->xsurf->maximized_horz || xw->xsurf->maximized_vert);
    }
}

static void
handle_xsurf_set_metadata(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    (void)data;
    if (xw->win)
        comp_window_update_xwayland_metadata(xw->win);
}

static void
handle_xsurf_destroy(void *owner, void *data)
{
    CompXWaylandWin *xw = owner;
    CompServer *s = xw->server;
    (void)data;
    bool was_mapped_win = (xw->mapped && xw->win != NULL);
    talloc_free(xw);
    if (was_mapped_win) {
        comp_window_relayout(s);
        comp_ipc_emit_workspaces(s);
        comp_ipc_emit_windows(s);
    }
}

static void
handle_xwayland_ready(void *owner, void *data)
{
    CompServer *s = owner;
    (void)data;
    if (!s->xwayland)
        return;
    wlr_xwayland_set_seat(s->xwayland, s->seat);
    if (s->xwayland->display_name) {
        setenv("DISPLAY", s->xwayland->display_name, 1);
        wlr_log(WLR_INFO, "XWayland ready on DISPLAY=%s", s->xwayland->display_name);
    }
    comp_xwayland_update_workarea(s);
}

static void
handle_xwayland_new_surface(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_xwayland_surface *xsurf = data;

    CompXWaylandWin *xw = talloc_zero(s, CompXWaylandWin);
    COMP_WARN_RETURN_VOID(xw != NULL);
    talloc_set_name_const(xw, "CompXWaylandWin");
    xw->server = s;
    xw->xsurf  = xsurf;
    wl_list_insert(&s->xwayland_wins, &xw->link);
    talloc_set_destructor(xw, xwayland_win_destructor);

    bool ok = comp_listen(xw, &xsurf->events.associate, handle_xsurf_associate) &&
              comp_listen(xw, &xsurf->events.dissociate, handle_xsurf_dissociate) &&
              comp_listen(xw, &xsurf->events.request_configure, handle_xsurf_request_configure) &&
              comp_listen(xw, &xsurf->events.set_geometry, handle_xsurf_set_geometry) &&
              comp_listen(xw, &xsurf->events.request_activate, handle_xsurf_request_activate) &&
              comp_listen(xw, &xsurf->events.request_fullscreen, handle_xsurf_request_fullscreen) &&
              comp_listen(xw, &xsurf->events.request_maximize, handle_xsurf_request_maximize) &&
              comp_listen(xw, &xsurf->events.set_title, handle_xsurf_set_metadata) &&
              comp_listen(xw, &xsurf->events.set_class, handle_xsurf_set_metadata) &&
              comp_listen(xw, &xsurf->events.destroy, handle_xsurf_destroy);
    if (!ok)
        talloc_free(xw);
}

void
comp_xwayland_init(CompServer *s)
{
    wl_list_init(&s->xwayland_wins);
    /* Spec §6.5.3: XWayland is disabled in --greeter-mode */
    if (s->opts.greeter_mode)
        return;
    const char *no_xw = getenv("KAJO_COMP_NO_XWAYLAND");
    if (no_xw && no_xw[0] != '\0' && no_xw[0] != '0')
        return;

    s->xwayland = wlr_xwayland_create(s->display, s->compositor, true);
    if (!s->xwayland) {
        wlr_log(WLR_INFO, "XWayland not available; continuing Wayland-only");
        return;
    }
    void *xw_ctx = talloc_named_const(s, 0, "CompXWayland listeners");
    s->xwayland->data = xw_ctx;
    if (xw_ctx) {
        comp_listen_ctx(xw_ctx, s, &s->xwayland->events.ready, handle_xwayland_ready);
        comp_listen_ctx(xw_ctx, s, &s->xwayland->events.new_surface, handle_xwayland_new_surface);
    }
}

void
comp_xwayland_finish(CompServer *s)
{
    if (s && s->xwayland) {
        talloc_free(s->xwayland->data);
        s->xwayland->data = NULL;
        wlr_xwayland_destroy(s->xwayland);
        s->xwayland = NULL;
        s->xwayland_last_dpi = 0;
    }
}
