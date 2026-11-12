/* comp_protocols.c — OSK input-method/text-input/virtual-keyboard relay,
 * pointer-constraints-v1, relative-pointer-v1, pointer-gestures-v1,
 * xdg-foreign-v2, xdg-dialog-v1, xdg-activation-v1, keyboard-shortcuts-inhibit-v1,
 * tablet-v2, screencopy/ext-image-copy-capture, and color management globals (Phase 2c).
 */
#include "comp_server.h"

#include "comp_log.h"

#include <stdlib.h>
#include <talloc.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xcursor_manager.h>

typedef struct {
    CompServer                  *server;
    struct wlr_text_input_v3    *ti;
} CompTextInput;

typedef struct {
    CompServer                  *server;
    struct wlr_input_method_v2  *im;
} CompInputMethod;

typedef struct {
    CompServer                         *server;
    struct wlr_input_popup_surface_v2  *popup;
    struct wlr_scene_tree              *scene_tree;
} CompInputPopup;

typedef struct {
    CompServer                        *server;
    struct wlr_pointer_constraint_v1  *constraint;
} CompPointerConstraint;

typedef struct {
    CompServer                                  *server;
    struct wlr_keyboard_shortcuts_inhibitor_v1  *inhibitor;
} CompShortcutsInhibitor;

/* ── Text-input-v3 & Input-method-v2 relay ── */

static struct wlr_text_input_v3 *
find_active_text_input(CompServer *s)
{
    if (!s || !s->text_input_mgr)
        return NULL;
    struct wlr_text_input_v3 *ti;
    wl_list_for_each(ti, &s->text_input_mgr->text_inputs, link) {
        if (ti->focused_surface && ti->current_enabled)
            return ti;
    }
    return NULL;
}

static void
relay_text_input_to_im(CompServer *s, struct wlr_text_input_v3 *ti)
{
    struct wlr_input_method_v2 *im = s->active_input_method;
    if (!im || !ti)
        return;

    if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
        wlr_input_method_v2_send_surrounding_text(
            im,
            ti->current.surrounding.text ? ti->current.surrounding.text : "",
            ti->current.surrounding.cursor,
            ti->current.surrounding.anchor);
    }
    wlr_input_method_v2_send_text_change_cause(im, ti->current.text_change_cause);
    if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
        wlr_input_method_v2_send_content_type(
            im, ti->current.content_type.hint, ti->current.content_type.purpose);
    }
    wlr_input_method_v2_send_done(im);
}

static void
handle_text_input_enable(void *owner, void *data)
{
    CompTextInput *cti = owner;
    CompServer *s = cti->server;
    (void)data;
    if (s->active_input_method) {
        wlr_input_method_v2_send_activate(s->active_input_method);
        relay_text_input_to_im(s, cti->ti);
    }
}

static void
handle_text_input_commit(void *owner, void *data)
{
    CompTextInput *cti = owner;
    CompServer *s = cti->server;
    (void)data;
    if (cti->ti->current_enabled)
        relay_text_input_to_im(s, cti->ti);
}

static void
handle_text_input_disable(void *owner, void *data)
{
    CompTextInput *cti = owner;
    CompServer *s = cti->server;
    (void)data;
    if (s->active_input_method) {
        wlr_input_method_v2_send_deactivate(s->active_input_method);
        wlr_input_method_v2_send_done(s->active_input_method);
    }
}

static void
handle_text_input_destroy(void *owner, void *data)
{
    CompTextInput *cti = owner;
    (void)data;
    talloc_free(cti);
}

static void
handle_new_text_input(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_text_input_v3 *ti = data;

    CompTextInput *cti = talloc_zero(s, CompTextInput);
    COMP_WARN_RETURN_VOID(cti != NULL);
    cti->server = s;
    cti->ti     = ti;

    if (!comp_listen(cti, &ti->events.enable, handle_text_input_enable) ||
        !comp_listen(cti, &ti->events.commit, handle_text_input_commit) ||
        !comp_listen(cti, &ti->events.disable, handle_text_input_disable) ||
        !comp_listen(cti, &ti->events.destroy, handle_text_input_destroy)) {
        talloc_free(cti);
        return;
    }

    struct wlr_surface *focused = s->seat ? s->seat->keyboard_state.focused_surface : NULL;
    if (focused && wl_resource_get_client(ti->resource) == wl_resource_get_client(focused->resource))
        wlr_text_input_v3_send_enter(ti, focused);
}

