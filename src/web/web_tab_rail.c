#include "web_tab_rail.h"
#include <string.h>

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

    GtkWidget              *trigger;        /* 12px rgba(0,0,0,0.01) overlay trigger strip */
    GtkWidget              *ticks_box;      /* 2px vertical tab ticks inside trigger */
    GtkWidget              *compact_spacer; /* 44px docked spacer in compact mode */
    GtkWidget              *root;           /* main rail container */
    GtkWidget              *hdr_bar;
    GtkWidget              *hdr_mode_lbl;
    GtkWidget              *pin_grid;       /* pinned square tiles */
    GtkWidget              *tab_list;       /* scrollable vertical list */
    GtkWidget              *footer_box;
    GtkWidget              *btn_new;
    GtkWidget              *btn_new_lbl;
    GtkWidget              *btn_undo;
    GtkWidget              *btn_undo_lbl;

    /* Borrowed child widgets updated in-place on compact collapse/expand */
    GPtrArray              *compact_hide;   /* GtkWidget* hidden when compact+collapsed */
    GPtrArray              *compact_icons;  /* GtkWidget* centered when compact+collapsed */
    GPtrArray              *row_sel_btns;   /* GtkWidget* select buttons */
};

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
        gtk_widget_set_size_request(rail->root, compact_collapsed ? 44 : rail->width_px, -1);
    } else {
        gtk_widget_set_visible(rail->root, TRUE);
        gtk_widget_set_size_request(rail->root, rail->width_px, -1);
    }

    /* Toggle compact-hidden widgets cleanly so GTK4 never blows out 44px width */
    gboolean show_full = !compact_collapsed;
    gtk_widget_set_visible(rail->hdr_bar, show_full);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(rail->pin_grid), compact_collapsed ? 1 : 4);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(rail->footer_box),
                                   compact_collapsed ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);

    if (rail->btn_new_lbl)
        gtk_label_set_text(GTK_LABEL(rail->btn_new_lbl), show_full ? "+ NEW TAB" : "+");
    if (rail->btn_undo_lbl)
        gtk_label_set_text(GTK_LABEL(rail->btn_undo_lbl), show_full ? "↺ UNDO" : "↺");

    for (guint i = 0; i < rail->compact_hide->len; i++) {
        GtkWidget *w = g_ptr_array_index(rail->compact_hide, i);
        gtk_widget_set_visible(w, show_full);
    }
    for (guint i = 0; i < rail->compact_icons->len; i++) {
        GtkWidget *ico = g_ptr_array_index(rail->compact_icons, i);
        gtk_widget_set_halign(ico, compact_collapsed ? GTK_ALIGN_CENTER : GTK_ALIGN_START);
        gtk_widget_set_hexpand(ico, compact_collapsed);
    }

    gtk_label_set_text(GTK_LABEL(rail->hdr_mode_lbl),
                       rail->mode == KAJO_WEB_RAIL_PINNED ? "PINNED"
                       : (rail->mode == KAJO_WEB_RAIL_COMPACT ? "COMPACT" : "AUTOHIDE"));
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
    rail->compact_icons = g_ptr_array_new();
    rail->row_sel_btns = g_ptr_array_new();

    /* 12px Edge Trigger Strip (rgba(0,0,0,0.01) — effortless hit-zone, never opacity: 0) */
    rail->trigger = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(rail->trigger, 12, -1);
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

    /* Compact mode docked spacer (44px) */
    rail->compact_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(rail->compact_spacer, 44, -1);
    gtk_widget_set_visible(rail->compact_spacer, FALSE);
    g_object_ref_sink(rail->compact_spacer);

    /* Main Vertical Rail */
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

    rail->hdr_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(rail->hdr_bar, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(rail->hdr_bar, "web-rail-header");
    GtkWidget *ht = gtk_label_new("TABS • VERTICAL");
    gtk_widget_set_hexpand(ht, TRUE);
    gtk_widget_set_halign(ht, GTK_ALIGN_START);
    gtk_widget_set_valign(ht, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(ht, "web-rail-title");
    rail->hdr_mode_lbl = gtk_label_new("AUTOHIDE");
    gtk_widget_add_css_class(rail->hdr_mode_lbl, "web-badge-telemetry");
    gtk_widget_set_valign(rail->hdr_mode_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(rail->hdr_bar), ht);
    gtk_box_append(GTK_BOX(rail->hdr_bar), rail->hdr_mode_lbl);
    gtk_box_append(GTK_BOX(rail->root), rail->hdr_bar);

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
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), rail->tab_list);
    gtk_box_append(GTK_BOX(rail->root), scroll);

    rail->footer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(rail->footer_box, "web-rail-footer");

    rail->btn_new = gtk_button_new();
    rail->btn_new_lbl = gtk_label_new("+ NEW TAB");
    gtk_widget_set_valign(rail->btn_new_lbl, GTK_ALIGN_CENTER);
    gtk_button_set_child(GTK_BUTTON(rail->btn_new), rail->btn_new_lbl);
    gtk_widget_set_hexpand(rail->btn_new, TRUE);
    gtk_widget_set_tooltip_text(rail->btn_new, "New Tab (Ctrl+T)");
    gtk_widget_add_css_class(rail->btn_new, "web-rail-foot-btn");
    gtk_widget_add_css_class(rail->btn_new, "primary");
    g_signal_connect(rail->btn_new, "clicked", G_CALLBACK(on_new_tab_clicked), rail);

    rail->btn_undo = gtk_button_new();
    rail->btn_undo_lbl = gtk_label_new("↺ UNDO");
    gtk_widget_set_valign(rail->btn_undo_lbl, GTK_ALIGN_CENTER);
    gtk_button_set_child(GTK_BUTTON(rail->btn_undo), rail->btn_undo_lbl);
    gtk_widget_set_tooltip_text(rail->btn_undo, "Reopen Closed Tab (Ctrl+Shift+T)");
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
    g_clear_pointer(&rail->compact_hide, g_ptr_array_unref);
    g_clear_pointer(&rail->compact_icons, g_ptr_array_unref);
    g_clear_pointer(&rail->row_sel_btns, g_ptr_array_unref);
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

