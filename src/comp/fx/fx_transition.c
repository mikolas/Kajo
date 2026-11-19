/* fx_transition.c — Window open / close / resize snapshot transitions (M5, H5). */
#include "fx_internal.h"

#if defined(WLR_HAS_KFX) && WLR_HAS_KFX

#include "transition_resize_frag.h"
#include "transition_single_frag.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/render/color.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

#define COMP_FX_TRANS_MAGIC 0x4B465854u /* "KFXT" */

typedef struct {
    uint32_t                 magic;
    struct wl_list           link;          /* CompFxSystem.transitions */
    CompFxSystem            *sys;
    CompFxTransitionKind     kind;
    uint32_t                 preset;
    CompFxTransitionCurve    curve;
    double                   start_s;
    double                   duration_s;
    double                   progress;
    double                   eased_progress;

    struct wlr_box           prev_box;
    struct wlr_box           curr_box;
    float                    capture_scale;

    struct wlr_kfx_target   *target0;       /* main snapshot (or prev for resize) */
    struct wlr_kfx_target   *target1;       /* curr snapshot (resize only) */
    int                      t0_px_w;
    int                      t0_px_h;
    int                      t1_px_w;
    int                      t1_px_h;
    bool                     follow_live_box;

    struct wlr_scene_tree   *src_tree;      /* live window tree (NULL for close) */
    struct wl_listener       src_tree_destroy;

    struct wlr_scene_buffer *anchor;
    struct wl_listener       anchor_destroy;
    struct wlr_kfx_node_hook hook;
} CompFxTransition;

static double
fx_now_monotonic_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static bool
is_transition_anchor_node(const CompFxSystem *sys, const struct wlr_scene_node *node)
{
    if (!sys || !sys->blank_buf || !node ||
        node->type != WLR_SCENE_NODE_BUFFER || !node->data)
        return false;
    struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node((struct wlr_scene_node *)node);
    if (!sbuf || sbuf->buffer != &sys->blank_buf->base)
        return false;
    const CompFxTransition *tr = node->data;
    return tr->magic == COMP_FX_TRANS_MAGIC && tr->anchor == sbuf;
}

static bool
clip_box_to_pass(int x, int y, int w, int h, int pass_w, int pass_h, struct wlr_box *out)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > pass_w ? pass_w : (x + w);
    int y1 = (y + h) > pass_h ? pass_h : (y + h);
    if (x1 <= x0 || y1 <= y0)
        return false;
    out->x      = x0;
    out->y      = y0;
    out->width  = x1 - x0;
    out->height = y1 - y0;
    return true;
}

