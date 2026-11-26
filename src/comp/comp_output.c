/* comp_output.c — single-output priority selection (OUT1–OUT4), hotplug,
 * atomic KMS test-before-commit cascade (§7.3.1), VRR on-demand, and scene rendering.
 */
#include "comp_server.h"

#include "comp_log.h"
#include "core/comp_schema_keys.h"
#include "fx/comp_fx.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <talloc.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

static bool
is_internal_panel_name(const char *name)
{
    if (!name)
        return false;
    return strncasecmp(name, "eDP-", 4) == 0 ||
           strncasecmp(name, "LVDS-", 5) == 0 ||
           strncasecmp(name, "DSI-", 4) == 0;
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

static const CompOutputRule *
find_matching_output_rule_in_cfg(const CompConfig *cfg, const struct wlr_output *wo)
{
    if (!cfg || !wo)
        return NULL;
    const CompOutputRule *matched = NULL;
    for (size_t i = 0; i < cfg->n_outputs; i++) {
        if (output_matches_rule(wo, cfg->outputs[i].match))
            matched = &cfg->outputs[i];
    }
    return matched;
}

static const CompOutputRule *
find_matching_output_rule(const CompServer *s, const struct wlr_output *wo)
{
    return s ? find_matching_output_rule_in_cfg(s->config, wo) : NULL;
}

void
comp_output_invalidate_changed_rules(CompServer *s, const CompConfig *old_cfg, const CompConfig *new_cfg)
{
    if (!s)
        return;
    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (!o->wlr_output)
            continue;
        const CompOutputRule *ro = find_matching_output_rule_in_cfg(old_cfg, o->wlr_output);
        const CompOutputRule *rn = find_matching_output_rule_in_cfg(new_cfg, o->wlr_output);

        bool om = (ro && ro->has_mode), nm = (rn && rn->has_mode);
        if (om != nm || (om && strcmp(ro->mode ? ro->mode : "", rn->mode ? rn->mode : "") != 0))
            o->has_manual_mode = false;

        bool os = (ro && ro->has_scale), ns = (rn && rn->has_scale);
        if (os != ns || (os && fabs(ro->scale - rn->scale) > 1e-6))
            o->has_manual_scale = false;

        bool ot = (ro && ro->has_transform), nt = (rn && rn->has_transform);
        if (ot != nt || (ot && ro->transform != rn->transform))
            o->has_manual_transform = false;

        bool ov = (ro && ro->has_vrr), nv = (rn && rn->has_vrr);
        if (ov != nv || (ov && ro->vrr != rn->vrr))
            o->has_manual_vrr = false;

        bool oh = (ro && ro->has_hdr), nh = (rn && rn->has_hdr);
        if (oh != nh || (oh && ro->hdr != rn->hdr))
            o->has_manual_hdr = false;

        bool op = (ro && ro->has_hdr_peak_nits), np = (rn && rn->has_hdr_peak_nits);
        if (op != np || (op && ro->hdr_peak_nits != rn->hdr_peak_nits))
            o->has_manual_hdr_peak_nits = false;
    }
}

/* Spec §3.13 OUT2: deterministic output priority in session/normal mode.
 * 1. First connected output matching an [[output]] rule in load order.
 * 2. First connected internal panel (eDP-* / LVDS-* / DSI-*).
 * 3. First connected connector in arrival order.
 */
static CompOutput *
select_primary_output(const CompServer *s)
{
    if (!s || wl_list_empty(&s->outputs))
        return NULL;

    if (s->config) {
        for (size_t i = 0; i < s->config->n_outputs; i++) {
            const char *match = s->config->outputs[i].match;
            CompOutput *o;
            wl_list_for_each(o, &s->outputs, link) {
                if (o->wlr_output && output_matches_rule(o->wlr_output, match))
                    return o;
            }
        }
    }

    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr_output && is_internal_panel_name(o->wlr_output->name))
            return o;
    }

    return wl_container_of(s->outputs.next, o, link);
}

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
        if (head && s->output_layout && wlr_output_layout_get(s->output_layout, o->wlr_output)) {
            struct wlr_box box = {0};
            wlr_output_layout_get_box(s->output_layout, o->wlr_output, &box);
            head->state.x = box.x;
            head->state.y = box.y;
        }
    }
    wlr_output_manager_v1_set_configuration(s->output_mgr, cfg);
}

static void
comp_output_post_change(CompServer *s)
{
    if (!s)
        return;
    update_output_manager_config(s);
    comp_lock_idle_reconfigure_output(s);
    s->has_usable_area = false;
    comp_layer_arrange(s);
    comp_ipc_emit_outputs(s);
}

static void
sync_primary_output_devices(CompServer *s)
{
    struct wlr_output *primary = comp_server_primary_output(s);
    if (!primary)
        return;
    double w_mm = primary->phys_width > 0 ? (double)primary->phys_width : 300.0;
    double h_mm = primary->phys_height > 0 ? (double)primary->phys_height : 188.0;
    comp_touch_tracker_set_geometry(&s->touch_tracker, w_mm, h_mm,
                                    (CompTransform)primary->transform);
    comp_input_map_devices_to_output(s, primary);
}

static CompOutput *
find_comp_output(CompServer *s, const struct wlr_output *wo)
{
    if (!s || !wo)
        return NULL;
    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr_output == wo)
            return o;
    }
    return NULL;
}