static int
input_popup_destructor(CompInputPopup *cip)
{
    if (cip->server->scene && cip->scene_tree) {
        wlr_scene_node_destroy(&cip->scene_tree->node);
        cip->scene_tree = NULL;
    }
    return 0;
}

static void
handle_input_popup_destroy(void *owner, void *data)
{
    CompInputPopup *cip = owner;
    (void)data;
    talloc_free(cip);
}

static void
handle_im_new_popup_surface(void *owner, void *data)
{
    CompInputMethod *cim = owner;
    CompServer *s = cim->server;
    struct wlr_input_popup_surface_v2 *popup = data;

    CompInputPopup *cip = talloc_zero(cim, CompInputPopup);
    COMP_WARN_RETURN_VOID(cip != NULL);
    cip->server = s;
    cip->popup  = popup;
    talloc_set_destructor(cip, input_popup_destructor);

    cip->scene_tree = wlr_scene_subsurface_tree_create(s->tree_overlay, popup->surface);
    if (!comp_listen(cip, &popup->events.destroy, handle_input_popup_destroy))
        talloc_free(cip);
}

static void
handle_im_commit(void *owner, void *data)
{
    CompInputMethod *cim = owner;
    CompServer *s = cim->server;
    struct wlr_input_method_v2 *im = cim->im;
    (void)data;

    struct wlr_text_input_v3 *ti = find_active_text_input(s);
    if (!ti)
        return;

    if (im->current.preedit.text) {
        wlr_text_input_v3_send_preedit_string(ti, im->current.preedit.text,
                                              im->current.preedit.cursor_begin,
                                              im->current.preedit.cursor_end);
    }
    if (im->current.commit_text)
        wlr_text_input_v3_send_commit_string(ti, im->current.commit_text);
    if (im->current.delete.before_length || im->current.delete.after_length) {
        wlr_text_input_v3_send_delete_surrounding_text(
            ti, im->current.delete.before_length, im->current.delete.after_length);
    }
    wlr_text_input_v3_send_done(ti);
}

static void
handle_im_destroy(void *owner, void *data)
{
    CompInputMethod *cim = owner;
    CompServer *s = cim->server;
    (void)data;
    if (s->active_input_method == cim->im)
        s->active_input_method = NULL;
    talloc_free(cim);
}

static void
handle_new_input_method(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_input_method_v2 *im = data;

    if (s->active_input_method) {
        wlr_input_method_v2_send_unavailable(im);
        return;
    }

    CompInputMethod *cim = talloc_zero(s, CompInputMethod);
    if (!cim) {
        wlr_input_method_v2_send_unavailable(im);
        return;
    }
    cim->server = s;
    cim->im     = im;
    s->active_input_method = im;

    if (!comp_listen(cim, &im->events.commit, handle_im_commit) ||
        !comp_listen(cim, &im->events.new_popup_surface, handle_im_new_popup_surface) ||
        !comp_listen(cim, &im->events.destroy, handle_im_destroy)) {
        s->active_input_method = NULL;
        talloc_free(cim);
    }
}

void
comp_protocols_notify_focus_surface(CompServer *s, struct wlr_surface *surface)
{
    if (!s || !s->text_input_mgr)
        return;

    struct wlr_text_input_v3 *ti;
    wl_list_for_each(ti, &s->text_input_mgr->text_inputs, link) {
        if (ti->focused_surface && ti->focused_surface != surface) {
            if (s->active_input_method && ti->current_enabled) {
                wlr_input_method_v2_send_deactivate(s->active_input_method);
                wlr_input_method_v2_send_done(s->active_input_method);
            }
            wlr_text_input_v3_send_leave(ti);
        }
    }

    if (!surface)
        return;

    wl_list_for_each(ti, &s->text_input_mgr->text_inputs, link) {
        if (!ti->focused_surface &&
            wl_resource_get_client(ti->resource) == wl_resource_get_client(surface->resource)) {
            wlr_text_input_v3_send_enter(ti, surface);
        }
    }
}

