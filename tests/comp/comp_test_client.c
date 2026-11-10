/* comp_test_client — minimal Wayland client used by the kajo-comp tests.
 *
 * Usage: comp-test-client globals IFACE...   exit 0 iff every IFACE is advertised
 *        comp-test-client churn N            N sequential connect/roundtrip/disconnect cycles
 * Exit status: 0 ok, 1 missing global / protocol error, 2 usage, 3 cannot connect.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

typedef struct {
    char **want;
    int    n_want;
    int   *seen;
} GlobalsCheck;

static void
on_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    (void)reg; (void)name; (void)version;
    GlobalsCheck *c = data;
    for (int i = 0; i < c->n_want; i++)
        if (strcmp(c->want[i], iface) == 0)
            c->seen[i] = 1;
}

static void
on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener k_registry = { on_global, on_global_remove };

static int
check_globals(char **want, int n)
{
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) {
        fprintf(stderr, "comp-test-client: cannot connect\n");
        return 3;
    }
    GlobalsCheck c = { want, n, calloc((size_t)n, sizeof(int)) };
    if (!c.seen) {
        wl_display_disconnect(d);
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &k_registry, &c);
    int rc = wl_display_roundtrip(d) < 0 ? 1 : 0;
    for (int i = 0; i < n && rc == 0; i++) {
        if (!c.seen[i]) {
            fprintf(stderr, "comp-test-client: missing global %s\n", want[i]);
            rc = 1;
        }
    }
    wl_registry_destroy(reg);
    wl_display_disconnect(d);
    free(c.seen);
    return rc;
}

static int
churn(int n)
{
    for (int i = 0; i < n; i++) {
        struct wl_display *d = wl_display_connect(NULL);
        if (!d)
            return 3;
        struct wl_registry *reg = wl_display_get_registry(d);
        int rc = wl_display_roundtrip(d);
        wl_registry_destroy(reg);
        wl_display_disconnect(d);
        if (rc < 0)
            return 1;
    }
    return 0;
}

int
main(int argc, char *argv[])
{
    if (argc >= 3 && strcmp(argv[1], "globals") == 0)
        return check_globals(&argv[2], argc - 2);
    if (argc == 3 && strcmp(argv[1], "churn") == 0)
        return churn(atoi(argv[2]));
    fprintf(stderr, "usage: %s globals IFACE... | churn N\n", argv[0]);
    return 2;
}
