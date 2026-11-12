/* comp_input.c — seat, keyboards, pointers, touch, touchpad gestures & libinput (Phase 4).
 *
 * Built-in keys that are always handled, whatever the binds say:
 *   Ctrl+Alt+F1..F12        switch VT (bare metal only; spec §6.5.3, §7.5)
 * Never log key codes, keysyms or text here (spec §6.5.3).
 */
#include "comp_server.h"

#include "comp_log.h"
#include "core/comp_binds.h"
#include "core/comp_schema_keys.h"

#include <libinput.h>
#include <math.h>
#include <talloc.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/region.h>
#include <xkbcommon/xkbcommon.h>

typedef struct {
    struct wl_list          link;           /* CompServer.keyboards */
    CompServer             *server;
    struct wlr_keyboard    *wlr_keyboard;
    struct wl_event_source *repeat_timer;
    const CompBind         *repeat_bind;
    uint32_t                repeat_keycode;
    uint64_t                consumed_keys[8]; /* 512-bit bitmap of compositor-consumed key presses */
} CompKeyboard;

typedef struct {
    struct wl_list           link;          /* CompServer.input_devs */
    CompServer              *server;
    struct wlr_input_device *dev;
} CompInputDev;

static double
now_monotonic_sec(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

uint32_t
comp_input_mods_from_wlr(uint32_t m)
{
    uint32_t out = 0;
    if (m & WLR_MODIFIER_SHIFT) out |= COMP_MOD_SHIFT;
    if (m & WLR_MODIFIER_CTRL)  out |= COMP_MOD_CTRL;
    if (m & WLR_MODIFIER_ALT)   out |= COMP_MOD_ALT;
    if (m & WLR_MODIFIER_LOGO)  out |= COMP_MOD_SUPER;
    return out;
}

static void
keyboard_disarm_repeat(CompKeyboard *k)
{
    if (!k)
        return;
    k->repeat_bind = NULL;
    if (k->repeat_timer)
        wl_event_source_timer_update(k->repeat_timer, 0);
}

static int
handle_keyboard_repeat_timer(void *data)
{
    CompKeyboard *k = data;
    CompServer *s = k ? k->server : NULL;
    if (!k || !s || !k->repeat_bind)
        return 0;
    if (s->locked && !k->repeat_bind->allow_locked) {
        keyboard_disarm_repeat(k);
        return 0;
    }
    comp_window_handle_bind(s, k->repeat_bind);
    int32_t rate = k->wlr_keyboard ? k->wlr_keyboard->repeat_info.rate : 0;
    if (rate > 0 && k->repeat_timer) {
        int step_ms = 1000 / rate;
        if (step_ms < 5)
            step_ms = 5;
        wl_event_source_timer_update(k->repeat_timer, step_ms);
    }
    return 0;
}

static int
keyboard_destructor(CompKeyboard *k)
{
    if (k->repeat_timer) {
        wl_event_source_remove(k->repeat_timer);
        k->repeat_timer = NULL;
    }
    wl_list_remove(&k->link);
    return 0;
}

static void
keyboard_set_consumed(CompKeyboard *k, uint32_t ev_keycode, bool consumed)
{
    if (!k || ev_keycode >= 512)
        return;
    uint64_t bit = 1ULL << (ev_keycode & 63u);
    if (consumed)
        k->consumed_keys[ev_keycode >> 6] |= bit;
    else
        k->consumed_keys[ev_keycode >> 6] &= ~bit;
}

static bool
keyboard_take_consumed(CompKeyboard *k, uint32_t ev_keycode)
{
    if (!k || ev_keycode >= 512)
        return false;
    uint64_t bit = 1ULL << (ev_keycode & 63u);
    bool was = (k->consumed_keys[ev_keycode >> 6] & bit) != 0;
    k->consumed_keys[ev_keycode >> 6] &= ~bit;
    return was;
}

static bool
keyboard_allowed_while_locked(const CompServer *s, struct wlr_keyboard *wk)
{
    if (!s || !wk || !s->locked)
        return true;
    struct wlr_virtual_keyboard_v1 *vkbd =
        wlr_input_device_get_virtual_keyboard(&wk->base);
    if (!vkbd)
        return true;
    /* Only the session-lock owner (e.g. ShellOSK inside kajo) may inject
     * virtual-keyboard events while the session is locked. */
    if (!s->active_lock || !s->active_lock->resource || !vkbd->resource)
        return false;
    return wl_resource_get_client(vkbd->resource) ==
           wl_resource_get_client(s->active_lock->resource);
}

static void
keyboard_handle_modifiers(void *owner, void *data)
{
    CompKeyboard *k = owner;
    CompServer *s = k->server;
    (void)data;
    if (!keyboard_allowed_while_locked(s, k->wlr_keyboard))
        return;
    keyboard_disarm_repeat(k);
    wlr_seat_set_keyboard(s->seat, k->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(s->seat, &k->wlr_keyboard->modifiers);
    if (comp_switcher_is_active(s)) {
        uint32_t mods = comp_input_mods_from_wlr(wlr_keyboard_get_modifiers(k->wlr_keyboard));
        comp_switcher_handle_modifiers(s, mods);
    }
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
    if (!keyboard_allowed_while_locked(s, k->wlr_keyboard))
        return;

    bool pressed = ev->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    uint32_t keycode = ev->keycode + 8;
    bool handled = false;

    comp_lock_idle_notify_activity(s);

    if (!pressed) {
        if (k->repeat_keycode == ev->keycode)
            keyboard_disarm_repeat(k);
        handled = keyboard_take_consumed(k, ev->keycode);
    } else {
        keyboard_disarm_repeat(k);
        /* Translated keysyms catch the XF86Switch_VT_* produced by Ctrl+Alt+Fn */
        const xkb_keysym_t *syms;
        int n = xkb_state_key_get_syms(k->wlr_keyboard->xkb_state, keycode, &syms);
        for (int i = 0; i < n && !handled; i++)
            handled = handle_builtin_key(s, syms[i]);

        uint32_t mods = comp_input_mods_from_wlr(wlr_keyboard_get_modifiers(k->wlr_keyboard));
        if (!handled && comp_screenshot_is_active(s)) {
            for (int i = 0; i < n && !handled; i++)
                handled = comp_screenshot_handle_key(s, mods, syms[i]);
            handled = true;
        }
        if (!handled && comp_switcher_is_active(s)) {
            xkb_layout_index_t layout = xkb_state_key_get_layout(k->wlr_keyboard->xkb_state, keycode);
            const xkb_keysym_t *base;
            int nb = xkb_keymap_key_get_syms_by_level(k->wlr_keyboard->keymap, keycode,
                                                      layout == XKB_LAYOUT_INVALID ? 0 : layout,
                                                      0, &base);
            for (int i = 0; i < nb && !handled; i++)
                handled = comp_switcher_handle_key(s, mods, base[i]);
            handled = true;
        }
        if (!handled && (s->key_hook || s->binds)) {
            /* Bind matching uses the level-0 keysym (without Shift), see comp_binds.h */
            xkb_layout_index_t layout = xkb_state_key_get_layout(k->wlr_keyboard->xkb_state, keycode);
            const xkb_keysym_t *base;
            int nb = xkb_keymap_key_get_syms_by_level(k->wlr_keyboard->keymap, keycode,
                                                      layout == XKB_LAYOUT_INVALID ? 0 : layout,
                                                      0, &base);
            bool inhibited = comp_protocols_shortcuts_inhibited(s);
            for (int i = 0; i < nb && !handled; i++) {
                if (s->key_hook)
                    handled = s->key_hook(s, mods, base[i], true);
                if (!handled && s->binds) {
                    const CompBind *b = comp_binds_lookup_key(s->binds, mods, base[i]);
                    if (b) {
                        if (s->locked && !b->allow_locked)
                            continue;
                        if (inhibited && b->allow_inhibiting &&
                            b->kind != COMP_BIND_BREAK_SHORTCUTS_INHIBIT)
                            continue;
                        handled = comp_window_handle_bind(s, b);
                        if (handled && b->repeat && k->repeat_timer &&
                            k->wlr_keyboard->repeat_info.rate > 0 &&
                            k->wlr_keyboard->repeat_info.delay > 0) {
                            k->repeat_bind = b;
                            k->repeat_keycode = ev->keycode;
                            wl_event_source_timer_update(k->repeat_timer,
                                                         k->wlr_keyboard->repeat_info.delay);
                        }
                    }
                }
            }
        }
        keyboard_set_consumed(k, ev->keycode, handled);
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
configure_keyboard(CompServer *s, struct wlr_keyboard *wk, bool is_new)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx)
        return;

    struct xkb_rule_names names = {0};
    const struct xkb_rule_names *names_ptr = NULL;
    int32_t rate = 35, delay = 250;
    if (s && s->config) {
        if (s->config->xkb_layout && *s->config->xkb_layout) {
            names.layout  = s->config->xkb_layout;
            names.options = s->config->xkb_options ? s->config->xkb_options : "";
            names_ptr     = &names;
        }
        rate  = s->config->repeat_rate;
        delay = s->config->repeat_delay;
    }

    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, names_ptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km && names_ptr)
        km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (km) {
        wlr_keyboard_set_keymap(wk, km);
        if (is_new && s && s->config && s->config->numlock_on_startup) {
            xkb_mod_index_t numlock_idx = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_NUM);
            if (numlock_idx != XKB_MOD_INVALID) {
                uint32_t locked = wk->modifiers.locked | (1u << numlock_idx);
                wlr_keyboard_notify_modifiers(wk, wk->modifiers.depressed,
                                              wk->modifiers.latched, locked,
                                              wk->modifiers.group);
            }
        }
        xkb_keymap_unref(km);
    } else {
        wlr_log(WLR_ERROR, "keyboard: could not compile a keymap");
    }
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(wk, rate, delay);
}