static void
snapshot_walk_node(CompFxSystem *sys,
                   struct wlr_render_pass *pass,
                   struct wlr_scene_node *node,
                   int rel_x, int rel_y,
                   double scale, int pass_w, int pass_h,
                   bool ignore_disabled_subsurfaces)
{
    if (!node)
        return;
    if (!node->enabled) {
        if (!ignore_disabled_subsurfaces || node->type == WLR_SCENE_NODE_RECT)
            return;
    }

    int lx = rel_x + node->x;
    int ly = rel_y + node->y;

    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link) {
            snapshot_walk_node(sys, pass, child, lx, ly, scale, pass_w, pass_h,
                               ignore_disabled_subsurfaces);
        }
        return;
    }

    if (fx_slot_from_scene_node(sys, node) || is_transition_anchor_node(sys, node))
        return;

    if (node->type == WLR_SCENE_NODE_RECT) {
        struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
        if (rect->width <= 0 || rect->height <= 0 || rect->color[3] <= 0.0f)
            return;
        int rx = (int)lround((double)lx * scale);
        int ry = (int)lround((double)ly * scale);
        int rw = (int)lround((double)(lx + rect->width) * scale) - rx;
        int rh = (int)lround((double)(ly + rect->height) * scale) - ry;
        struct wlr_box cbox;
        if (!clip_box_to_pass(rx, ry, rw, rh, pass_w, pass_h, &cbox))
            return;
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
            .box   = cbox,
            .color = {
                .r = rect->color[0],
                .g = rect->color[1],
                .b = rect->color[2],
                .a = rect->color[3],
            },
            .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
        });
        return;
    }

    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
        if (!sbuf->buffer || sbuf->opacity <= 0.0f)
            return;
        int log_w = sbuf->dst_width > 0 ? sbuf->dst_width : sbuf->buffer->width;
        int log_h = sbuf->dst_height > 0 ? sbuf->dst_height : sbuf->buffer->height;
        if (log_w <= 0 || log_h <= 0)
            return;
        int bx = (int)lround((double)lx * scale);
        int by = (int)lround((double)ly * scale);
        int bw = (int)lround((double)(lx + log_w) * scale) - bx;
        int bh = (int)lround((double)(ly + log_h) * scale) - by;
        struct wlr_box cbox;
        if (!clip_box_to_pass(bx, by, bw, bh, pass_w, pass_h, &cbox))
            return;

        struct wlr_client_buffer *cbuf = wlr_client_buffer_get(sbuf->buffer);
        struct wlr_single_pixel_buffer_v1 *spb =
            wlr_single_pixel_buffer_v1_try_from_buffer(
                (cbuf && cbuf->source) ? cbuf->source : sbuf->buffer);
        if (spb) {
            float op = sbuf->opacity;
            float a = ((float)spb->a / (float)UINT32_MAX) * op;
            wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                .box   = cbox,
                .color = {
                    .r = ((float)spb->r / (float)UINT32_MAX) * op,
                    .g = ((float)spb->g / (float)UINT32_MAX) * op,
                    .b = ((float)spb->b / (float)UINT32_MAX) * op,
                    .a = a,
                },
                .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
            });
            return;
        }

        struct wlr_texture *tex = (cbuf && cbuf->texture)
                                      ? cbuf->texture
                                      : wlr_texture_from_buffer(sys->server->renderer, sbuf->buffer);
        if (!tex)
            return;

        struct wlr_fbox src_box = sbuf->src_box;
        if (cbox.x != bx || cbox.y != by || cbox.width != bw || cbox.height != bh) {
            if (wlr_fbox_empty(&src_box)) {
                int tw = (int)tex->width;
                int th = (int)tex->height;
                wlr_output_transform_coords(sbuf->transform, &tw, &th);
                src_box = (struct wlr_fbox){
                    .x      = 0.0,
                    .y      = 0.0,
                    .width  = (double)(tw > 0 ? tw : sbuf->buffer->width),
                    .height = (double)(th > 0 ? th : sbuf->buffer->height),
                };
            }
            double sx_scale = src_box.width / (double)bw;
            double sy_scale = src_box.height / (double)bh;
            src_box.x      += (double)(cbox.x - bx) * sx_scale;
            src_box.y      += (double)(cbox.y - by) * sy_scale;
            src_box.width   = (double)cbox.width * sx_scale;
            src_box.height  = (double)cbox.height * sy_scale;
        }

        struct wlr_color_primaries primaries_val;
        const struct wlr_color_primaries *primaries = NULL;
        if (sbuf->primaries != 0) {
            wlr_color_primaries_from_named(&primaries_val, sbuf->primaries);
            primaries = &primaries_val;
        }
        float alpha = sbuf->opacity;
        wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
            .texture           = tex,
            .src_box           = src_box,
            .dst_box           = cbox,
            .transform         = wlr_output_transform_invert(sbuf->transform),
            .alpha             = &alpha,
            .filter_mode       = sbuf->filter_mode,
            .blend_mode        = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
            .transfer_function = sbuf->transfer_function,
            .primaries         = primaries,
            .color_encoding    = sbuf->color_encoding,
            .color_range       = sbuf->color_range,
        });
        if (!cbuf || !cbuf->texture)
            wlr_texture_destroy(tex);
    }
}

static struct wlr_kfx_target *
capture_tree_snapshot(CompFxSystem *sys,
                      struct wlr_scene_tree *tree,
                      int log_w, int log_h,
                      float scale,
                      bool ignore_disabled_subsurfaces,
                      int *out_px_w, int *out_px_h)
{
    if (!sys || !sys->server || !sys->server->renderer || !tree ||
        log_w <= 0 || log_h <= 0)
        return NULL;

    float s = (scale > 0.0f) ? scale : 1.0f;
    int px_w = (int)ceil((double)log_w * (double)s);
    int px_h = (int)ceil((double)log_h * (double)s);
    if (px_w < 1) px_w = 1;
    if (px_h < 1) px_h = 1;
    if (px_w > 4096) px_w = 4096;
    if (px_h > 4096) px_h = 4096;

    struct wlr_kfx_target *target = wlr_kfx_target_acquire(sys->server->renderer, px_w, px_h);
    if (!target)
        return NULL;

    struct wlr_render_pass *pass = wlr_kfx_target_begin_pass(target);
    if (!pass) {
        wlr_kfx_target_release(target);
        return NULL;
    }

    bool was_enabled = tree->node.enabled;
    tree->node.enabled = true;
    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link) {
        snapshot_walk_node(sys, pass, child, 0, 0, (double)s, px_w, px_h,
                           ignore_disabled_subsurfaces);
    }
    tree->node.enabled = was_enabled;

    if (!wlr_render_pass_submit(pass)) {
        wlr_kfx_target_release(target);
        return NULL;
    }
    if (out_px_w) *out_px_w = px_w;
    if (out_px_h) *out_px_h = px_h;
    return target;
}

