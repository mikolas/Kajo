/* comp_motion.c — see comp_motion.h. */
#include "comp_motion.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define POS_EPS  1e-6
#define SIZE_EPS 0.5   /* px: displayed size counts as settled within half a pixel */

typedef struct {
    CompWinId          id;
    uint64_t           ws_id;
    size_t             ws_idx;     /* index of ws_id at the last sync */
    CompSpring         sx, sy, sw, sh;
    CompWindowGeometry geom;       /* latest logical target geometry */
} WinMotion;

typedef struct {
    uint64_t   ws_id;
    CompSpring cam;
    size_t     win_count;          /* windows on the workspace at the last sync (O1) */
} WsCamera;

struct CompMotion {
    CompMotionConfig   cfg;
    bool               view_init;
    double             step;        /* view_h + WS7 gap, from the last sync */
    double             view_h;
    CompSpring         strip;       /* camera on the vertical strip (px) */
    size_t             active_idx;
    uint64_t           active_id;
    uint64_t          *ws_ids;      /* workspace order at the last sync */
    size_t             nws_ids, capws_ids;
    WsCamera          *cams;
    size_t             ncams, capcams;
    WinMotion         *wins;
    size_t             nwins, capwins;
    CompMotionDragAxis drag;
    double             drag_value;
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
    CompSpringParams wp = scaled_params(&cfg->workspace_switch, cfg->slowdown);
    comp_spring_init(&m->strip, &wp, 0.0, 0.0, 0.0);
    return m;
}

void
comp_motion_free(CompMotion *m)
{
    if (!m)
        return;
    free(m->ws_ids);
    free(m->cams);
    free(m->wins);
    free(m);
}

bool
comp_motion_set_config(CompMotion *m, const CompMotionConfig *cfg)
{
    if (!m || !comp_motion_config_valid(cfg))
        return false;
    m->cfg = *cfg;
    m->strip.params = scaled_params(&cfg->workspace_switch, cfg->slowdown);
    CompSpringParams hp = scaled_params(&cfg->horizontal_scroll, cfg->slowdown);
    for (size_t i = 0; i < m->ncams; i++)
        m->cams[i].cam.params = hp;
    CompSpringParams mp = scaled_params(&cfg->window_movement, cfg->slowdown);
    for (size_t i = 0; i < m->nwins; i++) {
        m->wins[i].sx.params = mp;
        m->wins[i].sy.params = mp;
        m->wins[i].sw.params = mp;
        m->wins[i].sh.params = mp;
    }
    return true;
}

static bool
should_snap(const CompMotion *m)
{
    return !m->cfg.enabled || m->cfg.slowdown <= 1e-6;
}

static bool
grow(void **arr, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap)
        return true;
    size_t ncap = *cap ? *cap * 2 : 8;
    while (ncap < need)
        ncap *= 2;
    void *p = realloc(*arr, ncap * elem);
    if (!p)
        return false;
    *arr = p;
    *cap = ncap;
    return true;
}

static bool
old_index_of(const CompMotion *m, uint64_t ws_id, size_t *out)
{
    for (size_t i = 0; i < m->nws_ids; i++) {
        if (m->ws_ids[i] == ws_id) {
            *out = i;
            return true;
        }
    }
    return false;
}

static WsCamera *
find_cam(const CompMotion *m, uint64_t ws_id)
{
    for (size_t i = 0; i < m->ncams; i++)
        if (m->cams[i].ws_id == ws_id)
            return &m->cams[i];
    return NULL;
}

static WinMotion *
find_win(WinMotion *wins, size_t n, CompWinId id)
{
    for (size_t i = 0; i < n; i++)
        if (wins[i].id == id)
            return &wins[i];
    return NULL;
}

static void
retarget_or_snap(CompSpring *s, const CompSpringParams *p, double to, bool snap, double now)
{
    if (snap)
        comp_spring_init(s, p, to, to, now);
    else if (fabs(to - s->to) > POS_EPS)
        comp_spring_retarget(s, to, now);
}

