/* comp_layout_private.h — internal layout model types and helpers (pure C). */
#ifndef KAJO_COMP_LAYOUT_PRIVATE_H
#define KAJO_COMP_LAYOUT_PRIVATE_H

#include "comp_layout.h"

#define MIN_COL_WIDTH   0.05
#define MIN_WIN_HEIGHT  0.10
#define WIDTH_EPS       1e-6

typedef struct {
    CompWinId id;
    double    height;      /* proportion of the column height, 0 = automatic */
    int       preset;      /* index into preset_heights of the last switch, -1 none */
} Win;

typedef struct {
    Win    *wins;
    size_t  n, cap;
    size_t  focus;
    double  width;          /* proportion of the viewport */
    double  width_saved;    /* restored when maximize is toggled off */
    bool    maximized;
    bool    fullscreen;
    bool    tabbed;
} Column;

typedef struct {
    CompWinId id;
    CompRect  rect;         /* viewport-relative; w or h <= 0 means "pick a default" */
} Floater;

typedef struct {
    Column  *cols;
    size_t   n, cap;
    size_t   focus;
    Floater *fl;
    size_t   nfl, capfl;
    size_t   fl_focus;
    bool     focus_floating;
    bool     center_once;
    bool     center_visible_once;
    uint64_t id;            /* stable across reordering and removal of other workspaces */
} Workspace;

struct CompLayout {
    CompLayoutConfig cfg;
    Workspace       *ws;
    size_t           nws, capws;
    size_t           active;
    uint64_t         next_ws_id;
};

typedef struct {
    size_t ws, col, win, fl;
    bool   floating;
} Loc;

static inline Workspace *
comp_layout_active_ws(const CompLayout *L)
{
    return &L->ws[L->active];
}

static inline Column *
comp_layout_focused_col(const Workspace *w)
{
    return w->n ? &w->cols[w->focus] : NULL;
}

/* Shared internal helpers implemented in comp_layout.c */
bool comp_layout_grow(void **arr, size_t *cap, size_t need, size_t elem);
void comp_layout_normalize_workspaces(CompLayout *L);
bool comp_layout_find_window(const CompLayout *L, CompWinId id, Loc *out);
void comp_layout_remove_column(Workspace *ws, size_t at, bool free_it);
Win  comp_layout_column_take(Column *c, size_t at);
bool comp_layout_add_tiled_to(CompLayout *L, size_t wsi, Win win, double width);

/* Floating-window helpers implemented in comp_layout_float.c */
bool comp_layout_add_floating_to(Workspace *ws, CompWinId id, CompRect rect);
void comp_layout_remove_floating_at(Workspace *ws, size_t f);
bool comp_layout_toggle_floating(CompLayout *L);

#endif /* KAJO_COMP_LAYOUT_PRIVATE_H */
