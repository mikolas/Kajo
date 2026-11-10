/* libFuzzer target: random operation sequences on the kajo-comp layout model.
 * Every input byte pair is one operation; structural invariants are checked
 * after every operation and a violation aborts (which the fuzzer reports).
 *
 * Build for fuzzing (see DEVELOPMENT.md):
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION \
 *     tests/fuzz/fuzz_comp_layout.c src/comp/core/comp_layout.c -lm -o /tmp/fuzz-comp-layout
 * Without that define it builds a small corpus driver, run by `meson test` as a smoke test.
 */
#include "../../src/comp/core/comp_layout.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check(const CompLayout *L, const char *what)
{
    const char *why = NULL;
    if (!comp_layout_check_invariants(L, &why)) {
        fprintf(stderr, "invariant violated after %s: %s\n", what, why);
        abort();
    }
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 8192)
        return 0;
    CompLayoutConfig cfg;
    comp_layout_config_defaults(&cfg);
    /* Let the first byte pick some config variation */
    if (size > 0) {
        cfg.center_mode = (CompCenterMode)(data[0] % 3);
        cfg.always_center_single_column = (data[0] & 0x08) != 0;
        cfg.gap = (double)(data[0] >> 4);
        cfg.n_preset_widths = (size_t)(data[0] % 4);   /* include zero presets */
    }
    CompLayout *L = comp_layout_new(&cfg);
    if (!L)
        abort();

    CompWinId next = 1;
    size_t live = 0;
    for (size_t i = 1; i + 1 < size; i += 2) {
        uint8_t op = data[i], arg = data[i + 1];
        switch (op % 8) {
        case 0:
            if (comp_layout_add_window(L, next++))
                live++;
            break;
        case 1:
            if (comp_layout_add_floating(L, next++, (CompRect){ arg, arg, arg % 7 ? arg : 0, arg }))
                live++;
            break;
        case 2:
            if (comp_layout_remove_window(L, (CompWinId)(arg % (next + 1))))
                live--;
            break;
        case 3:
            (void)comp_layout_focus_window(L, (CompWinId)(arg % (next + 1)));
            break;
        case 4:
            (void)comp_layout_adjust_column_width(L, ((double)arg - 128.0) / 100.0);
            break;
        case 5:
            (void)comp_layout_adjust_window_height(L, ((double)arg - 128.0) / 100.0);
            break;
        case 6:
            (void)comp_layout_focus_workspace_index(L, arg % 8);
            break;
        default:
            (void)comp_layout_apply(L, (CompAction)(arg % (COMP_ACT__COUNT + 2)));  /* includes invalid */
            break;
        }
        check(L, "operation");
        if (comp_layout_window_count(L) != live)
            abort();

        CompWindowGeometry g[32];
        double vw = 200.0 + (double)arg * 13.0, vh = 150.0 + (double)op * 7.0;
        size_t n = comp_layout_arrange(L, vw, vh, g, 32);
        for (size_t k = 0; k < n && k < 32; k++) {
            if (!isfinite(g[k].rect.x) || !isfinite(g[k].rect.y) || !(g[k].rect.w >= 1.0) ||
                !(g[k].rect.h >= 1.0))
                abort();
        }
        if (!isfinite(comp_layout_view_target(L, vw, (double)arg * 31.0)))
            abort();
    }
    /* Remove everything: must return to a single empty workspace */
    for (CompWinId id = 1; id < next; id++)
        (void)comp_layout_remove_window(L, id);
    check(L, "teardown");
    if (comp_layout_window_count(L) != 0 || comp_layout_workspace_count(L) != 1)
        abort();
    comp_layout_free(L);
    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int
main(void)
{
    /* Deterministic pseudo-random corpus so the smoke test covers many paths */
    uint8_t buf[4096];
    uint32_t x = 0x9e3779b9u;
    for (int round = 0; round < 64; round++) {
        size_t n = 64 + (size_t)round * 60;
        if (n > sizeof(buf))
            n = sizeof(buf);
        for (size_t i = 0; i < n; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            buf[i] = (uint8_t)x;
        }
        LLVMFuzzerTestOneInput(buf, n);
    }
    LLVMFuzzerTestOneInput(NULL, 0);
    return 0;
}
#endif
