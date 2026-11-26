#include "amiga_project_ui.h"

#include <string.h>

/* ─── 1. Welcome / Start Screen ─── */

typedef struct {
  AmigaActionCb on_new;
  AmigaActionCb on_open;
  AmigaActionCb on_attach;
  AmigaActionCb on_settings;
  gpointer      user_data;
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

GtkWidget *
amiga_welcome_view_new(const AmigaToolchainStatus *tc,
                       AmigaActionCb               on_new,
                       AmigaActionCb               on_open,
                       AmigaActionCb               on_attach,
                       AmigaActionCb               on_settings,
                       gpointer                    user_data)
{
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_add_css_class(scroll, "amiga-welcome-box");
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);

  GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(center, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(center, 760, -1);

  GtkWidget *hero = gtk_label_new("KAJO AMIGA • 68000 / C + ASM OCS–AGA STUDIO");
  gtk_widget_add_css_class(hero, "settings-header-title");
  gtk_label_set_xalign(GTK_LABEL(hero), 0.0f);

  GtkWidget *sub = gtk_label_new(
      "Self-contained C17 IDE with Option D build DAG, in-process ADFlib packager, and stock Amiberry >= 8.5.0 GDB RSP.");
  gtk_label_set_wrap(GTK_LABEL(sub), TRUE);
  gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);

  gtk_box_append(GTK_BOX(center), hero);
  gtk_box_append(GTK_BOX(center), sub);

  GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_box_set_homogeneous(GTK_BOX(cols), TRUE);

  /* Actions card */
  GtkWidget *act_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class(act_card, "amiga-welcome-card");
  GtkWidget *act_hdr = gtk_label_new("START WORKFLOW");
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

  WelcomeCtx *ctx = g_new0(WelcomeCtx, 1);
  ctx->on_new = on_new;
  ctx->on_open = on_open;
  ctx->on_attach = on_attach;
  ctx->on_settings = on_settings;
  ctx->user_data = user_data;
  g_object_set_data_full(G_OBJECT(scroll), "welcome-ctx", ctx, g_free);

  g_signal_connect(b_new, "clicked", G_CALLBACK(on_w_new), ctx);
  g_signal_connect(b_open, "clicked", G_CALLBACK(on_w_open), ctx);
  g_signal_connect(b_attach, "clicked", G_CALLBACK(on_w_attach), ctx);
  g_signal_connect(b_set, "clicked", G_CALLBACK(on_w_settings), ctx);

  gtk_box_append(GTK_BOX(act_card), act_hdr);
  gtk_box_append(GTK_BOX(act_card), b_new);
  gtk_box_append(GTK_BOX(act_card), b_open);
  gtk_box_append(GTK_BOX(act_card), b_attach);
  gtk_box_append(GTK_BOX(act_card), b_set);

  /* Toolchain readiness card */
  GtkWidget *tc_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(tc_card, "amiga-welcome-card");
  GtkWidget *tc_hdr = gtk_label_new("TOOLCHAIN & ADFLIB READINESS");
  gtk_label_set_xalign(GTK_LABEL(tc_hdr), 0.0f);
  gtk_widget_add_css_class(tc_hdr, "amiga-disasm-header");
  gtk_box_append(GTK_BOX(tc_card), tc_hdr);

  gtk_box_append(GTK_BOX(tc_card), make_tool_badge_row("amiberry (>= 8.5.0)", tc ? tc->amiberry : NULL));
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

static void
on_wiz_cancel(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  WizardCtx *w = ud;
  gtk_window_destroy(w->modal);
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

static void
on_wiz_browse(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  WizardCtx *w = ud;
  g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Select Parent Folder");
  gtk_file_dialog_select_folder(dlg, w->modal, NULL, on_wiz_browse_done, w);
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
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->tpl_btns[i]))) {
      tpl = (AmigaTemplateKind)i;
      break;
    }
  }
  static const char *const profs[] = { "a500_ocs", "a500p_ecs", "a1200_aga", "a1200_fast" };
  guint pi = gtk_drop_down_get_selected(GTK_DROP_DOWN(w->dd_profile));
  const char *prof_id = (pi < G_N_ELEMENTS(profs)) ? profs[pi] : "a500_ocs";

  guint ki = gtk_drop_down_get_selected(GTK_DROP_DOWN(w->dd_pkg));
  AmigaTargetKind pkg = (ki == 1) ? AMIGA_TARGET_ADF_DOS : (ki == 2) ? AMIGA_TARGET_ADF_RAW : AMIGA_TARGET_OS_EXE;

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
                             const AmigaToolchainStatus *tc G_GNUC_UNUSED,
                             AmigaDirSelectedCb          on_created,
                             gpointer                    user_data)
{
  GtkWindow *win = GTK_WINDOW(gtk_window_new());
  gtk_window_set_decorated(win, FALSE);
  gtk_window_set_modal(win, parent != NULL);
  if (parent) gtk_window_set_transient_for(win, parent);
  gtk_window_set_default_size(win, 620, 460);
  gtk_widget_add_css_class(GTK_WIDGET(win), "shell-amiga-modal");

  WizardCtx *w = g_new0(WizardCtx, 1);
  w->modal = win;
  w->on_created = on_created;
  w->user_data = user_data;
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

  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(body, 16);
  gtk_widget_set_margin_bottom(body, 16);
  gtk_widget_set_margin_start(body, 16);
  gtk_widget_set_margin_end(body, 16);

  GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(name_row, GTK_ALIGN_CENTER);
  GtkWidget *ln = gtk_label_new("Name:");
  gtk_widget_set_size_request(ln, 96, -1);
  gtk_label_set_xalign(GTK_LABEL(ln), 0.0f);
  w->entry_name = gtk_entry_new();
  gtk_widget_add_css_class(w->entry_name, "amiga-entry");
  gtk_editable_set_text(GTK_EDITABLE(w->entry_name), "CopperDemo");
  gtk_widget_set_hexpand(w->entry_name, TRUE);
  gtk_box_append(GTK_BOX(name_row), ln);
  gtk_box_append(GTK_BOX(name_row), w->entry_name);
  gtk_box_append(GTK_BOX(body), name_row);

  GtkWidget *dir_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(dir_row, GTK_ALIGN_CENTER);
  GtkWidget *ld = gtk_label_new("Location:");
  gtk_widget_set_size_request(ld, 96, -1);
  gtk_label_set_xalign(GTK_LABEL(ld), 0.0f);
  w->entry_dir = gtk_entry_new();
  gtk_widget_add_css_class(w->entry_dir, "amiga-entry");
  gtk_editable_set_text(GTK_EDITABLE(w->entry_dir), g_get_home_dir());
  gtk_widget_set_hexpand(w->entry_dir, TRUE);
  GtkWidget *btn_br = gtk_button_new_with_label("BROWSE…");
  gtk_widget_add_css_class(btn_br, "ide-toolbar-btn");
  g_signal_connect(btn_br, "clicked", G_CALLBACK(on_wiz_browse), w);
  gtk_box_append(GTK_BOX(dir_row), ld);
  gtk_box_append(GTK_BOX(dir_row), w->entry_dir);
  gtk_box_append(GTK_BOX(dir_row), btn_br);
  gtk_box_append(GTK_BOX(body), dir_row);

  static const char *const tpl_titles[4] = {
    "68k ASM AmigaDOS (os_exe)",
    "Bare-Metal OCS/ECS Demo (os_exe)",
    "Bootable Floppy Disk (adflib .adf)",
    "Mixed C + 68k ASM (gcc + elf2hunk)",
  };
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
  for (int i = 0; i < 4; i++) {
    w->tpl_btns[i] = gtk_toggle_button_new_with_label(tpl_titles[i]);
    gtk_widget_add_css_class(w->tpl_btns[i], "amiga-template-card");
    if (i > 0) {
      gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(w->tpl_btns[i]),
                                  GTK_TOGGLE_BUTTON(w->tpl_btns[0]));
    }
    gtk_grid_attach(GTK_GRID(grid), w->tpl_btns[i], i % 2, i / 2, 1, 1);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->tpl_btns[0]), TRUE);
  gtk_box_append(GTK_BOX(body), grid);

  GtkWidget *dd_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  const char *profs[] = { "A500 OCS (68000)", "A500+ ECS", "A1200 AGA (68020)", "A1200 + Fast", NULL };
  w->dd_profile = gtk_drop_down_new_from_strings(profs);
  const char *pkgs[] = { "os_exe (DH0:)", "adf_dos (OFS .adf)", "adf_raw (Bootblock)", NULL };
  w->dd_pkg = gtk_drop_down_new_from_strings(pkgs);
  gtk_widget_set_hexpand(w->dd_profile, TRUE);
  gtk_widget_set_hexpand(w->dd_pkg, TRUE);
  gtk_box_append(GTK_BOX(dd_row), w->dd_profile);
  gtk_box_append(GTK_BOX(dd_row), w->dd_pkg);
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
  gtk_widget_add_css_class(b_create, "ide-toolbar-btn");
  gtk_widget_add_css_class(b_create, "accent-btn");
  g_signal_connect(b_create, "clicked", G_CALLBACK(on_wiz_create), w);
  gtk_box_append(GTK_BOX(foot), b_cancel);
  gtk_box_append(GTK_BOX(foot), b_create);
  gtk_box_append(GTK_BOX(body), foot);

  gtk_box_append(GTK_BOX(outer), body);
  gtk_window_set_child(win, outer);
  gtk_window_present(win);
}