static void
persist_manual_head_state(CompOutput *o, const struct wlr_output_configuration_head_v1 *head)
{
    if (!o || !o->wlr_output || !head)
        return;
    struct wlr_output *wo = o->wlr_output;

    const struct wlr_output_mode *m = head->state.mode;
    int32_t w = m ? m->width   : (head->state.custom_mode.width > 0 ? head->state.custom_mode.width : wo->width);
    int32_t h = m ? m->height  : (head->state.custom_mode.height > 0 ? head->state.custom_mode.height : wo->height);
    int32_t r = m ? m->refresh : (head->state.custom_mode.refresh > 0 ? head->state.custom_mode.refresh : wo->refresh);
    if (w > 0 && h > 0) {
        char buf[64];
        if (r > 0)
            snprintf(buf, sizeof(buf), "%dx%d@%.3f", w, h, (double)r / 1000.0);
        else
            snprintf(buf, sizeof(buf), "%dx%d", w, h);
        talloc_free(o->manual_mode);
        o->manual_mode     = talloc_strdup(o, buf);
        o->has_manual_mode = (o->manual_mode != NULL);
    }
    if (head->state.scale > 0.0f) {
        o->has_manual_scale = true;
        o->manual_scale     = (double)head->state.scale;
    }
    if ((int)head->state.transform >= COMP_TRANSFORM_NORMAL &&
        (int)head->state.transform <= COMP_TRANSFORM_FLIPPED_270) {
        o->has_manual_transform = true;
        o->manual_transform     = (CompTransform)head->state.transform;
    }
    o->has_manual_vrr = true;
    o->manual_vrr     = head->state.adaptive_sync_enabled ? COMP_VRR_ON : COMP_VRR_OFF;
    o->vrr_policy     = o->manual_vrr;
}

static void
apply_or_test_output_config(CompServer *s, struct wlr_output_configuration_v1 *cfg, bool apply)
{
    if (!s || !cfg)
        return;

    size_t n_heads = 0;
    size_t n_enabled = 0;
    bool ok = true;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &cfg->heads, link) {
        if (!head->state.output) {
            ok = false;
            break;
        }
        if (head->state.enabled)
            n_enabled++;
        n_heads++;
    }

    /* Spec §3.13 OUT1: reject configs enabling > 1 output in normal/session mode. */
    if (!s->opts.greeter_mode && n_enabled > 1)
        ok = false;

    struct wlr_backend_output_state *states = NULL;
    if (ok && n_heads > 0) {
        states = calloc(n_heads, sizeof(*states));
        if (!states)
            ok = false;
    }

    if (ok && states) {
        size_t idx = 0;
        wl_list_for_each(head, &cfg->heads, link) {
            states[idx].output = head->state.output;
            wlr_output_state_init(&states[idx].base);
            wlr_output_head_v1_state_apply(&head->state, &states[idx].base);
            idx++;
        }

        ok = wlr_backend_test(s->backend, states, n_heads);
        if (ok && apply)
            ok = wlr_backend_commit(s->backend, states, n_heads);

        for (size_t i = 0; i < n_heads; i++)
            wlr_output_state_finish(&states[i].base);
    }
    free(states);

    if (ok && apply) {
        wl_list_for_each(head, &cfg->heads, link) {
            struct wlr_output *wo = head->state.output;
            CompOutput *o = find_comp_output(s, wo);
            if (!head->state.enabled) {
                if (wlr_output_layout_get(s->output_layout, wo)) {
                    wlr_output_layout_remove(s->output_layout, wo);
                    if (o)
                        o->scene_output = wlr_scene_get_scene_output(s->scene, wo);
                }
                continue;
            }
            if (o) {
                persist_manual_head_state(o, head);
                o->scene_output = wlr_scene_get_scene_output(s->scene, wo);
                if (!o->scene_output)
                    o->scene_output = wlr_scene_output_create(s->scene, wo);
            }
            bool had_lo = (wlr_output_layout_get(s->output_layout, wo) != NULL);
            struct wlr_output_layout_output *lo =
                wlr_output_layout_add(s->output_layout, wo, head->state.x, head->state.y);
            if (!had_lo && lo && o && o->scene_output)
                wlr_scene_output_layout_add_output(s->scene_layout, lo, o->scene_output);
        }
        comp_output_update_vrr(s);
        sync_primary_output_devices(s);
    }

    if (ok)
        wlr_output_configuration_v1_send_succeeded(cfg);
    else
        wlr_output_configuration_v1_send_failed(cfg);
    wlr_output_configuration_v1_destroy(cfg);

    if (apply && ok)
        comp_output_post_change(s);
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
    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr_output && o->wlr_output->enabled)
            return o->wlr_output;
    }
    CompOutput *first = wl_container_of(s->outputs.next, first, link);
    return first->wlr_output;
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
comp_server_output_box(const CompServer *s, struct wlr_output *wo, struct wlr_box *out_box)
{
    if (!out_box)
        return;
    *out_box = (struct wlr_box){0};
    if (s && s->output_layout && wo)
        wlr_output_layout_get_box(s->output_layout, wo, out_box);
    if (out_box->width <= 0 || out_box->height <= 0) {
        int ew = 0, eh = 0;
        if (wo)
            wlr_output_effective_resolution(wo, &ew, &eh);
        if (ew > 0 && eh > 0) {
            out_box->width  = ew;
            out_box->height = eh;
        } else {
            double vw = 0.0, vh = 0.0;
            comp_server_viewport_size(s, &vw, &vh);
            out_box->width  = (int)vw;
            out_box->height = (int)vh;
        }
    }
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
    double vw = 0.0, vh = 0.0;
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
        if (o->wlr_output && o->wlr_output->enabled)
            wlr_output_schedule_frame(o->wlr_output);
    }
}

