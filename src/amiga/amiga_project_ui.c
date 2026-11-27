#include "amiga_project_ui.h"
#include "../config.h"

#include <stdlib.h>
#include <string.h>

/* ─── 1. Welcome / Start Screen ─── */

typedef struct {
  AmigaActionCb      on_new;
  AmigaActionCb      on_open;
  AmigaDirSelectedCb on_open_recent;
  AmigaActionCb      on_attach;
  AmigaActionCb      on_settings;
  gpointer           user_data;
} WelcomeCtx;

static void on_w_new(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  WelcomeCtx *c = ud; if (c->on_new) c->on_new(c->user_data);
}
static void on_w_open(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  WelcomeCtx *c = ud; if (c->on_open) c->on_open(c->user_data);
}
static void on_w_attach(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  WelcomeCtx *c = ud; if (c->on_attach) c->on_attach(c->user_data);
}
static void on_w_settings(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  WelcomeCtx *c = ud; if (c->on_settings) c->on_settings(c->user_data);
}
static void on_w_recent_activated(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *row, gpointer ud) {
  WelcomeCtx *c = ud;
  const char *dir = g_object_get_data(G_OBJECT(row), "recent-dir");
  if (c->on_open_recent && dir) c->on_open_recent(dir, c->user_data);
}

static GtkWidget *
make_tool_badge_row(const char *label, const char *found_val)
{
  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);
  GtkWidget *lbl = gtk_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_widget_set_hexpand(lbl, TRUE);
  gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
  GtkWidget *st = gtk_label_new(found_val ? found_val : "NOT FOUND");
  gtk_widget_add_css_class(st, found_val ? "amiga-badge-live" : "amiga-badge-placeholder");
  gtk_widget_set_valign(st, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(hb), lbl);
  gtk_box_append(GTK_BOX(hb), st);
  return hb;
}

static void
populate_recent_list(GtkWidget *list)
{
  g_autofree char *rec = g_build_filename(g_get_user_data_dir(), "kajo", "amiga-recent.txt", NULL);
  g_autofree char *raw = NULL;
  int added = 0;
  if (g_file_get_contents(rec, &raw, NULL, NULL) && raw) {
    g_auto(GStrv) lines = g_strsplit(raw, "\n", 6);
    for (int i = 0; lines[i] && lines[i][0]; i++) {
      char *dir = g_strstrip(lines[i]);
      if (!dir[0] || !g_file_test(dir, G_FILE_TEST_IS_DIR)) continue;
      g_autofree char *bn = g_path_get_basename(dir);
      g_autofree char *txt = g_strdup_printf("📁 %s  (%s)", bn, dir);
      GtkWidget *l = gtk_label_new(txt);
      gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
      gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
      gtk_list_box_append(GTK_LIST_BOX(list), l);
      GtkWidget *row = gtk_widget_get_last_child(list);
      if (GTK_IS_LIST_BOX_ROW(row)) {
        g_object_set_data_full(G_OBJECT(row), "recent-dir", g_strdup(dir), g_free);
      }
      added++;
    }
  }
  if (added == 0) {
    GtkWidget *e = gtk_label_new("No recent Amiga workspaces yet.");
    gtk_label_set_xalign(GTK_LABEL(e), 0.0f);
    gtk_widget_add_css_class(e, "amiga-muted-hint");
    gtk_list_box_append(GTK_LIST_BOX(list), e);
  }
}

GtkWidget *
amiga_welcome_view_new(const AmigaToolchainStatus *tc,
                       AmigaActionCb               on_new,
                       AmigaActionCb               on_open,
                       AmigaDirSelectedCb          on_open_recent,
                       AmigaActionCb               on_attach,
                       AmigaActionCb               on_settings,
                       gpointer                    user_data)
{
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_add_css_class(scroll, "amiga-welcome-box");
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);

  GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(center, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(center, 820, -1);

  GtkWidget *hero = gtk_label_new("◈ KAJO AMIGA • 68000 / C + ASM OCS–AGA STUDIO");
  gtk_widget_add_css_class(hero, "settings-header-title");
  gtk_label_set_xalign(GTK_LABEL(hero), 0.0f);
  GtkWidget *sub = gtk_label_new(
      "Self-contained C17 IDE with Option D build DAG, in-process ADFlib packager, and stock Amiberry >= 8.5.0 GDB RSP.");
  gtk_label_set_wrap(GTK_LABEL(sub), TRUE);
  gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
  gtk_box_append(GTK_BOX(center), hero);
  gtk_box_append(GTK_BOX(center), sub);

  GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_box_set_homogeneous(GTK_BOX(cols), TRUE);

  WelcomeCtx *ctx = g_new0(WelcomeCtx, 1);
  ctx->on_new = on_new; ctx->on_open = on_open; ctx->on_open_recent = on_open_recent;
  ctx->on_attach = on_attach; ctx->on_settings = on_settings; ctx->user_data = user_data;
  g_object_set_data_full(G_OBJECT(scroll), "welcome-ctx", ctx, g_free);

  GtkWidget *act_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(act_card, "amiga-welcome-card");
  GtkWidget *act_hdr = gtk_label_new("START WORKFLOW & RECENT PROJECTS");
  gtk_label_set_xalign(GTK_LABEL(act_hdr), 0.0f);
  gtk_widget_add_css_class(act_hdr, "amiga-disasm-header");

  GtkWidget *b_new = gtk_button_new_with_label("+ NEW PROJECT (Ctrl+Shift+N)");
  gtk_widget_add_css_class(b_new, "ide-toolbar-btn");
  gtk_widget_add_css_class(b_new, "accent-btn");
  GtkWidget *b_open = gtk_button_new_with_label("📁 OPEN WORKSPACE / MAKEFILE (Ctrl+O)");
  gtk_widget_add_css_class(b_open, "ide-toolbar-btn");
  GtkWidget *b_attach = gtk_button_new_with_label("🔌 ATTACH AMIBERRY :2345 (Ctrl+Shift+A)");
  gtk_widget_add_css_class(b_attach, "ide-toolbar-btn");
  GtkWidget *b_set = gtk_button_new_with_label("⚙ TOOLCHAIN & ROM SETTINGS (Ctrl+,)");
  gtk_widget_add_css_class(b_set, "ide-toolbar-btn");

  g_signal_connect(b_new, "clicked", G_CALLBACK(on_w_new), ctx);
  g_signal_connect(b_open, "clicked", G_CALLBACK(on_w_open), ctx);
  g_signal_connect(b_attach, "clicked", G_CALLBACK(on_w_attach), ctx);
  g_signal_connect(b_set, "clicked", G_CALLBACK(on_w_settings), ctx);

  GtkWidget *rlist = gtk_list_box_new();
  gtk_widget_add_css_class(rlist, "amiga-diag-list");
  populate_recent_list(rlist);
  g_signal_connect(rlist, "row-activated", G_CALLBACK(on_w_recent_activated), ctx);

  gtk_box_append(GTK_BOX(act_card), act_hdr);
  gtk_box_append(GTK_BOX(act_card), b_new);
  gtk_box_append(GTK_BOX(act_card), b_open);
  gtk_box_append(GTK_BOX(act_card), b_attach);
  gtk_box_append(GTK_BOX(act_card), b_set);
  gtk_box_append(GTK_BOX(act_card), rlist);

  g_autoptr(KajoAmigaCfgRef) acfg = kajo_config_store_acquire_amiga(NULL);
  GtkWidget *tc_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(tc_card, "amiga-welcome-card");
  GtkWidget *tc_hdr = gtk_label_new("TOOLCHAIN, KICKSTART & ADFLIB");
  gtk_label_set_xalign(GTK_LABEL(tc_hdr), 0.0f);
  gtk_widget_add_css_class(tc_hdr, "amiga-disasm-header");
  gtk_box_append(GTK_BOX(tc_card), tc_hdr);

  gtk_box_append(GTK_BOX(tc_card), make_tool_badge_row("amiberry (>= 8.5.0)", tc ? tc->amiberry : NULL));
  gtk_box_append(GTK_BOX(tc_card), make_tool_badge_row(
      "Kickstart A500", (acfg && acfg->kickstart_a500 && acfg->kickstart_a500[0]) ? acfg->kickstart_a500 : NULL));
  gtk_box_append(GTK_BOX(tc_card), make_tool_badge_row(
      "Kickstart A1200", (acfg && acfg->kickstart_a1200 && acfg->kickstart_a1200[0]) ? acfg->kickstart_a1200 : NULL));
  gtk_box_append(GTK_BOX(tc_card), make_tool_badge_row("adflib (in-process)", tc ? tc->adflib_version : NULL));
  gtk_box_append(GTK_BOX(tc_card), make_tool_badge_row("vasmm68k_mot", tc ? tc->vasmm68k_mot : NULL));
  gtk_box_append(GTK_BOX(tc_card), make_tool_badge_row("m68k-amiga-elf-gcc", tc ? tc->m68k_elf_gcc : NULL));
  gtk_box_append(GTK_BOX(tc_card), make_tool_badge_row("elf2hunk / vlink",
                                                       (tc && tc->elf2hunk) ? tc->elf2hunk : (tc ? tc->vlink : NULL)));
  gtk_box_append(GTK_BOX(tc_card), make_tool_badge_row("m68k-lsp-server", tc ? tc->m68k_lsp_server : NULL));
  gtk_box_append(GTK_BOX(tc_card), make_tool_badge_row("clangd", tc ? tc->clangd : NULL));

  gtk_box_append(GTK_BOX(cols), act_card);
  gtk_box_append(GTK_BOX(cols), tc_card);
  gtk_box_append(GTK_BOX(center), cols);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), center);
  return scroll;
}