/* ─── 3. Center-Pane [⚙ kajo-amiga.toml] Setup View ─── */

struct _AmigaProjectSetupView {
  GtkBox parent_instance;

  AmigaProject        *proj; /* borrowed */
  AmigaProjectSavedCb  on_saved;
  gpointer             user_data;

  GtkWidget *seg_form;
  GtkWidget *seg_raw;
  GtkWidget *mode_stack;
  GtkWidget *form_stack;
  GtkWidget *tab_btns[4];

  /* Targets & Packaging */
  GtkWidget *entry_proj_name;
  GtkWidget *dd_build_mode;
  GtkWidget *entry_build_cmd;
  GtkWidget *dd_target_kind;
  GtkWidget *entry_target_cpu;
  GtkWidget *dd_toolchain;
  GtkWidget *entry_sources;

  /* Hardware Profiles */
  GtkWidget *lbl_profile_info;

  /* Assets & Hooks */
  GtkWidget *entry_pre_build;
  GtkWidget *entry_post_build;

  /* Debug & Symbols */
  GtkWidget *chk_trap86;
  GtkWidget *entry_hrm_dir;
  GtkWidget *entry_copper_sym;
  GtkWidget *entry_bpl_sym;

  /* Raw TOML view */
  GtkWidget *raw_view;
  GtkWidget *lbl_status;
};

