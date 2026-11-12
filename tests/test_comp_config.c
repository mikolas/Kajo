/* Tests for src/comp/core/comp_binds.c, comp_rules.c, and comp_config.c (pure core). */
#include "../src/comp/core/comp_binds.h"
#include "../src/comp/core/comp_config.h"
#include "../src/comp/core/comp_rules.h"
#include "../src/comp/core/comp_touch.h"

#include <glib.h>
#include <glib/gstdio.h>
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
        "[[bind]]\n"
        "keys = [\"mod+left\", \"mod+h\"]\n"
        "action = \"focus-column-left\"\n"
        "[[bind]]\n"
        "key = \"mod+shift+l\"\n"
        "action = \"move-column-right\"\n"
        "[[bind]]\n"
        "key = \"mod+t\"\n"
        "spawn = [\"kitty\", \"-e\", \"htop\"]\n"
        "[[bind]]\n"
        "key = \"mod+q\"\n"
        "action = \"close-window\"\n"
        "repeat = false\n"
        "[[bind]]\n"
        "key = \"mod+wheel-down\"\n"
        "action = \"focus-workspace-down\"\n"
        "cooldown_ms = 150\n"
        "[[bind]]\n"
        "key = \"mod+minus\"\n"
        "action = \"set-column-width\"\n"
        "delta = \"-10%\"\n"
        "[[bind]]\n"
        "key = \"mod+equal\"\n"
        "action = \"set-window-height\"\n"
        "delta = \"0.25\"\n"
        "[[bind]]\n"
        "key = \"volume-mute\"\n"
        "spawn = [\"kajo\", \"--volume-mute\"]\n"
        "allow_locked = true\n"
        "[[bind]]\n"
        "key = \"alt+tab\"\n"
        "action = \"next-window\"\n"
        "scope = \"output\"\n"
        "[[bind]]\n"
        "key = \"alt+grave\"\n"
        "action = \"next-window\"\n"
        "filter = \"app-id\"\n"
        "[[bind]]\n"
        "key = \"ctrl+print\"\n"
        "spawn = [\"kajo-capture\", \"--screen\"]\n",
        &d);
    dump_diag(&d);
    g_assert_nonnull(b);
    g_assert_cmpuint(d.n, ==, 0);
    g_assert_cmpuint(comp_binds_count(b), ==, 12);

    const CompBind *x = comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_h);
    g_assert_nonnull(x);
    g_assert_cmpint(x->kind, ==, COMP_BIND_LAYOUT);
    g_assert_cmpint(x->layout_action, ==, COMP_ACT_FOCUS_COLUMN_LEFT);
    g_assert_true(x->repeat);

    x = comp_binds_lookup_key(b, COMP_MOD_SUPER, XKB_KEY_Left);
    g_assert_nonnull(x);
    g_assert_cmpint(x->layout_action, ==, COMP_ACT_FOCUS_COLUMN_LEFT);

    /* Upper-case keysym from the client side still matches (lower-cased) */
    x = comp_binds_lookup_key(b, COMP_MOD_SUPER | COMP_MOD_SHIFT, XKB_KEY_L);
    g_assert_nonnull(x);
    g_assert_cmpint(x->layout_action, ==, COMP_ACT_MOVE_COLUMN_RIGHT);
    g_assert_cmpstr(x->spec, ==, "super+shift+l");
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
    g_assert_nonnull(x);
    g_assert_true(x->allow_locked);

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
    const char *t = "[[bind]]\nkey = \"mod+j\"\naction = \"focus-window-down\"\n";
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
        "[[bind]]\nkey = \"mod+h\"\naction = \"focus-colum-left\"\n"                  /* typo */
        "[[bind]]\nkey = \"mod+d\"\naction = \"toggle-overview\"\n"                   /* planned */
        "[[bind]]\nkey = \"hyper+x\"\naction = \"close-window\"\n"                    /* bad modifier */
        "[[bind]]\nkey = \"mod+nosuchkey\"\naction = \"close-window\"\n"              /* bad key */
        "[[bind]]\nkey = \"mod+shift+mod+x\"\naction = \"close-window\"\n"            /* modifier twice */
        "[[bind]]\nkey = \"mod+\"\naction = \"close-window\"\n"                       /* no key */
        "[[bind]]\nkey = \"mod+t\"\nspawn = \"kitty\"\n"                              /* spawn as string */
        "[[bind]]\nkey = \"mod+y\"\nspawn = []\n"                                     /* empty spawn */
        "[[bind]]\nkey = \"mod+u\"\naction = \"close-window\"\nspawn = [\"x\"]\n"     /* both */
        "[[bind]]\nkey = \"mod+i\"\naction = \"set-column-width\"\n"                  /* no delta */
        "[[bind]]\nkey = \"mod+o\"\naction = \"set-column-width\"\ndelta = \"lots\"\n"/* bad delta */
        "[[bind]]\naction = \"close-window\"\n"                                       /* missing key/keys */
        "[[bind]]\nkey = \"super+b\"\naction = \"close-window\"\n"                    /* ok */
        "[[bind]]\nkey = \"mod+b\"\naction = \"maximize-column\"\n"                   /* duplicate */
        "[[bind]]\nkey = \"mod+c\"\naction = \"center-column\"\ncooldown_ms = 99999\ncolour = 1\n" /* clamp + unknown */
        "[[bind]]\nkey = \"mod+e\"\naction = \"close-window\"\ndelta = \"5%\"\nscope = \"output\"\n", /* ignored opts */
        &d);
    dump_diag(&d);
    g_assert_nonnull(b);
    g_assert_true(comp_diag_contains(&d, "'mod+h': unknown action 'focus-colum-left'"));
    g_assert_true(comp_diag_contains(&d, "'mod+d': action 'toggle-overview' is planned but not available in this version"));
    g_assert_true(comp_diag_contains(&d, "'hyper+x': unknown modifier"));
    g_assert_true(comp_diag_contains(&d, "'mod+nosuchkey': unknown key name"));
    g_assert_true(comp_diag_contains(&d, "'mod+shift+mod+x': modifier given twice"));
    g_assert_true(comp_diag_contains(&d, "'mod+': missing key"));
    g_assert_true(comp_diag_contains(&d, "'mod+t': spawn takes an array"));
    g_assert_true(comp_diag_contains(&d, "'mod+y': spawn must be a non-empty array"));
    g_assert_true(comp_diag_contains(&d, "'mod+u': give exactly one of 'action' or 'spawn'"));
    g_assert_true(comp_diag_contains(&d, "'mod+i': 'delta' must be"));
    g_assert_true(comp_diag_contains(&d, "'mod+o': 'delta' must be"));
    g_assert_true(comp_diag_contains(&d, "[[bind]] requires exactly one of 'key' or 'keys'"));
    g_assert_true(comp_diag_contains(&d, "'mod+b' is the same key as line"));
    g_assert_true(comp_diag_contains(&d, "'mod+c': cooldown_ms 99999 out of range"));
    g_assert_true(comp_diag_contains(&d, "'mod+c': unknown option 'colour' ignored"));
    g_assert_true(comp_diag_contains(&d, "'mod+e': 'delta' only applies"));
    g_assert_true(comp_diag_contains(&d, "'mod+e': 'scope' only applies"));

    /* Survivors: super+b (close-window), mod+c (center-column, clamped), mod+e (close-window) */
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
    g_assert_null(parse_binds("[[bind\nkey = \"mod+h\"\n", &d));
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

