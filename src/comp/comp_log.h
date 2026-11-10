/* comp_log.h — logging and recoverable-invariant macros for kajo-comp.
 *
 * kajo-comp never aborts on a broken invariant (spec §7.2.1). COMP_WARN_RETURN
 * logs the failed condition with its location — rate-limited per call site so
 * a misbehaving client cannot flood the journal — and returns `fallback` from
 * the enclosing function.
 *
 * Rule: never log key codes, keysyms or text from input events. The compositor
 * sees every keystroke, including passwords typed into a lock screen or greeter.
 */
#ifndef KAJO_COMP_LOG_H
#define KAJO_COMP_LOG_H

#include <stdbool.h>
#include <wlr/util/log.h>

/* True at most once per `interval_s` seconds for the given per-site state. */
bool comp_log_rate_ok(double *last_s, double interval_s);

#define COMP_WARN_RETURN(cond, fallback)                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            static double comp__last_ = -1.0e9;                                       \
            if (comp_log_rate_ok(&comp__last_, 10.0))                                 \
                wlr_log(WLR_ERROR, "invariant failed: %s (in %s)", #cond, __func__);  \
            return fallback;                                                          \
        }                                                                             \
    } while (0)

#define COMP_WARN_RETURN_VOID(cond)                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            static double comp__last_ = -1.0e9;                                       \
            if (comp_log_rate_ok(&comp__last_, 10.0))                                 \
                wlr_log(WLR_ERROR, "invariant failed: %s (in %s)", #cond, __func__);  \
            return;                                                                   \
        }                                                                             \
    } while (0)

#endif /* KAJO_COMP_LOG_H */
