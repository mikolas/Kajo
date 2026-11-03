#define _GNU_SOURCE
#include "osk.h"
#include "osk_xkb.h"
#include "../shell.h"
#include "../config.h"
#include "../compositor/compositor.h"
#include "../hardware/hw_caps.h"
#include "../posture/posture_manager.h"

#include <gtk4-layer-shell.h>
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"

typedef enum {
    OSK_BTN_EVDEV = 0,     /* Regular evdev keycode mapped dynamically via XKB */
    OSK_BTN_SYMBOL,        /* Direct UTF-8 symbol resolved via shell_osk_xkb_find_symbol */
    OSK_BTN_MOD_SHIFT,     /* Sticky Shift / Caps Lock */
    OSK_BTN_MOD_CTRL,      /* Sticky Control */
    OSK_BTN_MOD_ALT,       /* Sticky Alt */
    OSK_BTN_MOD_SUPER,     /* Sticky Super */
    OSK_BTN_MOD_ALTGR,     /* Sticky AltGr (ISO_Level3_Shift) */
    OSK_BTN_LAYER_SYM,     /* Switch to ?123 symbol layer */
    OSK_BTN_LAYER_ALPHA,   /* Switch to ABC alpha layer */
    OSK_BTN_GLOBE,         /* Switch keyboard layout group */
    OSK_BTN_HIDE           /* Hide OSK */
} OskButtonKind;

typedef enum {
    MOD_STATE_OFF = 0,
    MOD_STATE_LATCHED,
    MOD_STATE_LOCKED
} OskModState;

typedef struct {
    ShellOSK     *osk;
    OskButtonKind kind;
    uint32_t      evdev_code;
    const char   *static_label;
    const char   *symbol_utf8;
    gboolean      repeatable;
    GtkWidget    *button;
    GtkWidget    *primary_label;
    GtkWidget    *hint_label;
} OskKeyButton;

struct _ShellOSK {
    ShellApp                              *app;
    ShellOskXkb                           *xkb;

    /* Wayland objects */
    struct wl_display                     *wl_display;
    struct wl_registry                    *wl_registry;
    struct wl_seat                        *wl_seat;
    struct wl_keyboard                    *wl_keyboard;
    struct zwp_virtual_keyboard_manager_v1 *vkbd_manager;
    struct zwp_virtual_keyboard_v1        *vkbd;
    struct zwp_input_method_manager_v2    *im_manager;
    struct zwp_input_method_v2            *im;

    /* Input-method state */
    gboolean                               pending_im_active;
    gboolean                               im_active;
    gboolean                               visible;
    gboolean                               manual_pinned;

    /* Sticky modifier states */
    OskModState                            mod_shift;
    OskModState                            mod_ctrl;
    OskModState                            mod_alt;
    OskModState                            mod_super;
    OskModState                            mod_altgr;

    /* Hold-to-repeat timers */
    OskKeyButton                          *repeat_btn;
    guint                                  repeat_delay_id;
    guint                                  repeat_tick_id;

    /* GTK4 UI */
    GtkWindow                             *window;
    GtkStack                              *layer_stack;
    GPtrArray                             *key_buttons; /* Array of OskKeyButton* */
};

static void refresh_all_key_labels(ShellOSK *self);
static void fire_key_button(ShellOSK *self, OskKeyButton *kb);

static uint32_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

static void stop_repeat_timers(ShellOSK *self) {
    if (!self) return;
    if (self->repeat_delay_id > 0) {
        g_source_remove(self->repeat_delay_id);
        self->repeat_delay_id = 0;
    }
    if (self->repeat_tick_id > 0) {
        g_source_remove(self->repeat_tick_id);
        self->repeat_tick_id = 0;
    }
    self->repeat_btn = NULL;
}

static void upload_current_xkb_keymap(ShellOSK *self) {
    if (!self || !self->vkbd || !self->xkb) return;
    const char *km_str = shell_osk_xkb_get_keymap_string(self->xkb);
    if (!km_str) return;

    size_t size = strlen(km_str) + 1;
    int fd = memfd_create("kajo-osk-keymap", MFD_CLOEXEC);
    if (fd < 0) return;

    ssize_t written = write(fd, km_str, size);
    if (written == (ssize_t)size) {
        lseek(fd, 0, SEEK_SET);
        zwp_virtual_keyboard_v1_keymap(
            self->vkbd,
            WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
            fd,
            (uint32_t)size
        );
        if (self->wl_display) {
            wl_display_flush(self->wl_display);
        }
    }
    close(fd);
}

static uint32_t compute_active_depressed_mods(ShellOSK *self) {
    if (!self || !self->xkb) return 0;
    uint32_t mods = 0;
    if (self->mod_shift != MOD_STATE_OFF) mods |= shell_osk_xkb_get_shift_mask(self->xkb);
    if (self->mod_ctrl  != MOD_STATE_OFF) mods |= shell_osk_xkb_get_ctrl_mask(self->xkb);
    if (self->mod_alt   != MOD_STATE_OFF) mods |= shell_osk_xkb_get_alt_mask(self->xkb);
    if (self->mod_super != MOD_STATE_OFF) mods |= shell_osk_xkb_get_super_mask(self->xkb);
    if (self->mod_altgr != MOD_STATE_OFF) mods |= shell_osk_xkb_get_altgr_mask(self->xkb);
    return mods;
}

