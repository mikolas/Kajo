/* kajo-comp — Kajo Wayland compositor (docs/spec_kajo_comp.md §6.1 & §8).
 *
 * Runs bare metal, nested (WLR_BACKENDS=wayland) or headless (WLR_BACKENDS=headless),
 * and provides the `kajo-comp msg <subcommand>` IPC client.
 */
#include "comp_server.h"

#include "core/comp_binds.h"
#include "core/comp_schema_keys.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sysexits.h>
#include <unistd.h>
#include <wlr/util/log.h>

static void
usage(FILE *f, const char *argv0)
{
    fprintf(f,
            "Usage: %s [options] [-- COMMAND [ARGS...]]\n"
            "       %s msg <subcommand> [args...]\n"
            "\n"
            "      --session            run as primary login session compositor\n"
            "      --greeter-mode       run as locked-down pre-login greeter host\n"
            "  -c, --config PATH        override primary config file path\n"
            "  -v, --verbose            debug logging\n"
            "  -q, --quiet              errors only\n"
            "      --mod alt|super      what \"mod\" means in binds.toml (default super)\n"
            "      --exit-with-command  exit when COMMAND exits, with its exit status\n"
            "  -h, --help               this help\n"
            "\n"
            "IPC Subcommands:\n"
            "  %s msg get-outputs\n"
            "  %s msg get-workspaces\n"
            "  %s msg get-windows\n"
            "  %s msg get-keyboard-layouts\n"
            "  %s msg action <ACTION_NAME> [KEY=VALUE ...]\n"
            "  %s msg subscribe [EVENT ...]\n"
            "  %s msg debug-mem\n",
            argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static void
fprint_json_str(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp);  break;
        case '\r': fputs("\\r", fp);  break;
        case '\t': fputs("\\t", fp);  break;
        default:
            if (c < 0x20)
                fprintf(fp, "\\u%04x", (unsigned)c);
            else
                fputc(c, fp);
            break;
        }
    }
    fputc('"', fp);
}

static bool
is_numeric_key_val(const char *key, const char *val)
{
    if (strcmp(key, COMP_IPC_KEY_ID) != 0 &&
        strcmp(key, COMP_IPC_KEY_IDX) != 0 &&
        strcmp(key, COMP_KEY_SCALE) != 0)
        return false;
    if (!val || !*val)
        return false;
    char *end = NULL;
    errno = 0;
    double d = strtod(val, &end);
    return end != val && *end == '\0' && errno == 0 && isfinite(d);
}

static int
connect_unix_socket(const char *path)
{
    if (!path || !*path)
        return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int
connect_nested_fallback_socket(const char *dir, char *out_path, size_t out_cap)
{
    if (!dir || !*dir)
        return -1;
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    int fd = -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "kajo-comp-", 10) != 0)
            continue;
        size_t nlen = strlen(de->d_name);
        if (nlen <= 15 || strcmp(de->d_name + nlen - 5, ".sock") != 0)
            continue;
        char candidate[256];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", dir, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(candidate))
            continue;
        fd = connect_unix_socket(candidate);
        if (fd >= 0) {
            snprintf(out_path, out_cap, "%s", candidate);
            break;
        }
    }
    closedir(d);
    return fd;
}