static int
output_destructor(CompOutput *o)
{
    if (o->hdr_auto_timer) {
        wl_event_source_remove(o->hdr_auto_timer);
        o->hdr_auto_timer = NULL;
        o->hdr_auto_timer_armed = false;
    }
    wl_list_remove(&o->link);
    return 0;
}

static void
handle_present(void *owner, void *data)
{
    CompOutput *o = owner;
    const struct wlr_output_event_present *ev = data;
    if (!o->pending_lock_present)
        return;
    if (ev && !ev->presented)
        return;
    o->pending_lock_present = false;
    if (o->server && o->server->locked)
        comp_lock_idle_notify_output_presented(o->server);
}

static void
handle_frame(void *owner, void *data)
{
    CompOutput *o = owner;
    CompServer *s = o->server;
    (void)data;

    if (!o->wlr_output || !o->wlr_output->enabled || !o->scene_output)
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double now_s = (double)now.tv_sec + (double)now.tv_nsec / 1e9;

    comp_window_tick_frame(s, now_s);
    bool ov_animating = comp_overview_tick_frame(s, now_s);
    comp_output_update_hdr_auto(s);
    bool fx_animating = comp_fx_output_prepare(o, now_s);

    struct wlr_scene_output_state_options scene_opts = {
        .color_transform = comp_fx_output_get_color_transform(o),
    };
    const struct wlr_scene_output_state_options *opts_ptr =
        scene_opts.color_transform ? &scene_opts : NULL;

    bool lock_active = s->locked;
    bool committed = false;
    if (!lock_active && comp_window_focused_wants_tearing(s) &&
        wlr_scene_output_needs_frame(o->scene_output)) {
        struct wlr_output_state st;
        wlr_output_state_init(&st);
        if (wlr_scene_output_build_state(o->scene_output, &st, opts_ptr)) {
            st.tearing_page_flip = true;
            if (!wlr_output_test_state(o->wlr_output, &st))
                st.tearing_page_flip = false;
            committed = wlr_output_commit_state(o->wlr_output, &st);
        }
        wlr_output_state_finish(&st);
    } else {
        committed = wlr_scene_output_commit(o->scene_output, opts_ptr);
    }
    if (committed)
        wlr_scene_output_send_frame_done(o->scene_output, &now);
    if (lock_active) {
        if (committed)
            o->pending_lock_present = true;
        else
            comp_lock_idle_notify_output_presented(s);
    }

    bool animating = comp_motion_is_animating(s->motion, now_s);
    if (animating || o->was_animating) {
        uint32_t msec = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
        comp_input_refresh_pointer_focus(s, msec);
    }
    if (animating || fx_animating || ov_animating)
        wlr_output_schedule_frame(o->wlr_output);
    o->was_animating = animating;
}

static void
handle_request_state(void *owner, void *data)
{
    CompOutput *o = owner;
    const struct wlr_output_event_request_state *ev = data;
    if (!o->wlr_output || !ev || !ev->state)
        return;
    if (!wlr_output_test_state(o->wlr_output, ev->state) ||
        !wlr_output_commit_state(o->wlr_output, ev->state)) {
        wlr_log(WLR_ERROR, "output %s: requested state was rejected", o->wlr_output->name);
        return;
    }
    comp_output_post_change(o->server);
}

static void
handle_destroy(void *owner, void *data)
{
    CompOutput *o = owner;
    CompServer *s = o->server;
    (void)data;
    wlr_log(WLR_INFO, "output %s removed", o->wlr_output ? o->wlr_output->name : "unknown");
    talloc_free(o);   /* unlinks all listeners and the list link */
    if (!s || !s->scene)
        return;
    /* Spec §3.13 OUT3: select replacement output (if any) and transfer workspaces intact */
    comp_output_apply_config(s);
    comp_output_post_change(s);
}

_Static_assert((int)COMP_TRANSFORM_NORMAL      == (int)WL_OUTPUT_TRANSFORM_NORMAL &&
               (int)COMP_TRANSFORM_90          == (int)WL_OUTPUT_TRANSFORM_90 &&
               (int)COMP_TRANSFORM_180         == (int)WL_OUTPUT_TRANSFORM_180 &&
               (int)COMP_TRANSFORM_270         == (int)WL_OUTPUT_TRANSFORM_270 &&
               (int)COMP_TRANSFORM_FLIPPED     == (int)WL_OUTPUT_TRANSFORM_FLIPPED &&
               (int)COMP_TRANSFORM_FLIPPED_90  == (int)WL_OUTPUT_TRANSFORM_FLIPPED_90 &&
               (int)COMP_TRANSFORM_FLIPPED_180 == (int)WL_OUTPUT_TRANSFORM_FLIPPED_180 &&
               (int)COMP_TRANSFORM_FLIPPED_270 == (int)WL_OUTPUT_TRANSFORM_FLIPPED_270,
               "CompTransform and wl_output_transform values must match");

