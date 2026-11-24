#include "web_tab_rail.h"
#include <string.h>

#define KAJO_WEB_COMPACT_RAIL_PX 48

typedef struct {
    guint64  id;
    gboolean pinned;
    gchar   *group;
    gboolean collapsed;
    gboolean visible_in_list;
} RailSnapEntry;

typedef struct {
    guint64     tab_id;
    gboolean    is_pinned;
    gboolean    active;
    gboolean    suspended;
    gboolean    muted;
    gboolean    wants_mute_btn;
    gboolean    wants_audio_dot;
    GtkWidget  *widget;     /* tab_list row box OR pin_grid button (ref owned) */
    GtkWidget  *sel_btn;    /* NULL when is_pinned */
    GtkWidget  *ico_stack;  /* "badge" | "favicon" | "spinner" */
    GtkWidget  *audio_dot;  /* 6x6 compact audio indicator dot */
    GtkWidget  *badge_lbl;
    GtkWidget  *fav_img;
    GtkWidget  *loading_lbl;
    GtkWidget  *text_vbox;  /* NULL when is_pinned */
    GtkWidget  *title_lbl;
    GtkWidget  *domain_lbl;
    GtkWidget  *mute_btn;
    GtkWidget  *close_btn;
    GdkTexture *last_favicon;
    GtkWidget  *tick;       /* ref owned */
} TabRowWidgets;

struct _KajoWebTabRail {
    KajoWebTabRailCallbacks cbs;
    gpointer                ud;
    KajoWebRailFsm          fsm;
    KajoWebRailMode         mode;
    KajoWebRailSide         side;
    gint                    width_px;
    gint                    reveal_delay_ms;
    gint                    hide_delay_ms;
    gboolean                show_domains;

    guint                   reveal_timer_id;
    guint                   hide_timer_id;

    GtkWidget              *trigger;        /* rgba(0,0,0,0.01) edge trigger strip */
    GtkWidget              *ticks_box;      /* 2px vertical tab ticks inside trigger */
    GtkWidget              *compact_spacer; /* 48px docked spacer in compact mode */
    GtkWidget              *root;           /* main rail container */
    GtkWidget              *pin_grid;       /* pinned square tiles */
    GtkWidget              *tab_list;       /* scrollable vertical list */
    GtkWidget              *footer_box;
    GtkWidget              *btn_new;
    GtkWidget              *btn_new_lbl;
    GtkWidget              *btn_undo;
    GtkWidget              *active_ctx_pop;

    GHashTable             *row_map;        /* guint64* tab_id -> TabRowWidgets* */
    GPtrArray              *snap;           /* RailSnapEntry* */
    GPtrArray              *compact_hide;   /* group header labels/counts hidden in compact */
};

static guint64 *
tab_id_key(guint64 id)
{
    guint64 *p = g_new(guint64, 1);
    *p = id;
    return p;
}

static void
rail_snap_entry_free(gpointer ptr)
{
    RailSnapEntry *e = ptr;
    if (!e)
        return;
    g_free(e->group);
    g_free(e);
}

static void
tab_row_widgets_free(gpointer ptr)
{
    TabRowWidgets *rw = ptr;
    if (!rw)
        return;
    g_clear_object(&rw->last_favicon);
    g_clear_object(&rw->widget);
    g_clear_object(&rw->tick);
    g_free(rw);
}

static void
cancel_timers(KajoWebTabRail *rail)
{
    if (rail->reveal_timer_id) {
        g_source_remove(rail->reveal_timer_id);
        rail->reveal_timer_id = 0;
    }
    if (rail->hide_timer_id) {
        g_source_remove(rail->hide_timer_id);
        rail->hide_timer_id = 0;
    }
}

static void
sync_row_compact_visibility(TabRowWidgets *rw, gboolean show_full)
{
    if (!rw)
        return;
    if (rw->audio_dot)
        gtk_widget_set_visible(rw->audio_dot, (rw->is_pinned || !show_full) && rw->wants_audio_dot);
    if (rw->is_pinned)
        return;
    if (rw->text_vbox)
        gtk_widget_set_visible(rw->text_vbox, show_full);
    if (rw->close_btn)
        gtk_widget_set_visible(rw->close_btn, show_full);
    if (rw->mute_btn)
        gtk_widget_set_visible(rw->mute_btn, show_full && rw->wants_mute_btn);
}

static void
sync_visual_state(KajoWebTabRail *rail)
{
    gboolean expanded = (rail->mode == KAJO_WEB_RAIL_PINNED) ||
                        (rail->fsm.state == KAJO_RAIL_FSM_EXPANDED) ||
                        (rail->fsm.state == KAJO_RAIL_FSM_HIDE_WAIT);
    gboolean compact_collapsed = (rail->mode == KAJO_WEB_RAIL_COMPACT) && !expanded;

    GtkAlign halign = (rail->side == KAJO_WEB_SIDE_RIGHT) ? GTK_ALIGN_END : GTK_ALIGN_START;
    gtk_widget_set_halign(rail->trigger, halign);
    gtk_widget_set_halign(rail->ticks_box, halign);
    gtk_widget_set_halign(rail->root, halign);

    gtk_widget_remove_css_class(rail->root, "rail-right");
    gtk_widget_remove_css_class(rail->trigger, "rail-right");
    if (rail->side == KAJO_WEB_SIDE_RIGHT) {
        gtk_widget_add_css_class(rail->root, "rail-right");
        gtk_widget_add_css_class(rail->trigger, "rail-right");
    }

    gtk_widget_remove_css_class(rail->root, "compact-collapsed");
    gtk_widget_remove_css_class(rail->root, "overlay-elevated");
    if (compact_collapsed)
        gtk_widget_add_css_class(rail->root, "compact-collapsed");
    else if (rail->mode != KAJO_WEB_RAIL_PINNED)
        gtk_widget_add_css_class(rail->root, "overlay-elevated");

    gtk_widget_set_visible(rail->trigger, rail->mode == KAJO_WEB_RAIL_AUTOHIDE && !expanded);
    gtk_widget_set_visible(rail->compact_spacer, rail->mode == KAJO_WEB_RAIL_COMPACT);

    if (rail->mode == KAJO_WEB_RAIL_AUTOHIDE) {
        gtk_widget_set_visible(rail->root, expanded);
        gtk_widget_set_size_request(rail->root, rail->width_px, -1);
    } else if (rail->mode == KAJO_WEB_RAIL_COMPACT) {
        gtk_widget_set_visible(rail->root, TRUE);
        gtk_widget_set_size_request(rail->root,
                                    compact_collapsed ? KAJO_WEB_COMPACT_RAIL_PX : rail->width_px, -1);
    } else {
        gtk_widget_set_visible(rail->root, TRUE);
        gtk_widget_set_size_request(rail->root, rail->width_px, -1);
    }

    /* Keep pin_grid column count constant in COMPACT mode (1 col) so hovering NEVER
     * reflows vertical height; use 4-col grid only in AUTOHIDE / PINNED. */
    gtk_flow_box_set_max_children_per_line(
        GTK_FLOW_BOX(rail->pin_grid),
        (rail->mode == KAJO_WEB_RAIL_COMPACT) ? 1 : 4);

    gboolean show_full = !compact_collapsed;
    if (rail->btn_new_lbl)
        gtk_label_set_text(GTK_LABEL(rail->btn_new_lbl), show_full ? "+ NEW TAB" : "+");
    if (rail->btn_undo)
        gtk_widget_set_visible(rail->btn_undo, show_full);

    for (guint i = 0; i < rail->compact_hide->len; i++) {
        GtkWidget *w = g_ptr_array_index(rail->compact_hide, i);
        gtk_widget_set_visible(w, show_full);
    }

    if (rail->row_map) {
        GHashTableIter iter;
        gpointer val = NULL;
        g_hash_table_iter_init(&iter, rail->row_map);
        while (g_hash_table_iter_next(&iter, NULL, &val))
            sync_row_compact_visibility(val, show_full);
    }
}

