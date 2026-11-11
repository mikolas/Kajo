/* comp_layout.c — see comp_layout.h. */
#include "comp_layout.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
} Workspace;

struct CompLayout {
    CompLayoutConfig cfg;
    Workspace       *ws;
    size_t           nws, capws;
    size_t           active;
};

/* ───────────────────────── small helpers ───────────────────────── */

static bool
grow(void **arr, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap)
        return true;
    size_t ncap = *cap ? *cap * 2 : 4;
    while (ncap < need)
        ncap *= 2;
    void *p = realloc(*arr, ncap * elem);
    if (!p)
        return false;
    *arr = p;
    *cap = ncap;
    return true;
}

static double
clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void
column_free(Column *c)
{
    free(c->wins);
    memset(c, 0, sizeof(*c));
}

static void
workspace_free(Workspace *w)
{
    for (size_t i = 0; i < w->n; i++)
        column_free(&w->cols[i]);
    free(w->cols);
    free(w->fl);
    memset(w, 0, sizeof(*w));
}

static bool
workspace_empty(const Workspace *w)
{
    return w->n == 0 && w->nfl == 0;
}

static Workspace *
active_ws(const CompLayout *L)
{
    return &L->ws[L->active];
}

static Column *
focused_col(const Workspace *w)
{
    return w->n ? &w->cols[w->focus] : NULL;
}

/* ───────────────────────── configuration ───────────────────────── */

void
comp_layout_config_defaults(CompLayoutConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->gap                         = 6.0;
    cfg->default_width               = 0.5;
    cfg->preset_widths[0]            = 0.33333;
    cfg->preset_widths[1]            = 0.5;
    cfg->preset_widths[2]            = 0.66667;
    cfg->n_preset_widths             = 3;
    cfg->preset_heights[0]           = 0.33333;
    cfg->preset_heights[1]           = 0.5;
    cfg->preset_heights[2]           = 0.66667;
    cfg->preset_heights[3]           = 1.0;
    cfg->n_preset_heights            = 4;
    cfg->center_mode                 = COMP_CENTER_NEVER;
    cfg->always_center_single_column = true;
}

bool
comp_layout_config_valid(const CompLayoutConfig *cfg)
{
    if (!cfg || !isfinite(cfg->gap) || cfg->gap < 0.0 || cfg->gap > 1000.0)
        return false;
    if (!isfinite(cfg->default_width) || cfg->default_width < MIN_COL_WIDTH || cfg->default_width > 1.0)
        return false;
    if (cfg->n_preset_widths > COMP_LAYOUT_MAX_PRESETS || cfg->n_preset_heights > COMP_LAYOUT_MAX_PRESETS)
        return false;
    for (size_t i = 0; i < cfg->n_preset_widths; i++) {
        double v = cfg->preset_widths[i];
        if (!isfinite(v) || v < MIN_COL_WIDTH || v > 1.0)
            return false;
    }
    for (size_t i = 0; i < cfg->n_preset_heights; i++) {
        double v = cfg->preset_heights[i];
        if (!isfinite(v) || v < MIN_WIN_HEIGHT || v > 1.0)
            return false;
    }
    return cfg->center_mode <= COMP_CENTER_ALWAYS;
}

static bool
append_empty_workspace(CompLayout *L)
{
    if (!grow((void **)&L->ws, &L->capws, L->nws + 1, sizeof(Workspace)))
        return false;
    memset(&L->ws[L->nws], 0, sizeof(Workspace));
    L->nws++;
    return true;
}

CompLayout *
comp_layout_new(const CompLayoutConfig *cfg)
{
    if (!comp_layout_config_valid(cfg))
        return NULL;
    CompLayout *L = calloc(1, sizeof(*L));
    if (!L)
        return NULL;
    L->cfg = *cfg;
    if (!append_empty_workspace(L)) {
        free(L);
        return NULL;
    }
    return L;
}

void
comp_layout_free(CompLayout *L)
{
    if (!L)
        return;
    for (size_t i = 0; i < L->nws; i++)
        workspace_free(&L->ws[i]);
    free(L->ws);
    free(L);
}

bool
comp_layout_set_config(CompLayout *L, const CompLayoutConfig *cfg)
{
    if (!L || !comp_layout_config_valid(cfg))
        return false;
    L->cfg = *cfg;
    return true;
}

/* Keep exactly one empty workspace at the end; drop other empty, unfocused ones. */
static void
normalize_workspaces(CompLayout *L)
{
    size_t i = 0;
    while (i + 1 < L->nws) {
        if (i != L->active && workspace_empty(&L->ws[i])) {
            workspace_free(&L->ws[i]);
            memmove(&L->ws[i], &L->ws[i + 1], (L->nws - i - 1) * sizeof(Workspace));
            L->nws--;
            if (L->active > i)
                L->active--;
            continue;
        }
        i++;
    }
    if (!workspace_empty(&L->ws[L->nws - 1]))
        (void)append_empty_workspace(L);
    while (L->nws >= 2 && workspace_empty(&L->ws[L->nws - 1]) &&
           workspace_empty(&L->ws[L->nws - 2]) && L->active != L->nws - 1) {
        workspace_free(&L->ws[L->nws - 1]);
        L->nws--;
    }
}

/* ───────────────────────── lookup ───────────────────────── */

typedef struct {
    size_t ws, col, win, fl;
    bool   floating;
} Loc;

static bool
find_window(const CompLayout *L, CompWinId id, Loc *out)
{
    if (id == 0)
        return false;
    for (size_t w = 0; w < L->nws; w++) {
        const Workspace *ws = &L->ws[w];
        for (size_t c = 0; c < ws->n; c++) {
            for (size_t i = 0; i < ws->cols[c].n; i++) {
                if (ws->cols[c].wins[i].id == id) {
                    *out = (Loc){ .ws = w, .col = c, .win = i, .floating = false };
                    return true;
                }
            }
        }
        for (size_t f = 0; f < ws->nfl; f++) {
            if (ws->fl[f].id == id) {
                *out = (Loc){ .ws = w, .fl = f, .floating = true };
                return true;
            }
        }
    }
    return false;
}