/* ─── 2. New Project Wizard Modal (Ctrl+Shift+N) ─── */

typedef struct {
  GtkWindow          *modal;
  GtkWidget          *entry_name;
  GtkWidget          *entry_dir;
  GtkWidget          *tpl_btns[4];
  GtkWidget          *dd_profile;
  GtkWidget          *dd_pkg;
  GtkWidget          *lbl_err;
  AmigaDirSelectedCb  on_created;
  gpointer            user_data;
} WizardCtx;

static void on_wiz_cancel(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  gtk_window_destroy(((WizardCtx *)ud)->modal);
}

static void
on_wiz_browse_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  WizardCtx *w = ud;
  g_autoptr(GFile) f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
  if (f && GTK_IS_EDITABLE(w->entry_dir)) {
    g_autofree char *p = g_file_get_path(f);
    if (p) gtk_editable_set_text(GTK_EDITABLE(w->entry_dir), p);
  }
}

static void on_wiz_browse(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  WizardCtx *w = ud;
  g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Select Parent Folder");
  gtk_file_dialog_select_folder(dlg, w->modal, NULL, on_wiz_browse_done, w);
}

static void
on_wiz_tpl_toggled(GtkToggleButton *tb, gpointer ud)
{
  WizardCtx *w = ud;
  if (!gtk_toggle_button_get_active(tb)) return;
  if (GTK_WIDGET(tb) == w->tpl_btns[2]) {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->dd_pkg), 1);
  } else {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->dd_pkg), 0);
  }
}

static void
on_wiz_create(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  WizardCtx *w = ud;
  const char *name = gtk_editable_get_text(GTK_EDITABLE(w->entry_name));
  const char *parent = gtk_editable_get_text(GTK_EDITABLE(w->entry_dir));
  if (!name || !name[0] || !parent || !parent[0]) {
    gtk_label_set_text(GTK_LABEL(w->lbl_err), "Project name and folder are required.");
    gtk_widget_set_visible(w->lbl_err, TRUE);
    return;
  }
  AmigaTemplateKind tpl = AMIGA_TEMPLATE_ASM_OS;
  for (int i = 0; i < 4; i++) {
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->tpl_btns[i]))) { tpl = (AmigaTemplateKind)i; break; }
  }
  static const char *const profs[] = { "a500_ocs", "a500p_ecs", "a1200_aga", "a1200_fast" };
  guint pi = gtk_drop_down_get_selected(GTK_DROP_DOWN(w->dd_profile));
  const char *prof_id = (pi < G_N_ELEMENTS(profs)) ? profs[pi] : "a500_ocs";
  guint ki = gtk_drop_down_get_selected(GTK_DROP_DOWN(w->dd_pkg));
  AmigaTargetKind pkg = (ki == 1) ? AMIGA_TARGET_EXE_ADF : (ki == 2) ? AMIGA_TARGET_FLAT_BIN : AMIGA_TARGET_EXE_DH0;

  g_autofree char *target_dir = g_build_filename(parent, name, NULL);
  g_autoptr(GError) err = NULL;
  if (!amiga_project_init_template(target_dir, name, tpl, prof_id, pkg, &err)) {
    gtk_label_set_text(GTK_LABEL(w->lbl_err), err ? err->message : "Project init failed");
    gtk_widget_set_visible(w->lbl_err, TRUE);
    return;
  }
  AmigaDirSelectedCb cb = w->on_created;
  gpointer udata = w->user_data;
  g_autofree char *created = g_strdup(target_dir);
  gtk_window_destroy(w->modal);
  if (cb) cb(created, udata);
}