static void sync_vkbd_modifiers(ShellOSK *self, uint32_t depressed_override, gboolean use_override) {
    if (!self || !self->vkbd || !self->xkb) return;
    uint32_t depressed = use_override ? depressed_override : compute_active_depressed_mods(self);
    uint32_t locked = (self->mod_shift == MOD_STATE_LOCKED) ? shell_osk_xkb_get_caps_mask(self->xkb) : 0;
    uint32_t group = shell_osk_xkb_get_group(self->xkb);

    zwp_virtual_keyboard_v1_modifiers(self->vkbd, depressed, 0, locked, group);
}

static void clear_latched_modifiers(ShellOSK *self) {
    if (!self) return;
    gboolean changed = FALSE;
    if (self->mod_shift == MOD_STATE_LATCHED) { self->mod_shift = MOD_STATE_OFF; changed = TRUE; }
    if (self->mod_ctrl  == MOD_STATE_LATCHED) { self->mod_ctrl  = MOD_STATE_OFF; changed = TRUE; }
    if (self->mod_alt   == MOD_STATE_LATCHED) { self->mod_alt   = MOD_STATE_OFF; changed = TRUE; }
    if (self->mod_super == MOD_STATE_LATCHED) { self->mod_super = MOD_STATE_OFF; changed = TRUE; }
    if (self->mod_altgr == MOD_STATE_LATCHED) { self->mod_altgr = MOD_STATE_OFF; changed = TRUE; }

    if (changed) {
        sync_vkbd_modifiers(self, 0, FALSE);
        refresh_all_key_labels(self);
    }
}

static void send_evdev_key_tap(ShellOSK *self, uint32_t evdev_code, uint32_t mods_mask, gboolean use_mods_override) {
    if (!self || !self->vkbd || evdev_code == 0) return;

    uint32_t now = get_monotonic_ms();
    sync_vkbd_modifiers(self, mods_mask, use_mods_override);
    zwp_virtual_keyboard_v1_key(self->vkbd, now, evdev_code, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(self->vkbd, now + 1, evdev_code, WL_KEYBOARD_KEY_STATE_RELEASED);

    if (use_mods_override) {
        sync_vkbd_modifiers(self, 0, FALSE);
    }
    if (self->wl_display) {
        wl_display_flush(self->wl_display);
    }
}

/* --- Wayland Seat & Keyboard Listeners --- */

static void on_kb_keymap(void *data, struct wl_keyboard *wl_kb G_GNUC_UNUSED,
                         uint32_t format, int32_t fd, uint32_t size) {
    ShellOSK *self = data;
    if (!self) {
        if (fd >= 0) close(fd);
        return;
    }

    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && fd >= 0 && size > 0) {
        void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) {
            map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        }
        if (map != MAP_FAILED) {
            shell_osk_xkb_load_from_string(self->xkb, (const char *)map);
            munmap(map, size);
        }
        if (self->vkbd) {
            lseek(fd, 0, SEEK_SET);
            zwp_virtual_keyboard_v1_keymap(self->vkbd, format, fd, size);
            if (self->wl_display) wl_display_flush(self->wl_display);
        }
        refresh_all_key_labels(self);
    }

    if (fd >= 0) close(fd);
}

static void on_kb_enter(void *data G_GNUC_UNUSED, struct wl_keyboard *wl_kb G_GNUC_UNUSED,
                        uint32_t serial G_GNUC_UNUSED, struct wl_surface *surface G_GNUC_UNUSED,
                        struct wl_array *keys G_GNUC_UNUSED) {}

static void on_kb_leave(void *data G_GNUC_UNUSED, struct wl_keyboard *wl_kb G_GNUC_UNUSED,
                        uint32_t serial G_GNUC_UNUSED, struct wl_surface *surface G_GNUC_UNUSED) {}

static void on_kb_key(void *data G_GNUC_UNUSED, struct wl_keyboard *wl_kb G_GNUC_UNUSED,
                      uint32_t serial G_GNUC_UNUSED, uint32_t time G_GNUC_UNUSED,
                      uint32_t key G_GNUC_UNUSED, uint32_t state G_GNUC_UNUSED) {}

static void on_kb_modifiers(void *data, struct wl_keyboard *wl_kb G_GNUC_UNUSED,
                            uint32_t serial G_GNUC_UNUSED, uint32_t mods_depressed G_GNUC_UNUSED,
                            uint32_t mods_latched G_GNUC_UNUSED, uint32_t mods_locked G_GNUC_UNUSED,
                            uint32_t group) {
    ShellOSK *self = data;
    if (!self || !self->xkb) return;
    if (shell_osk_xkb_get_group(self->xkb) != group) {
        shell_osk_xkb_set_group(self->xkb, group);
        sync_vkbd_modifiers(self, 0, FALSE);
        refresh_all_key_labels(self);
    }
}

static void on_kb_repeat_info(void *data G_GNUC_UNUSED, struct wl_keyboard *wl_kb G_GNUC_UNUSED,
                              int32_t rate G_GNUC_UNUSED, int32_t delay G_GNUC_UNUSED) {}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = on_kb_keymap,
    .enter       = on_kb_enter,
    .leave       = on_kb_leave,
    .key         = on_kb_key,
    .modifiers   = on_kb_modifiers,
    .repeat_info = on_kb_repeat_info,
};

