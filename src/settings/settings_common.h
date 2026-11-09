#ifndef DESKTOP_SETTINGS_COMMON_H
#define DESKTOP_SETTINGS_COMMON_H

#include <gtk/gtk.h>
#include "../config.h"
#include "../theme.h"
#include "../compositor/compositor.h"

G_BEGIN_DECLS

typedef void (*SettingsActionFunc)(gpointer user_data);

typedef struct {
    const char        *secondary_label;
    SettingsActionFunc on_secondary;
    SettingsActionFunc on_revert;
    SettingsActionFunc on_save;
    gpointer           user_data;
    gboolean           has_save_revert;
    gboolean           is_dirty;
    char              *live_badge_text;
    char              *live_status_text;
    char              *cnt_keys[4];
    char              *cnt_vals[4];
    char              *cnt_styles[4];
    char              *deck_badge1;
    char              *deck_badge2;
} SettingsPageState;

typedef struct {
    GtkWidget *window;
    GtkWidget *root_vbox;
    GtkWidget *main_paned;
    GtkWidget *sidebar_vbox;
    GtkWidget *sidebar_list;
    GtkWidget *right_viewport;
    GtkWidget *stack;
    GtkWidget *stack_scroll;

    /* Zone 1: Frameless Header Bar */
    GtkWidget *btn_rail;
    GtkWidget *hdr_title_lbl;
    GtkWidget *hdr_page_badge;
    GtkWidget *search_entry;
    GtkWidget *btn_float;
    GtkWidget *btn_close;

    /* Zone 2: Pinned Page Header & 4-Cell Live Telemetry Strip */
    GtkWidget *deck_box;
    GtkWidget *deck_title_lbl;
    GtkWidget *deck_sub_lbl;
    GtkWidget *deck_badge1_lbl;
    GtkWidget *deck_badge2_lbl;
    GtkWidget *counter_grid;
    GtkWidget *cnt_cells[4];
    GtkWidget *cnt_key_lbls[4];
    GtkWidget *cnt_val_lbls[4];

    /* Zone 4: Docked Action & Persistence Footer Bar */
    GtkWidget *footer_bar;
    GtkWidget *footer_state_badge;
    GtkWidget *footer_status_lbl;
    GtkWidget *footer_actions_box;
    GtkWidget *btn_secondary;
    GtkWidget *btn_revert;
    GtkWidget *btn_save;

    /* Responsive Niri Column Reflow State */
    int        current_width;
    int        sim_col_width;
    gboolean   is_narrow_mode;
    gboolean   is_wide_mode;
    gboolean   is_compact_rail;
    gboolean   rail_force_expanded;
    gboolean   rail_force_compact;
    char      *active_page;
} SettingsApp;

typedef enum {
    SETTINGS_CARD_LEFT = 0,
    SETTINGS_CARD_CENTER,
    SETTINGS_CARD_RIGHT,
    SETTINGS_CARD_PLAIN
} SettingsCardAccent;

/* Sidebar Navigation Builders */
GtkWidget *create_sidebar_category_header(const gchar *category_name);
GtkWidget *create_sidebar_row(const gchar *icon_name,
                              const gchar *label_text,
                              const gchar *subtitle_text,
                              const gchar *page_name);
GtkWidget *create_sidebar_row_with_keywords(const gchar *icon_name,
                                            const gchar *label_text,
                                            const gchar *subtitle_text,
                                            const gchar *page_name,
                                            const gchar *keywords);
GtkWidget *create_settings_nav_row(const gchar *glyph,
                                   const gchar *label_text,
                                   const gchar *badge_text,
                                   const gchar *badge_style,
                                   const gchar *tooltip_text,
                                   const gchar *page_name,
                                   const gchar *keywords);

/* Zone 3 Responsive Bento Canvas & Canonical Section Cards */
GtkWidget *settings_flex_bin_new(GtkWidget *child);
GtkWidget *settings_bento_new(void);
void       settings_bento_add_full(GtkWidget *bento, GtkWidget *card);
void       settings_bento_add_pair(GtkWidget *bento, GtkWidget *card_left, GtkWidget *card_right);
void       settings_bento_update_columns(GtkWidget *bento, gboolean allow_two_col, int avail_canvas_width);
void       settings_update_descendant_scales(GtkWidget *root, gboolean narrow);