void
amiga_project_wizard_present(GtkWindow                  *parent,
                             const AmigaToolchainStatus *tc,
                             AmigaDirSelectedCb          on_created,
                             gpointer                    user_data)
{
  GtkWindow *win = GTK_WINDOW(gtk_window_new());
  gtk_window_set_decorated(win, FALSE);
  gtk_window_set_modal(win, parent != NULL);
  if (parent) gtk_window_set_transient_for(win, parent);
  gtk_window_set_default_size(win, 660, 500);
  gtk_widget_add_css_class(GTK_WIDGET(win), "shell-amiga-modal");

  WizardCtx *w = g_new0(WizardCtx, 1);
  w->modal = win; w->on_created = on_created; w->user_data = user_data;
  g_object_set_data_full(G_OBJECT(win), "wiz-ctx", w, g_free);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *handle = gtk_window_handle_new();
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(hdr, "settings-header-bar");
  GtkWidget *title = gtk_label_new("📁 NEW AMIGA PROJECT");
  gtk_widget_add_css_class(title, "settings-header-title");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  GtkWidget *btn_x = gtk_button_new_with_label("✕");
  gtk_widget_add_css_class(btn_x, "settings-close-btn");
  g_signal_connect(btn_x, "clicked", G_CALLBACK(on_wiz_cancel), w);
  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), btn_x);
  gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), hdr);
  gtk_box_append(GTK_BOX(outer), handle);

  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(body, 14); gtk_widget_set_margin_bottom(body, 14);
  gtk_widget_set_margin_start(body, 16); gtk_widget_set_margin_end(body, 16);

  /* Live toolchain readiness strip using passed tc */
  GtkWidget *tc_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  struct { const char *lbl; bool ok; } chips[5] = {
    { "amiberry", tc && tc->amiberry },
    { "vasm", tc && tc->vasmm68k_mot },
    { "m68k-gcc", tc && tc->m68k_elf_gcc },
    { "elf2hunk", tc && tc->elf2hunk },
    { "adflib", tc && tc->adflib_version },
  };
  for (int i = 0; i < 5; i++) {
    g_autofree char *s = g_strdup_printf("%s %s", chips[i].ok ? "✓" : "○", chips[i].lbl);
    GtkWidget *c = gtk_label_new(s);
    gtk_widget_add_css_class(c, chips[i].ok ? "amiga-badge-live" : "amiga-badge-placeholder");
    gtk_box_append(GTK_BOX(tc_row), c);
  }
  gtk_box_append(GTK_BOX(body), tc_row);

  GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *ln = gtk_label_new("Name:");
  gtk_widget_set_size_request(ln, 96, -1); gtk_label_set_xalign(GTK_LABEL(ln), 0.0f);
  w->entry_name = gtk_entry_new();
  gtk_widget_add_css_class(w->entry_name, "amiga-entry");
  gtk_editable_set_text(GTK_EDITABLE(w->entry_name), "CopperDemo");
  gtk_widget_set_hexpand(w->entry_name, TRUE);
  gtk_box_append(GTK_BOX(name_row), ln); gtk_box_append(GTK_BOX(name_row), w->entry_name);
  gtk_box_append(GTK_BOX(body), name_row);

  GtkWidget *dir_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *ld = gtk_label_new("Location:");
  gtk_widget_set_size_request(ld, 96, -1); gtk_label_set_xalign(GTK_LABEL(ld), 0.0f);
  w->entry_dir = gtk_entry_new();
  gtk_widget_add_css_class(w->entry_dir, "amiga-entry");
  gtk_editable_set_text(GTK_EDITABLE(w->entry_dir), g_get_home_dir());
  gtk_widget_set_hexpand(w->entry_dir, TRUE);
  GtkWidget *btn_br = gtk_button_new_with_label("BROWSE…");
  gtk_widget_add_css_class(btn_br, "ide-toolbar-btn");
  g_signal_connect(btn_br, "clicked", G_CALLBACK(on_wiz_browse), w);
  gtk_box_append(GTK_BOX(dir_row), ld); gtk_box_append(GTK_BOX(dir_row), w->entry_dir);
  gtk_box_append(GTK_BOX(dir_row), btn_br); gtk_box_append(GTK_BOX(body), dir_row);

  static const char *const tpl_titles[4] = {
    "68k ASM AmigaDOS", "Bare-Metal OCS/ECS", "Bootable ADF Floppy", "Mixed C + 68k ASM"
  };
  static const char *const tpl_subs[4] = {
    "vasmm68k_mot · EXE (Direct DH0:)",
    "Hardware copper/BPL loop · EXE",
    "ADFlib OFS image booting single .exe",
    "m68k-elf-gcc + vasm -> elf2hunk"
  };
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8); gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
  for (int i = 0; i < 4; i++) {
    GtkWidget *cb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *ct = gtk_label_new(tpl_titles[i]);
    gtk_label_set_xalign(GTK_LABEL(ct), 0.0f); gtk_widget_add_css_class(ct, "amiga-kv-val");
    GtkWidget *cs = gtk_label_new(tpl_subs[i]);
    gtk_label_set_xalign(GTK_LABEL(cs), 0.0f); gtk_widget_add_css_class(cs, "amiga-muted-hint");
    gtk_box_append(GTK_BOX(cb), ct); gtk_box_append(GTK_BOX(cb), cs);
    w->tpl_btns[i] = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(w->tpl_btns[i]), cb);
    gtk_widget_add_css_class(w->tpl_btns[i], "amiga-template-card");
    if (i > 0) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(w->tpl_btns[i]), GTK_TOGGLE_BUTTON(w->tpl_btns[0]));
    g_signal_connect(w->tpl_btns[i], "toggled", G_CALLBACK(on_wiz_tpl_toggled), w);
    gtk_grid_attach(GTK_GRID(grid), w->tpl_btns[i], i % 2, i / 2, 1, 1);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->tpl_btns[0]), TRUE);
  gtk_box_append(GTK_BOX(body), grid);

  GtkWidget *dd_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  w->dd_profile = gtk_drop_down_new_from_strings((const char *[]){
      "A500 OCS (68000)", "A500+ ECS", "A1200 AGA (68020)", "A1200 Fast", NULL });
  w->dd_pkg = gtk_drop_down_new_from_strings((const char *[]){
      "EXE (Direct DH0:)", "EXE (Bootable ADF)", "Flat Binary (.bin)", NULL });
  gtk_widget_set_hexpand(w->dd_profile, TRUE); gtk_widget_set_hexpand(w->dd_pkg, TRUE);
  gtk_box_append(GTK_BOX(dd_row), w->dd_profile); gtk_box_append(GTK_BOX(dd_row), w->dd_pkg);
  gtk_box_append(GTK_BOX(body), dd_row);

  w->lbl_err = gtk_label_new("");
  gtk_widget_add_css_class(w->lbl_err, "amiga-diag-error");
  gtk_widget_set_visible(w->lbl_err, FALSE);
  gtk_box_append(GTK_BOX(body), w->lbl_err);

  GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(foot, GTK_ALIGN_END);
  GtkWidget *b_cancel = gtk_button_new_with_label("CANCEL");
  gtk_widget_add_css_class(b_cancel, "ide-toolbar-btn");
  g_signal_connect(b_cancel, "clicked", G_CALLBACK(on_wiz_cancel), w);
  GtkWidget *b_create = gtk_button_new_with_label("CREATE PROJECT");
  gtk_widget_add_css_class(b_create, "ide-toolbar-btn"); gtk_widget_add_css_class(b_create, "accent-btn");
  g_signal_connect(b_create, "clicked", G_CALLBACK(on_wiz_create), w);
  gtk_box_append(GTK_BOX(foot), b_cancel); gtk_box_append(GTK_BOX(foot), b_create);
  gtk_box_append(GTK_BOX(body), foot);

  gtk_box_append(GTK_BOX(outer), body);
  gtk_window_set_child(win, outer);
  gtk_window_present(win);
}

