#include "amiga_window_private.h"

#include "../common/spawn_detached.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_TYPE(KajoAmigaWindow, kajo_amiga_window, GTK_TYPE_APPLICATION_WINDOW)

/* ─── Helpers & Toast ─── */

static gboolean
on_toast_hide(gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  self->toast_timer_id = 0;
  if (self->toast_bar) gtk_widget_set_visible(self->toast_bar, FALSE);
  return G_SOURCE_REMOVE;
}

void
amiga_window_show_toast(KajoAmigaWindow *self, const char *msg)
{
  if (!self || !self->toast_bar || !msg) return;
  gtk_label_set_text(GTK_LABEL(self->toast_lbl), msg);
  gtk_widget_set_visible(self->toast_bar, TRUE);
  g_clear_handle_id(&self->toast_timer_id, g_source_remove);
  self->toast_timer_id = g_timeout_add(3200, on_toast_hide, self);
}

#define show_toast amiga_window_show_toast

const AmigaTarget *
amiga_window_get_selected_target(KajoAmigaWindow *self)
{
  if (!self->project || !self->project->targets || self->project->targets->len == 0) return NULL;
  guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_target));
  if (idx >= self->project->targets->len) idx = 0;
  return g_ptr_array_index(self->project->targets, idx);
}

const AmigaHwProfile *
amiga_window_get_selected_profile(KajoAmigaWindow *self)
{
  if (!self->project || !self->project->profiles || self->project->profiles->len == 0) return NULL;
  guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_profile));
  if (idx >= self->project->profiles->len) idx = 0;
  return g_ptr_array_index(self->project->profiles, idx);
}

void
amiga_window_sync_compat_badge(KajoAmigaWindow *self)
{
  if (self->session_state == AMIGA_SESSION_ATTACH ||
      self->session_state == AMIGA_SESSION_WELCOME) {
    gtk_widget_set_visible(self->compat_badge, FALSE);
    return;
  }
  const AmigaTarget *t = amiga_window_get_selected_target(self);
  const AmigaHwProfile *p = amiga_window_get_selected_profile(self);
  g_autofree char *warn = amiga_project_check_profile_compat(t, p);
  if (warn) {
    gtk_label_set_text(GTK_LABEL(self->compat_badge), warn);
    gtk_widget_set_visible(self->compat_badge, TRUE);
  } else {
    gtk_widget_set_visible(self->compat_badge, FALSE);
  }
}

void
amiga_window_sync_mode_pill(KajoAmigaWindow *self)
{
  if (!self->mode_pill) return;
  gtk_widget_remove_css_class(self->mode_pill, "insert");
  gtk_widget_remove_css_class(self->mode_pill, "select");
  gtk_widget_remove_css_class(self->mode_pill, "attach");
  gtk_widget_remove_css_class(self->mode_pill, "running");
  gtk_widget_remove_css_class(self->mode_pill, "paused");

  if (self->session_state == AMIGA_SESSION_ATTACH) {
    gtk_label_set_text(GTK_LABEL(self->mode_pill), "ATTACH");
    gtk_widget_add_css_class(self->mode_pill, "attach");
    return;
  }
  if (self->session_state == AMIGA_SESSION_RUNNING) {
    gtk_label_set_text(GTK_LABEL(self->mode_pill), "RUNNING");
    gtk_widget_add_css_class(self->mode_pill, "running");
    return;
  }
  if (self->session_state == AMIGA_SESSION_PAUSED) {
    gtk_label_set_text(GTK_LABEL(self->mode_pill), "PAUSED");
    gtk_widget_add_css_class(self->mode_pill, "paused");
    return;
  }
  IdeState st = self->fsm ? ide_fsm_get_state(self->fsm) : IDE_STATE_NORMAL;
  gtk_label_set_text(GTK_LABEL(self->mode_pill), ide_state_to_string(st));
  if (st == IDE_STATE_INSERT) gtk_widget_add_css_class(self->mode_pill, "insert");
  else if (st == IDE_STATE_SELECT) gtk_widget_add_css_class(self->mode_pill, "select");
}

void
amiga_window_sync_status_bar(KajoAmigaWindow *self)
{
  if (!self->lbl_lsp_status) return;
  bool has_m68k = (self->lsp_m68k != NULL);
  bool has_c = (self->lsp_clangd != NULL);
  g_autofree char *lsp = g_strdup_printf("LSP: %s",
                                         (has_m68k && has_c) ? "m68k+clangd"
                                         : has_m68k          ? "m68k"
                                         : has_c             ? "clangd"
                                                             : "off");
  gtk_label_set_text(GTK_LABEL(self->lbl_lsp_status), lsp);

  int errs = 0, warns = 0;
  for (guint i = 0; self->diags && i < self->diags->len; i++) {
    AmigaBuildDiag *d = g_ptr_array_index(self->diags, i);
    if (d->severity == AMIGA_DIAG_WARNING) warns++;
    else errs++;
  }
  if (errs + warns > 0) {
    g_autofree char *ds = g_strdup_printf("✗ %d err · ⚠ %d warn", errs, warns);
    gtk_button_set_label(GTK_BUTTON(self->btn_diag_count), ds);
    gtk_widget_set_visible(self->btn_diag_count, TRUE);
  } else {
    gtk_widget_set_visible(self->btn_diag_count, FALSE);
  }

  if (self->first_hunk_addr != 0) {
    g_autofree char *hs = g_strdup_printf("HUNK0 $%06X", self->first_hunk_addr);
    gtk_button_set_label(GTK_BUTTON(self->btn_hunk_status), hs);
    gtk_widget_set_visible(self->btn_hunk_status, TRUE);
  } else {
    gtk_widget_set_visible(self->btn_hunk_status, FALSE);
  }
}

static void
update_cursor_and_selection_cycles(KajoAmigaWindow *self)
{
  if (!self->lbl_cursor) return;
  if (!self->active_buf) {
    gtk_label_set_text(GTK_LABEL(self->lbl_cursor), "Ln 1, Col 1");
    gtk_widget_set_visible(self->btn_sel_cycles, FALSE);
    return;
  }
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
  GtkTextIter cur;
  gtk_text_buffer_get_iter_at_mark(tb, &cur, gtk_text_buffer_get_insert(tb));
  g_autofree char *pos = g_strdup_printf("Ln %d, Col %d",
                                         gtk_text_iter_get_line(&cur) + 1,
                                         gtk_text_iter_get_line_offset(&cur) + 1);
  gtk_label_set_text(GTK_LABEL(self->lbl_cursor), pos);

  GtkTextIter s, e;
  if (self->show_sel_cycles && gtk_text_buffer_get_selection_bounds(tb, &s, &e)) {
    int l1 = gtk_text_iter_get_line(&s), l2 = gtk_text_iter_get_line(&e);
    if (l2 > l1) {
      GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
      for (int ln = l1; ln <= l2; ln++) {
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(tb, &ls, ln);
        le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        g_ptr_array_add(lines, gtk_text_buffer_get_text(tb, &ls, &le, FALSE));
      }
      AmigaCycleSum sum = amiga_m68k_sum_cycles((const char *const *)lines->pdata, lines->len);
      g_ptr_array_unref(lines);
      if (sum.instr_count > 0) {
        g_autofree char *cs = (sum.min_cycles == sum.max_cycles)
                                  ? g_strdup_printf("⏱ SEL: %u ln · %u cyc", sum.instr_count, sum.min_cycles)
                                  : g_strdup_printf("⏱ SEL: %u ln · %u-%u cyc",
                                                    sum.instr_count, sum.min_cycles, sum.max_cycles);
        gtk_button_set_label(GTK_BUTTON(self->btn_sel_cycles), cs);
        gtk_widget_set_visible(self->btn_sel_cycles, TRUE);
        return;
      }
    }
  }
  gtk_widget_set_visible(self->btn_sel_cycles, FALSE);
}