static void on_seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    ShellOSK *self = data;
    if (!self) return;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->wl_keyboard) {
        self->wl_keyboard = wl_seat_get_keyboard(seat);
        if (self->wl_keyboard) {
            wl_keyboard_add_listener(self->wl_keyboard, &keyboard_listener, self);
        }
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && self->wl_keyboard) {
        wl_keyboard_destroy(self->wl_keyboard);
        self->wl_keyboard = NULL;
    }
}

static void on_seat_name(void *data G_GNUC_UNUSED, struct wl_seat *seat G_GNUC_UNUSED,
                         const char *name G_GNUC_UNUSED) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = on_seat_capabilities,
    .name         = on_seat_name,
};

/* --- Wayland Input Method v2 Listener --- */

static gboolean should_auto_show_osk(ShellOSK *self) {
    if (!self || !self->app) return FALSE;
    ShellConfig *cfg = shell_app_get_config(self->app);
    const char *mode = (cfg && cfg->osk_auto_show_mode) ? cfg->osk_auto_show_mode : "tablet";

    if (g_strcmp0(mode, "manual") == 0) {
        return FALSE;
    }
    if (g_strcmp0(mode, "always") == 0) {
        return TRUE;
    }

    /* Default: "tablet" mode */
    if (self->app->posture_manager &&
        shell_posture_manager_get_state(self->app->posture_manager) == SHELL_POSTURE_FOLDED) {
        return TRUE;
    }

    const ShellHwCaps *caps = shell_hw_caps_get(FALSE);
    if (caps && !caps->has_hinge_sensor && caps->has_touchscreen) {
        return TRUE;
    }

    return FALSE;
}

static void on_im_activate(void *data, struct zwp_input_method_v2 *im G_GNUC_UNUSED) {
    ShellOSK *self = data;
    if (self) self->pending_im_active = TRUE;
}

static void on_im_deactivate(void *data, struct zwp_input_method_v2 *im G_GNUC_UNUSED) {
    ShellOSK *self = data;
    if (self) self->pending_im_active = FALSE;
}

static void on_im_surrounding_text(void *data G_GNUC_UNUSED, struct zwp_input_method_v2 *im G_GNUC_UNUSED,
                                   const char *text G_GNUC_UNUSED, uint32_t cursor G_GNUC_UNUSED,
                                   uint32_t anchor G_GNUC_UNUSED) {}

static void on_im_text_change_cause(void *data G_GNUC_UNUSED, struct zwp_input_method_v2 *im G_GNUC_UNUSED,
                                    uint32_t cause G_GNUC_UNUSED) {}

static void on_im_content_type(void *data G_GNUC_UNUSED, struct zwp_input_method_v2 *im G_GNUC_UNUSED,
                               uint32_t hint G_GNUC_UNUSED, uint32_t purpose G_GNUC_UNUSED) {}

static void on_im_done(void *data, struct zwp_input_method_v2 *im G_GNUC_UNUSED) {
    ShellOSK *self = data;
    if (!self) return;

    gboolean prev = self->im_active;
    self->im_active = self->pending_im_active;

    if (self->im_active && !prev) {
        if (should_auto_show_osk(self)) {
            shell_osk_show(self, FALSE);
        }
    } else if (!self->im_active && prev) {
        if (self->visible && !self->manual_pinned) {
            shell_osk_hide(self);
        }
    }
}

static void on_im_unavailable(void *data, struct zwp_input_method_v2 *im) {
    ShellOSK *self = data;
    if (!self) return;
    if (self->im == im) {
        zwp_input_method_v2_destroy(self->im);
        self->im = NULL;
    }
}

static const struct zwp_input_method_v2_listener im_listener = {
    .activate          = on_im_activate,
    .deactivate        = on_im_deactivate,
    .surrounding_text  = on_im_surrounding_text,
    .text_change_cause = on_im_text_change_cause,
    .content_type      = on_im_content_type,
    .done              = on_im_done,
    .unavailable       = on_im_unavailable,
};

static void try_bind_seat_protocols(ShellOSK *self) {
    if (!self || !self->wl_seat) return;

    if (self->vkbd_manager && !self->vkbd) {
        self->vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
            self->vkbd_manager, self->wl_seat
        );
        upload_current_xkb_keymap(self);
    }

    if (self->im_manager && !self->im) {
        self->im = zwp_input_method_manager_v2_get_input_method(
            self->im_manager, self->wl_seat
        );
        if (self->im) {
            zwp_input_method_v2_add_listener(self->im, &im_listener, self);
        }
    }
}

static void on_registry_global(void *data, struct wl_registry *registry,
                               uint32_t name, const char *interface, uint32_t version) {
    ShellOSK *self = data;
    if (!self || !interface) return;

    if (strcmp(interface, wl_seat_interface.name) == 0 && !self->wl_seat) {
        uint32_t bind_ver = version < 5 ? version : 5;
        self->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, bind_ver);
        if (self->wl_seat) {
            wl_seat_add_listener(self->wl_seat, &seat_listener, self);
            try_bind_seat_protocols(self);
        }
    } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0 && !self->vkbd_manager) {
        self->vkbd_manager = wl_registry_bind(
            registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1
        );
        try_bind_seat_protocols(self);
    } else if (strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0 && !self->im_manager) {
        self->im_manager = wl_registry_bind(
            registry, name, &zwp_input_method_manager_v2_interface, 1
        );
        try_bind_seat_protocols(self);
    }
}