/* WS7: workspaces are stacked with a 10 % gap. The strip camera is in pixels;
 * workspace i sits at i * step. Indices are rebased by stable workspace id. */
static double
sync_strip(CompMotion *m, const CompLayout *layout, double view_h, bool snap,
           const CompSpringParams *wp, double now)
{
    double step   = view_h + floor(0.10 * view_h);
    size_t active = comp_layout_active_workspace(layout);
    uint64_t aid  = comp_layout_workspace_id(layout, active);

    if (!m->view_init || fabs(step - m->step) > POS_EPS) {
        comp_spring_init(&m->strip, wp, (double)active * step, (double)active * step, now);
        if (m->drag == COMP_MOTION_DRAG_Y)
            m->drag_value = (double)active * step;
    } else {
        /* Rebase: keep the on-screen position of a workspace whose index moved. */
        size_t o = 0;
        double shift = 0.0;
        if (old_index_of(m, aid, &o)) {
            shift = ((double)active - (double)o) * step;
        } else {
            size_t n = comp_layout_workspace_count(layout);
            for (size_t i = 0; i < n; i++) {
                if (comp_layout_workspace_id(layout, i) == m->active_id) {
                    shift = ((double)i - (double)m->active_idx) * step;
                    break;
                }
            }
        }
        comp_spring_shift(&m->strip, shift);
        if (m->drag == COMP_MOTION_DRAG_Y)
            m->drag_value += shift;
        if (m->drag != COMP_MOTION_DRAG_Y)
            retarget_or_snap(&m->strip, wp, (double)active * step, snap, now);
        else
            m->strip.to = (double)active * step;
        /* Window strip offsets are rebased by the caller using `shift`. */
        m->step = step;
        m->active_idx = active;
        m->active_id  = aid;
        return shift;
    }
    m->step       = step;
    m->active_idx = active;
    m->active_id  = aid;
    return 0.0;
}

static bool
sync_cameras(CompMotion *m, CompLayout *layout, double view_w, bool snap,
             const CompSpringParams *hp, double now)
{
    size_t nws = comp_layout_workspace_count(layout);
    WsCamera *ncams = nws ? calloc(nws, sizeof(WsCamera)) : NULL;
    if (nws && !ncams)
        return false;
    for (size_t i = 0; i < nws; i++) {
        uint64_t id  = comp_layout_workspace_id(layout, i);
        size_t   cnt = comp_layout_workspace_window_count(layout, i);
        WsCamera *old = find_cam(m, id);
        bool dragged = (m->drag == COMP_MOTION_DRAG_X && i == m->active_idx);
        WsCamera c = old ? *old : (WsCamera){ .ws_id = id };
        double cur = old ? c.cam.to : 0.0;
        if (dragged) {
            ncams[i] = c;
            ncams[i].win_count = cnt;
            continue;
        }
        double target = comp_layout_workspace_view_target(layout, i, view_w, cur);
        if (!old || (old->win_count == 0 && cnt > 0))
            comp_spring_init(&c.cam, hp, target, target, now);   /* O1 */
        else
            retarget_or_snap(&c.cam, hp, target, snap, now);
        c.win_count = cnt;
        ncams[i] = c;
    }
    free(m->cams);
    m->cams    = ncams;
    m->ncams   = nws;
    m->capcams = nws;
    return true;
}

