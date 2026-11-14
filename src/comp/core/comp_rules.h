/* comp_rules.h — rules.toml parser and matcher for kajo-comp (pure C).
 *
 * Format (spec §5.2.3):
 *   [[rule]]
 *   app_id = ['^steam$']                        # non-empty array of regexes (any may match)
 *   title  = ['^notificationtoasts_\d+_desktop$']
 *   floating = true
 *   anchor_corner = "bottom-right"
 *   anchor_x = 10
 *   anchor_y = 10
 *   focus = false
 *
 *   [[layer_rule]]
 *   namespace = ['^kajo$', '^kajo-launcher$']
 *   opacity = 0.85
 *
 * Regexes use PCRE2 syntax (\d, \., anchors), UTF-8, case-insensitive by
 * default (PCRE2_CASELESS; opt into exact case via `case_sensitive = true`),
 * unanchored unless the pattern anchors itself. Inside one rule, every given
 * match field must match. Rules apply in file order; later rules override
 * earlier ones for each property they set.
 *
 * Invalid entries are skipped with a diagnostic; a TOML syntax error rejects
 * the whole file (the caller keeps its previous rules).
 */
#ifndef KAJO_COMP_RULES_H
#define KAJO_COMP_RULES_H

#include "comp_diag.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    COMP_CORNER_TOP_LEFT = 0,
    COMP_CORNER_TOP_RIGHT,
    COMP_CORNER_BOTTOM_LEFT,
    COMP_CORNER_BOTTOM_RIGHT,
} CompCorner;

/* Result of matching all window rules. Only fields whose has_* flag is set were
 * set by some rule; everything else keeps the compositor's default. */
typedef struct {
    bool       has_floating;      bool   floating;
    bool       has_width;         double width;          /* column proportion (0.05..1] */
    bool       has_focus;         bool   focus;          /* focus on open */
    bool       has_anchor;        CompCorner corner; double anchor_x, anchor_y;  /* floating offset, logical px */
    bool       has_scroll_factor; double scroll_factor;  /* 0.01..10 */
    bool       has_vrr;           bool   vrr;
    bool       has_shadow;        bool   shadow;
} CompWindowRuleResult;

typedef struct {
    bool   has_opacity;       double opacity;        /* 0..1 */
    bool   has_popup_opacity; double popup_opacity;  /* 0..1 */
    bool   has_shadow;        bool   shadow;
} CompLayerRuleResult;

typedef struct CompRules CompRules;

CompRules *comp_rules_new(void);
CompRules *comp_rules_parse(const char *src, size_t len, const char *source_name, CompDiagList *diag);
/* Parses any `reset_rules = true`, `[[rule]]`, and `[[layer_rule]]` blocks in `src`
 * and appends them into `rules`. Returns false on TOML syntax error. */
bool       comp_rules_merge_toml(CompRules *rules, const char *src, size_t len,
                                 const char *source_name, CompDiagList *diag);
void       comp_rules_free(CompRules *rules);
size_t     comp_rules_window_count(const CompRules *rules);
size_t     comp_rules_layer_count(const CompRules *rules);

/* NULL app_id/title/namespace are treated as "". */
void       comp_rules_match_window(const CompRules *rules, const char *app_id, const char *title,
                                   CompWindowRuleResult *out);
void       comp_rules_match_layer(const CompRules *rules, const char *layer_namespace,
                                  CompLayerRuleResult *out);

#endif /* KAJO_COMP_RULES_H */
