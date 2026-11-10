/* comp_server.h — kajo-comp server state (Phase 2a skeleton).
 *
 * Ownership (spec §3.8): CompServer is the root talloc context. Each output,
 * keyboard and pointer is a talloc child of the server; its listeners are
 * talloc children of the object (comp_listen), so freeing the object unlinks
 * everything it listened to. Objects free themselves from their wlroots
 * `destroy` handler.
 */
#ifndef KAJO_COMP_SERVER_H
#define KAJO_COMP_SERVER_H

#include "comp_listen.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wlr_allocator;
struct wlr_backend;
struct wlr_cursor;
struct wlr_input_device;
struct wlr_keyboard;
struct wlr_output;
struct wlr_output_layout;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_output_layout;
struct wlr_seat;
struct wlr_session;
struct wlr_xcursor_manager;

typedef struct {
    const char *const *startup_argv;   /* optional: run once the socket exists (no shell) */
    bool               exit_with_startup; /* terminate when that child exits (tests, greeter-like use) */
    uint32_t           mod_mask;       /* CompModMask meaning of "Mod"; Super by default */
} CompServerOptions;

typedef struct CompServer {
    CompServerOptions               opts;
    struct wl_display              *display;
    struct wl_event_loop           *loop;
    struct wlr_backend             *backend;
    struct wlr_session             *session;      /* NULL when nested/headless */
    struct wlr_renderer            *renderer;
    struct wlr_allocator           *allocator;
    struct wlr_scene               *scene;
    struct wlr_output_layout       *output_layout;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_seat                *seat;
    struct wlr_cursor              *cursor;
    struct wlr_xcursor_manager     *cursor_mgr;
    const char                     *socket;       /* owned by libwayland */
    void                           *listeners;    /* talloc ctx for listeners on server-owned
                                                     wlroots objects; freed first on teardown */

    struct wl_event_source         *signal_sources[3]; /* SIGINT, SIGTERM, SIGCHLD; removed on teardown */
    struct wl_list                  outputs;      /* CompOutput.link */
    struct wl_list                  keyboards;    /* CompKeyboard.link */
    unsigned                        n_pointers;

    int                             startup_pid;  /* 0 when none or already exited */
    int                             exit_status;  /* main() returns this */

    /* Optional hook for the next phases: called for every handled key press
     * before it is forwarded. Return true to consume. */
    bool                          (*key_hook)(struct CompServer *server, uint32_t mods,
                                              uint32_t keysym, bool pressed);
} CompServer;

/* Creates the display, backend, renderer and globals. Returns NULL on failure (logged). */
CompServer *comp_server_create(const CompServerOptions *opts);
/* Adds the socket, starts the backend and the startup command. */
bool        comp_server_start(CompServer *server);
void        comp_server_run(CompServer *server);
void        comp_server_destroy(CompServer *server);
void        comp_server_terminate(CompServer *server, int exit_status);

/* comp_output.c */
void comp_output_init(CompServer *server);
/* comp_input.c */
void comp_input_init(CompServer *server);
void comp_input_update_capabilities(CompServer *server);
/* comp_spawn.c: fork/exec without a shell, child in its own session with WAYLAND_DISPLAY set.
 * Returns the pid or -1. */
int  comp_spawn(CompServer *server, const char *const *argv);
void comp_spawn_init(CompServer *server);

#endif /* KAJO_COMP_SERVER_H */
