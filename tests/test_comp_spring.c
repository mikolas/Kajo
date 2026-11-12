/* Tests for src/comp/core/comp_spring.c (pure core, no wlroots). */
#include "../src/comp/core/comp_spring.h"

#include <glib.h>
#include <math.h>

/* The three spring settings of the shipped comp.toml (spec §3.3.5). */
static const CompSpringParams k_workspace = { 0.80, 523.0, 0.0001 };
static const CompSpringParams k_hscroll   = { 0.85, 423.0, 0.0001 };
static const CompSpringParams k_window    = { 0.75, 323.0, 0.0001 };
static const CompSpringParams k_critical  = { 1.00, 400.0, 0.0001 };
static const CompSpringParams k_over      = { 2.50, 400.0, 0.0001 };

static void
test_params_validation(void)
{
    g_assert_true(comp_spring_params_valid(&k_workspace));
    g_assert_false(comp_spring_params_valid(NULL));
    CompSpringParams bad = k_workspace;
    bad.damping_ratio = 0.0;
    g_assert_false(comp_spring_params_valid(&bad));
    bad = k_workspace;
    bad.stiffness = -1.0;
    g_assert_false(comp_spring_params_valid(&bad));
    bad = k_workspace;
    bad.epsilon = 0.0;
    g_assert_false(comp_spring_params_valid(&bad));
    bad = k_workspace;
    bad.stiffness = NAN;
    g_assert_false(comp_spring_params_valid(&bad));
    bad = k_workspace;
    bad.damping_ratio = INFINITY;
    g_assert_false(comp_spring_params_valid(&bad));
}

static void
test_endpoints_and_settling(void)
{
    const CompSpringParams *all[] = { &k_workspace, &k_hscroll, &k_window, &k_critical, &k_over };
    for (gsize i = 0; i < G_N_ELEMENTS(all); i++) {
        CompSpring s;
        comp_spring_init(&s, all[i], 0.0, 1000.0, 10.0);
        g_assert_cmpfloat_with_epsilon(comp_spring_value(&s, 10.0), 0.0, 1e-9);
        g_assert_cmpfloat_with_epsilon(comp_spring_velocity(&s, 10.0), 0.0, 1e-9);
        /* Times before start clamp to the start */
        g_assert_cmpfloat_with_epsilon(comp_spring_value(&s, 9.0), 0.0, 1e-9);
        g_assert_false(comp_spring_is_done(&s, 10.0));

        double settle = comp_spring_settle_time(&s, 10.0);
        g_assert_cmpfloat(settle, <, 10.0);
        g_assert_true(comp_spring_is_done(&s, 10.0 + settle));
        g_assert_cmpfloat_with_epsilon(comp_spring_value(&s, 10.0 + settle), 1000.0,
                                       all[i]->epsilon * 1000.0 + 1e-9);
    }
}

/* Analytical velocity must equal the numerical derivative of the position. */
static void
test_velocity_matches_derivative(void)
{
    const CompSpringParams *all[] = { &k_workspace, &k_critical, &k_over };
    const double h = 1e-6;
    for (gsize i = 0; i < G_N_ELEMENTS(all); i++) {
        CompSpring s;
        comp_spring_init(&s, all[i], -300.0, 700.0, 0.0);
        for (double t = 0.005; t < 0.6; t += 0.0371) {
            double num = (comp_spring_value(&s, t + h) - comp_spring_value(&s, t - h)) / (2.0 * h);
            double ana = comp_spring_velocity(&s, t);
            g_assert_cmpfloat_with_epsilon(ana, num, fmax(1e-3, fabs(num) * 1e-5));
        }
    }
}

static double
max_value(const CompSpring *s, double t0, double t1)
{
    double m = -INFINITY;
    for (double t = t0; t <= t1; t += 0.0005)
        m = fmax(m, comp_spring_value(s, t));
    return m;
}