/* ─── Perspective State Machine ─── */

void
amiga_window_apply_half_screen_visibility(KajoAmigaWindow *self)
{
  bool half = (self->last_alloc_w > 0 && self->last_alloc_w < 900);
  if (self->session_state == AMIGA_SESSION_WELCOME) half = false;
  if (self->half_strip_box) gtk_widget_set_visible(self->half_strip_box, half);
  if (self->layout_toggles_box) {
    gtk_widget_set_visible(self->layout_toggles_box,
                           self->session_state != AMIGA_SESSION_WELCOME && !half);
  }
  if (self->dd_profile) {
    gtk_widget_set_visible(self->dd_profile,
                           self->session_state != AMIGA_SESSION_WELCOME &&
                           self->session_state != AMIGA_SESSION_ATTACH && !half);
  }
  if (!half) return;

  int p = self->active_half_pane;
  gtk_widget_set_visible(self->sidebar_box, p == 0);
  gtk_widget_set_visible(self->center_col, p == 1 || p == 2);
  if (p == 1) gtk_stack_set_visible_child_name(GTK_STACK(self->center_stack), "source");
  else if (p == 2) gtk_stack_set_visible_child_name(GTK_STACK(self->center_stack), "disasm");
  gtk_widget_set_visible(self->inspector_box, p == 3 && gtk_widget_get_visible(self->btn_tog_right));
  gtk_widget_set_visible(self->drawer_box, p == 4);
}

static void
sync_transport_widgets(KajoAmigaWindow *self, AmigaSessionState st, bool in_ws, bool is_live, bool is_attach)
{
  bool is_running = (st == AMIGA_SESSION_RUNNING);
  gtk_widget_set_visible(self->offline_grp, in_ws && !is_live);
  gtk_widget_set_visible(self->live_grp, in_ws && is_live);
  if (!is_live) return;

  gtk_widget_remove_css_class(self->lbl_state_badge, "running");
  gtk_widget_remove_css_class(self->lbl_state_badge, "paused");
  gtk_widget_remove_css_class(self->lbl_state_badge, "attach");
  if (is_attach) {
    gtk_label_set_text(GTK_LABEL(self->lbl_state_badge), "● ATTACH");
    gtk_widget_add_css_class(self->lbl_state_badge, "attach");
  } else if (is_running) {
    gtk_label_set_text(GTK_LABEL(self->lbl_state_badge), "● RUNNING");
    gtk_widget_add_css_class(self->lbl_state_badge, "running");
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_state_badge), "● PAUSED");
    gtk_widget_add_css_class(self->lbl_state_badge, "paused");
  }

  bool guest_paused = !is_running && (!self->rsp ||
                      amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_RUNNING);
  gtk_button_set_label(GTK_BUTTON(self->btn_resume_pause), guest_paused ? "▶" : "⏸");
  gtk_widget_set_sensitive(self->btn_step_over, guest_paused);
  gtk_widget_set_sensitive(self->btn_step_into, guest_paused);
  gtk_widget_set_sensitive(self->btn_step_out, guest_paused);
  gtk_widget_set_visible(self->btn_restart, !is_attach);
  gtk_widget_set_visible(self->btn_stop, !is_attach);
  gtk_button_set_label(GTK_BUTTON(self->btn_attach_detach), is_attach ? "🔌 DETACH" : "🔌");
  gtk_widget_set_sensitive(self->btn_snapshot, true);
}

void
amiga_window_apply_session_state(KajoAmigaWindow *self, AmigaSessionState st)
{
  self->session_state = st;
  bool in_ws = (st != AMIGA_SESSION_WELCOME);
  bool is_live = (st == AMIGA_SESSION_RUNNING || st == AMIGA_SESSION_PAUSED || st == AMIGA_SESSION_ATTACH);
  bool is_paused_or_att = (st == AMIGA_SESSION_PAUSED || st == AMIGA_SESSION_ATTACH);
  bool is_attach = (st == AMIGA_SESSION_ATTACH);
  bool insp_allowed = (is_paused_or_att || (st == AMIGA_SESSION_RUNNING && self->inspector_frozen_snap));

  gtk_stack_set_visible_child_name(GTK_STACK(self->root_stack), in_ws ? "workspace" : "welcome");
  gtk_widget_set_visible(self->btn_proj_menu, in_ws);
  bool multi_target = (self->project && self->project->targets && self->project->targets->len > 1);
  gtk_widget_set_visible(self->dd_target, in_ws && !is_attach && multi_target);
  gtk_widget_set_visible(self->dd_profile, in_ws && !is_attach);
  gtk_widget_set_visible(self->transport_box, in_ws);
  gtk_widget_set_visible(self->layout_toggles_box, in_ws);
  amiga_window_sync_compat_badge(self);
  sync_transport_widgets(self, st, in_ws, is_live, is_attach);

  amiga_debug_sidebar_set_attach_mode(self->debug_sidebar, is_attach);
  gtk_widget_set_visible(self->btn_sb_debug, is_paused_or_att);
  gtk_widget_set_visible(self->btn_sb_files, !is_attach);
  gtk_widget_set_visible(self->btn_sb_search, !is_paused_or_att);
  if (is_paused_or_att) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_sb_debug), TRUE);
    gtk_stack_set_visible_child_name(GTK_STACK(self->sidebar_stack), "debug");
  } else if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->sidebar_stack)), "debug") == 0) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_sb_files), TRUE);
    gtk_stack_set_visible_child_name(GTK_STACK(self->sidebar_stack), "files");
  }

  gtk_widget_set_visible(self->btn_step_granularity, is_paused_or_att);
  gtk_widget_set_visible(self->btn_toggle_disasm, is_live);

  amiga_window_drawer_sync_tabs(self, st);
  if (st == AMIGA_SESSION_BUILD_ERR || st == AMIGA_SESSION_RUNNING) {
    self->user_drawer_open = true;
    amiga_window_drawer_select(self, "build");
  } else if (is_paused_or_att) {
    self->user_drawer_open = true;
    if (is_attach && g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->drawer_stack)), "build") == 0) {
      amiga_window_drawer_select(self, "mem");
    }
  } else if (st == AMIGA_SESSION_EDIT || st == AMIGA_SESSION_SETUP) {
    self->user_drawer_open = false;
  }

  if (!insp_allowed) self->user_inspector_open = false;
  else if (is_paused_or_att) {
    self->user_inspector_open = true;
    amiga_window_inspector_set_badge(self, "LIVE SNAP", false);
  }
  gtk_widget_set_visible(self->btn_tog_right, insp_allowed);

  bool narrow_side = (self->last_alloc_w >= 900 && self->last_alloc_w < 1200);
  bool auto_sb = (st == AMIGA_SESSION_RUNNING) ? false : self->user_sidebar_open;
  bool sb_vis = in_ws && (is_paused_or_att || (!narrow_side && auto_sb));
  gtk_widget_set_visible(self->sidebar_box, sb_vis);
  gtk_widget_set_visible(self->drawer_box, in_ws && self->user_drawer_open);
  gtk_widget_set_visible(self->inspector_box, in_ws && insp_allowed && self->user_inspector_open);

  int sb_w = is_paused_or_att ? 286 : 230;
  int alloc_w = (self->last_alloc_w > 0) ? self->last_alloc_w : 1280;
  gtk_paned_set_position(GTK_PANED(self->main_paned), sb_w);
  gtk_paned_set_position(GTK_PANED(self->center_paned),
                         MAX(320, alloc_w - (sb_vis ? sb_w : 0) - 434));

  self->updating_layout_toggles = true;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tog_left), sb_vis);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tog_bottom), self->user_drawer_open);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tog_right), insp_allowed && self->user_inspector_open);
  self->updating_layout_toggles = false;

  if (st == AMIGA_SESSION_SETUP) amiga_window_open_setup_tab(self);
  else if (is_attach) amiga_window_switch_center_page(self, "disasm");
  amiga_window_apply_half_screen_visibility(self);
  amiga_window_sync_mode_pill(self);
  amiga_window_sync_status_bar(self);
  amiga_window_debug_sync_ui(self);
}

