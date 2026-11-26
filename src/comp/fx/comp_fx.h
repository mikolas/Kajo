/* comp_fx.h — single public facade between kajo-comp and src/comp/fx/ (CB1).
 *
 * Other src/comp/ files include ONLY this header for effects; they never
 * touch anchors, wlr_kfx_* APIs, or WLR_HAS_KFX (enforced by scripts/audit.sh).
 */
#ifndef KAJO_COMP_FX_H
#define KAJO_COMP_FX_H

#include "../core/comp_fx_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

struct CompConfig;
struct CompOutput;
struct CompServer;
struct wlr_buffer;
struct wlr_color_transform;
struct wlr_output;
struct wlr_render_pass;
struct wlr_scene_node;
struct wlr_scene_tree;

typedef struct {
    bool  supported;
    float max_cll_nits;
    float max_fall_nits;
    float min_luma_nits;
} CompFxOutputHdrCaps;

typedef struct CompFxSurface CompFxSurface;

typedef enum {
    COMP_FX_SURFACE_WINDOW = 0,
    COMP_FX_SURFACE_POPUP,
    COMP_FX_SURFACE_LAYER,
} CompFxSurfaceKind;

typedef struct {
    struct wlr_box         outer_box;    /* outer box in parent_tree coords (borders/window) */
    struct wlr_box         surface_box;  /* content surface box in parent_tree coords */
    struct wlr_scene_node *shadow_below; /* sibling to place shadow anchor below (nullable) */
    struct wlr_scene_node *dim_above;    /* sibling to place dim anchor above (nullable) */
    bool                   has_rule_shadow;
    bool                   rule_shadow;
    bool                   has_rule_blur;
    bool                   rule_blur;
    bool                   has_rule_xray;
    bool                   rule_xray;
    bool                   has_rule_glass_style;
    CompFxGlassStyle       rule_glass_style;
    bool                   is_floating;
    bool                   is_fullscreen;
    bool                   is_focused;
    bool                   is_background_or_bottom;
    bool                   mapped;
} CompFxSurfaceState;

bool comp_fx_init(struct CompServer *server);
void comp_fx_finish(struct CompServer *server);
void comp_fx_config_apply(struct CompServer *server, const struct CompConfig *cfg);
void comp_fx_renderer_lost(struct CompServer *server);

/* Called in comp_output.c right before wlr_scene_output_build_state().
 * Returns true while any effect timeline is still animating and needs frames. */
bool comp_fx_output_prepare(struct CompOutput *output, double now_s);
void comp_fx_background_changed(struct CompOutput *output);
struct wlr_color_transform *comp_fx_output_get_color_transform(struct CompOutput *output);

CompFxSurface *comp_fx_surface_create(struct CompServer *server,
                                      struct wlr_scene_tree *parent_tree,
                                      CompFxSurfaceKind kind);
void comp_fx_surface_sync(CompFxSurface *fx, const CompFxSurfaceState *state);
void comp_fx_surface_destroy(CompFxSurface *fx);
uint64_t comp_fx_surface_get_glass_recomputes(const CompFxSurface *fx);

/* Window open / close / resize transitions (M5, FX_IN_TEXTURE). */
void comp_fx_transition_start_open(struct CompServer *server,
                                   struct wlr_scene_tree *win_tree,
                                   struct wlr_scene_tree *parent_tree,
                                   struct wlr_box outer_box);
void comp_fx_transition_start_close(struct CompServer *server,
                                    struct wlr_scene_tree *win_tree,
                                    struct wlr_scene_tree *parent_tree,
                                    struct wlr_box outer_box);
void comp_fx_transition_start_resize(struct CompServer *server,
                                     struct wlr_scene_tree *win_tree,
                                     struct wlr_scene_tree *parent_tree,
                                     struct wlr_box prev_box,
                                     struct wlr_box curr_box);
void comp_fx_transition_update_live(struct CompServer *server,
                                    struct wlr_scene_tree *win_tree,
                                    struct wlr_box live_box,
                                    int capture_w, int capture_h,
                                    bool recapture_curr);
bool comp_fx_transition_sync_resize(struct CompServer *server,
                                    struct wlr_scene_tree *win_tree,
                                    struct wlr_box live_box,
                                    double progress,
                                    bool done);
void comp_fx_transition_cancel_tree(struct CompServer *server,
                                    const struct wlr_scene_tree *win_tree);
bool comp_fx_tree_is_transitioning(const struct CompServer *server,
                                   const struct wlr_scene_tree *win_tree);

bool comp_fx_is_active(const struct CompServer *server);

/* Output HDR capabilities & state helpers (keeps wlr_kfx_* isolated inside src/comp/fx/). */
bool  comp_fx_output_get_hdr_caps(struct wlr_output *output, CompFxOutputHdrCaps *out);
float comp_fx_output_get_hdr_headroom(struct wlr_output *output);
float comp_fx_output_get_sdr_white_nits(struct wlr_output *output);
float comp_fx_output_get_brightness(struct wlr_output *output);

/* Screenshot capture helpers (keeps wlr_kfx_* isolated inside src/comp/fx/). */
struct wlr_render_pass *comp_fx_screenshot_begin_pass(struct CompServer *server,
                                                      struct wlr_buffer *buffer);
struct wlr_render_pass *comp_fx_screenshot_begin_hdr_pass(struct CompServer *server,
                                                          struct wlr_buffer *buffer);
bool comp_fx_screenshot_render_node(struct CompServer *server,
                                    struct wlr_render_pass *pass,
                                    struct wlr_scene_node *node,
                                    const struct wlr_box *dst_box,
                                    const struct wlr_box *clip_box,
                                    int origin_lx, int origin_ly,
                                    double scale, int pass_w, int pass_h);

#endif /* KAJO_COMP_FX_H */
