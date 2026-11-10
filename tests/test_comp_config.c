/* Tests for src/comp/core/comp_binds.c and comp_rules.c (pure core). */
#include "../src/comp/core/comp_binds.h"
#include "../src/comp/core/comp_rules.h"

#include <glib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static CompBinds *
parse_binds(const char *text, CompDiagList *d)
{
    comp_diag_init(d);
    return comp_binds_parse(text, strlen(text), "binds.toml", COMP_MOD_SUPER, d);
}

static void
dump_diag(const CompDiagList *d)
{
    for (size_t i = 0; i < d->n; i++)
        g_test_message("diag: %s", d->msgs[i]);
}

static void
test_binds_basic(void)
{
    CompDiagList d;
    CompBinds *b = parse_binds(
        "[binds]\n"
        "\"Mod+H\" = \"focus-column-left\"\n"
        "\"Mod+Shift+L\" = \"move-column-right\"\n"
        "\"Mod+T\" = { spawn = [\"kitty\", \"-e\", \"htop\"] }\n"
        "\"Mod+Q\" = { action = \"close-window\", repeat = false }\n"
        "\"Mod+WheelScrollDown\" = { action = \"focus-workspace-down\", cooldown_ms = 150 }\n"
        "\"Mod+Minus\" = { action = \"set-column-width\", delta = \"-10%\" }\n"
        "\"Mod+Equal\" = { action = \"set-window-height\", delta = \"0.25\" }\n"
        "\"XF86AudioMute\" = { spawn = [\"kajo\", \"--volume-mute\"], allow_when_locked = true }\n"
        "\"Alt+Tab\" = { action = \"next-window\", scope = \"output\" }\n"
        "\"Alt+grave\" = { action = \"next-window\", filter = \"app-id\" }\n"
        "\"Ctrl+Print\" = { spawn = [\"kajo-capture\", \"--screen\"] }\n",
        &d);
    dump_diag(&d);
    g_assert_nonnull(b);
    g_assert_cmpuint(d.n, ==, 0);
    g_assert_cmpuint(comp_binds_count(b), ==, 11);

    const CompBind *x = comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_h);
    g_assert_nonnull(x);
    g_assert_cmpint(x->kind, ==, COMP_BIND_LAYOUT);
    g_assert_cmpint(x->layout_action, ==, COMP_ACT_FOCUS_COLUMN_LEFT);
    g_assert_true(x->repeat);

    /* Upper-case keysym from the client side still matches (lower-cased) */
    x = comp_binds_lookup_key(b, COMP_MOD_SUPER | COMP_MOD_SHIFT, XKB_KEY_L);
    g_assert_nonnull(x);
    g_assert_cmpint(x->layout_action, ==, COMP_ACT_MOVE_COLUMN_RIGHT);
    g_assert_cmpstr(x->spec, ==, "Super+Shift+l");
    /* Modifiers must match exactly */
    g_assert_null(comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_l));
    g_assert_null(comp_binds_lookup_key(b, COMP_MOD_SUPER | COMP_MOD_SHIFT | COMP_MOD_CTRL, XKB_KEY_l));

    x = comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_t);
    g_assert_cmpint(x->kind, ==, COMP_BIND_SPAWN);
    g_assert_cmpstr(x->argv[0], ==, "kitty");
    g_assert_cmpstr(x->argv[2], ==, "htop");
    g_assert_null(x->argv[3]);

    x = comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_q);
    g_assert_cmpint(x->kind, ==, COMP_BIND_CLOSE_WINDOW);
    g_assert_false(x->repeat);

    x = comp_binds_lookup_wheel(b, COMP_MOD_SUPER, COMP_TRIGGER_WHEEL_DOWN);
    g_assert_nonnull(x);
    g_assert_cmpint(x->cooldown_ms, ==, 150);
    g_assert_null(comp_binds_lookup_wheel(b, COMP_MOD_SUPER, COMP_TRIGGER_WHEEL_UP));

    x = comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_minus);
    g_assert_cmpint(x->kind, ==, COMP_BIND_SET_COLUMN_WIDTH);
    g_assert_cmpfloat_with_epsilon(x->delta, -0.1, 1e-12);
    x = comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_equal);
    g_assert_cmpfloat_with_epsilon(x->delta, 0.25, 1e-12);

    x = comp_binds_lookup_key(b, 0, XKB_KEY_XF86AudioMute);
    g_assert_true(x->allow_when_locked);

    x = comp_binds_lookup_key(b, COMP_MOD_ALT, XKB_KEY_Tab);
    g_assert_cmpint(x->kind, ==, COMP_BIND_NEXT_WINDOW);
    g_assert_cmpint(x->scope, ==, COMP_SWITCH_SCOPE_OUTPUT);
    x = comp_binds_lookup_key(b, COMP_MOD_ALT, XKB_KEY_grave);
    g_assert_true(x->same_app);

    g_assert_nonnull(comp_binds_lookup_key(b, COMP_MOD_CTRL, XKB_KEY_Print));
    comp_binds_free(b);
    comp_diag_clear(&d);
}

