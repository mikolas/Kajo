/* comp_lock_idle.c — ext-session-lock-v1, ext-idle-notify-v1, idle-inhibit-v1,
 * and wlr-output-power-management-v1 (Phase 2c).
 */
#include "comp_server.h"

#include "comp_log.h"

#include <talloc.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>

static const float k_lock_backdrop_color[4] = { 0.066667f, 0.066667f, 0.066667f, 1.0f }; /* #111111 */

typedef struct {
    CompServer                        *server;
    struct wlr_session_lock_v1        *lock;
    bool                               locked_sent;
} CompSessionLock;

typedef struct {
    CompSessionLock                    *parent_lock;
    struct wlr_session_lock_surface_v1 *lsurf;
    struct wlr_scene_tree              *scene_tree;
} CompLockSurface;

typedef struct {
    CompServer                  *server;
    struct wlr_idle_inhibitor_v1 *inhibitor;
} CompIdleInhibitor;

void
comp_lock_idle_reconfigure_output(CompServer *s)
{
    if (!s || !s->tree_lock)
        return;

    double vw = 1920.0, vh = 1080.0;
    comp_server_viewport_size(s, &vw, &vh);

    if (s->desktop_bg) {
        wlr_scene_node_set_position(&s->desktop_bg->node, 0, 0);
        wlr_scene_rect_set_size(s->desktop_bg, (int)vw, (int)vh);
    }

    if (s->lock_backdrop) {
        wlr_scene_node_set_enabled(&s->lock_backdrop->node, s->locked);
        wlr_scene_node_set_position(&s->lock_backdrop->node, 0, 0);
        wlr_scene_rect_set_size(s->lock_backdrop, (int)vw, (int)vh);
    }

    if (s->active_lock) {
        struct wlr_session_lock_surface_v1 *lsurf;
        wl_list_for_each(lsurf, &s->active_lock->surfaces, link) {
            wlr_session_lock_surface_v1_configure(lsurf, (uint32_t)vw, (uint32_t)vh);
        }
    }
}

/* ── ext-session-lock-v1 ── */

static int
lock_surface_destructor(CompLockSurface *cls)
{
    CompServer *s = cls->parent_lock->server;
    if (s->lock_surface == cls->lsurf->surface) {
        s->lock_surface = NULL;
        comp_server_restore_keyboard_focus(s);
    }
    if (s->scene && cls->scene_tree) {
        wlr_scene_node_destroy(&cls->scene_tree->node);
        cls->scene_tree = NULL;
    }
    return 0;
}

static void
handle_lock_surface_map(void *owner, void *data)
{
    CompLockSurface *cls = owner;
    CompServer *s = cls->parent_lock->server;
    (void)data;

    s->lock_surface = cls->lsurf->surface;
    comp_server_restore_keyboard_focus(s);

    if (!cls->parent_lock->locked_sent) {
        cls->parent_lock->locked_sent = true;
        wlr_session_lock_v1_send_locked(cls->parent_lock->lock);
    }
    comp_server_schedule_frames(s);
}

static void
handle_lock_surface_destroy(void *owner, void *data)
{
    CompLockSurface *cls = owner;
    (void)data;
    talloc_free(cls);
}

static void
handle_lock_new_surface(void *owner, void *data)
{
    CompSessionLock *csl = owner;
    CompServer *s = csl->server;
    struct wlr_session_lock_surface_v1 *lsurf = data;

    CompLockSurface *cls = talloc_zero(csl, CompLockSurface);
    COMP_WARN_RETURN_VOID(cls != NULL);
    cls->parent_lock = csl;
    cls->lsurf       = lsurf;
    talloc_set_destructor(cls, lock_surface_destructor);

    cls->scene_tree = wlr_scene_subsurface_tree_create(s->tree_lock, lsurf->surface);
    if (!cls->scene_tree) {
        talloc_free(cls);
        return;
    }

    double vw = 1920.0, vh = 1080.0;
    comp_server_viewport_size(s, &vw, &vh);
    wlr_session_lock_surface_v1_configure(lsurf, (uint32_t)vw, (uint32_t)vh);

    if (!comp_listen(cls, &lsurf->surface->events.map, handle_lock_surface_map) ||
        !comp_listen(cls, &lsurf->events.destroy, handle_lock_surface_destroy)) {
        talloc_free(cls);
        return;
    }
}

static void
handle_lock_unlock(void *owner, void *data)
{
    CompSessionLock *csl = owner;
    CompServer *s = csl->server;
    (void)data;

    s->locked       = false;
    s->active_lock  = NULL;
    s->lock_surface = NULL;
    if (s->lock_backdrop)
        wlr_scene_node_set_enabled(&s->lock_backdrop->node, false);

    comp_server_restore_keyboard_focus(s);
    comp_server_schedule_frames(s);
}

static void
handle_lock_destroy(void *owner, void *data)
{
    CompSessionLock *csl = owner;
    CompServer *s = csl->server;
    (void)data;

    if (s->active_lock == csl->lock)
        s->active_lock = NULL;
    s->lock_surface = NULL;
    /* Note: if destroyed without unlock (client crashed), s->locked stays true
     * and s->lock_backdrop stays enabled (fail-secure per ext-session-lock-v1). */
    talloc_free(csl);
    comp_server_restore_keyboard_focus(s);
    comp_server_schedule_frames(s);
}