G_DEFINE_FINAL_TYPE(AmigaProjectSetupView, amiga_project_setup_view, GTK_TYPE_BOX)

static void
amiga_project_setup_view_class_init(AmigaProjectSetupViewClass *klass G_GNUC_UNUSED)
{
}

static void
on_setup_mode_toggled(GtkToggleButton *tb, gpointer ud)
{
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->mode_stack || !gtk_toggle_button_get_active(tb)) return;
  bool is_raw = (GTK_WIDGET(tb) == self->seg_raw);
  if (is_raw && self->proj) {
    g_autofree char *toml = amiga_project_serialize_toml(self->proj);
    GtkTextBuffer *tb_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->raw_view));
    gtk_text_buffer_set_text(tb_buf, toml ? toml : "", -1);
  }
  gtk_stack_set_visible_child_name(GTK_STACK(self->mode_stack), is_raw ? "raw" : "form");
}

static void
on_subtab_clicked(GtkToggleButton *tb, gpointer ud)
{
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->form_stack || !gtk_toggle_button_get_active(tb)) return;
  static const char *const pages[4] = { "targets", "profiles", "assets", "debug" };
  for (int i = 0; i < 4; i++) {
    if (GTK_WIDGET(tb) == self->tab_btns[i]) {
      gtk_stack_set_visible_child_name(GTK_STACK(self->form_stack), pages[i]);
      break;
    }
  }
}