static void
configure_libinput_device(CompServer *s, struct wlr_input_device *dev)
{
    if (!s || !s->config || !dev || !wlr_input_device_is_libinput(dev))
        return;
    struct libinput_device *ld = wlr_libinput_get_device_handle(dev);
    if (!ld)
        return;

    bool is_touchpad = libinput_device_config_tap_get_finger_count(ld) > 0;
    if (is_touchpad) {
        libinput_device_config_tap_set_enabled(
            ld, s->config->touchpad_tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
        if (libinput_device_config_scroll_has_natural_scroll(ld))
            libinput_device_config_scroll_set_natural_scroll_enabled(ld, s->config->touchpad_natural_scroll);
        if (libinput_device_config_dwt_is_available(ld)) {
            libinput_device_config_dwt_set_enabled(
                ld, s->config->touchpad_dwt ? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
        }
    } else if (dev->type == WLR_INPUT_DEVICE_POINTER) {
        if (libinput_device_config_scroll_has_natural_scroll(ld))
            libinput_device_config_scroll_set_natural_scroll_enabled(ld, s->config->mouse_natural_scroll);
        if (libinput_device_config_accel_is_available(ld)) {
            enum libinput_config_accel_profile prof =
                (s->config->mouse_accel_profile == COMP_ACCEL_FLAT)
                    ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
                    : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
            libinput_device_config_accel_set_profile(ld, prof);
            libinput_device_config_accel_set_speed(ld, s->config->mouse_accel_speed);
        }
    }
}

void
comp_input_apply_config(CompServer *s)
{
    if (!s)
        return;
    CompKeyboard *k;
    wl_list_for_each(k, &s->keyboards, link) {
        keyboard_disarm_repeat(k);
        configure_keyboard(s, k->wlr_keyboard, false);
    }
    CompInputDev *id;
    wl_list_for_each(id, &s->input_devs, link) {
        configure_libinput_device(s, id->dev);
    }
    comp_ipc_emit_keyboard_layouts(s);
}

void
comp_input_map_devices_to_output(CompServer *s, struct wlr_output *output)
{
    if (!s || !s->cursor)
        return;
    CompInputDev *id;
    wl_list_for_each(id, &s->input_devs, link) {
        if (!id->dev)
            continue;
        if (id->dev->type == WLR_INPUT_DEVICE_TOUCH ||
            id->dev->type == WLR_INPUT_DEVICE_TABLET) {
            wlr_cursor_map_input_to_output(s->cursor, id->dev, output);
        }
    }
}

bool
comp_input_switch_keyboard_layout(CompServer *s, int idx)
{
    if (!s || wl_list_empty(&s->keyboards))
        return false;
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
    if (!kb) {
        CompKeyboard *first = wl_container_of(s->keyboards.next, first, link);
        kb = first->wlr_keyboard;
    }
    if (!kb || !kb->keymap || !kb->xkb_state)
        return false;

    xkb_layout_index_t n_layouts = xkb_keymap_num_layouts(kb->keymap);
    if (n_layouts == 0)
        return false;

    xkb_layout_index_t cur = xkb_state_serialize_layout(kb->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    xkb_layout_index_t target = (idx < 0) ? ((cur + 1) % n_layouts) : (xkb_layout_index_t)idx;
    if (target >= n_layouts)
        return false;

    CompKeyboard *k;
    wl_list_for_each(k, &s->keyboards, link) {
        struct wlr_keyboard *wk = k->wlr_keyboard;
        if (!wk || !wk->xkb_state)
            continue;
        wlr_keyboard_notify_modifiers(wk,
                                      wk->modifiers.depressed,
                                      wk->modifiers.latched,
                                      wk->modifiers.locked,
                                      target);
    }
    comp_ipc_emit_keyboard_layouts(s);
    return true;
}

size_t
comp_input_get_keyboard_layouts(const CompServer *s, const char **names_out,
                                size_t max_names, uint32_t *current_idx_out)
{
    if (current_idx_out)
        *current_idx_out = 0;
    if (!s)
        return 0;

    struct wlr_keyboard *kb = s->seat ? wlr_seat_get_keyboard(s->seat) : NULL;
    if (!kb && !wl_list_empty(&s->keyboards)) {
        const CompKeyboard *first = wl_container_of(s->keyboards.next, first, link);
        kb = first->wlr_keyboard;
    }
    if (kb && kb->keymap) {
        xkb_layout_index_t n = xkb_keymap_num_layouts(kb->keymap);
        if (current_idx_out && kb->xkb_state && n > 0) {
            xkb_layout_index_t cur = xkb_state_serialize_layout(kb->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
            *current_idx_out = cur < n ? (uint32_t)cur : 0;
        }
        for (xkb_layout_index_t i = 0; i < n; i++) {
            if (names_out && (size_t)i < max_names) {
                const char *nm = xkb_keymap_layout_get_name(kb->keymap, i);
                names_out[i] = nm ? nm : "English (US)";
            }
        }
        if (n > 0)
            return (size_t)n;
    }
    if (names_out && max_names > 0)
        names_out[0] = "English (US)";
    return 1;
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
    if (s->loop)
        k->repeat_timer = wl_event_loop_add_timer(s->loop, handle_keyboard_repeat_timer, k);

    configure_keyboard(s, wk, true);

    if (!comp_listen(k, &wk->events.modifiers, keyboard_handle_modifiers) ||
        !comp_listen(k, &wk->events.key, keyboard_handle_key) ||
        !comp_listen(k, &dev->events.destroy, keyboard_handle_destroy)) {
        talloc_free(k);
        return;
    }
    wlr_seat_set_keyboard(s->seat, wk);
    comp_ipc_emit_keyboard_layouts(s);
}

void
comp_input_add_keyboard_device(CompServer *s, struct wlr_input_device *dev)
{
    if (!s || !dev)
        return;
    new_keyboard(s, dev);
    comp_input_update_capabilities(s);
}

static int
input_dev_destructor(CompInputDev *id)
{
    wl_list_remove(&id->link);
    return 0;
}

static void
input_dev_handle_destroy(void *owner, void *data)
{
    (void)data;
    CompInputDev *id = owner;
    CompServer *s = id->server;
    if (id->dev) {
        if (id->dev->type == WLR_INPUT_DEVICE_POINTER && s->n_pointers)
            s->n_pointers--;
        else if (id->dev->type == WLR_INPUT_DEVICE_TOUCH && s->n_touch)
            s->n_touch--;
    }
    talloc_free(id);
    comp_input_update_capabilities(s);
}

static void
register_input_dev(CompServer *s, struct wlr_input_device *dev)
{
    CompInputDev *id = talloc_zero(s, CompInputDev);
    COMP_WARN_RETURN_VOID(id != NULL);
    talloc_set_name_const(id, "CompInputDev");
    id->server = s;
    id->dev    = dev;
    wl_list_insert(&s->input_devs, &id->link);
    talloc_set_destructor(id, input_dev_destructor);
    if (!comp_listen(id, &dev->events.destroy, input_dev_handle_destroy)) {
        talloc_free(id);
        return;
    }
    configure_libinput_device(s, dev);

    if (dev->type == WLR_INPUT_DEVICE_POINTER) {
        s->n_pointers++;
        wlr_cursor_attach_input_device(s->cursor, dev);
    } else if (dev->type == WLR_INPUT_DEVICE_TOUCH) {
        s->n_touch++;
        struct wlr_touch *wt = wlr_touch_from_input_device(dev);
        if (wt && wt->width_mm > 0.0 && wt->height_mm > 0.0) {
            s->touch_tracker.width_mm  = wt->width_mm;
            s->touch_tracker.height_mm = wt->height_mm;
        }
        wlr_cursor_attach_input_device(s->cursor, dev);
        struct wlr_output *primary = comp_server_primary_output(s);
        if (primary)
            wlr_cursor_map_input_to_output(s->cursor, dev, primary);
    } else if (dev->type == WLR_INPUT_DEVICE_TABLET) {
        if (s->tablet_mgr)
            wlr_tablet_create(s->tablet_mgr, s->seat, dev);
        wlr_cursor_attach_input_device(s->cursor, dev);
        struct wlr_output *primary = comp_server_primary_output(s);
        if (primary)
            wlr_cursor_map_input_to_output(s->cursor, dev, primary);
    }
}

void
comp_input_update_capabilities(CompServer *s)
{
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;   /* the cursor always exists */
    if (!wl_list_empty(&s->keyboards))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    if (s->n_touch > 0)
        caps |= WL_SEAT_CAPABILITY_TOUCH;
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
    case WLR_INPUT_DEVICE_TOUCH:
    case WLR_INPUT_DEVICE_TABLET:
        register_input_dev(s, dev);
        break;
    case WLR_INPUT_DEVICE_TABLET_PAD:
        if (s->tablet_mgr)
            wlr_tablet_pad_create(s->tablet_mgr, s->seat, dev);
        break;
    case WLR_INPUT_DEVICE_SWITCH:
    default:
        break;
    }
    comp_input_update_capabilities(s);
}

/* ── cursor ── */

static struct wlr_surface *
surface_at_coords(CompServer *s, double lx, double ly, double *out_sx, double *out_sy)
{
    double sx = 0.0, sy = 0.0;
    struct wlr_scene_node *root = (s->locked && s->tree_lock)
                                      ? &s->tree_lock->node
                                      : &s->scene->tree.node;
    struct wlr_scene_node *node = wlr_scene_node_at(root, lx, ly, &sx, &sy);
    struct wlr_surface *surface = NULL;
    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
        if (ss)
            surface = ss->surface;
    }
    if (out_sx) *out_sx = sx;
    if (out_sy) *out_sy = sy;
    return surface;
}

static void
pointer_focus_at_cursor(CompServer *s, uint32_t time_msec)
{
    if (!s || !s->cursor || !s->seat)
        return;
    double sx = 0.0, sy = 0.0;
    struct wlr_surface *surface = surface_at_coords(s, s->cursor->x, s->cursor->y, &sx, &sy);
    if (surface) {
        wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(s->seat, time_msec, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(s->seat);
    }
}

void
comp_input_refresh_pointer_focus(CompServer *s, uint32_t time_msec)
{
    pointer_focus_at_cursor(s, time_msec);
}

static void
cursor_motion(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_motion_event *ev = data;
    comp_lock_idle_notify_activity(s);

    if (s->relative_pointer_mgr) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            s->relative_pointer_mgr, s->seat, (uint64_t)ev->time_msec * 1000,
            ev->delta_x, ev->delta_y, ev->unaccel_dx, ev->unaccel_dy);
    }
    if (s->active_constraint &&
        s->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        return;
    }

    double dx = ev->delta_x;
    double dy = ev->delta_y;
    if (s->active_constraint &&
        s->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED &&
        s->active_constraint->surface) {
        int slx = 0, sly = 0;
        if (comp_server_surface_scene_coords(s, s->active_constraint->surface, &slx, &sly)) {
            double sx1 = s->cursor->x - (double)slx;
            double sy1 = s->cursor->y - (double)sly;
            double sx2 = sx1 + dx;
            double sy2 = sy1 + dy;
            double cx = sx2, cy = sy2;
            if (wlr_region_confine(&s->active_constraint->region, sx1, sy1, sx2, sy2, &cx, &cy)) {
                dx = cx - sx1;
                dy = cy - sy1;
            } else {
                dx = 0.0;
                dy = 0.0;
            }
        }
    }

    wlr_cursor_move(s->cursor, &ev->pointer->base, dx, dy);
    if (comp_screenshot_is_active(s)) {
        comp_screenshot_handle_motion(s, s->cursor->x, s->cursor->y);
        return;
    }
    if (s->grab_win != 0 && !s->locked)
        comp_window_update_grab(s);
    pointer_focus_at_cursor(s, ev->time_msec);
}

static void
cursor_motion_absolute(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_motion_absolute_event *ev = data;
    comp_lock_idle_notify_activity(s);
    wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
    if (comp_screenshot_is_active(s)) {
        comp_screenshot_handle_motion(s, s->cursor->x, s->cursor->y);
        return;
    }
    if (s->grab_win != 0 && !s->locked)
        comp_window_update_grab(s);
    pointer_focus_at_cursor(s, ev->time_msec);
}

static void
maybe_focus_on_demand_layer(CompServer *s, struct wlr_surface *surf)
{
    if (!s || !surf || s->locked || s->exclusive_layer_surface)
        return;
    struct wlr_layer_surface_v1 *lsurf = wlr_layer_surface_v1_try_from_wlr_surface(surf);
    if (lsurf && lsurf->surface && lsurf->surface->mapped &&
        lsurf->current.keyboard_interactive ==
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) {
        comp_protocols_notify_focus_surface(s, lsurf->surface);
        comp_server_keyboard_enter(s, lsurf->surface);
    }
}

static void
cursor_button(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_button_event *ev = data;
    comp_lock_idle_notify_activity(s);
    if (comp_screenshot_is_active(s)) {
        comp_screenshot_handle_button(s, ev->button,
                                      ev->state == WL_POINTER_BUTTON_STATE_PRESSED,
                                      s->cursor->x, s->cursor->y);
        return;
    }
    if (comp_switcher_is_active(s)) {
        if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED)
            comp_switcher_handle_click(s, s->cursor->x, s->cursor->y);
        return;
    }
    if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED && s->grab_win != 0) {
        comp_window_end_grab(s);
    } else if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && !s->locked) {
        /* Spec §3.5 F4 & F5: click-to-focus and pass-through */
        struct wlr_surface *surf = NULL;
        double sx = 0.0, sy = 0.0;
        struct CompWindow *w = comp_window_at(s, s->cursor->x, s->cursor->y, &surf, &sx, &sy);
        if (w) {
            comp_window_focus(s, w);
        } else {
            if (surf)
                maybe_focus_on_demand_layer(s, surf);
            pointer_focus_at_cursor(s, ev->time_msec);
        }
    }
    wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
}

