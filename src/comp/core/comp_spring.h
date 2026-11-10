/* comp_spring.h — analytical damped-spring animation for kajo-comp.
 *
 * Pure C, no wlroots/GLib. A spring animates one scalar from `from` to `to`
 * using the closed-form solution of a unit-mass harmonic oscillator
 *   x'' = -k (x - to) - c x',   c = 2 * zeta * sqrt(k)
 * so position and velocity are exact at any time (no integration error, no
 * dependence on frame rate). Retargeting keeps the instantaneous position and
 * velocity, which is what makes rapid repeated input (e.g. Mod+L, Mod+L) feel
 * continuous instead of restarting.
 *
 * Time is in seconds. Position units are the caller's (logical pixels, or a
 * 0..1 progress). Parameters match Niri's `spring` animation settings.
 */
#ifndef KAJO_COMP_SPRING_H
#define KAJO_COMP_SPRING_H

#include <stdbool.h>

typedef struct {
    double damping_ratio; /* zeta: <1 under-damped (overshoot), 1 critical, >1 over-damped */
    double stiffness;     /* k, per second squared (unit mass) */
    double epsilon;       /* settle threshold, relative to the distance of the current segment */
} CompSpringParams;

typedef struct {
    CompSpringParams params;
    double from;          /* position at start_time */
    double to;            /* target */
    double v0;            /* velocity at start_time */
    double start_time;    /* seconds */
    double scale;         /* |from - to| when the segment started, floor 1.0 (epsilon reference) */
} CompSpring;

/* False if params are unusable: non-finite, damping_ratio <= 0, stiffness <= 0
 * or epsilon <= 0. Callers validate config values with this before use. */
bool   comp_spring_params_valid(const CompSpringParams *params);

/* Start a spring at rest at `from`, heading to `to`. */
void   comp_spring_init(CompSpring *s, const CompSpringParams *params,
                        double from, double to, double now);

/* Exact position and velocity at time `t` (t < start_time is treated as start_time). */
double comp_spring_value(const CompSpring *s, double t);
double comp_spring_velocity(const CompSpring *s, double t);

/* Change the target at time `now`, keeping the current position and velocity. */
void   comp_spring_retarget(CompSpring *s, double to, double now);

/* True once both the remaining distance and the velocity are within
 * epsilon * scale (velocity measured per second). */
bool   comp_spring_is_done(const CompSpring *s, double t);

/* Time (seconds after start_time) until comp_spring_is_done() first holds,
 * searched up to `max_seconds`; returns max_seconds if it does not settle. */
double comp_spring_settle_time(const CompSpring *s, double max_seconds);

#endif /* KAJO_COMP_SPRING_H */
