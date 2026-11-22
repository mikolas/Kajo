/* comp_layout_float.c — floating-window operations and action-name table (pure C). */
#include "comp_layout_private.h"

#include <math.h>
#include <string.h>

/* ───────────────────────── floating windows ───────────────────────── */

bool
comp_layout_add_floating_to(Workspace *ws, CompWinId id, CompRect rect)
{
    if (!comp_layout_grow((void **)&ws->fl, &ws->capfl, ws->nfl + 1, sizeof(Floater)))
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
    if (!comp_layout_add_floating_to(comp_layout_active_ws(L), id, rect))
        return false;
    comp_layout_normalize_workspaces(L);
    return true;
}

void
comp_layout_remove_floating_at(Workspace *ws, size_t f)
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
comp_layout_set_floating_rect(CompLayout *L, CompWinId id, CompRect rect)
{
    Loc loc;
    if (!L || !comp_layout_find_window(L, id, &loc) || !loc.floating)
        return false;
    if (!isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.w) || !isfinite(rect.h))
        return false;
    L->ws[loc.ws].fl[loc.fl].rect = rect;
    return true;
}

bool
comp_layout_toggle_floating(CompLayout *L)
{
    Workspace *ws = comp_layout_active_ws(L);
    if (ws->focus_floating && ws->nfl) {
        Floater f = ws->fl[ws->fl_focus];
        comp_layout_remove_floating_at(ws, ws->fl_focus);
        Win win = { .id = f.id, .height = 0.0, .preset = -1 };
        if (!comp_layout_add_tiled_to(L, L->active, win, L->cfg.default_width)) {
            (void)comp_layout_add_floating_to(comp_layout_active_ws(L), f.id, f.rect);
            return false;
        }
        return true;
    }
    Column *c = comp_layout_focused_col(ws);
    if (!c)
        return false;
    CompWinId id = c->wins[c->focus].id;
    if (!comp_layout_add_floating_to(ws, id, (CompRect){ 0 }))
        return false;
    (void)comp_layout_column_take(c, c->focus);
    if (c->n == 0)
        comp_layout_remove_column(ws, ws->focus, true);
    return true;
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