static void
cursor_axis(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_axis_event *ev = data;
    comp_lock_idle_notify_activity(s);
    if (s->binds && ev->delta != 0.0 && !s->locked) {
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
        uint32_t mods = kb ? comp_input_mods_from_wlr(wlr_keyboard_get_modifiers(kb)) : 0;
        CompTrigger trig = COMP_TRIGGER_WHEEL_DOWN;
        if (ev->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL)
            trig = ev->delta < 0.0 ? COMP_TRIGGER_WHEEL_UP : COMP_TRIGGER_WHEEL_DOWN;
        else
            trig = ev->delta < 0.0 ? COMP_TRIGGER_WHEEL_LEFT : COMP_TRIGGER_WHEEL_RIGHT;
        const CompBind *b = comp_binds_lookup_wheel(s->binds, mods, trig);
        if (b) {
            double now = now_monotonic_sec();
            double cd_sec = (double)(b->cooldown_ms > 0 ? b->cooldown_ms : 0) / 1000.0;
            if ((unsigned)trig < 8 && now - s->wheel_last_fire_sec[trig] < cd_sec)
                return;   /* consume wheel tick within cooldown window */
            if (comp_window_handle_bind(s, b)) {
                if ((unsigned)trig < 8)
                    s->wheel_last_fire_sec[trig] = now;
                return;
            }
        }
    }
    double factor = comp_window_scroll_factor_for_surface(s, s->seat->pointer_state.focused_surface);
    double delta = ev->delta * factor;
    int32_t discrete = (int32_t)round((double)ev->delta_discrete * factor);
    wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation, delta,
                                 discrete, ev->source, ev->relative_direction);
}

