/* comp_ipc_private.h — internal IPC types, JSON builder, serializers & watcher. */
#ifndef KAJO_COMP_IPC_PRIVATE_H
#define KAJO_COMP_IPC_PRIVATE_H

#include "comp_server.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define IPC_MAX_FRAME   65536
#define IPC_MAX_CLIENTS 64

enum {
    SUB_WORKSPACES       = 1u << 0,
    SUB_WINDOWS          = 1u << 1,
    SUB_KEYBOARD_LAYOUTS = 1u << 2,
    SUB_OUTPUTS          = 1u << 3,
    SUB_TOUCH_GESTURES   = 1u << 4,
    SUB_SCREENSHOTS      = 1u << 5,
    SUB_ALL              = 0x3fu,
};

typedef struct {
    struct wl_list          link;   /* CompIpcState.clients */
    CompServer             *server;
    int                     fd;
    struct wl_event_source *ev;
    uint32_t                subs;
    char                    rbuf[IPC_MAX_FRAME];
    size_t                  rlen;
    char                   *wbuf;
    size_t                  wlen;
    size_t                  wcap;
    bool                    closing, in_dispatch;
} CompIpcClient;

struct CompIpcState {
    CompServer             *server;
    char                   *socket_path;
    int                     listen_fd;
    struct wl_event_source *listen_ev;
    bool                    has_socket_stat;
    dev_t                   socket_dev;
    ino_t                   socket_ino;
    struct wl_list          clients;        /* CompIpcClient.link */
    size_t                  n_clients;

    char                   *config_dir;
    int                     inotify_fd;
    int                     wd_root;
    int                     wd_confd;
    struct wl_event_source *inotify_ev;
    struct wl_event_source *debounce_timer;
};

typedef struct {
    void   *ctx;
    char   *buf;
    size_t  len;
    size_t  cap;
} JsonBuf;

/* EventStream delivery implemented in comp_ipc.c */
bool comp_ipc_has_subscribers(const CompServer *s, uint32_t sub_bit);
void comp_ipc_broadcast_event(CompServer *s, uint32_t sub_bit, const char *line);

/* JSON builder & payload serializers implemented in comp_ipc_watch.c */
void comp_ipc_jbuf_init(JsonBuf *b, void *talloc_ctx);
void comp_ipc_jbuf_puts(JsonBuf *b, const char *s);
__attribute__((format(printf, 2, 3)))
void comp_ipc_jbuf_printf(JsonBuf *b, const char *fmt, ...);
void comp_ipc_jbuf_str(JsonBuf *b, const char *s);

void comp_ipc_append_outputs_array(JsonBuf *b, const CompServer *s);
void comp_ipc_append_workspaces_array(JsonBuf *b, const CompServer *s);
void comp_ipc_append_windows_array(JsonBuf *b, const CompServer *s);
void comp_ipc_append_keyboard_layouts_fields(JsonBuf *b, const CompServer *s);

void comp_ipc_build_workspaces_event(JsonBuf *b, const CompServer *s);
void comp_ipc_build_windows_event(JsonBuf *b, const CompServer *s);
void comp_ipc_build_keyboard_layouts_event(JsonBuf *b, const CompServer *s);
void comp_ipc_build_outputs_event(JsonBuf *b, const CompServer *s);

/* Inotify config watcher implemented in comp_ipc_watch.c */
void comp_ipc_watch_init(CompServer *s, struct CompIpcState *ipc);
void comp_ipc_watch_finish(struct CompIpcState *ipc);

#endif /* KAJO_COMP_IPC_PRIVATE_H */