static void on_registry_global_remove(void *data G_GNUC_UNUSED,
                                      struct wl_registry *registry G_GNUC_UNUSED,
                                      uint32_t name G_GNUC_UNUSED) {}

static const struct wl_registry_listener registry_listener = {
    .global        = on_registry_global,
    .global_remove = on_registry_global_remove,
};

/* --- Key Action & Repeat Engine --- */

static gboolean on_repeat_tick(gpointer user_data) {
    ShellOSK *self = user_data;
    if (!self || !self->repeat_btn || !self->visible) {
        if (self) self->repeat_tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    fire_key_button(self, self->repeat_btn);
    return G_SOURCE_CONTINUE;
}

static gboolean on_repeat_delay_done(gpointer user_data) {
    ShellOSK *self = user_data;
    if (!self || !self->repeat_btn || !self->visible) {
        if (self) self->repeat_delay_id = 0;
        return G_SOURCE_REMOVE;
    }
    self->repeat_delay_id = 0;
    fire_key_button(self, self->repeat_btn);
    self->repeat_tick_id = g_timeout_add(45, on_repeat_tick, self);
    return G_SOURCE_REMOVE;
}

static void toggle_sticky_mod(ShellOSK *self, OskModState *mod_ptr, gboolean allow_lock) {
    if (!self || !mod_ptr) return;
    if (*mod_ptr == MOD_STATE_OFF) {
        *mod_ptr = MOD_STATE_LATCHED;
    } else if (*mod_ptr == MOD_STATE_LATCHED && allow_lock) {
        *mod_ptr = MOD_STATE_LOCKED;
    } else {
        *mod_ptr = MOD_STATE_OFF;
    }
    sync_vkbd_modifiers(self, 0, FALSE);
    if (self->wl_display) wl_display_flush(self->wl_display);
    refresh_all_key_labels(self);
}

static void fire_key_button(ShellOSK *self, OskKeyButton *kb) {
    if (!self || !kb) return;

    switch (kb->kind) {
        case OSK_BTN_EVDEV: {
            send_evdev_key_tap(self, kb->evdev_code, 0, FALSE);
            clear_latched_modifiers(self);
            break;
        }
        case OSK_BTN_SYMBOL: {
            if (!kb->symbol_utf8) break;
            ShellOskSymbolBinding bind = shell_osk_xkb_find_symbol(self->xkb, kb->symbol_utf8);
            if (bind.found) {
                uint32_t extra_mods = compute_active_depressed_mods(self) &
                    ~(shell_osk_xkb_get_shift_mask(self->xkb) | shell_osk_xkb_get_altgr_mask(self->xkb));
                send_evdev_key_tap(self, bind.evdev_code, bind.mods_mask | extra_mods, TRUE);
                if (bind.is_dead_key) {
                    /* Commit dead accent as literal ASCII character by tapping Space (57) */
                    send_evdev_key_tap(self, 57, 0, TRUE);
                }
            }
            clear_latched_modifiers(self);
            break;
        }
        case OSK_BTN_MOD_SHIFT:
            toggle_sticky_mod(self, &self->mod_shift, TRUE);
            break;
        case OSK_BTN_MOD_CTRL:
            toggle_sticky_mod(self, &self->mod_ctrl, FALSE);
            break;
        case OSK_BTN_MOD_ALT:
            toggle_sticky_mod(self, &self->mod_alt, FALSE);
            break;
        case OSK_BTN_MOD_SUPER:
            toggle_sticky_mod(self, &self->mod_super, FALSE);
            break;
        case OSK_BTN_MOD_ALTGR:
            toggle_sticky_mod(self, &self->mod_altgr, FALSE);
            break;
        case OSK_BTN_LAYER_SYM:
            if (self->layer_stack) {
                gtk_stack_set_visible_child_name(self->layer_stack, "symbols");
            }
            break;
        case OSK_BTN_LAYER_ALPHA:
            if (self->layer_stack) {
                gtk_stack_set_visible_child_name(self->layer_stack, "alpha");
            }
            break;
        case OSK_BTN_GLOBE: {
            uint32_t num_groups = shell_osk_xkb_get_num_groups(self->xkb);
            uint32_t next_group = (shell_osk_xkb_get_group(self->xkb) + 1) % (num_groups > 0 ? num_groups : 1);
            shell_osk_xkb_set_group(self->xkb, next_group);
            if (self->app && self->app->compositor) {
                shell_compositor_switch_keyboard_layout_idx(self->app->compositor, (guint8)next_group);
            }
            sync_vkbd_modifiers(self, 0, FALSE);
            refresh_all_key_labels(self);
            break;
        }
        case OSK_BTN_HIDE:
            shell_osk_hide(self);
            break;
    }
}

static void on_key_gesture_pressed(GtkGestureClick *gesture G_GNUC_UNUSED,
                                   gint n_press G_GNUC_UNUSED,
                                   gdouble x G_GNUC_UNUSED,
                                   gdouble y G_GNUC_UNUSED,
                                   gpointer user_data) {
    OskKeyButton *kb = user_data;
    if (!kb || !kb->osk) return;
    ShellOSK *self = kb->osk;

    stop_repeat_timers(self);
    fire_key_button(self, kb);

    if (kb->repeatable) {
        self->repeat_btn = kb;
        self->repeat_delay_id = g_timeout_add(400, on_repeat_delay_done, self);
    }
}

static void on_key_gesture_released(GtkGestureClick *gesture G_GNUC_UNUSED,
                                    gint n_press G_GNUC_UNUSED,
                                    gdouble x G_GNUC_UNUSED,
                                    gdouble y G_GNUC_UNUSED,
                                    gpointer user_data) {
    OskKeyButton *kb = user_data;
    if (!kb || !kb->osk) return;
    stop_repeat_timers(kb->osk);
}

static void on_key_gesture_stopped(GtkGestureClick *gesture G_GNUC_UNUSED,
                                   gpointer user_data) {
    OskKeyButton *kb = user_data;
    if (!kb || !kb->osk) return;
    stop_repeat_timers(kb->osk);
}

/* --- Key Grid Construction & Dynamic Label Refresh --- */

static void update_mod_button_classes(GtkWidget *btn, OskModState state) {
    if (!btn) return;
    gtk_widget_remove_css_class(btn, "osk-key-latched");
    gtk_widget_remove_css_class(btn, "osk-key-locked");
    if (state == MOD_STATE_LATCHED) {
        gtk_widget_add_css_class(btn, "osk-key-latched");
    } else if (state == MOD_STATE_LOCKED) {
        gtk_widget_add_css_class(btn, "osk-key-locked");
    }
}

static void refresh_all_key_labels(ShellOSK *self) {
    if (!self || !self->key_buttons || !self->xkb) return;

    gboolean shift_on = (self->mod_shift == MOD_STATE_LATCHED);
    gboolean caps_on  = (self->mod_shift == MOD_STATE_LOCKED);
    gboolean altgr_on = (self->mod_altgr != MOD_STATE_OFF);

    char group_short[8] = "US";
    shell_osk_xkb_get_short_group_label(self->xkb, group_short, sizeof(group_short));

    for (guint i = 0; i < self->key_buttons->len; i++) {
        OskKeyButton *kb = g_ptr_array_index(self->key_buttons, i);
        if (!kb) continue;

        if (kb->kind == OSK_BTN_EVDEV && kb->static_label == NULL) {
            char main_lbl[16] = {0};
            char hint_lbl[16] = {0};
            if (shell_osk_xkb_get_key_label(self->xkb, kb->evdev_code, shift_on, caps_on, altgr_on, main_lbl, sizeof(main_lbl))) {
                gtk_label_set_text(GTK_LABEL(kb->primary_label), main_lbl);
                gtk_widget_set_sensitive(kb->button, TRUE);
            } else {
                gtk_label_set_text(GTK_LABEL(kb->primary_label), "");
            }

            if (kb->hint_label) {
                if (shell_osk_xkb_get_key_hint(self->xkb, kb->evdev_code, shift_on || caps_on, altgr_on, hint_lbl, sizeof(hint_lbl))) {
                    gtk_label_set_text(GTK_LABEL(kb->hint_label), hint_lbl);
                    gtk_widget_set_visible(kb->hint_label, TRUE);
                } else {
                    gtk_label_set_text(GTK_LABEL(kb->hint_label), "");
                    gtk_widget_set_visible(kb->hint_label, FALSE);
                }
            }
        } else if (kb->kind == OSK_BTN_GLOBE) {
            gtk_label_set_text(GTK_LABEL(kb->primary_label), group_short);
        } else if (kb->kind == OSK_BTN_MOD_SHIFT) {
            update_mod_button_classes(kb->button, self->mod_shift);
            gtk_label_set_text(GTK_LABEL(kb->primary_label),
                self->mod_shift == MOD_STATE_LOCKED ? "⇪" : "⇧");
        } else if (kb->kind == OSK_BTN_MOD_CTRL) {
            update_mod_button_classes(kb->button, self->mod_ctrl);
        } else if (kb->kind == OSK_BTN_MOD_ALT) {
            update_mod_button_classes(kb->button, self->mod_alt);
        } else if (kb->kind == OSK_BTN_MOD_SUPER) {
            update_mod_button_classes(kb->button, self->mod_super);
        } else if (kb->kind == OSK_BTN_MOD_ALTGR) {
            update_mod_button_classes(kb->button, self->mod_altgr);
        }
    }
}

static GtkWidget *create_osk_key(
    ShellOSK     *self,
    OskButtonKind kind,
    uint32_t      evdev_code,
    const char   *static_label,
    const char   *symbol_utf8,
    gboolean      repeatable,
    gboolean      is_special,
    gboolean      hexpand_wide
) {
    OskKeyButton *kb = g_new0(OskKeyButton, 1);
    kb->osk = self;
    kb->kind = kind;
    kb->evdev_code = evdev_code;
    kb->static_label = static_label;
    kb->symbol_utf8 = symbol_utf8;
    kb->repeatable = repeatable;

    GtkWidget *btn = gtk_button_new();
    gtk_widget_set_can_focus(btn, FALSE);
    gtk_widget_set_focus_on_click(btn, FALSE);
    gtk_widget_set_hexpand(btn, TRUE);
    gtk_widget_set_vexpand(btn, TRUE);
    gtk_widget_add_css_class(btn, "osk-key");
    if (is_special) {
        gtk_widget_add_css_class(btn, "osk-key-special");
    }
    if (hexpand_wide) {
        gtk_widget_add_css_class(btn, "osk-key-space");
    }

    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(overlay, TRUE);
    gtk_widget_set_vexpand(overlay, TRUE);

    const char *init_text = static_label ? static_label : (symbol_utf8 ? symbol_utf8 : "");
    GtkWidget *lbl = gtk_label_new(init_text);
    gtk_widget_add_css_class(lbl, "osk-key-label");
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_vexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), lbl);

    if (kind == OSK_BTN_EVDEV && static_label == NULL) {
        GtkWidget *hint = gtk_label_new("");
        gtk_widget_add_css_class(hint, "osk-key-hint");
        gtk_widget_set_halign(hint, GTK_ALIGN_END);
        gtk_widget_set_valign(hint, GTK_ALIGN_START);
        gtk_widget_set_can_target(hint, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), hint);
        kb->hint_label = hint;
    }

    gtk_button_set_child(GTK_BUTTON(btn), overlay);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_key_gesture_pressed), kb);
    g_signal_connect(click, "released", G_CALLBACK(on_key_gesture_released), kb);
    g_signal_connect(click, "stopped", G_CALLBACK(on_key_gesture_stopped), kb);
    gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(click));

    kb->button = btn;
    kb->primary_label = lbl;
    g_ptr_array_add(self->key_buttons, kb);
    return btn;
}