bool
comp_layout_contains(const CompLayout *L, CompWinId id)
{
    Loc loc;
    return L && find_window(L, id, &loc);
}

size_t
comp_layout_window_count(const CompLayout *L)
{
    size_t n = 0;
    for (size_t w = 0; L && w < L->nws; w++) {
        for (size_t c = 0; c < L->ws[w].n; c++)
            n += L->ws[w].cols[c].n;
        n += L->ws[w].nfl;
    }
    return n;
}

CompWinId
comp_layout_focused_window(const CompLayout *L)
{
    if (!L)
        return 0;
    const Workspace *ws = active_ws(L);
    if (ws->focus_floating && ws->nfl)
        return ws->fl[ws->fl_focus].id;
    const Column *c = focused_col(ws);
    return c ? c->wins[c->focus].id : 0;
}

/* ───────────────────────── column / window insertion ───────────────────────── */

static bool
insert_column(Workspace *ws, size_t at, Column col)
{
    if (!grow((void **)&ws->cols, &ws->cap, ws->n + 1, sizeof(Column)))
        return false;
    memmove(&ws->cols[at + 1], &ws->cols[at], (ws->n - at) * sizeof(Column));
    ws->cols[at] = col;
    ws->n++;
    return true;
}

static bool
column_new_with(Column *col, Win win, double width)
{
    memset(col, 0, sizeof(*col));
    if (!grow((void **)&col->wins, &col->cap, 1, sizeof(Win)))
        return false;
    col->wins[0] = win;
    col->n       = 1;
    col->width   = width;
    return true;
}

/* Removes column `at` (already emptied or to be freed by caller) and fixes focus. */
static void
remove_column(Workspace *ws, size_t at, bool free_it)
{
    if (free_it)
        column_free(&ws->cols[at]);
    memmove(&ws->cols[at], &ws->cols[at + 1], (ws->n - at - 1) * sizeof(Column));
    ws->n--;
    if (ws->n == 0)
        ws->focus = 0;
    else if (ws->focus > at || ws->focus >= ws->n)
        ws->focus = ws->focus ? ws->focus - 1 : 0;
}

static Win
column_take(Column *c, size_t at)
{
    Win w = c->wins[at];
    memmove(&c->wins[at], &c->wins[at + 1], (c->n - at - 1) * sizeof(Win));
    c->n--;
    if (c->n == 0)
        c->focus = 0;
    else if (c->focus > at || c->focus >= c->n)
        c->focus = c->focus ? c->focus - 1 : 0;
    return w;
}

static bool
add_tiled_to(CompLayout *L, size_t wsi, Win win, double width)
{
    Workspace *ws = &L->ws[wsi];
    Column col;
    if (!column_new_with(&col, win, width))
        return false;
    size_t at = ws->n ? ws->focus + 1 : 0;
    if (!insert_column(ws, at, col)) {
        column_free(&col);
        return false;
    }
    ws->focus          = at;
    ws->focus_floating = false;
    return true;
}

bool
comp_layout_add_window(CompLayout *L, CompWinId id)
{
    if (!L || id == 0 || comp_layout_contains(L, id))
        return false;
    Win win = { .id = id, .height = 0.0, .preset = -1 };
    if (!add_tiled_to(L, L->active, win, L->cfg.default_width))
        return false;
    normalize_workspaces(L);
    return true;
}

static bool
add_floating_to(Workspace *ws, CompWinId id, CompRect rect)
{
    if (!grow((void **)&ws->fl, &ws->capfl, ws->nfl + 1, sizeof(Floater)))
        return false;
    ws->fl[ws->nfl] = (Floater){ .id = id, .rect = rect };
    ws->fl_focus    = ws->nfl;
    ws->nfl++;
    ws->focus_floating = true;
    return true;
}

bool
comp_layout_add_floating(CompLayout *L, CompWinId id, CompRect rect)
{
    if (!L || id == 0 || comp_layout_contains(L, id))
        return false;
    if (!isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.w) || !isfinite(rect.h))
        rect = (CompRect){ 0 };
    if (!add_floating_to(active_ws(L), id, rect))
        return false;
    normalize_workspaces(L);
    return true;
}

static void
remove_floating_at(Workspace *ws, size_t f)
{
    memmove(&ws->fl[f], &ws->fl[f + 1], (ws->nfl - f - 1) * sizeof(Floater));
    ws->nfl--;
    if (ws->nfl == 0) {
        ws->fl_focus       = 0;
        ws->focus_floating = false;
    } else if (ws->fl_focus > f || ws->fl_focus >= ws->nfl) {
        ws->fl_focus = ws->fl_focus ? ws->fl_focus - 1 : 0;
    }
}

bool
comp_layout_remove_window(CompLayout *L, CompWinId id)
{
    Loc loc;
    if (!L || !find_window(L, id, &loc))
        return false;
    Workspace *ws = &L->ws[loc.ws];
    if (loc.floating) {
        remove_floating_at(ws, loc.fl);
    } else {
        Column *c = &ws->cols[loc.col];
        (void)column_take(c, loc.win);
        if (c->n == 0)
            remove_column(ws, loc.col, true);
        if (ws->n == 0 && ws->nfl)
            ws->focus_floating = true;
    }
    normalize_workspaces(L);
    return true;
}