/* ─── 3. Center-Pane [⚙ kajo-amiga.toml] Setup View ─── */

struct _AmigaProjectSetupView {
  GtkBox parent_instance;

  AmigaProject               *proj; /* borrowed */
  const AmigaToolchainStatus *tc;   /* borrowed */
  AmigaProjectSavedCb         on_saved;
  gpointer                    user_data;
  guint                       sel_target;
  guint                       sel_profile;
  bool                        binding;

  GtkWidget *seg_form;
  GtkWidget *seg_raw;
  GtkWidget *mode_stack;
  GtkWidget *form_stack;
  GtkWidget *tab_btns[3];

  /* Targets & Artifacts (master-detail) */
  GtkWidget *entry_proj_name;
  GtkWidget *dd_build_mode;
  GtkWidget *entry_build_cmd;
  GtkWidget *entry_shared_sources;
  GtkWidget *targets_list;
  GtkWidget *entry_target_name;
  GtkWidget *dd_target_kind;
  GtkWidget *entry_target_cpu;
  GtkWidget *dd_toolchain;
  GtkWidget *entry_entry_sym;
  GtkWidget *entry_load_addr;
  GtkWidget *entry_sources;
  GtkWidget *entry_c_includes;
  GtkWidget *entry_asm_includes;
  GtkWidget *entry_defines;

  /* Hardware Profiles (master-detail editable) */
  GtkWidget *profiles_list;
  GtkWidget *entry_prof_id;
  GtkWidget *entry_prof_model;
  GtkWidget *entry_prof_cpu;
  GtkWidget *entry_prof_chipset;
  GtkWidget *entry_prof_mem;
  GtkWidget *entry_prof_kick;
  GtkWidget *chk_prof_ntsc;
  GtkWidget *chk_prof_ce;

  /* Debug & Hooks */
  GtkWidget *entry_pre_build;
  GtkWidget *entry_post_build;
  GtkWidget *chk_trap86;
  GtkWidget *entry_hrm_dir;
  GtkWidget *entry_copper_sym;
  GtkWidget *entry_bpl_sym;

  GtkWidget *raw_view;
  GtkWidget *lbl_status;
};

G_DEFINE_FINAL_TYPE(AmigaProjectSetupView, amiga_project_setup_view, GTK_TYPE_BOX)

static void
amiga_project_setup_view_class_init(AmigaProjectSetupViewClass *klass G_GNUC_UNUSED)
{
}

static void
split_csv_into(const char *csv, GPtrArray *dst)
{
  g_ptr_array_set_size(dst, 0);
  if (!csv || !csv[0]) return;
  g_auto(GStrv) parts = g_strsplit(csv, ",", -1);
  for (int i = 0; parts[i] != NULL; i++) {
    char *item = g_strstrip(parts[i]);
    if (item[0]) g_ptr_array_add(dst, g_strdup(item));
  }
}

static char *
join_csv(const GPtrArray *arr)
{
  g_autoptr(GString) s = g_string_new("");
  for (guint i = 0; arr && i < arr->len; i++) {
    if (i > 0) g_string_append(s, ", ");
    g_string_append(s, (const char *)g_ptr_array_index(arr, i));
  }
  return g_string_free(g_steal_pointer(&s), FALSE);
}

static void
clear_lb(GtkWidget *lb)
{
  GtkWidget *ch;
  while ((ch = gtk_widget_get_first_child(lb)) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(lb), ch);
  }
}

static void
populate_target_fields(AmigaProjectSetupView *self)
{
  if (!self->proj || !self->proj->targets || self->proj->targets->len == 0) return;
  if (self->sel_target >= self->proj->targets->len) self->sel_target = 0;
  const AmigaTarget *t = g_ptr_array_index(self->proj->targets, self->sel_target);
  self->binding = true;
  gtk_editable_set_text(GTK_EDITABLE(self->entry_target_name), t->name ? t->name : "");
  gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_target_kind), (guint)t->kind);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_toolchain), (guint)t->toolchain);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_target_cpu), t->cpu ? t->cpu : "68000");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_entry_sym), t->entry_symbol ? t->entry_symbol : "");
  g_autofree char *la = t->load_addr ? g_strdup_printf("$%08X", t->load_addr) : g_strdup("");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_load_addr), la);
  g_autofree char *srcs = join_csv(t->sources);
  g_autofree char *c_incs = join_csv(t->c_includes->len > 0 ? t->c_includes : t->includes);
  g_autofree char *a_incs = join_csv(t->asm_includes->len > 0 ? t->asm_includes : t->includes);
  g_autofree char *defs = join_csv(t->defines);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_sources), srcs);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_c_includes), c_incs);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_asm_includes), a_incs);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_defines), defs);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_copper_sym), t->copper_symbol ? t->copper_symbol : "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_bpl_sym), t->bitplane_symbol ? t->bitplane_symbol : "");
  self->binding = false;
}

