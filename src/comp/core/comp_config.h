/* comp_config.h — multi-file TOML configuration loader & merger for kajo-comp (pure C).
 *
 * Implements spec §5.1 & §5.2:
 *   - 100% lowercase, zero inline `{ ... }` tables, uniform `[section]` / `[[block]]`.
 *   - Deterministic load order:
 *       1. theme.toml + shell.toml (accent_color + [touch])
 *       2. comp.toml, outputs.toml, binds.toml, rules.toml (with defaults fallback)
 *       3. top-level `include = ["..."]` globs (sorted, cycle-checked, depth <= 4)
 *       4. conf.d/<name>.toml drop-ins in lexicographic order (syntax error in one drop-in
 *          skips only that drop-in; syntax error in a base file rejects the reload).
 *   - Multi-file merging:
 *       - [[rule]] / [[layer_rule]]: concatenated in load order (`reset_rules = true` resets)
 *       - [[bind]]: merged by normalized chord (`action = "none"` unbinds)
 *       - [[output]]: merged field-by-field by case-insensitive `match` string
 *       - [env]: merged by variable name
 *       - [autostart].commands: concatenated (`reset = true` clears earlier commands)
 */
#ifndef KAJO_COMP_CONFIG_H
#define KAJO_COMP_CONFIG_H

#include "comp_binds.h"
#include "comp_diag.h"
#include "comp_fx_core.h"
#include "comp_layout.h"
#include "comp_motion.h"
#include "comp_rules.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    COMP_VRR_OFF       = 0,
    COMP_VRR_ON        = 1,
    COMP_VRR_ON_DEMAND = 2,
} CompVrrPolicy;

typedef enum {
    COMP_TRANSFORM_NORMAL      = 0,
    COMP_TRANSFORM_90          = 1,
    COMP_TRANSFORM_180         = 2,
    COMP_TRANSFORM_270         = 3,
    COMP_TRANSFORM_FLIPPED     = 4,
    COMP_TRANSFORM_FLIPPED_90  = 5,
    COMP_TRANSFORM_FLIPPED_180 = 6,
    COMP_TRANSFORM_FLIPPED_270 = 7,
} CompTransform;

typedef enum {
    COMP_ACCEL_FLAT     = 0,
    COMP_ACCEL_ADAPTIVE = 1,
} CompAccelProfile;

typedef enum {
    COMP_SWIPE_NONE           = 0,
    COMP_SWIPE_LAUNCHER       = 1,
    COMP_SWIPE_OVERVIEW       = 2,
    COMP_SWIPE_OSK            = 3,
    COMP_SWIPE_CONTROL_CENTER = 4,
    COMP_SWIPE_NOTIFICATIONS  = 5,
    COMP_SWIPE_COLUMN         = 6,
    COMP_SWIPE_WORKSPACE      = 7,
} CompTouchSwipeAction;

bool                  comp_vrr_policy_from_str(const char *s, CompVrrPolicy *out);
const char           *comp_vrr_policy_to_str(CompVrrPolicy policy);

bool                  comp_transform_from_str(const char *s, CompTransform *out);
const char           *comp_transform_to_str(CompTransform tf);

bool                  comp_center_mode_from_str(const char *s, CompCenterMode *out);
const char           *comp_center_mode_to_str(CompCenterMode mode);

bool                  comp_accel_profile_from_str(const char *s, CompAccelProfile *out);
const char           *comp_accel_profile_to_str(CompAccelProfile profile);

bool                  comp_touch_swipe_from_str(const char *s, CompTouchSwipeAction *out);
const char           *comp_touch_swipe_to_str(CompTouchSwipeAction action);

typedef struct {
    char *key;
    char *value;
} CompEnvVar;

typedef struct {
    char         *match;          /* lowercase connector ("edp-1") or EDID ("samsung display corp. 0x41aa") */
    bool          has_mode;
    char         *mode;           /* "preferred" or "WxH@Hz" */
    bool          has_scale;
    double        scale;          /* 0.5..4.0 */
    bool          has_transform;
    CompTransform transform;      /* COMP_TRANSFORM_* */
    bool          has_vrr;
    CompVrrPolicy vrr;            /* COMP_VRR_* */
} CompOutputRule;