static GtkWidget *create_osk_grid(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_hexpand(grid, TRUE);
    gtk_widget_set_vexpand(grid, TRUE);
    return grid;
}

static GtkWidget *build_alpha_layer(ShellOSK *self) {
    /* 28-column homogeneous grid (1 standard key = 2 columns) modeled after Windows Touch Keyboard */
    GtkWidget *grid = create_osk_grid();
    int col = 0;

    /* Row 0: Esc (2), 1..0 + (22), Backspace (4) = 28 cols */
    col = 0;
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 1, "Esc", NULL, FALSE, TRUE, FALSE), col, 0, 2, 1);
    col += 2;
    for (uint32_t code = 2; code <= 12; code++) {
        gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, code, NULL, NULL, FALSE, FALSE, FALSE), col, 0, 2, 1);
        col += 2;
    }
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 14, "⌫", NULL, TRUE, TRUE, FALSE), col, 0, 4, 1);

    /* Row 1: Tab (3), q w e r t y u i o p å (22), Del (3) = 28 cols */
    col = 0;
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 15, "Tab", NULL, FALSE, TRUE, FALSE), col, 1, 3, 1);
    col += 3;
    for (uint32_t code = 16; code <= 26; code++) {
        gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, code, NULL, NULL, FALSE, FALSE, FALSE), col, 1, 2, 1);
        col += 2;
    }
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 111, "Del", NULL, TRUE, TRUE, FALSE), col, 1, 3, 1);

    /* Row 2: <> (2), a s d f g h j k l ö ä (22), Enter (4) = 28 cols */
    col = 0;
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 86, NULL, NULL, FALSE, FALSE, FALSE), col, 2, 2, 1);
    col += 2;
    for (uint32_t code = 30; code <= 40; code++) {
        gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, code, NULL, NULL, FALSE, FALSE, FALSE), col, 2, 2, 1);
        col += 2;
    }
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 28, "⏎", NULL, FALSE, TRUE, FALSE), col, 2, 4, 1);

    /* Row 3: Left Shift (4), z x c v b n m , . - (20), Right Shift (4) = 28 cols */
    col = 0;
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_MOD_SHIFT, 0, "⇧", NULL, FALSE, TRUE, FALSE), col, 3, 4, 1);
    col += 4;
    for (uint32_t code = 44; code <= 53; code++) {
        gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, code, NULL, NULL, FALSE, FALSE, FALSE), col, 3, 2, 1);
        col += 2;
    }
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_MOD_SHIFT, 0, "⇧", NULL, FALSE, TRUE, FALSE), col, 3, 4, 1);

    /* Row 4: &123 (2), Ctrl (2), Super (2), Alt (2), Globe (2), Space (6), AltGr (2), Arrows (8), Hide (2) = 28 cols */
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_LAYER_SYM, 0, "&123",  NULL, FALSE, TRUE,  FALSE),  0, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_MOD_CTRL,  0, "Ctrl",  NULL, FALSE, TRUE,  FALSE),  2, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_MOD_SUPER, 0, "Super", NULL, FALSE, TRUE,  FALSE),  4, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_MOD_ALT,   0, "Alt",   NULL, FALSE, TRUE,  FALSE),  6, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_GLOBE,     0, "FI",    NULL, FALSE, TRUE,  FALSE),  8, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV,    57, "",      NULL, TRUE,  FALSE, TRUE),  10, 4, 6, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_MOD_ALTGR, 0, "AltGr", NULL, FALSE, TRUE,  FALSE), 16, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV,   105, "←",     NULL, TRUE,  TRUE,  FALSE), 18, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV,   103, "↑",     NULL, TRUE,  TRUE,  FALSE), 20, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV,   108, "↓",     NULL, TRUE,  TRUE,  FALSE), 22, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV,   106, "→",     NULL, TRUE,  TRUE,  FALSE), 24, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_HIDE,      0, "⌨↓",    NULL, FALSE, TRUE,  FALSE), 26, 4, 2, 1);

    return grid;
}

