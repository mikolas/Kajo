/* comp_output.c — outputs: enable with the preferred mode, drive spring frames and render the scene. */
#include "comp_server.h"

#include "comp_log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <talloc.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

static void
update_output_manager_config(CompServer *s)
{
    if (!s || !s->output_mgr)
        return;

    struct wlr_output_configuration_v1 *cfg = wlr_output_configuration_v1_create();
    if (!cfg)
        return;

    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (!o->wlr_output)
            continue;
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(cfg, o->wlr_output);
        if (head && s->output_layout) {
            struct wlr_box box = {0};
            wlr_output_layout_get_box(s->output_layout, o->wlr_output, &box);
            head->state.x = box.x;
            head->state.y = box.y;
        }
    }
    wlr_output_manager_v1_set_configuration(s->output_mgr, cfg);
}

static void
apply_or_test_output_config(CompServer *s, struct wlr_output_configuration_v1 *cfg, bool apply)
{
    if (!s || !cfg)
        return;

    bool ok = true;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &cfg->heads, link) {
        struct wlr_output *wo = head->state.output;
        if (!wo) {
            ok = false;
            break;
        }
        struct wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_head_v1_state_apply(&head->state, &st);
        bool step_ok = wlr_output_test_state(wo, &st);
        if (step_ok && apply)
            step_ok = wlr_output_commit_state(wo, &st);
        wlr_output_state_finish(&st);
        if (!step_ok) {
            ok = false;
            break;
        }
    }

    if (ok)
        wlr_output_configuration_v1_send_succeeded(cfg);
    else
        wlr_output_configuration_v1_send_failed(cfg);
    wlr_output_configuration_v1_destroy(cfg);

    if (apply && ok) {
        update_output_manager_config(s);
        comp_lock_idle_reconfigure_output(s);
        comp_layer_arrange(s);
        comp_window_relayout(s);
        comp_ipc_emit_outputs(s);
    }
}

static void
handle_output_mgr_test(void *owner, void *data)
{
    apply_or_test_output_config(owner, data, false);
}

static void
handle_output_mgr_apply(void *owner, void *data)
{
    apply_or_test_output_config(owner, data, true);
}

static void
handle_drm_lease_request(void *owner, void *data)
{
    (void)owner;
    struct wlr_drm_lease_request_v1 *req = data;
    if (!req)
        return;
    struct wlr_drm_lease_v1 *lease = wlr_drm_lease_request_v1_grant(req);
    if (!lease)
        wlr_drm_lease_request_v1_reject(req);
}

struct wlr_output *
comp_server_primary_output(const CompServer *s)
{
    if (!s || wl_list_empty(&s->outputs))
        return NULL;
    CompOutput *o = wl_container_of(s->outputs.next, o, link);
    return o->wlr_output;
}