/* The binds.toml example in the spec (§5.2.2) must parse without any message, so the
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
        "app_id = ['^org\\.gnome\\.calculator$', '^galculator$']\n"
        "floating = true\n"
        "[[rule]]\n"
        "app_id = ['^steam$']\n"
        "title  = ['^notificationtoasts_\\d+_desktop$']\n"
        "floating = true\n"
        "anchor_corner = \"bottom-right\"\n"
        "anchor_x = 10\n"
        "anchor_y = 10\n"
        "focus = false\n"
        "[[rule]]\n"
        "title = ['brave', 'chromium']\n"
        "scroll_factor = 0.15\n"
        "[[rule]]\n"
        "app_id = ['^galculator$']\n"
        "floating = false\n"
        "width = 0.5\n"
        "[[rule]]\n"
        "app_id = ['^org\\.kajo\\.player$', '^steam_app_\\d+$']\n"
        "vrr = true\n",
        &d);
    dump_diag(&d);
    g_assert_nonnull(r);
    g_assert_cmpuint(d.n, ==, 0);
    g_assert_cmpuint(comp_rules_window_count(r), ==, 5);

    CompWindowRuleResult m;
    /* Case-insensitive by default: '^org\.gnome\.calculator$' matches 'org.gnome.Calculator' */
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

    /* Unanchored case-insensitive substring match on the title */
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
        "app_id = ['([unclosed']\n"                           /* 3-4 bad regex */
        "floating = true\n"
        "[[rule]]\n"
        "app_id = 'not-an-array'\n"                           /* 6-7 bare string rejected */
        "floating = true\n"
        "[[rule]]\n"
        "app_id = ['ok']\n"
        "width = 2.0\n"                                       /* out of range */
        "floating = \"yes\"\n"                                /* wrong type */
        "anchor_corner = \"middle\"\n"                        /* bad corner */
        "colour = 3\n"                                        /* unknown key */
        "[[layer_rule]]\n"
        "namespace = ['^kajo$']\n"
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
    g_assert_true(comp_diag_contains(&d, "'app_id' must be a non-empty array of regex strings"));
    g_assert_true(comp_diag_contains(&d, "'width' = 2 is outside 0.05..1; ignored"));
    g_assert_true(comp_diag_contains(&d, "'floating' must be true or false"));
    g_assert_true(comp_diag_contains(&d, "anchor_corner must be"));
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

