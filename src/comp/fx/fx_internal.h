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
    FX_SLOT_DIM    = 1,
    FX_SLOT_COUNT
} FxSlotKind;

typedef struct CompFxSystem CompFxSystem;

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
    struct wl_list         link; /* CompFxSystem.surfaces */
    CompFxSystem          *sys;
    struct wlr_scene_tree *parent_tree;
    struct wl_listener     tree_destroy;
    CompFxSurfaceKind      kind;
    CompFxSurfaceState     state;

    CompFxSlot             slots[FX_SLOT_COUNT];

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
    struct wl_list           surfaces; /* CompFxSurface.link */

    struct wlr_kfx_shader   *shadow_shader;
    struct wlr_kfx_shader   *dim_shader;
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
extern const CompFxEffectDesc k_fx_dim_desc;

const CompFxEffectDesc *fx_registry_get(FxSlotKind kind);

bool fx_compute_outer_buf_box(const CompFxSurface *fx,
                              const struct wlr_kfx_render_ctx *ctx,
                              CompFxRect *out_outer_buf,
                              float *out_scale, int *out_transform);

#endif /* WLR_HAS_KFX */

#endif /* KAJO_COMP_FX_INTERNAL_H */