void
comp_server_viewport_size(const CompServer *s, double *out_w, double *out_h)
{
    double w = 1920.0, h = 1080.0;
    struct wlr_output *wo = comp_server_primary_output(s);
    if (wo) {
        int ew = 0, eh = 0;
        wlr_output_effective_resolution(wo, &ew, &eh);
        if (ew > 0 && eh > 0) {
            w = (double)ew;
            h = (double)eh;
        }
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

void
comp_server_working_area(const CompServer *s, CompRect *out_area)
{
    if (!out_area)
        return;
    if (s && s->has_usable_area && s->usable_area.w > 0.0 && s->usable_area.h > 0.0) {
        *out_area = s->usable_area;
        return;
    }
    double vw = 1920.0, vh = 1080.0;
    comp_server_viewport_size(s, &vw, &vh);
    out_area->x = 0.0;
    out_area->y = 0.0;
    out_area->w = vw;
    out_area->h = vh;
}

void
comp_server_schedule_frames(CompServer *s)
{
    if (!s)
        return;
    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        wlr_output_schedule_frame(o->wlr_output);
    }
}

static int
output_destructor(CompOutput *o)
{
    wl_list_remove(&o->link);
    return 0;
}

static void
handle_frame(void *owner, void *data)
{
    CompOutput *o = owner;
    CompServer *s = o->server;
    (void)data;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double now_s = (double)now.tv_sec + (double)now.tv_nsec / 1e9;

    comp_window_tick_frame(s, now_s);

    if (wlr_scene_output_commit(o->scene_output, NULL))
        wlr_scene_output_send_frame_done(o->scene_output, &now);

    bool animating = comp_motion_is_animating(s->motion, now_s);
    if (animating || o->was_animating) {
        uint32_t msec = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
        comp_input_refresh_pointer_focus(s, msec);
        if (animating)
            wlr_output_schedule_frame(o->wlr_output);
    }
    o->was_animating = animating;
}

static void
handle_request_state(void *owner, void *data)
{
    CompOutput *o = owner;
    const struct wlr_output_event_request_state *ev = data;
    /* Nested and headless backends ask for a new size when their window is resized */
    if (!wlr_output_commit_state(o->wlr_output, ev->state)) {
        wlr_log(WLR_ERROR, "output %s: requested state was rejected", o->wlr_output->name);
        return;
    }
    update_output_manager_config(o->server);
    comp_lock_idle_reconfigure_output(o->server);
    comp_layer_arrange(o->server);
    comp_window_relayout(o->server);
    comp_ipc_emit_outputs(o->server);
}

static void
handle_destroy(void *owner, void *data)
{
    CompOutput *o = owner;
    CompServer *s = o->server;
    (void)data;
    wlr_log(WLR_INFO, "output %s removed", o->wlr_output->name);
    talloc_free(o);   /* unlinks all listeners and the list link */
    update_output_manager_config(s);
    comp_ipc_emit_outputs(s);
}

static enum wl_output_transform
wl_transform_from_idx(int idx)
{
    switch (idx) {
    case 1:  return WL_OUTPUT_TRANSFORM_90;
    case 2:  return WL_OUTPUT_TRANSFORM_180;
    case 3:  return WL_OUTPUT_TRANSFORM_270;
    default: return WL_OUTPUT_TRANSFORM_NORMAL;
    }
}

void
comp_output_update_vrr(CompServer *s)
{
    if (!s)
        return;
    bool wants = comp_window_focused_wants_vrr(s);
    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_output *wo = o->wlr_output;
        if (!wo || !wo->enabled)
            continue;
        bool desired = (o->vrr_policy == COMP_VRR_ON) ||
                       (o->vrr_policy == COMP_VRR_ON_DEMAND && wants);
        bool current = (wo->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
        if (desired == current)
            continue;
        struct wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_set_adaptive_sync_enabled(&st, desired);
        if (wlr_output_test_state(wo, &st))
            wlr_output_commit_state(wo, &st);
        wlr_output_state_finish(&st);
    }
}

bool
comp_output_configure(CompServer *s, const char *output_name,
                      const char *mode_str, double scale, int transform, int vrr_policy)
{
    if (!s || wl_list_empty(&s->outputs))
        return false;

    CompOutput *target = NULL;
    if (!output_name || !*output_name) {
        target = wl_container_of(s->outputs.next, target, link);
    } else {
        CompOutput *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (o->wlr_output->name && strcasecmp(o->wlr_output->name, output_name) == 0) {
                target = o;
                break;
            }
        }
    }
    if (!target)
        return false;

    struct wlr_output *wo = target->wlr_output;
    if (vrr_policy >= 0 && vrr_policy <= COMP_VRR_ON_DEMAND)
        target->vrr_policy = (CompVrrPolicy)vrr_policy;

    struct wlr_output_state st;
    wlr_output_state_init(&st);
    bool has_changes = false;

    if (mode_str && *mode_str) {
        if (strcasecmp(mode_str, "preferred") == 0) {
            struct wlr_output_mode *pm = wlr_output_preferred_mode(wo);
            if (pm) {
                wlr_output_state_set_mode(&st, pm);
                has_changes = true;
            }
        } else {
            int mw = 0, mh = 0;
            double mhz = 0.0;
            int n = sscanf(mode_str, "%dx%d@%lf", &mw, &mh, &mhz);
            if (n >= 2 && mw > 0 && mh > 0) {
                int32_t target_mhz = (n == 3 && mhz > 0.0) ? (int32_t)round(mhz * 1000.0) : 0;
                struct wlr_output_mode *best = NULL;
                int32_t best_diff = 0x7fffffff;
                struct wlr_output_mode *m;
                wl_list_for_each(m, &wo->modes, link) {
                    if (m->width != mw || m->height != mh)
                        continue;
                    if (target_mhz == 0) {
                        if (!best || m->preferred || m->refresh > best->refresh)
                            best = m;
                    } else {
                        int32_t diff = abs(m->refresh - target_mhz);
                        if (diff < best_diff) {
                            best_diff = diff;
                            best = m;
                        }
                    }
                }
                if (best)
                    wlr_output_state_set_mode(&st, best);
                else
                    wlr_output_state_set_custom_mode(&st, mw, mh, target_mhz);
                has_changes = true;
            }
        }
    }

    if (scale > 0.0) {
        wlr_output_state_set_scale(&st, (float)scale);
        has_changes = true;
    }
    if (transform >= 0 && transform <= 3) {
        wlr_output_state_set_transform(&st, wl_transform_from_idx(transform));
        has_changes = true;
    }

    bool ok = true;
    if (has_changes) {
        ok = wlr_output_test_state(wo, &st) && wlr_output_commit_state(wo, &st);
        if (!ok && (scale > 0.0 || (transform >= 0 && transform <= 3))) {
            /* Fallback: apply scale/transform even if custom mode was rejected */
            wlr_output_state_finish(&st);
            wlr_output_state_init(&st);
            if (scale > 0.0)
                wlr_output_state_set_scale(&st, (float)scale);
            if (transform >= 0 && transform <= 3)
                wlr_output_state_set_transform(&st, wl_transform_from_idx(transform));
            ok = wlr_output_test_state(wo, &st) && wlr_output_commit_state(wo, &st);
        }
    }
    wlr_output_state_finish(&st);

    comp_output_update_vrr(s);
    update_output_manager_config(s);
    comp_lock_idle_reconfigure_output(s);
    comp_layer_arrange(s);
    comp_window_relayout(s);
    comp_ipc_emit_outputs(s);
    return ok;
}

