/* comp_screenshot_private.h — internal screenshot state & UI/PNG/clipboard helpers. */
#ifndef KAJO_COMP_SCREENSHOT_PRIVATE_H
#define KAJO_COMP_SCREENSHOT_PRIVATE_H

#include "comp_server.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

#define HANDLE_SIZE_PX   6
#define BADGE_FLIP_TOP  32
#define BADGE_GAP_PX     6

typedef enum {
    DRAG_NONE = 0,
    DRAG_NEW,
    DRAG_MOVE,
    DRAG_HANDLE_TL,
    DRAG_HANDLE_TM,
    DRAG_HANDLE_TR,
    DRAG_HANDLE_ML,
    DRAG_HANDLE_MR,
    DRAG_HANDLE_BL,
    DRAG_HANDLE_BM,
    DRAG_HANDLE_BR,
} ScreenshotDragMode;

typedef struct ScreenshotJobResult {
    struct ScreenshotJobResult *next;
    char                       *path;
    uint8_t                    *png_bytes;
    size_t                      png_len;
    bool                        copy_to_clipboard;
    bool                        show_notification;
    bool                        ok;
} ScreenshotJobResult;

typedef struct CompScreenshot {
    CompServer              *server;
    bool                     active;

    /* Frozen output state for interactive `screenshot` */
    uint8_t                 *frozen_pixels; /* owned copy for cropping on confirm */
    int                      phys_w;
    int                      phys_h;
    double                   scale;
    struct wlr_box           out_box;       /* logical output box */

    /* Selection box in output-local logical coordinates (0..out_box.width/height) */
    int                      sel_x;
    int                      sel_y;
    int                      sel_w;
    int                      sel_h;

    ScreenshotDragMode       drag_mode;
    double                   drag_start_lx;
    double                   drag_start_ly;
    int                      drag_orig_x;
    int                      drag_orig_y;
    int                      drag_orig_w;
    int                      drag_orig_h;

    /* Overlay scene nodes in s->tree_overlay */
    struct wlr_scene_tree   *tree;
    struct wlr_scene_buffer *frozen_buf;
    struct wlr_scene_rect   *dim[4];        /* top, bottom, left, right */
    struct wlr_scene_rect   *border[4];     /* 1px outside selection */
    struct wlr_scene_rect   *handles[8];    /* TL, TM, TR, ML, MR, BL, BM, BR */
    struct wlr_scene_buffer *badge_buf;

    /* Background worker completion queue */
    int                       notify_fd;
    struct wl_event_source   *notify_ev;
    pthread_mutex_t           res_lock;
    pthread_cond_t            workers_done;
    int                       pending_workers;
    bool                      shutting_down;
    ScreenshotJobResult      *res_head;
    ScreenshotJobResult      *res_tail;
} CompScreenshot;

/* Overlay scene UI, PNG writer & clipboard helpers implemented in comp_screenshot_ui.c */
void comp_screenshot_clamp_selection(CompScreenshot *ss);
void comp_screenshot_update_selection_scene(CompScreenshot *ss);
bool comp_screenshot_setup_overlay(CompScreenshot *ss, const struct wlr_box *obox,
                                   uint8_t *pixels, int phys_w, int phys_h, double scale);
void comp_screenshot_close_overlay(CompScreenshot *ss);
void comp_screenshot_dispatch_worker(CompServer *s, uint8_t *owned_argb, int width, int height);
int  comp_screenshot_handle_worker_notify_fd(int fd, uint32_t mask, void *data);

#endif /* KAJO_COMP_SCREENSHOT_PRIVATE_H */
