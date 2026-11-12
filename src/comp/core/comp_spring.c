/* comp_spring.c — see comp_spring.h. */
#include "comp_spring.h"

#include <math.h>

/* Closed-form displacement d(t) = x(t) - to and its derivative for initial
 * displacement x0 and velocity v0 (unit mass). */
static void
spring_eval(const CompSpringParams *p, double x0, double v0, double t,
            double *disp, double *vel)
{
    const double w0   = sqrt(p->stiffness);
    const double zeta = p->damping_ratio;

    if (t <= 0.0) {
        *disp = x0;
        *vel  = v0;
        return;
    }

    /* Treat |zeta - 1| < 1e-9 as critical damping: the under- and over-damped
     * forms divide by a vanishing root there. */
    if (fabs(zeta - 1.0) < 1e-9) {
        const double b = v0 + w0 * x0;
        const double e = exp(-w0 * t);
        *disp = e * (x0 + b * t);
        *vel  = e * (b - w0 * (x0 + b * t));
    } else if (zeta < 1.0) {
        const double wd = w0 * sqrt(1.0 - zeta * zeta);
        const double a  = zeta * w0;
        const double b  = (v0 + a * x0) / wd;
        const double e  = exp(-a * t);
        const double c  = cos(wd * t);
        const double s  = sin(wd * t);
        *disp = e * (x0 * c + b * s);
        *vel  = e * ((b * wd - a * x0) * c - (x0 * wd + a * b) * s);
    } else {
        const double root = sqrt(zeta * zeta - 1.0);
        const double r1   = -w0 * (zeta - root); /* slow root */
        const double r2   = -w0 * (zeta + root); /* fast root */
        const double c2   = (v0 - r1 * x0) / (r2 - r1);
        const double c1   = x0 - c2;
        const double e1   = exp(r1 * t);
        const double e2   = exp(r2 * t);
        *disp = c1 * e1 + c2 * e2;
        *vel  = c1 * r1 * e1 + c2 * r2 * e2;
    }
}

bool
comp_spring_params_valid(const CompSpringParams *params)
{
    return params &&
           isfinite(params->damping_ratio) && params->damping_ratio > 0.0 &&
           isfinite(params->stiffness) && params->stiffness > 0.0 &&
           isfinite(params->epsilon) && params->epsilon > 0.0;
}

static void
spring_start_segment(CompSpring *s, double from, double to, double v0, double now)
{
    s->from       = from;
    s->to         = to;
    s->v0         = v0;
    s->start_time = now;
    s->scale      = fmax(fabs(from - to), 1.0);
}

void
comp_spring_init(CompSpring *s, const CompSpringParams *params,
                 double from, double to, double now)
{
    s->params = *params;
    spring_start_segment(s, from, to, 0.0, now);
}

double
comp_spring_value(const CompSpring *s, double t)
{
    double d, v;
    spring_eval(&s->params, s->from - s->to, s->v0, t - s->start_time, &d, &v);
    return s->to + d;
}

double
comp_spring_velocity(const CompSpring *s, double t)
{
    double d, v;
    spring_eval(&s->params, s->from - s->to, s->v0, t - s->start_time, &d, &v);
    return v;
}

void
comp_spring_retarget(CompSpring *s, double to, double now)
{
    double d, v;
    spring_eval(&s->params, s->from - s->to, s->v0, now - s->start_time, &d, &v);
    spring_start_segment(s, s->to + d, to, v, now);
}

void
comp_spring_launch(CompSpring *s, const CompSpringParams *params,
                   double from, double to, double v0, double now)
{
    s->params = *params;
    spring_start_segment(s, from, to, isfinite(v0) ? v0 : 0.0, now);
}

void
comp_spring_shift(CompSpring *s, double d)
{
    if (!isfinite(d))
        return;
    s->from += d;
    s->to   += d;
}

bool
comp_spring_is_done(const CompSpring *s, double t)
{
    double d, v;
    spring_eval(&s->params, s->from - s->to, s->v0, t - s->start_time, &d, &v);
    const double tol = s->params.epsilon * s->scale;
    return fabs(d) <= tol && fabs(v) <= tol;
}

double
comp_spring_settle_time(const CompSpring *s, double max_seconds)
{
    /* Coarse scan at 1 ms, then the first settled sample. Overshooting springs
     * pass through zero displacement with non-zero velocity, so both terms are
     * checked at each sample. */
    const double step = 0.001;
    for (double dt = 0.0; dt <= max_seconds; dt += step) {
        if (comp_spring_is_done(s, s->start_time + dt))
            return dt;
    }
    return max_seconds;
}
