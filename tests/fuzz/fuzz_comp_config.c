/* libFuzzer target: arbitrary bytes through the binds.toml and rules.toml parsers,
 * then lookups/matches on whatever parsed. Must never crash or leak.
 *
 * Build for fuzzing (see DEVELOPMENT.md):
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION \
 *     -D_POSIX_C_SOURCE=200809L tests/fuzz/fuzz_comp_config.c src/comp/core/comp_{diag,layout,binds,rules}.c \
 *     src/vendor/tomlc17/tomlc17.c $(pkg-config --cflags --libs xkbcommon libpcre2-8) -lm -o /tmp/fuzz-comp-config
 */
#include "../../src/comp/core/comp_binds.h"
#include "../../src/comp/core/comp_rules.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536)
        return 0;
    /* tomlc17 takes a pointer + length but some paths expect NUL termination */
    char *src = malloc(size + 1);
    if (!src)
        return 0;
    if (size)
        memcpy(src, data, size);
    src[size] = '\0';

    CompDiagList d;
    comp_diag_init(&d);
    CompBinds *b = comp_binds_parse(src, size, "fuzz-binds", COMP_MOD_SUPER, &d);
    for (size_t i = 0; i < comp_binds_count(b); i++) {
        const CompBind *x = comp_binds_get(b, i);
        if (!x || !x->spec)
            abort();
        if (x->trigger == COMP_TRIGGER_KEY) {
            if (comp_binds_lookup_key(b, x->mods, x->keysym) != x)
                abort();
        } else if (comp_binds_lookup_wheel(b, x->mods, x->trigger) != x) {
            abort();
        }
        if (x->kind == COMP_BIND_SPAWN && (!x->argv || !x->argv[0]))
            abort();
    }
    comp_binds_free(b);

    CompRules *r = comp_rules_parse(src, size, "fuzz-rules", &d);
    CompWindowRuleResult w;
    CompLayerRuleResult l;
    comp_rules_match_window(r, "kitty", "~", &w);
    comp_rules_match_window(r, NULL, "\xff\xfe invalid utf8", &w);
    comp_rules_match_layer(r, "kajo", &l);
    comp_rules_free(r);

    comp_diag_clear(&d);
    free(src);
    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int
main(void)
{
    static const char *const k_corpus[] = {
        "",
        "[binds]\n\"Mod+H\" = \"focus-column-left\"\n",
        "[binds]\n\"Mod+T\" = { spawn = [\"kitty\"], repeat = false, cooldown_ms = -5 }\n",
        "[binds]\n\"+\" = 1\n\"Mod+\" = {}\n\"Mod+Mod+X\" = []\n",
        "[binds]\n\"Mod+Minus\" = { action = \"set-column-width\", delta = \"-1e309%\" }\n",
        "[binds]\n\"Mod+WheelScrollDown\" = { action = \"focus-workspace-down\", cooldown_ms = 9223372036854775807 }\n",
        "[[rule]]\napp_id = ['(?<x>a)\\\\k<x>', '\\\\p{L}+', '(((((((((((a)))))))))))']\nfloating = true\n",
        "[[rule]]\ntitle = '.*' \nanchor = { corner = \"top-left\", x = 1e308, y = nan }\n",
        "[[layer_rule]]\nnamespace = ''\nopacity = inf\nblur = true\n",
        "[[rule]]\n[[rule]]\napp_id = 5\n[[layer_rule]]\n",
        "rule = 1\nlayer_rule = \"x\"\nbinds = []\n",
        "[binds\n",
    };
    for (size_t i = 0; i < sizeof(k_corpus) / sizeof(k_corpus[0]); i++)
        LLVMFuzzerTestOneInput((const uint8_t *)k_corpus[i], strlen(k_corpus[i]));
    return 0;
}
#endif
