#ifndef KAJO_PA_CONN_H
#define KAJO_PA_CONN_H

#include <glib.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

G_BEGIN_DECLS

/* Delay before reconnecting after the PulseAudio/PipeWire server goes away
 * (e.g. `systemctl --user restart pipewire-pulse`), per AGENTS.md §4.C. */
#define SHELL_PA_RECONNECT_MS 2000

/* Called for every context state change, after the shared reconnect logic ran.
 * Subscribe to events and issue queries from here when state == PA_CONTEXT_READY. */
typedef void (*ShellPaStateFunc)(pa_context *ctx, pa_context_state_t state, gpointer user_data);

/* Optional: called right before an existing context is torn down (reconnect or
 * close) so the owner can drop streams / flags that depend on it. */
typedef void (*ShellPaDropFunc)(gpointer user_data);

/* One PulseAudio/PipeWire client connection driven by the GLib main loop, with
 * automatic reconnect. Embed it in the owning struct (as `pa`) and use
 * conn->ctx for pa_context_* calls; it is NULL until the first connect. */
typedef struct {
    pa_glib_mainloop *ml;
    pa_context       *ctx;
    guint             reconnect_id;
    gchar            *client_name;
    ShellPaStateFunc  state_cb;
    ShellPaDropFunc   drop_cb;
    gpointer          user_data;
} ShellPaConn;

/* Creates the main-loop glue and connects. Returns FALSE if the main loop
 * could not be created. */
gboolean shell_pa_conn_open(ShellPaConn *conn,
                            const gchar *client_name,
                            ShellPaStateFunc state_cb,
                            ShellPaDropFunc drop_cb,
                            gpointer user_data);

/* Cancels any pending reconnect, drops the context and frees the main loop.
 * Safe to call on a zeroed or already-closed connection. */
void shell_pa_conn_close(ShellPaConn *conn);

G_END_DECLS

#endif /* KAJO_PA_CONN_H */