static gboolean
on_reveal_timeout(gpointer user_data)
{
    KajoWebTabRail *rail = user_data;
    rail->reveal_timer_id = 0;
    if (web_rail_fsm_reveal_timeout(&rail->fsm) == KAJO_RAIL_ACT_EXPAND)
        sync_visual_state(rail);
    return G_SOURCE_REMOVE;
}

static gboolean
on_hide_timeout(gpointer user_data)
{
    KajoWebTabRail *rail = user_data;
    rail->hide_timer_id = 0;
    if (web_rail_fsm_hide_timeout(&rail->fsm) == KAJO_RAIL_ACT_COLLAPSE)
        sync_visual_state(rail);
    return G_SOURCE_REMOVE;
}

static void
handle_fsm_action(KajoWebTabRail *rail, KajoWebRailFsmAction act)
{
    switch (act) {
    case KAJO_RAIL_ACT_START_REVEAL_TIMER:
        if (rail->reveal_timer_id)
            g_source_remove(rail->reveal_timer_id);
        if (rail->reveal_delay_ms <= 0) {
            if (web_rail_fsm_reveal_timeout(&rail->fsm) == KAJO_RAIL_ACT_EXPAND)
                sync_visual_state(rail);
        } else {
            rail->reveal_timer_id = g_timeout_add((guint)rail->reveal_delay_ms, on_reveal_timeout, rail);
        }
        break;
    case KAJO_RAIL_ACT_CANCEL_REVEAL_TIMER:
        if (rail->reveal_timer_id) {
            g_source_remove(rail->reveal_timer_id);
            rail->reveal_timer_id = 0;
        }
        break;
    case KAJO_RAIL_ACT_EXPAND:
        cancel_timers(rail);
        sync_visual_state(rail);
        break;
    case KAJO_RAIL_ACT_START_HIDE_TIMER:
        if (rail->hide_timer_id)
            g_source_remove(rail->hide_timer_id);
        rail->hide_timer_id = g_timeout_add((guint)MAX(50, rail->hide_delay_ms), on_hide_timeout, rail);
        break;
    case KAJO_RAIL_ACT_CANCEL_HIDE_TIMER:
        if (rail->hide_timer_id) {
            g_source_remove(rail->hide_timer_id);
            rail->hide_timer_id = 0;
        }
        break;
    case KAJO_RAIL_ACT_COLLAPSE:
        cancel_timers(rail);
        sync_visual_state(rail);
        break;
    case KAJO_RAIL_ACT_NONE:
    default:
        break;
    }
}

static void
on_trigger_enter(GtkEventControllerMotion *c G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud)
{
    KajoWebTabRail *rail = ud;
    if (rail->mode == KAJO_WEB_RAIL_PINNED)
        return;
    handle_fsm_action(rail, web_rail_fsm_trigger_enter(&rail->fsm));
}

static void
on_trigger_leave(GtkEventControllerMotion *c G_GNUC_UNUSED, gpointer ud)
{
    KajoWebTabRail *rail = ud;
    if (rail->mode == KAJO_WEB_RAIL_PINNED)
        return;
    handle_fsm_action(rail, web_rail_fsm_trigger_leave(&rail->fsm));
}

static void
on_trigger_pressed(GtkGestureClick *g G_GNUC_UNUSED, gint n G_GNUC_UNUSED,
                   double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud)
{
    KajoWebTabRail *rail = ud;
    if (rail->mode == KAJO_WEB_RAIL_PINNED)
        return;
    handle_fsm_action(rail, web_rail_fsm_trigger_click(&rail->fsm));
}

static void
on_rail_enter(GtkEventControllerMotion *c G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud)
{
    KajoWebTabRail *rail = ud;
    if (rail->mode == KAJO_WEB_RAIL_PINNED)
        return;
    handle_fsm_action(rail, web_rail_fsm_rail_enter(&rail->fsm));
}

static void
on_rail_leave(GtkEventControllerMotion *c G_GNUC_UNUSED, gpointer ud)
{
    KajoWebTabRail *rail = ud;
    if (rail->mode == KAJO_WEB_RAIL_PINNED)
        return;
    handle_fsm_action(rail, web_rail_fsm_rail_leave(&rail->fsm));
}

static void
on_new_tab_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebTabRail *rail = ud;
    if (rail->cbs.on_new_tab)
        rail->cbs.on_new_tab(rail->ud);
}

static void
on_undo_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    KajoWebTabRail *rail = ud;
    if (rail->cbs.on_undo_close)
        rail->cbs.on_undo_close(rail->ud);
}