/* ── Virtual keyboard (ShellOSK) ── */

static void
handle_new_virtual_keyboard(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_virtual_keyboard_v1 *vkbd = data;
    comp_input_add_keyboard_device(s, &vkbd->keyboard.base);
}

/* ── Pointer constraints (pointer-constraints-v1) ── */

static void
handle_constraint_destroy(void *owner, void *data)
{
    CompPointerConstraint *cpc = owner;
    CompServer *s = cpc->server;
    (void)data;
    if (s->active_constraint == cpc->constraint)
        s->active_constraint = NULL;
    talloc_free(cpc);
}

static void
handle_new_pointer_constraint(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_constraint_v1 *constraint = data;

    CompPointerConstraint *cpc = talloc_zero(s, CompPointerConstraint);
    COMP_WARN_RETURN_VOID(cpc != NULL);
    cpc->server     = s;
    cpc->constraint = constraint;

    if (!comp_listen(cpc, &constraint->events.destroy, handle_constraint_destroy)) {
        talloc_free(cpc);
        return;
    }

    struct wlr_surface *focused = s->seat ? s->seat->pointer_state.focused_surface : NULL;
    if (focused && focused == constraint->surface)
        comp_protocols_check_pointer_constraint(s, focused);
}

void
comp_protocols_check_pointer_constraint(CompServer *s, struct wlr_surface *surface)
{
    if (!s || !s->pointer_constraints)
        return;

    struct wlr_pointer_constraint_v1 *want =
        surface ? wlr_pointer_constraints_v1_constraint_for_surface(s->pointer_constraints, surface, s->seat)
                : NULL;
    if (want == s->active_constraint)
        return;

    if (s->active_constraint) {
        struct wlr_pointer_constraint_v1 *old = s->active_constraint;
        s->active_constraint = NULL;
        wlr_pointer_constraint_v1_send_deactivated(old);
    }
    if (want) {
        s->active_constraint = want;
        wlr_pointer_constraint_v1_send_activated(want);
    }
}

/* ── Keyboard shortcuts inhibitor (keyboard-shortcuts-inhibit-v1) ── */

static void
handle_shortcuts_inhibitor_destroy(void *owner, void *data)
{
    CompShortcutsInhibitor *csi = owner;
    (void)data;
    talloc_free(csi);
}

static void
handle_new_shortcuts_inhibitor(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inh = data;

    CompShortcutsInhibitor *csi = talloc_zero(s, CompShortcutsInhibitor);
    COMP_WARN_RETURN_VOID(csi != NULL);
    csi->server    = s;
    csi->inhibitor = inh;

    wlr_keyboard_shortcuts_inhibitor_v1_activate(inh);
    if (!comp_listen(csi, &inh->events.destroy, handle_shortcuts_inhibitor_destroy))
        talloc_free(csi);
}

bool
comp_protocols_shortcuts_inhibited(CompServer *s)
{
    if (!s || !s->shortcuts_inhibit_mgr || !s->seat)
        return false;
    struct wlr_surface *focused = s->seat->keyboard_state.focused_surface;
    if (!focused)
        return false;

    struct wlr_keyboard_shortcuts_inhibitor_v1 *inh;
    wl_list_for_each(inh, &s->shortcuts_inhibit_mgr->inhibitors, link) {
        if (inh->active && inh->surface == focused)
            return true;
    }
    return false;
}

void
comp_protocols_break_shortcuts_inhibit(CompServer *s)
{
    if (!s || !s->shortcuts_inhibit_mgr)
        return;
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inh;
    wl_list_for_each(inh, &s->shortcuts_inhibit_mgr->inhibitors, link) {
        if (inh->active)
            wlr_keyboard_shortcuts_inhibitor_v1_deactivate(inh);
    }
}