static void
test_binds_mod_meaning(void)
{
    CompDiagList d;
    comp_diag_init(&d);
    const char *t = "[binds]\n\"Mod+J\" = \"focus-window-down\"\n";
    CompBinds *b = comp_binds_parse(t, strlen(t), "binds.toml", COMP_MOD_ALT, &d);
    g_assert_nonnull(comp_binds_lookup_key(b, COMP_MOD_ALT, XKB_KEY_j));
    g_assert_null(comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_j));
    comp_binds_free(b);
    comp_diag_clear(&d);
}

/* Every error path skips the entry with a specific message and keeps the rest. */
static void
test_binds_errors(void)
{
    CompDiagList d;
    CompBinds *b = parse_binds(
        "[binds]\n"
        "\"Mod+H\" = \"focus-colum-left\"\n"                        /* 2 typo */
        "\"Mod+D\" = \"toggle-overview\"\n"                         /* 3 planned */
        "\"Hyper+X\" = \"close-window\"\n"                          /* 4 bad modifier */
        "\"Mod+NoSuchKey\" = \"close-window\"\n"                    /* 5 bad key */
        "\"Mod+Shift+Mod+X\" = \"close-window\"\n"                  /* 6 modifier twice */
        "\"Mod+\" = \"close-window\"\n"                             /* 7 no key */
        "\"Mod+T\" = { spawn = \"kitty\" }\n"                       /* 8 spawn as string */
        "\"Mod+Y\" = { spawn = [] }\n"                              /* 9 empty spawn */
        "\"Mod+U\" = { action = \"close-window\", spawn = [\"x\"] }\n" /* 10 both */
        "\"Mod+I\" = { action = \"set-column-width\" }\n"           /* 11 no delta */
        "\"Mod+O\" = { action = \"set-column-width\", delta = \"lots\" }\n" /* 12 bad delta */
        "\"Mod+P\" = \"set-window-height\"\n"                       /* 13 needs table */
        "\"Mod+A\" = 42\n"                                          /* 14 bad value */
        "\"Super+B\" = \"close-window\"\n"                          /* 15 ok */
        "\"Mod+b\" = \"maximize-column\"\n"                         /* 16 duplicate of 15 */
        "\"Mod+C\" = { action = \"center-column\", cooldown_ms = 99999, colour = 1 }\n" /* 17 clamp + unknown */
        "\"Mod+E\" = { action = \"close-window\", delta = \"5%\", scope = \"output\" }\n", /* 18 ignored opts */
        &d);
    dump_diag(&d);
    g_assert_nonnull(b);
    g_assert_true(comp_diag_contains(&d, "binds.toml:2: 'Super+h': unknown action 'focus-colum-left'"));
    g_assert_true(comp_diag_contains(&d, "binds.toml:3: 'Super+d': action 'toggle-overview' is planned but not available in this version"));
    g_assert_true(comp_diag_contains(&d, "binds.toml:4: 'Hyper+X': unknown modifier"));
    g_assert_true(comp_diag_contains(&d, "binds.toml:5: 'Mod+NoSuchKey': unknown key name"));
    g_assert_true(comp_diag_contains(&d, ":6: 'Mod+Shift+Mod+X': modifier given twice"));
    g_assert_true(comp_diag_contains(&d, ":7: 'Mod+': missing key"));
    g_assert_true(comp_diag_contains(&d, ":8: 'Super+t': spawn takes an array"));
    g_assert_true(comp_diag_contains(&d, ":9: 'Super+y': spawn must be a non-empty array"));
    g_assert_true(comp_diag_contains(&d, ":10: 'Super+u': give exactly one of 'action' or 'spawn'"));
    g_assert_true(comp_diag_contains(&d, ":11: 'Super+i': 'delta' must be"));
    g_assert_true(comp_diag_contains(&d, ":12: 'Super+o': 'delta' must be"));
    g_assert_true(comp_diag_contains(&d, ":13: 'Super+p': 'set-window-height' needs an inline table with a delta"));
    g_assert_true(comp_diag_contains(&d, ":14: 'Super+a': value must be"));
    g_assert_true(comp_diag_contains(&d, ":16: 'Mod+b' is the same key as line 15"));
    g_assert_true(comp_diag_contains(&d, ":17: 'Super+c': cooldown_ms 99999 out of range"));
    g_assert_true(comp_diag_contains(&d, ":17: 'Super+c': unknown option 'colour' ignored"));
    g_assert_true(comp_diag_contains(&d, ":18: 'Super+e': 'delta' only applies"));
    g_assert_true(comp_diag_contains(&d, ":18: 'Super+e': 'scope' only applies"));

    /* Survivors: 15 (close-window), 17 (center-column, clamped), 18 (close-window) */
    g_assert_cmpuint(comp_binds_count(b), ==, 3);
    const CompBind *x = comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_b);
    g_assert_cmpint(x->kind, ==, COMP_BIND_CLOSE_WINDOW);
    x = comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_c);
    g_assert_cmpint(x->cooldown_ms, ==, 10000);
    comp_binds_free(b);
    comp_diag_clear(&d);
}