/* The rules.toml example in the spec (§5.2.3) must parse cleanly. */
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

static void
test_config_shipped_defaults(void)
{
    const gchar *root = g_getenv("KAJO_SOURCE_ROOT");
    if (!root)
        root = KAJO_SOURCE_DIR;
    g_autofree gchar *defaults_dir = g_build_filename(root, "data", "defaults", NULL);

    CompDiagList d;
    comp_diag_init(&d);
    CompConfig *cfg = comp_config_load_dir(NULL, defaults_dir, NULL, COMP_MOD_SUPER, &d);
    dump_diag(&d);
    g_assert_nonnull(cfg);
    g_assert_cmpuint(d.n, ==, 0);

    /* Verify shipped defaults match compiled C defaults field-for-field */
    CompConfig *def = comp_config_new_defaults(COMP_MOD_SUPER);
    g_assert_nonnull(def);
    g_assert_cmpfloat_with_epsilon(cfg->layout.gap, def->layout.gap, 1e-9);
    g_assert_cmpfloat_with_epsilon(cfg->layout.default_width, def->layout.default_width, 1e-9);
    g_assert_cmpint(cfg->border_px, ==, def->border_px);
    for (int i = 0; i < 4; i++) {
        g_assert_cmpfloat_with_epsilon(cfg->active_border_rgba[i], def->active_border_rgba[i], 1e-4f);
        g_assert_cmpfloat_with_epsilon(cfg->inactive_border_rgba[i], def->inactive_border_rgba[i], 1e-4f);
        g_assert_cmpfloat_with_epsilon(cfg->urgent_border_rgba[i], def->urgent_border_rgba[i], 1e-4f);
        g_assert_cmpfloat_with_epsilon(cfg->switcher_active_rgba[i], def->switcher_active_rgba[i], 1e-4f);
        g_assert_cmpfloat_with_epsilon(cfg->switcher_urgent_rgba[i], def->switcher_urgent_rgba[i], 1e-4f);
    }
    g_assert_cmpint(cfg->motion.enabled, ==, def->motion.enabled);
    g_assert_cmpfloat_with_epsilon(cfg->motion.horizontal_scroll.damping_ratio,
                                   def->motion.horizontal_scroll.damping_ratio, 1e-9);
    g_assert_cmpfloat_with_epsilon(cfg->motion.horizontal_scroll.stiffness,
                                   def->motion.horizontal_scroll.stiffness, 1e-9);
    g_assert_cmpstr(cfg->screenshot_path, ==, def->screenshot_path);
    g_assert_cmpint(cfg->screenshot_copy, ==, def->screenshot_copy);
    g_assert_cmpint(cfg->screenshot_notify, ==, def->screenshot_notify);
    g_assert_cmpint(cfg->screenshot_border, ==, def->screenshot_border);
    g_assert_cmpint(cfg->xwayland_enabled, ==, def->xwayland_enabled);
    g_assert_cmpstr(cfg->xkb_layout, ==, def->xkb_layout);
    g_assert_cmpint(cfg->repeat_rate, ==, def->repeat_rate);
    g_assert_cmpint(cfg->repeat_delay, ==, def->repeat_delay);
    g_assert_cmpint(cfg->touchpad_tap, ==, def->touchpad_tap);
    g_assert_cmpint(cfg->touchpad_natural_scroll, ==, def->touchpad_natural_scroll);
    g_assert_cmpint(cfg->mouse_accel_profile, ==, def->mouse_accel_profile);
    g_assert_cmpint(cfg->touch_enabled, ==, def->touch_enabled);
    g_assert_cmpint(cfg->touch_dwell_ms, ==, def->touch_dwell_ms);
    g_assert_cmpint(cfg->touch_swipe_center, ==, def->touch_swipe_center);
    g_assert_cmpint(cfg->touch_swipe_right, ==, def->touch_swipe_right);
    g_assert_cmpint(cfg->touch_swipe_horizontal, ==, def->touch_swipe_horizontal);
    comp_config_free(def);

    g_assert_cmpfloat_with_epsilon(cfg->layout.gap, 6.0, 1e-9);
    g_assert_cmpint(cfg->border_px, ==, 1);
    g_assert_true(cfg->motion.enabled);
    g_assert_true(cfg->xwayland_enabled);
    g_assert_true(cfg->touch_enabled);
    g_assert_cmpint(cfg->touch_dwell_ms, ==, 4000);
    g_assert_cmpint(cfg->mouse_accel_profile, ==, COMP_ACCEL_FLAT);
    g_assert_cmpint(cfg->touch_swipe_center, ==, COMP_SWIPE_LAUNCHER);
    g_assert_cmpint(cfg->touch_swipe_right, ==, COMP_SWIPE_CONTROL_CENTER);
    g_assert_cmpint(cfg->touch_swipe_horizontal, ==, COMP_SWIPE_COLUMN);

    /* Verify enum <-> string helpers round-trip */
    CompTransform tf = COMP_TRANSFORM_NORMAL;
    g_assert_true(comp_transform_from_str("flipped-270", &tf));
    g_assert_cmpint(tf, ==, COMP_TRANSFORM_FLIPPED_270);
    g_assert_cmpstr(comp_transform_to_str(tf), ==, "flipped-270");
    CompVrrPolicy vrr = COMP_VRR_OFF;
    g_assert_true(comp_vrr_policy_from_str("on-demand", &vrr));
    g_assert_cmpint(vrr, ==, COMP_VRR_ON_DEMAND);
    g_assert_cmpstr(comp_vrr_policy_to_str(vrr), ==, "on-demand");
    CompAccelProfile ap = COMP_ACCEL_FLAT;
    g_assert_true(comp_accel_profile_from_str("adaptive", &ap));
    g_assert_cmpint(ap, ==, COMP_ACCEL_ADAPTIVE);
    g_assert_cmpstr(comp_accel_profile_to_str(ap), ==, "adaptive");
    CompTouchSwipeAction sw = COMP_SWIPE_NONE;
    g_assert_true(comp_touch_swipe_from_str("control-center", &sw));
    g_assert_cmpint(sw, ==, COMP_SWIPE_CONTROL_CENTER);
    g_assert_cmpstr(comp_touch_swipe_to_str(sw), ==, "control-center");

    const CompBind *preset_h = comp_binds_lookup_key(cfg->binds, COMP_MOD_SUPER | COMP_MOD_SHIFT, XKB_KEY_r);
    g_assert_nonnull(preset_h);
    g_assert_cmpint(preset_h->kind, ==, COMP_BIND_LAYOUT);
    g_assert_cmpint(preset_h->layout_action, ==, COMP_ACT_SWITCH_PRESET_WINDOW_HEIGHT);

    const CompBind *brk = comp_binds_lookup_key(cfg->binds, COMP_MOD_SUPER, XKB_KEY_Escape);
    g_assert_nonnull(brk);
    g_assert_cmpint(brk->kind, ==, COMP_BIND_BREAK_SHORTCUTS_INHIBIT);

    CompWindowRuleResult wr;
    comp_rules_match_window(cfg->rules, "org.gnome.Calculator", "Calculator", &wr);
    g_assert_true(wr.has_floating && wr.floating);
    comp_rules_match_window(cfg->rules, "xdg-desktop-portal-gtk", "Open File", &wr);
    g_assert_true(wr.has_floating && wr.floating);
    comp_rules_match_window(cfg->rules, "org.freedesktop.impl.portal.desktop.gtk", "Select Folder", &wr);
    g_assert_true(wr.has_floating && wr.floating);

    CompLayerRuleResult lr;
    comp_rules_match_layer(cfg->rules, "kajo", &lr);
    g_assert_true(lr.has_opacity);
    g_assert_cmpfloat_with_epsilon(lr.opacity, 0.85, 1e-9);

    comp_config_free(cfg);
    comp_diag_clear(&d);
}