/* ─── Multi-File Center Tabs ─── */

static void
select_buffer(KajoAmigaWindow *self, IdeBuffer *buf, guint line_1based)
{
  self->active_buf = buf;
  self->editor_host.buffer = buf;
  if (buf) {
    GtkSourceBuffer *sbuf = ide_buffer_get_source_buffer(buf);
    gtk_text_view_set_buffer(GTK_TEXT_VIEW(self->source_view), GTK_TEXT_BUFFER(sbuf));
    kajo_ide_gutter_renderer_set_buffer(self->gutter, buf);
    amiga_window_debug_sync_buffer_breakpoints(self, buf);
    amiga_window_lsp_notify_open(self, buf);
    if (line_1based > 0) {
      GtkTextIter it;
      gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(sbuf), &it, (int)(line_1based - 1u));
      gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(sbuf), &it);
      gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view), &it, 0.2, FALSE, 0, 0.5);
    }
    g_autofree char *title = g_path_get_basename(ide_buffer_get_file_path(buf));
    gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), title);
  }
  amiga_window_switch_center_page(self, "source");
  amiga_window_refresh_source_gutter_and_outline(self);
  update_cursor_and_selection_cycles(self);
}

static void on_file_tab_clicked(GtkButton *b, gpointer ud) {
  IdeBuffer *buf = g_object_get_data(G_OBJECT(b), "buf-ptr");
  if (buf) select_buffer(KAJO_AMIGA_WINDOW(ud), buf, 0);
}

static void
on_file_tab_close_clicked(GtkButton *b, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  IdeBuffer *buf = g_object_get_data(G_OBJECT(b), "buf-ptr");
  if (!buf || !self->open_bufs) return;
  if (ide_buffer_get_modified(buf)) ide_buffer_save(buf, NULL);
  guint idx = 0;
  if (g_ptr_array_find(self->open_bufs, buf, &idx)) {
    bool was_active = (self->active_buf == buf);
    g_ptr_array_remove_index(self->open_bufs, idx);
    if (was_active) {
      if (self->open_bufs->len > 0) select_buffer(self, g_ptr_array_index(self->open_bufs, 0), 0);
      else {
        self->active_buf = NULL; self->editor_host.buffer = NULL;
        gtk_text_view_set_buffer(GTK_TEXT_VIEW(self->source_view), NULL);
        kajo_ide_gutter_renderer_set_buffer(self->gutter, NULL);
        amiga_window_refresh_source_gutter_and_outline(self);
      }
    }
    amiga_window_rebuild_center_tabs(self);
  }
}

static void on_setup_tab_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_open_setup_tab(KAJO_AMIGA_WINDOW(ud)); }
static void on_setup_tab_close_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud) { amiga_window_close_setup_tab(KAJO_AMIGA_WINDOW(ud)); }

void
amiga_window_rebuild_center_tabs(KajoAmigaWindow *self)
{
  if (!self->center_tabs_box) return;
  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(self->center_tabs_box)) != NULL) {
    gtk_box_remove(GTK_BOX(self->center_tabs_box), ch);
  }
  const char *cur_page = gtk_stack_get_visible_child_name(GTK_STACK(self->center_stack));
  for (guint i = 0; self->open_bufs && i < self->open_bufs->len; i++) {
    IdeBuffer *buf = g_ptr_array_index(self->open_bufs, i);
    const char *path = ide_buffer_get_file_path(buf);
    g_autofree char *name = path ? g_path_get_basename(path) : g_strdup("untitled");
    g_autofree char *lbl = ide_buffer_get_modified(buf) ? g_strdup_printf("%s ●", name) : g_strdup(name);
    GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(pill, "amiga-file-tab");
    if (buf == self->active_buf && g_strcmp0(cur_page, "source") == 0) gtk_widget_add_css_class(pill, "active");
    GtkWidget *bt = gtk_button_new_with_label(lbl);
    gtk_widget_add_css_class(bt, "amiga-file-tab-btn");
    g_object_set_data(G_OBJECT(bt), "buf-ptr", buf);
    g_signal_connect(bt, "clicked", G_CALLBACK(on_file_tab_clicked), self);
    GtkWidget *bx = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(bx, "amiga-mini-btn");
    g_object_set_data(G_OBJECT(bx), "buf-ptr", buf);
    g_signal_connect(bx, "clicked", G_CALLBACK(on_file_tab_close_clicked), self);
    gtk_box_append(GTK_BOX(pill), bt); gtk_box_append(GTK_BOX(pill), bx);
    gtk_box_append(GTK_BOX(self->center_tabs_box), pill);
  }
  if (self->setup_tab_open) {
    GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(sp, "amiga-file-tab");
    if (g_strcmp0(cur_page, "setup") == 0) gtk_widget_add_css_class(sp, "active");
    GtkWidget *bt = gtk_button_new_with_label("⚙ kajo-amiga.toml");
    gtk_widget_add_css_class(bt, "amiga-file-tab-btn");
    g_signal_connect(bt, "clicked", G_CALLBACK(on_setup_tab_clicked), self);
    GtkWidget *bx = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(bx, "amiga-mini-btn");
    g_signal_connect(bx, "clicked", G_CALLBACK(on_setup_tab_close_clicked), self);
    gtk_box_append(GTK_BOX(sp), bt); gtk_box_append(GTK_BOX(sp), bx);
    gtk_box_append(GTK_BOX(self->center_tabs_box), sp);
  }
}

void
amiga_window_switch_center_page(KajoAmigaWindow *self, const char *page)
{
  if (!self->center_stack || !page) return;
  gtk_stack_set_visible_child_name(GTK_STACK(self->center_stack), page);
  self->disasm_tab_open = (g_strcmp0(page, "disasm") == 0);
  if (self->btn_toggle_disasm) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_toggle_disasm), self->disasm_tab_open);
  amiga_window_rebuild_center_tabs(self);
}

void amiga_window_open_setup_tab(KajoAmigaWindow *self) {
  self->setup_tab_open = true;
  if (self->project) amiga_project_setup_view_bind(self->setup_view, self->project, self->toolchain);
  amiga_window_switch_center_page(self, "setup");
  if (self->session_state == AMIGA_SESSION_EDIT) amiga_window_apply_session_state(self, AMIGA_SESSION_SETUP);
}

void amiga_window_close_setup_tab(KajoAmigaWindow *self) {
  self->setup_tab_open = false;
  amiga_window_switch_center_page(self, "source");
  if (self->session_state == AMIGA_SESSION_SETUP) amiga_window_apply_session_state(self, AMIGA_SESSION_EDIT);
}

void
amiga_window_open_file_at_line(KajoAmigaWindow *self, const char *abs_path, guint line_1based)
{
  if (!abs_path || !g_file_test(abs_path, G_FILE_TEST_IS_REGULAR)) return;
  for (guint i = 0; self->open_bufs && i < self->open_bufs->len; i++) {
    IdeBuffer *existing = g_ptr_array_index(self->open_bufs, i);
    if (g_strcmp0(ide_buffer_get_file_path(existing), abs_path) == 0) {
      select_buffer(self, existing, line_1based);
      return;
    }
  }
  IdeBuffer *buf = ide_buffer_new(abs_path);
  g_autoptr(GError) err = NULL;
  if (!ide_buffer_load_from_disk(buf, &err)) {
    ide_buffer_free(buf);
    show_toast(self, err ? err->message : "Open failed");
    return;
  }
  ide_buffer_set_style_scheme(buf, self->inherited_scheme ? self->inherited_scheme : "kajo-dark");
  g_ptr_array_add(self->open_bufs, buf);
  select_buffer(self, buf, line_1based);
}

