/* comp_window_private.h — internal window struct, border state & chrome helpers. */
#ifndef KAJO_COMP_WINDOW_PRIVATE_H
#define KAJO_COMP_WINDOW_PRIVATE_H

#include "comp_server.h"
#include "fx/comp_fx.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland/xwayland.h>

#define COMP_BORDER_PX 1
#define COMP_MAX_TABS  16

typedef struct {
    int32_t          tile_w;
    int32_t          tile_h;
    int32_t          border_px;
    struct wlr_box   geom;
    bool             has_mapped_surf;
    bool             fullscreen;
    bool             focused;
    bool             urgent;
    bool             maximized;
    bool             tabbed;
    bool             resizing;
    bool             floating;
    bool             has_rule_shadow;
    bool             rule_shadow;
    bool             has_rule_blur;
    bool             rule_blur;
    bool             has_rule_glass_style;
    CompFxGlassStyle rule_glass_style;
    bool             mapped;
    size_t           tab_count;
    size_t           tab_index;
    float            col_active[4];
    float            col_inactive[4];
    float            col_urgent[4];
} CompBorderState;

typedef struct CompWindow {
    struct wl_list                          link;        /* CompServer.windows (mapped only) */
    CompServer                             *server;
    CompWinId                               id;          /* 0 when unmapped */
    struct wlr_xdg_toplevel                *toplevel;
    struct wlr_xwayland_surface            *xsurf;

    struct wlr_scene_tree                  *scene_tree;  /* node.data == CompWindow* */
    struct wlr_scene_rect                  *backdrop;    /* solid #000000 behind fullscreen */
    struct wlr_scene_tree                  *xdg_tree;    /* client surface + subsurfaces (clipped) */
    struct wlr_scene_rect                  *border[4];   /* top, bottom, left, right 1px rects */
    struct wlr_scene_rect                  *tabs[COMP_MAX_TABS]; /* spec §3.9 T3 tab strip */
    struct wlr_scene_tree                  *popup_tree;  /* xdg_popup trees (unclipped, above borders) */
    CompFxSurface                          *fx;

    struct wlr_foreign_toplevel_handle_v1      *foreign_handle;
    struct wlr_ext_foreign_toplevel_handle_v1  *ext_foreign_handle;
    struct wlr_ext_image_capture_source_v1     *capture_source;
    void                                       *foreign_listeners;

    char                                   *title;
    char                                   *app_id;
    uint64_t                                focus_timestamp;

    bool                                    mapped;
    bool                                    tick_seen;
    bool                                    pending_open_transition;
    bool                                    was_resizing;
    bool                                    was_fullscreen;
    bool                                    was_floating;
    int32_t                                 last_outer_w;
    int32_t                                 last_outer_h;
    int32_t                                 resize_start_w;
    int32_t                                 resize_start_h;
    int32_t                                 resize_target_w;
    int32_t                                 resize_target_h;
    bool                                    is_floating;
    bool                                    is_fullscreen;
    bool                                    is_urgent;
    bool                                    manually_floated;
    bool                                    rule_vrr;
    bool                                    has_rule_shadow;
    bool                                    rule_shadow;
    bool                                    has_rule_blur;
    bool                                    rule_blur;
    bool                                    has_rule_blur_popups;
    bool                                    rule_blur_popups;
    bool                                    has_rule_glass_style;
    CompFxGlassStyle                        rule_glass_style;
    double                                  scroll_factor;
    bool                                    maximized_to_edges;

    int32_t                                 cfg_w;
    int32_t                                 cfg_h;
    int32_t                                 cfg_x;
    int32_t                                 cfg_y;
    bool                                    cfg_activated;
    bool                                    cfg_fullscreen;
    bool                                    cfg_maximized;
    uint32_t                                cfg_tiled;

    bool                                    border_state_valid;
    CompBorderState                         last_border;
    bool                                    has_last_g;
    int32_t                                 last_rw;
    int32_t                                 last_rh;
    CompWindowGeometry                      last_g;

    CompRect                                last_screen_rect;
} CompWindow;

static inline int32_t
comp_window_server_border_px(const CompServer *s)
{
    return (s && s->config) ? s->config->border_px : COMP_BORDER_PX;
}

static inline int32_t
comp_window_clamp_i32(double v, int32_t lo, int32_t hi)
{
    if (!isfinite(v)) return lo;
    double r = round(v);
    if (r < (double)lo) return lo;
    if (r > (double)hi) return hi;
    return (int32_t)r;
}

/* Shared helper implemented in comp_window.c */
CompWindowGeometry *comp_window_ensure_geom_scratch(CompServer *s, size_t need);

/* Chrome, borders, tab strip, toplevel configure & interactive move/resize
 * implemented in comp_window_chrome.c */
bool     comp_window_create_chrome(CompWindow *w, struct wlr_xdg_surface *xdg_surf);
CompRect comp_window_clamp_floating_rect(CompRect r, double vw, double vh);
CompRect comp_window_initial_floating_rect(const CompWindow *w, const CompWindowRuleResult *rr,
                                           double vw, double vh);
void     comp_window_update_borders(CompWindow *w, int32_t tile_w, int32_t tile_h,
                                    const CompWindowGeometry *g);
void     comp_window_configure_toplevel(CompWindow *w, int32_t tile_x, int32_t tile_y,
                                        int32_t tile_w, int32_t tile_h,
                                        bool activated, bool fullscreen, bool floating);
void     comp_window_handle_request_move(void *owner, void *data);
void     comp_window_handle_request_resize(void *owner, void *data);

#endif /* KAJO_COMP_WINDOW_PRIVATE_H */