static enum wl_output_transform
wl_transform_from_idx(int idx)
{
    if (idx >= COMP_TRANSFORM_NORMAL && idx <= COMP_TRANSFORM_FLIPPED_270)
        return (enum wl_output_transform)idx;
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

void
comp_output_update_vrr(CompServer *s)
{
    if (!s)
        return;
    bool wants = comp_window_focused_wants_vrr(s);
    s->last_wants_vrr = wants;
    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_output *wo = o->wlr_output;
        if (!wo || !wo->enabled)
            continue;
        bool desired = (o->vrr_policy == COMP_VRR_ON) ||
                       (o->vrr_policy == COMP_VRR_ON_DEMAND && wants);
        bool current = (wo->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
        if (desired == current || (desired && o->vrr_unsupported))
            continue;
        struct wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_set_adaptive_sync_enabled(&st, desired);
        if (wlr_output_test_state(wo, &st) && wlr_output_commit_state(wo, &st)) {
            if (desired)
                o->vrr_unsupported = false;
        } else if (desired) {
            o->vrr_unsupported = true;
        }
        wlr_output_state_finish(&st);
    }
}

/* Spec §6.5.3: Greeter Mode Deterministic Integer Scale Rule.
 * Assigns integer scale 2 if internal panel (eDP-*) with vertical resolution >= 1600 px
 * or physical DPI >= 170, and 1 otherwise.
 */
static double
greeter_default_scale(const struct wlr_output *wo)
{
    if (!wo || !is_internal_panel_name(wo->name))
        return 1.0;
    const struct wlr_output_mode *pm = wlr_output_preferred_mode((struct wlr_output *)wo);
    int w = pm ? pm->width  : wo->width;
    int h = pm ? pm->height : wo->height;
    if (h >= 1600)
        return 2.0;
    if (wo->phys_width > 0 && w > 0) {
        double dpi = ((double)w * 25.4) / (double)wo->phys_width;
        if (dpi >= 170.0)
            return 2.0;
    }
    return 1.0;
}

static void
disarm_hdr_auto_timer(CompOutput *o)
{
    if (!o)
        return;
    if (o->hdr_auto_timer)
        wl_event_source_timer_update(o->hdr_auto_timer, 0);
    o->hdr_auto_timer_armed = false;
}

static void
output_refresh_hdr_caps(CompOutput *o)
{
    if (!o || !o->server || !o->wlr_output)
        return;
    CompServer *s = o->server;
    struct wlr_output *wo = o->wlr_output;

    CompFxOutputHdrCaps caps = {0};
    bool caps_ok = comp_fx_output_get_hdr_caps(wo, &caps);
    o->hdr_edid_max_cll  = caps.max_cll_nits;
    o->hdr_edid_max_fall = caps.max_fall_nits;
    o->hdr_edid_min_luma = caps.min_luma_nits;

    bool has_renderer_ct = s->renderer &&
                           s->renderer->features.input_color_transform &&
                           s->renderer->features.output_color_transform;
    if (!has_renderer_ct) {
        o->hdr_supported = false;
        o->hdr_reason    = "no_vulkan_color_transform";
    } else if (!caps_ok || !caps.supported) {
        o->hdr_supported = false;
        o->hdr_reason    = "no_edid_support";
    } else {
        o->hdr_supported = true;
        o->hdr_reason    = o->hdr_kms_rejected ? "kms_rejected" : "";
    }

    double cfg_sdr_white = s->config ? s->config->fx.sdr_white_nits : 0.0;
    CompFxHdrLuminance lum;
    comp_fx_clamp_hdr_luminance(caps.max_cll_nits, caps.max_fall_nits, caps.min_luma_nits,
                                o->hdr_peak_nits, cfg_sdr_white, &lum);
    o->hdr_effective_peak_nits = lum.effective_peak_nits;
    o->hdr_max_fall_nits       = lum.max_fall_nits;
    o->hdr_min_luma_nits       = lum.min_luma_nits;
}

static void
build_hdr10_image_description(const CompOutput *o,
                              struct wlr_output_image_description *out)
{
    float peak = (o && o->hdr_effective_peak_nits >= 100.0f) ? o->hdr_effective_peak_nits : 400.0f;
    float fall = (o && o->hdr_max_fall_nits >= 50.0f) ? fminf(o->hdr_max_fall_nits, peak) : peak;
    float min_l = (o && o->hdr_min_luma_nits > 0.0f) ? o->hdr_min_luma_nits : 0.0004f;
    *out = (struct wlr_output_image_description){
        .primaries         = WLR_COLOR_NAMED_PRIMARIES_BT2020,
        .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
        .mastering_display_primaries = {
            .red   = { 0.708f,  0.292f },
            .green = { 0.170f,  0.797f },
            .blue  = { 0.131f,  0.046f },
            .white = { 0.3127f, 0.3290f },
        },
        .mastering_luminance = {
            .min = min_l,
            .max = peak,
        },
        .max_cll  = (unsigned)lroundf(peak),
        .max_fall = (unsigned)lroundf(fall),
    };
}

static void
log_and_notify_hdr_transition(CompOutput *o)
{
    if (!o || !o->server || !o->wlr_output)
        return;
    struct wlr_output *wo = o->wlr_output;
    float sdr_w = comp_fx_output_get_sdr_white_nits(wo);
    float headroom = comp_fx_output_get_hdr_headroom(wo);
    wlr_log(WLR_INFO,
            "output %s: HDR10 %s (policy=%s, edid=[%.1f/%.1f/%.4f], eff_peak=%.1f, "
            "sdr_white=%.1f, headroom=%.2f, max_cll=%.0f, max_fall=%.0f, reason=%s)",
            wo->name ? wo->name : "unknown",
            o->hdr_active ? "enabled" : "disabled",
            comp_hdr_policy_to_str(o->hdr_policy),
            o->hdr_edid_max_cll, o->hdr_edid_max_fall, o->hdr_edid_min_luma,
            o->hdr_effective_peak_nits,
            sdr_w > 0.0f ? sdr_w : 203.0f,
            headroom,
            o->hdr_effective_peak_nits, o->hdr_max_fall_nits,
            (o->hdr_reason && *o->hdr_reason) ? o->hdr_reason : "none");
    comp_protocols_update_preferred_image_descriptions(o->server);
}

static bool
try_test_and_commit_with_format(CompOutput *o, const struct wlr_output_mode *mode,
                                int custom_w, int custom_h, int32_t custom_mhz,
                                double scale, int transform, int vrr_enable,
                                bool want_hdr, uint32_t render_fmt,
                                bool apply_render_fmt, bool set_img_desc)
{
    CompServer *s = o->server;
    struct wlr_output *wo = o->wlr_output;
    bool prev_hdr_active = o->hdr_active;
    struct wlr_output_image_description img_desc;
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    if (mode)
        wlr_output_state_set_mode(&st, (struct wlr_output_mode *)mode);
    else if (custom_w > 0 && custom_h > 0)
        wlr_output_state_set_custom_mode(&st, custom_w, custom_h, custom_mhz);
    if (scale > 0.0)
        wlr_output_state_set_scale(&st, (float)scale);
    if (transform >= 0 && transform <= COMP_TRANSFORM_FLIPPED_270)
        wlr_output_state_set_transform(&st, wl_transform_from_idx(transform));
    if (vrr_enable >= 0)
        wlr_output_state_set_adaptive_sync_enabled(&st, vrr_enable != 0);

    if (want_hdr) {
        build_hdr10_image_description(o, &img_desc);
        wlr_output_state_set_render_format(&st, render_fmt);
        wlr_output_state_set_image_description(&st, &img_desc);
        o->hdr_active = true;
    } else if (set_img_desc) {
        if (apply_render_fmt)
            wlr_output_state_set_render_format(&st, render_fmt);
        wlr_output_state_set_image_description(&st, NULL);
        o->hdr_active = false;
    }

    if ((want_hdr || set_img_desc) && o->scene_output) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double now_s = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
        (void)comp_fx_output_prepare(o, now_s);
        struct wlr_scene_output_state_options scene_opts = {
            .color_transform = comp_fx_output_get_color_transform(o),
        };
        const struct wlr_scene_output_state_options *opts_ptr =
            scene_opts.color_transform ? &scene_opts : NULL;
        if (!wlr_scene_output_build_state(o->scene_output, &st, opts_ptr)) {
            o->hdr_active = prev_hdr_active;
            wlr_output_state_finish(&st);
            return false;
        }
    }

    bool ok = wlr_output_test_state(wo, &st) && wlr_output_commit_state(wo, &st);
    wlr_output_state_finish(&st);
    if (!ok) {
        o->hdr_active = prev_hdr_active;
        return false;
    }
    if (want_hdr) {
        o->hdr_kms_rejected = false;
        o->hdr_reason       = "";
    }
    if (s && set_img_desc && !want_hdr)
        o->hdr_active = false;
    return true;
}