/* ─── Build, Save & Project Actions ─── */

void
amiga_window_save_active_buffer(KajoAmigaWindow *self)
{
  if (!self->active_buf) return;
  g_autoptr(GError) err = NULL;
  if (ide_buffer_save(self->active_buf, &err)) {
    amiga_window_refresh_source_gutter_and_outline(self);
    amiga_window_rebuild_center_tabs(self);
    amiga_window_lsp_notify_save(self, self->active_buf);
    show_toast(self, "✓ File saved");
  } else {
    show_toast(self, err ? err->message : "Save failed");
  }
}

static void
on_build_finished(GObject *src G_GNUC_UNUSED, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GError) err = NULL;
  AmigaBuildResult *br = amiga_build_run_finish(res, &err);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak); g_free(weak);
  if (!self) { amiga_build_result_free(br); return; }

  self->building = false;
  bool launch_pending = self->launch_after_build;
  self->launch_after_build = false;
  if (self->btn_build) gtk_widget_set_sensitive(self->btn_build, TRUE);

  GtkTextBuffer *cb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->console_view));
  if (err) {
    gtk_text_buffer_set_text(cb, err->message, -1);
    amiga_window_apply_session_state(self, AMIGA_SESSION_BUILD_ERR);
    show_toast(self, err->message);
    return;
  }
  gtk_text_buffer_set_text(cb, (br && br->log_text) ? br->log_text : "", -1);
  amiga_window_drawer_set_diags(self, br ? br->diagnostics : NULL);
  bool ok = (br && br->success);
  amiga_build_result_free(br);
  if (ok) {
    amiga_window_debug_reload_symbols(self);
    if (launch_pending) amiga_window_debug_launch_after_build(self);
    else { amiga_window_apply_session_state(self, AMIGA_SESSION_EDIT); show_toast(self, "✓ Build succeeded"); }
  } else {
    amiga_window_apply_session_state(self, AMIGA_SESSION_BUILD_ERR);
    show_toast(self, "✗ Build failed (see diagnostics)");
  }
}

void
amiga_window_trigger_build(KajoAmigaWindow *self)
{
  if (!self->project || self->building) return;
  const AmigaTarget *t = amiga_window_get_selected_target(self);
  if (!t) { self->launch_after_build = false; show_toast(self, "No target defined in project"); return; }
  amiga_window_save_active_buffer(self);
  self->building = true;
  if (self->btn_build) gtk_widget_set_sensitive(self->btn_build, FALSE);
  self->user_drawer_open = true;
  gtk_widget_set_visible(self->drawer_box, TRUE);
  amiga_window_drawer_select(self, "build");

  g_cancellable_cancel(self->build_cancel);
  g_clear_object(&self->build_cancel);
  self->build_cancel = g_cancellable_new();
  g_autofree char *staging = g_build_filename(
      g_get_user_cache_dir(), "kajo", "amiga", self->project->name ? self->project->name : "default", NULL);
  GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
  amiga_build_run_async(self->project, t, self->toolchain, staging, self->build_cancel, on_build_finished, weak);
}

void amiga_window_jump_mem(KajoAmigaWindow *self, uint32_t addr) {
  if (!self) return;
  self->user_drawer_open = true;
  gtk_widget_set_visible(self->drawer_box, TRUE);
  if (self->btn_tog_bottom) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tog_bottom), TRUE);
  amiga_window_drawer_select(self, "mem");
  amiga_mem_view_jump_to(self->mem_view, addr);
}

void amiga_window_jump_disasm(KajoAmigaWindow *self, uint32_t addr) {
  if (!self) return;
  amiga_window_switch_center_page(self, "disasm");
  amiga_disasm_view_go_to(self->disasm_view, addr, true);
}

static void on_wizard_created(const char *dir, gpointer ud) { kajo_amiga_window_open_project_dir(KAJO_AMIGA_WINDOW(ud), dir); }
void amiga_window_act_new_project(KajoAmigaWindow *self) {
  amiga_project_wizard_present(GTK_WINDOW(self), self->toolchain, on_wizard_created, self);
}
static void on_act_new_project(gpointer ud) { amiga_window_act_new_project(KAJO_AMIGA_WINDOW(ud)); }
static void on_open_folder_done(GObject *src, GAsyncResult *res, gpointer ud) {
  GWeakRef *weak = ud;
  g_autoptr(GFile) f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak); g_free(weak);
  if (self && f) { g_autofree char *p = g_file_get_path(f); if (p) kajo_amiga_window_open_project_dir(self, p); }
}
void amiga_window_act_open_project(KajoAmigaWindow *self) {
  g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Open Amiga Project Folder");
  GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(self), NULL, on_open_folder_done, weak);
}
static void on_act_open_project(gpointer ud) { amiga_window_act_open_project(KAJO_AMIGA_WINDOW(ud)); }
void amiga_window_act_attach(KajoAmigaWindow *self) {
  kajo_amiga_window_open_attach(self, self->cfg_gdb_host, self->cfg_gdb_port);
}
static void on_act_attach(gpointer ud) { amiga_window_act_attach(KAJO_AMIGA_WINDOW(ud)); }
void amiga_window_act_settings(KajoAmigaWindow *self G_GNUC_UNUSED) {
  const char *argv[] = { "kajo-settings", "--page", "amiga", NULL };
  kajo_spawn_detached_argv(NULL, argv, NULL, NULL);
}
static void on_act_settings(gpointer ud) { amiga_window_act_settings(KAJO_AMIGA_WINDOW(ud)); }

static void on_proj_saved(AmigaProject *p, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (p && p->manifest_path && g_file_test(p->manifest_path, G_FILE_TEST_IS_REGULAR)) {
    AmigaProject *reloaded = amiga_project_load_dir(p->root_dir, NULL);
    if (reloaded) { g_clear_pointer(&self->project, amiga_project_free); self->project = reloaded; p = reloaded; }
  }
  amiga_window_populate_project_dropdowns(self);
  amiga_project_setup_view_bind(self->setup_view, p, self->toolchain);
  if (p->hrm_dir && p->hrm_dir[0]) {
    g_autofree char *abs_hrm = g_path_is_absolute(p->hrm_dir) ? g_strdup(p->hrm_dir) : g_build_filename(p->root_dir, p->hrm_dir, NULL);
    amiga_hrm_view_set_hrm_dir(self->hrm_view, abs_hrm);
  }
  amiga_project_write_compile_commands(p, self->toolchain, NULL);
  amiga_window_debug_reload_symbols(self);
  amiga_window_lsp_sync_project(self);
  amiga_window_sync_compat_badge(self);
  show_toast(self, "✓ Project saved + compile_commands.json updated");
}

/* ─── Keys, Allocation & Construction ─── */