/* ── xdg-activation-v1 ── */

static void
handle_xdg_activation_request(void *owner, void *data)
{
    CompServer *s = owner;
    const struct wlr_xdg_activation_v1_request_activate_event *ev = data;
    if (!s || s->locked || !ev || !ev->surface)
        return;

    struct wlr_xdg_toplevel *tl = wlr_xdg_toplevel_try_from_wlr_surface(ev->surface);
    if (tl && tl->base && tl->base->data)
        comp_window_request_activate(s, (struct CompWindow *)tl->base->data, ev->token);
}

/* ── Color management (SDR in v1) ── */

static void
init_color_management(CompServer *s)
{
    wlr_color_representation_manager_v1_create_with_renderer(s->display, 1, s->renderer);

    size_t n_tf = 0, n_prim = 0;
    enum wp_color_manager_v1_transfer_function *tfs =
        wlr_color_manager_v1_transfer_function_list_from_renderer(s->renderer, &n_tf);
    enum wp_color_manager_v1_primaries *prims =
        wlr_color_manager_v1_primaries_list_from_renderer(s->renderer, &n_prim);

    if (tfs && prims && n_tf > 0 && n_prim > 0) {
        static const enum wp_color_manager_v1_render_intent k_intents[] = {
            WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
        };
        struct wlr_color_manager_v1_options opts = {
            .features = {
                .parametric = true,
                .set_mastering_display_primaries = true,
            },
            .render_intents = k_intents,
            .render_intents_len = 1,
            .transfer_functions = tfs,
            .transfer_functions_len = n_tf,
            .primaries = prims,
            .primaries_len = n_prim,
        };
        struct wlr_color_manager_v1 *cm = wlr_color_manager_v1_create(s->display, 1, &opts);
        if (cm)
            wlr_scene_set_color_manager_v1(s->scene, cm);
    }
    free(tfs);
    free(prims);
}

/* ── xdg_popup and xdg_decoration_v1 ── */

typedef struct {
    CompServer            *server;
    struct wlr_xdg_popup  *popup;
    struct wlr_scene_tree *scene_tree;
} CompPopup;

typedef struct {
    CompServer                            *server;
    struct wlr_xdg_toplevel_decoration_v1 *dec;
} CompDecoration;

static struct CompWindow *
find_popup_root_window(struct wlr_xdg_popup *popup)
{
    struct wlr_surface *p = popup ? popup->parent : NULL;
    while (p) {
        struct wlr_xdg_surface *xs = wlr_xdg_surface_try_from_wlr_surface(p);
        if (!xs)
            break;
        if (xs->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xs->data)
            return (struct CompWindow *)xs->data;
        if (xs->role == WLR_XDG_SURFACE_ROLE_POPUP && xs->popup)
            p = xs->popup->parent;
        else
            break;
    }
    return NULL;
}

static void
unconstrain_popup(CompPopup *cp)
{
    if (!cp || !cp->popup || !cp->popup->base->initialized)
        return;

    double vw = 1920.0, vh = 1080.0;
    comp_server_viewport_size(cp->server, &vw, &vh);

    int lx = 0, ly = 0;
    struct CompWindow *root = find_popup_root_window(cp->popup);
    struct wlr_scene_node *xdg_node = NULL;
    if (root)
        comp_window_get_popup_trees(root, NULL, &xdg_node);
    if (xdg_node)
        wlr_scene_node_coords(xdg_node, &lx, &ly);

    struct wlr_box box = {
        .x      = -lx,
        .y      = -ly,
        .width  = (int)vw,
        .height = (int)vh,
    };
    wlr_xdg_popup_unconstrain_from_box(cp->popup, &box);
}

static int
popup_destructor(CompPopup *cp)
{
    if (cp->popup && cp->popup->base && cp->popup->base->data == cp)
        cp->popup->base->data = NULL;
    return 0;
}

static void
handle_popup_commit(void *owner, void *data)
{
    CompPopup *cp = owner;
    (void)data;
    if (cp->popup->base->initial_commit)
        unconstrain_popup(cp);
}