bool
comp_layout_set_floating_rect(CompLayout *L, CompWinId id, CompRect rect)
{
    Loc loc;
    if (!L || !find_window(L, id, &loc) || !loc.floating)
        return false;
    if (!isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.w) || !isfinite(rect.h))
        return false;
    L->ws[loc.ws].fl[loc.fl].rect = rect;
    return true;
}

bool
comp_layout_focus_window(CompLayout *L, CompWinId id)
{
    Loc loc;
    if (!L || !find_window(L, id, &loc))
        return false;
    Workspace *ws = &L->ws[loc.ws];
    if (loc.floating) {
        ws->fl_focus       = loc.fl;
        ws->focus_floating = true;
    } else {
        ws->focus                 = loc.col;
        ws->cols[loc.col].focus   = loc.win;
        ws->focus_floating        = false;
    }
    L->active = loc.ws;
    normalize_workspaces(L);
    return true;
}

/* ───────────────────────── fullscreen (S1–S4) ───────────────────────── */

bool
comp_layout_set_fullscreen(CompLayout *L, CompWinId id, bool fullscreen)
{
    Loc loc;
    if (!L || !find_window(L, id, &loc) || loc.floating)
        return false;
    Workspace *ws = &L->ws[loc.ws];
    Column *c = &ws->cols[loc.col];
    if (c->fullscreen == fullscreen && (c->n == 1 || c->tabbed))
        return false;
    /* S2: promoting a window from a multi-window stacked column expels it first */
    if (fullscreen && c->n >= 2 && !c->tabbed) {
        Win w = c->wins[loc.win];
        w.height = 0.0;
        w.preset = -1;
        Column ncol;
        if (!column_new_with(&ncol, w, c->width))
            return false;
        ncol.fullscreen = true;
        if (!insert_column(ws, loc.col + 1, ncol)) {
            column_free(&ncol);
            return false;
        }
        c = &ws->cols[loc.col];
        (void)column_take(c, loc.win);
        if (ws->focus == loc.col)
            ws->focus = loc.col + 1;
        else if (ws->focus > loc.col)
            ws->focus++;
        return true;
    }
    c->fullscreen = fullscreen;
    return true;
}

bool
comp_layout_toggle_fullscreen(CompLayout *L)
{
    if (!L)
        return false;
    CompWinId id = comp_layout_focused_window(L);
    Loc loc;
    if (!id || !find_window(L, id, &loc) || loc.floating)
        return false;
    bool cur = L->ws[loc.ws].cols[loc.col].fullscreen;
    return comp_layout_set_fullscreen(L, id, !cur);
}

/* ───────────────────────── workspaces ───────────────────────── */

size_t
comp_layout_workspace_count(const CompLayout *L)
{
    return L ? L->nws : 0;
}

size_t
comp_layout_active_workspace(const CompLayout *L)
{
    return L ? L->active : 0;
}

size_t
comp_layout_workspace_window_count(const CompLayout *L, size_t ws_index)
{
    if (!L || ws_index >= L->nws)
        return 0;
    const Workspace *ws = &L->ws[ws_index];
    size_t count = ws->nfl;
    for (size_t c = 0; c < ws->n; c++)
        count += ws->cols[c].n;
    return count;
}

bool
comp_layout_window_workspace(const CompLayout *L, CompWinId id, size_t *out_ws_index)
{
    Loc loc;
    if (!L || id == 0 || !find_window(L, id, &loc))
        return false;
    if (out_ws_index)
        *out_ws_index = loc.ws;
    return true;
}

bool
comp_layout_focus_workspace_index(CompLayout *L, size_t index)
{
    if (!L || index >= L->nws || index == L->active)
        return false;
    L->active = index;
    normalize_workspaces(L);
    return true;
}

static bool
move_column_to_workspace(CompLayout *L, size_t target)
{
    Workspace *src = active_ws(L);
    if (src->n == 0 || target >= L->nws || target == L->active)
        return false;
    Column col = src->cols[src->focus];
    Workspace *dst = &L->ws[target];
    size_t at = dst->n ? dst->focus + 1 : 0;
    if (!insert_column(dst, at, col))
        return false;
    src = active_ws(L);
    remove_column(src, src->focus, false);
    dst->focus          = at;
    dst->focus_floating = false;
    L->active           = target;
    normalize_workspaces(L);
    return true;
}

static bool
 swap_workspace(CompLayout *L, bool up)
{
    size_t a = L->active;
    if (up ? a == 0 : a + 1 >= L->nws)
        return false;
    size_t b = up ? a - 1 : a + 1;
    /* The trailing empty workspace must stay last */
    if (b == L->nws - 1 && workspace_empty(&L->ws[b]))
        return false;
    if (a == L->nws - 1 && workspace_empty(&L->ws[a]))
        return false;
    Workspace tmp = L->ws[a];
    L->ws[a]  = L->ws[b];
    L->ws[b]  = tmp;
    L->active = b;
    normalize_workspaces(L);
    return true;
}

/* ───────────────────────── per-column actions ───────────────────────── */

static bool
switch_preset_width(const CompLayoutConfig *cfg, Column *c)
{
    if (cfg->n_preset_widths == 0)
        return false;
    double cur      = c->maximized ? 1.0 : c->width;
    double smallest = cfg->preset_widths[0];
    double next     = INFINITY;
    for (size_t i = 0; i < cfg->n_preset_widths; i++) {
        double p = cfg->preset_widths[i];
        smallest = fmin(smallest, p);
        if (p > cur + WIDTH_EPS && p < next)
            next = p;
    }
    if (!isfinite(next))
        next = smallest;
    bool changed = fabs(next - c->width) > WIDTH_EPS || c->maximized;
    c->width     = next;
    c->maximized = false;
    return changed;
}