static gboolean
on_source_key_pressed(GtkEventControllerKey *c G_GNUC_UNUSED, guint keyval,
                      guint kc G_GNUC_UNUSED, GdkModifierType state, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  if (!self->fsm) return FALSE;
  if (ide_fsm_get_state(self->fsm) == IDE_STATE_NORMAL &&
      keyval == GDK_KEY_K && (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) == 0 && self->active_buf) {
    GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
    GtkTextIter ws, we;
    gtk_text_buffer_get_iter_at_mark(tb, &ws, gtk_text_buffer_get_insert(tb));
    we = ws;
    while (!gtk_text_iter_starts_line(&ws)) {
      GtkTextIter prev = ws; gtk_text_iter_backward_char(&prev);
      gunichar ch = gtk_text_iter_get_char(&prev);
      if (!g_unichar_isalnum(ch) && ch != '_' && ch != '$' && ch != '-') break;
      ws = prev;
    }
    while (!gtk_text_iter_ends_line(&we)) {
      gunichar ch = gtk_text_iter_get_char(&we);
      if (!g_unichar_isalnum(ch) && ch != '_') break;
      gtk_text_iter_forward_char(&we);
    }
    g_autofree char *tok = gtk_text_buffer_get_text(tb, &ws, &we, FALSE);
    amiga_window_show_hrm_quickdoc(self, (tok && tok[0]) ? tok : "");
    return TRUE;
  }
  g_autofree char *ks = ide_editor_translate_gdk_key(keyval, state);
  if (!ks) return FALSE;
  IdeAction act = ide_fsm_handle_key(self->fsm, ks);
  amiga_window_sync_mode_pill(self);
  const char *toast = NULL;
  IdeEditorOpOutcome res = ide_editor_ops_execute(&self->editor_host, &act, &toast);
  if (res != IDE_EDITOR_OP_UNHANDLED) {
    if (toast) show_toast(self, toast);
    if (self->active_buf && (res == IDE_EDITOR_OP_SCROLL || res == IDE_EDITOR_OP_POST_EDIT)) {
      GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
      gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(self->source_view),
                                         gtk_text_buffer_get_insert(tb));
    }
    if (res == IDE_EDITOR_OP_POST_EDIT) {
      amiga_window_refresh_source_gutter_and_outline(self);
      amiga_window_rebuild_center_tabs(self);
      gtk_widget_queue_draw(self->source_view);
    }
    update_cursor_and_selection_cycles(self);
    return TRUE;
  }
  if (act.type == IDE_ACTION_SAVE || act.type == IDE_ACTION_SAVE_ALL) { amiga_window_save_active_buffer(self); return TRUE; }
  if (act.type == IDE_ACTION_TASK_BUILD || act.type == IDE_ACTION_TASK_RUN) { amiga_window_trigger_build(self); return TRUE; }
  if (act.type == IDE_ACTION_TOGGLE_FILE_TREE) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_tog_left),
                                 !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_tog_left)));
    return TRUE;
  }
  return FALSE;
}

static gboolean
on_window_key_pressed(GtkEventControllerKey *c G_GNUC_UNUSED, guint keyval,
                      guint kc G_GNUC_UNUSED, GdkModifierType state, gpointer ud)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  bool ctrl = (state & GDK_CONTROL_MASK) != 0, shift = (state & GDK_SHIFT_MASK) != 0, alt = (state & GDK_ALT_MASK) != 0;
  if (ctrl && shift && (keyval == GDK_KEY_N || keyval == GDK_KEY_n)) { amiga_window_act_new_project(self); return TRUE; }
  if (ctrl && !shift && (keyval == GDK_KEY_o || keyval == GDK_KEY_O)) { amiga_window_act_open_project(self); return TRUE; }
  if (ctrl && shift && (keyval == GDK_KEY_P || keyval == GDK_KEY_p)) { amiga_window_open_setup_tab(self); return TRUE; }
  if (ctrl && shift && (keyval == GDK_KEY_A || keyval == GDK_KEY_a)) { amiga_window_act_attach(self); return TRUE; }
  if (ctrl && shift && (keyval == GDK_KEY_F || keyval == GDK_KEY_f)) {
    gtk_widget_set_visible(self->sidebar_box, TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_sb_search), TRUE);
    gtk_widget_grab_focus(self->search_entry); return TRUE;
  }
  if (ctrl && keyval == GDK_KEY_comma) { amiga_window_act_settings(self); return TRUE; }
  if (ctrl && (keyval == GDK_KEY_b || keyval == GDK_KEY_B)) { amiga_window_trigger_build(self); return TRUE; }
  if (ctrl && (keyval == GDK_KEY_g || keyval == GDK_KEY_G)) {
    amiga_window_switch_center_page(self, "disasm");
    amiga_disasm_view_focus_goto(self->disasm_view); return TRUE;
  }
  if (alt && (keyval == GDK_KEY_d || keyval == GDK_KEY_D)) {
    amiga_window_switch_center_page(self, self->disasm_tab_open ? "source" : "disasm"); return TRUE;
  }
  if (alt && keyval == GDK_KEY_Left) { amiga_disasm_view_nav_back(self->disasm_view); return TRUE; }
  if (alt && keyval == GDK_KEY_Right) { amiga_disasm_view_nav_fwd(self->disasm_view); return TRUE; }
  if (keyval == GDK_KEY_F5) {
    if (ctrl && shift) amiga_window_debug_restart(self);
    else if (shift) amiga_window_debug_stop(self);
    else amiga_window_debug_start_or_continue(self);
    return TRUE;
  }
  if (keyval == GDK_KEY_F6) { amiga_window_debug_pause(self); return TRUE; }
  if (keyval == GDK_KEY_F8) { amiga_window_drawer_jump_diag(self, shift ? -1 : +1); return TRUE; }
  if (keyval == GDK_KEY_F9 && self->active_buf) {
    GtkTextBuffer *tb = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(self->active_buf));
    GtkTextIter cur; gtk_text_buffer_get_iter_at_mark(tb, &cur, gtk_text_buffer_get_insert(tb));
    amiga_window_debug_toggle_source_bp(self, self->active_buf, (guint)gtk_text_iter_get_line(&cur) + 1u);
    return TRUE;
  }
  if (keyval == GDK_KEY_F10) {
    if (ctrl) amiga_window_debug_run_to_cursor_source(self); else amiga_window_debug_step_over(self);
    return TRUE;
  }
  if (keyval == GDK_KEY_F11) { amiga_window_debug_step_into(self); return TRUE; }
  if (keyval == GDK_KEY_F12) { amiga_window_debug_step_out(self); return TRUE; }
  return FALSE;
}

static void on_reflow_idle(gpointer ud) {
  GWeakRef *weak = ud;
  g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak); g_free(weak);
  if (!self) return;
  self->reflow_idle_queued = false;
  self->narrow_mode = (self->last_alloc_w > 0 && self->last_alloc_w < 900);
  amiga_window_apply_session_state(self, self->session_state);
}

static void
kajo_amiga_window_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
  GTK_WIDGET_CLASS(kajo_amiga_window_parent_class)->size_allocate(widget, width, height, baseline);
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(widget);
  int old_tier = (self->last_alloc_w < 900) ? 0 : (self->last_alloc_w < 1200) ? 1 : 2;
  int new_tier = (width < 900) ? 0 : (width < 1200) ? 1 : 2;
  self->last_alloc_w = width;
  if (old_tier != new_tier && !self->reflow_idle_queued) {
    self->reflow_idle_queued = true;
    GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
    g_idle_add_once(on_reflow_idle, weak);
  }
}

static void on_step_granularity_clicked(GtkButton *b, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  self->step_insn_mode = !self->step_insn_mode;
  gtk_button_set_label(b, self->step_insn_mode ? "STEP: INSN" : "STEP: LINE");
}
static void on_toggle_disasm_clicked(GtkToggleButton *tb, gpointer ud) {
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(ud);
  bool on = gtk_toggle_button_get_active(tb);
  gtk_stack_set_visible_child_name(GTK_STACK(self->center_stack), on ? "disasm" : "source");
  self->disasm_tab_open = on; amiga_window_rebuild_center_tabs(self);
}
static void on_gutter_bp_toggled(IdeBuffer *buf, guint line, gboolean en G_GNUC_UNUSED, gpointer ud) {
  amiga_window_debug_toggle_source_bp(KAJO_AMIGA_WINDOW(ud), buf, line);
}

