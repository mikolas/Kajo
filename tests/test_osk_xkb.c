#include "../src/osk/osk_xkb.h"
#include <glib.h>

static void
test_finnish_layout(void)
{
    ShellOskXkb *xkb = shell_osk_xkb_new();
    g_assert_nonnull(xkb);
    g_assert_true(shell_osk_xkb_load_from_names(xkb, "fi", NULL));

    char group_lbl[8] = {0};
    shell_osk_xkb_get_short_group_label(xkb, group_lbl, sizeof(group_lbl));
    g_assert_cmpstr(group_lbl, ==, "FI");

    char buf[16] = {0};
    /* KEY_LEFTBRACE (26) -> å / Å */
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 26, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "å");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 26, TRUE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "Å");

    /* KEY_SEMICOLON (39) -> ö / Ö */
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 39, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "ö");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 39, FALSE, TRUE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "Ö");

    /* KEY_APOSTROPHE (40) -> ä / Ä */
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 40, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "ä");

    /* Reverse symbol lookup on Finnish layout: @ € { [ ] } \ | ~ */
    const char *symbols[] = { "@", "€", "$", "{", "[", "]", "}", "\\", "|", "~" };
    for (size_t i = 0; i < G_N_ELEMENTS(symbols); i++) {
        ShellOskSymbolBinding b = shell_osk_xkb_find_symbol(xkb, symbols[i]);
        g_assert_true(b.found);
        g_assert_cmpuint(b.evdev_code, >, 0);
    }

    shell_osk_xkb_free(xkb);
}

static void
test_us_and_multigroup_layout(void)
{
    ShellOskXkb *xkb = shell_osk_xkb_new();
    g_assert_nonnull(xkb);
    g_assert_true(shell_osk_xkb_load_from_names(xkb, "fi,us", NULL));
    g_assert_cmpuint(shell_osk_xkb_get_num_groups(xkb), ==, 2);

    char buf[16] = {0};
    shell_osk_xkb_set_group(xkb, 0);
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 26, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "å");

    /* Switch to group 1 (US) */
    shell_osk_xkb_set_group(xkb, 1);
    char group_lbl[8] = {0};
    shell_osk_xkb_get_short_group_label(xkb, group_lbl, sizeof(group_lbl));
    g_assert_cmpstr(group_lbl, ==, "US");

    g_assert_true(shell_osk_xkb_get_key_label(xkb, 26, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "[");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 26, TRUE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "{");

    g_assert_true(shell_osk_xkb_get_key_label(xkb, 39, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, ";");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 40, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "'");

    shell_osk_xkb_free(xkb);
}

static void
test_multilang_adaptive_rows(void)
{
    ShellOskXkb *xkb = shell_osk_xkb_new();
    g_assert_nonnull(xkb);
    g_assert_true(shell_osk_xkb_load_from_names(xkb, "fi,us,ru", NULL));
    g_assert_cmpuint(shell_osk_xkb_get_num_groups(xkb), ==, 3);

    char group_lbl[8] = {0};
    ShellOskAlphaLayout layout;

    /* Group 0: Finnish -> (11, 11, 12, 10) */
    shell_osk_xkb_set_group(xkb, 0);
    shell_osk_xkb_get_short_group_label(xkb, group_lbl, sizeof(group_lbl));
    g_assert_cmpstr(group_lbl, ==, "FI");
    shell_osk_xkb_compute_alpha_layout(xkb, &layout);
    g_assert_cmpuint(layout.rows[0].count, ==, 11);
    g_assert_cmpuint(layout.rows[1].count, ==, 11);
    g_assert_cmpuint(layout.rows[2].count, ==, 12);
    g_assert_cmpuint(layout.rows[3].count, ==, 10);
    g_assert_cmpuint(layout.rows[0].evdev_codes[0], ==, 2);
    g_assert_cmpuint(layout.rows[1].evdev_codes[10], ==, 26);
    g_assert_cmpuint(layout.rows[2].evdev_codes[0], ==, 86);

    /* Group 1: English (US) -> (11, 10, 11, 10), no LSGT/AD11/AD12/TLDE */
    shell_osk_xkb_set_group(xkb, 1);
    shell_osk_xkb_get_short_group_label(xkb, group_lbl, sizeof(group_lbl));
    g_assert_cmpstr(group_lbl, ==, "US");
    shell_osk_xkb_compute_alpha_layout(xkb, &layout);
    g_assert_cmpuint(layout.rows[0].count, ==, 11);
    g_assert_cmpuint(layout.rows[1].count, ==, 10);
    g_assert_cmpuint(layout.rows[2].count, ==, 11);
    g_assert_cmpuint(layout.rows[3].count, ==, 10);
    g_assert_cmpuint(layout.rows[0].evdev_codes[0], ==, 2);
    g_assert_cmpuint(layout.rows[1].evdev_codes[9], ==, 25);
    g_assert_cmpuint(layout.rows[2].evdev_codes[0], ==, 30);

    /* Group 2: Russian -> (12, 12, 11, 10), includes TLDE(41=ё), AD11(26=х), AD12(27=ъ) */
    shell_osk_xkb_set_group(xkb, 2);
    shell_osk_xkb_get_short_group_label(xkb, group_lbl, sizeof(group_lbl));
    g_assert_cmpstr(group_lbl, ==, "RU");
    shell_osk_xkb_compute_alpha_layout(xkb, &layout);
    g_assert_cmpuint(layout.rows[0].count, ==, 12);
    g_assert_cmpuint(layout.rows[1].count, ==, 12);
    g_assert_cmpuint(layout.rows[2].count, ==, 11);
    g_assert_cmpuint(layout.rows[3].count, ==, 10);
    g_assert_cmpuint(layout.rows[0].evdev_codes[0], ==, 41);
    g_assert_cmpuint(layout.rows[1].evdev_codes[10], ==, 26);
    g_assert_cmpuint(layout.rows[1].evdev_codes[11], ==, 27);
    g_assert_cmpuint(layout.rows[2].evdev_codes[0], ==, 30);

    char buf[16] = {0};
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 41, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "ё");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 26, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "х");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 27, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "ъ");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 39, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "ж");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 40, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "э");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 51, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "б");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 52, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "ю");

    shell_osk_xkb_free(xkb);
}