static void
test_binds_syntax_error_and_empty(void)
{
    CompDiagList d;
    g_assert_null(parse_binds("[binds\n\"Mod+H\" = \"x\"\n", &d));
    g_assert_true(comp_diag_contains(&d, "TOML syntax error"));
    g_assert_true(comp_diag_contains(&d, "keeping previous binds"));
    comp_diag_clear(&d);

    CompBinds *b = parse_binds("", &d);
    g_assert_nonnull(b);
    g_assert_cmpuint(comp_binds_count(b), ==, 0);
    comp_binds_free(b);
    comp_diag_clear(&d);

    b = parse_binds("[keys]\nx = 1\n", &d);
    g_assert_true(comp_diag_contains(&d, "unknown top-level key 'keys'"));
    comp_binds_free(b);
    comp_diag_clear(&d);
}

/* The binds.toml example in the spec (§4.3.2) must parse without any message, so the
 * spec cannot advertise binds the compositor does not have. Planned ones are commented out. */
static void
test_binds_spec_example(void)
{
    const gchar *root = g_getenv("KAJO_SOURCE_ROOT");
    if (!root)
        root = KAJO_SOURCE_DIR;
    g_autofree gchar *path = g_build_filename(root, "docs", "spec_kajo_comp.md", NULL);
    g_autofree gchar *doc = NULL;
    g_assert_true(g_file_get_contents(path, &doc, NULL, NULL));
    const char *start = strstr(doc, "#### 2. `~/.config/kajo/binds.toml`");
    g_assert_nonnull(start);
    start = strstr(start, "```toml\n");
    g_assert_nonnull(start);
    start += strlen("```toml\n");
    const char *end = strstr(start, "```");
    g_assert_nonnull(end);
    g_autofree gchar *toml = g_strndup(start, (gsize)(end - start));

    CompDiagList d;
    CompBinds *b = parse_binds(toml, &d);
    dump_diag(&d);
    g_assert_nonnull(b);
    g_assert_cmpuint(d.n, ==, 0);
    g_assert_cmpuint(comp_binds_count(b), >, 60);
    comp_binds_free(b);
    comp_diag_clear(&d);
}