KajoWebTabRail *
web_tab_rail_new(const KajoWebTabRailCallbacks *cbs, gpointer user_data)
{
    KajoWebTabRail *rail = g_new0(KajoWebTabRail, 1);
    if (cbs)
        rail->cbs = *cbs;
    rail->ud = user_data;
    web_rail_fsm_init(&rail->fsm);
    rail->mode = KAJO_WEB_RAIL_AUTOHIDE;
    rail->side = KAJO_WEB_SIDE_LEFT;
    rail->width_px = 240;
    rail->reveal_delay_ms = 35;
    rail->hide_delay_ms = 220;
    rail->show_domains = TRUE;
    rail->compact_hide = g_ptr_array_new();
    rail->snap = g_ptr_array_new_with_free_func(rail_snap_entry_free);
    rail->row_map = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, tab_row_widgets_free);

    /* Edge Trigger Strip (rgba(0,0,0,0.01) — never opacity: 0 on Wayland) */
    rail->trigger = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(rail->trigger, 6, -1);
    gtk_widget_set_vexpand(rail->trigger, TRUE);
    gtk_widget_set_halign(rail->trigger, GTK_ALIGN_START);
    gtk_widget_add_css_class(rail->trigger, "web-rail-trigger");
    g_object_ref_sink(rail->trigger);

    rail->ticks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_top(rail->ticks_box, 12);
    gtk_widget_set_halign(rail->ticks_box, GTK_ALIGN_START);
    gtk_widget_set_can_target(rail->ticks_box, FALSE);
    gtk_box_append(GTK_BOX(rail->trigger), rail->ticks_box);

    GtkEventController *tm = gtk_event_controller_motion_new();
    g_signal_connect(tm, "enter", G_CALLBACK(on_trigger_enter), rail);
    g_signal_connect(tm, "leave", G_CALLBACK(on_trigger_leave), rail);
    gtk_widget_add_controller(rail->trigger, tm);

    GtkGesture *tc = gtk_gesture_click_new();
    g_signal_connect(tc, "pressed", G_CALLBACK(on_trigger_pressed), rail);
    gtk_widget_add_controller(rail->trigger, GTK_EVENT_CONTROLLER(tc));

    /* Compact mode docked spacer (48px) */
    rail->compact_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(rail->compact_spacer, KAJO_WEB_COMPACT_RAIL_PX, -1);
    gtk_widget_set_visible(rail->compact_spacer, FALSE);
    g_object_ref_sink(rail->compact_spacer);

    /* Main Vertical Rail — tabs start immediately at y = 0 (no header bar noise) */
    rail->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(rail->root, FALSE);
    gtk_widget_set_vexpand(rail->root, TRUE);
    gtk_widget_set_overflow(rail->root, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(rail->root, "web-tab-rail");
    g_object_ref_sink(rail->root);

    GtkEventController *rm = gtk_event_controller_motion_new();
    g_signal_connect(rm, "enter", G_CALLBACK(on_rail_enter), rail);
    g_signal_connect(rm, "leave", G_CALLBACK(on_rail_leave), rail);
    gtk_widget_add_controller(rail->root, rm);

    rail->pin_grid = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(rail->pin_grid), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(rail->pin_grid), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(rail->pin_grid), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(rail->pin_grid), 4);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(rail->pin_grid), 4);
    gtk_widget_add_css_class(rail->pin_grid, "web-pin-grid");
    gtk_box_append(GTK_BOX(rail->root), rail->pin_grid);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    rail->tab_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(rail->tab_list, GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), rail->tab_list);
    gtk_box_append(GTK_BOX(rail->root), scroll);

    /* Single-row fixed-height footer */
    rail->footer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(rail->footer_box, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(rail->footer_box, "web-rail-footer");

    rail->btn_new = gtk_button_new();
    rail->btn_new_lbl = gtk_label_new("+ NEW TAB");
    gtk_widget_set_valign(rail->btn_new_lbl, GTK_ALIGN_CENTER);
    gtk_button_set_child(GTK_BUTTON(rail->btn_new), rail->btn_new_lbl);
    gtk_widget_set_hexpand(rail->btn_new, TRUE);
    gtk_widget_set_valign(rail->btn_new, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(rail->btn_new, "New Tab (Ctrl+T)");
    gtk_widget_add_css_class(rail->btn_new, "web-rail-foot-btn");
    gtk_widget_add_css_class(rail->btn_new, "primary");
    g_signal_connect(rail->btn_new, "clicked", G_CALLBACK(on_new_tab_clicked), rail);

    rail->btn_undo = gtk_button_new_with_label("↺");
    gtk_widget_set_valign(rail->btn_undo, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(rail->btn_undo, "Reopen Closed Tab (Ctrl+Shift+T)");
    gtk_widget_set_sensitive(rail->btn_undo, FALSE);
    gtk_widget_add_css_class(rail->btn_undo, "web-rail-foot-btn");
    g_signal_connect(rail->btn_undo, "clicked", G_CALLBACK(on_undo_clicked), rail);

    gtk_box_append(GTK_BOX(rail->footer_box), rail->btn_new);
    gtk_box_append(GTK_BOX(rail->footer_box), rail->btn_undo);
    gtk_box_append(GTK_BOX(rail->root), rail->footer_box);

    sync_visual_state(rail);
    return rail;
}

void
web_tab_rail_free(KajoWebTabRail *rail)
{
    if (!rail)
        return;
    cancel_timers(rail);
    g_clear_pointer(&rail->active_ctx_pop, gtk_widget_unparent);
    g_clear_pointer(&rail->row_map, g_hash_table_unref);
    g_clear_pointer(&rail->snap, g_ptr_array_unref);
    g_clear_pointer(&rail->compact_hide, g_ptr_array_unref);
    g_clear_object(&rail->trigger);
    g_clear_object(&rail->compact_spacer);
    g_clear_object(&rail->root);
    g_free(rail);
}

GtkWidget *
web_tab_rail_get_trigger(KajoWebTabRail *rail)
{
    return rail ? rail->trigger : NULL;
}

GtkWidget *
web_tab_rail_get_widget(KajoWebTabRail *rail)
{
    return rail ? rail->root : NULL;
}

GtkWidget *
web_tab_rail_get_compact_spacer(KajoWebTabRail *rail)
{
    return rail ? rail->compact_spacer : NULL;
}

void
web_tab_rail_apply_config(KajoWebTabRail *rail, const KajoWebCfg *cfg)
{
    if (!rail || !cfg)
        return;
    rail->mode = cfg->rail_mode;
    rail->side = cfg->rail_side;
    rail->width_px = CLAMP(cfg->rail_width_px, 180, 360);
    rail->reveal_delay_ms = CLAMP(cfg->rail_reveal_delay_ms, 0, 300);
    rail->hide_delay_ms = CLAMP(cfg->rail_hide_delay_ms, 50, 1000);
    rail->show_domains = cfg->show_tab_domains;
    if (rail->mode == KAJO_WEB_RAIL_PINNED) {
        cancel_timers(rail);
        rail->fsm.state = KAJO_RAIL_FSM_EXPANDED;
    } else if (rail->fsm.state != KAJO_RAIL_FSM_EXPANDED) {
        rail->fsm.state = KAJO_RAIL_FSM_COLLAPSED;
    }
    sync_visual_state(rail);
}

typedef struct {
    KajoWebTabRail *rail;
    guint64         tab_id;
} TabRowAction;

static void
rail_free_closure_data(gpointer data, GClosure *closure G_GNUC_UNUSED)
{
    g_free(data);
}

static void
on_tab_select_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabRowAction *a = ud;
    if (a->rail->cbs.on_select_tab)
        a->rail->cbs.on_select_tab(a->tab_id, a->rail->ud);
}

static void
on_tab_close_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabRowAction *a = ud;
    if (a->rail->cbs.on_close_tab)
        a->rail->cbs.on_close_tab(a->tab_id, a->rail->ud);
}

static void
on_tab_mute_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabRowAction *a = ud;
    if (a->rail->cbs.on_toggle_mute)
        a->rail->cbs.on_toggle_mute(a->tab_id, a->rail->ud);
}

static void
on_tab_middle_click(GtkGestureClick *gest, gint n G_GNUC_UNUSED,
                    double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud)
{
    TabRowAction *a = ud;
    gtk_gesture_set_state(GTK_GESTURE(gest), GTK_EVENT_SEQUENCE_CLAIMED);
    if (a->rail->cbs.on_close_tab)
        a->rail->cbs.on_close_tab(a->tab_id, a->rail->ud);
}

/* ─── Right-Click Context Menu Parented to rail->root (Survives Tab Updates) ─── */

typedef struct {
    KajoWebTabRail *rail;
    GtkWidget      *popover;
    guint64         tab_id;
} TabCtxAction;

static void
free_tab_ctx_action(gpointer data, GClosure *c G_GNUC_UNUSED)
{
    g_free(data);
}

static void
on_ctx_pin(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabCtxAction *a = ud;
    if (a->popover)
        gtk_popover_popdown(GTK_POPOVER(a->popover));
    if (a->rail->cbs.on_toggle_pin)
        a->rail->cbs.on_toggle_pin(a->tab_id, a->rail->ud);
}

static void
on_ctx_mute(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabCtxAction *a = ud;
    if (a->popover)
        gtk_popover_popdown(GTK_POPOVER(a->popover));
    if (a->rail->cbs.on_toggle_mute)
        a->rail->cbs.on_toggle_mute(a->tab_id, a->rail->ud);
}