static void
handle_popup_reposition(void *owner, void *data)
{
    CompPopup *cp = owner;
    (void)data;
    unconstrain_popup(cp);
}

static void
handle_popup_destroy(void *owner, void *data)
{
    CompPopup *cp = owner;
    (void)data;
    talloc_free(cp);
}

static void
handle_new_popup(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_xdg_popup *popup = data;
    if (!popup || !popup->parent)
        return;

    /* Layer-shell popups are handled by comp_layer.c via lsurf->events.new_popup */
    struct wlr_xdg_surface *parent_xs = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (!parent_xs)
        return;

    struct wlr_scene_tree *parent_tree = NULL;
    if (parent_xs->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && parent_xs->data) {
        comp_window_get_popup_trees(parent_xs->data, &parent_tree, NULL);
    } else if (parent_xs->role == WLR_XDG_SURFACE_ROLE_POPUP && parent_xs->data) {
        CompPopup *pcp = parent_xs->data;
        parent_tree = pcp->scene_tree;
    }
    if (!parent_tree)
        return;

    CompPopup *cp = talloc_zero(s, CompPopup);
    COMP_WARN_RETURN_VOID(cp != NULL);
    talloc_set_name_const(cp, "CompPopup");
    cp->server     = s;
    cp->popup      = popup;
    cp->scene_tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
    popup->base->data = cp;
    talloc_set_destructor(cp, popup_destructor);

    struct wlr_surface *surf = popup->base->surface;
    if (!cp->scene_tree ||
        !comp_listen(cp, &surf->events.commit, handle_popup_commit) ||
        !comp_listen(cp, &popup->events.reposition, handle_popup_reposition) ||
        !comp_listen(cp, &popup->events.destroy, handle_popup_destroy)) {
        talloc_free(cp);
    }
}