static GtkWidget *
build_center_col(KajoAmigaWindow *self)
{
  self->center_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(self->center_col, TRUE); gtk_widget_set_vexpand(self->center_col, TRUE);

  self->center_tabs_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(self->center_tabs_bar, "amiga-tab-strip");
  GtkWidget *tab_scr = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab_scr), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_widget_set_hexpand(tab_scr, TRUE);
  self->center_tabs_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tab_scr), self->center_tabs_box);

  self->btn_step_granularity = gtk_button_new_with_label("STEP: LINE");
  gtk_widget_add_css_class(self->btn_step_granularity, "amiga-mini-btn");
  g_signal_connect(self->btn_step_granularity, "clicked", G_CALLBACK(on_step_granularity_clicked), self);
  self->btn_toggle_disasm = gtk_toggle_button_new_with_label("⚙ DISASM");
  gtk_widget_add_css_class(self->btn_toggle_disasm, "amiga-seg-btn");
  g_signal_connect(self->btn_toggle_disasm, "toggled", G_CALLBACK(on_toggle_disasm_clicked), self);

  gtk_box_append(GTK_BOX(self->center_tabs_bar), tab_scr);
  gtk_box_append(GTK_BOX(self->center_tabs_bar), self->btn_step_granularity);
  gtk_box_append(GTK_BOX(self->center_tabs_bar), self->btn_toggle_disasm);
  gtk_box_append(GTK_BOX(self->center_col), self->center_tabs_bar);

  self->center_stack = gtk_stack_new();
  gtk_stack_set_vhomogeneous(GTK_STACK(self->center_stack), FALSE);
  gtk_stack_set_hhomogeneous(GTK_STACK(self->center_stack), FALSE);
  gtk_widget_set_hexpand(self->center_stack, TRUE); gtk_widget_set_vexpand(self->center_stack, TRUE);

  GtkWidget *src_scroll = gtk_scrolled_window_new();
  self->source_view = gtk_source_view_new();
  gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(self->source_view), 2);
  gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(self->source_view), TRUE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->source_view), TRUE);
  gtk_widget_set_has_tooltip(self->source_view, TRUE);
  g_signal_connect(self->source_view, "query-tooltip", G_CALLBACK(amiga_window_source_query_tooltip), self);

  GtkSourceGutter *lg = gtk_source_view_get_gutter(GTK_SOURCE_VIEW(self->source_view), GTK_TEXT_WINDOW_LEFT);
  self->gutter = KAJO_IDE_GUTTER_RENDERER(kajo_ide_gutter_renderer_new(NULL));
  kajo_ide_gutter_renderer_set_breakpoint_callback(self->gutter, on_gutter_bp_toggled, self);
  kajo_ide_gutter_renderer_set_annotation_callback(self->gutter, amiga_window_gutter_cycle_annotate, 76, self);
  gtk_source_gutter_insert(lg, GTK_SOURCE_GUTTER_RENDERER(self->gutter), 0);
  GtkEventController *sk = gtk_event_controller_key_new();
  g_signal_connect(sk, "key-pressed", G_CALLBACK(on_source_key_pressed), self);
  gtk_widget_add_controller(self->source_view, sk);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(src_scroll), self->source_view);
  gtk_stack_add_named(GTK_STACK(self->center_stack), src_scroll, "source");

  self->disasm_view = amiga_disasm_view_new();
  AmigaDisasmCallbacks dcbs = {0};
  amiga_window_debug_fill_disasm_callbacks(&dcbs);
  amiga_disasm_view_set_callbacks(self->disasm_view, &dcbs, self);
  gtk_stack_add_named(GTK_STACK(self->center_stack), GTK_WIDGET(self->disasm_view), "disasm");

  self->setup_view = amiga_project_setup_view_new(on_proj_saved, self);
  gtk_stack_add_named(GTK_STACK(self->center_stack), GTK_WIDGET(self->setup_view), "setup");
  gtk_box_append(GTK_BOX(self->center_col), self->center_stack);
  return self->center_col;
}

static void
build_ui(KajoAmigaWindow *self)
{
  gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(self), 1280, 720);
  gtk_widget_add_css_class(GTK_WIDGET(self), "shell-settings-window");
  gtk_widget_add_css_class(GTK_WIDGET(self), "shell-amiga-window");

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(outer), amiga_window_chrome_build_header(self));

  self->root_stack = gtk_stack_new();
  gtk_widget_set_hexpand(self->root_stack, TRUE); gtk_widget_set_vexpand(self->root_stack, TRUE);

  self->welcome_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(self->welcome_slot),
                 amiga_welcome_view_new(self->toolchain, on_act_new_project, on_act_open_project,
                                        (AmigaDirSelectedCb)kajo_amiga_window_open_project_dir,
                                        on_act_attach, on_act_settings, self));
  gtk_stack_add_named(GTK_STACK(self->root_stack), self->welcome_slot, "welcome");

  GtkWidget *ws_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(ws_vbox), amiga_window_chrome_build_half_strip(self));

  self->vert_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(self->vert_paned, "ide-paned");
  gtk_widget_set_hexpand(self->vert_paned, TRUE); gtk_widget_set_vexpand(self->vert_paned, TRUE);

  self->main_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->main_paned, "ide-paned");
  gtk_paned_set_start_child(GTK_PANED(self->main_paned), amiga_window_chrome_build_sidebar(self));
  gtk_paned_set_resize_start_child(GTK_PANED(self->main_paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(self->main_paned), FALSE);

  self->center_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->center_paned, "ide-paned");
  gtk_paned_set_start_child(GTK_PANED(self->center_paned), build_center_col(self));
  gtk_paned_set_resize_start_child(GTK_PANED(self->center_paned), TRUE);
  gtk_paned_set_end_child(GTK_PANED(self->center_paned), amiga_window_inspector_build(self));
  gtk_paned_set_resize_end_child(GTK_PANED(self->center_paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(self->center_paned), FALSE);
  gtk_paned_set_end_child(GTK_PANED(self->main_paned), self->center_paned);
  gtk_paned_set_resize_end_child(GTK_PANED(self->main_paned), TRUE);

  gtk_paned_set_start_child(GTK_PANED(self->vert_paned), self->main_paned);
  gtk_paned_set_resize_start_child(GTK_PANED(self->vert_paned), TRUE);
  gtk_paned_set_end_child(GTK_PANED(self->vert_paned), amiga_window_drawer_build(self));
  gtk_paned_set_resize_end_child(GTK_PANED(self->vert_paned), FALSE);

  GtkWidget *ws_scr = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ws_scr),
                                 GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(ws_scr), FALSE);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(ws_scr), FALSE);
  gtk_widget_set_hexpand(ws_scr, TRUE); gtk_widget_set_vexpand(ws_scr, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ws_scr), self->vert_paned);
  gtk_box_append(GTK_BOX(ws_vbox), ws_scr);
  gtk_stack_add_named(GTK_STACK(self->root_stack), ws_vbox, "workspace");
  gtk_box_append(GTK_BOX(outer), self->root_stack);

  self->toast_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->toast_bar, "amiga-toast-banner");
  gtk_widget_set_visible(self->toast_bar, FALSE);
  self->toast_lbl = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->toast_lbl), 0.0f); gtk_widget_set_hexpand(self->toast_lbl, TRUE);
  gtk_box_append(GTK_BOX(self->toast_bar), self->toast_lbl);
  gtk_box_append(GTK_BOX(outer), self->toast_bar);
  gtk_box_append(GTK_BOX(outer), amiga_window_chrome_build_status_bar(self));
  gtk_window_set_child(GTK_WINDOW(self), outer);

  GtkEventController *wk = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(wk, GTK_PHASE_CAPTURE);
  g_signal_connect(wk, "key-pressed", G_CALLBACK(on_window_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), wk);
  amiga_window_apply_session_state(self, AMIGA_SESSION_WELCOME);
}