static bool
switch_preset_height(const CompLayoutConfig *cfg, Column *c)
{
    if (cfg->n_preset_heights == 0 || c->n < 2 || c->tabbed)
        return false;
    /* G4: at most one window in a column holds an explicit height */
    for (size_t i = 0; i < c->n; i++) {
        if (i != c->focus) {
            c->wins[i].height = 0.0;
            c->wins[i].preset = -1;
        }
    }
    Win *w = &c->wins[c->focus];
    int next = w->preset + 1;
    if (next < 0 || (size_t)next >= cfg->n_preset_heights)
        next = 0;
    w->preset = next;
    w->height = cfg->preset_heights[(size_t)next];
    return true;
}

static bool
toggle_floating(CompLayout *L)
{
    Workspace *ws = active_ws(L);
    if (ws->focus_floating && ws->nfl) {
        Floater f = ws->fl[ws->fl_focus];
        remove_floating_at(ws, ws->fl_focus);
        Win win = { .id = f.id, .height = 0.0, .preset = -1 };
        if (!add_tiled_to(L, L->active, win, L->cfg.default_width)) {
            (void)add_floating_to(active_ws(L), f.id, f.rect);
            return false;
        }
        return true;
    }
    Column *c = focused_col(ws);
    if (!c)
        return false;
    CompWinId id = c->wins[c->focus].id;
    if (!add_floating_to(ws, id, (CompRect){ 0 }))
        return false;
    (void)column_take(c, c->focus);
    if (c->n == 0)
        remove_column(ws, ws->focus, true);
    return true;
}

static bool
consume_into_left(Workspace *ws)
{
    if (ws->focus_floating || ws->n < 2 || ws->focus == 0)
        return false;
    Column *c    = &ws->cols[ws->focus];
    Column *left = &ws->cols[ws->focus - 1];
    if (!grow((void **)&left->wins, &left->cap, left->n + 1, sizeof(Win)))
        return false;
    Win w = column_take(c, c->focus);
    left->wins[left->n] = w;
    left->focus         = left->n;
    left->n++;
    size_t old = ws->focus;
    if (c->n == 0)
        remove_column(ws, old, true);
    ws->focus = old - 1;
    return true;
}

static bool
consume_into_right(Workspace *ws)
{
    if (ws->focus_floating || ws->n < 2 || ws->focus + 1 >= ws->n)
        return false;
    Column *c     = &ws->cols[ws->focus];
    Column *right = &ws->cols[ws->focus + 1];
    if (!grow((void **)&right->wins, &right->cap, right->n + 1, sizeof(Win)))
        return false;
    Win w = column_take(c, c->focus);
    right->wins[right->n] = w;
    right->focus          = right->n;
    right->n++;
    size_t old = ws->focus;
    if (c->n == 0) {
        remove_column(ws, old, true);
        ws->focus = old;   /* right shifted into `old` */
    } else {
        ws->focus = old + 1;
    }
    return true;
}

static bool
expel_dir(CompLayout *L, bool to_right)
{
    Workspace *ws = active_ws(L);
    Column *c = focused_col(ws);
    if (ws->focus_floating || !c || c->n < 2)
        return false;
    Win w = c->wins[c->focus];
    w.height = 0.0;
    w.preset = -1;
    Column col;
    /* K4: expelled column inherits the width of the column it left */
    if (!column_new_with(&col, w, c->width))
        return false;
    size_t ins = to_right ? ws->focus + 1 : ws->focus;
    if (!insert_column(ws, ins, col)) {
        column_free(&col);
        return false;
    }
    size_t src_col = to_right ? ws->focus : ws->focus + 1;
    c = &ws->cols[src_col];
    (void)column_take(c, c->focus);
    ws->focus = ins;
    return true;
}

static bool
expel_from_column(CompLayout *L)
{
    return expel_dir(L, true);
}

static bool
consume_or_expel(CompLayout *L, bool to_right)
{
    Workspace *ws = active_ws(L);
    Column *c = focused_col(ws);
    if (ws->focus_floating || !c)
        return false;
    if (c->n == 1)
        return to_right ? consume_into_right(ws) : consume_into_left(ws);
    return expel_dir(L, to_right);
}

/* W3: expand focused column to fill remaining available width beside visible neighbors. */
static bool
expand_column_to_available(CompLayout *L)
{
    Workspace *ws = active_ws(L);
    Column *c = focused_col(ws);
    if (!c || c->maximized || c->fullscreen)
        return false;
    double target = 1.0;
    if (ws->n > 1 && L->cfg.center_mode != COMP_CENTER_ALWAYS) {
        double used = 0.0;
        if (ws->focus > 0) {
            double lw = ws->cols[ws->focus - 1].maximized ? 1.0 : ws->cols[ws->focus - 1].width;
            if (lw + MIN_COL_WIDTH <= 1.0 + WIDTH_EPS)
                used += lw;
        }
        if (used == 0.0 && ws->focus + 1 < ws->n) {
            double rw = ws->cols[ws->focus + 1].maximized ? 1.0 : ws->cols[ws->focus + 1].width;
            if (rw + MIN_COL_WIDTH <= 1.0 + WIDTH_EPS)
                used += rw;
        }
        target = clampd(1.0 - used, MIN_COL_WIDTH, 1.0);
    }
    if (fabs(target - c->width) <= WIDTH_EPS)
        return false;
    c->width = target;
    return true;
}

static bool
swap_columns(Workspace *ws, size_t a, size_t b)
{
    if (a == b || a >= ws->n || b >= ws->n)
        return false;
    Column t    = ws->cols[a];
    ws->cols[a] = ws->cols[b];
    ws->cols[b] = t;
    return true;
}