static void
on_ctx_close(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabCtxAction *a = ud;
    if (a->popover)
        gtk_popover_popdown(GTK_POPOVER(a->popover));
    if (a->rail->cbs.on_close_tab)
        a->rail->cbs.on_close_tab(a->tab_id, a->rail->ud);
}

static void
on_ctx_reload(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabCtxAction *a = ud;
    if (a->popover)
        gtk_popover_popdown(GTK_POPOVER(a->popover));
    if (a->rail->cbs.on_reload_tab)
        a->rail->cbs.on_reload_tab(a->tab_id, a->rail->ud);
}

static void
on_ctx_duplicate(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabCtxAction *a = ud;
    if (a->popover)
        gtk_popover_popdown(GTK_POPOVER(a->popover));
    if (a->rail->cbs.on_duplicate_tab)
        a->rail->cbs.on_duplicate_tab(a->tab_id, a->rail->ud);
}

static void
on_ctx_copy_url(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabCtxAction *a = ud;
    if (a->popover)
        gtk_popover_popdown(GTK_POPOVER(a->popover));
    if (a->rail->cbs.on_copy_tab_url)
        a->rail->cbs.on_copy_tab_url(a->tab_id, a->rail->ud);
}

static void
on_ctx_suspend(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabCtxAction *a = ud;
    if (a->popover)
        gtk_popover_popdown(GTK_POPOVER(a->popover));
    if (a->rail->cbs.on_suspend_tab)
        a->rail->cbs.on_suspend_tab(a->tab_id, a->rail->ud);
}

static void
on_ctx_close_others(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabCtxAction *a = ud;
    if (a->popover)
        gtk_popover_popdown(GTK_POPOVER(a->popover));
    if (a->rail->cbs.on_close_other_tabs)
        a->rail->cbs.on_close_other_tabs(a->tab_id, a->rail->ud);
}

static void
on_ctx_close_below(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
    TabCtxAction *a = ud;
    if (a->popover)
        gtk_popover_popdown(GTK_POPOVER(a->popover));
    if (a->rail->cbs.on_close_tabs_below)
        a->rail->cbs.on_close_tabs_below(a->tab_id, a->rail->ud);
}

static void
on_ctx_closed(GtkPopover *pop, gpointer ud)
{
    KajoWebTabRail *rail = ud;
    if (rail->active_ctx_pop == GTK_WIDGET(pop))
        rail->active_ctx_pop = NULL;
    handle_fsm_action(rail, web_rail_fsm_release(&rail->fsm));
    if (gtk_widget_get_parent(GTK_WIDGET(pop)))
        gtk_widget_unparent(GTK_WIDGET(pop));
}

static GtkWidget *
make_ctx_btn(const gchar *lbl, gboolean danger, KajoWebTabRail *rail,
             GtkWidget *pop, guint64 tab_id, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_label(lbl);
    gtk_widget_set_halign(b, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(b, "web-ctx-row");
    if (danger)
        gtk_widget_add_css_class(b, "danger");
    GtkWidget *c = gtk_button_get_child(GTK_BUTTON(b));
    if (GTK_IS_LABEL(c))
        gtk_widget_set_halign(c, GTK_ALIGN_START);
    TabCtxAction *act = g_new0(TabCtxAction, 1);
    act->rail = rail;
    act->popover = pop;
    act->tab_id = tab_id;
    g_signal_connect_data(b, "clicked", cb, act, free_tab_ctx_action, 0);
    return b;
}

static void
on_tab_right_click(GtkGestureClick *gest, gint n G_GNUC_UNUSED,
                   double x, double y, gpointer ud)
{
    TabRowAction *a = ud;
    KajoWebTabRail *rail = a->rail;
    GtkWidget *target = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gest));
    if (!target || !rail || !rail->root)
        return;

    const TabRowWidgets *rw = g_hash_table_lookup(rail->row_map, &a->tab_id);
    if (!rw)
        return;

    if (rail->active_ctx_pop)
        gtk_popover_popdown(GTK_POPOVER(rail->active_ctx_pop));

    web_rail_fsm_hold(&rail->fsm);

    GtkWidget *pop = gtk_popover_new();
    gtk_widget_set_parent(pop, rail->root);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_widget_add_css_class(pop, "shell-popover");

    graphene_point_t pt = GRAPHENE_POINT_INIT(0.f, 0.f);
    if (!gtk_widget_compute_point(target, rail->root,
                                  &GRAPHENE_POINT_INIT((float)x, (float)y), &pt)) {
        pt.x = 12.f;
        pt.y = 12.f;
    }
    GdkRectangle rect = { (int)pt.x, (int)pt.y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
    g_signal_connect(pop, "closed", G_CALLBACK(on_ctx_closed), rail);
    rail->active_ctx_pop = pop;

    gint snap_idx = -1;
    for (guint i = 0; i < rail->snap->len; i++) {
        const RailSnapEntry *se = g_ptr_array_index(rail->snap, i);
        if (se->id == a->tab_id) {
            snap_idx = (gint)i;
            break;
        }
    }

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_size_request(box, 196, -1);
    gtk_box_append(GTK_BOX(box), make_ctx_btn("↻ RELOAD TAB",
                                              FALSE, rail, pop, a->tab_id, G_CALLBACK(on_ctx_reload)));
    gtk_box_append(GTK_BOX(box), make_ctx_btn("⧉ DUPLICATE TAB",
                                              FALSE, rail, pop, a->tab_id, G_CALLBACK(on_ctx_duplicate)));
    gtk_box_append(GTK_BOX(box), make_ctx_btn("🔗 COPY URL",
                                              FALSE, rail, pop, a->tab_id, G_CALLBACK(on_ctx_copy_url)));
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), make_ctx_btn(rw->is_pinned ? "⊘ UNPIN TAB" : "📌 PIN TAB",
                                              FALSE, rail, pop, a->tab_id, G_CALLBACK(on_ctx_pin)));
    gtk_box_append(GTK_BOX(box), make_ctx_btn(rw->muted ? "🔊 UNMUTE TAB" : "🔇 MUTE TAB",
                                              FALSE, rail, pop, a->tab_id, G_CALLBACK(on_ctx_mute)));
    GtkWidget *b_susp = make_ctx_btn("💤 SUSPEND TAB",
                                     FALSE, rail, pop, a->tab_id, G_CALLBACK(on_ctx_suspend));
    gtk_widget_set_sensitive(b_susp, !rw->active && !rw->suspended);
    gtk_box_append(GTK_BOX(box), b_susp);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *b_oth = make_ctx_btn("✕ CLOSE OTHER TABS",
                                    FALSE, rail, pop, a->tab_id, G_CALLBACK(on_ctx_close_others));
    gtk_widget_set_sensitive(b_oth, rail->snap->len > 1);
    gtk_box_append(GTK_BOX(box), b_oth);
    GtkWidget *b_bel = make_ctx_btn("✕ CLOSE TABS BELOW",
                                    FALSE, rail, pop, a->tab_id, G_CALLBACK(on_ctx_close_below));
    gtk_widget_set_sensitive(b_bel, !rw->is_pinned && snap_idx >= 0 && (guint)(snap_idx + 1) < rail->snap->len);
    gtk_box_append(GTK_BOX(box), b_bel);
    gtk_box_append(GTK_BOX(box), make_ctx_btn("✕ CLOSE TAB",
                                              TRUE, rail, pop, a->tab_id, G_CALLBACK(on_ctx_close)));
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_popover_popup(GTK_POPOVER(pop));
}

