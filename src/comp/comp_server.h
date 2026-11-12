/* comp_server.h — kajo-comp server state (Phase 2a + 2b).
 *
 * Ownership (spec §2.6): CompServer is the root talloc context. Each output,
 * window, popup, keyboard and pointer is a talloc child of the server; its
 * listeners are talloc children of the object (comp_listen), so freeing the
 * object unlinks everything it listened to. Objects free themselves from their
 * wlroots `destroy` handler.
 */
#ifndef KAJO_COMP_SERVER_H
#define KAJO_COMP_SERVER_H

#include "comp_listen.h"
#include "core/comp_binds.h"
#include "core/comp_config.h"
#include "core/comp_layout.h"
#include "core/comp_motion.h"
#include "core/comp_rules.h"
#include "core/comp_touch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wlr_allocator;
struct wlr_backend;
struct wlr_box;
struct wlr_buffer;
struct wlr_compositor;
struct wlr_cursor;
struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1;
struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request;
struct wlr_ext_foreign_toplevel_list_v1;
struct wlr_ext_image_copy_capture_manager_v1;
struct wlr_ext_output_image_capture_source_manager_v1;
struct wlr_export_dmabuf_manager_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_idle_inhibit_manager_v1;
struct wlr_idle_notifier_v1;
struct wlr_input_device;
struct wlr_input_method_manager_v2;
struct wlr_input_method_v2;
struct wlr_keyboard;
struct wlr_keyboard_shortcuts_inhibit_manager_v1;
struct wlr_layer_shell_v1;
struct wlr_output;
struct wlr_output_layout;
struct wlr_output_manager_v1;
struct wlr_output_power_manager_v1;
struct wlr_drm_lease_v1_manager;
struct wlr_pointer_constraint_v1;
struct wlr_pointer_constraints_v1;
struct wlr_pointer_gestures_v1;
struct wlr_relative_pointer_manager_v1;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_buffer;
struct wlr_scene_node;
struct wlr_scene_output_layout;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_screencopy_manager_v1;
struct wlr_seat;
struct wlr_security_context_manager_v1;
struct wlr_session;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_v1;
struct wlr_surface;
struct wlr_tablet_manager_v2;
struct wlr_text_input_manager_v3;
struct wlr_virtual_keyboard_manager_v1;
struct wlr_xcursor_manager;
struct wlr_xdg_activation_token_v1;
struct wlr_xdg_activation_v1;
struct wlr_xdg_decoration_manager_v1;
struct wlr_xdg_foreign_registry;
struct wlr_xdg_foreign_v2;
struct wlr_xdg_shell;
struct wlr_xdg_toplevel;
struct wlr_xdg_wm_dialog_v1;
struct wlr_xwayland;
struct wlr_xwayland_surface;

struct CompWindow;
struct CompSwitcher;
struct CompScreenshot;

typedef struct {
    const char *const *startup_argv;      /* optional: run once the socket exists (no shell) */
    const char        *config_path;       /* optional --config override */
    bool               exit_with_startup; /* terminate when that child exits (tests, greeter-like use) */
    bool               session_mode;      /* --session: primary desktop session */
    bool               greeter_mode;      /* --greeter-mode: locked-down pre-login greeter host */
    uint32_t           mod_mask;          /* CompModMask meaning of "mod"; Super by default */
} CompServerOptions;

