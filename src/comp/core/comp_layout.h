/* comp_layout.h — scrolling-tiling layout model for kajo-comp (pure C).
 *
 * No wlroots, Vulkan or GLib. The model knows windows only by an opaque
 * 64-bit id chosen by the caller (the compositor maps ids to surfaces), so it
 * can be unit-tested and fuzzed without a display.
 *
 * Model (spec §3.3):
 *   layout → workspaces (vertical stack, one output) → columns (horizontal
 *   strip) → windows (vertical stack inside a column, or tabs);
 *   each workspace also has a list of floating windows.
 * There is always exactly one empty workspace at the end of the stack; other
 * empty workspaces are removed when they are not focused (Niri behaviour).
 *
 * Geometry is computed on demand for a viewport; the layout returns *target*
 * positions. Animating towards them (springs) is the caller's job.
 */
#ifndef KAJO_COMP_LAYOUT_H
#define KAJO_COMP_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COMP_LAYOUT_MAX_PRESETS 8

typedef uint64_t CompWinId; /* 0 is never a valid id */

typedef struct {
    double x, y, w, h;
} CompRect;

typedef enum {
    COMP_CENTER_NEVER = 0,     /* pan minimally so the focused column is visible */
    COMP_CENTER_ON_OVERFLOW,   /* like NEVER, but center a column wider than the view */
    COMP_CENTER_ALWAYS,        /* keep the focused column centered */
} CompCenterMode;

typedef struct {
    double         gap;                          /* px between and around columns/windows, >= 0 */
    double         default_width;                /* proportion of the viewport, (0, 1] */
    double         preset_widths[COMP_LAYOUT_MAX_PRESETS];
    size_t         n_preset_widths;              /* 0..COMP_LAYOUT_MAX_PRESETS */
    double         preset_heights[COMP_LAYOUT_MAX_PRESETS];
    size_t         n_preset_heights;
    CompCenterMode center_mode;
    bool           always_center_single_column;  /* center the strip when it is narrower than the view */
} CompLayoutConfig;

typedef struct CompLayout CompLayout;

/* Fills `cfg` with the defaults used by the shipped comp.toml (live Niri values). */
void        comp_layout_config_defaults(CompLayoutConfig *cfg);

/* Returns NULL on allocation failure or invalid config. */
CompLayout *comp_layout_new(const CompLayoutConfig *cfg);
void        comp_layout_free(CompLayout *layout);
/* Replace the configuration (e.g. hot reload). Returns false and keeps the old one if invalid. */
bool        comp_layout_set_config(CompLayout *layout, const CompLayoutConfig *cfg);
bool        comp_layout_config_valid(const CompLayoutConfig *cfg);

/* ── Windows ── */
/* Adds a tiled window as a new column to the right of the focused column on the
 * focused workspace and focuses it. Returns false if id is 0, already present,
 * or on allocation failure. */
bool        comp_layout_add_window(CompLayout *layout, CompWinId id);
/* Adds a floating window with an initial rect (logical px, viewport-relative). */
bool        comp_layout_add_floating(CompLayout *layout, CompWinId id, CompRect rect);
/* Updates a floating window's rect (e.g. after an interactive move). False if not floating or non-finite. */
bool        comp_layout_set_floating_rect(CompLayout *layout, CompWinId id, CompRect rect);
/* Removes the window wherever it is. Focus moves to a neighbour. */
bool        comp_layout_remove_window(CompLayout *layout, CompWinId id);
bool        comp_layout_contains(const CompLayout *layout, CompWinId id);
CompWinId   comp_layout_focused_window(const CompLayout *layout); /* 0 if none */
/* Focus a specific window, switching workspace if needed. */
bool        comp_layout_focus_window(CompLayout *layout, CompWinId id);
size_t      comp_layout_window_count(const CompLayout *layout);

