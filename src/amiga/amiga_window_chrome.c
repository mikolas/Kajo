#include "amiga_window_private.h"
#include "../compositor/compositor.h"

static void on_hdr_build_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_trigger_build(KAJO_AMIGA_WINDOW(ud)); }
static void on_hdr_run_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_debug_run_free(KAJO_AMIGA_WINDOW(ud)); }
static void on_hdr_debug_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_debug_start_or_continue(KAJO_AMIGA_WINDOW(ud)); }
static void on_hdr_step_over_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_debug_step_over(KAJO_AMIGA_WINDOW(ud)); }
static void on_hdr_step_into_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_debug_step_into(KAJO_AMIGA_WINDOW(ud)); }
static void on_hdr_step_out_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_debug_step_out(KAJO_AMIGA_WINDOW(ud)); }
static void on_hdr_restart_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_debug_restart(KAJO_AMIGA_WINDOW(ud)); }
static void on_hdr_snapshot_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_debug_snapshot_now(KAJO_AMIGA_WINDOW(ud)); }
static void on_hdr_stop_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_debug_stop(KAJO_AMIGA_WINDOW(ud)); }
static void on_hdr_attach_detach_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_debug_stop(KAJO_AMIGA_WINDOW(ud)); }
static void on_hdr_float_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud G_GNUC_UNUSED) { niri_toggle_window_floating(); }
static void on_hdr_close_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { gtk_window_close(GTK_WINDOW(ud)); }

static void
on_hdr_resume_pause_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) == AMIGA_RSP_RUNNING) {
    amiga_window_debug_pause(self);
  } else {
    amiga_window_debug_start_or_continue(self);
  }
}

static void
on_layout_toggle_clicked(GtkToggleButton *tb, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->updating_layout_toggles) return;
  bool act = gtk_toggle_button_get_active(tb);
  if (GTK_WIDGET(tb) == self->btn_tog_left) {
    self->user_sidebar_open = act;
    gtk_widget_set_visible(self->sidebar_box, act);
  } else if (GTK_WIDGET(tb) == self->btn_tog_bottom) {
    self->user_drawer_open = act;
    gtk_widget_set_visible(self->drawer_box, act);
  } else if (GTK_WIDGET(tb) == self->btn_tog_right) {
    self->user_inspector_open = act;
    gtk_widget_set_visible(self->inspector_box, act);
  }
}

static void
on_sb_tab_toggled(GtkToggleButton *tb, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!gtk_toggle_button_get_active(tb)) return;
  const char *pg = g_object_get_data(G_OBJECT(tb), "sb-page");
  if (pg) gtk_stack_set_visible_child_name(GTK_STACK(self->sidebar_stack), pg);
}

static void
on_half_pane_toggled(GtkToggleButton *tb, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!gtk_toggle_button_get_active(tb)) return;
  self->active_half_pane = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tb), "half-idx"));
  amiga_window_apply_half_screen_visibility(self);
}

static void
on_status_cycles_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  self->show_cycle_gutter = !self->show_cycle_gutter;
  gtk_widget_queue_draw(self->source_view);
  amiga_window_show_toast(self, self->show_cycle_gutter ? "68k cycle gutter ON" : "68k cycle gutter OFF");
}

static void
on_status_diag_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  self->user_drawer_open = true;
  gtk_widget_set_visible(self->drawer_box, TRUE);
  if (self->btn_tog_bottom) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tog_bottom), TRUE);
  amiga_window_drawer_select(self, "build");
}

static void
on_status_hunk_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (self->first_hunk_addr != 0) amiga_window_jump_disasm(self, self->first_hunk_addr);
}

static void
on_proj_menu_action(GSimpleAction *a, GVariant *p G_GNUC_UNUSED, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  const char *nm = g_action_get_name(G_ACTION(a));
  if (g_strcmp0(nm, "new") == 0) amiga_window_act_new_project(self);
  else if (g_strcmp0(nm, "open") == 0) amiga_window_act_open_project(self);
  else if (g_strcmp0(nm, "setup") == 0) amiga_window_open_setup_tab(self);
  else if (g_strcmp0(nm, "attach") == 0) amiga_window_act_attach(self);
  else if (g_strcmp0(nm, "welcome") == 0) amiga_window_apply_session_state(self, AMIGA_SESSION_WELCOME);
  else if (g_strcmp0(nm, "settings") == 0) amiga_window_act_settings(self);
}