static void
populate_profile_fields(AmigaProjectSetupView *self)
{
  if (!self->proj || !self->proj->profiles || self->proj->profiles->len == 0) return;
  if (self->sel_profile >= self->proj->profiles->len) self->sel_profile = 0;
  const AmigaHwProfile *hp = g_ptr_array_index(self->proj->profiles, self->sel_profile);
  self->binding = true;
  gtk_editable_set_text(GTK_EDITABLE(self->entry_prof_id), hp->id ? hp->id : "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_prof_model), hp->model ? hp->model : "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_prof_cpu), hp->cpu ? hp->cpu : "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_prof_chipset), hp->chipset ? hp->chipset : "");
  g_autofree char *ms = g_strdup_printf("%u,%u,%u", hp->chip_kb, hp->slow_kb, hp->fast_kb);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_prof_mem), ms);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_prof_kick), hp->kickstart ? hp->kickstart : "");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(self->chk_prof_ntsc), hp->ntsc);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(self->chk_prof_ce), hp->cycle_exact);
  self->binding = false;
}

static void
rebuild_master_lists(AmigaProjectSetupView *self)
{
  if (!self->proj) return;
  clear_lb(self->targets_list);
  for (guint i = 0; i < self->proj->targets->len; i++) {
    const AmigaTarget *t = g_ptr_array_index(self->proj->targets, i);
    GtkWidget *l = gtk_label_new(t->name ? t->name : "target");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_list_box_append(GTK_LIST_BOX(self->targets_list), l);
  }
  clear_lb(self->profiles_list);
  for (guint i = 0; i < self->proj->profiles->len; i++) {
    const AmigaHwProfile *p = g_ptr_array_index(self->proj->profiles, i);
    GtkWidget *l = gtk_label_new(p->id ? p->id : "profile");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_list_box_append(GTK_LIST_BOX(self->profiles_list), l);
  }
}

static void
save_current_target_fields(AmigaProjectSetupView *self)
{
  if (self->binding || !self->proj || !self->proj->targets || self->sel_target >= self->proj->targets->len) return;
  AmigaTarget *t = g_ptr_array_index(self->proj->targets, self->sel_target);
  const char *tn = gtk_editable_get_text(GTK_EDITABLE(self->entry_target_name));
  if (tn && tn[0]) { g_free(t->name); t->name = g_strdup(tn); }
  t->kind = (AmigaTargetKind)gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_target_kind));
  t->toolchain = (AmigaToolchainKind)gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_toolchain));
  g_free(t->cpu); t->cpu = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_target_cpu)));
  const char *es = gtk_editable_get_text(GTK_EDITABLE(self->entry_entry_sym));
  g_free(t->entry_symbol); t->entry_symbol = (es && es[0]) ? g_strdup(es) : NULL;
  const char *la = gtk_editable_get_text(GTK_EDITABLE(self->entry_load_addr));
  t->load_addr = (la && la[0]) ? (uint32_t)strtoul((la[0] == '$') ? la + 1 : la, NULL, 16) : 0;
  split_csv_into(gtk_editable_get_text(GTK_EDITABLE(self->entry_sources)), t->sources);
  split_csv_into(gtk_editable_get_text(GTK_EDITABLE(self->entry_c_includes)), t->c_includes);
  split_csv_into(gtk_editable_get_text(GTK_EDITABLE(self->entry_asm_includes)), t->asm_includes);
  split_csv_into(gtk_editable_get_text(GTK_EDITABLE(self->entry_defines)), t->defines);
  const char *cs = gtk_editable_get_text(GTK_EDITABLE(self->entry_copper_sym));
  g_free(t->copper_symbol); t->copper_symbol = (cs && cs[0]) ? g_strdup(cs) : NULL;
  const char *bs = gtk_editable_get_text(GTK_EDITABLE(self->entry_bpl_sym));
  g_free(t->bitplane_symbol); t->bitplane_symbol = (bs && bs[0]) ? g_strdup(bs) : NULL;
}

static void
save_current_profile_fields(AmigaProjectSetupView *self)
{
  if (self->binding || !self->proj || !self->proj->profiles || self->sel_profile >= self->proj->profiles->len) return;
  AmigaHwProfile *hp = g_ptr_array_index(self->proj->profiles, self->sel_profile);
  const char *id = gtk_editable_get_text(GTK_EDITABLE(self->entry_prof_id));
  if (id && id[0]) { g_free(hp->id); hp->id = g_strdup(id); }
  g_free(hp->model); hp->model = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_prof_model)));
  g_free(hp->cpu); hp->cpu = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_prof_cpu)));
  g_free(hp->chipset); hp->chipset = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_prof_chipset)));
  const char *ks = gtk_editable_get_text(GTK_EDITABLE(self->entry_prof_kick));
  g_free(hp->kickstart); hp->kickstart = (ks && ks[0]) ? g_strdup(ks) : NULL;
  g_auto(GStrv) mem = g_strsplit(gtk_editable_get_text(GTK_EDITABLE(self->entry_prof_mem)), ",", 3);
  if (mem[0]) {
    hp->chip_kb = (uint32_t)strtoul(mem[0], NULL, 10);
    if (mem[1]) {
      hp->slow_kb = (uint32_t)strtoul(mem[1], NULL, 10);
      if (mem[2]) hp->fast_kb = (uint32_t)strtoul(mem[2], NULL, 10);
    }
  }
  hp->ntsc = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->chk_prof_ntsc));
  hp->cycle_exact = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->chk_prof_ce));
}

static void
apply_form_to_project(AmigaProjectSetupView *self)
{
  AmigaProject *p = self->proj;
  if (!p) return;
  g_free(p->name); p->name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_proj_name)));
  p->build_mode = (gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_build_mode)) == 1)
                      ? AMIGA_BUILD_EXTERNAL : AMIGA_BUILD_NATIVE;
  const char *bc = gtk_editable_get_text(GTK_EDITABLE(self->entry_build_cmd));
  g_free(p->build_cmd); p->build_cmd = (bc && bc[0]) ? g_strdup(bc) : NULL;
  split_csv_into(gtk_editable_get_text(GTK_EDITABLE(self->entry_shared_sources)), p->shared_sources);
  const char *pre = gtk_editable_get_text(GTK_EDITABLE(self->entry_pre_build));
  g_free(p->pre_build_cmd); p->pre_build_cmd = (pre && pre[0]) ? g_strdup(pre) : NULL;
  const char *post = gtk_editable_get_text(GTK_EDITABLE(self->entry_post_build));
  g_free(p->post_build_cmd); p->post_build_cmd = (post && post[0]) ? g_strdup(post) : NULL;
  p->trap86_guest_output = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->chk_trap86));
  const char *hrm = gtk_editable_get_text(GTK_EDITABLE(self->entry_hrm_dir));
  g_free(p->hrm_dir); p->hrm_dir = (hrm && hrm[0]) ? g_strdup(hrm) : NULL;
  save_current_target_fields(self);
  save_current_profile_fields(self);
}