static bool
try_test_and_commit(CompOutput *o, const struct wlr_output_mode *mode,
                    int custom_w, int custom_h, int32_t custom_mhz,
                    double scale, int transform, int vrr_enable, bool want_hdr)
{
    struct wlr_output *wo = o->wlr_output;
    if (want_hdr) {
        static const uint32_t k_hdr10_fmts[] = {
            DRM_FORMAT_XRGB2101010,
            DRM_FORMAT_XBGR2101010,
        };
        for (size_t i = 0; i < sizeof(k_hdr10_fmts) / sizeof(k_hdr10_fmts[0]); i++) {
            if (try_test_and_commit_with_format(o, mode, custom_w, custom_h, custom_mhz,
                                                scale, transform, vrr_enable, true,
                                                k_hdr10_fmts[i], true, true)) {
                return true;
            }
        }
        return false;
    }

    if (o->hdr_active || wo->image_description != NULL) {
        if (try_test_and_commit_with_format(o, mode, custom_w, custom_h, custom_mhz,
                                            scale, transform, vrr_enable, false,
                                            DRM_FORMAT_XRGB8888, true, true)) {
            return true;
        }
        return try_test_and_commit_with_format(o, mode, custom_w, custom_h, custom_mhz,
                                               scale, transform, vrr_enable, false,
                                               DRM_FORMAT_INVALID, false, true);
    }

    return try_test_and_commit_with_format(o, mode, custom_w, custom_h, custom_mhz,
                                           scale, transform, vrr_enable, false,
                                           DRM_FORMAT_INVALID, false, false);
}

/* Spec §7.3.1 & spec_comp_hdr.md §3.3: Atomic KMS Test-Before-Commit Cascade.
 * Tests full requested state (including 10-bit PQ HDR10 when requested) first;
 * on failure steps down: vrr="off" -> hdr="off" (SDR 8-bit) -> lower refresh
 * -> preferred mode -> scale/transform only.
 */