static void
send_frame_done_walk(struct wlr_scene_node *node, const struct timespec *now)
{
    if (!node)
        return;
    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link) {
            send_frame_done_walk(child, now);
        }
        return;
    }
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sbuf);
        if (ss && ss->surface)
            wlr_surface_send_frame_done(ss->surface, now);
    }
}

static float
max_output_scale(const CompServer *s)
{
    float best = 1.0f;
    if (!s)
        return best;
    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr_output && o->wlr_output->scale > best)
            best = o->wlr_output->scale;
    }
    return best;
}

static void
schedule_all_outputs(CompServer *s)
{
    if (!s)
        return;
    CompOutput *o;
    wl_list_for_each(o, &s->outputs, link) {
        if (o->wlr_output && o->wlr_output->enabled)
            wlr_output_schedule_frame(o->wlr_output);
    }
}

static void
transition_destroy(CompFxTransition *tr, bool restore_src_tree)
{
    if (!tr)
        return;

    if (tr->src_tree) {
        wl_list_remove(&tr->src_tree_destroy.link);
        wl_list_init(&tr->src_tree_destroy.link);
        if (restore_src_tree)
            wlr_scene_node_set_enabled(&tr->src_tree->node, true);
        tr->src_tree = NULL;
    }

    if (tr->anchor) {
        wlr_kfx_node_hook_finish(&tr->hook);
        wl_list_remove(&tr->anchor_destroy.link);
        wl_list_init(&tr->anchor_destroy.link);
        tr->anchor->node.data = NULL;
        struct wlr_scene_buffer *a = tr->anchor;
        tr->anchor = NULL;
        wlr_scene_node_destroy(&a->node);
    }

    if (tr->target0) {
        wlr_kfx_target_release(tr->target0);
        tr->target0 = NULL;
    }
    if (tr->target1) {
        wlr_kfx_target_release(tr->target1);
        tr->target1 = NULL;
    }

    wl_list_remove(&tr->link);
    tr->magic = 0;
    free(tr);
}

static void
handle_transition_anchor_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    CompFxTransition *tr = wl_container_of(listener, tr, anchor_destroy);
    wlr_kfx_node_hook_finish(&tr->hook);
    wl_list_remove(&tr->anchor_destroy.link);
    wl_list_init(&tr->anchor_destroy.link);
    tr->anchor = NULL;
    transition_destroy(tr, true);
}

static void
handle_transition_src_tree_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    CompFxTransition *tr = wl_container_of(listener, tr, src_tree_destroy);
    wl_list_remove(&tr->src_tree_destroy.link);
    wl_list_init(&tr->src_tree_destroy.link);
    tr->src_tree = NULL;
    if (tr->kind != COMP_FX_TRANS_CLOSE)
        transition_destroy(tr, false);
}