static void
cursor_frame(void *owner, void *data)
{
    CompServer *s = owner;
    (void)data;
    wlr_seat_pointer_notify_frame(s->seat);
}

/* ── touch & touchpad gestures (Spec §3.15) ── */

static CompTransform
primary_comp_transform(const CompServer *s)
{
    struct wlr_output *out = comp_server_primary_output(s);
    if (!out || (int)out->transform < COMP_TRANSFORM_NORMAL ||
        (int)out->transform > COMP_TRANSFORM_FLIPPED_270) {
        return COMP_TRANSFORM_NORMAL;
    }
    return (CompTransform)out->transform;
}

static void
cancel_all_client_touches(CompServer *s)
{
    if (!s || !s->seat)
        return;
    for (int i = 0; i < COMP_TOUCH_MAX_SLOTS; i++) {
        if (!s->touch_tracker.slots[i].active)
            continue;
        struct wlr_touch_point *pt = wlr_seat_touch_get_point(s->seat, s->touch_tracker.slots[i].touch_id);
        if (pt && pt->client)
            wlr_seat_touch_notify_cancel(s->seat, pt->client);
    }
}

static void
dispatch_touch_event(CompServer *s, CompTouchGestureEvent ev)
{
    if (!s || s->locked || ev == COMP_TOUCH_EV_NONE)
        return;

    CompBind b = { .kind = COMP_BIND_LAYOUT };
    CompTouchSwipeAction horiz = s->config ? s->config->touch_swipe_horizontal : COMP_SWIPE_COLUMN;

    switch (ev) {
    case COMP_TOUCH_EV_3F_SWIPE_UP:
        b.layout_action = COMP_ACT_FOCUS_WORKSPACE_DOWN;
        comp_window_handle_bind(s, &b);
        break;
    case COMP_TOUCH_EV_3F_SWIPE_DOWN:
        b.layout_action = COMP_ACT_FOCUS_WORKSPACE_UP;
        comp_window_handle_bind(s, &b);
        break;
    case COMP_TOUCH_EV_3F_SWIPE_LEFT:
        if (horiz == COMP_SWIPE_NONE)
            return;
        b.layout_action = (horiz == COMP_SWIPE_WORKSPACE)
                              ? COMP_ACT_FOCUS_WORKSPACE_DOWN
                              : COMP_ACT_FOCUS_COLUMN_RIGHT;
        comp_window_handle_bind(s, &b);
        break;
    case COMP_TOUCH_EV_3F_SWIPE_RIGHT:
        if (horiz == COMP_SWIPE_NONE)
            return;
        b.layout_action = (horiz == COMP_SWIPE_WORKSPACE)
                              ? COMP_ACT_FOCUS_WORKSPACE_UP
                              : COMP_ACT_FOCUS_COLUMN_LEFT;
        comp_window_handle_bind(s, &b);
        break;
    case COMP_TOUCH_EV_BEZEL_TOP:
        comp_ipc_emit_touch_bezel_swipe(s, COMP_IPC_EDGE_TOP, s->touch_tracker.last_x_ratio);
        break;
    case COMP_TOUCH_EV_BEZEL_BOTTOM:
        comp_ipc_emit_touch_bezel_swipe(s, COMP_IPC_EDGE_BOTTOM, s->touch_tracker.last_x_ratio);
        break;
    case COMP_TOUCH_EV_NONE:
    default:
        break;
    }
}