static bool
commit_output_cascade(CompOutput *o, const char *mode_str, double scale,
                      int transform, int vrr_policy)
{
    CompServer *s = o->server;
    struct wlr_output *wo = o->wlr_output;
    if (!wo)
        return false;

    if (vrr_policy >= 0 && vrr_policy <= COMP_VRR_ON_DEMAND)
        o->vrr_policy = (CompVrrPolicy)vrr_policy;
    o->vrr_unsupported = false;

    output_refresh_hdr_caps(o);
    if (o->hdr_policy != COMP_HDR_AUTO)
        disarm_hdr_auto_timer(o);

    bool prev_hdr_active = o->hdr_active;
    bool want_hdr = false;
    if (o->hdr_supported) {
        if (o->hdr_policy == COMP_HDR_ON)
            want_hdr = true;
        else if (o->hdr_policy == COMP_HDR_AUTO)
            want_hdr = comp_window_has_visible_hdr_surface(s) ||
                       (o->hdr_active && o->hdr_auto_timer_armed);
    }

    const struct wlr_output_mode *req_mode = NULL;
    int req_w = 0, req_h = 0;
    int32_t req_mhz = 0;
    bool has_explicit_mode = false;

    if (mode_str && *mode_str) {
        if (strcasecmp(mode_str, COMP_VAL_PREFERRED) == 0) {
            req_mode = wlr_output_preferred_mode(wo);
            has_explicit_mode = (req_mode != NULL);
        } else {
            double mhz_d = 0.0;
            int n = sscanf(mode_str, "%dx%d@%lf", &req_w, &req_h, &mhz_d);
            if (n >= 2 && req_w > 0 && req_h > 0) {
                has_explicit_mode = true;
                req_mhz = (n == 3 && mhz_d > 0.0) ? (int32_t)round(mhz_d * 1000.0) : 0;
                int32_t best_diff = 0x7fffffff;
                const struct wlr_output_mode *m;
                wl_list_for_each(m, &wo->modes, link) {
                    if (m->width != req_w || m->height != req_h)
                        continue;
                    if (req_mhz == 0) {
                        if (!req_mode || m->preferred || m->refresh > req_mode->refresh)
                            req_mode = m;
                    } else {
                        int32_t diff = abs(m->refresh - req_mhz);
                        if (diff < best_diff) {
                            best_diff = diff;
                            req_mode = m;
                        }
                    }
                }
            }
        }
    } else if (!wo->enabled) {
        req_mode = wlr_output_preferred_mode(wo);
    }

    bool want_vrr = (o->vrr_policy == COMP_VRR_ON) ||
                    (o->vrr_policy == COMP_VRR_ON_DEMAND && comp_window_focused_wants_vrr(s));
    int vrr_arg = want_vrr ? 1 : -1;

    /* Step 1: Full requested state (with HDR10 if requested) */
    if (try_test_and_commit(o, req_mode, req_w, req_h, req_mhz, scale, transform, vrr_arg, want_hdr)) {
        if (o->hdr_active != prev_hdr_active)
            log_and_notify_hdr_transition(o);
        return true;
    }

    bool stepped_down = false;

    /* Step 2: Step down VRR -> "off" (keep HDR10 if requested) */
    if (want_vrr && try_test_and_commit(o, req_mode, req_w, req_h, req_mhz, scale, transform, 0, want_hdr)) {
        o->vrr_policy      = COMP_VRR_OFF;
        o->vrr_unsupported = true;
        stepped_down       = true;
        goto out_ok;
    }

    /* Step 3: Step down HDR10 -> SDR 8-bit at the requested mode */
    if (want_hdr && try_test_and_commit(o, req_mode, req_w, req_h, req_mhz, scale, transform, vrr_arg, false)) {
        o->hdr_kms_rejected = true;
        o->hdr_reason       = "kms_rejected";
        stepped_down        = true;
        goto out_ok;
    }

    /* Step 4: Step down to lower refresh rate at the same WxH resolution (SDR) */
    if (req_w > 0 && req_h > 0 && !wl_list_empty(&wo->modes)) {
        int32_t ceil_mhz = req_mode ? req_mode->refresh : (req_mhz > 0 ? req_mhz : 0x7fffffff);
        for (;;) {
            const struct wlr_output_mode *cand = NULL;
            const struct wlr_output_mode *m;
            wl_list_for_each(m, &wo->modes, link) {
                if (m->width == req_w && m->height == req_h && m->refresh < ceil_mhz) {
                    if (!cand || m->refresh > cand->refresh)
                        cand = m;
                }
            }
            if (!cand)
                break;
            ceil_mhz = cand->refresh;
            if (try_test_and_commit(o, cand, 0, 0, 0, scale, transform, -1, false)) {
                if (want_hdr) {
                    o->hdr_kms_rejected = true;
                    o->hdr_reason       = "kms_rejected";
                }
                stepped_down = true;
                goto out_ok;
            }
        }
    }

    /* Step 5: Step down to preferred mode (SDR) */
    const struct wlr_output_mode *pm = wlr_output_preferred_mode(wo);
    if (pm && try_test_and_commit(o, pm, 0, 0, 0, scale, transform, -1, false)) {
        if (want_hdr) {
            o->hdr_kms_rejected = true;
            o->hdr_reason       = "kms_rejected";
        }
        stepped_down = has_explicit_mode || want_hdr;
        goto out_ok;
    }

    /* Step 6: Apply scale/transform on current mode (SDR) */
    if (try_test_and_commit(o, NULL, 0, 0, 0, scale, transform, -1, false)) {
        if (want_hdr) {
            o->hdr_kms_rejected = true;
            o->hdr_reason       = "kms_rejected";
        }
        stepped_down = has_explicit_mode || want_hdr;
        goto out_ok;
    }

    return false;

out_ok:
    if (o->hdr_active != prev_hdr_active)
        log_and_notify_hdr_transition(o);
    if (stepped_down) {
        if (o->has_manual_mode && wo->width > 0 && wo->height > 0) {
            char fallback_mode[64];
            if (wo->refresh > 0) {
                snprintf(fallback_mode, sizeof(fallback_mode), "%dx%d@%.3f",
                         wo->width, wo->height, (double)wo->refresh / 1000.0);
            } else {
                snprintf(fallback_mode, sizeof(fallback_mode), "%dx%d", wo->width, wo->height);
            }
            talloc_free(o->manual_mode);
            o->manual_mode     = talloc_strdup(o, fallback_mode);
            o->has_manual_mode = (o->manual_mode != NULL);
        }
        if (o->has_manual_vrr)
            o->manual_vrr = o->vrr_policy;
        wlr_log(WLR_ERROR, "output %s: requested KMS state rejected; stepped down to %dx%d @ %.2f Hz (vrr=%s, hdr=%s)",
                wo->name ? wo->name : "unknown", wo->width, wo->height,
                (double)wo->refresh / 1000.0, comp_vrr_policy_to_str(o->vrr_policy),
                o->hdr_active ? "on" : "off");
        if (s->opts.session_mode && !s->opts.greeter_mode && !getenv("KAJO_COMP_NO_DBUS_ENV")) {
            const char *const notify_argv[] = {
                "notify-send", "-a", "kajo-comp",
                "Display Configuration Stepped Down",
                "Requested display mode, VRR, or HDR was rejected by hardware; using safe fallback mode.",
                NULL,
            };
            comp_spawn(s, notify_argv);
        }
    }
    return true;
}