static void
transition_draw_cb(struct wlr_kfx_node_hook *hook,
                   enum wlr_kfx_hook_stage stage,
                   const struct wlr_kfx_render_ctx *ctx)
{
    (void)stage;
    if (!hook || !ctx || !ctx->render_pass)
        return;
    CompFxTransition *tr = wl_container_of(hook, tr, hook);
    if (!tr || tr->magic != COMP_FX_TRANS_MAGIC || !tr->sys)
        return;

    CompFxSystem *sys = tr->sys;
    struct wlr_kfx_shader *shader = NULL;
    struct wlr_kfx_draw_options opts = {0};

    if (tr->kind == COMP_FX_TRANS_RESIZE) {
        if (!sys->transition_resize_shader || !tr->target0 || !tr->target1)
            return;
        shader = sys->transition_resize_shader;
        opts.inputs[0] = tr->target0;
        opts.inputs[1] = tr->target1;
    } else {
        if (!sys->transition_single_shader || !tr->target0)
            return;
        shader = sys->transition_single_shader;
        opts.inputs[0] = tr->target0;
    }

    CompFxRect dst = {
        .x      = ctx->dst_box.x,
        .y      = ctx->dst_box.y,
        .width  = ctx->dst_box.width,
        .height = ctx->dst_box.height,
    };
    float scale = (ctx->output && ctx->output->output && ctx->output->output->scale > 0.0f)
                      ? ctx->output->output->scale
                      : 1.0f;

    float uvs0_x = 1.0f, uvs0_y = 1.0f;
    float uvs1_x = 1.0f, uvs1_y = 1.0f;
    if (tr->target0 && tr->t0_px_w > 0 && tr->t0_px_h > 0) {
        int tw = 0, th = 0;
        wlr_kfx_target_get_size(tr->target0, &tw, &th);
        if (tw > 0 && th > 0) {
            uvs0_x = (float)tr->t0_px_w / (float)tw;
            uvs0_y = (float)tr->t0_px_h / (float)th;
        }
    }
    if (tr->target1 && tr->t1_px_w > 0 && tr->t1_px_h > 0) {
        int tw = 0, th = 0;
        wlr_kfx_target_get_size(tr->target1, &tw, &th);
        if (tw > 0 && th > 0) {
            uvs1_x = (float)tr->t1_px_w / (float)tw;
            uvs1_y = (float)tr->t1_px_h / (float)th;
        }
    }

    CompFxTransitionParams params;
    comp_fx_transition_pack(dst, tr->kind, tr->preset,
                            tr->progress, tr->eased_progress,
                            (float)tr->prev_box.width, (float)tr->prev_box.height,
                            (float)tr->curr_box.width, (float)tr->curr_box.height,
                            uvs0_x, uvs0_y, uvs1_x, uvs1_y,
                            scale, &params);
    if (ctx->output && ctx->output->output)
        params.common.hdr_headroom = wlr_kfx_output_get_hdr_headroom(ctx->output->output);

    opts.shader     = shader;
    opts.dst_box    = ctx->dst_box;
    opts.clip       = ctx->clip;
    opts.params     = &params;
    opts.params_len = sizeof(params);
    opts.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED;
    wlr_kfx_render_pass_add_draw(ctx->render_pass, &opts);
}

static const struct wlr_kfx_node_hook_impl k_trans_hook_impl = {
    .render = transition_draw_cb,
};

bool
fx_transition_init_shaders(CompFxSystem *sys)
{
    if (!sys || !sys->server || !sys->server->renderer)
        return false;

    if (!sys->transition_single_shader) {
        struct wlr_kfx_shader_layout layout = {
            .n_inputs    = 1,
            .params_size = sizeof(CompFxTransitionParams),
        };
        sys->transition_single_shader = wlr_kfx_shader_create(
            sys->server->renderer,
            transition_single_frag_spv, sizeof(transition_single_frag_spv), &layout);
        if (!sys->transition_single_shader) {
            wlr_log(WLR_ERROR, "kfx: failed to create transition_single fragment shader");
            return false;
        }
    }

    if (!sys->transition_resize_shader) {
        struct wlr_kfx_shader_layout layout = {
            .n_inputs    = 2,
            .params_size = sizeof(CompFxTransitionParams),
        };
        sys->transition_resize_shader = wlr_kfx_shader_create(
            sys->server->renderer,
            transition_resize_frag_spv, sizeof(transition_resize_frag_spv), &layout);
        if (!sys->transition_resize_shader) {
            wlr_log(WLR_ERROR, "kfx: failed to create transition_resize fragment shader");
            return false;
        }
    }
    return true;
}

void
fx_transition_destroy_shaders(CompFxSystem *sys)
{
    if (!sys)
        return;
    if (sys->transition_single_shader) {
        wlr_kfx_shader_destroy(sys->transition_single_shader);
        sys->transition_single_shader = NULL;
    }
    if (sys->transition_resize_shader) {
        wlr_kfx_shader_destroy(sys->transition_resize_shader);
        sys->transition_resize_shader = NULL;
    }
}

void
fx_transition_cancel_all(CompFxSystem *sys)
{
    if (!sys)
        return;
    CompFxTransition *tr, *tmp;
    wl_list_for_each_safe(tr, tmp, &sys->transitions, link) {
        transition_destroy(tr, true);
    }
}