static bool
sync_windows(CompMotion *m, const CompLayout *layout, double view_w, double view_h,
             double strip_shift, bool snap, const CompSpringParams *mp, double now)
{
    size_t nws   = comp_layout_workspace_count(layout);
    size_t total = 0;
    for (size_t i = 0; i < nws; i++)
        total += comp_layout_arrange_workspace(layout, i, view_w, view_h, NULL, 0);

    CompWindowGeometry *g = total ? malloc(total * sizeof(*g)) : NULL;
    WinMotion *nw = total ? malloc(total * sizeof(*nw)) : NULL;
    if (total && (!g || !nw)) {
        free(g);
        free(nw);
        return false;
    }

    size_t k = 0;
    for (size_t i = 0; i < nws && k < total; i++) {
        uint64_t id = comp_layout_workspace_id(layout, i);
        size_t n = comp_layout_arrange_workspace(layout, i, view_w, view_h, g + k, total - k);
        if (n > total - k)
            n = total - k;
        for (size_t j = 0; j < n; j++) {
            const CompWindowGeometry *t = &g[k + j];
            WinMotion *old = find_win(m->wins, m->nwins, t->id);
            WinMotion w;
            if (!old) {
                memset(&w, 0, sizeof(w));
                w.id = t->id;
                comp_spring_init(&w.sx, mp, t->rect.x, t->rect.x, now);
                comp_spring_init(&w.sy, mp, t->rect.y, t->rect.y, now);
                comp_spring_init(&w.sw, mp, t->rect.w, t->rect.w, now);
                comp_spring_init(&w.sh, mp, t->rect.h, t->rect.h, now);
            } else {
                w = *old;
                /* M4 + rebase: keep the on-screen y of a window whose workspace index changed. */
                double dy = ((double)old->ws_idx - (double)i) * m->step + strip_shift;
                if (fabs(dy) > POS_EPS)
                    comp_spring_shift(&w.sy, dy);
                retarget_or_snap(&w.sx, mp, t->rect.x, snap, now);
                retarget_or_snap(&w.sy, mp, t->rect.y, snap, now);
                bool snap_size = snap || t->floating;
                retarget_or_snap(&w.sw, mp, t->rect.w, snap_size, now);
                retarget_or_snap(&w.sh, mp, t->rect.h, snap_size, now);
            }
            w.ws_id  = id;
            w.ws_idx = i;
            w.geom   = *t;
            nw[k + j] = w;
        }
        k += n;
    }

    free(g);
    free(m->wins);
    m->wins    = nw;
    m->nwins   = k;
    m->capwins = total;
    return true;
}

static bool
remember_order(CompMotion *m, const CompLayout *layout)
{
    size_t nws = comp_layout_workspace_count(layout);
    if (!grow((void **)&m->ws_ids, &m->capws_ids, nws ? nws : 1, sizeof(uint64_t)))
        return false;
    for (size_t i = 0; i < nws; i++)
        m->ws_ids[i] = comp_layout_workspace_id(layout, i);
    m->nws_ids = nws;
    return true;
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

    m->view_h = view_h;
    double shift = sync_strip(m, layout, view_h, snap, &wp, now);
    m->view_init = true;
    if (!sync_cameras(m, layout, view_w, snap, &hp, now))
        return false;
    if (!sync_windows(m, layout, view_w, view_h, shift, snap, &mp, now))
        return false;
    return remember_order(m, layout);
}

static double
strip_value(const CompMotion *m, double now, bool snap)
{
    if (m->drag == COMP_MOTION_DRAG_Y)
        return m->drag_value;
    return snap ? m->strip.to : comp_spring_value(&m->strip, now);
}

static double
cam_value(const CompMotion *m, size_t ws_idx, double now, bool snap)
{
    if (ws_idx >= m->ncams)
        return 0.0;
    if (m->drag == COMP_MOTION_DRAG_X && ws_idx == m->active_idx)
        return m->drag_value;
    const CompSpring *c = &m->cams[ws_idx].cam;
    return snap ? c->to : comp_spring_value(c, now);
}