static int
handle_hdr_auto_timer(void *data)
{
    CompOutput *o = data;
    if (!o || !o->server || !o->wlr_output)
        return 0;
    o->hdr_auto_timer_armed = false;
    if (!o->wlr_output->enabled || o->hdr_policy != COMP_HDR_AUTO || !o->hdr_active)
        return 0;
    if (comp_window_has_visible_hdr_surface(o->server))
        return 0;

    if (try_test_and_commit(o, NULL, 0, 0, 0, -1.0, -1, -1, false)) {
        log_and_notify_hdr_transition(o);
        comp_ipc_emit_outputs(o->server);
    }
    return 0;
}

void
comp_output_update_hdr_auto(CompServer *s)
{
    if (!s || wl_list_empty(&s->outputs))
        return;

    bool have_hdr_surface = false;
    bool checked_surfaces = false;
    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_output *wo = o->wlr_output;
        if (!wo || !wo->enabled || !o->hdr_supported || o->hdr_policy != COMP_HDR_AUTO) {
            if (o->hdr_auto_timer_armed)
                disarm_hdr_auto_timer(o);
            continue;
        }
        if (!checked_surfaces) {
            have_hdr_surface = comp_window_has_visible_hdr_surface(s);
            checked_surfaces = true;
        }
        if (have_hdr_surface) {
            if (o->hdr_auto_timer_armed)
                disarm_hdr_auto_timer(o);
            if (!o->hdr_active && !o->hdr_kms_rejected) {
                if (try_test_and_commit(o, NULL, 0, 0, 0, -1.0, -1, -1, true)) {
                    log_and_notify_hdr_transition(o);
                    comp_ipc_emit_outputs(s);
                } else {
                    o->hdr_kms_rejected = true;
                    o->hdr_reason       = "kms_rejected";
                    comp_ipc_emit_outputs(s);
                }
            }
        } else if (o->hdr_active && !o->hdr_auto_timer_armed) {
            if (!o->hdr_auto_timer && s->loop)
                o->hdr_auto_timer = wl_event_loop_add_timer(s->loop, handle_hdr_auto_timer, o);
            if (o->hdr_auto_timer) {
                wl_event_source_timer_update(o->hdr_auto_timer, 2000);
                o->hdr_auto_timer_armed = true;
            }
        }
    }
}

bool
comp_output_configure(CompServer *s, const char *output_name,
                      const char *mode_str, double scale, int transform,
                      int vrr_policy, int hdr_policy, int hdr_peak_nits)
{
    if (!s || wl_list_empty(&s->outputs))
        return false;

    CompOutput *target = NULL;
    if (!output_name || !*output_name) {
        target = select_primary_output(s);
    } else {
        CompOutput *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (o->wlr_output && o->wlr_output->name &&
                strcasecmp(o->wlr_output->name, output_name) == 0) {
                target = o;
                break;
            }
        }
    }
    if (!target || !target->wlr_output)
        return false;

    if (mode_str && *mode_str) {
        talloc_free(target->manual_mode);
        target->manual_mode = talloc_strdup(target, mode_str);
        target->has_manual_mode = (target->manual_mode != NULL);
    }
    if (scale > 0.0) {
        target->has_manual_scale = true;
        target->manual_scale     = scale;
    }
    if (transform >= 0 && transform <= COMP_TRANSFORM_FLIPPED_270) {
        target->has_manual_transform = true;
        target->manual_transform     = (CompTransform)transform;
    }
    if (vrr_policy >= 0 && vrr_policy <= COMP_VRR_ON_DEMAND) {
        target->has_manual_vrr = true;
        target->manual_vrr     = (CompVrrPolicy)vrr_policy;
    }
    if (hdr_policy >= 0 && hdr_policy <= COMP_HDR_AUTO) {
        target->has_manual_hdr   = true;
        target->manual_hdr       = (CompHdrPolicy)hdr_policy;
        target->hdr_policy       = (CompHdrPolicy)hdr_policy;
        target->hdr_kms_rejected = false;
    }
    if (hdr_peak_nits >= 0) {
        int32_t clamped_peak = (hdr_peak_nits == 0)
            ? 0
            : (hdr_peak_nits < 100 ? 100 : (hdr_peak_nits > 10000 ? 10000 : hdr_peak_nits));
        target->has_manual_hdr_peak_nits = true;
        target->hdr_manual_peak_nits     = clamped_peak;
        target->hdr_peak_nits            = clamped_peak;
        target->hdr_kms_rejected         = false;
    }

    bool ok = commit_output_cascade(target, mode_str, scale, transform, vrr_policy);
    comp_output_update_vrr(s);
    sync_primary_output_devices(s);
    comp_output_post_change(s);
    return ok;
}