static void
test_config_multifile_merge_and_confd(void)
{
    const gchar *root = g_getenv("KAJO_SOURCE_ROOT");
    if (!root)
        root = KAJO_SOURCE_DIR;
    g_autofree gchar *defaults_dir = g_build_filename(root, "data", "defaults", NULL);

    g_autofree gchar *tmp = g_dir_make_tmp("kajo-comp-cfg-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree gchar *confd = g_build_filename(tmp, "conf.d", NULL);
    g_autofree gchar *extra = g_build_filename(tmp, "extra", NULL);
    g_assert_cmpint(g_mkdir_with_parents(confd, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(extra, 0700), ==, 0);

    g_autofree gchar *p_theme = g_build_filename(tmp, "theme.toml", NULL);
    g_autofree gchar *p_shell = g_build_filename(tmp, "shell.toml", NULL);
    g_autofree gchar *p_comp  = g_build_filename(tmp, "comp.toml", NULL);
    g_autofree gchar *p_out   = g_build_filename(tmp, "outputs.toml", NULL);
    g_autofree gchar *p_binds = g_build_filename(tmp, "binds.toml", NULL);
    g_autofree gchar *p_rules = g_build_filename(tmp, "rules.toml", NULL);
    g_autofree gchar *p_extra = g_build_filename(extra, "custom.toml", NULL);
    g_autofree gchar *p_d10   = g_build_filename(confd, "10-broken.toml", NULL);
    g_autofree gchar *p_d20   = g_build_filename(confd, "20-override.toml", NULL);

    g_assert_true(g_file_set_contents(p_theme,
        "accent_color = \"#008a00\"\n", -1, NULL));
    g_assert_true(g_file_set_contents(p_shell,
        "[touch]\n"
        "dwell_ms = 750\n", -1, NULL));
    g_assert_true(g_file_set_contents(p_comp,
        "include = [\"extra/*.toml\"]\n"
        "[layout]\n"
        "gap_px = 12\n"
        "[env]\n"
        "FOO = \"bar\"\n", -1, NULL));
    g_assert_true(g_file_set_contents(p_extra,
        "[input.keyboard]\n"
        "xkb_layout = \"us,cz\"\n", -1, NULL));
    g_assert_true(g_file_set_contents(p_out,
        "[[output]]\n"
        "match = \"DP-1\"\n"
        "scale = 1.5\n"
        "vrr = \"on-demand\"\n", -1, NULL));
    g_assert_true(g_file_set_contents(p_binds,
        "[[bind]]\n"
        "key = \"mod+q\"\n"
        "action = \"none\"\n", -1, NULL));
    g_assert_true(g_file_set_contents(p_rules,
        "reset_rules = true\n"
        "[[rule]]\n"
        "app_id = ['^my-custom-app$']\n"
        "floating = true\n", -1, NULL));
    /* Fault isolation: syntax error in one drop-in must skip only that file */
    g_assert_true(g_file_set_contents(p_d10,
        "[layout\n"
        "gap_px = 99\n", -1, NULL));
    g_assert_true(g_file_set_contents(p_d20,
        "[theme]\n"
        "accent_color = \"#e81123\"\n"
        "[shell.touch]\n"
        "dwell_ms = 1200\n"
        "[comp.layout]\n"
        "gap_px = 18\n"
        "[[comp.output]]\n"
        "match = \"HDMI-A-2\"\n"
        "scale = 2.0\n"
        "[[comp.bind]]\n"
        "key = \"mod+b\"\n"
        "action = \"toggle-window-floating\"\n"
        "[[comp.rule]]\n"
        "app_id = ['^scoped-app$']\n"
        "floating = true\n"
        "[[comp.layer_rule]]\n"
        "namespace = [\"^kajo-scoped$\"]\n"
        "opacity = 0.77\n", -1, NULL));

    CompDiagList d;
    comp_diag_init(&d);
    CompConfig *cfg = comp_config_load_dir(tmp, defaults_dir, NULL, COMP_MOD_SUPER, &d);
    dump_diag(&d);
    g_assert_nonnull(cfg);
    g_assert_true(comp_diag_contains(&d, "10-broken.toml"));

    /* conf.d/20-override.toml [theme] accent_color = "#e81123" overrode theme.toml */
    g_assert_cmpfloat_with_epsilon(cfg->active_border_rgba[0], (float)(0xe8 / 255.0), 1e-2f);
    /* conf.d/20-override.toml [shell.touch].dwell_ms overrode shell.toml */
    g_assert_cmpint(cfg->touch_dwell_ms, ==, 1200);
    /* extra/custom.toml via include */
    g_assert_cmpstr(cfg->xkb_layout, ==, "us,cz");
    /* conf.d/20-override.toml [comp.layout] overrode comp.toml gap_px */
    g_assert_cmpfloat_with_epsilon(cfg->layout.gap, 18.0, 1e-9);
    /* outputs.toml + [[comp.output]] in conf.d */
    const CompOutputRule *orule = comp_config_match_output(cfg, "dp-1", NULL);
    g_assert_nonnull(orule);
    g_assert_true(orule->has_scale);
    g_assert_cmpfloat_with_epsilon(orule->scale, 1.5, 1e-9);
    g_assert_cmpint(orule->vrr, ==, COMP_VRR_ON_DEMAND);
    const CompOutputRule *orule2 = comp_config_match_output(cfg, "hdmi-a-2", NULL);
    g_assert_nonnull(orule2);
    g_assert_cmpfloat_with_epsilon(orule2->scale, 2.0, 1e-9);
    /* binds.toml unbound mod+q while keeping mod+t, and [[comp.bind]] bound mod+b */
    g_assert_null(comp_binds_lookup_key(cfg->binds, COMP_MOD_SUPER, XKB_KEY_q));
    g_assert_nonnull(comp_binds_lookup_key(cfg->binds, COMP_MOD_SUPER, XKB_KEY_t));
    g_assert_nonnull(comp_binds_lookup_key(cfg->binds, COMP_MOD_SUPER, XKB_KEY_b));
    /* rules.toml reset_rules = true wiped default calculator rule and added my-custom-app + [[comp.rule]] */
    CompWindowRuleResult wr;
    comp_rules_match_window(cfg->rules, "org.gnome.Calculator", "Calculator", &wr);
    g_assert_false(wr.has_floating);
    comp_rules_match_window(cfg->rules, "my-custom-app", "Main", &wr);
    g_assert_true(wr.has_floating && wr.floating);
    comp_rules_match_window(cfg->rules, "scoped-app", "Main", &wr);
    g_assert_true(wr.has_floating && wr.floating);
    CompLayerRuleResult lr;
    comp_rules_match_layer(cfg->rules, "kajo-scoped", &lr);
    g_assert_true(lr.has_opacity);
    g_assert_cmpfloat_with_epsilon(lr.opacity, 0.77, 1e-9);

    comp_config_free(cfg);
    comp_diag_clear(&d);

    /* Syntax error in base comp.toml must reject reload (return NULL) */
    g_assert_true(g_file_set_contents(p_comp, "[layout\n", -1, NULL));
    comp_diag_init(&d);
    g_assert_null(comp_config_load_dir(tmp, defaults_dir, NULL, COMP_MOD_SUPER, &d));
    comp_diag_clear(&d);

    g_unlink(p_d20);
    g_unlink(p_d10);
    g_unlink(p_extra);
    g_unlink(p_rules);
    g_unlink(p_binds);
    g_unlink(p_out);
    g_unlink(p_comp);
    g_unlink(p_shell);
    g_unlink(p_theme);
    g_rmdir(confd);
    g_rmdir(extra);
    g_rmdir(tmp);
}

static void
test_touch_gestures_and_transforms(void)
{
    CompTouchTracker t;
    comp_touch_tracker_init(&t, 300.0, 200.0);

    /* 1. 1-finger bottom bezel swipe (y_start >= 1 - 0.032 = 0.968, dy <= -12mm -> -0.06) */
    comp_touch_tracker_down(&t, 1, 0.72, 0.99);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 1, 0.72, 0.90), ==, COMP_TOUCH_EV_BEZEL_BOTTOM);
    g_assert_cmpfloat_with_epsilon(t.last_x_ratio, 0.72, 1e-4);
    /* Latched until all fingers release */
    g_assert_cmpint(comp_touch_tracker_motion(&t, 1, 0.72, 0.80), ==, COMP_TOUCH_EV_NONE);
    comp_touch_tracker_up(&t, 1);

    /* 2. 1-finger top bezel swipe (y_start <= 0.032, dy >= +12mm -> +0.06) */
    comp_touch_tracker_down(&t, 2, 0.25, 0.01);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 2, 0.25, 0.10), ==, COMP_TOUCH_EV_BEZEL_TOP);
    g_assert_cmpfloat_with_epsilon(t.last_x_ratio, 0.25, 1e-4);
    comp_touch_tracker_up(&t, 2);

    /* 3. Multi-finger re-anchoring; 3 fingers are a drag (comp_gesture), never a discrete event */
    comp_touch_tracker_down(&t, 10, 0.30, 0.60);
    comp_touch_tracker_motion(&t, 10, 0.30, 0.50); /* moves before 2nd/3rd finger land */
    comp_touch_tracker_down(&t, 11, 0.50, 0.50);
    comp_touch_tracker_down(&t, 12, 0.70, 0.50);   /* re-anchors all 3 slots at y = 0.50 */
    g_assert_cmpint(comp_touch_tracker_motion(&t, 10, 0.30, 0.40), ==, COMP_TOUCH_EV_NONE);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 11, 0.50, 0.40), ==, COMP_TOUCH_EV_NONE);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 12, 0.70, 0.40), ==, COMP_TOUCH_EV_NONE);
    comp_touch_tracker_up(&t, 10);
    comp_touch_tracker_up(&t, 11);
    comp_touch_tracker_up(&t, 12);

    /* 4. 90-degree rotated posture: raw X=0.99 is rotated Y=0.99 (bottom bezel), rotated X = 1 - raw_y = 0.60 */
    comp_touch_tracker_set_geometry(&t, 300.0, 200.0, COMP_TRANSFORM_90);
    comp_touch_tracker_down(&t, 20, 0.99, 0.40);
    g_assert_cmpint(comp_touch_tracker_motion(&t, 20, 0.90, 0.40), ==, COMP_TOUCH_EV_BEZEL_BOTTOM);
    g_assert_cmpfloat_with_epsilon(t.last_x_ratio, 0.60, 1e-4);
    comp_touch_tracker_up(&t, 20);
}


