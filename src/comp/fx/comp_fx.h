/* comp_fx.h — single public facade between kajo-comp and src/comp/fx/ (CB1).
 *
 * Other src/comp/ files include ONLY this header for effects; they never
 * touch anchors, wlr_kfx_* APIs, or WLR_HAS_KFX (enforced by scripts/audit.sh).
 */
#ifndef KAJO_COMP_FX_H
#define KAJO_COMP_FX_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct CompConfig;
struct CompOutput;
struct CompServer;
struct wlr_buffer;
struct wlr_render_pass;
struct wlr_scene_node;
struct wlr_scene_tree;

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

CompFxSurface *comp_fx_surface_create(struct CompServer *server,
                                      struct wlr_scene_tree *parent_tree,
                                      CompFxSurfaceKind kind);
void comp_fx_surface_sync(CompFxSurface *fx, const CompFxSurfaceState *state);
void comp_fx_surface_destroy(CompFxSurface *fx);

bool comp_fx_is_active(const struct CompServer *server);

/* Screenshot capture helpers (keeps wlr_kfx_* isolated inside src/comp/fx/). */
struct wlr_render_pass *comp_fx_screenshot_begin_pass(struct CompServer *server,
                                                      struct wlr_buffer *buffer);
bool comp_fx_screenshot_render_node(struct CompServer *server,
                                    struct wlr_render_pass *pass,
                                    struct wlr_scene_node *node,
                                    const struct wlr_box *dst_box,
                                    const struct wlr_box *clip_box,
                                    int origin_lx, int origin_ly,
                                    double scale, int pass_w, int pass_h);

#endif /* KAJO_COMP_FX_H */
