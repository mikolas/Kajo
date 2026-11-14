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