void
amiga_window_populate_project_dropdowns(KajoAmigaWindow *self)
{
  if (!self->project) return;
  g_autoptr(GPtrArray) tnames = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < self->project->targets->len; i++) {
    AmigaTarget *t = g_ptr_array_index(self->project->targets, i);
    g_ptr_array_add(tnames, g_strdup(t->name));
  }
  if (tnames->len == 0) g_ptr_array_add(tnames, g_strdup("default"));
  g_ptr_array_add(tnames, NULL);
  gtk_drop_down_set_model(GTK_DROP_DOWN(self->dd_target),
                          G_LIST_MODEL(gtk_string_list_new((const char *const *)tnames->pdata)));
  bool in_ws = (self->session_state != AMIGA_SESSION_WELCOME);
  bool is_att = (self->session_state == AMIGA_SESSION_ATTACH);
  gtk_widget_set_visible(self->dd_target,
                         in_ws && !is_att && self->project->targets->len > 1);

  g_autoptr(GPtrArray) pnames = g_ptr_array_new_with_free_func(g_free);
  guint def_pidx = 0;
  for (guint i = 0; i < self->project->profiles->len; i++) {
    AmigaHwProfile *p = g_ptr_array_index(self->project->profiles, i);
    g_ptr_array_add(pnames, amiga_hw_profile_display_label(p));
    if (g_strcmp0(p->id, self->project->default_profile) == 0) def_pidx = i;
  }
  if (pnames->len == 0) g_ptr_array_add(pnames, g_strdup("A500 OCS"));
  g_ptr_array_add(pnames, NULL);
  gtk_drop_down_set_model(GTK_DROP_DOWN(self->dd_profile),
                          G_LIST_MODEL(gtk_string_list_new((const char *const *)pnames->pdata)));
  gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_profile), def_pidx);
  amiga_window_sync_compat_badge(self);
}

/* ─── GObject Lifecycle & Config ─── */

static void
kajo_amiga_window_dispose(GObject *obj)
{
  KajoAmigaWindow *self = KAJO_AMIGA_WINDOW(obj);
  if (self->cfg_sub_id) { kajo_config_store_unsubscribe(NULL, self->cfg_sub_id); self->cfg_sub_id = 0; }
  g_clear_pointer(&self->inherited_scheme, g_free);
  g_clear_handle_id(&self->toast_timer_id, g_source_remove);
  if (self->build_cancel) { g_cancellable_cancel(self->build_cancel); g_clear_object(&self->build_cancel); }
  if (self->search_cancel) { g_cancellable_cancel(self->search_cancel); g_clear_object(&self->search_cancel); }
  if (self->rsp && amiga_rsp_client_get_state(self->rsp) != AMIGA_RSP_DISCONNECTED) {
    amiga_rsp_client_set_stop_callback(self->rsp, NULL, NULL);
  }
  g_clear_object(&self->rsp);
  if (self->runner) {
    amiga_runner_set_callbacks(self->runner, NULL, NULL, NULL);
    if (amiga_runner_owns_process(self->runner)) amiga_runner_stop(self->runner);
    g_clear_object(&self->runner);
  }
  g_clear_pointer(&self->last_chip_ram, g_bytes_unref);
  self->active_buf = NULL;
  g_clear_pointer(&self->open_bufs, g_ptr_array_unref);
  g_clear_pointer(&self->file_tree, ide_tree_free);
  g_clear_pointer(&self->fsm, ide_fsm_free);
  g_clear_pointer(&self->yank_reg, g_free);
  g_clear_pointer(&self->symbols, amiga_symbols_free);
  g_clear_pointer(&self->project, amiga_project_free);
  g_clear_pointer(&self->toolchain, amiga_toolchain_status_free);
  g_clear_pointer(&self->diags, g_ptr_array_unref);
  g_clear_pointer(&self->bp_addrs, g_hash_table_unref);
  g_free(self->attach_host); self->attach_host = NULL;
  amiga_window_debug_dispose(self);
  G_OBJECT_CLASS(kajo_amiga_window_parent_class)->dispose(obj);
}

static void
kajo_amiga_window_class_init(KajoAmigaWindowClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = kajo_amiga_window_dispose;
  GTK_WIDGET_CLASS(klass)->size_allocate = kajo_amiga_window_size_allocate;
}

static void
apply_global_config(KajoAmigaWindow *self)
{
  g_autoptr(KajoAmigaCfgRef) acfg = kajo_config_store_acquire_amiga(NULL);
  if (!acfg) return;

  g_free(self->cfg_amiberry_bin); self->cfg_amiberry_bin = g_strdup(acfg->amiberry_bin);
  g_free(self->cfg_kickstart_a500); self->cfg_kickstart_a500 = g_strdup(acfg->kickstart_a500);
  g_free(self->cfg_kickstart_a1200); self->cfg_kickstart_a1200 = g_strdup(acfg->kickstart_a1200);
  g_free(self->cfg_gdb_host);
  self->cfg_gdb_host = g_strdup((acfg->gdb_host && acfg->gdb_host[0]) ? acfg->gdb_host : "127.0.0.1");
  self->cfg_gdb_port = acfg->gdb_port ? (uint16_t)acfg->gdb_port : 2345;
  g_free(self->cfg_c_include_dirs); self->cfg_c_include_dirs = g_strdup(acfg->c_include_dirs);
  g_free(self->cfg_asm_include_dirs); self->cfg_asm_include_dirs = g_strdup(acfg->asm_include_dirs);
  self->cfg_entry_break_mode = acfg->entry_break_mode;
  self->cfg_capture_guest_output = acfg->capture_guest_output;
  self->cfg_enable_trace_history = acfg->enable_trace_history;
  self->cfg_default_exc_mask = (uint8_t)acfg->default_exception_mask;
  self->cfg_trace_blitter_writes = acfg->trace_blitter_writes;
  self->cfg_hex_grouping = (acfg->default_hex_grouping == KAJO_AMIGA_HEX_8B) ? 1
                         : (acfg->default_hex_grouping == KAJO_AMIGA_HEX_32B) ? 4 : 2;
  self->cfg_bitplane_grid_16px = acfg->bitplane_grid_16px;
  self->cfg_enable_m68k_lsp = acfg->enable_m68k_lsp;
  self->cfg_enable_clangd = acfg->enable_clangd;
  self->show_cycle_gutter = acfg->show_cycle_gutter;
  self->show_sel_cycles = acfg->show_selection_cycle_sum;
  self->enable_hover_decoders = acfg->enable_hover_decoders;

  if (self->mem_view) amiga_mem_view_set_grouping(self->mem_view, self->cfg_hex_grouping);
  if (self->bpl_view) amiga_bitplane_view_set_grid(self->bpl_view, self->cfg_bitplane_grid_16px);
  amiga_window_debug_apply_exc_mask(self, self->cfg_default_exc_mask);

  g_auto(GStrv) extra = (acfg->extra_bin_paths && acfg->extra_bin_paths[0])
                            ? g_strsplit(acfg->extra_bin_paths, ":", -1) : NULL;
  AmigaToolchainStatus *ntc = amiga_project_probe_toolchain_ex(
      extra ? (const char *const *)extra : NULL,
      self->cfg_c_include_dirs,
      self->cfg_asm_include_dirs);
  g_clear_pointer(&self->toolchain, amiga_toolchain_status_free);
  self->toolchain = ntc;
  if (self->toolchain && self->cfg_amiberry_bin && self->cfg_amiberry_bin[0]) {
    g_autofree char *resolved = g_find_program_in_path(self->cfg_amiberry_bin);
    if (resolved) { g_free(self->toolchain->amiberry); self->toolchain->amiberry = g_steal_pointer(&resolved); }
    else if (g_file_test(self->cfg_amiberry_bin, G_FILE_TEST_IS_EXECUTABLE)) {
      g_free(self->toolchain->amiberry); self->toolchain->amiberry = g_strdup(self->cfg_amiberry_bin);
    }
  }
  if (self->hrm_view && (!self->project || !self->project->hrm_dir || !self->project->hrm_dir[0])) {
    amiga_hrm_view_set_hrm_dir(self->hrm_view, acfg->hrm_dir);
  }

  const char *scheme = "kajo-dark";
  int tab_w = 2;
  if (acfg->inherit_ide_editor_prefs) {
    g_autoptr(KajoIdeCfgRef) icfg = kajo_config_store_acquire_ide(NULL);
    if (icfg) {
      if (icfg->color_scheme && icfg->color_scheme[0]) scheme = icfg->color_scheme;
      if (icfg->tab_width > 0) tab_w = icfg->tab_width;
    }
  }
  g_free(self->inherited_scheme); self->inherited_scheme = g_strdup(scheme);
  self->inherited_tab_width = tab_w;
  if (self->source_view) {
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(self->source_view), (guint)tab_w);
    for (guint i = 0; self->open_bufs && i < self->open_bufs->len; i++) {
      ide_buffer_set_style_scheme(g_ptr_array_index(self->open_bufs, i), scheme);
    }
    gtk_widget_queue_draw(self->source_view);
  }
  if (self->project) amiga_project_write_compile_commands(self->project, self->toolchain, NULL);
  amiga_window_lsp_sync_project(self);
  amiga_window_sync_status_bar(self);
  update_cursor_and_selection_cycles(self);
}