static void
on_setup_mode_toggled(GtkToggleButton *tb, gpointer ud)
{
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->mode_stack || !gtk_toggle_button_get_active(tb)) return;
  bool is_raw = (GTK_WIDGET(tb) == self->seg_raw);
  if (is_raw && self->proj) {
    apply_form_to_project(self);
    g_autofree char *toml = amiga_project_serialize_toml(self->proj);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->raw_view)), toml ? toml : "", -1);
  }
  gtk_stack_set_visible_child_name(GTK_STACK(self->mode_stack), is_raw ? "raw" : "form");
}

static void
on_subtab_clicked(GtkToggleButton *tb, gpointer ud)
{
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->form_stack || !gtk_toggle_button_get_active(tb)) return;
  static const char *const pages[3] = { "targets", "profiles", "debug" };
  for (int i = 0; i < 3; i++) {
    if (GTK_WIDGET(tb) == self->tab_btns[i]) {
      gtk_stack_set_visible_child_name(GTK_STACK(self->form_stack), pages[i]);
      break;
    }
  }
}

static void on_target_row_sel(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *r, gpointer ud) {
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!r) return;
  save_current_target_fields(self);
  self->sel_target = (guint)gtk_list_box_row_get_index(r);
  populate_target_fields(self);
}
static void on_target_add(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->proj) return;
  save_current_target_fields(self);
  g_autofree char *nm = g_strdup_printf("target_%u", self->proj->targets->len + 1u);
  g_ptr_array_add(self->proj->targets, amiga_target_new(nm));
  self->sel_target = self->proj->targets->len - 1u;
  rebuild_master_lists(self); populate_target_fields(self);
}
static void on_target_del(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->proj || self->proj->targets->len <= 1) return;
  g_ptr_array_remove_index(self->proj->targets, self->sel_target);
  self->sel_target = 0; rebuild_master_lists(self); populate_target_fields(self);
}

static void on_prof_row_sel(GtkListBox *lb G_GNUC_UNUSED, GtkListBoxRow *r, gpointer ud) {
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!r) return;
  save_current_profile_fields(self);
  self->sel_profile = (guint)gtk_list_box_row_get_index(r);
  populate_profile_fields(self);
}
static void on_prof_clone(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->proj) return;
  save_current_profile_fields(self);
  g_autofree char *id = g_strdup_printf("custom_%u", self->proj->profiles->len + 1u);
  g_ptr_array_add(self->proj->profiles, amiga_hw_profile_new(id));
  self->sel_profile = self->proj->profiles->len - 1u;
  rebuild_master_lists(self); populate_profile_fields(self);
}
static void on_prof_del(GtkButton *b G_GNUC_UNUSED, gpointer ud) {
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->proj || self->proj->profiles->len <= 1) return;
  g_ptr_array_remove_index(self->proj->profiles, self->sel_profile);
  self->sel_profile = 0; rebuild_master_lists(self); populate_profile_fields(self);
}

static void
on_setup_save_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->proj) return;
  g_autoptr(GError) err = NULL;
  if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->mode_stack)), "raw") == 0) {
    GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->raw_view));
    GtkTextIter s, e; gtk_text_buffer_get_bounds(tb, &s, &e);
    g_autofree char *txt = gtk_text_buffer_get_text(tb, &s, &e, FALSE);
    AmigaProject *parsed = NULL;
    if (!amiga_project_parse_toml(txt, self->proj->root_dir, &parsed, &err)) {
      gtk_label_set_text(GTK_LABEL(self->lbl_status), err ? err->message : "TOML parse error");
      gtk_widget_set_visible(self->lbl_status, TRUE);
      return;
    }
    amiga_project_free(parsed);
    if (self->proj->manifest_path && !g_file_set_contents(self->proj->manifest_path, txt, -1, &err)) {
      gtk_label_set_text(GTK_LABEL(self->lbl_status), err ? err->message : "Write error");
      gtk_widget_set_visible(self->lbl_status, TRUE);
      return;
    }
  } else {
    apply_form_to_project(self);
    if (!amiga_project_save(self->proj, &err)) {
      gtk_label_set_text(GTK_LABEL(self->lbl_status), err ? err->message : "Save failed");
      gtk_widget_set_visible(self->lbl_status, TRUE);
      return;
    }
  }
  gtk_label_set_text(GTK_LABEL(self->lbl_status), "✓ Saved kajo-amiga.toml");
  gtk_widget_set_visible(self->lbl_status, TRUE);
  if (self->on_saved) self->on_saved(self->proj, self->user_data);
}

static void
on_export_makefile_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->proj || !self->proj->root_dir) return;
  apply_form_to_project(self);
  g_autofree char *mk = amiga_project_export_makefile(self->proj, self->tc);
  g_autofree char *path = g_build_filename(self->proj->root_dir, "Makefile.kajo", NULL);
  g_autoptr(GError) err = NULL;
  gtk_label_set_text(GTK_LABEL(self->lbl_status),
                     (mk && g_file_set_contents(path, mk, -1, &err))
                         ? "✓ Exported Makefile.kajo"
                         : (err ? err->message : "Export failed"));
  gtk_widget_set_visible(self->lbl_status, TRUE);
}

static GtkWidget *
make_form_row(const char *label_text, GtkWidget *ctrl)
{
  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);
  GtkWidget *lbl = gtk_label_new(label_text);
  gtk_widget_set_size_request(lbl, 136, -1);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_widget_set_hexpand(ctrl, TRUE);
  gtk_box_append(GTK_BOX(hb), lbl);
  gtk_box_append(GTK_BOX(hb), ctrl);
  return hb;
}

static GtkWidget *
make_entry(const char *ph)
{
  GtkWidget *e = gtk_entry_new();
  gtk_widget_add_css_class(e, "amiga-entry");
  if (ph) gtk_entry_set_placeholder_text(GTK_ENTRY(e), ph);
  return e;
}

static GtkWidget *
build_master_pane(GtkWidget **out_list, const char *add_lbl, GCallback add_cb, GCallback del_cb,
                  GCallback sel_cb, AmigaProjectSetupView *self)
{
  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_size_request(vb, 165, -1);
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *ba = gtk_button_new_with_label(add_lbl);
  GtkWidget *bd = gtk_button_new_with_label("DEL");
  gtk_widget_add_css_class(ba, "amiga-mini-btn"); gtk_widget_add_css_class(bd, "amiga-mini-btn");
  g_signal_connect(ba, "clicked", add_cb, self); g_signal_connect(bd, "clicked", del_cb, self);
  gtk_box_append(GTK_BOX(bar), ba); gtk_box_append(GTK_BOX(bar), bd);
  *out_list = gtk_list_box_new();
  gtk_widget_add_css_class(*out_list, "amiga-diag-list");
  gtk_widget_set_vexpand(*out_list, TRUE);
  g_signal_connect(*out_list, "row-selected", sel_cb, self);
  gtk_box_append(GTK_BOX(vb), bar); gtk_box_append(GTK_BOX(vb), *out_list);
  return vb;
}