static GdkContentProvider *
on_tab_drag_prepare(GtkDragSource *ds G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud)
{
    TabRowAction *a = ud;
    GValue val = G_VALUE_INIT;
    g_value_init(&val, G_TYPE_UINT64);
    g_value_set_uint64(&val, a->tab_id);
    GdkContentProvider *cp = gdk_content_provider_new_for_value(&val);
    g_value_unset(&val);
    return cp;
}

static GdkDragAction
on_tab_drop_enter(GtkDropTarget *dt, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED)
{
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(dt));
    if (w)
        gtk_widget_add_css_class(w, "drag-over");
    return GDK_ACTION_MOVE;
}

static void
on_tab_drop_leave(GtkDropTarget *dt, gpointer ud G_GNUC_UNUSED)
{
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(dt));
    if (w)
        gtk_widget_remove_css_class(w, "drag-over");
}

static gboolean
on_tab_drop_perform(GtkDropTarget *dt, const GValue *val, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, gpointer ud)
{
    TabRowAction *a = ud;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(dt));
    if (w)
        gtk_widget_remove_css_class(w, "drag-over");
    if (!G_VALUE_HOLDS_UINT64(val))
        return FALSE;

    guint64 src_id = g_value_get_uint64(val);
    if (src_id == 0 || src_id == a->tab_id || !a->rail || !a->rail->cbs.on_reorder_tab)
        return FALSE;

    for (guint i = 0; i < a->rail->snap->len; i++) {
        const RailSnapEntry *se = g_ptr_array_index(a->rail->snap, i);
        if (se->id == a->tab_id) {
            a->rail->cbs.on_reorder_tab(src_id, i, a->rail->ud);
            return TRUE;
        }
    }
    return FALSE;
}

static void
attach_tab_pointer_gestures(GtkWidget *w, KajoWebTabRail *rail, guint64 tab_id)
{
    GtkGesture *rc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
    TabRowAction *ra = g_new0(TabRowAction, 1);
    ra->rail = rail;
    ra->tab_id = tab_id;
    g_signal_connect_data(rc, "pressed", G_CALLBACK(on_tab_right_click), ra, rail_free_closure_data, 0);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(rc));

    GtkGesture *mc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mc), GDK_BUTTON_MIDDLE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(mc), GTK_PHASE_CAPTURE);
    TabRowAction *ma = g_new0(TabRowAction, 1);
    ma->rail = rail;
    ma->tab_id = tab_id;
    g_signal_connect_data(mc, "pressed", G_CALLBACK(on_tab_middle_click), ma, rail_free_closure_data, 0);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(mc));

    GtkDragSource *ds = gtk_drag_source_new();
    gtk_drag_source_set_actions(ds, GDK_ACTION_MOVE);
    TabRowAction *da = g_new0(TabRowAction, 1);
    da->rail = rail;
    da->tab_id = tab_id;
    g_signal_connect_data(ds, "prepare", G_CALLBACK(on_tab_drag_prepare), da, rail_free_closure_data, 0);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(ds));

    GtkDropTarget *dt = gtk_drop_target_new(G_TYPE_UINT64, GDK_ACTION_MOVE);
    TabRowAction *pa = g_new0(TabRowAction, 1);
    pa->rail = rail;
    pa->tab_id = tab_id;
    g_signal_connect(dt, "enter", G_CALLBACK(on_tab_drop_enter), NULL);
    g_signal_connect(dt, "leave", G_CALLBACK(on_tab_drop_leave), NULL);
    g_signal_connect_data(dt, "drop", G_CALLBACK(on_tab_drop_perform), pa, rail_free_closure_data, 0);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(dt));
}

static void
format_tab_monogram(const KajoWebTabMeta *t, const gchar *host, gchar out[8])
{
    if (t->suspended) {
        g_strlcpy(out, "Z", 8);
        return;
    }
    if (g_str_has_prefix(t->uri, "kajo://")) {
        g_strlcpy(out, "❖", 8);
        return;
    }
    const gchar *src = (host && *host) ? host : (t->title ? t->title : "");
    if (g_str_has_prefix(src, "www."))
        src += 4;
    if (src[0] && g_ascii_isalnum(src[0])) {
        out[0] = (char)g_ascii_toupper(src[0]);
        out[1] = '\0';
        return;
    }
    g_strlcpy(out, "•", 8);
}

static GtkWidget *
create_icon_stack(GtkWidget **out_badge, GtkWidget **out_img, GtkWidget **out_loading)
{
    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_halign(stack, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(stack, GTK_ALIGN_CENTER);

    GtkWidget *badge = gtk_label_new("•");
    gtk_widget_set_halign(badge, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(badge, "web-tab-badge");

    GtkWidget *img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), 18);
    gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(img, "web-tab-favicon");

    GtkWidget *loading = gtk_label_new("◌");
    gtk_widget_set_size_request(loading, 18, 18);
    gtk_widget_set_halign(loading, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(loading, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(loading, "web-tab-loading");

    gtk_stack_add_named(GTK_STACK(stack), badge, "badge");
    gtk_stack_add_named(GTK_STACK(stack), img, "favicon");
    gtk_stack_add_named(GTK_STACK(stack), loading, "loading");
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "badge");

    *out_badge = badge;
    *out_img = img;
    *out_loading = loading;
    return stack;
}

static GtkWidget *
create_icon_overlay(GtkWidget **out_stack, GtkWidget **out_badge, GtkWidget **out_img,
                    GtkWidget **out_loading, GtkWidget **out_audio_dot)
{
    GtkWidget *ov = gtk_overlay_new();
    gtk_widget_set_hexpand(ov, TRUE);

    GtkWidget *stack = create_icon_stack(out_badge, out_img, out_loading);
    gtk_overlay_set_child(GTK_OVERLAY(ov), stack);

    GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(dot, 6, 6);
    gtk_widget_set_halign(dot, GTK_ALIGN_END);
    gtk_widget_set_valign(dot, GTK_ALIGN_START);
    gtk_widget_set_margin_top(dot, 4);
    gtk_widget_set_margin_end(dot, 4);
    gtk_widget_set_can_target(dot, FALSE);
    gtk_widget_add_css_class(dot, "web-tab-audio-dot");
    gtk_widget_set_visible(dot, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), dot);

    *out_stack = stack;
    *out_audio_dot = dot;
    return ov;
}

static TabRowWidgets *
create_pin_tile_widgets(KajoWebTabRail *rail, guint64 tab_id)
{
    TabRowWidgets *rw = g_new0(TabRowWidgets, 1);
    rw->tab_id = tab_id;
    rw->is_pinned = TRUE;

    rw->tick = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(rw->tick, 2, 14);
    gtk_widget_add_css_class(rw->tick, "web-rail-tick");
    g_object_ref_sink(rw->tick);

    GtkWidget *pt = gtk_button_new();
    gtk_widget_set_size_request(pt, 36, 36);
    gtk_widget_add_css_class(pt, "web-pin-tile");
    GtkWidget *ov = create_icon_overlay(&rw->ico_stack, &rw->badge_lbl, &rw->fav_img,
                                        &rw->loading_lbl, &rw->audio_dot);
    gtk_button_set_child(GTK_BUTTON(pt), ov);

    TabRowAction *a = g_new0(TabRowAction, 1);
    a->rail = rail;
    a->tab_id = tab_id;
    g_signal_connect_data(pt, "clicked", G_CALLBACK(on_tab_select_clicked), a, rail_free_closure_data, 0);
    attach_tab_pointer_gestures(pt, rail, tab_id);

    g_object_ref_sink(pt);
    rw->widget = pt;
    return rw;
}