static void
cancel_transitions_for_tree(CompFxSystem *sys, const struct wlr_scene_tree *tree)
{
    if (!sys || !tree)
        return;
    CompFxTransition *tr, *tmp;
    wl_list_for_each_safe(tr, tmp, &sys->transitions, link) {
        if (tr->src_tree == tree)
            transition_destroy(tr, true);
    }
}

static CompFxTransition *
create_transition(CompFxSystem *sys,
                  struct wlr_scene_tree *parent_tree,
                  struct wlr_scene_tree *src_tree,
                  CompFxTransitionKind kind,
                  uint32_t preset,
                  CompFxTransitionCurve curve,
                  int32_t duration_ms,
                  struct wlr_box prev_box,
                  struct wlr_box curr_box,
                  struct wlr_kfx_target *target0,
                  struct wlr_kfx_target *target1)
{
    if (!sys || !sys->blank_buf || !parent_tree || !target0)
        return NULL;

    if (sys->server && sys->server->config && !sys->server->config->motion.enabled)
        return NULL;

    double slowdown = (sys->server && sys->server->config &&
                       sys->server->config->motion.slowdown > 0.0)
                          ? sys->server->config->motion.slowdown
                          : 1.0;
    double dur_s = ((double)duration_ms * slowdown) / 1000.0;
    if (!(dur_s > 0.005))
        return NULL;

    CompFxTransition *tr = calloc(1, sizeof(*tr));
    if (!tr)
        return NULL;

    tr->magic          = COMP_FX_TRANS_MAGIC;
    tr->sys            = sys;
    tr->kind           = kind;
    tr->preset         = preset;
    tr->curve          = curve;
    tr->start_s        = fx_now_monotonic_s();
    tr->duration_s     = dur_s;
    tr->progress       = 0.0;
    tr->eased_progress = 0.0;
    tr->prev_box       = prev_box;
    tr->curr_box       = curr_box;
    tr->capture_scale  = max_output_scale(sys->server);
    tr->target0        = target0;
    tr->target1        = target1;
    tr->src_tree       = src_tree;

    wl_list_init(&tr->src_tree_destroy.link);
    wl_list_init(&tr->anchor_destroy.link);

    tr->anchor = wlr_scene_buffer_create(parent_tree, &sys->blank_buf->base);
    if (!tr->anchor) {
        free(tr);
        return NULL;
    }
    tr->anchor->node.data = tr;
    wlr_scene_node_set_position(&tr->anchor->node, prev_box.x, prev_box.y);
    wlr_scene_buffer_set_dest_size(tr->anchor, prev_box.width, prev_box.height);

    tr->anchor_destroy.notify = handle_transition_anchor_destroy;
    wl_signal_add(&tr->anchor->node.events.destroy, &tr->anchor_destroy);

    if (src_tree) {
        tr->src_tree_destroy.notify = handle_transition_src_tree_destroy;
        wl_signal_add(&src_tree->node.events.destroy, &tr->src_tree_destroy);
        wlr_scene_node_place_above(&tr->anchor->node, &src_tree->node);
        wlr_scene_node_set_enabled(&src_tree->node, false);
    }

    if (!wlr_kfx_node_hook_init(&tr->hook, &tr->anchor->node,
                                WLR_KFX_INSTEAD, &k_trans_hook_impl)) {
        tr->target0 = NULL;
        tr->target1 = NULL;
        wl_list_init(&tr->link);
        transition_destroy(tr, true);
        return NULL;
    }
    wl_list_insert(&sys->transitions, &tr->link);
    schedule_all_outputs(sys->server);
    return tr;
}

void
comp_fx_transition_start_open(struct CompServer *server,
                              struct wlr_scene_tree *win_tree,
                              struct wlr_scene_tree *parent_tree,
                              struct wlr_box outer_box)
{
    if (!server || !server->fx || !win_tree || !parent_tree)
        return;
    CompFxSystem *sys = server->fx;
    if (!sys->supported || !sys->cfg.enabled || !sys->cfg.transitions.enabled)
        return;
    if (!sys->transition_single_shader || sys->cfg.transitions.open_duration_ms <= 0)
        return;
    if (outer_box.width <= 0 || outer_box.height <= 0)
        return;

