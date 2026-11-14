/* fx_internal.h — internal declarations shared only within src/comp/fx/ (CB1/CB2). */
#ifndef KAJO_COMP_FX_INTERNAL_H
#define KAJO_COMP_FX_INTERNAL_H

#include "comp_fx.h"
#include "../comp_server.h"
#include "../core/comp_fx_core.h"

#include <wlr/config.h>

#if defined(WLR_HAS_KFX) && WLR_HAS_KFX

#include <wlr/kfx/kfx.h>
#include <wlr/kfx/render.h>
#include <wlr/kfx/scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

typedef enum {
    FX_SLOT_SHADOW = 0,
    FX_SLOT_GLASS  = 1,
    FX_SLOT_DIM    = 2,
    FX_SLOT_COUNT
} FxSlotKind;

typedef struct CompFxSystem CompFxSystem;

typedef struct {
    struct wl_list           link;        /* CompFxSurface.glass_outputs */
    struct CompFxSurface    *surface;
    struct wlr_scene_output *scene_output;
    struct wl_listener       output_destroy;
    struct wlr_kfx_target   *target;
    struct wlr_box           t0_src_box;
    struct wlr_box           last_logical_box;
    uint64_t                 built_bg_serial;
    double                   last_recompute_s;
    bool                     dirty_pending;
    bool                     valid;
} CompFxGlassOutput;

typedef struct {
    struct wl_list           link;        /* CompFxSystem.output_tracks */
    struct wlr_scene_output *scene_output;
    struct wl_listener       output_destroy;
    int                      last_w;
    int                      last_h;
    float                    last_scale;
    int                      last_transform;
    uint64_t                 bg_serial;
} CompFxOutputTrack;

typedef struct {
    struct wlr_buffer base;
} CompFxBlankBuffer;

typedef struct {
    struct wlr_scene_buffer *anchor;
    struct wl_listener       anchor_destroy;
    struct wlr_kfx_node_hook hook;
    struct CompFxSurface    *surface;
    FxSlotKind               kind;
    bool                     enabled;
    struct wlr_box           local_box;
    float                    last_opacity;
} CompFxSlot;

struct CompFxSurface {
    struct wl_list         link;          /* CompFxSystem.surfaces */
    CompFxSystem          *sys;
    struct wlr_scene_tree *parent_tree;
    struct wl_listener     tree_destroy;
    CompFxSurfaceKind      kind;
    CompFxSurfaceState     state;

    CompFxSlot             slots[FX_SLOT_COUNT];
    struct wl_list         glass_outputs; /* CompFxGlassOutput.link */
    uint64_t               glass_recomputes;

    bool                   spring_init;
    bool                   was_focused;
    CompSpring             shadow_spring;
    CompSpring             dim_spring;
    double                 shadow_target;
    double                 dim_target;
};

struct CompFxSystem {
    CompServer              *server;
    CompFxConfig             cfg;
    bool                     supported;
    bool                     logged_unsupported;

    CompFxBlankBuffer       *blank_buf;
    struct wl_list           surfaces;      /* CompFxSurface.link */
    struct wl_list           output_tracks; /* CompFxOutputTrack.link */

    struct wlr_kfx_shader   *shadow_shader;
    struct wlr_kfx_shader   *dim_shader;
    struct wlr_kfx_shader   *blur_down_shader;
    struct wlr_kfx_shader   *blur_up_shader;
    struct wlr_kfx_shader   *glass_shader;
};

typedef struct {
    FxSlotKind                 kind;
    const char                *name;
    enum wlr_kfx_hook_stage    stage;
    bool                     (*init_shaders)(CompFxSystem *sys);
    void                     (*destroy_shaders)(CompFxSystem *sys);
    bool                     (*should_enable)(const CompFxSystem *sys,
                                              const CompFxSurface *fx);
    struct wlr_box           (*compute_box)(const CompFxSystem *sys,
                                            const CompFxSurface *fx);
    void                     (*draw)(CompFxSystem *sys, CompFxSurface *fx,
                                     const struct wlr_kfx_render_ctx *ctx);
} CompFxEffectDesc;

extern const CompFxEffectDesc k_fx_shadow_desc;
extern const CompFxEffectDesc k_fx_glass_desc;
extern const CompFxEffectDesc k_fx_dim_desc;

const CompFxEffectDesc *fx_registry_get(FxSlotKind kind);

bool fx_compute_slot_pass_boxes(const CompFxSurface *fx,
                                FxSlotKind slot_kind,
                                const struct wlr_kfx_render_ctx *ctx,
                                CompFxRect *out_full_slot_buf,
                                CompFxRect *out_pass_outer_buf,
                                float *out_eff_scale,
                                int *out_transform);

void fx_glass_surface_clear_targets(CompFxSurface *fx);
void fx_glass_system_invalidate_all(CompFxSystem *sys);
bool fx_glass_prepare_output(CompFxSystem *sys, struct CompOutput *output,
                             CompFxOutputTrack *track, double now_s);

#endif /* WLR_HAS_KFX */

#endif /* KAJO_COMP_FX_INTERNAL_H */