static void on_tree_file_open(const char *abs_path, gpointer ud) {
  amiga_window_open_file_at_line(KAJO_AMIGA_WINDOW(ud), abs_path, 1);
}
static void on_dropdown_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *ps G_GNUC_UNUSED, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  amiga_window_debug_reload_symbols(self);
  amiga_window_lsp_sync_project(self);
  amiga_window_sync_compat_badge(self);
}

static GtkWidget *
build_header_left(KajoAmigaWindow *self)
{
  GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
  GtkWidget *title = gtk_label_new("◈ KAJO AMIGA");
  gtk_widget_add_css_class(title, "settings-header-title");

  GSimpleActionGroup *pg = g_simple_action_group_new();
  const char *pacts[] = { "new", "open", "setup", "attach", "welcome", "settings" };
  for (gsize i = 0; i < G_N_ELEMENTS(pacts); i++) {
    GSimpleAction *sa = g_simple_action_new(pacts[i], NULL);
    g_signal_connect(sa, "activate", G_CALLBACK(on_proj_menu_action), self);
    g_action_map_add_action(G_ACTION_MAP(pg), G_ACTION(sa));
    g_object_unref(sa);
  }
  gtk_widget_insert_action_group(GTK_WIDGET(self), "proj", G_ACTION_GROUP(pg));
  g_object_unref(pg);

  GMenu *pmenu = g_menu_new();
  g_menu_append(pmenu, "New Project…\tCtrl+Shift+N", "proj.new");
  g_menu_append(pmenu, "Open Workspace…\tCtrl+O", "proj.open");
  g_menu_append(pmenu, "Project Setup (kajo-amiga.toml)", "proj.setup");
  g_menu_append(pmenu, "Attach Amiberry :2345\tCtrl+Shift+A", "proj.attach");
  g_menu_append(pmenu, "Welcome Screen", "proj.welcome");
  g_menu_append(pmenu, "Toolchain Settings…\tCtrl+,", "proj.settings");

  self->btn_proj_menu = gtk_menu_button_new();
  gtk_menu_button_set_label(GTK_MENU_BUTTON(self->btn_proj_menu), "📁 PROJ");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(self->btn_proj_menu), G_MENU_MODEL(pmenu));
  gtk_widget_add_css_class(self->btn_proj_menu, "ide-toolbar-btn");
  g_object_unref(pmenu);

  self->dd_target = gtk_drop_down_new_from_strings((const char *[]){"default", NULL});
  gtk_widget_add_css_class(self->dd_target, "amiga-header-dd");
  gtk_widget_set_tooltip_text(self->dd_target, "Active Build Target");
  g_signal_connect(self->dd_target, "notify::selected", G_CALLBACK(on_dropdown_changed), self);

  self->dd_profile = gtk_drop_down_new_from_strings((const char *[]){"A500 OCS", NULL});
  gtk_widget_add_css_class(self->dd_profile, "amiga-header-dd");
  gtk_widget_set_tooltip_text(self->dd_profile, "Amiberry Hardware Profile");
  g_signal_connect(self->dd_profile, "notify::selected", G_CALLBACK(on_dropdown_changed), self);

  self->compat_badge = gtk_label_new("");
  gtk_widget_add_css_class(self->compat_badge, "amiga-compat-warn");
  gtk_widget_set_visible(self->compat_badge, FALSE);

  gtk_box_append(GTK_BOX(left), title);
  gtk_box_append(GTK_BOX(left), self->btn_proj_menu);
  gtk_box_append(GTK_BOX(left), self->dd_target);
  gtk_box_append(GTK_BOX(left), self->dd_profile);
  gtk_box_append(GTK_BOX(left), self->compat_badge);
  return left;
}

