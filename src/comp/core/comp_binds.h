/* comp_binds.h — binds.toml parser and lookup for kajo-comp (pure C).
 *
 * Format (spec §5.2.2):
 *   [[bind]]
 *   key = "mod+shift+h"
 *   action = "move-column-left"
 *
 *   [[bind]]
 *   keys = ["mod+left", "mod+h", "mod+wheel-left"]
 *   action = "focus-column-left"
 *
 *   [[bind]]
 *   key = "mod+t"
 *   spawn = ["kitty"]
 *
 *   [[bind]]
 *   key = "mod+q"
 *   action = "close-window"
 *   repeat = false
 *
 *   [[bind]]
 *   key = "mod+wheel-down"
 *   action = "focus-workspace-down"
 *   cooldown_ms = 150
 *
 *   [[bind]]
 *   key = "mod+minus"
 *   action = "set-column-width"
 *   delta = "-10%"
 *
 * Key specs are `modifier+...+key` (all lowercase). Modifiers: mod (meaning
 * chosen by the caller, normally super), super/win/logo, ctrl/control, shift, alt.
 * Key is a built-in lowercase alias (volume-up, volume-down, volume-mute,
 * mic-mute, brightness-up, brightness-down, media-play, media-next, media-prev,
 * page-up, page-down, bracket-left, bracket-right, wheel-up, wheel-down,
 * wheel-left, wheel-right) or an xkbcommon keysym name matched case-insensitively
 * and stored in lower case.
 *
 * Bad entries are skipped with a diagnostic; a TOML syntax error rejects the
 * whole file (the caller keeps its previous binds).
 */
#ifndef KAJO_COMP_BINDS_H
#define KAJO_COMP_BINDS_H

#include "comp_diag.h"
#include "comp_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    COMP_MOD_SHIFT = 1u << 0,
    COMP_MOD_CTRL  = 1u << 1,
    COMP_MOD_ALT   = 1u << 2,
    COMP_MOD_SUPER = 1u << 3,
} CompModMask;

typedef enum {
    COMP_TRIGGER_KEY = 0,
    COMP_TRIGGER_WHEEL_UP,
    COMP_TRIGGER_WHEEL_DOWN,
    COMP_TRIGGER_WHEEL_LEFT,
    COMP_TRIGGER_WHEEL_RIGHT,
} CompTrigger;

typedef enum {
    COMP_BIND_LAYOUT = 0,        /* layout.action */
    COMP_BIND_SPAWN,             /* argv */
    COMP_BIND_CLOSE_WINDOW,
    COMP_BIND_FULLSCREEN_WINDOW,
    COMP_BIND_NEXT_WINDOW,       /* MRU switcher; scope/filter below */
    COMP_BIND_PREVIOUS_WINDOW,
    COMP_BIND_SET_COLUMN_WIDTH,  /* delta */
    COMP_BIND_SET_WINDOW_HEIGHT, /* delta */
    COMP_BIND_BREAK_SHORTCUTS_INHIBIT,
    COMP_BIND_RELOAD_CONFIG,
    COMP_BIND_QUIT,
    COMP_BIND_SCREENSHOT,
    COMP_BIND_SCREENSHOT_WINDOW,
    COMP_BIND_SCREENSHOT_SCREEN,
} CompBindKind;

typedef enum {
    COMP_SWITCH_SCOPE_ALL = 0,
    COMP_SWITCH_SCOPE_OUTPUT,
    COMP_SWITCH_SCOPE_WORKSPACE,
} CompSwitchScope;

typedef enum {
    COMP_DELTA_REL_PROP = 0, /* "+10%", "-10%" */
    COMP_DELTA_SET_PROP,     /* "50%" */
    COMP_DELTA_REL_PX,       /* "+20", "-20", "+20px", "-20px" */
    COMP_DELTA_SET_PX,       /* "800", "800px" */
} CompDeltaMode;

typedef struct {
    uint32_t        mods;            /* CompModMask */
    CompTrigger     trigger;
    uint32_t        keysym;          /* lower-case keysym when trigger == KEY */
    char           *spec;            /* normalized key spec, e.g. "super+shift+s" */

    CompBindKind    kind;
    CompAction      layout_action;   /* COMP_BIND_LAYOUT */
    char          **argv;            /* COMP_BIND_SPAWN, NULL-terminated */
    CompDeltaMode   delta_mode;      /* SET_COLUMN_WIDTH / SET_WINDOW_HEIGHT */
    double          delta;           /* proportion or logical px, SET_* kinds */
    CompSwitchScope scope;           /* NEXT/PREVIOUS_WINDOW */
    bool            same_app;        /* NEXT/PREVIOUS_WINDOW, filter = "app-id" */

    bool            repeat;          /* default true; false: ignore key repeats */
    int             cooldown_ms;     /* 0..10000, default 0 */
    bool            allow_locked;
    bool            allow_inhibiting; /* default true; false: bypass keyboard-shortcuts-inhibit */
    int             line;            /* source line, for messages */
} CompBind;

typedef struct CompBinds CompBinds;

/* `mod_meaning` is the CompModMask that "mod" stands for (normally COMP_MOD_SUPER).
 * Returns NULL on a TOML syntax error or allocation failure (details in `diag`). */
CompBinds      *comp_binds_new(void);
CompBinds      *comp_binds_parse(const char *src, size_t len, const char *source_name,
                                 uint32_t mod_meaning, CompDiagList *diag);
/* Parses any `[[bind]]` blocks in `src` and merges them into `binds`. When
 * `replace_existing` is true, a later binding for the same chord replaces an
 * earlier one, and `action = "none"` removes any existing binding for that chord.
 * Returns false on TOML syntax error. */
bool            comp_binds_merge_toml(CompBinds *binds, const char *src, size_t len,
                                      const char *source_name, uint32_t mod_meaning,
                                      bool replace_existing, CompDiagList *diag);
void            comp_binds_free(CompBinds *binds);
size_t          comp_binds_count(const CompBinds *binds);
const CompBind *comp_binds_get(const CompBinds *binds, size_t index);

/* Parse a column/window size delta string ("+10%", "-10%", "50%", "+20", "800"). */
bool            comp_binds_parse_delta(const char *s, double *out, CompDeltaMode *mode_out);

/* Lookup. `keysym` should be the keysym the key produces without Shift
 * (xkbcommon level 0); it is lower-cased before matching. */
const CompBind *comp_binds_lookup_key(const CompBinds *binds, uint32_t mods, uint32_t keysym);
const CompBind *comp_binds_lookup_wheel(const CompBinds *binds, uint32_t mods, CompTrigger wheel);

/* Names accepted as actions, for docs/tests. Layout actions come from
 * comp_action_name(); these are the compositor-level ones. */
const char     *comp_bind_kind_name(CompBindKind kind);

#endif /* KAJO_COMP_BINDS_H */
