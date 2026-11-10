/* comp_input.c — seat, keyboards, pointers and cursor (Phase 2a: forward to clients).
 *
 * Built-in keys that are always handled, whatever the binds say:
 *   Ctrl+Alt+F1..F12        switch VT (bare metal only; spec §6.5.3, §7.5)
 * Never log key codes, keysyms or text here (spec §6.5.3).
 */
#include "comp_server.h"

#include "comp_log.h"
#include "core/comp_binds.h"

#include <talloc.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <xkbcommon/xkbcommon.h>

typedef struct {
    struct wl_list       link;     /* CompServer.keyboards */
    CompServer          *server;
    struct wlr_keyboard *wlr_keyboard;
} CompKeyboard;

typedef struct {
    CompServer *server;
} CompPointer;

static uint32_t
comp_mods_from_wlr(uint32_t m)
{
    uint32_t out = 0;
    if (m & WLR_MODIFIER_SHIFT) out |= COMP_MOD_SHIFT;
    if (m & WLR_MODIFIER_CTRL)  out |= COMP_MOD_CTRL;
    if (m & WLR_MODIFIER_ALT)   out |= COMP_MOD_ALT;
    if (m & WLR_MODIFIER_LOGO)  out |= COMP_MOD_SUPER;
    return out;
}

static int
keyboard_destructor(CompKeyboard *k)
{
    wl_list_remove(&k->link);
    return 0;
}

static void
keyboard_handle_modifiers(void *owner, void *data)
{
    CompKeyboard *k = owner;
    (void)data;
    wlr_seat_set_keyboard(k->server->seat, k->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(k->server->seat, &k->wlr_keyboard->modifiers);
}

static bool
handle_builtin_key(CompServer *s, xkb_keysym_t sym)
{
    if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
        if (s->session) {
            unsigned vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
            wlr_session_change_vt(s->session, vt);
        }
        return true;   /* consume even when nested: never forward VT keysyms */
    }
    return false;
}

static void
keyboard_handle_key(void *owner, void *data)
{
    CompKeyboard *k = owner;
    CompServer *s = k->server;
    struct wlr_keyboard_key_event *ev = data;
    bool pressed = ev->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    uint32_t keycode = ev->keycode + 8;
    bool handled = false;

    if (pressed) {
        /* Translated keysyms catch the XF86Switch_VT_* produced by Ctrl+Alt+Fn */
        const xkb_keysym_t *syms;
        int n = xkb_state_key_get_syms(k->wlr_keyboard->xkb_state, keycode, &syms);
        for (int i = 0; i < n && !handled; i++)
            handled = handle_builtin_key(s, syms[i]);
        if (!handled && s->key_hook) {
            /* Bind matching uses the level-0 keysym (without Shift), see comp_binds.h */
            xkb_layout_index_t layout = xkb_state_key_get_layout(k->wlr_keyboard->xkb_state, keycode);
            const xkb_keysym_t *base;
            int nb = xkb_keymap_key_get_syms_by_level(k->wlr_keyboard->keymap, keycode,
                                                      layout == XKB_LAYOUT_INVALID ? 0 : layout,
                                                      0, &base);
            uint32_t mods = comp_mods_from_wlr(wlr_keyboard_get_modifiers(k->wlr_keyboard));
            for (int i = 0; i < nb && !handled; i++)
                handled = s->key_hook(s, mods, base[i], true);
        }
    }
    if (!handled) {
        wlr_seat_set_keyboard(s->seat, k->wlr_keyboard);
        wlr_seat_keyboard_notify_key(s->seat, ev->time_msec, ev->keycode, ev->state);
    }
}

static void
keyboard_handle_destroy(void *owner, void *data)
{
    (void)data;
    CompKeyboard *k = owner;
    CompServer *s = k->server;
    talloc_free(k);
    comp_input_update_capabilities(s);
}

static void
new_keyboard(CompServer *s, struct wlr_input_device *dev)
{
    struct wlr_keyboard *wk = wlr_keyboard_from_input_device(dev);
    CompKeyboard *k = talloc_zero(s, CompKeyboard);
    COMP_WARN_RETURN_VOID(k != NULL);
    talloc_set_name_const(k, "CompKeyboard");
    k->server = s;
    k->wlr_keyboard = wk;
    wl_list_insert(&s->keyboards, &k->link);
    talloc_set_destructor(k, keyboard_destructor);

    /* Keymap from XKB_DEFAULT_* environment for now; comp.toml [input.keyboard] in 2d */
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = ctx ? xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    if (km)
        wlr_keyboard_set_keymap(wk, km);
    else
        wlr_log(WLR_ERROR, "keyboard %s: could not compile a keymap", dev->name);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(wk, 35, 250);

    if (!comp_listen(k, &wk->events.modifiers, keyboard_handle_modifiers) ||
        !comp_listen(k, &wk->events.key, keyboard_handle_key) ||
        !comp_listen(k, &dev->events.destroy, keyboard_handle_destroy)) {
        talloc_free(k);
        return;
    }
    wlr_seat_set_keyboard(s->seat, wk);
}