static TabRowWidgets *
create_tab_row_widgets(KajoWebTabRail *rail, guint64 tab_id)
{
    TabRowWidgets *rw = g_new0(TabRowWidgets, 1);
    rw->tab_id = tab_id;
    rw->is_pinned = FALSE;

    rw->tick = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(rw->tick, 2, 14);
    gtk_widget_add_css_class(rw->tick, "web-rail-tick");
    g_object_ref_sink(rw->tick);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(row, "web-tab-row");

    rw->sel_btn = gtk_button_new();
    gtk_widget_set_hexpand(rw->sel_btn, TRUE);
    gtk_widget_set_valign(rw->sel_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(rw->sel_btn, "web-tab-select-btn");

    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(inner, TRUE);
    gtk_widget_set_halign(inner, GTK_ALIGN_FILL);
    gtk_widget_set_valign(inner, GTK_ALIGN_CENTER);

    /* Fixed 44px left slot (3px border + 44px slot + 1px border = 48px compact) */
    GtkWidget *ico_slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(ico_slot, 44, 42);
    gtk_widget_set_hexpand(ico_slot, FALSE);
    gtk_widget_set_hexpand_set(ico_slot, TRUE);
    gtk_widget_set_halign(ico_slot, GTK_ALIGN_START);
    gtk_widget_set_valign(ico_slot, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(ico_slot, "web-tab-ico-slot");

    GtkWidget *ico_ov = create_icon_overlay(&rw->ico_stack, &rw->badge_lbl, &rw->fav_img,
                                            &rw->loading_lbl, &rw->audio_dot);
    gtk_box_append(GTK_BOX(ico_slot), ico_ov);
    gtk_box_append(GTK_BOX(inner), ico_slot);

    rw->text_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(rw->text_vbox, TRUE);
    gtk_widget_set_halign(rw->text_vbox, GTK_ALIGN_FILL);
    gtk_widget_set_valign(rw->text_vbox, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(rw->text_vbox, "web-tab-text-col");

    rw->title_lbl = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(rw->title_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(rw->title_lbl), 1);
    gtk_label_set_xalign(GTK_LABEL(rw->title_lbl), 0.0f);
    gtk_widget_set_halign(rw->title_lbl, GTK_ALIGN_FILL);
    gtk_widget_set_valign(rw->title_lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(rw->title_lbl, "web-tab-title");

    rw->domain_lbl = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(rw->domain_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(rw->domain_lbl), 1);
    gtk_label_set_xalign(GTK_LABEL(rw->domain_lbl), 0.0f);
    gtk_widget_set_halign(rw->domain_lbl, GTK_ALIGN_FILL);
    gtk_widget_set_valign(rw->domain_lbl, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(rw->domain_lbl, "web-tab-domain");

    gtk_box_append(GTK_BOX(rw->text_vbox), rw->title_lbl);
    gtk_box_append(GTK_BOX(rw->text_vbox), rw->domain_lbl);
    gtk_box_append(GTK_BOX(inner), rw->text_vbox);

    gtk_button_set_child(GTK_BUTTON(rw->sel_btn), inner);
    TabRowAction *sa = g_new0(TabRowAction, 1);
    sa->rail = rail;
    sa->tab_id = tab_id;
    g_signal_connect_data(rw->sel_btn, "clicked", G_CALLBACK(on_tab_select_clicked), sa, rail_free_closure_data, 0);
    gtk_box_append(GTK_BOX(row), rw->sel_btn);

    rw->mute_btn = gtk_button_new_with_label("🔊");
    gtk_widget_set_valign(rw->mute_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(rw->mute_btn, "Toggle Mute (Ctrl+M)");
    gtk_widget_add_css_class(rw->mute_btn, "web-tab-mini-btn");
    TabRowAction *ma = g_new0(TabRowAction, 1);
    ma->rail = rail;
    ma->tab_id = tab_id;
    g_signal_connect_data(rw->mute_btn, "clicked", G_CALLBACK(on_tab_mute_clicked), ma, rail_free_closure_data, 0);
    gtk_box_append(GTK_BOX(row), rw->mute_btn);

    rw->close_btn = gtk_button_new_with_label("✕");
    gtk_widget_set_valign(rw->close_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(rw->close_btn, "Close Tab (Ctrl+W / Middle-click)");
    gtk_widget_add_css_class(rw->close_btn, "web-tab-close-btn");
    TabRowAction *ca = g_new0(TabRowAction, 1);
    ca->rail = rail;
    ca->tab_id = tab_id;
    g_signal_connect_data(rw->close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), ca, rail_free_closure_data, 0);
    gtk_box_append(GTK_BOX(row), rw->close_btn);

    attach_tab_pointer_gestures(row, rail, tab_id);
    g_object_ref_sink(row);
    rw->widget = row;
    return rw;
}

static void
rail_set_css_class(GtkWidget *w, const gchar *cls, gboolean enabled)
{
    if (!w)
        return;
    gboolean has = gtk_widget_has_css_class(w, cls);
    if (enabled && !has)
        gtk_widget_add_css_class(w, cls);
    else if (!enabled && has)
        gtk_widget_remove_css_class(w, cls);
}

static void
update_tab_row_widgets(KajoWebTabRail *rail, TabRowWidgets *rw,
                       const KajoWebTabMeta *t, gboolean active, gboolean show_full)
{
    g_autofree gchar *host = web_model_extract_host(t->uri);
    gchar glyph[8];
    format_tab_monogram(t, host, glyph);

    if (rw->tick && rw->active != active) {
        gtk_widget_set_size_request(rw->tick, 2, active ? 22 : 14);
        rail_set_css_class(rw->tick, "web-rail-tick-active", active);
        rail_set_css_class(rw->tick, "web-rail-tick", !active);
    }

    rail_set_css_class(rw->widget, "active", active);
    rail_set_css_class(rw->widget, "suspended", t->suspended);

    const gchar *tip = (t->title && *t->title) ? t->title : t->uri;
    GtkWidget *tip_target = rw->is_pinned ? rw->widget : rw->sel_btn;
    if (tip_target && g_strcmp0(gtk_widget_get_tooltip_text(tip_target), tip) != 0)
        gtk_widget_set_tooltip_text(tip_target, tip);

    /* Update icon slot: static loading glyph -> real favicon -> monogram badge */
    GdkTexture *fav = (!t->suspended && rail->cbs.get_favicon)
        ? rail->cbs.get_favicon(t->id, rail->ud) : NULL;
    const gchar *cur_child = gtk_stack_get_visible_child_name(GTK_STACK(rw->ico_stack));
    if (t->loading && !t->suspended) {
        if (g_strcmp0(cur_child, "loading") != 0)
            gtk_stack_set_visible_child_name(GTK_STACK(rw->ico_stack), "loading");
    } else if (fav) {
        if (fav != rw->last_favicon) {
            gtk_image_set_from_paintable(GTK_IMAGE(rw->fav_img), GDK_PAINTABLE(fav));
            g_set_object(&rw->last_favicon, fav);
        }
        if (g_strcmp0(cur_child, "favicon") != 0)
            gtk_stack_set_visible_child_name(GTK_STACK(rw->ico_stack), "favicon");
    } else {
        g_clear_object(&rw->last_favicon);
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(rw->badge_lbl)), glyph) != 0)
            gtk_label_set_text(GTK_LABEL(rw->badge_lbl), glyph);
        rail_set_css_class(rw->badge_lbl, "kajo", g_str_has_prefix(t->uri, "kajo://"));
        rail_set_css_class(rw->badge_lbl, "suspended", !g_str_has_prefix(t->uri, "kajo://") && t->suspended);
        if (g_strcmp0(cur_child, "badge") != 0)
            gtk_stack_set_visible_child_name(GTK_STACK(rw->ico_stack), "badge");
    }

    rw->active = active;
    rw->suspended = t->suspended;
    rw->muted = t->muted;
    rw->wants_mute_btn = (t->audible || t->muted);
    rw->wants_audio_dot = (t->audible || t->muted);
    rail_set_css_class(rw->audio_dot, "muted", t->muted);

    if (rw->is_pinned) {
        sync_row_compact_visibility(rw, show_full);
        return;
    }

    const gchar *ttl = (t->title && *t->title) ? t->title : t->uri;
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(rw->title_lbl)), ttl) != 0)
        gtk_label_set_text(GTK_LABEL(rw->title_lbl), ttl);

    if (gtk_widget_get_visible(rw->domain_lbl) != rail->show_domains)
        gtk_widget_set_visible(rw->domain_lbl, rail->show_domains);
    if (rail->show_domains) {
        const gchar *dom = host ? host : "";
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(rw->domain_lbl)), dom) != 0)
            gtk_label_set_text(GTK_LABEL(rw->domain_lbl), dom);
    }

    const gchar *mlbl = t->muted ? "🔇" : "🔊";
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(rw->mute_btn)), mlbl) != 0)
        gtk_button_set_label(GTK_BUTTON(rw->mute_btn), mlbl);
    rail_set_css_class(rw->mute_btn, "muted", t->muted);

    sync_row_compact_visibility(rw, show_full);
}

