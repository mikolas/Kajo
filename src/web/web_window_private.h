#ifndef KAJO_WEB_WINDOW_PRIVATE_H
#define KAJO_WEB_WINDOW_PRIVATE_H

#include "web_window.h"
#include "web_model.h"
#include "web_tab.h"
#include "web_tab_rail.h"
#include "web_storage.h"
#include "web_adblock.h"
#include "web_schemes.h"
#include "web_sync.h"
#include "web_downloads.h"
#include "web_palette.h"
#include "web_omnibar_ac.h"
#include "../config/kajo_config_store.h"

struct _KajoWebWindow {
    GtkWindow                  *win;
    KajoWebProfileManager      *pm;
    gchar                      *profile_id;
    gboolean                    is_private;
    gboolean                    hw_disabled_override;
    KajoWebWindowAppCallbacks   app_cbs;
    gpointer                    app_ud;

    WebKitWebContext           *web_ctx;
    WebKitNetworkSession       *session;
    KajoWebStorage             *storage;
    KajoWebAdblock             *adblock;
    KajoWebSync                *sync;
    KajoWebDownloads           *downloads;
    KajoWebSchemeContext        scheme_ctx;
    KajoWebModel               *model;
    const KajoWebCfg           *cfg;
    KajoWebRailMode             last_unpinned_rail_mode;
    GHashTable                 *tabs;          /* guint64* id -> KajoWebTab* */
    guint                       cfg_sub_id;
    guint                       suspend_timer_id;
    guint                       sync_chrome_idle_id;
    guint                       focus_omni_idle_id;
    guint                       deferred_load_timer_id;
    guint64                     deferred_initial_tab_id;
    GtkCssProvider             *border_css;
    gchar                      *border_css_class;

    /* Layout & Chrome */
    GtkWidget                  *root_overlay;
    GtkWidget                  *main_vbox;
    GtkWidget                  *top_zen_trigger;
    GtkWidget                  *header_handle;
    GtkWidget                  *btn_back;
    GtkWidget                  *btn_fwd;
    GtkWidget                  *btn_reload;
    GtkWidget                  *btn_pin_rail;
    GtkWidget                  *omni_tls;
    GtkWidget                  *omni_entry;
    KajoWebOmnibarAc           *omni_ac;
    GtkWidget                  *btn_zoom;
    GtkWidget                  *btn_bookmark;
    GtkWidget                  *btn_shield;
    GtkWidget                  *omni_progress;
    GtkWidget                  *chip_sw;
    GtkWidget                  *chip_priv;
    GtkWidget                  *btn_media;
    GtkWidget                  *btn_downloads;
    GtkWidget                  *btn_profile;
    GtkWidget                  *profile_dot;
    GtkWidget                  *profile_lbl;
    GtkWidget                  *btn_menu;

    GtkWidget                  *body_overlay;
    GtkWidget                  *body_hbox;
    GtkWidget                  *viewport_overlay;
    GtkWidget                  *viewport_vbox;
    GtkWidget                  *perm_bar;
    GtkWidget                  *perm_lbl;
    WebKitPermissionRequest    *pending_perm;
    GtkWidget                  *web_stack;
    GtkWidget                  *hover_hud;
    GtkWidget                  *find_bar;
    GtkWidget                  *find_entry;
    GtkWidget                  *find_count_lbl;
    gint                        find_total;
    gint                        find_pos;
    GtkWidget                  *palette;
    KajoWebTabRail             *rail;

    /* Popovers */
    GtkWidget                  *pop_shield;
    GtkWidget                  *pop_media;
    GtkWidget                  *pop_downloads;
    GtkWidget                  *pop_profile;
    GtkWidget                  *pop_menu;
    GtkWidget                  *sync_err_lbl;
    gchar                      *sync_err_msg;

    guint                       zen_hide_timer_id;
    guint                       dl_refresh_timer_id;
    gboolean                    dl_popover_dirty;
    gboolean                    window_fullscreen;
    gboolean                    html5_fullscreen;
    gboolean                    top_autohide;
    gchar                      *cached_bm_uri;
    gboolean                    cached_bm_starred;
    gboolean                    cached_bm_valid;
    gdouble                     last_progress_frac;
    gint                        last_tls_kind;
    gboolean                    updating_omni;
    gboolean                    omni_had_focus_on_press;
    gboolean                    initial_load_unfiltered;
};

/* Helpers shared between web_window.c, web_window_actions.c, and web_window_popovers.c */
KajoWebTab *kajo_web_win_active_tab(KajoWebWindow *w);
void        kajo_web_win_invalidate_bookmark_cache(KajoWebWindow *w);
void        kajo_web_win_sync_chrome(KajoWebWindow *w);
void        kajo_web_win_schedule_sync_chrome(KajoWebWindow *w);
void        kajo_web_win_new_tab(KajoWebWindow *w, const gchar *uri, gboolean background);
void        kajo_web_win_select_tab(KajoWebWindow *w, guint64 tab_id);
void        kajo_web_win_close_tab(KajoWebWindow *w, guint64 tab_id);
guint64     kajo_web_win_add_tab_full(KajoWebWindow *w,
                                      const gchar   *uri,
                                      const gchar   *title,
                                      const gchar   *group,
                                      gboolean       pinned,
                                      gboolean       suspended,
                                      gboolean       defer_load,
                                      gboolean       background,
                                      gboolean       skip_rail_rebuild);
void        kajo_web_win_focus_webview(KajoWebWindow *w);
gboolean    kajo_web_win_is_blank_uri(const gchar *uri);
void        kajo_web_win_toggle_zen_topbar(KajoWebWindow *w);
void        kajo_web_win_toggle_pin_rail(KajoWebWindow *w);
void        kajo_web_win_on_tab_changed(KajoWebTab *tab, gpointer ud);
void        kajo_web_win_cancel_deferred_load(KajoWebWindow *w);
void        kajo_web_win_open_settings(void);
void        kajo_web_win_open_find(KajoWebWindow *w);
void        kajo_web_win_toggle_fullscreen(KajoWebWindow *w);
void        kajo_web_win_set_html5_fullscreen(KajoWebWindow *w, gboolean enter);
void        kajo_web_win_sync_fullscreen_chrome(KajoWebWindow *w);
gboolean    kajo_web_win_handle_scheme_action(KajoWebWindow *w, KajoWebTab *tab, const gchar *uri);
void        kajo_web_win_setup_popovers(KajoWebWindow *w);
void        kajo_web_win_show_shield_popover(KajoWebWindow *w);
void        kajo_web_win_show_media_popover(KajoWebWindow *w);
void        kajo_web_win_show_downloads_popover(KajoWebWindow *w);
void        kajo_web_win_show_profile_popover(KajoWebWindow *w);
void        kajo_web_win_show_menu_popover(KajoWebWindow *w);

/* Handlers in web_window_actions.c */
void        kajo_web_win_fill_rail_callbacks(KajoWebTabRailCallbacks *out);
void        kajo_web_win_fill_palette_callbacks(KajoWebPaletteCallbacks *out);
void        kajo_web_win_wire_omnibar_and_find(KajoWebWindow *w,
                                               GtkWidget     *bf_prev,
                                               GtkWidget     *bf_next,
                                               GtkWidget     *bf_close);
gboolean    kajo_web_win_on_key_pressed(GtkEventControllerKey *c,
                                        guint                  keyval,
                                        guint                  kc,
                                        GdkModifierType        st,
                                        gpointer               ud);

#endif /* KAJO_WEB_WINDOW_PRIVATE_H */