static GtkWidget *build_symbols_layer(ShellOSK *self) {
    GtkWidget *grid = create_osk_grid();
    int col = 0;

    /* Row 0: Esc (2), F1..F12 (24), Backspace (2) = 28 cols */
    col = 0;
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 1, "Esc", NULL, FALSE, TRUE, FALSE), col, 0, 2, 1);
    col += 2;
    static const struct { uint32_t code; const char *lbl; } fkeys[] = {
        {59, "F1"}, {60, "F2"}, {61, "F3"}, {62, "F4"}, {63, "F5"}, {64, "F6"},
        {65, "F7"}, {66, "F8"}, {67, "F9"}, {68, "F10"}, {87, "F11"}, {88, "F12"}
    };
    for (size_t i = 0; i < G_N_ELEMENTS(fkeys); i++) {
        gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, fkeys[i].code, fkeys[i].lbl, NULL, FALSE, TRUE, FALSE), col, 0, 2, 1);
        col += 2;
    }
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 14, "⌫", NULL, TRUE, TRUE, FALSE), col, 0, 2, 1);

    /* Row 1: Direct one-tap symbols (@ # € $ % & { [ ] } \ |) (24) + Home (2) + End (2) = 28 cols */
    col = 0;
    static const char *sym_row1[] = { "@", "#", "€", "$", "%", "&", "{", "[", "]", "}", "\\", "|" };
    for (size_t i = 0; i < G_N_ELEMENTS(sym_row1); i++) {
        gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_SYMBOL, 0, NULL, sym_row1[i], FALSE, FALSE, FALSE), col, 1, 2, 1);
        col += 2;
    }
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 102, "Home", NULL, FALSE, TRUE, FALSE), 24, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 107, "End",  NULL, FALSE, TRUE, FALSE), 26, 1, 2, 1);

    /* Row 2: Direct one-tap symbols (~ ^ < > ( ) - _ = + * /) (24) + PgUp (2) + PgDn (2) = 28 cols */
    col = 0;
    static const char *sym_row2[] = { "~", "^", "<", ">", "(", ")", "-", "_", "=", "+", "*", "/" };
    for (size_t i = 0; i < G_N_ELEMENTS(sym_row2); i++) {
        gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_SYMBOL, 0, NULL, sym_row2[i], FALSE, FALSE, FALSE), col, 2, 2, 1);
        col += 2;
    }
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 104, "PgUp", NULL, FALSE, TRUE, FALSE), 24, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 109, "PgDn", NULL, FALSE, TRUE, FALSE), 26, 2, 2, 1);

    /* Row 3: Direct one-tap punctuation (! ? " ' ` : ; , .) + ´ + Ins + Del + Enter (4) = 28 cols */
    col = 0;
    static const char *sym_row3[] = { "!", "?", "\"", "'", "`", ":", ";", ",", "." };
    for (size_t i = 0; i < G_N_ELEMENTS(sym_row3); i++) {
        gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_SYMBOL, 0, NULL, sym_row3[i], FALSE, FALSE, FALSE), col, 3, 2, 1);
        col += 2;
    }
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 13,  NULL,  NULL, FALSE, FALSE, FALSE), 18, 3, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 110, "Ins", NULL, FALSE, TRUE,  FALSE), 20, 3, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 111, "Del", NULL, TRUE,  TRUE,  FALSE), 22, 3, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV, 28,  "⏎",   NULL, FALSE, TRUE,  FALSE), 24, 3, 4, 1);

    /* Row 4: ABC (2), Ctrl (2), Super (2), Alt (2), Globe (2), Space (6), AltGr (2), Arrows (8), Hide (2) = 28 cols */
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_LAYER_ALPHA, 0, "ABC",   NULL, FALSE, TRUE,  FALSE),  0, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_MOD_CTRL,    0, "Ctrl",  NULL, FALSE, TRUE,  FALSE),  2, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_MOD_SUPER,   0, "Super", NULL, FALSE, TRUE,  FALSE),  4, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_MOD_ALT,     0, "Alt",   NULL, FALSE, TRUE,  FALSE),  6, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_GLOBE,       0, "FI",    NULL, FALSE, TRUE,  FALSE),  8, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV,      57, "",      NULL, TRUE,  FALSE, TRUE),  10, 4, 6, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_MOD_ALTGR,   0, "AltGr", NULL, FALSE, TRUE,  FALSE), 16, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV,     105, "←",     NULL, TRUE,  TRUE,  FALSE), 18, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV,     103, "↑",     NULL, TRUE,  TRUE,  FALSE), 20, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV,     108, "↓",     NULL, TRUE,  TRUE,  FALSE), 22, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_EVDEV,     106, "→",     NULL, TRUE,  TRUE,  FALSE), 24, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), create_osk_key(self, OSK_BTN_HIDE,        0, "⌨↓",    NULL, FALSE, TRUE,  FALSE), 26, 4, 2, 1);

    return grid;
}