bool
comp_output_configure_hdr(CompServer *s, const char *output_name,
                          int hdr_policy, int hdr_peak_nits)
{
    return comp_output_configure(s, output_name, NULL, -1.0, -1, -1,
                                 hdr_policy, hdr_peak_nits);
}

bool
comp_output_set_brightness(CompServer *s, const char *output_name, float brightness)
{
    if (!s || wl_list_empty(&s->outputs))
        return false;
    if (!isfinite(brightness) || brightness < 0.0f || brightness > 1.0f)
        return false;

    CompOutput *target = NULL;
    if (!output_name || !*output_name) {
        target = select_primary_output(s);
    } else {
        CompOutput *o;
        wl_list_for_each(o, &s->outputs, link) {
            if (o->wlr_output && o->wlr_output->name &&
                strcasecmp(o->wlr_output->name, output_name) == 0) {
                target = o;
                break;
            }
        }
    }
    if (!target || !target->wlr_output)
        return false;

    float b = brightness < 0.05f ? 0.05f : (brightness > 1.0f ? 1.0f : brightness);
    target->hdr_brightness = b;
    (void)comp_fx_output_prepare(target, comp_now_sec());
    if (target->wlr_output->enabled)
        wlr_output_schedule_frame(target->wlr_output);
    comp_ipc_emit_outputs(s);
    return true;
}

void
comp_output_apply_config(CompServer *s)
{
    if (!s || wl_list_empty(&s->outputs))
        return;

    CompOutput *primary = s->opts.greeter_mode ? NULL : select_primary_output(s);

    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_output *wo = o->wlr_output;
        if (!wo)
            continue;

        bool should_enable = s->opts.greeter_mode || (o == primary);
        if (!should_enable) {
            if (wlr_output_layout_get(s->output_layout, wo)) {
                wlr_output_layout_remove(s->output_layout, wo);
                o->scene_output = wlr_scene_get_scene_output(s->scene, wo);
            }
            if (wo->enabled) {
                struct wlr_output_state st;
                wlr_output_state_init(&st);
                wlr_output_state_set_enabled(&st, false);
                if (wlr_output_test_state(wo, &st))
                    wlr_output_commit_state(wo, &st);
                wlr_output_state_finish(&st);
            }
            continue;
        }

        const CompOutputRule *r = find_matching_output_rule(s, wo);
        const char *mode_str = o->has_manual_mode ? o->manual_mode
                                                  : ((r && r->has_mode) ? r->mode : NULL);
        double scale = o->has_manual_scale ? o->manual_scale
                                           : ((r && r->has_scale) ? r->scale
                                                                  : (s->opts.greeter_mode ? greeter_default_scale(wo) : -1.0));
        int tf = o->has_manual_transform ? (int)o->manual_transform
                                         : ((r && r->has_transform) ? (int)r->transform : -1);
        int vrr = o->has_manual_vrr ? (int)o->manual_vrr
                                    : ((r && r->has_vrr) ? (int)r->vrr : -1);
        o->hdr_policy = o->has_manual_hdr ? o->manual_hdr
                                          : ((r && r->has_hdr) ? r->hdr : COMP_HDR_OFF);
        o->hdr_peak_nits = o->has_manual_hdr_peak_nits ? o->hdr_manual_peak_nits
                                                      : ((r && r->has_hdr_peak_nits) ? r->hdr_peak_nits : 0);
        o->hdr_kms_rejected = false;

        if (!commit_output_cascade(o, mode_str, scale, tf, vrr)) {
            wlr_log(WLR_ERROR, "output %s: failed to enable output", wo->name);
            continue;
        }

        o->scene_output = wlr_scene_get_scene_output(s->scene, wo);
        if (!o->scene_output)
            o->scene_output = wlr_scene_output_create(s->scene, wo);
        if (!wlr_output_layout_get(s->output_layout, wo)) {
            struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(s->output_layout, wo);
            if (lo && o->scene_output)
                wlr_scene_output_layout_add_output(s->scene_layout, lo, o->scene_output);
        }
    }

    comp_output_update_vrr(s);
    sync_primary_output_devices(s);
    update_output_manager_config(s);
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

    CompOutput *o = talloc_zero(s, CompOutput);
    COMP_WARN_RETURN_VOID(o != NULL);
    talloc_set_name_const(o, "CompOutput");
    o->server = s;
    o->wlr_output = wo;
    o->hdr_brightness = 1.0f;
    /* Insert at tail to preserve connector arrival order (OUT2 & §6.5.3) */
    wl_list_insert(s->outputs.prev, &o->link);
    talloc_set_destructor(o, output_destructor);

    o->scene_output = wlr_scene_output_create(s->scene, wo);
    if (!o->scene_output ||
        !comp_listen(o, &wo->events.frame, handle_frame) ||
        !comp_listen(o, &wo->events.present, handle_present) ||
        !comp_listen(o, &wo->events.request_state, handle_request_state) ||
        !comp_listen(o, &wo->events.destroy, handle_destroy)) {
        talloc_free(o);
        return;
    }

    comp_output_apply_config(s);
    if (s->cursor && s->cursor_mgr && (!s->seat || !s->seat->pointer_state.focused_surface))
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
    wlr_log(WLR_INFO, "output %s connected (%s, %dx%d @ %.3f Hz, scale %.2f)",
            wo->name, wo->enabled ? "active" : "standby",
            wo->width, wo->height, wo->refresh / 1000.0, (double)wo->scale);
    comp_output_post_change(s);
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