static int
run_msg_cli(int argc, char *argv[])
{
    if (argc < 3 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
        usage(argc < 3 ? stderr : stdout, argv[0]);
        return argc < 3 ? 2 : 0;
    }

    const char *sub = argv[2];
    char *req_buf = NULL;
    size_t req_len = 0;
    FILE *req_fp = open_memstream(&req_buf, &req_len);
    if (!req_fp)
        return 1;

    bool is_subscribe = false;
    if (strcmp(sub, COMP_IPC_CMD_GET_OUTPUTS) == 0 ||
        strcmp(sub, COMP_IPC_CMD_GET_WORKSPACES) == 0 ||
        strcmp(sub, COMP_IPC_CMD_GET_WINDOWS) == 0 ||
        strcmp(sub, COMP_IPC_CMD_GET_KEYBOARD_LAYOUTS) == 0 ||
        strcmp(sub, COMP_IPC_CMD_DEBUG_MEM) == 0) {
        fprintf(req_fp, "{\"" COMP_IPC_KEY_CMD "\":");
        fprint_json_str(req_fp, sub);
        fprintf(req_fp, "}\n");
    } else if (strcmp(sub, COMP_IPC_CMD_SUBSCRIBE) == 0) {
        is_subscribe = true;
        fprintf(req_fp, "{\"" COMP_IPC_KEY_CMD "\":\"" COMP_IPC_CMD_SUBSCRIBE "\",\"" COMP_IPC_KEY_EVENTS "\":[");
        for (int i = 3; i < argc; i++) {
            if (i > 3)
                fputc(',', req_fp);
            fprint_json_str(req_fp, argv[i]);
        }
        fprintf(req_fp, "]}\n");
    } else if (strcmp(sub, COMP_IPC_CMD_ACTION) == 0) {
        if (argc < 4) {
            fclose(req_fp);
            free(req_buf);
            fprintf(stderr, "kajo-comp msg action requires an <ACTION_NAME>\n");
            return 2;
        }
        fprintf(req_fp, "{\"" COMP_IPC_KEY_CMD "\":\"" COMP_IPC_CMD_ACTION "\",\"" COMP_IPC_KEY_NAME "\":");
        fprint_json_str(req_fp, argv[3]);
        for (int i = 4; i < argc; i++) {
            const char *eq = strchr(argv[i], '=');
            if (!eq || eq == argv[i]) {
                fclose(req_fp);
                free(req_buf);
                fprintf(stderr, "invalid action argument '%s' (expected KEY=VALUE)\n", argv[i]);
                return 2;
            }
            size_t klen = (size_t)(eq - argv[i]);
            char key[128];
            if (klen >= sizeof(key))
                klen = sizeof(key) - 1;
            memcpy(key, argv[i], klen);
            key[klen] = '\0';
            const char *val = eq + 1;

            fputc(',', req_fp);
            fprint_json_str(req_fp, key);
            fputc(':', req_fp);
            if (is_numeric_key_val(key, val) || val[0] == '[' ||
                strcmp(val, "true") == 0 || strcmp(val, "false") == 0)
                fputs(val, req_fp);
            else
                fprint_json_str(req_fp, val);
        }
        fprintf(req_fp, "}\n");
    } else {
        fclose(req_fp);
        free(req_buf);
        fprintf(stderr, "unknown msg subcommand '%s'\n", sub);
        return 2;
    }
    fclose(req_fp);

    char sock_path[256];
    const char *env_sock = getenv(COMP_IPC_ENV_SOCKET);
    const char *xdg_run  = getenv("XDG_RUNTIME_DIR");
    if (env_sock && *env_sock)
        snprintf(sock_path, sizeof(sock_path), "%s", env_sock);
    else if (xdg_run && *xdg_run)
        snprintf(sock_path, sizeof(sock_path), "%s/%s", xdg_run, COMP_IPC_SOCK_NAME);
    else
        snprintf(sock_path, sizeof(sock_path), "/tmp/kajo-comp-%d.sock", (int)getuid());

    int fd = connect_unix_socket(sock_path);
    if (fd < 0 && (!env_sock || !*env_sock)) {
        int saved_err = errno;
        const char *scan_dir = (xdg_run && *xdg_run) ? xdg_run : "/tmp";
        fd = connect_nested_fallback_socket(scan_dir, sock_path, sizeof(sock_path));
        if (fd < 0)
            errno = saved_err;
    }
    if (fd < 0) {
        fprintf(stderr, "cannot connect to %s: %s\n", sock_path, strerror(errno));
        free(req_buf);
        return 1;
    }

    size_t sent = 0;
    while (sent < req_len) {
        ssize_t n = send(fd, req_buf + sent, req_len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            close(fd);
            free(req_buf);
            return 1;
        }
        sent += (size_t)n;
    }
    free(req_buf);

    FILE *sock_fp = fdopen(fd, "r");
    if (!sock_fp) {
        close(fd);
        return 1;
    }

    char *line = NULL;
    size_t cap = 0;
    int exit_code = 1;
    while (getline(&line, &cap, sock_fp) >= 0) {
        fputs(line, stdout);
        fflush(stdout);
        if (strncmp(line, "{\"ok\":", 6) == 0)
            exit_code = 0;
        else if (strncmp(line, "{\"error\":", 9) == 0)
            exit_code = 1;
        if (!is_subscribe)
            break;
    }
    free(line);
    fclose(sock_fp);
    return exit_code;
}