static bool
output_matches_rule(const struct wlr_output *wo, const char *match)
{
    if (!wo || !match || !*match)
        return false;
    if (wo->name && strcasecmp(wo->name, match) == 0)
        return true;
    if (wo->make && strcasecmp(wo->make, match) == 0)
        return true;
    if (wo->model && strcasecmp(wo->model, match) == 0)
        return true;
    if (wo->make && wo->model) {
        char edid[256];
        snprintf(edid, sizeof(edid), "%s %s", wo->make, wo->model);
        if (strcasecmp(edid, match) == 0)
            return true;
    }
    return false;
}

static int
transform_idx_from_str(const char *s)
{
    if (!s) return -1;
    if (strcasecmp(s, "normal") == 0) return 0;
    if (strcmp(s, "90") == 0)         return 1;
    if (strcmp(s, "180") == 0)        return 2;
    if (strcmp(s, "270") == 0)        return 3;
    return -1;
}

static int
vrr_idx_from_str(const char *s)
{
    if (!s) return -1;
    if (strcasecmp(s, "off") == 0)       return COMP_VRR_OFF;
    if (strcasecmp(s, "on") == 0)        return COMP_VRR_ON;
    if (strcasecmp(s, "on-demand") == 0) return COMP_VRR_ON_DEMAND;
    return -1;
}

