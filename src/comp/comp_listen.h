/* comp_listen.h — auto-unlinking Wayland listeners owned by a talloc context.
 *
 * Spec §3.8.3 rule 1: a forgotten wl_list_remove() on a listener is an instant
 * crash the next time the signal fires. comp_listen() allocates the listener as
 * a talloc child of `owner`; the listener's destructor unlinks it, so freeing
 * the owner (talloc_free) can never leave a dangling listener behind.
 *
 * The callback receives `owner` (not the wl_listener), so handlers never need
 * wl_container_of().
 *
 * It is safe to talloc_free(owner) from inside one of its own handlers:
 * libwayland's wl_signal_emit_mutable(), which wlroots uses, tolerates removal
 * of the listener that is currently running.
 */
#ifndef KAJO_COMP_LISTEN_H
#define KAJO_COMP_LISTEN_H

#include <stdbool.h>
#include <wayland-server-core.h>

typedef void (*CompListenFn)(void *owner, void *data);

typedef struct CompListener CompListener;

/* Returns NULL (and logs) on allocation failure or NULL arguments. */
CompListener *comp_listen(void *owner, struct wl_signal *signal, CompListenFn fn);

/* Same, but the listener is a talloc child of `ctx` while the callback receives
 * `owner`. Used when listeners must be unlinked before their owner is torn
 * down (the server frees its listener context before destroying the backend). */
CompListener *comp_listen_ctx(void *ctx, void *owner, struct wl_signal *signal, CompListenFn fn);

/* Unlinks and frees one listener before its owner goes away. NULL is a no-op. */
void          comp_unlisten(CompListener *listener);

#endif /* KAJO_COMP_LISTEN_H */