void
web_tab_rail_rebuild(KajoWebTabRail *rail, const KajoWebModel *model)
{
    if (!rail || !model)
        return;

    g_ptr_array_set_size(rail->compact_hide, 0);
    g_ptr_array_set_size(rail->compact_icons, 0);
    g_ptr_array_set_size(rail->row_sel_btns, 0);

    clear_box_children(rail->ticks_box);
    clear_flowbox_children(rail->pin_grid);
    clear_box_children(rail->tab_list);

    guint pinned_count = 0;
    const gchar *last_group = NULL;

    for (guint i = 0; i < model->tabs->len; i++) {
        const KajoWebTabMeta *t = g_ptr_array_index(model->tabs, i);
        gboolean active = (t->id == model->active_id);

        /* Edge tick inside autohide trigger */
        GtkWidget *tick = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_size_request(tick, 2, active ? 22 : 14);
        gtk_widget_add_css_class(tick, active ? "web-rail-tick-active" : "web-rail-tick");
        gtk_box_append(GTK_BOX(rail->ticks_box), tick);

        if (t->pinned) {
            pinned_count++;
            g_autofree gchar *host = web_model_extract_host(t->uri);
            gchar glyph[8] = "🌐";
            if (g_str_has_prefix(t->uri, "kajo://"))
                g_strlcpy(glyph, "❖", sizeof(glyph));
            else if (host && g_ascii_isalpha(host[0])) {
                glyph[0] = (char)g_ascii_toupper(host[0]);
                glyph[1] = '\0';
            }
            GtkWidget *pt = gtk_button_new_with_label(glyph);
            gtk_widget_set_size_request(pt, 32, 32);
            gtk_widget_set_tooltip_text(pt, t->title ? t->title : t->uri);
            gtk_widget_add_css_class(pt, "web-pin-tile");
            if (active)
                gtk_widget_add_css_class(pt, "active");
            TabRowAction *a = g_new0(TabRowAction, 1);
            a->rail = rail;
            a->tab_id = t->id;
            g_signal_connect_data(pt, "clicked", G_CALLBACK(on_tab_select_clicked), a, rail_free_closure_data, 0);
            gtk_flow_box_append(GTK_FLOW_BOX(rail->pin_grid), pt);
            continue;
        }

        /* Optional Tab Group Header */
        if (t->group && *t->group && g_strcmp0(last_group, t->group) != 0) {
            last_group = t->group;
            guint gcount = 0;
            for (guint j = i; j < model->tabs->len; j++) {
                const KajoWebTabMeta *tj = g_ptr_array_index(model->tabs, j);
                if (!tj->pinned && g_strcmp0(tj->group, t->group) == 0)
                    gcount++;
            }
            GtkWidget *ghdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_set_valign(ghdr, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(ghdr, "web-tab-group-hdr");
            g_autofree gchar *gup = g_utf8_strup(t->group, -1);
            GtkWidget *glbl = gtk_label_new(gup);
            gtk_widget_set_hexpand(glbl, TRUE);
            gtk_widget_set_halign(glbl, GTK_ALIGN_START);
            gtk_widget_set_valign(glbl, GTK_ALIGN_CENTER);
            g_autofree gchar *gcnt_s = g_strdup_printf("%u", gcount);
            GtkWidget *gcnt = gtk_label_new(gcnt_s);
            gtk_widget_set_valign(gcnt, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(gcnt, "web-tab-group-count");
            gtk_box_append(GTK_BOX(ghdr), glbl);
            gtk_box_append(GTK_BOX(ghdr), gcnt);
            gtk_box_append(GTK_BOX(rail->tab_list), ghdr);
            g_ptr_array_add(rail->compact_hide, ghdr);
        }

        /* Regular vertical tab row (34px tall) */
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(row, "web-tab-row");
        if (active)
            gtk_widget_add_css_class(row, "active");
        if (t->suspended)
            gtk_widget_add_css_class(row, "suspended");

        GtkWidget *sel_btn = gtk_button_new();
        gtk_widget_set_hexpand(sel_btn, TRUE);
        gtk_widget_set_valign(sel_btn, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(sel_btn, t->title && *t->title ? t->title : t->uri);
        gtk_widget_add_css_class(sel_btn, "web-tab-select-btn");

        GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
        gtk_widget_set_valign(inner, GTK_ALIGN_CENTER);

        g_autofree gchar *host = web_model_extract_host(t->uri);
        const gchar *ico_txt = t->suspended ? "💤"
                             : (g_str_has_prefix(t->uri, "kajo://") ? "❖" : "🌐");
        GtkWidget *ico = gtk_label_new(ico_txt);
        gtk_widget_set_valign(ico, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(ico, "web-tab-ico");
        gtk_box_append(GTK_BOX(inner), ico);
        g_ptr_array_add(rail->compact_icons, ico);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(vbox, TRUE);
        gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

        GtkWidget *title_lbl = gtk_label_new(t->title && *t->title ? t->title : t->uri);
        gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(title_lbl), 1);
        gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
        gtk_widget_set_valign(title_lbl, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(title_lbl, "web-tab-title");
        gtk_box_append(GTK_BOX(vbox), title_lbl);

        if (rail->show_domains) {
            GtkWidget *dom = gtk_label_new(host ? host : "");
            gtk_label_set_ellipsize(GTK_LABEL(dom), PANGO_ELLIPSIZE_END);
            gtk_label_set_width_chars(GTK_LABEL(dom), 1);
            gtk_widget_set_halign(dom, GTK_ALIGN_START);
            gtk_widget_set_valign(dom, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(dom, "web-tab-domain");
            gtk_box_append(GTK_BOX(vbox), dom);
        }
        gtk_box_append(GTK_BOX(inner), vbox);
        g_ptr_array_add(rail->compact_hide, vbox);

        gtk_button_set_child(GTK_BUTTON(sel_btn), inner);
        TabRowAction *sa = g_new0(TabRowAction, 1);
        sa->rail = rail;
        sa->tab_id = t->id;
        g_signal_connect_data(sel_btn, "clicked", G_CALLBACK(on_tab_select_clicked), sa, rail_free_closure_data, 0);
        gtk_box_append(GTK_BOX(row), sel_btn);
        g_ptr_array_add(rail->row_sel_btns, sel_btn);

        if (t->audible || t->muted) {
            GtkWidget *mbtn = gtk_button_new_with_label(t->muted ? "🔇" : "🔊");
            gtk_widget_set_valign(mbtn, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(mbtn, "web-tab-mini-btn");
            if (t->muted)
                gtk_widget_add_css_class(mbtn, "muted");
            TabRowAction *ma = g_new0(TabRowAction, 1);
            ma->rail = rail;
            ma->tab_id = t->id;
            g_signal_connect_data(mbtn, "clicked", G_CALLBACK(on_tab_mute_clicked), ma, rail_free_closure_data, 0);
            gtk_box_append(GTK_BOX(row), mbtn);
            g_ptr_array_add(rail->compact_hide, mbtn);
        }

        GtkWidget *cbtn = gtk_button_new_with_label("✕");
        gtk_widget_set_valign(cbtn, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(cbtn, "web-tab-close-btn");
        TabRowAction *ca = g_new0(TabRowAction, 1);
        ca->rail = rail;
        ca->tab_id = t->id;
        g_signal_connect_data(cbtn, "clicked", G_CALLBACK(on_tab_close_clicked), ca, rail_free_closure_data, 0);
        gtk_box_append(GTK_BOX(row), cbtn);
        g_ptr_array_add(rail->compact_hide, cbtn);

        gtk_box_append(GTK_BOX(rail->tab_list), row);
    }

    gtk_widget_set_visible(rail->pin_grid, pinned_count > 0);
    sync_visual_state(rail);
}

void
web_tab_rail_toggle_reveal(KajoWebTabRail *rail)
{
    if (!rail)
        return;
    if (rail->mode == KAJO_WEB_RAIL_PINNED) {
        rail->mode = KAJO_WEB_RAIL_AUTOHIDE;
        handle_fsm_action(rail, web_rail_fsm_escape(&rail->fsm));
    } else if (rail->fsm.state == KAJO_RAIL_FSM_EXPANDED) {
        handle_fsm_action(rail, web_rail_fsm_escape(&rail->fsm));
    } else {
        handle_fsm_action(rail, web_rail_fsm_keyboard_reveal(&rail->fsm));
    }
}

void
web_tab_rail_escape(KajoWebTabRail *rail)
{
    if (!rail || rail->mode == KAJO_WEB_RAIL_PINNED)
        return;
    handle_fsm_action(rail, web_rail_fsm_escape(&rail->fsm));
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