GtkWidget *create_section_card(SettingsCardAccent accent,
                               const gchar       *heading_text,
                               const gchar       *badge_text,
                               const gchar       *badge_style_class,
                               GtkWidget         *extra_header_widget);
void       section_card_set_badge(GtkWidget   *card,
                                  const gchar *badge_text,
                                  const gchar *badge_style_class);

/* Canonical Boxed Control Rows & Shrinkable Controls */
GtkWidget *create_boxed_row(const gchar *glyph_or_icon,
                            const gchar *title,
                            const gchar *subtitle,
                            GtkWidget   *right_control);
void       boxed_row_set_subtitle(GtkWidget *row, const gchar *subtitle);
void       boxed_row_set_title(GtkWidget *row, const gchar *title);

GtkWidget *create_action_row(const gchar *icon_name,
                             const gchar *title,
                             const gchar *subtitle,
                             GtkWidget   *control);
GtkWidget *create_string_dropdown(const char *const items[]);
void       settings_make_dropdown_shrinkable(GtkWidget *dropdown);
GtkWidget *create_scale_with_badge(double       min,
                                   double       max,
                                   double       step,
                                   double       initial,
                                   const gchar *badge_fmt,
                                   GtkWidget  **out_scale,
                                   GtkWidget  **out_badge);

/* Toggle card: GtkToggleButton with status line + lit left indicator */
GtkWidget *create_toggle_card(const gchar *icon_name,
                              const gchar *title,
                              const gchar *subtitle,
                              gboolean     active);
void       toggle_card_set_subtitle(GtkWidget *card, const gchar *subtitle);

/* Segmented group: short exclusive choices as joined toggle buttons */
typedef void (*SegmentedChangedFunc)(GtkWidget *group, guint selected, gpointer user_data);
GtkWidget *create_segmented_group(const char *const labels[], guint selected);
void       segmented_group_connect(GtkWidget *group, SegmentedChangedFunc cb, gpointer user_data);
guint      segmented_group_get_selected(GtkWidget *group);
void       segmented_group_set_selected(GtkWidget *group, guint idx);

GtkWidget *create_simple_item_row(const char *icon_name,
                                  const char *title,
                                  const char *subtitle,
                                  GtkWidget  *action_btn);
GtkWidget *create_action_button(const char *label,
                                const char *payload_key,
                                const char *payload_val,
                                GCallback   callback,
                                gpointer    user_data);
GtkWidget *create_empty_state_label(const char *message);

/* Zone 2 Telemetry & Zone 4 Footer Binding API for Pages */
void       settings_page_bind_actions(GtkWidget         *page_root,
                                      const gchar       *secondary_label,
                                      SettingsActionFunc on_secondary,
                                      SettingsActionFunc on_revert,
                                      SettingsActionFunc on_save,
                                      gpointer           user_data);
void       settings_page_set_dirty(GtkWidget *page_widget, gboolean dirty);
void       settings_page_set_live_status(GtkWidget   *page_widget,
                                         const gchar *badge_text,
                                         const gchar *status_text);
void       settings_page_set_counter(GtkWidget   *page_widget,
                                     int          cell_idx,
                                     const gchar *key_text,
                                     const gchar *val_text,
                                     const gchar *style_class);
void       settings_page_set_deck_badges(GtkWidget   *page_widget,
                                         const gchar *badge1,
                                         const gchar *badge2);
void       settings_page_set_nav_badge(GtkWidget   *any_widget_in_win,
                                       const gchar *page_name,
                                       const gchar *badge_text,
                                       const gchar *badge_style);
void       settings_window_sync_active_page_chrome(SettingsApp *app_ctx);

/* IPC, Theme & String Utilities */
const gchar *settings_accent_short_name(const gchar *hex_color);
gboolean     settings_send_ipc_command(const gchar *command);
void         settings_set_global_theme(ShellTheme *theme);
void         settings_reload_global_theme(void);
gchar       *format_mac_address_from_bluez_path(const char *path);
gboolean     is_mac_address_string(const char *str);

G_END_DECLS

#endif /* DESKTOP_SETTINGS_COMMON_H */
