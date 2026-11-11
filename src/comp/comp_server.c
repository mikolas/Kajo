/* comp_server.c — server lifecycle, scene hierarchy and protocol globals (Phase 2a + 2b). */
#include "comp_server.h"

#include "comp_log.h"

#include <libgen.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <talloc.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>

double
comp_now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

CompWinId
comp_win_alloc_id(CompServer *s, struct CompWindow *w)
{
    COMP_WARN_RETURN(s && w, 0);
    if (!s->win_slots) {
        s->win_slots_cap = 64;
        s->win_slots = talloc_zero_array(s, struct CompWindow *, (unsigned)s->win_slots_cap);
        s->win_gens  = talloc_zero_array(s, uint32_t, (unsigned)s->win_slots_cap);
        COMP_WARN_RETURN(s->win_slots && s->win_gens, 0);
    }
    size_t slot = 0;
    for (size_t i = 1; i < s->win_slots_cap; i++) {
        if (!s->win_slots[i]) {
            slot = i;
            break;
        }
    }
    if (slot == 0) {
        size_t old_cap = s->win_slots_cap;
        size_t new_cap = old_cap * 2;
        struct CompWindow **ns = talloc_realloc(s, s->win_slots, struct CompWindow *, (unsigned)new_cap);
        uint32_t *ng = talloc_realloc(s, s->win_gens, uint32_t, (unsigned)new_cap);
        COMP_WARN_RETURN(ns && ng, 0);
        memset(ns + old_cap, 0, (new_cap - old_cap) * sizeof(*ns));
        memset(ng + old_cap, 0, (new_cap - old_cap) * sizeof(*ng));
        s->win_slots     = ns;
        s->win_gens      = ng;
        s->win_slots_cap = new_cap;
        slot             = old_cap;
    }
    uint32_t gen = ++s->win_gens[slot];
    if (gen == 0)
        gen = ++s->win_gens[slot];
    s->win_slots[slot] = w;
    return ((uint64_t)gen << 32) | (uint32_t)slot;
}

void
comp_win_free_id(CompServer *s, CompWinId id)
{
    if (!s || id == 0 || !s->win_slots)
        return;
    uint32_t slot = (uint32_t)(id & 0xffffffffu);
    uint32_t gen  = (uint32_t)(id >> 32);
    if (slot > 0 && slot < s->win_slots_cap && s->win_gens[slot] == gen)
        s->win_slots[slot] = NULL;
}

struct CompWindow *
comp_win_from_id(const CompServer *s, CompWinId id)
{
    if (!s || id == 0 || !s->win_slots)
        return NULL;
    uint32_t slot = (uint32_t)(id & 0xffffffffu);
    uint32_t gen  = (uint32_t)(id >> 32);
    if (slot == 0 || slot >= s->win_slots_cap || s->win_gens[slot] != gen)
        return NULL;
    return s->win_slots[slot];
}

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
     * talloc contexts (outputs, windows, input devices) are freed after this
     * function returns; by then their wlroots objects are gone and their
     * destroy handlers have already freed them, so nothing is left to unlink. */
    comp_ipc_finish(s);
    comp_xwayland_finish(s);
    talloc_free(s->switcher);
    s->switcher = NULL;
    talloc_free(s->screenshot);
    s->screenshot = NULL;
    if (s->display)
        wl_display_destroy_clients(s->display);
    /* Unlink every listener on backend/cursor/seat/xdg_shell signals before those
     * objects go: wlroots asserts that listener lists are empty on destroy. */
    talloc_free(s->listeners);
    s->listeners = NULL;
    if (s->scene) {
        wlr_scene_node_destroy(&s->scene->tree.node);
        s->scene = NULL;
    }
    if (s->cursor_mgr)
        wlr_xcursor_manager_destroy(s->cursor_mgr);
    if (s->cursor)
        wlr_cursor_destroy(s->cursor);
    for (size_t i = 0; i < sizeof(s->signal_sources) / sizeof(s->signal_sources[0]); i++) {
        if (s->signal_sources[i])
            wl_event_source_remove(s->signal_sources[i]);
    }
    if (s->backend)
        wlr_backend_destroy(s->backend);       /* emits output/input destroy signals */
    if (s->display)
        wl_display_destroy(s->display);         /* destroys globals, compositor textures, seat, layout */
    if (s->allocator)
        wlr_allocator_destroy(s->allocator);
    if (s->renderer)
        wlr_renderer_destroy(s->renderer);
    comp_binds_free(s->binds);
    comp_rules_free(s->rules);
    comp_config_free(s->config);
    comp_motion_free(s->motion);
    comp_layout_free(s->layout);
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
    s->compositor = wlr_compositor_create(s->display, 6, s->renderer);
    return s->compositor &&
           wlr_subcompositor_create(s->display) &&
           wlr_data_device_manager_create(s->display) &&
           wlr_primary_selection_v1_device_manager_create(s->display) &&
           wlr_viewporter_create(s->display) &&
           wlr_single_pixel_buffer_manager_v1_create(s->display) &&
           wlr_fractional_scale_manager_v1_create(s->display, 1) &&
           wlr_presentation_create(s->display, s->backend, 2);
}