/* ───────────────────────── rules ───────────────────────── */

static CompRules *
parse_rules(const char *text, CompDiagList *d)
{
    comp_diag_init(d);
    return comp_rules_parse(text, strlen(text), "rules.toml", d);
}

static void
test_rules_matching_and_order(void)
{
    CompDiagList d;
    CompRules *r = parse_rules(
        "[[rule]]\n"
        "app_id = ['^org\\.gnome\\.Calculator$', '^galculator$']\n"
        "floating = true\n"
        "[[rule]]\n"
        "app_id = '^steam$'\n"
        "title  = '^notificationtoasts_\\d+_desktop$'\n"
        "floating = true\n"
        "anchor = { corner = \"bottom-right\", x = 10, y = 10 }\n"
        "focus = false\n"
        "[[rule]]\n"
        "title = ['Brave', 'Chromium']\n"
        "scroll_factor = 0.15\n"
        "[[rule]]\n"
        "app_id = '^galculator$'\n"
        "floating = false\n"
        "width = 0.5\n"
        "[[rule]]\n"
        "app_id = ['^org\\.kajo\\.Player$', '^steam_app_\\d+$']\n"
        "vrr = true\n",
        &d);
    dump_diag(&d);
    g_assert_nonnull(r);
    g_assert_cmpuint(d.n, ==, 0);
    g_assert_cmpuint(comp_rules_window_count(r), ==, 5);

    CompWindowRuleResult m;
    comp_rules_match_window(r, "org.gnome.Calculator", "Calculator", &m);
    g_assert_true(m.has_floating && m.floating);
    g_assert_false(m.has_width);
    /* '.' is escaped: a different app id must not match */
    comp_rules_match_window(r, "orgXgnomeXCalculator", "", &m);
    g_assert_false(m.has_floating);

    /* Later rules override earlier ones */
    comp_rules_match_window(r, "galculator", "", &m);
    g_assert_true(m.has_floating);
    g_assert_false(m.floating);
    g_assert_true(m.has_width);
    g_assert_cmpfloat_with_epsilon(m.width, 0.5, 1e-12);

    /* Both fields must match; \d works (PCRE2) */
    comp_rules_match_window(r, "steam", "notificationtoasts_17_desktop", &m);
    g_assert_true(m.has_anchor);
    g_assert_cmpint(m.corner, ==, COMP_CORNER_BOTTOM_RIGHT);
    g_assert_cmpfloat_with_epsilon(m.anchor_x, 10.0, 1e-12);
    g_assert_true(m.has_focus && !m.focus);
    comp_rules_match_window(r, "steam", "Steam", &m);
    g_assert_false(m.has_anchor);

    /* Unanchored substring match on the title */
    comp_rules_match_window(r, "brave-browser", "Kajo — Brave", &m);
    g_assert_true(m.has_scroll_factor);
    g_assert_cmpfloat_with_epsilon(m.scroll_factor, 0.15, 1e-12);

    comp_rules_match_window(r, "steam_app_1091500", "", &m);
    g_assert_true(m.has_vrr && m.vrr);
    comp_rules_match_window(r, NULL, NULL, &m);
    g_assert_false(m.has_vrr || m.has_floating || m.has_scroll_factor);
    comp_rules_free(r);
    comp_diag_clear(&d);
}

