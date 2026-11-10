/* comp_server.c — server lifecycle and protocol globals (Phase 2a). */
#include "comp_server.h"

#include "comp_log.h"
#include "core/comp_binds.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <talloc.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>

static int
handle_terminate_signal(int sig, void *data)
{
    CompServer *s = data;
    wlr_log(WLR_INFO, "signal %d: shutting down", sig);
    comp_server_terminate(s, 0);
    return 0;
}

void
comp_server_terminate(CompServer *s, int exit_status)
{
    s->exit_status = exit_status;
    wl_display_terminate(s->display);
}

static int
server_destructor(CompServer *s)
{
    /* Teardown order: clients first (their surfaces reference our scene), then
     * the scene, then the objects the scene and outputs render with. Child
     * talloc contexts (outputs, input devices) are freed after this function
     * returns; by then their wlroots objects are gone and their destroy
     * handlers have already freed them, so nothing is left to unlink. */
    if (s->display)
        wl_display_destroy_clients(s->display);
    /* Unlink every listener on backend/cursor/seat signals before those objects go:
     * wlroots asserts that listener lists are empty when an object is destroyed. */
    talloc_free(s->listeners);
    s->listeners = NULL;
    if (s->scene)
        wlr_scene_node_destroy(&s->scene->tree.node);
    if (s->cursor_mgr)
        wlr_xcursor_manager_destroy(s->cursor_mgr);
    if (s->cursor)
        wlr_cursor_destroy(s->cursor);
    if (s->backend)
        wlr_backend_destroy(s->backend);       /* emits output/input destroy signals */
    if (s->allocator)
        wlr_allocator_destroy(s->allocator);
    if (s->renderer)
        wlr_renderer_destroy(s->renderer);
    for (size_t i = 0; i < sizeof(s->signal_sources) / sizeof(s->signal_sources[0]); i++) {
        if (s->signal_sources[i])
            wl_event_source_remove(s->signal_sources[i]);   /* the loop does not free live sources */
    }
    if (s->display)
        wl_display_destroy(s->display);         /* destroys globals, seat, layout */
    return 0;
}

static bool
create_globals(CompServer *s)
{
    if (!wlr_renderer_init_wl_shm(s->renderer, s->display))
        return false;
    if (wlr_renderer_get_drm_fd(s->renderer) >= 0 &&
        wlr_renderer_get_texture_formats(s->renderer, WLR_BUFFER_CAP_DMABUF) != NULL) {
        struct wlr_linux_dmabuf_v1 *dmabuf =
            wlr_linux_dmabuf_v1_create_with_renderer(s->display, 5, s->renderer);
        if (!dmabuf)
            return false;
        wlr_scene_set_linux_dmabuf_v1(s->scene, dmabuf);
    }
    return wlr_compositor_create(s->display, 6, s->renderer) &&
           wlr_subcompositor_create(s->display) &&
           wlr_data_device_manager_create(s->display) &&
           wlr_primary_selection_v1_device_manager_create(s->display) &&
           wlr_viewporter_create(s->display) &&
           wlr_single_pixel_buffer_manager_v1_create(s->display) &&
           wlr_fractional_scale_manager_v1_create(s->display, 1) &&
           wlr_presentation_create(s->display, s->backend, 2);
}

CompServer *
comp_server_create(const CompServerOptions *opts)
{
    CompServer *s = talloc_zero(NULL, CompServer);
    if (!s)
        return NULL;
    talloc_set_name_const(s, "CompServer");
    talloc_set_destructor(s, server_destructor);
    s->opts = *opts;
    if (!s->opts.mod_mask)
        s->opts.mod_mask = COMP_MOD_SUPER;
    wl_list_init(&s->outputs);
    wl_list_init(&s->keyboards);
    s->listeners = talloc_named_const(s, 0, "CompServer listeners");
    if (!s->listeners)
        goto fail;

    s->display = wl_display_create();
    if (!s->display)
        goto fail;
    s->loop = wl_display_get_event_loop(s->display);

    s->backend = wlr_backend_autocreate(s->loop, &s->session);
    if (!s->backend) {
        wlr_log(WLR_ERROR, "could not create a backend");
        goto fail;
    }
    /* v1 is SDR: automatic renderer selection (spec §3.1, Revision 7) */
    s->renderer = wlr_renderer_autocreate(s->backend);
    if (!s->renderer) {
        wlr_log(WLR_ERROR, "could not create a renderer");
        goto fail;
    }
    s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);
    if (!s->allocator) {
        wlr_log(WLR_ERROR, "could not create an allocator");
        goto fail;
    }
    s->scene = wlr_scene_create();
    s->output_layout = wlr_output_layout_create(s->display);
    if (!s->scene || !s->output_layout)
        goto fail;
    s->scene_layout = wlr_scene_attach_output_layout(s->scene, s->output_layout);
    if (!s->scene_layout)
        goto fail;
    if (!create_globals(s)) {
        wlr_log(WLR_ERROR, "could not create protocol globals");
        goto fail;
    }

    s->seat = wlr_seat_create(s->display, "seat0");
    s->cursor = wlr_cursor_create();
    s->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    if (!s->seat || !s->cursor || !s->cursor_mgr)
        goto fail;
    wlr_cursor_attach_output_layout(s->cursor, s->output_layout);

    comp_output_init(s);
    comp_input_init(s);
    comp_spawn_init(s);

    s->signal_sources[0] = wl_event_loop_add_signal(s->loop, SIGINT, handle_terminate_signal, s);
    s->signal_sources[1] = wl_event_loop_add_signal(s->loop, SIGTERM, handle_terminate_signal, s);
    if (!s->signal_sources[0] || !s->signal_sources[1])
        wlr_log(WLR_ERROR, "cannot watch SIGINT/SIGTERM");
    return s;

fail:
    talloc_free(s);
    return NULL;
}

bool
comp_server_start(CompServer *s)
{
    s->socket = wl_display_add_socket_auto(s->display);
    if (!s->socket) {
        wlr_log(WLR_ERROR, "could not add a Wayland socket");
        return false;
    }
    if (!wlr_backend_start(s->backend)) {
        wlr_log(WLR_ERROR, "could not start the backend");
        return false;
    }
    setenv("WAYLAND_DISPLAY", s->socket, 1);
    wlr_log(WLR_INFO, "kajo-comp running on WAYLAND_DISPLAY=%s", s->socket);
    if (s->opts.startup_argv && s->opts.startup_argv[0]) {
        s->startup_pid = comp_spawn(s, s->opts.startup_argv);
        if (s->startup_pid < 0) {
            wlr_log(WLR_ERROR, "could not start '%s'", s->opts.startup_argv[0]);
            s->startup_pid = 0;
            if (s->opts.exit_with_startup)
                return false;
        }
    }
    return true;
}

void
comp_server_run(CompServer *s)
{
    wl_display_run(s->display);
}

void
comp_server_destroy(CompServer *s)
{
    talloc_free(s);
}