static GtkWidget *
build_header_transport(KajoAmigaWindow *self)
{
  self->transport_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_widget_add_css_class(self->transport_box, "amiga-transport-cluster");
  gtk_widget_set_halign(self->transport_box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(self->transport_box, TRUE);

  self->offline_grp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  self->btn_build = gtk_button_new_with_label("🔨 BUILD");
  self->btn_run = gtk_button_new_with_label("▶ RUN");
  self->btn_debug = gtk_button_new_with_label("🐞 F5");
  gtk_widget_add_css_class(self->btn_build, "ide-toolbar-btn");
  gtk_widget_add_css_class(self->btn_build, "accent-btn");
  gtk_widget_add_css_class(self->btn_run, "ide-toolbar-btn");
  gtk_widget_add_css_class(self->btn_run, "run-btn");
  gtk_widget_add_css_class(self->btn_debug, "ide-toolbar-btn");
  g_signal_connect(self->btn_build, "clicked", G_CALLBACK(on_hdr_build_clicked), self);
  g_signal_connect(self->btn_run, "clicked", G_CALLBACK(on_hdr_run_clicked), self);
  g_signal_connect(self->btn_debug, "clicked", G_CALLBACK(on_hdr_debug_clicked), self);
  gtk_box_append(GTK_BOX(self->offline_grp), self->btn_build);
  gtk_box_append(GTK_BOX(self->offline_grp), self->btn_run);
  gtk_box_append(GTK_BOX(self->offline_grp), self->btn_debug);

  self->live_grp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  self->lbl_state_badge = gtk_label_new("● PAUSED");
  gtk_widget_add_css_class(self->lbl_state_badge, "amiga-state-badge");
  self->btn_resume_pause = gtk_button_new_with_label("▶");
  self->btn_step_over = gtk_button_new_with_label("↷");
  self->btn_step_into = gtk_button_new_with_label("⤵");
  self->btn_step_out = gtk_button_new_with_label("⤴");
  self->btn_restart = gtk_button_new_with_label("↻");
  self->btn_snapshot = gtk_button_new_with_label("↻ SNAP");
  self->btn_stop = gtk_button_new_with_label("■");
  self->btn_attach_detach = gtk_button_new_with_label("🔌");

  GtkWidget *lbtns[] = {
    self->btn_resume_pause, self->btn_step_over, self->btn_step_into,
    self->btn_step_out, self->btn_restart, self->btn_snapshot,
    self->btn_stop, self->btn_attach_detach
  };
  for (gsize i = 0; i < G_N_ELEMENTS(lbtns); i++) gtk_widget_add_css_class(lbtns[i], "ide-toolbar-btn");
  gtk_widget_add_css_class(self->btn_stop, "stop-btn");
  gtk_widget_set_tooltip_text(self->btn_step_over, "Step Over (F10)");
  gtk_widget_set_tooltip_text(self->btn_step_into, "Step Into (F11)");
  gtk_widget_set_tooltip_text(self->btn_step_out, "Step Out (F12)");
  gtk_widget_set_tooltip_text(self->btn_restart, "Restart Debug (Ctrl+Shift+F5)");
  gtk_widget_set_tooltip_text(self->btn_snapshot, "Hardware Snapshot (Safe while running)");

  g_signal_connect(self->btn_resume_pause, "clicked", G_CALLBACK(on_hdr_resume_pause_clicked), self);
  g_signal_connect(self->btn_step_over, "clicked", G_CALLBACK(on_hdr_step_over_clicked), self);
  g_signal_connect(self->btn_step_into, "clicked", G_CALLBACK(on_hdr_step_into_clicked), self);
  g_signal_connect(self->btn_step_out, "clicked", G_CALLBACK(on_hdr_step_out_clicked), self);
  g_signal_connect(self->btn_restart, "clicked", G_CALLBACK(on_hdr_restart_clicked), self);
  g_signal_connect(self->btn_snapshot, "clicked", G_CALLBACK(on_hdr_snapshot_clicked), self);
  g_signal_connect(self->btn_stop, "clicked", G_CALLBACK(on_hdr_stop_clicked), self);
  g_signal_connect(self->btn_attach_detach, "clicked", G_CALLBACK(on_hdr_attach_detach_clicked), self);

  gtk_box_append(GTK_BOX(self->live_grp), self->lbl_state_badge);
  for (gsize i = 0; i < G_N_ELEMENTS(lbtns); i++) gtk_box_append(GTK_BOX(self->live_grp), lbtns[i]);
  gtk_box_append(GTK_BOX(self->transport_box), self->offline_grp);
  gtk_box_append(GTK_BOX(self->transport_box), self->live_grp);
  return self->transport_box;
}

GtkWidget *
amiga_window_chrome_build_header(KajoAmigaWindow *self)
{
  GtkWidget *handle = gtk_window_handle_new();
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(hdr, "settings-header-bar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  self->layout_toggles_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  self->btn_tog_left = gtk_toggle_button_new_with_label("◧");
  self->btn_tog_bottom = gtk_toggle_button_new_with_label("⬓");
  self->btn_tog_right = gtk_toggle_button_new_with_label("◨");
  GtkWidget *togs[3] = { self->btn_tog_left, self->btn_tog_bottom, self->btn_tog_right };
  const char *tog_tips[3] = {
    "Toggle Left Sidebar", "Toggle Bottom Drawer", "Toggle Hardware Inspector"
  };
  for (int i = 0; i < 3; i++) {
    gtk_widget_add_css_class(togs[i], "amiga-mini-btn");
    gtk_widget_add_css_class(togs[i], "amiga-hdr-icon-btn");
    gtk_widget_set_tooltip_text(togs[i], tog_tips[i]);
    g_signal_connect(togs[i], "toggled", G_CALLBACK(on_layout_toggle_clicked), self);
    gtk_box_append(GTK_BOX(self->layout_toggles_box), togs[i]);
  }
  gtk_box_append(GTK_BOX(right), self->layout_toggles_box);

  GtkWidget *b_set = gtk_button_new_with_label("⚙");
  gtk_widget_add_css_class(b_set, "amiga-mini-btn");
  gtk_widget_add_css_class(b_set, "amiga-hdr-icon-btn");
  gtk_widget_set_tooltip_text(b_set, "Toolchain & ROM Settings (Ctrl+,)");
  g_signal_connect_swapped(b_set, "clicked", G_CALLBACK(amiga_window_act_settings), self);
  GtkWidget *b_flt = gtk_button_new_with_label("🗗");
  gtk_widget_add_css_class(b_flt, "amiga-mini-btn");
  gtk_widget_add_css_class(b_flt, "amiga-hdr-icon-btn");
  gtk_widget_set_tooltip_text(b_flt, "Float / Tile Window");
  g_signal_connect(b_flt, "clicked", G_CALLBACK(on_hdr_float_clicked), self);
  GtkWidget *b_cls = gtk_button_new_with_label("✕");
  gtk_widget_add_css_class(b_cls, "settings-close-btn");
  g_signal_connect(b_cls, "clicked", G_CALLBACK(on_hdr_close_clicked), self);
  gtk_box_append(GTK_BOX(right), b_set);
  gtk_box_append(GTK_BOX(right), b_flt);
  gtk_box_append(GTK_BOX(right), b_cls);

  gtk_box_append(GTK_BOX(hdr), build_header_left(self));
  gtk_box_append(GTK_BOX(hdr), build_header_transport(self));
  gtk_box_append(GTK_BOX(hdr), right);

  GtkWidget *hdr_scr = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(hdr_scr),
                                 GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(hdr_scr), FALSE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(hdr_scr), hdr);
  gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), hdr_scr);
  return handle;
}