static void
amiga_project_setup_view_init(AmigaProjectSetupView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);

  GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(top_bar, "amiga-setup-tab-bar");
  GtkWidget *title = gtk_label_new("⚙ kajo-amiga.toml");
  gtk_widget_add_css_class(title, "amiga-disasm-header");
  gtk_widget_set_hexpand(title, TRUE); gtk_label_set_xalign(GTK_LABEL(title), 0.0f);

  self->seg_form = gtk_toggle_button_new_with_label("FORM");
  self->seg_raw = gtk_toggle_button_new_with_label("RAW TOML");
  gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->seg_raw), GTK_TOGGLE_BUTTON(self->seg_form));
  gtk_widget_add_css_class(self->seg_form, "amiga-seg-btn");
  gtk_widget_add_css_class(self->seg_raw, "amiga-seg-btn");
  g_signal_connect(self->seg_form, "toggled", G_CALLBACK(on_setup_mode_toggled), self);
  g_signal_connect(self->seg_raw, "toggled", G_CALLBACK(on_setup_mode_toggled), self);

  GtkWidget *btn_mk = gtk_button_new_with_label("EXPORT MAKEFILE");
  gtk_widget_add_css_class(btn_mk, "ide-toolbar-btn");
  g_signal_connect(btn_mk, "clicked", G_CALLBACK(on_export_makefile_clicked), self);
  GtkWidget *btn_save = gtk_button_new_with_label("💾 SAVE & APPLY");
  gtk_widget_add_css_class(btn_save, "ide-toolbar-btn"); gtk_widget_add_css_class(btn_save, "accent-btn");
  g_signal_connect(btn_save, "clicked", G_CALLBACK(on_setup_save_clicked), self);

  self->lbl_status = gtk_label_new("");
  gtk_widget_add_css_class(self->lbl_status, "amiga-badge-snapshot");
  gtk_widget_set_visible(self->lbl_status, FALSE);

  gtk_box_append(GTK_BOX(top_bar), title); gtk_box_append(GTK_BOX(top_bar), self->lbl_status);
  gtk_box_append(GTK_BOX(top_bar), self->seg_form); gtk_box_append(GTK_BOX(top_bar), self->seg_raw);
  gtk_box_append(GTK_BOX(top_bar), btn_mk); gtk_box_append(GTK_BOX(top_bar), btn_save);
  gtk_box_append(GTK_BOX(self), top_bar);

  self->mode_stack = gtk_stack_new();
  gtk_widget_set_hexpand(self->mode_stack, TRUE); gtk_widget_set_vexpand(self->mode_stack, TRUE);

  GtkWidget *form_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(tab_bar, "amiga-setup-tab-bar");
  static const char *const tnames[3] = { "Targets", "Profiles", "Debug & Symbols" };
  for (int i = 0; i < 3; i++) {
    self->tab_btns[i] = gtk_toggle_button_new_with_label(tnames[i]);
    gtk_widget_add_css_class(self->tab_btns[i], "amiga-seg-btn");
    if (i > 0) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->tab_btns[i]), GTK_TOGGLE_BUTTON(self->tab_btns[0]));
    g_signal_connect(self->tab_btns[i], "toggled", G_CALLBACK(on_subtab_clicked), self);
    gtk_box_append(GTK_BOX(tab_bar), self->tab_btns[i]);
  }
  gtk_box_append(GTK_BOX(form_outer), tab_bar);

  self->form_stack = gtk_stack_new();
  gtk_widget_set_hexpand(self->form_stack, TRUE); gtk_widget_set_vexpand(self->form_stack, TRUE);

  /* Page 1: Targets & Artifacts */
  GtkWidget *p_t = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(p_t, 10); gtk_widget_set_margin_start(p_t, 12); gtk_widget_set_margin_end(p_t, 12);
  self->entry_proj_name = make_entry("Project");
  self->dd_build_mode = gtk_drop_down_new_from_strings((const char *[]){"native (Option D)", "external (cmd)", NULL});
  self->entry_build_cmd = make_entry("make");
  self->entry_shared_sources = make_entry("src/common.s");
  gtk_box_append(GTK_BOX(p_t), make_form_row("Project Name", self->entry_proj_name));
  gtk_box_append(GTK_BOX(p_t), make_form_row("Build Mode", self->dd_build_mode));
  gtk_box_append(GTK_BOX(p_t), make_form_row("External Cmd", self->entry_build_cmd));
  gtk_box_append(GTK_BOX(p_t), make_form_row("Shared Sources", self->entry_shared_sources));

  GtkWidget *tsplit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_append(GTK_BOX(tsplit), build_master_pane(&self->targets_list, "+ ADD",
      G_CALLBACK(on_target_add), G_CALLBACK(on_target_del), G_CALLBACK(on_target_row_sel), self));
  GtkWidget *tform = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_hexpand(tform, TRUE);
  self->entry_target_name = make_entry("target");
  self->dd_target_kind = gtk_drop_down_new_from_strings((const char *[]){
      "EXE (Direct DH0:)", "EXE (Bootable ADF)", "Flat Binary (.bin)", NULL });
  self->entry_target_cpu = make_entry("68000");
  self->dd_toolchain = gtk_drop_down_new_from_strings((const char *[]){
      "elf2hunk", "hunk_vlink", "single_asm", "elf2bin", "vasm_bin", NULL });
  self->entry_entry_sym = make_entry("_start");
  self->entry_load_addr = make_entry("$00000000");
  self->entry_sources = make_entry("src/main.s");
  self->entry_c_includes = make_entry("include, /opt/amiga/m68k-amiga-elf/sys-include");
  self->entry_asm_includes = make_entry("include, /opt/amiga/ndk/Include_i");
  self->entry_defines = make_entry("NDEBUG");
  gtk_box_append(GTK_BOX(tform), make_form_row("Target Name", self->entry_target_name));
  gtk_box_append(GTK_BOX(tform), make_form_row("Artifact / Boot", self->dd_target_kind));
  gtk_box_append(GTK_BOX(tform), make_form_row("CPU", self->entry_target_cpu));
  gtk_box_append(GTK_BOX(tform), make_form_row("Toolchain", self->dd_toolchain));
  gtk_box_append(GTK_BOX(tform), make_form_row("Entry Symbol", self->entry_entry_sym));
  gtk_box_append(GTK_BOX(tform), make_form_row("Load Addr (.bin)", self->entry_load_addr));
  gtk_box_append(GTK_BOX(tform), make_form_row("Sources", self->entry_sources));
  gtk_box_append(GTK_BOX(tform), make_form_row("C Includes (.h)", self->entry_c_includes));
  gtk_box_append(GTK_BOX(tform), make_form_row("ASM Includes (.i)", self->entry_asm_includes));
  gtk_box_append(GTK_BOX(tform), make_form_row("Defines", self->entry_defines));
  gtk_box_append(GTK_BOX(tsplit), tform);
  gtk_box_append(GTK_BOX(p_t), tsplit);
  gtk_stack_add_named(GTK_STACK(self->form_stack), p_t, "targets");

  /* Page 2: Editable Profiles */
  GtkWidget *p_p = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_top(p_p, 10); gtk_widget_set_margin_start(p_p, 12); gtk_widget_set_margin_end(p_p, 12);
  gtk_box_append(GTK_BOX(p_p), build_master_pane(&self->profiles_list, "+ CLONE",
      G_CALLBACK(on_prof_clone), G_CALLBACK(on_prof_del), G_CALLBACK(on_prof_row_sel), self));
  GtkWidget *pform = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_hexpand(pform, TRUE);
  self->entry_prof_id = make_entry("a500_ocs");
  self->entry_prof_model = make_entry("A500");
  self->entry_prof_cpu = make_entry("68000");
  self->entry_prof_chipset = make_entry("OCS");
  self->entry_prof_mem = make_entry("512,512,0");
  self->entry_prof_kick = make_entry("roms/kick13.rom");
  self->chk_prof_ntsc = gtk_check_button_new_with_label("NTSC (262 lines)");
  self->chk_prof_ce = gtk_check_button_new_with_label("Cycle-Exact");
  gtk_box_append(GTK_BOX(pform), make_form_row("Profile ID", self->entry_prof_id));
  gtk_box_append(GTK_BOX(pform), make_form_row("Model", self->entry_prof_model));
  gtk_box_append(GTK_BOX(pform), make_form_row("CPU", self->entry_prof_cpu));
  gtk_box_append(GTK_BOX(pform), make_form_row("Chipset", self->entry_prof_chipset));
  gtk_box_append(GTK_BOX(pform), make_form_row("Chip/Slow/Fast", self->entry_prof_mem));
  gtk_box_append(GTK_BOX(pform), make_form_row("Kickstart ROM", self->entry_prof_kick));
  gtk_box_append(GTK_BOX(pform), self->chk_prof_ntsc);
  gtk_box_append(GTK_BOX(pform), self->chk_prof_ce);
  gtk_box_append(GTK_BOX(p_p), pform);
  gtk_stack_add_named(GTK_STACK(self->form_stack), p_p, "profiles");

  /* Page 3: Debug, Docs & Build Hooks */
  GtkWidget *p_d = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(p_d, 10); gtk_widget_set_margin_start(p_d, 12); gtk_widget_set_margin_end(p_d, 12);
  self->chk_trap86 = gtk_check_button_new_with_label("Enable Trap 86 guest-output (-s uaeboard=1 on ADF)");
  self->entry_hrm_dir = make_entry("docs/hrm");
  self->entry_copper_sym = make_entry("copper_list");
  self->entry_bpl_sym = make_entry("bpl0");
  self->entry_pre_build = make_entry("Pre-build command");
  self->entry_post_build = make_entry("Post-build command");
  gtk_box_append(GTK_BOX(p_d), self->chk_trap86);
  gtk_box_append(GTK_BOX(p_d), make_form_row("HRM Docs Dir", self->entry_hrm_dir));
  gtk_box_append(GTK_BOX(p_d), make_form_row("Copper Symbol", self->entry_copper_sym));
  gtk_box_append(GTK_BOX(p_d), make_form_row("Bitplane Symbol", self->entry_bpl_sym));
  gtk_box_append(GTK_BOX(p_d), make_form_row("Pre-Build Hook", self->entry_pre_build));
  gtk_box_append(GTK_BOX(p_d), make_form_row("Post-Build Hook", self->entry_post_build));
  gtk_stack_add_named(GTK_STACK(self->form_stack), p_d, "debug");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->tab_btns[0]), TRUE);

  GtkWidget *form_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(form_scroll), self->form_stack);
  gtk_box_append(GTK_BOX(form_outer), form_scroll);
  gtk_stack_add_named(GTK_STACK(self->mode_stack), form_outer, "form");

  GtkWidget *raw_scroll = gtk_scrolled_window_new();
  self->raw_view = gtk_text_view_new();
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->raw_view), TRUE);
  gtk_widget_add_css_class(self->raw_view, "amiga-hex-view");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(raw_scroll), self->raw_view);
  gtk_stack_add_named(GTK_STACK(self->mode_stack), raw_scroll, "raw");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->seg_form), TRUE);

  gtk_box_append(GTK_BOX(self), self->mode_stack);
}