static bool
move_column_to(Workspace *ws, size_t to)
{
    if (ws->n < 2 || to >= ws->n || to == ws->focus)
        return false;
    Column c = ws->cols[ws->focus];
    if (to < ws->focus)
        memmove(&ws->cols[to + 1], &ws->cols[to], (ws->focus - to) * sizeof(Column));
    else
        memmove(&ws->cols[ws->focus], &ws->cols[ws->focus + 1], (to - ws->focus) * sizeof(Column));
    ws->cols[to] = c;
    ws->focus    = to;
    return true;
}

static bool
apply_tiling_action(CompLayout *L, CompAction a)
{
    Workspace *ws = active_ws(L);
    Column *c = focused_col(ws);
    switch (a) {
    case COMP_ACT_FOCUS_COLUMN_LEFT:
        if (!ws->n || ws->focus == 0) return false;
        ws->focus--; return true;
    case COMP_ACT_FOCUS_COLUMN_RIGHT:
        if (!ws->n || ws->focus + 1 >= ws->n) return false;
        ws->focus++; return true;
    case COMP_ACT_FOCUS_COLUMN_FIRST:
        if (!ws->n || ws->focus == 0) return false;
        ws->focus = 0; return true;
    case COMP_ACT_FOCUS_COLUMN_LAST:
        if (!ws->n || ws->focus + 1 == ws->n) return false;
        ws->focus = ws->n - 1; return true;
    case COMP_ACT_FOCUS_WINDOW_UP:
        if (!c || c->focus == 0) return false;
        c->focus--; return true;
    case COMP_ACT_FOCUS_WINDOW_DOWN:
        if (!c || c->focus + 1 >= c->n) return false;
        c->focus++; return true;
    case COMP_ACT_MOVE_COLUMN_LEFT:
        if (!ws->n || ws->focus == 0) return false;
        (void)swap_columns(ws, ws->focus, ws->focus - 1); ws->focus--; return true;
    case COMP_ACT_MOVE_COLUMN_RIGHT:
        if (!ws->n || ws->focus + 1 >= ws->n) return false;
        (void)swap_columns(ws, ws->focus, ws->focus + 1); ws->focus++; return true;
    case COMP_ACT_MOVE_COLUMN_TO_FIRST:
        return move_column_to(ws, 0);
    case COMP_ACT_MOVE_COLUMN_TO_LAST:
        return ws->n ? move_column_to(ws, ws->n - 1) : false;
    case COMP_ACT_MOVE_WINDOW_UP: {
        if (!c || c->focus == 0) return false;
        Win t = c->wins[c->focus - 1];
        c->wins[c->focus - 1] = c->wins[c->focus];
        c->wins[c->focus] = t;
        c->focus--; return true;
    }
    case COMP_ACT_MOVE_WINDOW_DOWN: {
        if (!c || c->focus + 1 >= c->n) return false;
        Win t = c->wins[c->focus + 1];
        c->wins[c->focus + 1] = c->wins[c->focus];
        c->wins[c->focus] = t;
        c->focus++; return true;
    }
    case COMP_ACT_CONSUME_INTO_COLUMN_LEFT:
        return consume_into_left(ws);
    case COMP_ACT_EXPEL_FROM_COLUMN:
        return expel_from_column(L);
    case COMP_ACT_CONSUME_OR_EXPEL_LEFT:
        return consume_or_expel(L, false);
    case COMP_ACT_CONSUME_OR_EXPEL_RIGHT:
        return consume_or_expel(L, true);
    case COMP_ACT_SWITCH_PRESET_COLUMN_WIDTH:
        return c ? switch_preset_width(&L->cfg, c) : false;
    case COMP_ACT_SWITCH_PRESET_WINDOW_HEIGHT:
        return c ? switch_preset_height(&L->cfg, c) : false;
    case COMP_ACT_RESET_WINDOW_HEIGHT:
        if (!c || (c->wins[c->focus].height == 0.0 && c->wins[c->focus].preset < 0)) return false;
        c->wins[c->focus].height = 0.0; c->wins[c->focus].preset = -1; return true;
    case COMP_ACT_MAXIMIZE_COLUMN:
        if (!c) return false;
        if (c->maximized) { c->width = c->width_saved; c->maximized = false; }
        else { c->width_saved = c->width; c->width = 1.0; c->maximized = true; }
        return true;
    case COMP_ACT_EXPAND_COLUMN_TO_AVAILABLE:
        return expand_column_to_available(L);
    case COMP_ACT_TOGGLE_COLUMN_TABBED:
        if (!c) return false;
        if (c->fullscreen && c->tabbed) c->fullscreen = false;
        c->tabbed = !c->tabbed; return true;
    case COMP_ACT_CENTER_COLUMN:
        if (!c) return false;
        ws->center_once = true; return true;
    case COMP_ACT_CENTER_VISIBLE_COLUMNS:
        if (!c || L->cfg.center_mode == COMP_CENTER_ALWAYS) return false;
        ws->center_visible_once = true; return true;
    default:
        return false;
    }
}

bool
comp_layout_apply(CompLayout *L, CompAction a)
{
    if (!L || a >= COMP_ACT__COUNT)
        return false;
    Workspace *ws = active_ws(L);
    switch (a) {
    case COMP_ACT_FOCUS_WORKSPACE_UP:
        return L->active ? comp_layout_focus_workspace_index(L, L->active - 1) : false;
    case COMP_ACT_FOCUS_WORKSPACE_DOWN:
        return comp_layout_focus_workspace_index(L, L->active + 1);
    case COMP_ACT_MOVE_COLUMN_TO_WORKSPACE_UP:
        return (!ws->focus_floating && L->active) ? move_column_to_workspace(L, L->active - 1) : false;
    case COMP_ACT_MOVE_COLUMN_TO_WORKSPACE_DOWN:
        return !ws->focus_floating ? move_column_to_workspace(L, L->active + 1) : false;
    case COMP_ACT_MOVE_WORKSPACE_UP:
        return swap_workspace(L, true);
    case COMP_ACT_MOVE_WORKSPACE_DOWN:
        return swap_workspace(L, false);
    case COMP_ACT_TOGGLE_WINDOW_FLOATING: {
        bool ok = toggle_floating(L);
        normalize_workspaces(L);
        return ok;
    }
    case COMP_ACT_SWITCH_FOCUS_FLOATING_TILING:
        if (ws->focus_floating ? ws->n == 0 : ws->nfl == 0) return false;
        ws->focus_floating = !ws->focus_floating;
        return true;
    default:
        break;
    }
    /* Tiling actions act on the tiled layer even if a floating window has focus;
     * performing one moves focus back to tiling. */
    bool was_floating = ws->focus_floating;
    ws->focus_floating = false;
    bool changed = apply_tiling_action(L, a);
    if (!changed)
        ws->focus_floating = was_floating && ws->nfl;
    normalize_workspaces(L);
    return changed;
}

