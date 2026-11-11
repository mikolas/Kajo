/* comp_motion.c — see comp_motion.h. */
#include "comp_motion.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define POS_EPS 1e-6

typedef struct {
    CompWinId          id;
    CompSpring         sx, sy;
    CompWindowGeometry geom;       /* latest logical target geometry */
    bool               seen;
} WinMotion;

struct CompMotion {
    CompMotionConfig cfg;
    CompSpring       view_x;
    CompSpring       ws_y;
    bool             view_init;
    size_t           last_ws;
    size_t           last_win_count;
    WinMotion       *wins;
    size_t           nwins, capwins;
};

void
comp_motion_config_defaults(CompMotionConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled           = true;
    cfg->slowdown          = 1.0;
    cfg->workspace_switch  = (CompSpringParams){ .damping_ratio = 0.80, .stiffness = 523.0, .epsilon = 0.0001 };
    cfg->horizontal_scroll = (CompSpringParams){ .damping_ratio = 0.85, .stiffness = 423.0, .epsilon = 0.0001 };
    cfg->window_movement   = (CompSpringParams){ .damping_ratio = 0.75, .stiffness = 323.0, .epsilon = 0.0001 };
}

bool
comp_motion_config_valid(const CompMotionConfig *cfg)
{
    return cfg &&
           isfinite(cfg->slowdown) && cfg->slowdown >= 0.0 && cfg->slowdown <= 100.0 &&
           comp_spring_params_valid(&cfg->workspace_switch) &&
           comp_spring_params_valid(&cfg->horizontal_scroll) &&
           comp_spring_params_valid(&cfg->window_movement);
}

/* Effective spring params scaled by `slowdown` (stiffness scales as 1 / slowdown^2). */
static CompSpringParams
scaled_params(const CompSpringParams *p, double slowdown)
{
    CompSpringParams out = *p;
    if (slowdown > 0.0 && fabs(slowdown - 1.0) > 1e-9)
        out.stiffness = p->stiffness / (slowdown * slowdown);
    return out;
}

CompMotion *
comp_motion_new(const CompMotionConfig *cfg)
{
    if (!comp_motion_config_valid(cfg))
        return NULL;
    CompMotion *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->cfg = *cfg;
    CompSpringParams hp = scaled_params(&cfg->horizontal_scroll, cfg->slowdown);
    CompSpringParams wp = scaled_params(&cfg->workspace_switch, cfg->slowdown);
    comp_spring_init(&m->view_x, &hp, 0.0, 0.0, 0.0);
    comp_spring_init(&m->ws_y, &wp, 0.0, 0.0, 0.0);
    return m;
}

void
comp_motion_free(CompMotion *m)
{
    if (!m)
        return;
    free(m->wins);
    free(m);
}

bool
comp_motion_set_config(CompMotion *m, const CompMotionConfig *cfg)
{
    if (!m || !comp_motion_config_valid(cfg))
        return false;
    m->cfg = *cfg;
    m->view_x.params = scaled_params(&cfg->horizontal_scroll, cfg->slowdown);
    m->ws_y.params   = scaled_params(&cfg->workspace_switch, cfg->slowdown);
    CompSpringParams mp = scaled_params(&cfg->window_movement, cfg->slowdown);
    for (size_t i = 0; i < m->nwins; i++) {
        m->wins[i].sx.params = mp;
        m->wins[i].sy.params = mp;
    }
    return true;
}

static WinMotion *
find_win(CompMotion *m, CompWinId id)
{
    for (size_t i = 0; i < m->nwins; i++)
        if (m->wins[i].id == id)
            return &m->wins[i];
    return NULL;
}

static bool
should_snap(const CompMotion *m)
{
    return !m->cfg.enabled || m->cfg.slowdown <= 1e-6;
}

