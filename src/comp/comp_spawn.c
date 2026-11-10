/* comp_spawn.c — start clients without a shell, reap them from the event loop.
 *
 * Children run in their own session (setsid) so they are not killed with the
 * compositor's process group, and get a clean signal mask (wl_event_loop
 * blocks the signals it handles through signalfd, and masks are inherited).
 * Between fork() and exec() only async-signal-safe calls are made.
 */
#include "comp_server.h"

#include "comp_log.h"

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

int
comp_spawn(CompServer *s, const char *const *argv)
{
    COMP_WARN_RETURN(s && argv && argv[0], -1);
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "fork");
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        setsid();
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    wlr_log(WLR_INFO, "started '%s' (pid %d)", argv[0], (int)pid);
    return (int)pid;
}

static int
handle_sigchld(int sig, void *data)
{
    (void)sig;
    CompServer *s = data;
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;
        if (s->startup_pid && pid == s->startup_pid) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            wlr_log(WLR_INFO, "startup command exited with status %d", code);
            s->startup_pid = 0;
            if (s->opts.exit_with_startup)
                comp_server_terminate(s, code);
        }
    }
    return 0;
}

void
comp_spawn_init(CompServer *s)
{
    s->signal_sources[2] = wl_event_loop_add_signal(s->loop, SIGCHLD, handle_sigchld, s);
    if (!s->signal_sources[2])
        wlr_log(WLR_ERROR, "cannot watch SIGCHLD; exited clients will not be reaped");
}