bool
comp_layout_adjust_column_width(CompLayout *L, double delta)
{
    if (!L || !isfinite(delta))
        return false;
    Column *c = focused_col(active_ws(L));
    if (!c)
        return false;
    double nw = clampd(c->width + delta, MIN_COL_WIDTH, 1.0);
    bool changed = fabs(nw - c->width) > WIDTH_EPS || c->maximized;
    c->width     = nw;
    c->maximized = false;
    return changed;
}

bool
comp_layout_adjust_window_height(CompLayout *L, double delta)
{
    if (!L || !isfinite(delta))
        return false;
    Column *c = focused_col(active_ws(L));
    if (!c || c->n < 2 || c->tabbed)
        return false;
    Win *w = &c->wins[c->focus];
    double cur = w->height > 0.0 ? w->height : 1.0 / (double)c->n;
    double nh  = clampd(cur + delta, MIN_WIN_HEIGHT, 1.0);
    bool changed = fabs(nh - w->height) > WIDTH_EPS;
    for (size_t i = 0; i < c->n; i++) {
        if (i != c->focus) {
            c->wins[i].height = 0.0;
            c->wins[i].preset = -1;
        }
    }
    w->height = nh;
    w->preset = -1;
    return changed;
}

/* ───────────────────────── action names ───────────────────────── */

static const char *const k_action_names[COMP_ACT__COUNT] = {
    [COMP_ACT_FOCUS_COLUMN_LEFT]              = "focus-column-left",
    [COMP_ACT_FOCUS_COLUMN_RIGHT]             = "focus-column-right",
    [COMP_ACT_FOCUS_COLUMN_FIRST]             = "focus-column-first",
    [COMP_ACT_FOCUS_COLUMN_LAST]              = "focus-column-last",
    [COMP_ACT_FOCUS_WINDOW_UP]                = "focus-window-up",
    [COMP_ACT_FOCUS_WINDOW_DOWN]              = "focus-window-down",
    [COMP_ACT_MOVE_COLUMN_LEFT]               = "move-column-left",
    [COMP_ACT_MOVE_COLUMN_RIGHT]              = "move-column-right",
    [COMP_ACT_MOVE_COLUMN_TO_FIRST]           = "move-column-to-first",
    [COMP_ACT_MOVE_COLUMN_TO_LAST]            = "move-column-to-last",
    [COMP_ACT_MOVE_WINDOW_UP]                 = "move-window-up",
    [COMP_ACT_MOVE_WINDOW_DOWN]               = "move-window-down",
    [COMP_ACT_CONSUME_INTO_COLUMN_LEFT]       = "consume-window-into-column",
    [COMP_ACT_EXPEL_FROM_COLUMN]              = "expel-window-from-column",
    [COMP_ACT_CONSUME_OR_EXPEL_LEFT]          = "consume-or-expel-window-left",
    [COMP_ACT_CONSUME_OR_EXPEL_RIGHT]         = "consume-or-expel-window-right",
    [COMP_ACT_FOCUS_WORKSPACE_UP]             = "focus-workspace-up",
    [COMP_ACT_FOCUS_WORKSPACE_DOWN]           = "focus-workspace-down",
    [COMP_ACT_MOVE_COLUMN_TO_WORKSPACE_UP]    = "move-column-to-workspace-up",
    [COMP_ACT_MOVE_COLUMN_TO_WORKSPACE_DOWN]  = "move-column-to-workspace-down",
    [COMP_ACT_MOVE_WORKSPACE_UP]              = "move-workspace-up",
    [COMP_ACT_MOVE_WORKSPACE_DOWN]            = "move-workspace-down",
    [COMP_ACT_SWITCH_PRESET_COLUMN_WIDTH]     = "switch-preset-column-width",
    [COMP_ACT_SWITCH_PRESET_WINDOW_HEIGHT]    = "switch-preset-window-height",
    [COMP_ACT_RESET_WINDOW_HEIGHT]            = "reset-window-height",
    [COMP_ACT_MAXIMIZE_COLUMN]                = "maximize-column",
    [COMP_ACT_EXPAND_COLUMN_TO_AVAILABLE]     = "expand-column-to-available-width",
    [COMP_ACT_TOGGLE_COLUMN_TABBED]           = "toggle-column-tabbed",
    [COMP_ACT_TOGGLE_WINDOW_FLOATING]         = "toggle-window-floating",
    [COMP_ACT_SWITCH_FOCUS_FLOATING_TILING]   = "switch-focus-floating-tiling",
    [COMP_ACT_CENTER_COLUMN]                  = "center-column",
    [COMP_ACT_CENTER_VISIBLE_COLUMNS]         = "center-visible-columns",
};

const char *
comp_action_name(CompAction a)
{
    return a < COMP_ACT__COUNT ? k_action_names[a] : NULL;
}