static void on_global_cfg_changed(KajoConfigStore *s G_GNUC_UNUSED, KajoConfigDomain c G_GNUC_UNUSED, gpointer ud) {
  GWeakRef *weak = ud; g_autoptr(KajoAmigaWindow) self = g_weak_ref_get(weak);
  if (self) apply_global_config(self);
}
static void cfg_weak_free(gpointer d) { GWeakRef *w = d; g_weak_ref_clear(w); g_free(w); }

static void
kajo_amiga_window_init(KajoAmigaWindow *self)
{
  amiga_window_debug_init(self);
  self->toolchain = amiga_project_probe_toolchain(NULL);
  self->symbols = amiga_symbols_new();
  self->runner = amiga_runner_new();
  self->rsp = amiga_rsp_client_new();
  amiga_rsp_client_set_stop_callback(self->rsp, amiga_window_debug_on_rsp_stop, self);
  self->bp_addrs = g_hash_table_new(g_direct_hash, g_direct_equal);
  self->diags = g_ptr_array_new_with_free_func((GDestroyNotify)amiga_build_diag_free);
  self->open_bufs = g_ptr_array_new_with_free_func((GDestroyNotify)ide_buffer_free);
  self->fsm = ide_fsm_new();
  self->user_sidebar_open = true;
  self->show_cycle_gutter = true;
  self->show_sel_cycles = true;
  self->enable_hover_decoders = true;
  self->inherited_tab_width = 2;

  build_ui(self);
  self->editor_host.source_view = GTK_SOURCE_VIEW(self->source_view);
  self->editor_host.fsm = self->fsm;
  self->editor_host.yank_register = &self->yank_reg;

  apply_global_config(self);
  GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, self);
  self->cfg_sub_id = kajo_config_store_subscribe(
      NULL, (KajoConfigDomain)(KAJO_CONFIG_DOMAIN_AMIGA | KAJO_CONFIG_DOMAIN_IDE),
      on_global_cfg_changed, weak, cfg_weak_free);
}

KajoAmigaWindow *kajo_amiga_window_new(GtkApplication *app) {
  return g_object_new(KAJO_TYPE_AMIGA_WINDOW, "application", app, NULL);
}

static void
record_recent_project(const char *dir_path)
{
  if (!dir_path || !dir_path[0]) return;
  g_autofree char *rec = g_build_filename(g_get_user_data_dir(), "kajo", "amiga-recent.txt", NULL);
  g_autofree char *parent = g_path_get_dirname(rec);
  g_mkdir_with_parents(parent, 0755);
  g_autofree char *old = NULL;
  g_autoptr(GString) out = g_string_new(dir_path);
  g_string_append_c(out, '\n');
  if (g_file_get_contents(rec, &old, NULL, NULL) && old) {
    g_auto(GStrv) lines = g_strsplit(old, "\n", 10);
    for (int i = 0, kept = 0; lines[i] && kept < 5; i++) {
      char *s = g_strstrip(lines[i]);
      if (s[0] && g_strcmp0(s, dir_path) != 0) { g_string_append_printf(out, "%s\n", s); kept++; }
    }
  }
  g_file_set_contents(rec, out->str, (gssize)out->len, NULL);
}

void
kajo_amiga_window_open_project_dir(KajoAmigaWindow *self, const char *dir_path)
{
  g_return_if_fail(KAJO_IS_AMIGA_WINDOW(self));
  g_autoptr(GError) err = NULL;
  AmigaProject *proj = amiga_project_load_dir(dir_path, &err);
  if (!proj) { show_toast(self, err ? err->message : "Cannot load Amiga project"); return; }
  record_recent_project(dir_path);
  g_clear_pointer(&self->project, amiga_project_free);
  self->project = proj;
  self->attach_mode = false;
  self->setup_tab_open = false;
  g_ptr_array_set_size(self->open_bufs, 0);
  self->active_buf = NULL;

  amiga_project_write_compile_commands(proj, self->toolchain, NULL);
  ide_tree_set_workspace_root(self->file_tree, proj->root_dir);
  amiga_window_populate_project_dropdowns(self);
  amiga_project_setup_view_bind(self->setup_view, proj, self->toolchain);
  if (proj->hrm_dir && proj->hrm_dir[0]) {
    g_autofree char *abs_hrm = g_path_is_absolute(proj->hrm_dir) ? g_strdup(proj->hrm_dir)
                                                                 : g_build_filename(proj->root_dir, proj->hrm_dir, NULL);
    amiga_hrm_view_set_hrm_dir(self->hrm_view, abs_hrm);
  }
  amiga_window_debug_reload_symbols(self);
  amiga_window_lsp_sync_project(self);

  self->user_sidebar_open = true;
  amiga_window_apply_session_state(self, AMIGA_SESSION_EDIT);
  gtk_label_set_text(GTK_LABEL(self->lbl_ws_state), proj->name ? proj->name : dir_path);

  if (proj->targets->len > 0) {
    AmigaTarget *t = g_ptr_array_index(proj->targets, 0);
    if (t->bitplane_symbol) amiga_bitplane_view_set_symbol_hint(self->bpl_view, t->bitplane_symbol);
    if (t->sources->len > 0) {
      const char *rel = g_ptr_array_index(t->sources, 0);
      g_autofree char *abs = g_build_filename(proj->root_dir, rel, NULL);
      amiga_window_open_file_at_line(self, abs, 1);
    }
  }
}

void
kajo_amiga_window_open_attach(KajoAmigaWindow *self, const char *host, uint16_t port)
{
  g_return_if_fail(KAJO_IS_AMIGA_WINDOW(self));
  const char *h = (host && host[0]) ? host
                : (self->cfg_gdb_host && self->cfg_gdb_host[0]) ? self->cfg_gdb_host : "127.0.0.1";
  uint16_t p = port ? port : (self->cfg_gdb_port ? self->cfg_gdb_port : 2345);
  amiga_window_debug_connect_with_retry(self, h, p, true, NULL);
}
