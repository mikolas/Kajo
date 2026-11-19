/* fx_stub.c — no-op effects facade built when wlroots lacks KFX extensions (CB2). */
#include "comp_fx.h"
#include "../comp_server.h"

#include <stddef.h>
#include <wlr/render/wlr_renderer.h>

bool
comp_fx_init(struct CompServer *server)
{
    (void)server;
    return true;
}

void
comp_fx_finish(struct CompServer *server)
{
    (void)server;
}

void
comp_fx_config_apply(struct CompServer *server, const struct CompConfig *cfg)
{
    (void)server;
    (void)cfg;
}

void
comp_fx_renderer_lost(struct CompServer *server)
{
    (void)server;
}

bool
comp_fx_output_prepare(struct CompOutput *output, double now_s)
{
    (void)output;
    (void)now_s;
    return false;
}

void
comp_fx_background_changed(struct CompOutput *output)
{
    (void)output;
}

struct wlr_color_transform *
comp_fx_output_get_color_transform(struct CompOutput *output)
{
    (void)output;
    return NULL;
}

CompFxSurface *
comp_fx_surface_create(struct CompServer *server,
                       struct wlr_scene_tree *parent_tree,
                       CompFxSurfaceKind kind)
{
    (void)server;
    (void)parent_tree;
    (void)kind;
    return NULL;
}

void
comp_fx_surface_sync(CompFxSurface *fx, const CompFxSurfaceState *state)
{
    (void)fx;
    (void)state;
}

void
comp_fx_surface_destroy(CompFxSurface *fx)
{
    (void)fx;
}

uint64_t
comp_fx_surface_get_glass_recomputes(const CompFxSurface *fx)
{
    (void)fx;
    return 0;
}

void
comp_fx_transition_start_open(struct CompServer *server,
                              struct wlr_scene_tree *win_tree,
                              struct wlr_scene_tree *parent_tree,
                              struct wlr_box outer_box)
{
    (void)server;
    (void)win_tree;
    (void)parent_tree;
    (void)outer_box;
}

void
comp_fx_transition_start_close(struct CompServer *server,
                               struct wlr_scene_tree *win_tree,
                               struct wlr_scene_tree *parent_tree,
                               struct wlr_box outer_box)
{
    (void)server;
    (void)win_tree;
    (void)parent_tree;
    (void)outer_box;
}

void
comp_fx_transition_start_resize(struct CompServer *server,
                                struct wlr_scene_tree *win_tree,
                                struct wlr_scene_tree *parent_tree,
                                struct wlr_box prev_box,
                                struct wlr_box curr_box)
{
    (void)server;
    (void)win_tree;
    (void)parent_tree;
    (void)prev_box;
    (void)curr_box;
}

void
comp_fx_transition_update_live(struct CompServer *server,
                               struct wlr_scene_tree *win_tree,
                               struct wlr_box live_box,
                               bool recapture_curr)
{
    (void)server;
    (void)win_tree;
    (void)live_box;
    (void)recapture_curr;
}

bool
comp_fx_tree_is_transitioning(const struct CompServer *server,
                              const struct wlr_scene_tree *win_tree)
{
    (void)server;
    (void)win_tree;
    return false;
}

bool
comp_fx_is_active(const struct CompServer *server)
{
    (void)server;
    return false;
}

struct wlr_render_pass *
comp_fx_screenshot_begin_pass(struct CompServer *server, struct wlr_buffer *buffer)
{
    if (!server || !server->renderer || !buffer)
        return NULL;
    return wlr_renderer_begin_buffer_pass(server->renderer, buffer, NULL);
}

bool
comp_fx_screenshot_render_node(struct CompServer *server,
                               struct wlr_render_pass *pass,
                               struct wlr_scene_node *node,
                               const struct wlr_box *dst_box,
                               const struct wlr_box *clip_box,
                               int origin_lx, int origin_ly,
                               double scale, int pass_w, int pass_h)
{
    (void)server;
    (void)pass;
    (void)node;
    (void)dst_box;
    (void)clip_box;
    (void)origin_lx;
    (void)origin_ly;
    (void)scale;
    (void)pass_w;
    (void)pass_h;
    return false;
}