static void
cursor_touch_down(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_touch_down_event *ev = data;
    comp_lock_idle_notify_activity(s);

    s->touch_tracker.transform = primary_comp_transform(s);
    comp_touch_tracker_down(&s->touch_tracker, ev->touch_id, ev->x, ev->y);
    if (comp_touch_tracker_active_count(&s->touch_tracker) >= 3) {
        cancel_all_client_touches(s);
        return;
    }

    double lx = 0.0, ly = 0.0;
    wlr_cursor_absolute_to_layout_coords(s->cursor, &ev->touch->base, ev->x, ev->y, &lx, &ly);

    if (!s->locked) {
        struct wlr_surface *wsurf = NULL;
        double wsx = 0.0, wsy = 0.0;
        struct CompWindow *w = comp_window_at(s, lx, ly, &wsurf, &wsx, &wsy);
        if (w)
            comp_window_focus(s, w);
        else if (wsurf)
            maybe_focus_on_demand_layer(s, wsurf);
    }

    double sx = 0.0, sy = 0.0;
    struct wlr_surface *surface = surface_at_coords(s, lx, ly, &sx, &sy);
    if (surface && wlr_surface_accepts_touch(surface, s->seat))
        wlr_seat_touch_notify_down(s->seat, surface, ev->time_msec, ev->touch_id, sx, sy);
}