typedef struct CompServer {
    CompServerOptions                     opts;
    struct wl_display                    *display;
    struct wl_event_loop                 *loop;
    struct wlr_backend                   *backend;
    struct wlr_session                   *session;      /* NULL when nested/headless */
    struct wlr_renderer                  *renderer;
    struct wlr_allocator                 *allocator;
    struct wlr_scene                     *scene;
    struct wlr_output_layout             *output_layout;
    struct wlr_scene_output_layout       *scene_layout;

    /* Ordered scene-tree hierarchy (spec §2.3) */
    struct wlr_scene_tree                *tree_background;
    struct wlr_scene_tree                *tree_bottom;
    struct wlr_scene_tree                *tree_tiled;
    struct wlr_scene_tree                *tree_floating;
    struct wlr_scene_tree                *tree_top;
    struct wlr_scene_tree                *tree_fullscreen;
    struct wlr_scene_tree                *tree_overlay;
    struct wlr_scene_tree                *tree_lock;

    struct wlr_compositor                        *compositor;
    struct wlr_xdg_shell                         *xdg_shell;
    struct wlr_xdg_decoration_manager_v1         *xdg_decoration_mgr;
    struct wlr_layer_shell_v1                    *layer_shell;
    struct wlr_session_lock_manager_v1           *session_lock_mgr;
    struct wlr_session_lock_v1                   *active_lock;
    struct wlr_scene_rect                        *desktop_bg;
    struct wlr_scene_rect                        *lock_backdrop;
    struct wlr_surface                           *lock_surface;
    bool                                          locked;

    struct wlr_idle_notifier_v1                  *idle_notifier;
    struct wlr_idle_inhibit_manager_v1           *idle_inhibit_mgr;
    struct wlr_output_manager_v1                 *output_mgr;
    struct wlr_output_power_manager_v1           *output_power_mgr;
    struct wlr_drm_lease_v1_manager              *drm_lease_mgr;

    struct wlr_input_method_manager_v2           *input_method_mgr;
    struct wlr_input_method_v2                   *active_input_method;
    struct wlr_text_input_manager_v3             *text_input_mgr;
    struct wlr_virtual_keyboard_manager_v1       *virtual_kbd_mgr;

    struct wlr_relative_pointer_manager_v1       *relative_pointer_mgr;
    struct wlr_pointer_constraints_v1            *pointer_constraints;
    struct wlr_pointer_constraint_v1             *active_constraint;
    struct wlr_pointer_gestures_v1               *pointer_gestures;
    struct wlr_cursor_shape_manager_v1           *cursor_shape_mgr;
    struct wlr_tablet_manager_v2                 *tablet_mgr;
    struct wlr_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit_mgr;
    struct wlr_xdg_foreign_registry              *foreign_registry;
    struct wlr_xdg_foreign_v2                    *foreign_v2;
    struct wlr_xdg_wm_dialog_v1                  *xdg_dialog_mgr;
    struct wlr_xdg_activation_v1                 *xdg_activation;
    struct wlr_foreign_toplevel_manager_v1       *foreign_toplevel_mgr;
    struct wlr_ext_foreign_toplevel_list_v1      *ext_foreign_toplevel_list;
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *ext_toplevel_capture_mgr;
    struct wlr_screencopy_manager_v1             *screencopy_mgr;
    struct wlr_export_dmabuf_manager_v1          *export_dmabuf_mgr;
    struct wlr_ext_image_copy_capture_manager_v1 *ext_image_copy_capture_mgr;
    struct wlr_ext_output_image_capture_source_manager_v1 *ext_output_capture_mgr;
    struct wlr_security_context_manager_v1       *security_context_mgr;
    struct wlr_xwayland                          *xwayland;
    int                                           xwayland_last_dpi;

    struct wlr_seat                      *seat;
    struct wlr_cursor                    *cursor;
    struct wlr_xcursor_manager           *cursor_mgr;
    const char                           *socket;       /* owned by libwayland */
    void                                 *listeners;    /* talloc ctx for listeners on server-owned
                                                           wlroots objects; freed first on teardown */

    struct wl_event_source               *signal_sources[3]; /* SIGINT, SIGTERM, SIGCHLD; removed on teardown */
    struct wl_event_source               *watchdog_timer;    /* systemd WatchdogSec heartbeat (Phase 4) */
    struct wl_event_source               *greeter_dpms_timer;/* 5-minute idle DPMS timer in --greeter-mode */
    bool                                  greeter_dpms_blanked;
    void                                 *renderer_lost_ctx; /* talloc ctx for renderer->events.lost */
    uint32_t                              gpu_recovery_count;
    struct wl_list                        outputs;        /* CompOutput.link (FIFO connector order) */
    struct wl_list                        keyboards;      /* CompKeyboard.link */
    struct wl_list                        input_devs;     /* CompInputDev.link (pointer/touch/tablet) */
    struct wl_list                        windows;        /* CompWindow.link */
    struct wl_list                        layer_surfaces; /* CompLayerSurface.link */
    struct wl_list                        xwayland_wins;  /* CompXWaylandWin.link */
    unsigned                              n_pointers;
    unsigned                              n_touch;
    CompTouchTracker                      touch_tracker;
    CompTouchpadSwipe                     touchpad_swipe;
    double                                wheel_last_fire_sec[8];

    /* Usable working area after layer-shell exclusive zones (logical px) */
    CompRect                              usable_area;
    bool                                  has_usable_area;
    struct wlr_surface                   *exclusive_layer_surface;

    /* Pure core state */
    CompConfig                           *config;
    CompLayout                           *layout;
    CompMotion                           *motion;
    CompBinds                            *binds;
    CompRules                            *rules;
    struct CompIpcState                  *ipc;
    struct CompSwitcher                  *switcher;
    struct CompScreenshot                *screenshot;
    size_t                                last_active_ws;

    /* Generational CompWinId slot table (spec §2.6) */
    struct CompWindow                   **win_slots;
    uint32_t                             *win_gens;
    size_t                                win_slots_cap;
    uint64_t                              focus_seq;
    CompWinId                             last_focused_id;

    /* Client-initiated floating move/resize grab (spec §3.11 L8) */
    CompWinId                             grab_win;
    bool                                  grab_resizing;
    uint32_t                              grab_edges;
    double                                grab_start_cx, grab_start_cy;
    CompRect                              grab_start_rect;

    int                                   startup_pid;  /* 0 when none or already exited */
    int                                   exit_status;  /* main() returns this */
    bool                                  nested_at_startup; /* true if WAYLAND_DISPLAY was set before start */

    /* Optional hook for tests/binds: called for every handled key press
     * before it is forwarded. Return true to consume. */
    bool                                (*key_hook)(struct CompServer *server, uint32_t mods,
                                                    uint32_t keysym, bool pressed);
} CompServer;