AmigaProjectSetupView *
amiga_project_setup_view_new(AmigaProjectSavedCb on_saved, gpointer user_data)
{
  AmigaProjectSetupView *self = g_object_new(AMIGA_TYPE_PROJECT_SETUP_VIEW, NULL);
  self->on_saved = on_saved; self->user_data = user_data;
  return self;
}

void
amiga_project_setup_view_bind(AmigaProjectSetupView      *self,
                              AmigaProject               *proj,
                              const AmigaToolchainStatus *tc)
{
  g_return_if_fail(AMIGA_IS_PROJECT_SETUP_VIEW(self));
  self->proj = proj;
  self->tc = tc;
  self->sel_target = 0; self->sel_profile = 0;
  gtk_widget_set_visible(self->lbl_status, FALSE);
  if (!proj) return;

  gtk_editable_set_text(GTK_EDITABLE(self->entry_proj_name), proj->name ? proj->name : "");
  gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_build_mode),
                             proj->build_mode == AMIGA_BUILD_EXTERNAL ? 1 : 0);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_build_cmd), proj->build_cmd ? proj->build_cmd : "");
  g_autofree char *sh = join_csv(proj->shared_sources);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_shared_sources), sh);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_pre_build), proj->pre_build_cmd ? proj->pre_build_cmd : "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_post_build), proj->post_build_cmd ? proj->post_build_cmd : "");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(self->chk_trap86), proj->trap86_guest_output);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_hrm_dir), proj->hrm_dir ? proj->hrm_dir : "");

  rebuild_master_lists(self);
  populate_target_fields(self);
  populate_profile_fields(self);

  g_autofree char *toml = amiga_project_serialize_toml(proj);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->raw_view)), toml ? toml : "", -1);
}