static void
cursor_touch_up(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_touch_up_event *ev = data;
    comp_lock_idle_notify_activity(s);
    comp_touch_tracker_up(&s->touch_tracker, ev->touch_id);
    wlr_seat_touch_notify_up(s->seat, ev->time_msec, ev->touch_id);
}

static void
cursor_touch_motion(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_touch_motion_event *ev = data;
    comp_lock_idle_notify_activity(s);

    bool enabled = !s->config || s->config->touch_enabled;
    s->touch_tracker.transform = primary_comp_transform(s);
    CompTouchGestureEvent gev = comp_touch_tracker_motion(&s->touch_tracker, ev->touch_id, ev->x, ev->y);
    if (enabled && gev != COMP_TOUCH_EV_NONE) {
        cancel_all_client_touches(s);
        dispatch_touch_event(s, gev);
        return;
    }
    if (enabled && (s->touch_tracker.gesture_triggered ||
                    comp_touch_tracker_active_count(&s->touch_tracker) >= 3)) {
        return;
    }

    double lx = 0.0, ly = 0.0;
    wlr_cursor_absolute_to_layout_coords(s->cursor, &ev->touch->base, ev->x, ev->y, &lx, &ly);
    double sx = 0.0, sy = 0.0;
    struct wlr_touch_point *pt = wlr_seat_touch_get_point(s->seat, ev->touch_id);
    int slx = 0, sly = 0;
    if (pt && pt->surface && comp_server_surface_scene_coords(s, pt->surface, &slx, &sly)) {
        sx = lx - (double)slx;
        sy = ly - (double)sly;
    } else {
        surface_at_coords(s, lx, ly, &sx, &sy);
    }
    wlr_seat_touch_notify_motion(s->seat, ev->time_msec, ev->touch_id, sx, sy);
}