typedef struct {
    struct wl_list            link;    /* CompServer.outputs */
    CompServer               *server;
    struct wlr_output        *wlr_output;
    struct wlr_scene_output  *scene_output;
    CompVrrPolicy             vrr_policy;
    bool                      was_animating;
    bool                      pending_lock_present;
    bool                      has_manual_mode;
    char                     *manual_mode;
    bool                      has_manual_scale;
    double                    manual_scale;
    bool                      has_manual_transform;
    CompTransform             manual_transform;
    bool                      has_manual_vrr;
    CompVrrPolicy             manual_vrr;
} CompOutput;

typedef struct {
    CompWinId   id;
    const char *title;
    const char *app_id;
    size_t      workspace_idx; /* 1-based */
    bool        is_focused;
    bool        is_floating;
    bool        is_fullscreen;
    bool        is_urgent;
    uint64_t    focus_timestamp;
} CompWindowInfo;

/* Creates the display, backend, renderer and globals. Returns NULL on failure (logged). */
CompServer *comp_server_create(const CompServerOptions *opts);
/* Adds the socket, starts the backend and the startup command. */
bool        comp_server_start(CompServer *server);
void        comp_server_run(CompServer *server);
void        comp_server_destroy(CompServer *server);
void        comp_server_terminate(CompServer *server, int exit_status);
bool        comp_server_recover_gpu(CompServer *server);
bool        comp_sd_notify(const char *state);

/* Monotonic clock in seconds and active viewport / working-area dimensions. */
double             comp_now_sec(void);
struct wlr_output *comp_server_primary_output(const CompServer *server);
void               comp_server_viewport_size(const CompServer *server, double *out_w, double *out_h);
void               comp_server_working_area(const CompServer *server, CompRect *out_area);
void               comp_server_schedule_frames(CompServer *server);
void               comp_server_restore_keyboard_focus(CompServer *server);

/* Generational CompWinId table (spec §2.6). */
CompWinId          comp_win_alloc_id(CompServer *server, struct CompWindow *w);
void               comp_win_free_id(CompServer *server, CompWinId id);
struct CompWindow *comp_win_from_id(const CompServer *server, CompWinId id);