void
comp_output_apply_config(CompServer *s)
{
    if (!s || !s->config)
        return;
    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_output *wo = o->wlr_output;
        for (size_t i = 0; i < s->config->n_outputs; i++) {
            const CompOutputRule *r = &s->config->outputs[i];
            if (output_matches_rule(wo, r->match)) {
                comp_output_configure(s, wo->name,
                                      r->has_mode ? r->mode : NULL,
                                      r->has_scale ? r->scale : -1.0,
                                      r->has_transform ? transform_idx_from_str(r->transform) : -1,
                                      r->has_vrr ? vrr_idx_from_str(r->vrr) : -1);
            }
        }
    }
}

static bool
enable_output(struct wlr_output *wo)
{
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wo);
    if (mode)
        wlr_output_state_set_mode(&st, mode);
    bool ok = wlr_output_test_state(wo, &st) && wlr_output_commit_state(wo, &st);
    wlr_output_state_finish(&st);
    return ok;
}

static void
handle_new_output(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_output *wo = data;

    if (!wlr_output_init_render(wo, s->allocator, s->renderer)) {
        wlr_log(WLR_ERROR, "output %s: cannot initialise rendering; ignored", wo->name);
        return;
    }
    if (!enable_output(wo)) {
        wlr_log(WLR_ERROR, "output %s: preferred mode rejected; ignored", wo->name);
        return;
    }

    CompOutput *o = talloc_zero(s, CompOutput);
    COMP_WARN_RETURN_VOID(o != NULL);
    talloc_set_name_const(o, "CompOutput");
    o->server = s;
    o->wlr_output = wo;
    wl_list_insert(&s->outputs, &o->link);
    talloc_set_destructor(o, output_destructor);

    if (!comp_listen(o, &wo->events.frame, handle_frame) ||
        !comp_listen(o, &wo->events.request_state, handle_request_state) ||
        !comp_listen(o, &wo->events.destroy, handle_destroy)) {
        talloc_free(o);
        return;
    }

    struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(s->output_layout, wo);
    o->scene_output = wlr_scene_output_create(s->scene, wo);
    if (!lo || !o->scene_output) {
        wlr_log(WLR_ERROR, "output %s: cannot add to the layout", wo->name);
        talloc_free(o);
        return;
    }
    wlr_scene_output_layout_add_output(s->scene_layout, lo, o->scene_output);
    if (s->cursor && s->cursor_mgr && (!s->seat || !s->seat->pointer_state.focused_surface))
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
    wlr_log(WLR_INFO, "output %s: %dx%d @ %.3f Hz, scale %.2f", wo->name, wo->width, wo->height,
            wo->refresh / 1000.0, (double)wo->scale);
    comp_output_apply_config(s);
    update_output_manager_config(s);
    comp_lock_idle_reconfigure_output(s);
    comp_layer_arrange(s);
    comp_window_relayout(s);
    comp_ipc_emit_outputs(s);
}

void
comp_output_init(CompServer *s)
{
    s->output_mgr = wlr_output_manager_v1_create(s->display);
    if (s->output_mgr) {
        comp_listen_ctx(s->listeners, s, &s->output_mgr->events.test, handle_output_mgr_test);
        comp_listen_ctx(s->listeners, s, &s->output_mgr->events.apply, handle_output_mgr_apply);
    }

    s->drm_lease_mgr = wlr_drm_lease_v1_manager_create(s->display, s->backend);
    if (s->drm_lease_mgr) {
        comp_listen_ctx(s->listeners, s, &s->drm_lease_mgr->events.request, handle_drm_lease_request);
    }

    if (!comp_listen_ctx(s->listeners, s, &s->backend->events.new_output, handle_new_output))
        wlr_log(WLR_ERROR, "cannot listen for outputs");
}