GtkWidget *
amiga_window_chrome_build_sidebar(KajoAmigaWindow *self)
{
  self->sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(self->sidebar_box, "amiga-sidebar");

  self->sidebar_tabs_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(self->sidebar_tabs_bar, "amiga-sb-tabs");
  self->btn_sb_files = gtk_toggle_button_new_with_label("📁 FILES");
  self->btn_sb_search = gtk_toggle_button_new_with_label("🔎 SEARCH");
  self->btn_sb_debug = gtk_toggle_button_new_with_label("🐞 DEBUG");
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_sb_search), GTK_TOGGLE_BUTTON(self->btn_sb_files));
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_sb_debug), GTK_TOGGLE_BUTTON(self->btn_sb_files));

  struct { GtkWidget *w; const char *pg; } sbt[3] = {
    { self->btn_sb_files, "files" },
    { self->btn_sb_search, "search" },
    { self->btn_sb_debug, "debug" },
  };
  for (int i = 0; i < 3; i++) {
    gtk_widget_add_css_class(sbt[i].w, "amiga-seg-btn");
    gtk_widget_set_hexpand(sbt[i].w, TRUE);
    g_object_set_data(G_OBJECT(sbt[i].w), "sb-page", (gpointer)sbt[i].pg);
    g_signal_connect(sbt[i].w, "toggled", G_CALLBACK(on_sb_tab_toggled), self);
    gtk_box_append(GTK_BOX(self->sidebar_tabs_bar), sbt[i].w);
  }
  gtk_box_append(GTK_BOX(self->sidebar_box), self->sidebar_tabs_bar);

  self->sidebar_stack = gtk_stack_new();
  gtk_stack_set_hhomogeneous(GTK_STACK(self->sidebar_stack), FALSE);
  gtk_stack_set_vhomogeneous(GTK_STACK(self->sidebar_stack), FALSE);
  gtk_widget_set_vexpand(self->sidebar_stack, TRUE);

  GtkWidget *files_pg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->tree_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(self->tree_slot, TRUE);
  self->file_tree = ide_tree_new(".", on_tree_file_open, self);
  gtk_box_append(GTK_BOX(self->tree_slot), ide_tree_get_widget(self->file_tree));
  gtk_box_append(GTK_BOX(files_pg), self->tree_slot);
  gtk_box_append(GTK_BOX(files_pg), amiga_window_outline_build_box(self));
  gtk_stack_add_named(GTK_STACK(self->sidebar_stack), files_pg, "files");
  gtk_stack_add_named(GTK_STACK(self->sidebar_stack), amiga_window_search_build_page(self), "search");

  self->debug_sidebar = amiga_debug_sidebar_new();
  AmigaDebugSidebarCallbacks scbs = {0};
  amiga_window_debug_fill_sidebar_callbacks(&scbs);
  amiga_debug_sidebar_set_callbacks(self->debug_sidebar, &scbs, self);
  gtk_stack_add_named(GTK_STACK(self->sidebar_stack), GTK_WIDGET(self->debug_sidebar), "debug");

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_sb_files), TRUE);
  gtk_box_append(GTK_BOX(self->sidebar_box), self->sidebar_stack);
  return self->sidebar_box;
}