static void
test_greeter_config_filtering(void)
{
    gchar *tmp = g_dir_make_tmp("kajo-greeter-cfg-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    gchar *p_cfg = g_build_filename(tmp, "comp-greeter.toml", NULL);

    g_assert_true(g_file_set_contents(p_cfg,
        "[input.keyboard]\n"
        "xkb_layout = \"fi\"\n"
        "[env]\n"
        "FORBIDDEN_VAR = \"1\"\n"
        "[[bind]]\n"
        "key = \"mod+q\"\n"
        "action = \"close-window\"\n"
        "[[output]]\n"
        "match = \"edp-1\"\n"
        "scale = 2.0\n", -1, NULL));

    CompDiagList d;
    comp_diag_init(&d);
    CompConfig *cfg = comp_config_load_greeter(p_cfg, COMP_MOD_SUPER, &d);
    g_assert_nonnull(cfg);
    g_assert_false(cfg->xwayland_enabled);
    g_assert_cmpstr(cfg->xkb_layout, ==, "fi");
    /* Forbidden sections ([env], [[bind]]) must be flagged and ignored */
    g_assert_true(comp_diag_contains(&d, "not permitted in --greeter-mode"));
    g_assert_cmpuint(cfg->n_env, ==, 0);
    g_assert_cmpuint(comp_binds_count(cfg->binds), ==, 0);
    const CompOutputRule *orule = comp_config_match_output(cfg, "eDP-1", NULL);
    g_assert_nonnull(orule);
    g_assert_cmpfloat_with_epsilon(orule->scale, 2.0, 1e-9);
    comp_config_free(cfg);
    comp_diag_clear(&d);

    /* Unreadable explicit --config path in greeter mode returns NULL */
    comp_diag_init(&d);
    gchar *p_missing = g_build_filename(tmp, "nonexistent.toml", NULL);
    g_assert_null(comp_config_load_greeter(p_missing, COMP_MOD_SUPER, &d));
    comp_diag_clear(&d);

    g_free(p_missing);
    g_unlink(p_cfg);
    g_free(p_cfg);
    g_rmdir(tmp);
    g_free(tmp);
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
    g_test_add_func("/comp/config/shipped-defaults", test_config_shipped_defaults);
    g_test_add_func("/comp/config/multifile-merge-and-confd", test_config_multifile_merge_and_confd);
    g_test_add_func("/comp/config/greeter-filtering", test_greeter_config_filtering);
    g_test_add_func("/comp/touch/gestures-and-transforms", test_touch_gestures_and_transforms);
    return g_test_run();
}