size_t
comp_motion_sample(const CompMotion *m, double now,
                   CompWindowGeometry *out, size_t max,
                   double *out_view_x, double *out_ws_y)
{
    if (!m)
        return 0;
    bool snap    = should_snap(m);
    double strip = strip_value(m, now, snap);
    if (out_view_x)
        *out_view_x = cam_value(m, m->active_idx, now, snap);
    if (out_ws_y)
        *out_ws_y = (double)m->active_idx * m->step - strip;

    for (size_t i = 0; i < m->nwins && i < max && out; i++) {
        const WinMotion *wm = &m->wins[i];
        CompWindowGeometry g = wm->geom;
        double off = (double)wm->ws_idx * m->step - strip;
        double x = snap ? wm->sx.to : comp_spring_value(&wm->sx, now);
        double y = snap ? wm->sy.to : comp_spring_value(&wm->sy, now);
        double w = snap ? wm->sw.to : comp_spring_value(&wm->sw, now);
        double h = snap ? wm->sh.to : comp_spring_value(&wm->sh, now);
        g.rect.x = g.floating ? x : (x - cam_value(m, wm->ws_idx, now, snap));
        g.rect.y = y + off;
        g.rect.w = fmax(1.0, w);
        g.rect.h = fmax(1.0, h);
        g.visible  = wm->geom.visible && fabs(off) < m->view_h - 0.5;
        g.resizing = fabs(g.rect.w - wm->geom.rect.w) > SIZE_EPS ||
                     fabs(g.rect.h - wm->geom.rect.h) > SIZE_EPS;
        out[i] = g;
    }
    return m->nwins;
}

bool
comp_motion_is_animating(const CompMotion *m, double now)
{
    if (!m)
        return false;
    if (m->drag != COMP_MOTION_DRAG_NONE)
        return true;
    if (should_snap(m))
        return false;
    if (!comp_spring_is_done(&m->strip, now))
        return true;
    for (size_t i = 0; i < m->ncams; i++)
        if (!comp_spring_is_done(&m->cams[i].cam, now))
            return true;
    for (size_t i = 0; i < m->nwins; i++) {
        const WinMotion *w = &m->wins[i];
        if (!comp_spring_is_done(&w->sx, now) || !comp_spring_is_done(&w->sy, now) ||
            !comp_spring_is_done(&w->sw, now) || !comp_spring_is_done(&w->sh, now))
            return true;
    }
    return false;
}

double
comp_motion_workspace_step(const CompMotion *m)
{
    return m ? m->step : 0.0;
}

bool
comp_motion_drag_begin(CompMotion *m, CompMotionDragAxis axis, double now, double *out_start)
{
    if (!m || !m->view_init || m->drag != COMP_MOTION_DRAG_NONE ||
        (axis != COMP_MOTION_DRAG_X && axis != COMP_MOTION_DRAG_Y))
        return false;
    bool snap = should_snap(m);
    double start = axis == COMP_MOTION_DRAG_X ? cam_value(m, m->active_idx, now, snap)
                                              : strip_value(m, now, snap);
    m->drag       = axis;
    m->drag_value = start;
    if (out_start)
        *out_start = start;
    return true;
}

void
comp_motion_drag_set(CompMotion *m, double value)
{
    if (m && m->drag != COMP_MOTION_DRAG_NONE && isfinite(value))
        m->drag_value = value;
}

void
comp_motion_drag_end(CompMotion *m, double velocity, double target, double now)
{
    if (!m || m->drag == COMP_MOTION_DRAG_NONE)
        return;
    double v  = should_snap(m) || !isfinite(velocity) ? 0.0 : velocity;
    double to = isfinite(target) ? target : m->drag_value;
    if (m->drag == COMP_MOTION_DRAG_Y) {
        CompSpringParams wp = scaled_params(&m->cfg.workspace_switch, m->cfg.slowdown);
        comp_spring_launch(&m->strip, &wp, m->drag_value, to, v, now);
    } else if (m->active_idx < m->ncams) {
        CompSpringParams hp = scaled_params(&m->cfg.horizontal_scroll, m->cfg.slowdown);
        comp_spring_launch(&m->cams[m->active_idx].cam, &hp, m->drag_value, to, v, now);
    }
    m->drag = COMP_MOTION_DRAG_NONE;
}

CompMotionDragAxis
comp_motion_drag_axis(const CompMotion *m)
{
    return m ? m->drag : COMP_MOTION_DRAG_NONE;
}