int
main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "msg") == 0)
        return run_msg_cli(argc, argv);

    enum wlr_log_importance level = WLR_INFO;
    CompServerOptions opts = { .mod_mask = COMP_MOD_SUPER };
    bool mod_explicit = false;
    static const char *const k_default_greeter_argv[] = { "kajo-greeter", NULL };
    static const struct option long_opts[] = {
        { "session", no_argument, NULL, 'S' },
        { "greeter-mode", no_argument, NULL, 'G' },
        { "config", required_argument, NULL, 'c' },
        { "verbose", no_argument, NULL, 'v' },
        { "quiet", no_argument, NULL, 'q' },
        { "mod", required_argument, NULL, 'm' },
        { "exit-with-command", no_argument, NULL, 'x' },
        { "help", no_argument, NULL, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "+c:vqh", long_opts, NULL)) != -1) {
        switch (c) {
        case 'S': opts.session_mode = true; break;
        case 'G':
            opts.greeter_mode = true;
            opts.exit_with_startup = true;
            break;
        case 'c': opts.config_path = optarg; break;
        case 'v': level = WLR_DEBUG; break;
        case 'q': level = WLR_ERROR; break;
        case 'x': opts.exit_with_startup = true; break;
        case 'm':
            mod_explicit = true;
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
    else if (opts.greeter_mode)
        opts.startup_argv = k_default_greeter_argv;

    if (opts.exit_with_startup && !opts.startup_argv) {
        fprintf(stderr, "--exit-with-command needs a COMMAND after --\n");
        return 2;
    }

    /* Spec §6.1: default Mod key is super in bare-metal/session, alt when nested */
    if (!mod_explicit && !opts.session_mode && !opts.greeter_mode) {
        const char *wl_disp = getenv("WAYLAND_DISPLAY");
        const char *wlr_bk  = getenv("WLR_BACKENDS");
        if ((wl_disp && *wl_disp && (!wlr_bk || !strstr(wlr_bk, "headless"))) ||
            (wlr_bk && strstr(wlr_bk, "wayland"))) {
            opts.mod_mask = COMP_MOD_ALT;
        }
    }

    /* Spec §6.5.1 & §6.5.2: greeter-mode security hardening & EX_SOFTWARE (70) fallback */
    if (opts.greeter_mode) {
        struct rlimit zero_core = { 0, 0 };
        (void)setrlimit(RLIMIT_CORE, &zero_core);
        (void)prctl(PR_SET_DUMPABLE, 0);
        const char *xdg_run = getenv("XDG_RUNTIME_DIR");
        if (!xdg_run || !*xdg_run) {
            fprintf(stderr, "kajo-comp: XDG_RUNTIME_DIR is required in --greeter-mode\n");
            return EX_SOFTWARE;
        }
    }

    /* Writing to a disconnected client must never kill the compositor (spec §7.2.1) */
    signal(SIGPIPE, SIG_IGN);
    wlr_log_init(level, NULL);

    int fail_code = opts.greeter_mode ? EX_SOFTWARE : 1;
    CompServer *server = comp_server_create(&opts);
    if (!server)
        return fail_code;
    int status = fail_code;
    if (comp_server_start(server)) {
        comp_server_run(server);
        status = server->exit_status;
    }
    comp_server_destroy(server);
    return status;
}