static void
apply_form_to_project(AmigaProjectSetupView *self)
{
  AmigaProject *p = self->proj;
  if (!p) return;
  g_free(p->name);
  p->name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_proj_name)));
  p->build_mode = (gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_build_mode)) == 1)
                      ? AMIGA_BUILD_EXTERNAL : AMIGA_BUILD_NATIVE;
  const char *bc = gtk_editable_get_text(GTK_EDITABLE(self->entry_build_cmd));
  g_free(p->build_cmd);
  p->build_cmd = (bc && bc[0]) ? g_strdup(bc) : NULL;

  const char *pre = gtk_editable_get_text(GTK_EDITABLE(self->entry_pre_build));
  g_free(p->pre_build_cmd);
  p->pre_build_cmd = (pre && pre[0]) ? g_strdup(pre) : NULL;

  const char *post = gtk_editable_get_text(GTK_EDITABLE(self->entry_post_build));
  g_free(p->post_build_cmd);
  p->post_build_cmd = (post && post[0]) ? g_strdup(post) : NULL;

  p->trap86_guest_output = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->chk_trap86));
  const char *hrm = gtk_editable_get_text(GTK_EDITABLE(self->entry_hrm_dir));
  g_free(p->hrm_dir);
  p->hrm_dir = (hrm && hrm[0]) ? g_strdup(hrm) : NULL;

  if (p->targets && p->targets->len > 0) {
    AmigaTarget *t = g_ptr_array_index(p->targets, 0);
    t->kind = (AmigaTargetKind)gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_target_kind));
    t->toolchain = (AmigaToolchainKind)gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_toolchain));
    g_free(t->cpu);
    t->cpu = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_target_cpu)));

    g_ptr_array_set_size(t->sources, 0);
    const char *srcs = gtk_editable_get_text(GTK_EDITABLE(self->entry_sources));
    if (srcs && srcs[0]) {
      g_auto(GStrv) parts = g_strsplit(srcs, ",", -1);
      for (int i = 0; parts[i] != NULL; i++) {
        char *item = g_strstrip(parts[i]);
        if (item[0]) g_ptr_array_add(t->sources, g_strdup(item));
      }
    }
    const char *csym = gtk_editable_get_text(GTK_EDITABLE(self->entry_copper_sym));
    g_free(t->copper_symbol);
    t->copper_symbol = (csym && csym[0]) ? g_strdup(csym) : NULL;

    const char *bsym = gtk_editable_get_text(GTK_EDITABLE(self->entry_bpl_sym));
    g_free(t->bitplane_symbol);
    t->bitplane_symbol = (bsym && bsym[0]) ? g_strdup(bsym) : NULL;
  }
}

static void
on_setup_save_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaProjectSetupView *self = AMIGA_PROJECT_SETUP_VIEW(ud);
  if (!self->proj) return;

  g_autoptr(GError) err = NULL;
  if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->mode_stack)), "raw") == 0) {
    GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->raw_view));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(tb, &s, &e);
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
  g_autofree char *mk = amiga_project_export_makefile(self->proj, NULL);
  g_autofree char *path = g_build_filename(self->proj->root_dir, "Makefile.kajo", NULL);
  g_autoptr(GError) err = NULL;
  if (mk && g_file_set_contents(path, mk, -1, &err)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_status), "✓ Exported Makefile.kajo");
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_status), err ? err->message : "Export failed");
  }
  gtk_widget_set_visible(self->lbl_status, TRUE);
}