static void
pointer_handle_destroy(void *owner, void *data)
{
    (void)data;
    CompPointer *p = owner;
    CompServer *s = p->server;
    if (s->n_pointers)
        s->n_pointers--;
    talloc_free(p);
    comp_input_update_capabilities(s);
}

static void
new_pointer(CompServer *s, struct wlr_input_device *dev)
{
    CompPointer *p = talloc_zero(s, CompPointer);
    COMP_WARN_RETURN_VOID(p != NULL);
    talloc_set_name_const(p, "CompPointer");
    p->server = s;
    if (!comp_listen(p, &dev->events.destroy, pointer_handle_destroy)) {
        talloc_free(p);
        return;
    }
    s->n_pointers++;
    wlr_cursor_attach_input_device(s->cursor, dev);
}

void
comp_input_update_capabilities(CompServer *s)
{
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;   /* the cursor always exists */
    if (!wl_list_empty(&s->keyboards))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(s->seat, caps);
}

static void
handle_new_input(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_input_device *dev = data;
    switch (dev->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        new_keyboard(s, dev);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        new_pointer(s, dev);
        break;
    case WLR_INPUT_DEVICE_TOUCH:
    case WLR_INPUT_DEVICE_TABLET:
    case WLR_INPUT_DEVICE_TABLET_PAD:
    case WLR_INPUT_DEVICE_SWITCH:
    default:
        break;   /* Phase 2c / 4 */
    }
    comp_input_update_capabilities(s);
}

/* ── cursor ── */

static void
pointer_focus_at_cursor(CompServer *s, uint32_t time_msec)
{
    double sx = 0.0, sy = 0.0;
    struct wlr_scene_node *node = wlr_scene_node_at(&s->scene->tree.node, s->cursor->x, s->cursor->y, &sx, &sy);
    struct wlr_surface *surface = NULL;
    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
        if (ss)
            surface = ss->surface;
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(s->seat, time_msec, sx, sy);
    } else {
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(s->seat);
    }
}

static void
cursor_motion(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_motion_event *ev = data;
    wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    pointer_focus_at_cursor(s, ev->time_msec);
}

static void
cursor_motion_absolute(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_motion_absolute_event *ev = data;
    wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
    pointer_focus_at_cursor(s, ev->time_msec);
}

static void
cursor_button(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_button_event *ev = data;
    wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
}

static void
cursor_axis(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_axis_event *ev = data;
    wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation, ev->delta,
                                 ev->delta_discrete, ev->source, ev->relative_direction);
}

static void
cursor_frame(void *owner, void *data)
{
    CompServer *s = owner;
    (void)data;
    wlr_seat_pointer_notify_frame(s->seat);
}

static void
seat_request_cursor(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_seat_pointer_request_set_cursor_event *ev = data;
    /* Only the client with pointer focus may set the cursor image */
    if (ev->seat_client == s->seat->pointer_state.focused_client)
        wlr_cursor_set_surface(s->cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void
seat_pointer_focus_change(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_seat_pointer_focus_change_event *ev = data;
    if (!ev->new_surface)
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
}

static void
seat_request_set_selection(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_seat_request_set_selection_event *ev = data;
    wlr_seat_set_selection(s->seat, ev->source, ev->serial);
}

static void
seat_request_set_primary_selection(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_seat_request_set_primary_selection_event *ev = data;
    wlr_seat_set_primary_selection(s->seat, ev->source, ev->serial);
}

void
comp_input_init(CompServer *s)
{
    bool ok = comp_listen_ctx(s->listeners, s, &s->backend->events.new_input, handle_new_input) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.motion, cursor_motion) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.motion_absolute, cursor_motion_absolute) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.button, cursor_button) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.axis, cursor_axis) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.frame, cursor_frame) &&
              comp_listen_ctx(s->listeners, s, &s->seat->events.request_set_cursor, seat_request_cursor) &&
              comp_listen_ctx(s->listeners, s, &s->seat->pointer_state.events.focus_change, seat_pointer_focus_change) &&
              comp_listen_ctx(s->listeners, s, &s->seat->events.request_set_selection, seat_request_set_selection) &&
              comp_listen_ctx(s->listeners, s, &s->seat->events.request_set_primary_selection, seat_request_set_primary_selection);
    if (!ok)
        wlr_log(WLR_ERROR, "could not set up input listeners");
    comp_input_update_capabilities(s);
}
