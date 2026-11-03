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

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/osk-xkb/finnish-layout", test_finnish_layout);
    g_test_add_func("/osk-xkb/us-and-multigroup-layout", test_us_and_multigroup_layout);

    return g_test_run();
}