static GtkWidget *
make_form_row(const char *label_text, GtkWidget *ctrl)
{
  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_valign(hb, GTK_ALIGN_CENTER);
  GtkWidget *lbl = gtk_label_new(label_text);
  gtk_widget_set_size_request(lbl, 160, -1);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(ctrl, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(ctrl, TRUE);
  gtk_box_append(GTK_BOX(hb), lbl);
  gtk_box_append(GTK_BOX(hb), ctrl);
  return hb;
}

static void
amiga_project_setup_view_init(AmigaProjectSetupView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);

  GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(top_bar, "amiga-setup-tab-bar");
  gtk_widget_set_valign(top_bar, GTK_ALIGN_CENTER);

  GtkWidget *title = gtk_label_new("⚙ kajo-amiga.toml");
  gtk_widget_add_css_class(title, "amiga-disasm-header");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);

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

  GtkWidget *btn_save = gtk_button_new_with_label("💾 SAVE");
  gtk_widget_add_css_class(btn_save, "ide-toolbar-btn");
  gtk_widget_add_css_class(btn_save, "accent-btn");
  g_signal_connect(btn_save, "clicked", G_CALLBACK(on_setup_save_clicked), self);

  self->lbl_status = gtk_label_new("");
  gtk_widget_add_css_class(self->lbl_status, "amiga-badge-snapshot");
  gtk_widget_set_visible(self->lbl_status, FALSE);

  gtk_box_append(GTK_BOX(top_bar), title);
  gtk_box_append(GTK_BOX(top_bar), self->lbl_status);
  gtk_box_append(GTK_BOX(top_bar), self->seg_form);
  gtk_box_append(GTK_BOX(top_bar), self->seg_raw);
  gtk_box_append(GTK_BOX(top_bar), btn_mk);
  gtk_box_append(GTK_BOX(top_bar), btn_save);
  gtk_box_append(GTK_BOX(self), top_bar);

  self->mode_stack = gtk_stack_new();
  gtk_widget_set_hexpand(self->mode_stack, TRUE);
  gtk_widget_set_vexpand(self->mode_stack, TRUE);

  /* FORM view with 4 sub-tabs */
  GtkWidget *form_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(tab_bar, "amiga-setup-tab-bar");
  static const char *const tnames[4] = {
    "Targets & Packaging", "Hardware Profiles", "Assets & Hooks", "Debug & Symbols"
  };
  for (int i = 0; i < 4; i++) {
    self->tab_btns[i] = gtk_toggle_button_new_with_label(tnames[i]);
    gtk_widget_add_css_class(self->tab_btns[i], "amiga-seg-btn");
    if (i > 0) {
      gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(self->tab_btns[i]),
                                  GTK_TOGGLE_BUTTON(self->tab_btns[0]));
    }
    g_signal_connect(self->tab_btns[i], "toggled", G_CALLBACK(on_subtab_clicked), self);
    gtk_box_append(GTK_BOX(tab_bar), self->tab_btns[i]);
  }
  gtk_box_append(GTK_BOX(form_outer), tab_bar);

  self->form_stack = gtk_stack_new();
  gtk_widget_set_hexpand(self->form_stack, TRUE);
  gtk_widget_set_vexpand(self->form_stack, TRUE);

  /* Page 1: Targets & Packaging */
  GtkWidget *p_targets = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(p_targets, 14);
  gtk_widget_set_margin_start(p_targets, 14);
  gtk_widget_set_margin_end(p_targets, 14);
  self->entry_proj_name = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_proj_name, "amiga-entry");
  const char *bmodes[] = { "native (Option D DAG)", "external (build_cmd)", NULL };
  self->dd_build_mode = gtk_drop_down_new_from_strings(bmodes);
  self->entry_build_cmd = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_build_cmd, "amiga-entry");
  const char *kinds[] = { "os_exe (DH0:)", "adf_dos (OFS/FFS via adflib)", "adf_raw (Bootblock)", NULL };
  self->dd_target_kind = gtk_drop_down_new_from_strings(kinds);
  self->entry_target_cpu = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_target_cpu, "amiga-entry");
  const char *tcs[] = { "elf2hunk (GCC+VASM)", "hunk_vlink (VBCC/VASM)", "single_asm (vasm -Fhunkexe)", NULL };
  self->dd_toolchain = gtk_drop_down_new_from_strings(tcs);
  self->entry_sources = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_sources, "amiga-entry");

  gtk_box_append(GTK_BOX(p_targets), make_form_row("Project Name", self->entry_proj_name));
  gtk_box_append(GTK_BOX(p_targets), make_form_row("Build Mode", self->dd_build_mode));
  gtk_box_append(GTK_BOX(p_targets), make_form_row("External Command", self->entry_build_cmd));
  gtk_box_append(GTK_BOX(p_targets), make_form_row("Target Packaging", self->dd_target_kind));
  gtk_box_append(GTK_BOX(p_targets), make_form_row("Target CPU", self->entry_target_cpu));
  gtk_box_append(GTK_BOX(p_targets), make_form_row("Toolchain", self->dd_toolchain));
  gtk_box_append(GTK_BOX(p_targets), make_form_row("Sources (CSV)", self->entry_sources));
  gtk_stack_add_named(GTK_STACK(self->form_stack), p_targets, "targets");

  /* Page 2: Hardware Profiles */
  GtkWidget *p_prof = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(p_prof, 14);
  gtk_widget_set_margin_start(p_prof, 14);
  gtk_widget_set_margin_end(p_prof, 14);
  self->lbl_profile_info = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_profile_info), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(self->lbl_profile_info), TRUE);
  gtk_widget_add_css_class(self->lbl_profile_info, "amiga-section-card");
  gtk_box_append(GTK_BOX(p_prof), self->lbl_profile_info);
  gtk_stack_add_named(GTK_STACK(self->form_stack), p_prof, "profiles");

  /* Page 3: Assets & Hooks */
  GtkWidget *p_assets = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(p_assets, 14);
  gtk_widget_set_margin_start(p_assets, 14);
  gtk_widget_set_margin_end(p_assets, 14);
  self->entry_pre_build = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_pre_build, "amiga-entry");
  self->entry_post_build = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_post_build, "amiga-entry");
  gtk_box_append(GTK_BOX(p_assets), make_form_row("Pre-Build Hook", self->entry_pre_build));
  gtk_box_append(GTK_BOX(p_assets), make_form_row("Post-Build Hook", self->entry_post_build));
  gtk_stack_add_named(GTK_STACK(self->form_stack), p_assets, "assets");

  /* Page 4: Debug & Symbols */
  GtkWidget *p_debug = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(p_debug, 14);
  gtk_widget_set_margin_start(p_debug, 14);
  gtk_widget_set_margin_end(p_debug, 14);
  self->chk_trap86 = gtk_check_button_new_with_label("Enable Trap 86 guest-output (-s uaeboard=1 on ADF)");
  self->entry_hrm_dir = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_hrm_dir, "amiga-entry");
  self->entry_copper_sym = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_copper_sym, "amiga-entry");
  self->entry_bpl_sym = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_bpl_sym, "amiga-entry");
  gtk_box_append(GTK_BOX(p_debug), self->chk_trap86);
  gtk_box_append(GTK_BOX(p_debug), make_form_row("HRM Docs Directory", self->entry_hrm_dir));
  gtk_box_append(GTK_BOX(p_debug), make_form_row("Copper Symbol", self->entry_copper_sym));
  gtk_box_append(GTK_BOX(p_debug), make_form_row("Bitplane Symbol", self->entry_bpl_sym));
  gtk_stack_add_named(GTK_STACK(self->form_stack), p_debug, "debug");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->tab_btns[0]), TRUE);

  GtkWidget *form_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(form_scroll), self->form_stack);
  gtk_box_append(GTK_BOX(form_outer), form_scroll);
  gtk_stack_add_named(GTK_STACK(self->mode_stack), form_outer, "form");

  /* RAW TOML editor */
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
  self->on_saved = on_saved;
  self->user_data = user_data;
  return self;
}

