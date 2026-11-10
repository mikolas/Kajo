/* kajo-comp — Kajo Wayland compositor (in development; docs/spec_kajo_comp.md §8).
 *
 * Phase 2a: server skeleton. Runs bare metal, nested (WLR_BACKENDS=wayland)
 * or headless (WLR_BACKENDS=headless).
 */
#include "comp_server.h"

#include "core/comp_binds.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

static void
usage(FILE *f, const char *argv0)
{
    fprintf(f,
            "Usage: %s [options] [-- COMMAND [ARGS...]]\n"
            "\n"
            "  -v, --verbose          debug logging\n"
            "  -q, --quiet            errors only\n"
            "      --mod alt|super    what \"Mod\" means in binds.toml (default super)\n"
            "      --exit-with-command  exit when COMMAND exits, with its exit status\n"
            "  -h, --help             this help\n"
            "\n"
            "COMMAND is started once the Wayland socket exists, without a shell.\n"
            "Backends follow WLR_BACKENDS (e.g. 'wayland' for a nested window, 'headless').\n",
            argv0);
}

int
main(int argc, char *argv[])
{
    enum wlr_log_importance level = WLR_INFO;
    CompServerOptions opts = { .mod_mask = COMP_MOD_SUPER };
    static const struct option long_opts[] = {
        { "verbose", no_argument, NULL, 'v' },
        { "quiet", no_argument, NULL, 'q' },
        { "mod", required_argument, NULL, 'm' },
        { "exit-with-command", no_argument, NULL, 'x' },
        { "help", no_argument, NULL, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "+vqh", long_opts, NULL)) != -1) {
        switch (c) {
        case 'v': level = WLR_DEBUG; break;
        case 'q': level = WLR_ERROR; break;
        case 'x': opts.exit_with_startup = true; break;
        case 'm':
            if (strcmp(optarg, "alt") == 0)
                opts.mod_mask = COMP_MOD_ALT;
            else if (strcmp(optarg, "super") == 0)
                opts.mod_mask = COMP_MOD_SUPER;
            else {
                fprintf(stderr, "--mod must be 'alt' or 'super'\n");
                return 2;
            }
            break;
        case 'h': usage(stdout, argv[0]); return 0;
        default: usage(stderr, argv[0]); return 2;
        }
    }
    if (optind < argc)
        opts.startup_argv = (const char *const *)&argv[optind];
    if (opts.exit_with_startup && !opts.startup_argv) {
        fprintf(stderr, "--exit-with-command needs a COMMAND after --\n");
        return 2;
    }

    /* Writing to a disconnected client must never kill the compositor (spec §7.2.1) */
    signal(SIGPIPE, SIG_IGN);
    wlr_log_init(level, NULL);

    CompServer *server = comp_server_create(&opts);
    if (!server)
        return 1;
    int status = 1;
    if (comp_server_start(server)) {
        comp_server_run(server);
        status = server->exit_status;
    }
    comp_server_destroy(server);
    return status;
}