static void
handle_decoration_request_mode(void *owner, void *data)
{
    CompDecoration *cd = owner;
    (void)data;
    if (cd->dec->toplevel && cd->dec->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(
            cd->dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
}

static void
handle_decoration_surface_commit(void *owner, void *data)
{
    CompDecoration *cd = owner;
    (void)data;
    if (cd->dec && cd->dec->toplevel && cd->dec->toplevel->base->initial_commit) {
        wlr_xdg_toplevel_decoration_v1_set_mode(
            cd->dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
}

static void
handle_decoration_destroy(void *owner, void *data)
{
    CompDecoration *cd = owner;
    (void)data;
    talloc_free(cd);
}

static void
handle_new_toplevel_decoration(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_xdg_toplevel_decoration_v1 *dec = data;

    CompDecoration *cd = talloc_zero(s, CompDecoration);
    COMP_WARN_RETURN_VOID(cd != NULL);
    talloc_set_name_const(cd, "CompDecoration");
    cd->server = s;
    cd->dec    = dec;

    if (dec->toplevel && dec->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(
            dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else if (dec->toplevel && dec->toplevel->base && dec->toplevel->base->surface) {
        comp_listen(cd, &dec->toplevel->base->surface->events.commit,
                    handle_decoration_surface_commit);
    }

    if (!comp_listen(cd, &dec->events.request_mode, handle_decoration_request_mode) ||
        !comp_listen(cd, &dec->events.destroy, handle_decoration_destroy)) {
        talloc_free(cd);
    }
}

typedef struct {
    struct wlr_xdg_dialog_v1 *dlg;
} CompXdgDialog;

static void
handle_xdg_dialog_set_modal(void *owner, void *data)
{
    CompXdgDialog *cd = owner;
    (void)data;
    if (cd && cd->dlg && cd->dlg->xdg_toplevel)
        comp_window_on_dialog_state_changed(cd->dlg->xdg_toplevel);
}

static void
handle_xdg_dialog_destroy(void *owner, void *data)
{
    CompXdgDialog *cd = owner;
    (void)data;
    talloc_free(cd);
}

static void
handle_new_xdg_dialog(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_xdg_dialog_v1 *dlg = data;
    if (!s || !dlg)
        return;
    CompXdgDialog *cd = talloc_zero(s, CompXdgDialog);
    COMP_WARN_RETURN_VOID(cd != NULL);
    cd->dlg = dlg;
    if (!comp_listen(cd, &dlg->events.set_modal, handle_xdg_dialog_set_modal) ||
        !comp_listen(cd, &dlg->events.destroy, handle_xdg_dialog_destroy)) {
        talloc_free(cd);
    }
}

static void
handle_toplevel_capture_request(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *req = data;
    comp_window_accept_capture_request(s, req);
}

static void
handle_cursor_shape_request(void *owner, void *data)
{
    CompServer *s = owner;
    const struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
    if (!s || !s->seat || !s->cursor || !s->cursor_mgr || !ev)
        return;
    if (s->seat->pointer_state.focused_client != ev->seat_client)
        return;
    const char *shape_name = wlr_cursor_shape_v1_name(ev->shape);
    if (shape_name)
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, shape_name);
}

static bool
is_screen_capture_global(const CompServer *s, const struct wl_global *global)
{
    if (!s || !global)
        return false;
    return (s->screencopy_mgr && global == s->screencopy_mgr->global) ||
           (s->export_dmabuf_mgr && global == s->export_dmabuf_mgr->global) ||
           (s->ext_image_copy_capture_mgr && global == s->ext_image_copy_capture_mgr->global) ||
           (s->ext_output_capture_mgr && global == s->ext_output_capture_mgr->global) ||
           (s->ext_toplevel_capture_mgr && global == s->ext_toplevel_capture_mgr->global);
}

static bool
is_privileged_shell_global(const CompServer *s, const struct wl_global *global)
{
    if (!s || !global)
        return false;
    if (is_screen_capture_global(s, global))
        return true;
    return (s->security_context_mgr && global == s->security_context_mgr->global) ||
           (s->session_lock_mgr && global == s->session_lock_mgr->global) ||
           (s->layer_shell && global == s->layer_shell->global) ||
           (s->output_mgr && global == s->output_mgr->global) ||
           (s->output_power_mgr && global == s->output_power_mgr->global) ||
           (s->input_method_mgr && global == s->input_method_mgr->global) ||
           (s->virtual_kbd_mgr && global == s->virtual_kbd_mgr->global) ||
           (s->foreign_toplevel_mgr && global == s->foreign_toplevel_mgr->global) ||
           (s->ext_foreign_toplevel_list && global == s->ext_foreign_toplevel_list->global);
}

static bool
handle_global_filter(const struct wl_client *client, const struct wl_global *global, void *data)
{
    const CompServer *s = data;
    if (!s || !client || !global)
        return true;

    /* Block screen-capture globals while the session is locked. */
    if (s->locked && is_screen_capture_global(s, global))
        return false;

    /* Hide privileged shell/capture/input globals from sandboxed clients (Flatpak, etc.). */
    if (s->security_context_mgr &&
        wlr_security_context_manager_v1_lookup_client(s->security_context_mgr, client) != NULL &&
        is_privileged_shell_global(s, global)) {
        return false;
    }

    return true;
}

void
comp_protocols_init(CompServer *s)
{
    s->security_context_mgr  = wlr_security_context_manager_v1_create(s->display);
    wl_display_set_global_filter(s->display, handle_global_filter, s);

    s->xdg_decoration_mgr    = wlr_xdg_decoration_manager_v1_create(s->display);
    s->input_method_mgr      = wlr_input_method_manager_v2_create(s->display);
    s->text_input_mgr        = wlr_text_input_manager_v3_create(s->display);
    s->virtual_kbd_mgr       = wlr_virtual_keyboard_manager_v1_create(s->display);
    s->relative_pointer_mgr  = wlr_relative_pointer_manager_v1_create(s->display);
    s->pointer_constraints   = wlr_pointer_constraints_v1_create(s->display);
    s->pointer_gestures      = wlr_pointer_gestures_v1_create(s->display);
    s->cursor_shape_mgr      = wlr_cursor_shape_manager_v1_create(s->display, 1);
    s->tablet_mgr            = wlr_tablet_v2_create(s->display);
    s->shortcuts_inhibit_mgr = wlr_keyboard_shortcuts_inhibit_v1_create(s->display);
    s->foreign_registry      = wlr_xdg_foreign_registry_create(s->display);
    if (s->foreign_registry)
        s->foreign_v2 = wlr_xdg_foreign_v2_create(s->display, s->foreign_registry);
    s->xdg_dialog_mgr        = wlr_xdg_wm_dialog_v1_create(s->display, 1);
    s->xdg_activation        = wlr_xdg_activation_v1_create(s->display);

    struct wlr_server_decoration_manager *sdec = wlr_server_decoration_manager_create(s->display);
    if (sdec)
        wlr_server_decoration_manager_set_default_mode(sdec, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

    wlr_alpha_modifier_v1_create(s->display);
    wlr_xdg_output_manager_v1_create(s->display, s->output_layout);
    wlr_tearing_control_manager_v1_create(s->display, 1);

    /* Spec §6.5.3: screen-capture and foreign-toplevel protocols are disabled in --greeter-mode */
    if (!s->opts.greeter_mode) {
        s->screencopy_mgr             = wlr_screencopy_manager_v1_create(s->display);
        s->export_dmabuf_mgr          = wlr_export_dmabuf_manager_v1_create(s->display);
        s->ext_image_copy_capture_mgr = wlr_ext_image_copy_capture_manager_v1_create(s->display, 1);
        s->ext_output_capture_mgr     = wlr_ext_output_image_capture_source_manager_v1_create(s->display, 1);
        s->foreign_toplevel_mgr       = wlr_foreign_toplevel_manager_v1_create(s->display);
        s->ext_foreign_toplevel_list  = wlr_ext_foreign_toplevel_list_v1_create(s->display, 1);
        s->ext_toplevel_capture_mgr   = wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(s->display, 1);
        if (s->ext_toplevel_capture_mgr) {
            comp_listen_ctx(s->listeners, s,
                            &s->ext_toplevel_capture_mgr->events.new_request,
                            handle_toplevel_capture_request);
        }
    }

    init_color_management(s);

    if (s->xdg_shell)
        comp_listen_ctx(s->listeners, s, &s->xdg_shell->events.new_popup, handle_new_popup);
    if (s->xdg_decoration_mgr)
        comp_listen_ctx(s->listeners, s, &s->xdg_decoration_mgr->events.new_toplevel_decoration,
                        handle_new_toplevel_decoration);
    if (s->xdg_dialog_mgr)
        comp_listen_ctx(s->listeners, s, &s->xdg_dialog_mgr->events.new_dialog,
                        handle_new_xdg_dialog);
    if (s->text_input_mgr)
        comp_listen_ctx(s->listeners, s, &s->text_input_mgr->events.new_text_input, handle_new_text_input);
    if (s->input_method_mgr)
        comp_listen_ctx(s->listeners, s, &s->input_method_mgr->events.new_input_method, handle_new_input_method);
    if (s->virtual_kbd_mgr)
        comp_listen_ctx(s->listeners, s, &s->virtual_kbd_mgr->events.new_virtual_keyboard, handle_new_virtual_keyboard);
    if (s->pointer_constraints)
        comp_listen_ctx(s->listeners, s, &s->pointer_constraints->events.new_constraint, handle_new_pointer_constraint);
    if (s->shortcuts_inhibit_mgr)
        comp_listen_ctx(s->listeners, s, &s->shortcuts_inhibit_mgr->events.new_inhibitor, handle_new_shortcuts_inhibitor);
    if (s->xdg_activation)
        comp_listen_ctx(s->listeners, s, &s->xdg_activation->events.request_activate, handle_xdg_activation_request);
    if (s->cursor_shape_mgr)
        comp_listen_ctx(s->listeners, s, &s->cursor_shape_mgr->events.request_set_shape, handle_cursor_shape_request);
}