static void
clear_box_children(GtkWidget *box)
{
    GtkWidget *child = gtk_widget_get_first_child(box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

static void
clear_flowbox_children(GtkWidget *fb)
{
    GtkWidget *child = gtk_widget_get_first_child(fb);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_flow_box_remove(GTK_FLOW_BOX(fb), child);
        child = next;
    }
}

static gboolean
structure_matches_snapshot(const KajoWebTabRail *rail, const KajoWebModel *model)
{
    if (rail->snap->len != model->tabs->len ||
        g_hash_table_size(rail->row_map) != model->tabs->len)
        return FALSE;

    for (guint i = 0; i < model->tabs->len; i++) {
        const KajoWebTabMeta *t = g_ptr_array_index(model->tabs, i);
        const RailSnapEntry *e = g_ptr_array_index(rail->snap, i);
        gboolean col = (t->group && *t->group) ? web_model_is_group_collapsed(model, t->group) : FALSE;
        gboolean vis = !col || (t->id == model->active_id);
        if (t->id != e->id || t->pinned != e->pinned ||
            g_strcmp0(t->group, e->group) != 0 || col != e->collapsed || vis != e->visible_in_list)
            return FALSE;
    }
    return TRUE;
}

static gboolean
current_show_full(const KajoWebTabRail *rail)
{
    gboolean expanded = (rail->mode == KAJO_WEB_RAIL_PINNED) ||
                        (rail->fsm.state == KAJO_RAIL_FSM_EXPANDED) ||
                        (rail->fsm.state == KAJO_RAIL_FSM_HIDE_WAIT);
    return !((rail->mode == KAJO_WEB_RAIL_COMPACT) && !expanded);
}

void
web_tab_rail_update_tab(KajoWebTabRail *rail, const KajoWebModel *model, guint64 tab_id)
{
    if (!rail || !model || tab_id == 0)
        return;

    const KajoWebTabMeta *t = web_model_find_tab((KajoWebModel *)model, tab_id);
    TabRowWidgets *rw = g_hash_table_lookup(rail->row_map, &tab_id);
    if (!t || !rw || rw->is_pinned != t->pinned) {
        web_tab_rail_rebuild(rail, model);
        return;
    }

    update_tab_row_widgets(rail, rw, t, t->id == model->active_id, current_show_full(rail));
    if (rail->btn_undo)
        gtk_widget_set_sensitive(rail->btn_undo, model->closed_stack->len > 0);
}

typedef struct {
    KajoWebTabRail *rail;
    gchar          *group;
} RailGroupClickCtx;

static void
rail_group_click_ctx_free(gpointer data, GClosure *c G_GNUC_UNUSED)
{
    RailGroupClickCtx *ctx = data;
    if (!ctx)
        return;
    g_free(ctx->group);
    g_free(ctx);
}

static void
on_group_hdr_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    RailGroupClickCtx *ctx = user_data;
    if (ctx && ctx->rail && ctx->rail->cbs.on_toggle_group && ctx->group)
        ctx->rail->cbs.on_toggle_group(ctx->group, ctx->rail->ud);
}