static void
test_russian_cross_group_symbols(void)
{
    ShellOskXkb *xkb = shell_osk_xkb_new();
    g_assert_nonnull(xkb);
    g_assert_true(shell_osk_xkb_load_from_names(xkb, "fi,us,ru", NULL));
    shell_osk_xkb_set_group(xkb, 2);

    const char *symbols[] = {
        "@", "#", "€", "$", "%", "&", "{", "[", "]", "}",
        "\\", "|", "~", "^", "<", ">"
    };
    for (size_t i = 0; i < G_N_ELEMENTS(symbols); i++) {
        ShellOskSymbolBinding b = shell_osk_xkb_find_symbol(xkb, symbols[i]);
        g_assert_true(b.found);
        g_assert_cmpuint(b.evdev_code, >, 0);
        g_assert_cmpuint(b.group, <, 3);
    }

    shell_osk_xkb_free(xkb);
}

static void
test_german_adaptive_rows(void)
{
    ShellOskXkb *xkb = shell_osk_xkb_new();
    g_assert_nonnull(xkb);
    g_assert_true(shell_osk_xkb_load_from_names(xkb, "de", NULL));

    char group_lbl[8] = {0};
    shell_osk_xkb_get_short_group_label(xkb, group_lbl, sizeof(group_lbl));
    g_assert_cmpstr(group_lbl, ==, "DE");

    ShellOskAlphaLayout layout;
    shell_osk_xkb_compute_alpha_layout(xkb, &layout);
    g_assert_cmpuint(layout.rows[0].count, ==, 11);
    g_assert_cmpuint(layout.rows[1].count, ==, 11);
    g_assert_cmpuint(layout.rows[2].count, ==, 12);
    g_assert_cmpuint(layout.rows[3].count, ==, 10);

    char buf[16] = {0};
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 12, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "ß");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 26, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "ü");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 39, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "ö");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 40, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "ä");
    g_assert_true(shell_osk_xkb_get_key_label(xkb, 86, FALSE, FALSE, FALSE, buf, sizeof(buf)));
    g_assert_cmpstr(buf, ==, "<");

    shell_osk_xkb_free(xkb);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/osk-xkb/finnish-layout", test_finnish_layout);
    g_test_add_func("/osk-xkb/us-and-multigroup-layout", test_us_and_multigroup_layout);
    g_test_add_func("/osk-xkb/multilang-adaptive-rows", test_multilang_adaptive_rows);
    g_test_add_func("/osk-xkb/russian-cross-group-symbols", test_russian_cross_group_symbols);
    g_test_add_func("/osk-xkb/german-adaptive-rows", test_german_adaptive_rows);

    return g_test_run();
}
