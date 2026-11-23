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
    GHashTable                 *tabs;          /* guint64* id -> KajoWebTab* */
    guint                       cfg_sub_id;
    guint                       suspend_timer_id;
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
    GtkWidget                  *btn_shield;
    GtkWidget                  *omni_progress;
    GtkWidget                  *chip_sw;
    GtkWidget                  *chip_priv;
    GtkWidget                  *btn_media;
    GtkWidget                  *btn_downloads;
    GtkWidget                  *btn_profile;
    GtkWidget                  *profile_dot;
    GtkWidget                  *profile_lbl;

    GtkWidget                  *body_hbox;
    GtkWidget                  *viewport_overlay;
    GtkWidget                  *viewport_vbox;
    GtkWidget                  *perm_bar;
    GtkWidget                  *perm_lbl;
    WebKitPermissionRequest    *pending_perm;
    GtkWidget                  *web_stack;
    GtkWidget                  *find_bar;
    GtkWidget                  *find_entry;
    GtkWidget                  *palette;
    KajoWebTabRail             *rail;

    /* Popovers */
    GtkWidget                  *pop_shield;
    GtkWidget                  *pop_media;
    GtkWidget                  *pop_downloads;
    GtkWidget                  *pop_profile;

    gboolean                    top_autohide;
    gboolean                    updating_omni;
};

/* Helpers shared between web_window.c and web_window_popovers.c */
KajoWebTab *kajo_web_win_active_tab(KajoWebWindow *w);
void        kajo_web_win_sync_chrome(KajoWebWindow *w);
void        kajo_web_win_open_settings(void);
void        kajo_web_win_setup_popovers(KajoWebWindow *w);
void        kajo_web_win_show_shield_popover(KajoWebWindow *w);
void        kajo_web_win_show_media_popover(KajoWebWindow *w);
void        kajo_web_win_show_downloads_popover(KajoWebWindow *w);
void        kajo_web_win_show_profile_popover(KajoWebWindow *w);

#endif /* KAJO_WEB_WINDOW_PRIVATE_H */