static bool
create_scene_layers(CompServer *s)
{
    s->tree_background = wlr_scene_tree_create(&s->scene->tree);
    s->tree_bottom     = wlr_scene_tree_create(&s->scene->tree);
    s->tree_tiled      = wlr_scene_tree_create(&s->scene->tree);
    s->tree_floating   = wlr_scene_tree_create(&s->scene->tree);
    s->tree_top        = wlr_scene_tree_create(&s->scene->tree);
    s->tree_fullscreen = wlr_scene_tree_create(&s->scene->tree);
    s->tree_overlay    = wlr_scene_tree_create(&s->scene->tree);
    s->tree_lock       = wlr_scene_tree_create(&s->scene->tree);
    return s->tree_background && s->tree_bottom && s->tree_tiled &&
           s->tree_floating && s->tree_top && s->tree_fullscreen &&
           s->tree_overlay && s->tree_lock;
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
    wl_list_init(&s->windows);
    s->listeners = talloc_named_const(s, 0, "CompServer listeners");
    if (!s->listeners)
        goto fail;

    CompLayoutConfig lcfg;
    comp_layout_config_defaults(&lcfg);
    s->layout = comp_layout_new(&lcfg);
    CompMotionConfig mcfg;
    comp_motion_config_defaults(&mcfg);
    s->motion = comp_motion_new(&mcfg);
    if (!s->layout || !s->motion)
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
    /* Spec §1.1 & §6.5.3: force the Vulkan renderer in normal/session mode unless
     * WLR_RENDERER is explicitly set (e.g. headless CI) or --greeter-mode is active. */
    bool forced_vk = false;
    if (!s->opts.greeter_mode && !getenv("WLR_RENDERER")) {
        setenv("WLR_RENDERER", "vulkan", 1);
        forced_vk = true;
    }
    s->renderer = wlr_renderer_autocreate(s->backend);
    if (forced_vk)
        unsetenv("WLR_RENDERER");
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
    if (!s->scene || !s->output_layout || !create_scene_layers(s))
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
    comp_window_init(s);
    comp_layer_init(s);
    comp_lock_idle_init(s);
    comp_protocols_init(s);
    comp_xwayland_init(s);
    comp_input_init(s);
    comp_spawn_init(s);
    comp_switcher_init(s);
    comp_screenshot_init(s);
    comp_ipc_init(s);
    comp_server_reload_config(s);

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
    } else if (s->opts.session_mode && !s->opts.greeter_mode && s->config) {
        for (size_t i = 0; i < s->config->n_autostart; i++) {
            char *const *argv = s->config->autostart[i];
            if (argv && argv[0])
                comp_spawn(s, (const char *const *)argv);
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

char *
comp_server_resolve_config_dir(void *talloc_ctx, const CompServerOptions *opts)
{
    if (opts && opts->config_path && *opts->config_path) {
        struct stat st;
        if (stat(opts->config_path, &st) == 0 && S_ISDIR(st.st_mode))
            return talloc_strdup(talloc_ctx, opts->config_path);
        char *tmp = strdup(opts->config_path);
        if (!tmp)
            return NULL;
        char *dir = dirname(tmp);
        char *res = talloc_strdup(talloc_ctx, dir);
        free(tmp);
        return res;
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return talloc_asprintf(talloc_ctx, "%s/kajo", xdg);
    const char *home = getenv("HOME");
    if (home && *home)
        return talloc_asprintf(talloc_ctx, "%s/.config/kajo", home);
    return talloc_strdup(talloc_ctx, "/etc/kajo");
}

static const char *
resolve_defaults_dir(void)
{
    const char *env = getenv("KAJO_COMP_DEFAULTS_DIR");
    if (env && *env)
        return env;
    struct stat st;
    if (stat("data/defaults/comp.toml", &st) == 0 && S_ISREG(st.st_mode))
        return "data/defaults";
    if (stat("/usr/local/share/kajo/defaults/comp.toml", &st) == 0 && S_ISREG(st.st_mode))
        return "/usr/local/share/kajo/defaults";
    if (stat("/usr/share/kajo/defaults/comp.toml", &st) == 0 && S_ISREG(st.st_mode))
        return "/usr/share/kajo/defaults";
    return NULL;
}

bool
comp_server_reload_config(CompServer *s)
{
    if (!s)
        return false;

    CompDiagList diag = {0};
    CompConfig *cfg = NULL;
    const char *defaults_dir = resolve_defaults_dir();

    void *tmp = talloc_new(s);
    char *dir = comp_server_resolve_config_dir(tmp, &s->opts);

    if (s->opts.config_path && *s->opts.config_path) {
        struct stat st;
        bool is_dir = (stat(s->opts.config_path, &st) == 0 && S_ISDIR(st.st_mode));
        if (is_dir)
            cfg = comp_config_load_dir(s->opts.config_path, defaults_dir, NULL, s->opts.mod_mask, &diag);
        else
            cfg = comp_config_load_dir(dir, defaults_dir, s->opts.config_path, s->opts.mod_mask, &diag);
    } else {
        cfg = comp_config_load_dir(dir, defaults_dir, NULL, s->opts.mod_mask, &diag);
    }
    talloc_free(tmp);

    for (size_t i = 0; i < diag.n; i++)
        wlr_log(WLR_ERROR, "config: %s", diag.msgs[i] ? diag.msgs[i] : "<unknown>");
    comp_diag_clear(&diag);

    if (!cfg) {
        wlr_log(WLR_ERROR, "configuration reload failed; retaining active configuration");
        return false;
    }

    for (size_t i = 0; i < cfg->n_env; i++) {
        if (cfg->env[i].key && cfg->env[i].value)
            setenv(cfg->env[i].key, cfg->env[i].value, 1);
    }

    if (s->layout)
        comp_layout_set_config(s->layout, &cfg->layout);
    if (s->motion)
        comp_motion_set_config(s->motion, &cfg->motion);

    if (!s->opts.greeter_mode) {
        comp_binds_free(s->binds);
        s->binds = cfg->binds;
        cfg->binds = NULL;

        comp_rules_free(s->rules);
        s->rules = cfg->rules;
        cfg->rules = NULL;
    }

    comp_config_free(s->config);
    s->config = cfg;

    if (!cfg->xwayland_enabled && s->xwayland)
        comp_xwayland_finish(s);
    else if (cfg->xwayland_enabled && !s->xwayland)
        comp_xwayland_init(s);
    comp_xwayland_update_workarea(s);

    comp_input_apply_config(s);
    comp_output_apply_config(s);
    comp_window_reapply_rules(s);
    comp_layer_arrange(s);
    comp_window_relayout(s);
    return true;
}