GtkWidget *
amiga_window_chrome_build_half_strip(KajoAmigaWindow *self)
{
  self->half_strip_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_widget_add_css_class(self->half_strip_box, "amiga-half-strip");
  gtk_widget_set_visible(self->half_strip_box, FALSE);
  static const char *const k_half_lbl[5] = {
    "🐞 CPU/STATE", "📝 CODE", "⚙ DISASM", "🔧 CHIPS/BPL", "▓ HEX/CMD"
  };
  for (int i = 0; i < 5; i++) {
    self->btn_half_panes[i] = gtk_toggle_button_new_with_label(k_half_lbl[i]);
    gtk_widget_add_css_class(self->btn_half_panes[i], "amiga-seg-btn");
    gtk_widget_set_hexpand(self->btn_half_panes[i], TRUE);
    if (i > 0) {
      gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->btn_half_panes[i]),
                                  GTK_TOGGLE_BUTTON(self->btn_half_panes[0]));
    }
    g_object_set_data(G_OBJECT(self->btn_half_panes[i]), "half-idx", GINT_TO_POINTER(i));
    g_signal_connect(self->btn_half_panes[i], "toggled", G_CALLBACK(on_half_pane_toggled), self);
    gtk_box_append(GTK_BOX(self->half_strip_box), self->btn_half_panes[i]);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_half_panes[1]), TRUE);
  return self->half_strip_box;
}

GtkWidget *
amiga_window_chrome_build_status_bar(KajoAmigaWindow *self)
{
  GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(sbar, "amiga-status-bar");
  gtk_widget_set_valign(sbar, GTK_ALIGN_CENTER);

  self->mode_pill = gtk_label_new("NORMAL");
  gtk_widget_add_css_class(self->mode_pill, "amiga-mode-pill");
  self->lbl_ws_state = gtk_label_new("Welcome");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_ws_state), 0.0f);
  self->lbl_lsp_status = gtk_label_new("LSP: off");
  gtk_widget_add_css_class(self->lbl_lsp_status, "amiga-muted-hint");
  gtk_widget_set_hexpand(self->lbl_ws_state, TRUE);

  self->btn_sel_cycles = gtk_button_new_with_label("⏱ SEL");
  gtk_widget_add_css_class(self->btn_sel_cycles, "amiga-status-btn");
  gtk_widget_set_visible(self->btn_sel_cycles, FALSE);
  g_signal_connect(self->btn_sel_cycles, "clicked", G_CALLBACK(on_status_cycles_clicked), self);

  self->btn_diag_count = gtk_button_new_with_label("");
  gtk_widget_add_css_class(self->btn_diag_count, "amiga-status-btn");
  gtk_widget_set_visible(self->btn_diag_count, FALSE);
  g_signal_connect(self->btn_diag_count, "clicked", G_CALLBACK(on_status_diag_clicked), self);

  self->btn_hunk_status = gtk_button_new_with_label("");
  gtk_widget_add_css_class(self->btn_hunk_status, "amiga-status-btn");
  gtk_widget_set_visible(self->btn_hunk_status, FALSE);
  g_signal_connect(self->btn_hunk_status, "clicked", G_CALLBACK(on_status_hunk_clicked), self);

  self->lbl_cursor = gtk_label_new("Ln 1, Col 1");

  gtk_box_append(GTK_BOX(sbar), self->mode_pill);
  gtk_box_append(GTK_BOX(sbar), self->lbl_ws_state);
  gtk_box_append(GTK_BOX(sbar), self->lbl_lsp_status);
  gtk_box_append(GTK_BOX(sbar), self->btn_sel_cycles);
  gtk_box_append(GTK_BOX(sbar), self->btn_diag_count);
  gtk_box_append(GTK_BOX(sbar), self->btn_hunk_status);
  gtk_box_append(GTK_BOX(sbar), self->lbl_cursor);
  return sbar;
}