    cancel_transitions_for_tree(sys, win_tree);

    float scale = max_output_scale(server);
    int px_w = 0, px_h = 0;
    struct wlr_kfx_target *t0 = capture_tree_snapshot(
        sys, win_tree, outer_box.width, outer_box.height, scale, false, &px_w, &px_h);
    if (!t0)
        return;

    CompFxTransition *tr = create_transition(sys, parent_tree, win_tree, COMP_FX_TRANS_OPEN,
                                             (uint32_t)sys->cfg.transitions.open_preset,
                                             sys->cfg.transitions.open_curve,
                                             sys->cfg.transitions.open_duration_ms,
                                             outer_box, outer_box, t0, NULL);
    if (!tr) {
        wlr_kfx_target_release(t0);
        return;
    }
    tr->t0_px_w = px_w;
    tr->t0_px_h = px_h;
    tr->follow_live_box = true;
}

void
comp_fx_transition_start_close(struct CompServer *server,
                               struct wlr_scene_tree *win_tree,
                               struct wlr_scene_tree *parent_tree,
                               struct wlr_box outer_box)
{
    if (!server || !server->fx || !win_tree || !parent_tree)
        return;
    CompFxSystem *sys = server->fx;
    if (!sys->supported || !sys->cfg.enabled || !sys->cfg.transitions.enabled)
        return;
    if (!sys->transition_single_shader || sys->cfg.transitions.close_duration_ms <= 0)
        return;
    if (outer_box.width <= 0 || outer_box.height <= 0)
        return;

    cancel_transitions_for_tree(sys, win_tree);

    float scale = max_output_scale(server);
    int px_w = 0, px_h = 0;
    struct wlr_kfx_target *t0 = capture_tree_snapshot(
        sys, win_tree, outer_box.width, outer_box.height, scale, true, &px_w, &px_h);
    if (!t0)
        return;

    CompFxTransition *tr = create_transition(sys, parent_tree, NULL, COMP_FX_TRANS_CLOSE,
                                             (uint32_t)sys->cfg.transitions.close_preset,
                                             sys->cfg.transitions.close_curve,
                                             sys->cfg.transitions.close_duration_ms,
                                             outer_box, outer_box, t0, NULL);
    if (!tr) {
        wlr_kfx_target_release(t0);
        return;
    }
    tr->t0_px_w = px_w;
    tr->t0_px_h = px_h;
}

void
comp_fx_transition_start_resize(struct CompServer *server,
                                struct wlr_scene_tree *win_tree,
                                struct wlr_scene_tree *parent_tree,
                                struct wlr_box prev_box,
                                struct wlr_box curr_box)
{
    if (!server || !server->fx || !win_tree || !parent_tree)
        return;
    CompFxSystem *sys = server->fx;
    if (!sys->supported || !sys->cfg.enabled || !sys->cfg.transitions.enabled)
        return;
    if (!sys->transition_resize_shader || sys->cfg.transitions.resize_duration_ms <= 0)
        return;
    if (prev_box.width <= 0 || prev_box.height <= 0 ||
        curr_box.width <= 0 || curr_box.height <= 0)
        return;

    cancel_transitions_for_tree(sys, win_tree);

    float scale = max_output_scale(server);
    int px_w0 = 0, px_h0 = 0;
    int px_w1 = 0, px_h1 = 0;
    struct wlr_kfx_target *t0 = capture_tree_snapshot(
        sys, win_tree, prev_box.width, prev_box.height, scale, false, &px_w0, &px_h0);
    struct wlr_kfx_target *t1 = capture_tree_snapshot(
        sys, win_tree, curr_box.width, curr_box.height, scale, false, &px_w1, &px_h1);
    if (!t0 || !t1) {
        if (t0) wlr_kfx_target_release(t0);
        if (t1) wlr_kfx_target_release(t1);
        return;
    }

    CompFxTransition *tr = create_transition(sys, parent_tree, win_tree, COMP_FX_TRANS_RESIZE,
                                             (uint32_t)sys->cfg.transitions.resize_preset,
                                             COMP_FX_CURVE_EASE_OUT_CUBIC,
                                             sys->cfg.transitions.resize_duration_ms,
                                             prev_box, curr_box, t0, t1);
    if (!tr) {
        wlr_kfx_target_release(t0);
        wlr_kfx_target_release(t1);
        return;
    }
    tr->t0_px_w = px_w0;
    tr->t0_px_h = px_h0;
    tr->t1_px_w = px_w1;
    tr->t1_px_h = px_h1;
}