static void
test_overshoot_by_damping(void)
{
    CompSpring under, crit, over;
    comp_spring_init(&under, &k_window, 0.0, 100.0, 0.0);
    comp_spring_init(&crit, &k_critical, 0.0, 100.0, 0.0);
    comp_spring_init(&over, &k_over, 0.0, 100.0, 0.0);
    g_assert_cmpfloat(max_value(&under, 0.0, 2.0), >, 100.0 + 1e-6);
    g_assert_cmpfloat(max_value(&crit, 0.0, 2.0), <=, 100.0 + 1e-9);
    g_assert_cmpfloat(max_value(&over, 0.0, 2.0), <=, 100.0 + 1e-9);

    /* Critical and over-damped motion from rest is monotonic */
    double prev = -INFINITY;
    for (double t = 0.0; t < 2.0; t += 0.001) {
        double v = comp_spring_value(&over, t);
        g_assert_cmpfloat(v, >=, prev - 1e-12);
        prev = v;
    }
}

/* Retargeting must keep position and velocity continuous (spec §3.3.5). */
static void
test_retarget_preserves_position_and_velocity(void)
{
    CompSpring s;
    comp_spring_init(&s, &k_hscroll, 0.0, 800.0, 0.0);
    double t = 0.04;
    double p_before = comp_spring_value(&s, t);
    double v_before = comp_spring_velocity(&s, t);
    g_assert_cmpfloat(v_before, >, 0.0);

    comp_spring_retarget(&s, 1600.0, t);
    g_assert_cmpfloat_with_epsilon(comp_spring_value(&s, t), p_before, 1e-9);
    g_assert_cmpfloat_with_epsilon(comp_spring_velocity(&s, t), v_before, 1e-9);

    /* Two retargets in the same instant: still continuous, and it settles at the last target */
    comp_spring_retarget(&s, 2400.0, t);
    comp_spring_retarget(&s, 3200.0, t);
    g_assert_cmpfloat_with_epsilon(comp_spring_value(&s, t), p_before, 1e-9);
    g_assert_cmpfloat_with_epsilon(comp_spring_velocity(&s, t), v_before, 1e-9);
    double settle = comp_spring_settle_time(&s, 10.0);
    g_assert_cmpfloat_with_epsilon(comp_spring_value(&s, t + settle), 3200.0, 3200.0 * 1e-4 + 1e-9);

    /* Reversal mid-flight: velocity is still the old one at the moment of retarget */
    comp_spring_init(&s, &k_hscroll, 0.0, 800.0, 0.0);
    double v_mid = comp_spring_velocity(&s, 0.05);
    comp_spring_retarget(&s, 0.0, 0.05);
    g_assert_cmpfloat_with_epsilon(comp_spring_velocity(&s, 0.05), v_mid, 1e-9);
    g_assert_cmpfloat(v_mid, >, 0.0);
}

/* Retargeting to the current position while still moving must not report done. */
static void
test_retarget_to_current_position_while_moving(void)
{
    CompSpring s;
    comp_spring_init(&s, &k_workspace, 0.0, 500.0, 0.0);
    double t = 0.03;
    double p = comp_spring_value(&s, t);
    comp_spring_retarget(&s, p, t);
    g_assert_false(comp_spring_is_done(&s, t));
    double settle = comp_spring_settle_time(&s, 10.0);
    g_assert_cmpfloat(settle, >, 0.0);
    g_assert_cmpfloat(settle, <, 10.0);
}

static void
test_zero_distance_is_done(void)
{
    CompSpring s;
    comp_spring_init(&s, &k_workspace, 42.0, 42.0, 5.0);
    g_assert_true(comp_spring_is_done(&s, 5.0));
    g_assert_cmpfloat_with_epsilon(comp_spring_settle_time(&s, 10.0), 0.0, 1e-12);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/comp/spring/params-validation", test_params_validation);
    g_test_add_func("/comp/spring/endpoints-and-settling", test_endpoints_and_settling);
    g_test_add_func("/comp/spring/velocity-matches-derivative", test_velocity_matches_derivative);
    g_test_add_func("/comp/spring/overshoot-by-damping", test_overshoot_by_damping);
    g_test_add_func("/comp/spring/retarget-continuity", test_retarget_preserves_position_and_velocity);
    g_test_add_func("/comp/spring/retarget-to-current-position", test_retarget_to_current_position_while_moving);
    g_test_add_func("/comp/spring/zero-distance", test_zero_distance_is_done);
    return g_test_run();
}