void
web_tab_rail_rebuild(KajoWebTabRail *rail, const KajoWebModel *model)
{
    if (!rail || !model)
        return;

    gboolean show_full = current_show_full(rail);

    /* Fast path: identical tab order/pinning/grouping -> mutate widgets strictly in place */
    if (structure_matches_snapshot(rail, model)) {
        for (guint i = 0; i < model->tabs->len; i++) {
            const KajoWebTabMeta *t = g_ptr_array_index(model->tabs, i);
            TabRowWidgets *rw = g_hash_table_lookup(rail->row_map, &t->id);
            if (rw)
                update_tab_row_widgets(rail, rw, t, t->id == model->active_id, show_full);
        }
        if (rail->btn_undo)
            gtk_widget_set_sensitive(rail->btn_undo, model->closed_stack->len > 0);
        return;
    }

    /* Structural path: keep surviving TabRowWidgets alive without tearing down rows/spinners/popovers */
    GPtrArray *doomed_ids = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter riter;
    gpointer rk = NULL, rv = NULL;
    g_hash_table_iter_init(&riter, rail->row_map);
    while (g_hash_table_iter_next(&riter, &rk, &rv)) {
        TabRowWidgets *rw = rv;
        const KajoWebTabMeta *t = web_model_find_tab((KajoWebModel *)model, rw->tab_id);
        if (!t || rw->is_pinned != t->pinned)
            g_ptr_array_add(doomed_ids, tab_id_key(rw->tab_id));
    }
    for (guint i = 0; i < doomed_ids->len; i++)
        g_hash_table_remove(rail->row_map, g_ptr_array_index(doomed_ids, i));
    g_ptr_array_unref(doomed_ids);

    GPtrArray *temp_refs = g_ptr_array_new_with_free_func(g_object_unref);
    g_hash_table_iter_init(&riter, rail->row_map);
    while (g_hash_table_iter_next(&riter, &rk, &rv)) {
        TabRowWidgets *rw = rv;
        if (rw->widget) {
            g_ptr_array_add(temp_refs, g_object_ref(rw->widget));
            GtkWidget *par = gtk_widget_get_parent(rw->widget);
            if (par) {
                if (GTK_IS_FLOW_BOX_CHILD(par))
                    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(par), NULL);
                else if (GTK_IS_BOX(par))
                    gtk_box_remove(GTK_BOX(par), rw->widget);
            }
        }
        if (rw->tick) {
            g_ptr_array_add(temp_refs, g_object_ref(rw->tick));
            GtkWidget *tpar = gtk_widget_get_parent(rw->tick);
            if (tpar && GTK_IS_BOX(tpar))
                gtk_box_remove(GTK_BOX(tpar), rw->tick);
        }
    }

    g_ptr_array_set_size(rail->compact_hide, 0);
    g_ptr_array_set_size(rail->snap, 0);
    clear_box_children(rail->ticks_box);
    clear_flowbox_children(rail->pin_grid);
    clear_box_children(rail->tab_list);

    guint pinned_count = 0;
    const gchar *last_group = NULL;

    for (guint i = 0; i < model->tabs->len; i++) {
        const KajoWebTabMeta *t = g_ptr_array_index(model->tabs, i);
        gboolean grp_collapsed = (t->group && *t->group) ? web_model_is_group_collapsed(model, t->group) : FALSE;

        RailSnapEntry *se = g_new0(RailSnapEntry, 1);
        se->id = t->id;
        se->pinned = t->pinned;
        se->group = g_strdup(t->group);
        se->collapsed = grp_collapsed;
        se->visible_in_list = !grp_collapsed || (t->id == model->active_id);
        g_ptr_array_add(rail->snap, se);

        TabRowWidgets *rw = g_hash_table_lookup(rail->row_map, &t->id);
        if (!rw) {
            rw = t->pinned ? create_pin_tile_widgets(rail, t->id)
                           : create_tab_row_widgets(rail, t->id);
            g_hash_table_insert(rail->row_map, tab_id_key(t->id), rw);
        }

        if (rw->tick)
            gtk_box_append(GTK_BOX(rail->ticks_box), rw->tick);

        if (t->pinned) {
            pinned_count++;
            gtk_flow_box_append(GTK_FLOW_BOX(rail->pin_grid), rw->widget);
            update_tab_row_widgets(rail, rw, t, t->id == model->active_id, show_full);
            continue;
        }

        /* Optional Collapsible Tab Group Divider */
        if (t->group && *t->group && g_strcmp0(last_group, t->group) != 0) {
            last_group = t->group;
            guint gcount = 0;
            for (guint j = i; j < model->tabs->len; j++) {
                const KajoWebTabMeta *tj = g_ptr_array_index(model->tabs, j);
                if (!tj->pinned && g_strcmp0(tj->group, t->group) == 0)
                    gcount++;
            }
            GtkWidget *ghdr_btn = gtk_button_new();
            gtk_widget_add_css_class(ghdr_btn, "web-tab-group-hdr");
            RailGroupClickCtx *gctx = g_new0(RailGroupClickCtx, 1);
            gctx->rail = rail;
            gctx->group = g_strdup(t->group);
            g_signal_connect_data(ghdr_btn, "clicked", G_CALLBACK(on_group_hdr_clicked),
                                  gctx, rail_group_click_ctx_free, 0);

            GtkWidget *ghdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            gtk_widget_set_valign(ghdr, GTK_ALIGN_CENTER);

            GtkWidget *gdot = gtk_label_new("▪");
            gtk_widget_set_halign(gdot, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(gdot, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(gdot, "web-tab-group-dot");

            GtkWidget *gcaret = gtk_label_new(grp_collapsed ? "▸" : "▾");
            gtk_widget_set_valign(gcaret, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(gcaret, "web-tab-group-count");

            g_autofree gchar *gup = g_utf8_strup(t->group, -1);
            GtkWidget *glbl = gtk_label_new(gup);
            gtk_widget_set_hexpand(glbl, TRUE);
            gtk_widget_set_halign(glbl, GTK_ALIGN_START);
            gtk_widget_set_valign(glbl, GTK_ALIGN_CENTER);
            gtk_label_set_ellipsize(GTK_LABEL(glbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_width_chars(GTK_LABEL(glbl), 1);

            g_autofree gchar *gcnt_s = g_strdup_printf("%u", gcount);
            GtkWidget *gcnt = gtk_label_new(gcnt_s);
            gtk_widget_set_valign(gcnt, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(gcnt, "web-tab-group-count");

            gtk_box_append(GTK_BOX(ghdr), gdot);
            gtk_box_append(GTK_BOX(ghdr), gcaret);
            gtk_box_append(GTK_BOX(ghdr), glbl);
            gtk_box_append(GTK_BOX(ghdr), gcnt);
            gtk_button_set_child(GTK_BUTTON(ghdr_btn), ghdr);
            gtk_box_append(GTK_BOX(rail->tab_list), ghdr_btn);
            g_ptr_array_add(rail->compact_hide, gcaret);
            g_ptr_array_add(rail->compact_hide, glbl);
            g_ptr_array_add(rail->compact_hide, gcnt);
        }

        if (grp_collapsed && t->id != model->active_id) {
            update_tab_row_widgets(rail, rw, t, FALSE, show_full);
            continue;
        }

        gtk_box_append(GTK_BOX(rail->tab_list), rw->widget);
        update_tab_row_widgets(rail, rw, t, t->id == model->active_id, show_full);
    }

    g_ptr_array_unref(temp_refs);

    gtk_widget_set_visible(rail->pin_grid, pinned_count > 0);
    if (rail->btn_undo)
        gtk_widget_set_sensitive(rail->btn_undo, model->closed_stack->len > 0);
    sync_visual_state(rail);
}

void
web_tab_rail_toggle_reveal(KajoWebTabRail *rail)
{
    if (!rail || rail->mode == KAJO_WEB_RAIL_PINNED)
        return;
    if (rail->fsm.state == KAJO_RAIL_FSM_EXPANDED) {
        handle_fsm_action(rail, web_rail_fsm_escape(&rail->fsm));
    } else {
        handle_fsm_action(rail, web_rail_fsm_keyboard_reveal(&rail->fsm));
    }
}

gboolean
web_tab_rail_escape(KajoWebTabRail *rail)
{
    if (!rail || rail->mode == KAJO_WEB_RAIL_PINNED)
        return FALSE;
    KajoWebRailFsmAction act = web_rail_fsm_escape(&rail->fsm);
    if (act == KAJO_RAIL_ACT_COLLAPSE) {
        handle_fsm_action(rail, act);
        return TRUE;
    }
    return FALSE;
}

void
web_tab_rail_focus_lost(KajoWebTabRail *rail)
{
    if (!rail || rail->mode == KAJO_WEB_RAIL_PINNED)
        return;
    handle_fsm_action(rail, web_rail_fsm_focus_lost(&rail->fsm));
}

gboolean
web_tab_rail_is_expanded(const KajoWebTabRail *rail)
{
    if (!rail)
        return FALSE;
    return (rail->mode == KAJO_WEB_RAIL_PINNED) || (rail->fsm.state == KAJO_RAIL_FSM_EXPANDED);
}
