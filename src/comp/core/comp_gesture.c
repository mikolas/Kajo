/* comp_gesture.c — see comp_gesture.h. */
#include "comp_gesture.h"

#include <math.h>
#include <string.h>

void
comp_gesture_begin(CompGesture *g)
{
    if (!g)
        return;
    memset(g, 0, sizeof(*g));
    g->active = true;
}

void
comp_gesture_end(CompGesture *g)
{
    if (g)
        memset(g, 0, sizeof(*g));
}

static void
push_sample(CompGesture *g, double t, double d)
{
    if (g->nsamples == COMP_GESTURE_SAMPLES) {
        memmove(&g->samples[0], &g->samples[1], (COMP_GESTURE_SAMPLES - 1) * sizeof(g->samples[0]));
        g->nsamples--;
    }
    g->samples[g->nsamples++] = (CompGestureSample){ .t = t, .d = d };
}

double
comp_gesture_rubber_band(double v, double lo, double hi, double dim)
{
    if (lo > hi) {
        double tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (!(dim > 0.0) || !isfinite(v))
        return fmin(hi, fmax(lo, isfinite(v) ? v : lo));
    if (v < lo) {
        double d = lo - v;
        return lo - (1.0 - 1.0 / (d * COMP_GESTURE_RUBBER_K / dim + 1.0)) * dim;
    }
    if (v > hi) {
        double d = v - hi;
        return hi + (1.0 - 1.0 / (d * COMP_GESTURE_RUBBER_K / dim + 1.0)) * dim;
    }
    return v;
}

static double
travel(const CompGesture *g)
{
    return (g->axis == COMP_GESTURE_AXIS_X ? g->dx : g->dy) - g->lock_at;
}

CompGestureAxis
comp_gesture_update(CompGesture *g, double ddx, double ddy, double t)
{
    if (!g || !g->active || !isfinite(ddx) || !isfinite(ddy) || !isfinite(t))
        return COMP_GESTURE_AXIS_NONE;
    g->dx += ddx;
    g->dy += ddy;

    if (g->axis == COMP_GESTURE_AXIS_NONE) {
        double ax = fabs(g->dx), ay = fabs(g->dy);
        if (ax >= COMP_GESTURE_LOCK_PX && ax > COMP_GESTURE_LOCK_RATIO * ay)
            g->axis = COMP_GESTURE_AXIS_X;
        else if (ay >= COMP_GESTURE_LOCK_PX && ay > COMP_GESTURE_LOCK_RATIO * ax)
            g->axis = COMP_GESTURE_AXIS_Y;
        if (g->axis == COMP_GESTURE_AXIS_NONE)
            return COMP_GESTURE_AXIS_NONE;
        g->lock_at = g->axis == COMP_GESTURE_AXIS_X ? g->dx : g->dy;
        push_sample(g, t, 0.0);
        return g->axis;
    }

    double d = travel(g);
    push_sample(g, t, d);
    g->value = comp_gesture_rubber_band(g->start - d, g->lo, g->hi, g->dim);
    return COMP_GESTURE_AXIS_NONE;
}

void
comp_gesture_set_range(CompGesture *g, double start, double lo, double hi, double dim)
{
    if (!g)
        return;
    g->start = isfinite(start) ? start : 0.0;
    g->lo    = isfinite(lo) ? lo : g->start;
    g->hi    = isfinite(hi) ? hi : g->start;
    if (g->lo > g->hi) {
        double tmp = g->lo;
        g->lo = g->hi;
        g->hi = tmp;
    }
    g->dim   = (isfinite(dim) && dim > 0.0) ? dim : 0.0;
    g->value = comp_gesture_rubber_band(g->start - (g->axis ? travel(g) : 0.0), g->lo, g->hi, g->dim);
}

double
comp_gesture_velocity(const CompGesture *g, double t)
{
    if (!g || g->nsamples == 0 || !isfinite(t))
        return 0.0;
    /* A finger held still reports no motion events: treat `t` as a sample at the last position. */
    const CompGestureSample *last = &g->samples[g->nsamples - 1];
    double t_end = fmax(t, last->t);
    double d_end = last->d;
    size_t first = g->nsamples - 1;
    while (first > 0 && g->samples[first - 1].t >= t_end - COMP_GESTURE_VEL_WINDOW_S)
        first--;
    const CompGestureSample *a = &g->samples[first];
    double dt = t_end - a->t;
    if (dt < 1e-3)
        return 0.0;
    return -(d_end - a->d) / dt;   /* camera moves opposite to the fingers */
}

double
comp_gesture_projected(const CompGesture *g, double t)
{
    if (!g)
        return 0.0;
    return g->value + comp_gesture_velocity(g, t) * COMP_GESTURE_TAU_S;
}

size_t
comp_gesture_pick_workspace(double released, double projected, size_t n)
{
    if (n == 0)
        return 0;
    double hi = (double)(n - 1);
    double near = isfinite(released) ? fmin(hi, fmax(0.0, round(released))) : 0.0;
    double p    = isfinite(projected) ? round(projected) : near;
    p = fmin(near + 1.0, fmax(near - 1.0, p));
    p = fmin(hi, fmax(0.0, p));
    return (size_t)p;
}

size_t
comp_gesture_pick_snap(const double *snaps, size_t n, size_t current, double projected)
{
    if (!snaps || n == 0)
        return 0;
    size_t best = current < n ? current : n - 1;
    if (!isfinite(projected))
        return best;
    double bd = fabs(snaps[best] - projected);
    for (size_t i = 0; i < n; i++) {
        double d = fabs(snaps[i] - projected);
        if (d < bd - 0.5) {
            bd   = d;
            best = i;
        }
    }
    return best;
}