static void
cursor_touch_cancel(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_touch_cancel_event *ev = data;
    comp_touch_tracker_cancel(&s->touch_tracker);
    struct wlr_touch_point *pt = wlr_seat_touch_get_point(s->seat, ev->touch_id);
    if (pt && pt->client)
        wlr_seat_touch_notify_cancel(s->seat, pt->client);
}

static void
cursor_touch_frame(void *owner, void *data)
{
    CompServer *s = owner;
    (void)data;
    wlr_seat_touch_notify_frame(s->seat);
}

/* ── touchpad swipe / pinch / hold gestures (Spec §3.15.2) ── */

static void
cursor_swipe_begin(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_swipe_begin_event *ev = data;
    comp_lock_idle_notify_activity(s);
    comp_touchpad_swipe_begin(&s->touchpad_swipe, ev->fingers);
    if (ev->fingers != 3 && s->pointer_gestures)
        wlr_pointer_gestures_v1_send_swipe_begin(s->pointer_gestures, s->seat, ev->time_msec, ev->fingers);
}

static void
cursor_swipe_update(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_swipe_update_event *ev = data;
    comp_lock_idle_notify_activity(s);
    if (s->touchpad_swipe.fingers == 3) {
        bool nat = !s->config || s->config->touchpad_natural_scroll;
        bool en  = !s->config || s->config->touch_enabled;
        CompAction act = COMP_ACT_FOCUS_COLUMN_LEFT;
        if (en && !s->locked &&
            comp_touchpad_swipe_update(&s->touchpad_swipe, ev->fingers, ev->dx, ev->dy, nat, &act)) {
            CompBind b = { .kind = COMP_BIND_LAYOUT, .layout_action = act };
            comp_window_handle_bind(s, &b);
        }
    } else if (s->pointer_gestures) {
        wlr_pointer_gestures_v1_send_swipe_update(s->pointer_gestures, s->seat, ev->time_msec, ev->dx, ev->dy);
    }
}