static void
test_rules_errors(void)
{
    CompDiagList d;
    CompRules *r = parse_rules(
        "[[rule]]\n"
        "floating = true\n"                                   /* 1-2 no match field */
        "[[rule]]\n"
        "app_id = '([unclosed'\n"                             /* 3-4 bad regex */
        "floating = true\n"
        "[[rule]]\n"
        "app_id = 'ok'\n"
        "width = 2.0\n"                                       /* out of range */
        "floating = \"yes\"\n"                                /* wrong type */
        "anchor = { corner = \"middle\" }\n"                  /* bad corner */
        "colour = 3\n"                                        /* unknown key */
        "[[layer_rule]]\n"
        "namespace = '^kajo$'\n"
        "opacity = 0.85\n"
        "blur = true\n"                                       /* unsupported in v1 */
        "[[layer_rule]]\n"
        "opacity = 0.5\n"                                     /* no namespace */
        "[layouts]\n"
        "x = 1\n",
        &d);
    dump_diag(&d);
    g_assert_nonnull(r);
    g_assert_true(comp_diag_contains(&d, "rules.toml:1: [[rule]] needs app_id and/or title"));
    g_assert_true(comp_diag_contains(&d, "'app_id' regex '([unclosed' is invalid"));
    g_assert_true(comp_diag_contains(&d, "'width' = 2 is outside 0.05..1; ignored"));
    g_assert_true(comp_diag_contains(&d, "'floating' must be true or false"));
    g_assert_true(comp_diag_contains(&d, "anchor.corner must be"));
    g_assert_true(comp_diag_contains(&d, "unknown rule key 'colour' ignored"));
    g_assert_true(comp_diag_contains(&d, "layer_rule 'blur' is not supported in this version"));
    g_assert_true(comp_diag_contains(&d, "[[layer_rule]] needs a namespace"));
    g_assert_true(comp_diag_contains(&d, "unknown top-level key 'layouts'"));
    g_assert_cmpuint(comp_rules_window_count(r), ==, 1);
    g_assert_cmpuint(comp_rules_layer_count(r), ==, 1);

    CompWindowRuleResult m;
    comp_rules_match_window(r, "ok", "", &m);
    g_assert_false(m.has_width || m.has_floating || m.has_anchor);

    CompLayerRuleResult l;
    comp_rules_match_layer(r, "kajo", &l);
    g_assert_true(l.has_opacity);
    g_assert_cmpfloat_with_epsilon(l.opacity, 0.85, 1e-12);
    comp_rules_match_layer(r, "kajo-launcher", &l);
    g_assert_false(l.has_opacity);
    comp_rules_free(r);
    comp_diag_clear(&d);

    g_assert_null(parse_rules("[[rule]\n", &d));
    g_assert_true(comp_diag_contains(&d, "keeping previous rules"));
    comp_diag_clear(&d);
}

/* The rules.toml example in the spec (§4.3.3) must parse cleanly. */
static void
test_rules_spec_example(void)
{
    const gchar *root = g_getenv("KAJO_SOURCE_ROOT");
    if (!root)
        root = KAJO_SOURCE_DIR;
    g_autofree gchar *path = g_build_filename(root, "docs", "spec_kajo_comp.md", NULL);
    g_autofree gchar *doc = NULL;
    g_assert_true(g_file_get_contents(path, &doc, NULL, NULL));
    const char *start = strstr(doc, "#### 3. `~/.config/kajo/rules.toml`");
    g_assert_nonnull(start);
    start = strstr(start, "```toml\n");
    g_assert_nonnull(start);
    start += strlen("```toml\n");
    const char *end = strstr(start, "```");
    g_autofree gchar *toml = g_strndup(start, (gsize)(end - start));

    CompDiagList d;
    CompRules *r = parse_rules(toml, &d);
    dump_diag(&d);
    g_assert_nonnull(r);
    g_assert_cmpuint(d.n, ==, 0);
    g_assert_cmpuint(comp_rules_window_count(r), >=, 6);
    g_assert_cmpuint(comp_rules_layer_count(r), ==, 1);
    CompLayerRuleResult l;
    comp_rules_match_layer(r, "kajo-osd", &l);
    g_assert_true(l.has_opacity && l.has_popup_opacity);
    comp_rules_free(r);
    comp_diag_clear(&d);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/comp/binds/basic", test_binds_basic);
    g_test_add_func("/comp/binds/mod-meaning", test_binds_mod_meaning);
    g_test_add_func("/comp/binds/errors", test_binds_errors);
    g_test_add_func("/comp/binds/syntax-error-and-empty", test_binds_syntax_error_and_empty);
    g_test_add_func("/comp/binds/spec-example", test_binds_spec_example);
    g_test_add_func("/comp/rules/matching-and-order", test_rules_matching_and_order);
    g_test_add_func("/comp/rules/errors", test_rules_errors);
    g_test_add_func("/comp/rules/spec-example", test_rules_spec_example);
    return g_test_run();
}