void
comp_fx_transition_update_live(struct CompServer *server,
                               struct wlr_scene_tree *win_tree,
                               struct wlr_box live_box,
                               bool recapture_curr)
{
    if (!server || !server->fx || !win_tree)
        return;
    CompFxSystem *sys = server->fx;
    CompFxTransition *tr;
    wl_list_for_each(tr, &sys->transitions, link) {
        if (tr->src_tree != win_tree)
            continue;
        if (live_box.width > 0 && live_box.height > 0) {
            if (tr->kind == COMP_FX_TRANS_OPEN) {
                tr->prev_box = live_box;
                tr->curr_box = live_box;
            } else if (tr->kind == COMP_FX_TRANS_RESIZE) {
                tr->curr_box = live_box;
                tr->follow_live_box = true;
            }
        }
        if (recapture_curr && live_box.width > 0 && live_box.height > 0) {
            float scale = max_output_scale(server);
            int px_w = 0, px_h = 0;
            struct wlr_kfx_target *fresh = capture_tree_snapshot(
                sys, win_tree, live_box.width, live_box.height, scale, false, &px_w, &px_h);
            if (fresh) {
                if (tr->kind == COMP_FX_TRANS_OPEN) {
                    if (tr->target0)
                        wlr_kfx_target_release(tr->target0);
                    tr->target0 = fresh;
                    tr->t0_px_w = px_w;
                    tr->t0_px_h = px_h;
                } else if (tr->kind == COMP_FX_TRANS_RESIZE) {
                    if (tr->target1)
                        wlr_kfx_target_release(tr->target1);
                    tr->target1 = fresh;
                    tr->t1_px_w = px_w;
                    tr->t1_px_h = px_h;
                } else {
                    wlr_kfx_target_release(fresh);
                }
            }
        }
    }
}

bool
comp_fx_tree_is_transitioning(const struct CompServer *server,
                              const struct wlr_scene_tree *win_tree)
{
    if (!server || !server->fx || !win_tree)
        return false;
    const CompFxSystem *sys = server->fx;
    const CompFxTransition *tr;
    wl_list_for_each(tr, &sys->transitions, link) {
        if (tr->src_tree == win_tree)
            return true;
    }
    return false;
}

bool
fx_transition_prepare_output(CompFxSystem *sys, double now_s)
{
    if (!sys || wl_list_empty(&sys->transitions))
        return false;

    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);

    bool animating = false;
    CompFxTransition *tr, *tmp;
    wl_list_for_each_safe(tr, tmp, &sys->transitions, link) {
        double elapsed = now_s - tr->start_s;
        if (elapsed < 0.0)
            elapsed = 0.0;
        double prog = (tr->duration_s > 0.0) ? (elapsed / tr->duration_s) : 1.0;
        if (prog >= 1.0) {
            transition_destroy(tr, true);
            continue;
        }

        tr->progress       = prog;
        tr->eased_progress = comp_fx_transition_eval_curve(tr->curve, prog);

        if (tr->src_tree)
            send_frame_done_walk(&tr->src_tree->node, &now_ts);

        if (tr->anchor) {
            int x, y, w, h;
            if (tr->follow_live_box) {
                x = tr->curr_box.x;
                y = tr->curr_box.y;
                w = tr->curr_box.width;
                h = tr->curr_box.height;
            } else {
                double e = tr->eased_progress;
                x = (int)lround((double)tr->prev_box.x + (double)(tr->curr_box.x - tr->prev_box.x) * e);
                y = (int)lround((double)tr->prev_box.y + (double)(tr->curr_box.y - tr->prev_box.y) * e);
                w = (int)lround((double)tr->prev_box.width + (double)(tr->curr_box.width - tr->prev_box.width) * e);
                h = (int)lround((double)tr->prev_box.height + (double)(tr->curr_box.height - tr->prev_box.height) * e);
            }
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            wlr_scene_node_set_position(&tr->anchor->node, x, y);
            wlr_scene_buffer_set_dest_size(tr->anchor, w, h);
            /* Force damage on the anchor node each frame so the animated shader repaints. */
            wlr_scene_buffer_set_buffer(tr->anchor, &sys->blank_buf->base);
        }
        animating = true;
    }

    return animating;
}

#endif /* WLR_HAS_KFX */