bool
comp_action_from_name(const char *name, CompAction *out)
{
    if (!name)
        return false;
    for (size_t i = 0; i < COMP_ACT__COUNT; i++) {
        if (k_action_names[i] && strcmp(k_action_names[i], name) == 0) {
            if (out)
                *out = (CompAction)i;
            return true;
        }
    }
    return false;
}

/* ───────────────────────── geometry ───────────────────────── */

static double
column_px_width(const CompLayoutConfig *cfg, const Column *c, double view_w)
{
    if (c->fullscreen)
        return fmax(1.0, view_w);
    double w = c->maximized ? view_w - 2.0 * cfg->gap : (view_w - cfg->gap) * c->width - cfg->gap;
    return fmax(1.0, w);
}

static double
column_x(const CompLayoutConfig *cfg, const Workspace *ws, size_t idx, double view_w)
{
    double x = cfg->gap;
    for (size_t i = 0; i < idx && i < ws->n; i++)
        x += column_px_width(cfg, &ws->cols[i], view_w) + cfg->gap;
    return x;
}

static double
strip_width(const CompLayoutConfig *cfg, const Workspace *ws, double view_w)
{
    return ws->n ? column_x(cfg, ws, ws->n, view_w) : 0.0;
}

static void
column_heights(const CompLayoutConfig *cfg, const Column *c, double view_h, double *h)
{
    const double gap   = cfg->gap;
    const double avail = fmax((double)c->n, view_h - gap * (double)(c->n + 1));
    double fixed = 0.0;
    size_t n_auto = 0;
    for (size_t i = 0; i < c->n; i++) {
        if (c->wins[i].height > 0.0) {
            h[i] = fmax(1.0, (view_h - gap) * c->wins[i].height - gap);
            fixed += h[i];
        } else {
            h[i] = 0.0;
            n_auto++;
        }
    }
    double remaining = avail - fixed;
    double min_auto  = (double)n_auto;
    if (remaining < min_auto && fixed > 0.0) {
        double scale = (avail - min_auto) / fixed;
        if (scale < 0.0)
            scale = 0.0;
        for (size_t i = 0; i < c->n; i++)
            if (c->wins[i].height > 0.0)
                h[i] = fmax(1.0, h[i] * scale);
        fixed = 0.0;
        for (size_t i = 0; i < c->n; i++)
            if (c->wins[i].height > 0.0)
                fixed += h[i];
        remaining = avail - fixed;
    }
    if (n_auto) {
        double each = fmax(1.0, remaining / (double)n_auto);
        for (size_t i = 0; i < c->n; i++)
            if (c->wins[i].height <= 0.0)
                h[i] = each;
    } else if (fixed > 0.0 && fixed != avail) {
        for (size_t i = 0; i < c->n; i++)
            h[i] = fmax(1.0, h[i] * avail / fixed);
    }
}

static void
emit(CompWindowGeometry *out, size_t max, size_t *k, CompWindowGeometry g)
{
    if (*k < max && out)
        out[*k] = g;
    (*k)++;
}

size_t
comp_layout_arrange(const CompLayout *L, double view_w, double view_h,
                    CompWindowGeometry *out, size_t max)
{
    if (!L || !(view_w > 0.0) || !(view_h > 0.0) || !isfinite(view_w) || !isfinite(view_h))
        return 0;
    const CompLayoutConfig *cfg = &L->cfg;
    const Workspace *ws = active_ws(L);
    CompWinId focused = comp_layout_focused_window(L);
    size_t k = 0;

    double x = cfg->gap;
    for (size_t ci = 0; ci < ws->n; ci++) {
        const Column *c = &ws->cols[ci];
        double w = column_px_width(cfg, c, view_w);
        if (c->fullscreen) {
            for (size_t i = 0; i < c->n; i++) {
                CompWindowGeometry g = {
                    .id = c->wins[i].id,
                    .rect = { x, 0.0, fmax(1.0, view_w), fmax(1.0, view_h) },
                    .fullscreen = true,
                    .visible = (i == c->focus),
                    .focused = (c->wins[i].id == focused),
                };
                emit(out, max, &k, g);
            }
        } else if (c->tabbed) {
            for (size_t i = 0; i < c->n; i++) {
                CompWindowGeometry g = {
                    .id = c->wins[i].id,
                    .rect = { x, cfg->gap, w, fmax(1.0, view_h - 2.0 * cfg->gap) },
                    .tabbed = true, .tab_index = i, .tab_count = c->n,
                    .visible = (i == c->focus), .focused = (c->wins[i].id == focused),
                };
                emit(out, max, &k, g);
            }
        } else {
            double hs[64];
            double *h = c->n <= 64 ? hs : malloc(c->n * sizeof(double));
            if (!h) {
                k += c->n;
                x += w + cfg->gap;
                continue;
            }
            column_heights(cfg, c, view_h, h);
            double y = cfg->gap;
            for (size_t i = 0; i < c->n; i++) {
                CompWindowGeometry g = {
                    .id = c->wins[i].id, .rect = { x, y, w, h[i] },
                    .visible = true, .focused = (c->wins[i].id == focused),
                };
                emit(out, max, &k, g);
                y += h[i] + cfg->gap;
            }
            if (h != hs)
                free(h);
        }
        x += w + cfg->gap;
    }

    for (size_t f = 0; f < ws->nfl; f++) {
        CompRect r = ws->fl[f].rect;
        if (!(r.w > 0.0) || !(r.h > 0.0)) {
            r.w = fmax(1.0, view_w * 0.5);
            r.h = fmax(1.0, view_h * 0.5);
            r.x = (view_w - r.w) / 2.0;
            r.y = (view_h - r.h) / 2.0;
        }
        CompWindowGeometry g = {
            .id = ws->fl[f].id, .rect = r, .floating = true,
            .visible = true, .focused = (ws->fl[f].id == focused),
        };
        emit(out, max, &k, g);
    }
    return k;
}