static void build_osk_window(ShellOSK *self) {
    if (!self) return;

    GtkWidget *win = gtk_window_new();
    self->window = GTK_WINDOW(win);
    gtk_window_set_decorated(self->window, FALSE);
    gtk_widget_add_css_class(win, "osk-window");

    gtk_layer_init_for_window(self->window);
    gtk_layer_set_namespace(self->window, "kajo-osk");
    gtk_layer_set_layer(self->window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(self->window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    gtk_layer_set_anchor(self->window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(self->window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(self->window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(self->window, GTK_LAYER_SHELL_EDGE_TOP, FALSE);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(outer, "osk-container");
    gtk_widget_set_hexpand(outer, TRUE);
    gtk_widget_set_vexpand(outer, TRUE);
    gtk_widget_set_margin_start(outer, 6);
    gtk_widget_set_margin_end(outer, 6);
    gtk_widget_set_margin_top(outer, 5);
    gtk_widget_set_margin_bottom(outer, 5);

    GtkWidget *stack = gtk_stack_new();
    self->layer_stack = GTK_STACK(stack);
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_stack_set_transition_type(self->layer_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(self->layer_stack, 120);

    gtk_stack_add_named(self->layer_stack, build_alpha_layer(self), "alpha");
    gtk_stack_add_named(self->layer_stack, build_symbols_layer(self), "symbols");

    gtk_box_append(GTK_BOX(outer), stack);
    gtk_window_set_child(self->window, outer);

    shell_osk_apply_config(self);
    refresh_all_key_labels(self);
}

/* --- Public API --- */

ShellOSK *shell_osk_new(ShellApp *app) {
    ShellOSK *self = g_new0(ShellOSK, 1);
    self->app = app;
    self->xkb = shell_osk_xkb_new();
    self->key_buttons = g_ptr_array_new_with_free_func(g_free);

    build_osk_window(self);

    GdkDisplay *display = gdk_display_get_default();
    if (display && GDK_IS_WAYLAND_DISPLAY(display)) {
        self->wl_display = gdk_wayland_display_get_wl_display(display);
        if (self->wl_display) {
            self->wl_registry = wl_display_get_registry(self->wl_display);
            if (self->wl_registry) {
                wl_registry_add_listener(self->wl_registry, &registry_listener, self);
                wl_display_roundtrip(self->wl_display);
                /* Second roundtrip to receive wl_seat::capabilities and wl_keyboard::keymap */
                wl_display_roundtrip(self->wl_display);
            }
        }
    }

    return self;
}

void shell_osk_apply_config(ShellOSK *self) {
    if (!self || !self->window) return;
    ShellConfig *cfg = self->app ? shell_app_get_config(self->app) : NULL;
    gint height = (cfg && cfg->osk_height_px >= 180) ? cfg->osk_height_px : 260;
    gboolean push_windows = cfg ? cfg->osk_push_windows : TRUE;

    gtk_widget_set_size_request(GTK_WIDGET(self->window), -1, height);

    if (self->visible && push_windows) {
        gtk_layer_set_exclusive_zone(self->window, height);
        gtk_layer_set_margin(self->window, GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
    } else {
        gtk_layer_set_exclusive_zone(self->window, 0);
        gint bottom_margin = 0;
        if (cfg && cfg->panel_position == PANEL_POSITION_BOTTOM && !cfg->autohide_enabled) {
            bottom_margin = cfg->panel_height;
        }
        gtk_layer_set_margin(self->window, GTK_LAYER_SHELL_EDGE_BOTTOM, bottom_margin);
    }
}

void shell_osk_show(ShellOSK *self, gboolean manual_pin) {
    if (!self || !self->window) return;
    if (manual_pin) {
        self->manual_pinned = TRUE;
    }
    self->visible = TRUE;
    shell_osk_apply_config(self);
    refresh_all_key_labels(self);
    gtk_widget_set_visible(GTK_WIDGET(self->window), TRUE);
}

void shell_osk_hide(ShellOSK *self) {
    if (!self || !self->window) return;
    stop_repeat_timers(self);
    self->visible = FALSE;
    self->manual_pinned = FALSE;
    clear_latched_modifiers(self);
    gtk_layer_set_exclusive_zone(self->window, 0);
    gtk_widget_set_visible(GTK_WIDGET(self->window), FALSE);
}

void shell_osk_toggle(ShellOSK *self) {
    if (!self) return;
    if (self->visible) {
        shell_osk_hide(self);
    } else {
        shell_osk_show(self, TRUE);
    }
}

gboolean shell_osk_is_visible(ShellOSK *self) {
    return self ? self->visible : FALSE;
}

void shell_osk_destroy(ShellOSK *self) {
    if (!self) return;

    stop_repeat_timers(self);

    if (self->window) {
        gtk_window_destroy(self->window);
        self->window = NULL;
    }

    if (self->im) {
        zwp_input_method_v2_destroy(self->im);
        self->im = NULL;
    }
    if (self->im_manager) {
        zwp_input_method_manager_v2_destroy(self->im_manager);
        self->im_manager = NULL;
    }
    if (self->vkbd) {
        zwp_virtual_keyboard_v1_destroy(self->vkbd);
        self->vkbd = NULL;
    }
    if (self->vkbd_manager) {
        zwp_virtual_keyboard_manager_v1_destroy(self->vkbd_manager);
        self->vkbd_manager = NULL;
    }
    if (self->wl_keyboard) {
        wl_keyboard_destroy(self->wl_keyboard);
        self->wl_keyboard = NULL;
    }
    if (self->wl_seat) {
        wl_seat_destroy(self->wl_seat);
        self->wl_seat = NULL;
    }
    if (self->wl_registry) {
        wl_registry_destroy(self->wl_registry);
        self->wl_registry = NULL;
    }

    if (self->key_buttons) {
        g_ptr_array_free(self->key_buttons, TRUE);
        self->key_buttons = NULL;
    }
    if (self->xkb) {
        shell_osk_xkb_free(self->xkb);
        self->xkb = NULL;
    }

    g_free(self);
}