typedef struct CompConfig {
    uint32_t             mod_mask;
    CompLayoutConfig     layout;
    int32_t              border_px;

    float                accent_rgba[4];
    float                active_border_rgba[4];
    bool                 active_border_tracks_accent;
    float                inactive_border_rgba[4];
    float                urgent_border_rgba[4];

    float                switcher_active_rgba[4];
    bool                 switcher_active_tracks_accent;
    float                switcher_urgent_rgba[4];

    CompMotionConfig     motion;
    CompFxConfig         fx;

    char                *screenshot_path;
    bool                 screenshot_copy;
    bool                 screenshot_notify;
    bool                 screenshot_border;

    bool                 xwayland_enabled;
    bool                 xwayland_force_zero_scaling;

    char                *xkb_layout;
    char                *xkb_options;
    bool                 numlock_on_startup;
    int32_t              repeat_rate;
    int32_t              repeat_delay;

    bool                 touchpad_tap;
    bool                 touchpad_natural_scroll;
    bool                 touchpad_dwt;

    CompAccelProfile     mouse_accel_profile; /* COMP_ACCEL_FLAT | COMP_ACCEL_ADAPTIVE */
    double               mouse_accel_speed;   /* -1.0..1.0 */
    bool                 mouse_natural_scroll;

    bool                 touch_enabled;
    CompTouchSwipeAction touch_swipe_center;
    CompTouchSwipeAction touch_swipe_right;
    CompTouchSwipeAction touch_swipe_horizontal;
    int32_t              touch_dwell_ms;

    CompEnvVar          *env;
    size_t               n_env;

    char              ***autostart;           /* array of NULL-terminated argv arrays */
    size_t               n_autostart;

    CompOutputRule      *outputs;
    size_t               n_outputs;

    CompBinds           *binds;
    CompRules           *rules;
} CompConfig;

CompConfig           *comp_config_new_defaults(uint32_t mod_mask);
void                  comp_config_free(CompConfig *cfg);

/* Action of a horizontal 3-finger drag (spec §3.15 GS5). `[touch].swipe_horizontal`
 * applies to the touchscreen only; the touchpad always scrolls columns. */
CompTouchSwipeAction  comp_config_horizontal_drag_action(const CompConfig *cfg, bool touchpad);

/* Merge a single TOML buffer into `cfg`. `base_dir` is used to resolve relative
 * `include = [...]` globs when `allow_includes` is true. Returns false on TOML
 * syntax error. */
bool                  comp_config_merge_toml(CompConfig *cfg, const char *src, size_t len,
                                             const char *source_name, const char *base_dir,
                                             bool replace_binds, CompDiagList *diag);

/* Load a complete `CompConfig` from `config_dir` (with per-file fallback to
 * `defaults_dir`), `include` globs, and `conf.d/<name>.toml`. If `override_comp_path`
 * is non-NULL, it replaces `comp.toml`. Returns NULL if a base file has a TOML
 * syntax error. */
CompConfig           *comp_config_load_dir(const char *config_dir,
                                           const char *defaults_dir,
                                           const char *override_comp_path,
                                           uint32_t mod_mask,
                                           CompDiagList *diag);

/* Load a locked-down greeter `CompConfig` (`--greeter-mode`, spec §6.5.3).
 * Accepts only `[[output]]` and `[input.*]` sections; never reads user
 * `~/.config/kajo/` files. Returns NULL if an explicit `override_path` is
 * unreadable or has a TOML syntax error. */
bool                  comp_config_merge_greeter_toml(CompConfig *cfg, const char *src, size_t len,
                                                     const char *source_name, CompDiagList *diag);
CompConfig           *comp_config_load_greeter(const char *override_path,
                                               uint32_t mod_mask,
                                               CompDiagList *diag);

/* Find the merged output rule matching connector `name` (e.g. "eDP-1") or
 * `make_model` (e.g. "Samsung Display Corp. 0x41AA"). Returns NULL if none. */
const CompOutputRule *comp_config_match_output(const CompConfig *cfg,
                                               const char *connector_name,
                                               const char *make_model);

#endif /* KAJO_COMP_CONFIG_H */