static void
cursor_swipe_end(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_swipe_end_event *ev = data;
    comp_lock_idle_notify_activity(s);
    uint32_t prev_fingers = s->touchpad_swipe.fingers;
    comp_touchpad_swipe_end(&s->touchpad_swipe);
    if (prev_fingers != 3 && s->pointer_gestures)
        wlr_pointer_gestures_v1_send_swipe_end(s->pointer_gestures, s->seat, ev->time_msec, ev->cancelled);
}

static void
cursor_pinch_begin(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_pinch_begin_event *ev = data;
    comp_lock_idle_notify_activity(s);
    if (s->pointer_gestures)
        wlr_pointer_gestures_v1_send_pinch_begin(s->pointer_gestures, s->seat, ev->time_msec, ev->fingers);
}

static void
cursor_pinch_update(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_pinch_update_event *ev = data;
    comp_lock_idle_notify_activity(s);
    if (s->pointer_gestures) {
        wlr_pointer_gestures_v1_send_pinch_update(
            s->pointer_gestures, s->seat, ev->time_msec, ev->dx, ev->dy, ev->scale, ev->rotation);
    }
}

static void
cursor_pinch_end(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_pinch_end_event *ev = data;
    comp_lock_idle_notify_activity(s);
    if (s->pointer_gestures)
        wlr_pointer_gestures_v1_send_pinch_end(s->pointer_gestures, s->seat, ev->time_msec, ev->cancelled);
}

static void
cursor_hold_begin(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_hold_begin_event *ev = data;
    comp_lock_idle_notify_activity(s);
    if (s->pointer_gestures)
        wlr_pointer_gestures_v1_send_hold_begin(s->pointer_gestures, s->seat, ev->time_msec, ev->fingers);
}

static void
cursor_hold_end(void *owner, void *data)
{
    CompServer *s = owner;
    struct wlr_pointer_hold_end_event *ev = data;
    comp_lock_idle_notify_activity(s);
    if (s->pointer_gestures)
        wlr_pointer_gestures_v1_send_hold_end(s->pointer_gestures, s->seat, ev->time_msec, ev->cancelled);
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
    comp_protocols_check_pointer_constraint(s, ev->new_surface);
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
              comp_listen_ctx(s->listeners, s, &s->cursor->events.swipe_begin, cursor_swipe_begin) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.swipe_update, cursor_swipe_update) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.swipe_end, cursor_swipe_end) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.pinch_begin, cursor_pinch_begin) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.pinch_update, cursor_pinch_update) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.pinch_end, cursor_pinch_end) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.hold_begin, cursor_hold_begin) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.hold_end, cursor_hold_end) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.touch_down, cursor_touch_down) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.touch_up, cursor_touch_up) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.touch_motion, cursor_touch_motion) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.touch_cancel, cursor_touch_cancel) &&
              comp_listen_ctx(s->listeners, s, &s->cursor->events.touch_frame, cursor_touch_frame) &&
              comp_listen_ctx(s->listeners, s, &s->seat->events.request_set_cursor, seat_request_cursor) &&
              comp_listen_ctx(s->listeners, s, &s->seat->pointer_state.events.focus_change, seat_pointer_focus_change) &&
              comp_listen_ctx(s->listeners, s, &s->seat->events.request_set_selection, seat_request_set_selection) &&
              comp_listen_ctx(s->listeners, s, &s->seat->events.request_set_primary_selection, seat_request_set_primary_selection);
    if (!ok)
        wlr_log(WLR_ERROR, "could not set up input listeners");
    comp_input_update_capabilities(s);
}