static void
handle_new_session_lock(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_session_lock_v1 *lock = data;

    if (s->active_lock) {
        wlr_session_lock_v1_destroy(lock);
        return;
    }

    CompSessionLock *csl = talloc_zero(s, CompSessionLock);
    if (!csl) {
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    talloc_set_name_const(csl, "CompSessionLock");
    csl->server = s;
    csl->lock   = lock;

    s->locked      = true;
    s->active_lock = lock;
    comp_lock_idle_reconfigure_output(s);
    comp_server_restore_keyboard_focus(s);

    /* s->lock_backdrop in s->tree_lock immediately covers the entire viewport
     * with solid #111111 before any client surface maps, so normal desktop
     * content is already hidden and we can confirm locked state immediately. */
    csl->locked_sent = true;
    wlr_session_lock_v1_send_locked(lock);

    if (!comp_listen(csl, &lock->events.new_surface, handle_lock_new_surface) ||
        !comp_listen(csl, &lock->events.unlock, handle_lock_unlock) ||
        !comp_listen(csl, &lock->events.destroy, handle_lock_destroy)) {
        talloc_free(csl);
    }
}

/* ── ext-idle-notify-v1 & idle-inhibit-v1 ── */

void
comp_lock_idle_notify_activity(CompServer *s)
{
    if (s && s->idle_notifier && s->seat)
        wlr_idle_notifier_v1_notify_activity(s->idle_notifier, s->seat);
}

void
comp_lock_idle_update_inhibitors(CompServer *s)
{
    if (!s || !s->idle_notifier || !s->idle_inhibit_mgr)
        return;

    bool inhibited = false;
    struct wlr_idle_inhibitor_v1 *inh;
    wl_list_for_each(inh, &s->idle_inhibit_mgr->inhibitors, link) {
        if (inh->surface && inh->surface->mapped) {
            inhibited = true;
            break;
        }
    }
    wlr_idle_notifier_v1_set_inhibited(s->idle_notifier, inhibited);
}

static void
handle_inhibitor_surface_change(void *owner, void *data)
{
    CompIdleInhibitor *ci = owner;
    (void)data;
    comp_lock_idle_update_inhibitors(ci->server);
}

static void
handle_inhibitor_destroy(void *owner, void *data)
{
    CompIdleInhibitor *ci = owner;
    CompServer *s = ci->server;
    (void)data;
    talloc_free(ci);
    comp_lock_idle_update_inhibitors(s);
}

static void
handle_new_inhibitor(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_idle_inhibitor_v1 *inh = data;

    CompIdleInhibitor *ci = talloc_zero(s, CompIdleInhibitor);
    COMP_WARN_RETURN_VOID(ci != NULL);
    ci->server    = s;
    ci->inhibitor = inh;

    if (inh->surface) {
        comp_listen(ci, &inh->surface->events.map, handle_inhibitor_surface_change);
        comp_listen(ci, &inh->surface->events.unmap, handle_inhibitor_surface_change);
    }
    comp_listen(ci, &inh->events.destroy, handle_inhibitor_destroy);
    comp_lock_idle_update_inhibitors(s);
}

/* ── wlr-output-power-management-v1 ── */

static void
handle_output_power_set_mode(void *owner, void *data)
{
    (void)owner;
    const struct wlr_output_power_v1_set_mode_event *ev = data;
    if (!ev || !ev->output)
        return;

    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
    if (wlr_output_test_state(ev->output, &st))
        wlr_output_commit_state(ev->output, &st);
    wlr_output_state_finish(&st);
}

void
comp_lock_idle_init(CompServer *s)
{
    if (s->tree_background) {
        s->desktop_bg = wlr_scene_rect_create(s->tree_background, 0, 0, k_lock_backdrop_color);
        if (s->desktop_bg)
            wlr_scene_node_lower_to_bottom(&s->desktop_bg->node);
    }
    if (s->tree_lock) {
        s->lock_backdrop = wlr_scene_rect_create(s->tree_lock, 0, 0, k_lock_backdrop_color);
        if (s->lock_backdrop)
            wlr_scene_node_set_enabled(&s->lock_backdrop->node, false);
    }

    s->session_lock_mgr = wlr_session_lock_manager_v1_create(s->display);
    s->idle_notifier    = wlr_idle_notifier_v1_create(s->display);
    s->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(s->display);
    s->output_power_mgr = wlr_output_power_manager_v1_create(s->display);

    if (s->session_lock_mgr)
        comp_listen_ctx(s->listeners, s, &s->session_lock_mgr->events.new_lock, handle_new_session_lock);
    if (s->idle_inhibit_mgr)
        comp_listen_ctx(s->listeners, s, &s->idle_inhibit_mgr->events.new_inhibitor, handle_new_inhibitor);
    if (s->output_power_mgr)
        comp_listen_ctx(s->listeners, s, &s->output_power_mgr->events.set_mode, handle_output_power_set_mode);
}