bool
comp_motion_sync(CompMotion *m, CompLayout *layout,
                 double view_w, double view_h, double now)
{
    if (!m || !layout || !(view_w > 0.0) || !(view_h > 0.0) || !isfinite(now))
        return false;

    bool snap = should_snap(m);
    CompSpringParams hp = scaled_params(&m->cfg.horizontal_scroll, m->cfg.slowdown);
    CompSpringParams wp = scaled_params(&m->cfg.workspace_switch, m->cfg.slowdown);
    CompSpringParams mp = scaled_params(&m->cfg.window_movement, m->cfg.slowdown);

    /* 1. Vertical workspace switch (WS7: 10% vertical gap between adjacent workspaces) */
    size_t cur_ws = comp_layout_active_workspace(layout);
    if (!m->view_init) {
        m->last_ws = cur_ws;
        comp_spring_init(&m->ws_y, &wp, 0.0, 0.0, now);
    } else if (cur_ws != m->last_ws) {
        double step_h = view_h + floor(0.10 * view_h);
        double diff   = (double)cur_ws - (double)m->last_ws;
        double cur_y  = comp_spring_value(&m->ws_y, now);
        if (snap) {
            comp_spring_init(&m->ws_y, &wp, 0.0, 0.0, now);
        } else {
            comp_spring_init(&m->ws_y, &wp, cur_y + diff * step_h, 0.0, now);
        }
        m->last_ws = cur_ws;
    }

    /* 2. Query active workspace window geometries */
    size_t need = comp_layout_arrange(layout, view_w, view_h, NULL, 0);
    CompWindowGeometry stack_g[64];
    CompWindowGeometry *g = need <= 64 ? stack_g
                                       : (CompWindowGeometry *)malloc(need * sizeof(CompWindowGeometry));
    if (need > 64 && !g)
        return false;
    size_t n = comp_layout_arrange(layout, view_w, view_h, g, need);

    /* 3. Horizontal camera `view_x` (O1: snap when first window opens on empty workspace) */
    double cur_vx    = m->view_init ? m->view_x.to : 0.0;
    double target_vx = comp_layout_view_target(layout, view_w, cur_vx);
    if (!m->view_init || snap || (m->last_win_count == 0 && n > 0)) {
        comp_spring_init(&m->view_x, &hp, target_vx, target_vx, now);
        m->view_init = true;
    } else if (fabs(target_vx - m->view_x.to) > POS_EPS) {
        comp_spring_retarget(&m->view_x, target_vx, now);
    }
    m->last_win_count = n;

    /* 4. Per-window x/y springs (M1: smooth retarget; M3: stationary invariant; M5: immediate w/h) */
    if (n > m->capwins) {
        size_t ncap = m->capwins ? m->capwins * 2 : 16;
        while (ncap < n)
            ncap *= 2;
        WinMotion *p = realloc(m->wins, ncap * sizeof(WinMotion));
        if (!p) {
            if (g != stack_g)
                free(g);
            return false;
        }
        m->wins    = p;
        m->capwins = ncap;
    }

    for (size_t i = 0; i < m->nwins; i++)
        m->wins[i].seen = false;

    for (size_t i = 0; i < n; i++) {
        WinMotion *wm = find_win(m, g[i].id);
        if (!wm) {
            wm = &m->wins[m->nwins++];
            memset(wm, 0, sizeof(*wm));
            wm->id = g[i].id;
            comp_spring_init(&wm->sx, &mp, g[i].rect.x, g[i].rect.x, now);
            comp_spring_init(&wm->sy, &mp, g[i].rect.y, g[i].rect.y, now);
        } else if (snap) {
            comp_spring_init(&wm->sx, &mp, g[i].rect.x, g[i].rect.x, now);
            comp_spring_init(&wm->sy, &mp, g[i].rect.y, g[i].rect.y, now);
        } else {
            if (fabs(g[i].rect.x - wm->sx.to) > POS_EPS)
                comp_spring_retarget(&wm->sx, g[i].rect.x, now);
            if (fabs(g[i].rect.y - wm->sy.to) > POS_EPS)
                comp_spring_retarget(&wm->sy, g[i].rect.y, now);
        }
        wm->geom = g[i];
        wm->seen = true;
    }

    /* Compact away windows no longer on the active workspace, preserving order of `g` */
    WinMotion *ordered = n ? malloc(n * sizeof(WinMotion)) : NULL;
    if (n && ordered) {
        for (size_t i = 0; i < n; i++) {
            WinMotion *wm = find_win(m, g[i].id);
            ordered[i] = *wm;
        }
        memcpy(m->wins, ordered, n * sizeof(WinMotion));
        m->nwins = n;
        free(ordered);
    } else if (n == 0) {
        m->nwins = 0;
    }

    if (g != stack_g)
        free(g);
    return true;
}

size_t
comp_motion_sample(const CompMotion *m, double now,
                   CompWindowGeometry *out, size_t max,
                   double *out_view_x, double *out_ws_y)
{
    if (!m)
        return 0;
    bool snap   = should_snap(m);
    double vx   = snap ? m->view_x.to : comp_spring_value(&m->view_x, now);
    double ws_y = snap ? m->ws_y.to   : comp_spring_value(&m->ws_y, now);
    if (out_view_x)
        *out_view_x = vx;
    if (out_ws_y)
        *out_ws_y = ws_y;

    for (size_t i = 0; i < m->nwins && i < max && out; i++) {
        const WinMotion *wm = &m->wins[i];
        CompWindowGeometry g = wm->geom;
        double x = snap ? wm->sx.to : comp_spring_value(&wm->sx, now);
        double y = snap ? wm->sy.to : comp_spring_value(&wm->sy, now);
        g.rect.x = g.floating ? x : (x - vx);
        g.rect.y = y + ws_y;
        out[i]   = g;
    }
    return m->nwins;
}

bool
comp_motion_is_animating(const CompMotion *m, double now)
{
    if (!m || should_snap(m))
        return false;
    if (!comp_spring_is_done(&m->view_x, now) || !comp_spring_is_done(&m->ws_y, now))
        return true;
    for (size_t i = 0; i < m->nwins; i++) {
        if (!comp_spring_is_done(&m->wins[i].sx, now) ||
            !comp_spring_is_done(&m->wins[i].sy, now))
            return true;
    }
    return false;
}
