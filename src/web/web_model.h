#ifndef KAJO_WEB_MODEL_H
#define KAJO_WEB_MODEL_H

#include <glib.h>
#include "../config/kajo_config_types.h"

G_BEGIN_DECLS

/* ─── Autohide Vertical Tab Rail State Machine ─── */

typedef enum {
    KAJO_RAIL_FSM_COLLAPSED = 0,
    KAJO_RAIL_FSM_REVEAL_WAIT,
    KAJO_RAIL_FSM_EXPANDED,
    KAJO_RAIL_FSM_HIDE_WAIT,
} KajoWebRailFsmState;

typedef enum {
    KAJO_RAIL_ACT_NONE = 0,
    KAJO_RAIL_ACT_START_REVEAL_TIMER,
    KAJO_RAIL_ACT_CANCEL_REVEAL_TIMER,
    KAJO_RAIL_ACT_EXPAND,
    KAJO_RAIL_ACT_START_HIDE_TIMER,
    KAJO_RAIL_ACT_CANCEL_HIDE_TIMER,
    KAJO_RAIL_ACT_COLLAPSE,
} KajoWebRailFsmAction;

typedef struct {
    KajoWebRailFsmState state;
    gboolean            pointer_on_trigger;
    gboolean            pointer_on_rail;
    gint                hold_count;
} KajoWebRailFsm;

void                 web_rail_fsm_init(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_trigger_enter(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_trigger_leave(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_rail_enter(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_rail_leave(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_reveal_timeout(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_hide_timeout(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_trigger_click(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_keyboard_reveal(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_escape(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_focus_lost(KajoWebRailFsm *fsm);
void                 web_rail_fsm_hold(KajoWebRailFsm *fsm);
KajoWebRailFsmAction web_rail_fsm_release(KajoWebRailFsm *fsm);

/* ─── Pure Tab, Group, MRU & Undo-Close Model ─── */

typedef struct {
    guint64   id;
    gchar    *title;
    gchar    *uri;
    gchar    *group;          /* NULL or group name e.g. "WORK" */
    gchar    *favicon_url;    /* NULL or favicon URI */
    gboolean  pinned;
    gboolean  audible;
    gboolean  muted;
    gboolean  loading;
    gboolean  suspended;
    gboolean  capture_active;  /* WebRTC / mic / cam active */
    gboolean  download_active; /* Active download initiated by tab */
    gint64    last_active_us;  /* g_get_monotonic_time() */
} KajoWebTabMeta;

typedef struct {
    gchar   *title;
    gchar   *uri;
    gchar   *group;
    gchar   *favicon_url;
    gboolean pinned;
} KajoWebClosedTab;

typedef struct {
    GPtrArray  *tabs;          /* KajoWebTabMeta* */
    GArray     *mru_ids;       /* guint64, most recent at index 0 */
    GPtrArray  *closed_stack;  /* KajoWebClosedTab*, LIFO */
    GHashTable *collapsed_grps;/* gchar* -> GINT_TO_POINTER(1) */
    guint64     active_id;
    guint64     next_id;
} KajoWebModel;

KajoWebModel    *web_model_new(void);
void             web_model_free(KajoWebModel *m);

KajoWebTabMeta  *web_model_add_tab(KajoWebModel *m,
                                   const gchar  *uri,
                                   const gchar  *title,
                                   const gchar  *group,
                                   gboolean      pinned,
                                   gint64        now_us);
KajoWebTabMeta  *web_model_add_tab_ex(KajoWebModel *m,
                                      const gchar  *uri,
                                      const gchar  *title,
                                      const gchar  *group,
                                      gboolean      pinned,
                                      gboolean      activate,
                                      gint64        now_us);
KajoWebTabMeta  *web_model_add_tab_after(KajoWebModel *m,
                                         guint64       after_tab_id,
                                         const gchar  *uri,
                                         const gchar  *title,
                                         const gchar  *group,
                                         gboolean      pinned,
                                         gboolean      activate,
                                         gint64        now_us);
gboolean         web_model_close_tab(KajoWebModel *m, guint64 id);
KajoWebTabMeta  *web_model_reopen_closed(KajoWebModel *m, gint64 now_us);
gboolean         web_model_activate(KajoWebModel *m, guint64 id, gint64 now_us);
void             web_model_set_tab_favicon(KajoWebModel *m, guint64 id, const gchar *favicon_url);
gboolean         web_model_set_pinned(KajoWebModel *m, guint64 id, gboolean pinned);
gboolean         web_model_move_tab(KajoWebModel *m, guint64 id, guint target_index);
KajoWebTabMeta  *web_model_find_tab(KajoWebModel *m, guint64 id);
KajoWebTabMeta  *web_model_get_active(KajoWebModel *m);
guint64          web_model_cycle_mru(KajoWebModel *m, gboolean forward);
guint64          web_model_cycle_visual(const KajoWebModel *m, gboolean forward);
gboolean         web_model_can_suspend(const KajoWebModel   *m,
                                       const KajoWebTabMeta *t,
                                       gint                  suspend_idle_min,
                                       gint64                now_us);
void             web_model_set_group_collapsed(KajoWebModel *m, const gchar *group, gboolean collapsed);
gboolean         web_model_is_group_collapsed(const KajoWebModel *m, const gchar *group);

/* ─── Omnibar URL vs. Bang / Search Classifier ─── */

gchar *web_model_resolve_input(const gchar         *raw_input,
                               KajoWebSearchEngine  engine,
                               const gchar         *custom_search_url);
gchar *web_model_resolve_cli_arg(const gchar       *arg);
gchar *web_model_extract_host(const gchar *uri);

/* ─── Multi-Profile Registry (profiles.toml) ─── */

typedef struct {
    gchar    *id;           /* sanitized slug: "default", "work", "personal" */
    gchar    *name;         /* display label: "DEFAULT", "WORK" */
    gchar    *icon;         /* glyph: "🌐", "💼", "🏠" */
    gchar    *accent_color; /* "#0078d4" */
    gchar    *google_email; /* "" if local only */
    gboolean  sync_enabled;
    gint64    last_sync_epoch;
} KajoWebProfileInfo;

typedef struct {
    GPtrArray *profiles;    /* KajoWebProfileInfo* */
    gchar     *last_profile_id;
} KajoWebProfileRegistry;

KajoWebProfileRegistry *web_profile_registry_new(void);
void                    web_profile_registry_free(KajoWebProfileRegistry *reg);
gchar                  *web_profile_sanitize_id(const gchar *name);
KajoWebProfileInfo     *web_profile_registry_find(KajoWebProfileRegistry *reg, const gchar *id);
KajoWebProfileInfo     *web_profile_registry_add(KajoWebProfileRegistry *reg,
                                                 const gchar            *name,
                                                 const gchar            *icon,
                                                 const gchar            *accent_color);
gboolean                web_profile_registry_remove(KajoWebProfileRegistry *reg, const gchar *id);
gboolean                web_profile_registry_load(KajoWebProfileRegistry *reg, const gchar *filepath);
gboolean                web_profile_registry_save(const KajoWebProfileRegistry *reg,
                                                  const gchar                  *filepath,
                                                  GError                      **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoWebModel, web_model_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(KajoWebProfileRegistry, web_profile_registry_free)

G_END_DECLS

#endif /* KAJO_WEB_MODEL_H */