/* comp_output.c */
void comp_output_init(CompServer *server);
void comp_output_apply_config(CompServer *server);
bool comp_output_configure(CompServer *server, const char *output_name,
                           const char *mode_str, double scale, int transform, int vrr_policy);
void comp_output_update_vrr(CompServer *server);
/* comp_window.c */
void               comp_window_init(CompServer *server);
void               comp_window_relayout(CompServer *server);
void               comp_window_tick_frame(CompServer *server, double now_s);
struct CompWindow *comp_window_at(CompServer *server, double lx, double ly,
                                  struct wlr_surface **out_surface, double *out_sx, double *out_sy);
void               comp_window_focus(CompServer *server, struct CompWindow *w);
void               comp_window_request_activate(CompServer *server, struct CompWindow *w,
                                                const struct wlr_xdg_activation_token_v1 *token);
void               comp_window_focus_id(CompServer *server, CompWinId id);
bool               comp_window_focus_app(CompServer *server, const char *app_id);
bool               comp_window_close_id(CompServer *server, CompWinId id);
void               comp_window_get_popup_trees(const struct CompWindow *w,
                                               struct wlr_scene_tree **out_popup_tree,
                                               struct wlr_scene_node **out_xdg_node);
void               comp_window_resize_column(CompServer *server, double delta, CompDeltaMode mode);
void               comp_window_resize_height(CompServer *server, double delta, CompDeltaMode mode);
double             comp_window_scroll_factor_for_surface(CompServer *server, struct wlr_surface *surface);
bool               comp_window_focused_wants_vrr(const CompServer *server);
void               comp_window_reapply_rules(CompServer *server);
void               comp_window_on_dialog_state_changed(struct wlr_xdg_toplevel *toplevel);
size_t             comp_window_list_info(const CompServer *server, CompWindowInfo *out, size_t max_out);
struct wlr_surface *comp_window_get_surface(const struct CompWindow *w);
bool               comp_window_get_capture_box(const struct CompWindow *w, bool include_border,
                                               struct wlr_scene_node **out_node, struct wlr_box *out_logical_box);
void               comp_window_accept_capture_request(CompServer *server,
                                                      struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *req);
void               comp_window_destroy_capture_sources(CompServer *server);
struct CompWindow *comp_window_create_xwayland(CompServer *server, struct wlr_xwayland_surface *xsurf);
void               comp_window_map_xwayland(struct CompWindow *w);
void               comp_window_unmap_xwayland(struct CompWindow *w);
void               comp_window_update_xwayland_metadata(struct CompWindow *w);
void               comp_window_set_xwayland_fullscreen(struct CompWindow *w, bool fullscreen);
void               comp_window_set_xwayland_maximized(struct CompWindow *w, bool maximized);
void               comp_window_update_grab(CompServer *server);
void               comp_window_end_grab(CompServer *server);
bool               comp_window_handle_bind(CompServer *server, const CompBind *bind);
/* comp_text.c */
struct wlr_buffer *comp_pixel_buffer_create(uint8_t *owned_argb8888, int width, int height, size_t stride);
struct wlr_buffer *comp_text_render_label(const char *text, int logical_w, int logical_h, double scale,
                                          const float fg_rgba[4], const float bg_rgba[4],
                                          const float border_rgba[4], int border_px, int pad_x);
struct wlr_buffer *comp_text_render_badge(const char *text, double scale,
                                          const float fg_rgba[4], const float bg_rgba[4],
                                          const float border_rgba[4], int pad_x, int pad_y,
                                          int *out_logical_w, int *out_logical_h);