double
comp_layout_view_target(CompLayout *L, double view_w, double current)
{
    if (!L || !(view_w > 0.0) || !isfinite(view_w))
        return 0.0;
    if (!isfinite(current))
        current = 0.0;
    const CompLayoutConfig *cfg = &L->cfg;
    Workspace *ws = active_ws(L);
    if (ws->n == 0) {
        ws->center_once = false;
        ws->center_visible_once = false;
        return 0.0;
    }
    const Column *c = &ws->cols[ws->focus];
    double x  = column_x(cfg, ws, ws->focus, view_w);
    double w  = column_px_width(cfg, c, view_w);
    double centered = x + w / 2.0 - view_w / 2.0;

    /* V7: Fullscreen column aligns its left edge to the viewport */
    if (c->fullscreen) {
        ws->center_once = false;
        ws->center_visible_once = false;
        return x;
    }

    if (ws->center_once) {
        ws->center_once = false;
        ws->center_visible_once = false;
        return centered;
    }
    if (ws->center_visible_once) {
        ws->center_visible_once = false;
        size_t first = ws->focus, last = ws->focus;
        while (first > 0) {
            double lx = column_x(cfg, ws, first - 1, view_w);
            if (lx - cfg->gap >= current - WIDTH_EPS)
                first--;
            else
                break;
        }
        while (last + 1 < ws->n) {
            double rx = column_x(cfg, ws, last + 1, view_w) +
                        column_px_width(cfg, &ws->cols[last + 1], view_w);
            if (rx + cfg->gap <= current + view_w + WIDTH_EPS)
                last++;
            else
                break;
        }
        double left_edge  = column_x(cfg, ws, first, view_w);
        double right_edge = column_x(cfg, ws, last, view_w) +
                            column_px_width(cfg, &ws->cols[last], view_w);
        return (left_edge + right_edge) / 2.0 - view_w / 2.0;
    }
    double total = strip_width(cfg, ws, view_w);
    if (cfg->always_center_single_column && total <= view_w + WIDTH_EPS)
        return (total - view_w) / 2.0;
    if (cfg->center_mode == COMP_CENTER_ALWAYS)
        return centered;

    double left  = x - cfg->gap;
    double right = x + w + cfg->gap;
    if (right - left > view_w)
        return cfg->center_mode == COMP_CENTER_ON_OVERFLOW ? centered : left;
    if (left < current)
        return left;
    if (right > current + view_w)
        return right - view_w;
    return current;
}

/* ───────────────────────── invariants ───────────────────────── */

static int
cmp_id(const void *a, const void *b)
{
    CompWinId x = *(const CompWinId *)a, y = *(const CompWinId *)b;
    return x < y ? -1 : (x > y);
}

bool
comp_layout_check_invariants(const CompLayout *L, const char **why)
{
    const char *dummy;
    if (!why)
        why = &dummy;
    if (!L) { *why = "layout is NULL"; return false; }
    if (!comp_layout_config_valid(&L->cfg)) { *why = "invalid config"; return false; }
    if (L->nws == 0) { *why = "no workspaces"; return false; }
    if (L->active >= L->nws) { *why = "active workspace out of range"; return false; }
    if (!workspace_empty(&L->ws[L->nws - 1])) { *why = "last workspace is not empty"; return false; }
    for (size_t w = 0; w + 1 < L->nws; w++) {
        if (w != L->active && workspace_empty(&L->ws[w])) {
            *why = "empty unfocused workspace in the middle";
            return false;
        }
    }
    size_t total = comp_layout_window_count(L);
    CompWinId *ids = total ? malloc(total * sizeof(CompWinId)) : NULL;
    if (total && !ids) { *why = "out of memory while checking"; return false; }
    size_t k = 0;
    bool ok = true;
    for (size_t w = 0; w < L->nws && ok; w++) {
        const Workspace *ws = &L->ws[w];
        if (ws->n && ws->focus >= ws->n) { *why = "column focus out of range"; ok = false; break; }
        if (!ws->n && ws->focus != 0) { *why = "column focus set on empty workspace"; ok = false; break; }
        if (ws->nfl && ws->fl_focus >= ws->nfl) { *why = "floating focus out of range"; ok = false; break; }
        if (ws->focus_floating && !ws->nfl) { *why = "floating focus without floating windows"; ok = false; break; }
        for (size_t c = 0; c < ws->n && ok; c++) {
            const Column *col = &ws->cols[c];
            if (col->n == 0) { *why = "empty column"; ok = false; break; }
            if (col->focus >= col->n) { *why = "window focus out of range"; ok = false; break; }
            if (!(col->width >= MIN_COL_WIDTH - WIDTH_EPS && col->width <= 1.0 + WIDTH_EPS)) {
                *why = "column width out of range"; ok = false; break;
            }
            for (size_t i = 0; i < col->n; i++) {
                if (col->wins[i].id == 0) { *why = "zero window id"; ok = false; break; }
                if (col->wins[i].height < 0.0 || col->wins[i].height > 1.0 + WIDTH_EPS) {
                    *why = "window height out of range"; ok = false; break;
                }
                ids[k++] = col->wins[i].id;
            }
        }
        for (size_t f = 0; f < ws->nfl && ok; f++) {
            if (ws->fl[f].id == 0) { *why = "zero floating id"; ok = false; break; }
            ids[k++] = ws->fl[f].id;
        }
    }
    if (ok && k > 1) {
        qsort(ids, k, sizeof(CompWinId), cmp_id);
        for (size_t i = 1; i < k; i++) {
            if (ids[i] == ids[i - 1]) { *why = "duplicate window id"; ok = false; break; }
        }
    }
    free(ids);
    return ok;
}