/* ── Actions (names follow binds.toml, spec §4.3) — each returns true if something changed ── */
typedef enum {
    COMP_ACT_FOCUS_COLUMN_LEFT = 0,
    COMP_ACT_FOCUS_COLUMN_RIGHT,
    COMP_ACT_FOCUS_COLUMN_FIRST,
    COMP_ACT_FOCUS_COLUMN_LAST,
    COMP_ACT_FOCUS_WINDOW_UP,
    COMP_ACT_FOCUS_WINDOW_DOWN,
    COMP_ACT_MOVE_COLUMN_LEFT,
    COMP_ACT_MOVE_COLUMN_RIGHT,
    COMP_ACT_MOVE_COLUMN_TO_FIRST,
    COMP_ACT_MOVE_COLUMN_TO_LAST,
    COMP_ACT_MOVE_WINDOW_UP,              /* within the column; at the top: no-op */
    COMP_ACT_MOVE_WINDOW_DOWN,
    COMP_ACT_CONSUME_INTO_COLUMN_LEFT,    /* move focused window into the column on the left */
    COMP_ACT_EXPEL_FROM_COLUMN,           /* move focused window into its own column on the right */
    COMP_ACT_FOCUS_WORKSPACE_UP,
    COMP_ACT_FOCUS_WORKSPACE_DOWN,
    COMP_ACT_MOVE_COLUMN_TO_WORKSPACE_UP,
    COMP_ACT_MOVE_COLUMN_TO_WORKSPACE_DOWN,
    COMP_ACT_MOVE_WORKSPACE_UP,
    COMP_ACT_MOVE_WORKSPACE_DOWN,
    COMP_ACT_SWITCH_PRESET_COLUMN_WIDTH,
    COMP_ACT_SWITCH_PRESET_WINDOW_HEIGHT,
    COMP_ACT_RESET_WINDOW_HEIGHT,
    COMP_ACT_MAXIMIZE_COLUMN,             /* toggle full width */
    COMP_ACT_TOGGLE_COLUMN_TABBED,
    COMP_ACT_TOGGLE_WINDOW_FLOATING,
    COMP_ACT_SWITCH_FOCUS_FLOATING_TILING,
    COMP_ACT_CENTER_COLUMN,               /* one-shot: center the focused column */
    COMP_ACT__COUNT
} CompAction;

bool        comp_layout_apply(CompLayout *layout, CompAction action);
/* Change the focused column's width by `delta` (proportion, e.g. +0.1), clamped to (0.05, 1]. */
bool        comp_layout_adjust_column_width(CompLayout *layout, double delta);
/* Change the focused window's height proportion by `delta`, clamped to [0.1, 1]. */
bool        comp_layout_adjust_window_height(CompLayout *layout, double delta);

/* Action name table, used by the binds parser and by tests that check docs stay in sync. */
const char *comp_action_name(CompAction action);       /* NULL if out of range */
bool        comp_action_from_name(const char *name, CompAction *out);

/* ── Workspaces ── */
size_t      comp_layout_workspace_count(const CompLayout *layout);
size_t      comp_layout_active_workspace(const CompLayout *layout);
bool        comp_layout_focus_workspace_index(CompLayout *layout, size_t index);

/* ── Geometry for the active workspace ── */
typedef struct {
    CompWinId id;
    CompRect  rect;        /* strip coordinates: x is relative to the strip origin, not the camera */
    bool      floating;    /* floating rects are viewport-relative */
    bool      visible;     /* false for inactive tabs */
    bool      focused;
} CompWindowGeometry;

/* Writes up to `max` entries for the active workspace (tiled first, then floating,
 * bottom to top). Returns the number of windows on that workspace (may exceed `max`). */
size_t      comp_layout_arrange(const CompLayout *layout, double view_w, double view_h,
                                CompWindowGeometry *out, size_t max);
/* Target camera x (strip coordinate shown at the viewport's left edge) for the active
 * workspace, given the camera's current position. Deterministic for a given input. */
double      comp_layout_view_target(CompLayout *layout, double view_w, double current_view_x);

/* Checks structural invariants; on failure returns false and, if `why` is non-NULL,
 * points it at a static description. Used by tests and the fuzzer. */
bool        comp_layout_check_invariants(const CompLayout *layout, const char **why);

#endif /* KAJO_COMP_LAYOUT_H */