void
amiga_project_setup_view_bind(AmigaProjectSetupView      *self,
                              AmigaProject               *proj,
                              const AmigaToolchainStatus *tc G_GNUC_UNUSED)
{
  g_return_if_fail(AMIGA_IS_PROJECT_SETUP_VIEW(self));
  self->proj = proj;
  gtk_widget_set_visible(self->lbl_status, FALSE);
  if (!proj) return;

  gtk_editable_set_text(GTK_EDITABLE(self->entry_proj_name), proj->name ? proj->name : "");
  gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_build_mode),
                             proj->build_mode == AMIGA_BUILD_EXTERNAL ? 1 : 0);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_build_cmd), proj->build_cmd ? proj->build_cmd : "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_pre_build), proj->pre_build_cmd ? proj->pre_build_cmd : "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_post_build), proj->post_build_cmd ? proj->post_build_cmd : "");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(self->chk_trap86), proj->trap86_guest_output);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_hrm_dir), proj->hrm_dir ? proj->hrm_dir : "");

  if (proj->targets && proj->targets->len > 0) {
    const AmigaTarget *t = g_ptr_array_index(proj->targets, 0);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_target_kind), (guint)t->kind);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_toolchain), (guint)t->toolchain);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_target_cpu), t->cpu ? t->cpu : "68000");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_copper_sym), t->copper_symbol ? t->copper_symbol : "");
    gtk_editable_set_text(GTK_EDITABLE(self->entry_bpl_sym), t->bitplane_symbol ? t->bitplane_symbol : "");

    g_autoptr(GString) csv = g_string_new("");
    for (guint i = 0; t->sources && i < t->sources->len; i++) {
      if (i > 0) g_string_append(csv, ", ");
      g_string_append(csv, (const char *)g_ptr_array_index(t->sources, i));
    }
    gtk_editable_set_text(GTK_EDITABLE(self->entry_sources), csv->str);
  }

  g_autoptr(GString) prof_txt = g_string_new("CONFIGURED HARDWARE PROFILES:\n");
  for (guint i = 0; proj->profiles && i < proj->profiles->len; i++) {
    const AmigaHwProfile *hp = g_ptr_array_index(proj->profiles, i);
    g_string_append_printf(prof_txt, "• %s  [%s · %s · %s · %uK Chip / %uK Slow / %uK Fast · %s]\n",
                           hp->id, hp->model, hp->cpu, hp->chipset,
                           hp->chip_kb, hp->slow_kb, hp->fast_kb,
                           hp->ntsc ? "NTSC" : "PAL");
  }
  gtk_label_set_text(GTK_LABEL(self->lbl_profile_info), prof_txt->str);

  g_autofree char *toml = amiga_project_serialize_toml(proj);
  GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->raw_view));
  gtk_text_buffer_set_text(tb, toml ? toml : "", -1);
}