void               comp_scene_buffer_set_owned_buffer(struct wlr_scene_buffer *sbuf, struct wlr_buffer *buf);
/* comp_switcher.c */
void comp_switcher_init(CompServer *server);
bool comp_switcher_is_active(const CompServer *server);
void comp_switcher_step(CompServer *server, bool forward, CompSwitchScope scope, bool same_app, uint32_t held_mods);
void comp_switcher_confirm(CompServer *server);
void comp_switcher_cancel(CompServer *server);
void comp_switcher_on_window_unmap(CompServer *server, CompWinId id);
void comp_switcher_handle_modifiers(CompServer *server, uint32_t mods);
bool comp_switcher_handle_key(CompServer *server, uint32_t mods, uint32_t keysym);
bool comp_switcher_handle_click(CompServer *server, double lx, double ly);
/* comp_screenshot.c */
void comp_screenshot_init(CompServer *server);
bool comp_screenshot_is_active(const CompServer *server);
bool comp_screenshot_interactive(CompServer *server);
bool comp_screenshot_window(CompServer *server);
bool comp_screenshot_screen(CompServer *server);
void comp_screenshot_confirm(CompServer *server);
void comp_screenshot_cancel(CompServer *server);
bool comp_screenshot_handle_button(CompServer *server, uint32_t button, bool pressed, double lx, double ly);
bool comp_screenshot_handle_motion(CompServer *server, double lx, double ly);
bool comp_screenshot_handle_key(CompServer *server, uint32_t mods, uint32_t keysym);
/* comp_layer.c */
void comp_layer_init(CompServer *server);
void comp_layer_arrange(CompServer *server);
/* comp_lock_idle.c */
void comp_lock_idle_init(CompServer *server);
void comp_lock_idle_notify_activity(CompServer *server);
void comp_lock_idle_update_inhibitors(CompServer *server);
void comp_lock_idle_reconfigure_output(CompServer *server);
void comp_lock_idle_notify_output_presented(CompServer *server);
/* comp_protocols.c */
void comp_protocols_init(CompServer *server);
void comp_protocols_notify_focus_surface(CompServer *server, struct wlr_surface *surface);
void comp_protocols_check_pointer_constraint(CompServer *server, struct wlr_surface *surface);
bool comp_protocols_shortcuts_inhibited(CompServer *server);
void comp_protocols_break_shortcuts_inhibit(CompServer *server);
/* comp_xwayland.c */
void comp_xwayland_init(CompServer *server);
void comp_xwayland_finish(CompServer *server);
void comp_xwayland_update_workarea(CompServer *server);
/* comp_input.c */
void   comp_input_init(CompServer *server);
void   comp_input_apply_config(CompServer *server);
void   comp_input_map_devices_to_output(CompServer *server, struct wlr_output *output);
void   comp_input_add_keyboard_device(CompServer *server, struct wlr_input_device *dev);
void   comp_input_update_capabilities(CompServer *server);
void   comp_input_refresh_pointer_focus(CompServer *server, uint32_t time_msec);
bool   comp_input_switch_keyboard_layout(CompServer *server, int idx);
size_t comp_input_get_keyboard_layouts(const CompServer *server, const char **names_out,
                                       size_t max_names, uint32_t *current_idx_out);
/* comp_ipc.c */
bool        comp_ipc_init(CompServer *server);
void        comp_ipc_finish(CompServer *server);
void        comp_ipc_ensure_socket(CompServer *server);
const char *comp_ipc_socket_path(const CompServer *server);
char *comp_server_resolve_config_dir(void *talloc_ctx, const CompServerOptions *opts);
bool comp_server_reload_config(CompServer *server);
void comp_ipc_emit_workspaces(CompServer *server);
void comp_ipc_emit_windows(CompServer *server);
void comp_ipc_emit_focus(CompServer *server, CompWinId focused_id);
void comp_ipc_emit_keyboard_layouts(CompServer *server);
void comp_ipc_emit_outputs(CompServer *server);
void comp_ipc_emit_touch_bezel_swipe(CompServer *server, const char *edge, double x_ratio);
void comp_ipc_emit_screenshot_saved(CompServer *server, const char *path, bool copied);
void comp_ipc_fuzz_bytes(CompServer *server, const uint8_t *data, size_t len);
/* comp_spawn.c: fork/exec without a shell, child in its own session with WAYLAND_DISPLAY set.
 * Returns the pid or -1. */
int  comp_spawn(CompServer *server, const char *const *argv);
void comp_spawn_init(CompServer *server);

#endif /* KAJO_COMP_SERVER_H */
