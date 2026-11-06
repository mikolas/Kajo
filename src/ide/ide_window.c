#include "ide_window.h"
#include "../compositor/compositor.h"
#include "../config.h"
#include "../theme.h"
#include "ide_agent_ipc.h"
#include "ide_buffer.h"
#include "ide_dap.h"
#include "ide_fsm.h"
#include "ide_gutter.h"
#include "ide_lsp.h"
#include "ide_markdown.h"
#include "ide_picker.h"
#include "ide_search.h"
#include "ide_tree.h"
#include <gdk/gdkkeysyms.h>
#include <gtksourceview/gtksource.h>
#include <string.h>

typedef struct {
  char *path;
  guint line_1based;
  guint col_1based;
} IdeJumpEntry;

struct _KajoIdeWindow {
  GtkWindow parent_instance;

  char            *workspace_root;
  IdeFsm          *fsm;
  GPtrArray       *buffers; /* IdeBuffer* */
  guint            active_buf_idx;

  /* Subsystems */
  IdeTree         *tree;
  IdePicker       *picker;
  IdeSearchDrawer *search_drawer;
  IdeLspManager   *lsp;
  IdeDapView      *dap;
  IdeAgentIpc     *agent_ipc;
  IdeMarkdownView *md_split_view;
  IdeMarkdownView *md_drawer_view;

  /* Top Chrome & Tab Ribbon */
  GtkWidget       *btn_hdr_tree;
  GtkWidget       *btn_hdr_map;
  GtkWidget       *btn_hdr_md;
  GtkWidget       *btn_hdr_help;
  GtkWidget       *hdr_breadcrumb;
  GtkWidget       *ws_badge;
  GtkWidget       *agent_hdr_badge;
  GtkWidget       *tab_ribbon_box;

  /* Main Editor Stage & Resizable GtkPaned Splitters */
  GtkWidget       *vertical_paned;
  GtkWidget       *tree_paned;
  GtkWidget       *editor_md_paned;
  GtkWidget       *tree_container;
  GtkWidget       *source_scroll;
  GtkWidget       *source_view;
  GtkWidget       *source_map;
  KajoIdeGutterRenderer *gutter_renderer;
  GtkWidget       *md_split_container;

  /* Overlays */
  GtkWidget       *chord_hud_box;
  GtkWidget       *chord_hud_title;
  GtkWidget       *chord_hud_grid;
  GtkWidget       *toast_banner;
  GtkWidget       *toast_label;

  /* Bottom Contextual Drawer */
  GtkWidget       *drawer_revealer;
  GtkWidget       *drawer_stack;
  GtkWidget       *drawer_tab_agent;
  GtkWidget       *drawer_tab_dap;
  GtkWidget       *drawer_tab_search;
  GtkWidget       *drawer_tab_md;

  /* Bottom Status Bar */
  GtkWidget       *mode_pill;
  GtkWidget       *chord_badge;
  GtkWidget       *file_status_label;
  GtkWidget       *agent_status_badge;
  GtkWidget       *diag_status_label;
  GtkWidget       *lsp_status_badge;
  GtkWidget       *cursor_pos_label;

  /* Live Settings & Config Hot-Reload */
  GFileMonitor    *config_monitor;
  char            *active_scheme_id;
  char            *active_font_family;
  int              active_font_size_px;
  gboolean         config_minimap_enabled;

  /* In-buffer search bar */
  GtkWidget       *inline_search_bar;
  GtkWidget       *inline_search_entry;
  GString         *search_query;
  char            *yank_register;

  /* Cross-file Jumplist */
  GPtrArray       *jumplist; /* IdeJumpEntry* */
  int              jumplist_idx;
};

G_DEFINE_TYPE(KajoIdeWindow, kajo_ide_window, GTK_TYPE_WINDOW)

static void jump_entry_free(gpointer data) {
  IdeJumpEntry *e = data;
  if (!e) return;
  g_free(e->path);
  g_free(e);
}

static IdeBuffer *get_active_buffer(KajoIdeWindow *self) {
  if (!self || !self->buffers || self->buffers->len == 0) return NULL;
  if (self->active_buf_idx >= self->buffers->len)
    self->active_buf_idx = self->buffers->len - 1;
  return g_ptr_array_index(self->buffers, self->active_buf_idx);
}

static void get_cursor_line_col_1based(KajoIdeWindow *self,
                                       guint         *out_line,
                                       guint         *out_col) {
  if (out_line) *out_line = 1;
  if (out_col) *out_col = 1;
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_mark(tbuf, &iter, gtk_text_buffer_get_insert(tbuf));
  if (out_line) *out_line = (guint)gtk_text_iter_get_line(&iter) + 1;
  if (out_col) *out_col = (guint)gtk_text_iter_get_line_offset(&iter) + 1;
}

static void record_jumplist_Here(KajoIdeWindow *self) {
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf || !ide_buffer_get_file_path(buf)) return;
  guint line = 1, col = 1;
  get_cursor_line_col_1based(self, &line, &col);

  if (self->jumplist->len > 0 && self->jumplist_idx >= 0 &&
      self->jumplist_idx < (int)self->jumplist->len) {
    IdeJumpEntry *cur = g_ptr_array_index(self->jumplist, self->jumplist_idx);
    if (g_strcmp0(cur->path, ide_buffer_get_file_path(buf)) == 0 &&
        cur->line_1based == line) {
      return;
    }
  }

  IdeJumpEntry *e = g_new0(IdeJumpEntry, 1);
  e->path = g_strdup(ide_buffer_get_file_path(buf));
  e->line_1based = line;
  e->col_1based = col;
  g_ptr_array_add(self->jumplist, e);
  self->jumplist_idx = (int)self->jumplist->len - 1;
}

static void show_toast(KajoIdeWindow *self, const char *msg) {
  if (!self || !self->toast_banner || !msg || !*msg) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg);
  gtk_widget_set_visible(self->toast_banner, TRUE);
}

static void sync_markdown_views(KajoIdeWindow *self) {
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;
  gboolean split_vis = gtk_widget_get_visible(self->md_split_container);
  gboolean drawer_vis =
      gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer)) &&
      g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->drawer_stack)),
                "markdown") == 0;
  if (!split_vis && !drawer_vis) return;

  const char *doc_path = ide_buffer_get_file_path(buf);
  ide_markdown_view_set_context(
      self->md_split_view, doc_path, self->workspace_root);
  ide_markdown_view_set_context(
      self->md_drawer_view, doc_path, self->workspace_root);

  g_autofree char *text = ide_buffer_get_text(buf);
  if (split_vis) ide_markdown_view_render(self->md_split_view, text);
  if (drawer_vis) ide_markdown_view_render(self->md_drawer_view, text);
}

static char *format_breadcrumb_for_path(const char *workspace_root,
                                        const char *file_path,
                                        const char *fallback_title) {
  if (!file_path || !*file_path)
    return g_strdup(fallback_title ? fallback_title : "[No Name]");

  const char *rel = file_path;
  if (workspace_root && *workspace_root &&
      g_str_has_prefix(file_path, workspace_root)) {
    rel = file_path + strlen(workspace_root);
    while (*rel == '/') rel++;
  }
  if (!*rel) return g_path_get_basename(file_path);

  g_auto(GStrv) parts = g_strsplit(rel, "/", -1);
  return g_strjoinv(" ▸ ", parts);
}

static void apply_ide_config(KajoIdeWindow *self) {
  if (!self) return;
  ShellConfig *cfg = shell_config_load(NULL);
  if (!cfg) return;

  shell_theme_sync_from_config(cfg);

  g_free(self->active_scheme_id);
  self->active_scheme_id = g_strdup(
      (cfg->ide_color_scheme && *cfg->ide_color_scheme)
          ? cfg->ide_color_scheme
          : "kajo-metro-dark");

  g_free(self->active_font_family);
  self->active_font_family = g_strdup(
      (cfg->ide_font_family && *cfg->ide_font_family)
          ? cfg->ide_font_family
          : "Fira Code");

  self->active_font_size_px = CLAMP(cfg->ide_font_size_px, 10, 24);
  self->config_minimap_enabled = cfg->ide_show_minimap;

  if (self->source_view) {
    gtk_source_view_set_tab_width(
        GTK_SOURCE_VIEW(self->source_view),
        (guint)CLAMP(cfg->ide_tab_width, 2, 8));
    gtk_source_view_set_highlight_current_line(
        GTK_SOURCE_VIEW(self->source_view), cfg->ide_highlight_current_line);
  }

  if (self->gutter_renderer) {
    int gutter_pt = MAX(8, (self->active_font_size_px - 2) * 3 / 4);
    kajo_ide_gutter_renderer_set_font(
        self->gutter_renderer, self->active_font_family, gutter_pt);
    gtk_widget_set_visible(GTK_WIDGET(self->gutter_renderer),
                           cfg->ide_show_line_numbers);
  }

  if (self->source_map && self->md_split_container &&
      !gtk_widget_get_visible(self->md_split_container)) {
    gtk_widget_set_visible(self->source_map, self->config_minimap_enabled);
  }

  if (self->buffers) {
    for (guint i = 0; i < self->buffers->len; i++) {
      IdeBuffer *b = g_ptr_array_index(self->buffers, i);
      ide_buffer_set_style_scheme(b, self->active_scheme_id);
    }
  }

  if (self->source_view) gtk_widget_queue_draw(self->source_view);
  shell_config_destroy(cfg);
}

static void zoom_editor_font(KajoIdeWindow *self, int delta_or_zero) {
  if (!self) return;
  ShellConfig *cfg = shell_config_load(NULL);
  if (!cfg) return;
  if (delta_or_zero == 0) {
    cfg->ide_font_size_px = 13;
  } else {
    cfg->ide_font_size_px = CLAMP(cfg->ide_font_size_px + delta_or_zero, 10, 24);
  }
  int new_px = cfg->ide_font_size_px;
  shell_config_save(cfg, NULL);
  shell_theme_sync_from_config(cfg);
  shell_config_destroy(cfg);

  apply_ide_config(self);
  g_autofree char *msg = g_strdup_printf("🔍 Editor Font: %dpx", new_px);
  show_toast(self, msg);
}

static void on_config_file_changed(GFileMonitor      *monitor,
                                   GFile             *file,
                                   GFile             *other_file,
                                   GFileMonitorEvent  event_type,
                                   gpointer           user_data) {
  (void)monitor;
  (void)file;
  (void)other_file;
  if (event_type == G_FILE_MONITOR_EVENT_CHANGED ||
      event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
      event_type == G_FILE_MONITOR_EVENT_CREATED) {
    apply_ide_config(KAJO_IDE_WINDOW(user_data));
  }
}

static void update_status_bar_and_hud(KajoIdeWindow *self) {
  if (!self || !self->mode_pill) return;

  IdeState st = ide_fsm_get_state(self->fsm);
  gtk_label_set_text(GTK_LABEL(self->mode_pill), ide_state_to_string(st));

  const char *mode_classes[] = {
    "mode-normal", "mode-insert", "mode-select", "mode-search", "mode-leader", NULL
  };
  for (int i = 0; mode_classes[i]; i++)
    gtk_widget_remove_css_class(self->mode_pill, mode_classes[i]);
  gtk_widget_add_css_class(self->mode_pill, ide_state_get_css_class(st));

  /* Sync Header Toggle Button Active States */
  if (self->btn_hdr_tree && self->tree_container) {
    if (gtk_widget_get_visible(self->tree_container))
      gtk_widget_add_css_class(self->btn_hdr_tree, "active");
    else
      gtk_widget_remove_css_class(self->btn_hdr_tree, "active");
  }
  if (self->btn_hdr_map && self->source_map) {
    if (gtk_widget_get_visible(self->source_map))
      gtk_widget_add_css_class(self->btn_hdr_map, "active");
    else
      gtk_widget_remove_css_class(self->btn_hdr_map, "active");
  }
  if (self->btn_hdr_md && self->md_split_container) {
    if (gtk_widget_get_visible(self->md_split_container))
      gtk_widget_add_css_class(self->btn_hdr_md, "active");
    else
      gtk_widget_remove_css_class(self->btn_hdr_md, "active");
  }
  if (self->btn_hdr_help && self->picker) {
    if (ide_picker_is_visible(self->picker) &&
        ide_picker_get_mode(self->picker) == IDE_PICKER_HELP)
      gtk_widget_add_css_class(self->btn_hdr_help, "active");
    else
      gtk_widget_remove_css_class(self->btn_hdr_help, "active");
  }

  /* Update Git Branch & Status Summary Badge in Header */
  if (self->ws_badge && self->tree) {
    g_autofree char *git_sum = ide_tree_format_git_summary(self->tree);
    gtk_label_set_text(GTK_LABEL(self->ws_badge), git_sum);
  }

  const char *chord = ide_fsm_get_pending_chord(self->fsm);
  if (st == IDE_STATE_LEADER) {
    g_autofree char *badge =
        (chord && *chord)
            ? g_strdup_printf("Space → %s _", chord)
            : g_strdup("Space → _");
    gtk_label_set_text(GTK_LABEL(self->chord_badge), badge);
    gtk_widget_set_visible(self->chord_badge, TRUE);
    g_autofree char *hud_hdr = g_strdup_printf("LEADER CHORD (%s)", badge);
    gtk_label_set_text(GTK_LABEL(self->chord_hud_title), hud_hdr);
    if (self->chord_hud_grid) {
      if (g_strcmp0(chord, "m") == 0) {
        gtk_label_set_text(
            GTK_LABEL(self->chord_hud_grid),
            "m p  Toggle Markdown GUI Split      m m  Mermaid Vector Drawer");
      } else if (g_strcmp0(chord, "s") == 0) {
        gtk_label_set_text(
            GTK_LABEL(self->chord_hud_grid),
            "s r  Buffer Search & Replace        s p  Project-Wide Replace");
      } else if (g_strcmp0(chord, "d") == 0) {
        gtk_label_set_text(
            GTK_LABEL(self->chord_hud_grid),
            "d s  Review Agent Diffs   d a  Accept All     d r  Revert All\n"
            "d b  Toggle Breakpoint    d c  Start/Cont     d v  DAP Drawer");
      } else if (g_strcmp0(chord, "c") == 0) {
        gtk_label_set_text(
            GTK_LABEL(self->chord_hud_grid),
            "c a  LSP Code Action      c r  LSP Rename     c h  Clear Search");
      } else if (g_strcmp0(chord, "w") == 0) {
        gtk_label_set_text(
            GTK_LABEL(self->chord_hud_grid),
            "w s  Save Buffer          w a  Save All       w q  Force Close");
      } else {
        gtk_label_set_text(
            GTK_LABEL(self->chord_hud_grid),
            "f / SPC  Find Files      b  Buffers         /  Live Grep\n"
            "e        File Tree       a  Alternate C/H   w  Save File\n"
            "m p      Markdown GUI    s  Search/Replace  d  Agent / Debug\n"
            "c        Code Actions    o  Build Output    q  Close Buffer");
      }
    }
    gtk_widget_set_visible(self->chord_hud_box, TRUE);
  } else if (st == IDE_STATE_MATCH_PENDING) {
    g_autofree char *badge =
        (chord && *chord) ? g_strdup_printf("m → %s _", chord) : g_strdup("m → _");
    gtk_label_set_text(GTK_LABEL(self->chord_badge), badge);
    gtk_widget_set_visible(self->chord_badge, TRUE);
    gtk_label_set_text(GTK_LABEL(self->chord_hud_title), "TEXT OBJECT MATCH (m)");
    if (self->chord_hud_grid) {
      gtk_label_set_text(
          GTK_LABEL(self->chord_hud_grid),
          "i + ( [ { \" ' w   Select Inside Delimiter / Word\n"
          "a + ( [ { \" ' w   Select Around Delimiter / Word");
    }
    gtk_widget_set_visible(self->chord_hud_box, TRUE);
  } else if (chord && *chord) {
    g_autofree char *badge = g_strdup_printf("%s → _", chord);
    gtk_label_set_text(GTK_LABEL(self->chord_badge), badge);
    gtk_widget_set_visible(self->chord_badge, TRUE);
    g_autofree char *hud_hdr = g_strdup_printf("PENDING CHORD (%s)", chord);
    gtk_label_set_text(GTK_LABEL(self->chord_hud_title), hud_hdr);
    if (self->chord_hud_grid) {
      gtk_label_set_text(
          GTK_LABEL(self->chord_hud_grid),
          "g g  Buffer Start     g h  Line Start\n"
          "g l  Line End         g d  Go to Definition");
    }
    gtk_widget_set_visible(self->chord_hud_box, TRUE);
  } else {
    gtk_widget_set_visible(self->chord_badge, FALSE);
    gtk_widget_set_visible(self->chord_hud_box, FALSE);
  }

  gtk_widget_set_visible(self->inline_search_bar, (st == IDE_STATE_SEARCH));
  if (st == IDE_STATE_SEARCH) {
    g_autofree char *s_txt = g_strdup_printf("/%s", self->search_query->str);
    gtk_label_set_text(GTK_LABEL(self->inline_search_entry), s_txt);
  }

  IdeBuffer *buf = get_active_buffer(self);
  if (buf) {
    g_autofree char *crumb = format_breadcrumb_for_path(
        self->workspace_root,
        ide_buffer_get_file_path(buf),
        ide_buffer_get_title(buf));
    if (self->hdr_breadcrumb)
      gtk_label_set_text(GTK_LABEL(self->hdr_breadcrumb), crumb);

    g_autofree char *f_lbl = g_strdup_printf(
        "%s%s",
        ide_buffer_get_title(buf),
        ide_buffer_get_modified(buf) ? " [+]" : "");
    gtk_label_set_text(GTK_LABEL(self->file_status_label), f_lbl);

    const GPtrArray *diags = ide_buffer_get_diagnostics(buf);
    guint errs = 0, warns = 0;
    if (diags) {
      for (guint i = 0; i < diags->len; i++) {
        const IdeDiagnostic *d = g_ptr_array_index(diags, i);
        if (d->severity == IDE_DIAG_ERROR) errs++;
        else if (d->severity == IDE_DIAG_WARNING) warns++;
      }
    }
    g_autofree char *d_lbl = g_strdup_printf("✖ %u  ⚠ %u", errs, warns);
    gtk_label_set_text(GTK_LABEL(self->diag_status_label), d_lbl);
    if (errs > 0)
      gtk_widget_add_css_class(self->diag_status_label, "urgent");
    else
      gtk_widget_remove_css_class(self->diag_status_label, "urgent");

    guint line = 1, col = 1;
    get_cursor_line_col_1based(self, &line, &col);
    g_autofree char *pos_lbl =
        g_strdup_printf("UTF-8 • LF • Ln %u, Col %u", line, col);
    gtk_label_set_text(GTK_LABEL(self->cursor_pos_label), pos_lbl);
  }
}

static void on_tab_btn_clicked(GtkButton *btn, gpointer user_data);
static void on_tab_close_btn_clicked(GtkButton *btn, gpointer user_data);

static void on_buffer_mark_set(GtkTextBuffer     *tbuf,
                               const GtkTextIter *location,
                               GtkTextMark       *mark,
                               gpointer           user_data) {
  (void)location;
  KajoIdeWindow *self = user_data;
  if (!self || !tbuf) return;
  if (mark == gtk_text_buffer_get_insert(tbuf) ||
      mark == gtk_text_buffer_get_selection_bound(tbuf)) {
    update_status_bar_and_hud(self);
  }
}

static void rebuild_tab_ribbon(KajoIdeWindow *self) {
  if (!self || !self->tab_ribbon_box) return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(self->tab_ribbon_box)) != NULL)
    gtk_box_remove(GTK_BOX(self->tab_ribbon_box), child);

  for (guint i = 0; i < self->buffers->len; i++) {
    IdeBuffer *b = g_ptr_array_index(self->buffers, i);
    const char *title = ide_buffer_get_title(b);
    gboolean is_md = g_str_has_suffix(title, ".md") ||
                     g_str_has_suffix(title, ".markdown");

    GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(pill, "ide-tab-pill");
    if (i == self->active_buf_idx)
      gtk_widget_add_css_class(pill, "active");
    gtk_widget_set_valign(pill, GTK_ALIGN_FILL);

    GtkWidget *btn = gtk_button_new();
    gtk_widget_set_focusable(btn, FALSE);
    gtk_widget_add_css_class(btn, "ide-tab-main-btn");
    gtk_widget_set_valign(btn, GTK_ALIGN_FILL);

    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(inner, GTK_ALIGN_CENTER);

    g_autofree char *idx_txt = g_strdup_printf("[%u]", i + 1);
    GtkWidget *idx_lbl = gtk_label_new(idx_txt);
    gtk_widget_add_css_class(idx_lbl, "ide-tab-idx");
    gtk_widget_set_valign(idx_lbl, GTK_ALIGN_CENTER);

    GtkWidget *icon_lbl = gtk_label_new(is_md ? "📖" : "📄");
    gtk_widget_add_css_class(icon_lbl, "ide-tab-icon");
    gtk_widget_set_valign(icon_lbl, GTK_ALIGN_CENTER);

    GtkWidget *name_lbl = gtk_label_new(title);
    gtk_widget_add_css_class(name_lbl, "ide-tab-title");
    gtk_widget_set_valign(name_lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(inner), idx_lbl);
    gtk_box_append(GTK_BOX(inner), icon_lbl);
    gtk_box_append(GTK_BOX(inner), name_lbl);

    if (ide_buffer_get_agent_snapshot(b) != NULL) {
      GtkWidget *ag_lbl = gtk_label_new("⚡");
      gtk_widget_add_css_class(ag_lbl, "ide-tab-agent");
      gtk_widget_set_valign(ag_lbl, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(inner), ag_lbl);
    } else if (ide_buffer_get_modified(b)) {
      GtkWidget *mod_lbl = gtk_label_new("[+]");
      gtk_widget_add_css_class(mod_lbl, "ide-tab-mod");
      gtk_widget_set_valign(mod_lbl, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(inner), mod_lbl);
    }

    gtk_button_set_child(GTK_BUTTON(btn), inner);
    g_object_set_data(G_OBJECT(btn), "tab-index", GUINT_TO_POINTER(i));
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_btn_clicked), self);

    GtkWidget *close_btn = gtk_button_new_with_label("×");
    gtk_widget_set_focusable(close_btn, FALSE);
    gtk_widget_add_css_class(close_btn, "ide-tab-close");
    gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(close_btn), "tab-index", GUINT_TO_POINTER(i));
    g_signal_connect(
        close_btn, "clicked", G_CALLBACK(on_tab_close_btn_clicked), self);

    gtk_box_append(GTK_BOX(pill), btn);
    gtk_box_append(GTK_BOX(pill), close_btn);
    gtk_box_append(GTK_BOX(self->tab_ribbon_box), pill);
  }
}

static void bind_active_buffer_to_view(KajoIdeWindow *self) {
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;

  if (self->active_scheme_id && *self->active_scheme_id)
    ide_buffer_set_style_scheme(buf, self->active_scheme_id);

  GtkSourceBuffer *sbuf = ide_buffer_get_source_buffer(buf);
  g_signal_handlers_disconnect_by_func(
      sbuf, G_CALLBACK(on_buffer_mark_set), self);
  g_signal_connect(
      sbuf, "mark-set", G_CALLBACK(on_buffer_mark_set), self);

  gtk_text_view_set_buffer(GTK_TEXT_VIEW(self->source_view),
                           GTK_TEXT_BUFFER(sbuf));
  kajo_ide_gutter_renderer_set_buffer(self->gutter_renderer, buf);
  ide_lsp_manager_notify_open(self->lsp, buf);

  rebuild_tab_ribbon(self);
  sync_markdown_views(self);
  update_status_bar_and_hud(self);
  gtk_widget_grab_focus(self->source_view);
  gtk_widget_queue_draw(self->source_view);
}

static void on_tab_btn_clicked(GtkButton *btn, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "tab-index"));
  if (idx < self->buffers->len) {
    record_jumplist_Here(self);
    self->active_buf_idx = idx;
    bind_active_buffer_to_view(self);
  }
}

static void on_tab_close_btn_clicked(GtkButton *btn, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "tab-index"));
  if (self->buffers->len <= 1) {
    IdeBuffer *only = g_ptr_array_index(self->buffers, 0);
    if (ide_buffer_get_file_path(only) != NULL ||
        ide_buffer_get_modified(only)) {
      IdeBuffer *fresh = ide_buffer_new(NULL);
      if (self->active_scheme_id)
        ide_buffer_set_style_scheme(fresh, self->active_scheme_id);
      g_ptr_array_remove_index(self->buffers, 0);
      g_ptr_array_add(self->buffers, fresh);
      self->active_buf_idx = 0;
      bind_active_buffer_to_view(self);
    }
    return;
  }
  if (idx < self->buffers->len) {
    g_ptr_array_remove_index(self->buffers, idx);
    if (self->active_buf_idx >= self->buffers->len)
      self->active_buf_idx = self->buffers->len - 1;
    else if (idx < self->active_buf_idx && self->active_buf_idx > 0)
      self->active_buf_idx--;
    bind_active_buffer_to_view(self);
  }
}

static void move_cursor_to_line_col(KajoIdeWindow *self,
                                    guint          line_1based,
                                    guint          col_1based) {
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter iter;
  int line0 = line_1based > 0 ? (int)line_1based - 1 : 0;
  int col0 = col_1based > 0 ? (int)col_1based - 1 : 0;
  gtk_text_buffer_get_iter_at_line_offset(tbuf, &iter, line0, 0);
  int chars_in_line = gtk_text_iter_get_chars_in_line(&iter);
  if (col0 < chars_in_line)
    gtk_text_iter_set_line_offset(&iter, col0);
  gtk_text_buffer_place_cursor(tbuf, &iter);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view),
                               &iter,
                               0.15,
                               FALSE,
                               0.0,
                               0.5);
  update_status_bar_and_hud(self);
}

static void on_external_buffer_change(IdeBuffer *buf,
                                      gboolean   reloaded_cleanly,
                                      gpointer   user_data) {
  KajoIdeWindow *self = user_data;
  if (reloaded_cleanly) {
    g_autofree char *msg =
        g_strdup_printf("↻ Hot-reloaded %s from disk", ide_buffer_get_title(buf));
    show_toast(self, msg);
  } else {
    g_autofree char *msg = g_strdup_printf(
        "⚠ Disk conflict on %s (unsaved edits preserved)",
        ide_buffer_get_title(buf));
    show_toast(self, msg);
  }
  sync_markdown_views(self);
  update_status_bar_and_hud(self);
}

void kajo_ide_window_open_path(KajoIdeWindow *self,
                               const char    *path,
                               guint          line_1based,
                               guint          col_1based) {
  if (!self || !path || !*path) return;

  record_jumplist_Here(self);

  /* Check if already open */
  for (guint i = 0; i < self->buffers->len; i++) {
    IdeBuffer *existing = g_ptr_array_index(self->buffers, i);
    if (g_strcmp0(ide_buffer_get_file_path(existing), path) == 0) {
      self->active_buf_idx = i;
      bind_active_buffer_to_view(self);
      move_cursor_to_line_col(self, line_1based, col_1based);
      return;
    }
  }

  IdeBuffer *nbuf = ide_buffer_new(path);
  ide_buffer_load_from_disk(nbuf, NULL);
  ide_buffer_set_external_change_callback(nbuf, on_external_buffer_change, self);

  /* Replace initial empty [No Name] buffer if unmodified */
  if (self->buffers->len == 1) {
    IdeBuffer *first = g_ptr_array_index(self->buffers, 0);
    if (ide_buffer_get_file_path(first) == NULL &&
        !ide_buffer_get_modified(first)) {
      g_ptr_array_remove_index(self->buffers, 0);
    }
  }

  g_ptr_array_add(self->buffers, nbuf);
  self->active_buf_idx = self->buffers->len - 1;
  bind_active_buffer_to_view(self);
  move_cursor_to_line_col(self, line_1based, col_1based);
}

void kajo_ide_window_open_files(KajoIdeWindow *self,
                                GFile        **files,
                                int            n_files) {
  if (!self || !files || n_files <= 0) return;
  for (int i = 0; i < n_files; i++) {
    g_autofree char *path = g_file_get_path(files[i]);
    if (!path) continue;
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
      g_free(self->workspace_root);
      self->workspace_root = g_strdup(path);
      ide_tree_set_workspace_root(self->tree, path);
      ide_search_drawer_set_workspace_root(self->search_drawer, path);
      update_status_bar_and_hud(self);
    } else {
      kajo_ide_window_open_path(self, path, 1, 1);
    }
  }
}

static void show_drawer_page(KajoIdeWindow *self, const char *page_name) {
  if (!self || !page_name) return;
  gtk_stack_set_visible_child_name(GTK_STACK(self->drawer_stack), page_name);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), TRUE);

  struct {
    GtkWidget  *btn;
    const char *name;
  } tabs[] = {
    { self->drawer_tab_agent,  "agent"    },
    { self->drawer_tab_dap,    "dap"      },
    { self->drawer_tab_search, "search"   },
    { self->drawer_tab_md,     "markdown" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(tabs); i++) {
    if (!tabs[i].btn) continue;
    if (g_strcmp0(page_name, tabs[i].name) == 0)
      gtk_widget_add_css_class(tabs[i].btn, "active");
    else
      gtk_widget_remove_css_class(tabs[i].btn, "active");
  }
}

static void execute_buffer_motion_or_edit(KajoIdeWindow *self,
                                          const IdeAction *act) {
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  GtkTextIter ins, bound;
  gtk_text_buffer_get_iter_at_mark(tbuf, &ins, gtk_text_buffer_get_insert(tbuf));
  gtk_text_buffer_get_iter_at_mark(
      tbuf, &bound, gtk_text_buffer_get_selection_bound(tbuf));

  gboolean extend = (ide_fsm_get_state(self->fsm) == IDE_STATE_SELECT ||
                     act->type == IDE_ACTION_EXTEND_SELECTION);

  switch (act->type) {
    case IDE_ACTION_MOVE_LEFT:
      gtk_text_iter_backward_cursor_position(&ins);
      break;
    case IDE_ACTION_MOVE_RIGHT:
      gtk_text_iter_forward_cursor_position(&ins);
      break;
    case IDE_ACTION_MOVE_UP: {
      int line = gtk_text_iter_get_line(&ins);
      int col = gtk_text_iter_get_line_offset(&ins);
      if (line > 0) {
        gtk_text_buffer_get_iter_at_line_offset(tbuf, &ins, line - 1, 0);
        int max_c = gtk_text_iter_get_chars_in_line(&ins);
        if (gtk_text_iter_ends_line(&ins)) max_c = 0;
        else if (max_c > 0) max_c--;
        if (col < max_c) gtk_text_iter_set_line_offset(&ins, col);
        else gtk_text_iter_set_line_offset(&ins, max_c);
      }
      break;
    }
    case IDE_ACTION_MOVE_DOWN: {
      int line = gtk_text_iter_get_line(&ins);
      int col = gtk_text_iter_get_line_offset(&ins);
      int total = gtk_text_buffer_get_line_count(tbuf);
      if (line + 1 < total) {
        gtk_text_buffer_get_iter_at_line_offset(tbuf, &ins, line + 1, 0);
        int max_c = gtk_text_iter_get_chars_in_line(&ins);
        if (gtk_text_iter_ends_line(&ins)) max_c = 0;
        else if (max_c > 0) max_c--;
        if (col < max_c) gtk_text_iter_set_line_offset(&ins, col);
        else gtk_text_iter_set_line_offset(&ins, max_c);
      }
      break;
    }
    case IDE_ACTION_EXTEND_SELECTION: {
      if (g_strcmp0(act->str, "left") == 0) {
        gtk_text_iter_backward_cursor_position(&ins);
      } else if (g_strcmp0(act->str, "right") == 0) {
        gtk_text_iter_forward_cursor_position(&ins);
      } else if (g_strcmp0(act->str, "up") == 0) {
        int line = gtk_text_iter_get_line(&ins);
        int col = gtk_text_iter_get_line_offset(&ins);
        if (line > 0) {
          gtk_text_buffer_get_iter_at_line_offset(tbuf, &ins, line - 1, 0);
          int max_c = gtk_text_iter_get_chars_in_line(&ins);
          if (gtk_text_iter_ends_line(&ins)) max_c = 0;
          else if (max_c > 0) max_c--;
          gtk_text_iter_set_line_offset(&ins, MIN(col, max_c));
        }
      } else if (g_strcmp0(act->str, "down") == 0) {
        int line = gtk_text_iter_get_line(&ins);
        int col = gtk_text_iter_get_line_offset(&ins);
        int total = gtk_text_buffer_get_line_count(tbuf);
        if (line + 1 < total) {
          gtk_text_buffer_get_iter_at_line_offset(tbuf, &ins, line + 1, 0);
          int max_c = gtk_text_iter_get_chars_in_line(&ins);
          if (gtk_text_iter_ends_line(&ins)) max_c = 0;
          else if (max_c > 0) max_c--;
          gtk_text_iter_set_line_offset(&ins, MIN(col, max_c));
        }
      }
      extend = TRUE;
      break;
    }
    case IDE_ACTION_MOVE_WORD_FORWARD:
      gtk_text_iter_forward_word_end(&ins);
      break;
    case IDE_ACTION_SELECT_WORD: {
      if (gtk_text_iter_forward_word_end(&ins)) {
        bound = ins;
        gtk_text_iter_backward_word_start(&bound);
        gtk_text_buffer_select_range(tbuf, &ins, &bound);
      }
      goto scroll_and_done;
    }
    case IDE_ACTION_MOVE_WORD_BACKWARD:
      gtk_text_iter_backward_word_start(&ins);
      break;
    case IDE_ACTION_SELECT_WORD_BACKWARD: {
      if (gtk_text_iter_backward_word_start(&ins)) {
        bound = ins;
        gtk_text_iter_forward_word_end(&bound);
        gtk_text_buffer_select_range(tbuf, &ins, &bound);
      }
      goto scroll_and_done;
    }
    case IDE_ACTION_SELECT_LINE: {
      GtkTextIter s_it, e_it;
      if (gtk_text_buffer_get_selection_bounds(tbuf, &s_it, &e_it) &&
          gtk_text_iter_get_line_offset(&s_it) == 0 &&
          (gtk_text_iter_get_line_offset(&e_it) == 0 ||
           gtk_text_iter_is_end(&e_it)) &&
          !gtk_text_iter_equal(&s_it, &e_it)) {
        bound = s_it;
        ins = e_it;
        gtk_text_iter_forward_line(&ins);
      } else {
        bound = ins;
        gtk_text_iter_set_line_offset(&bound, 0);
        if (!gtk_text_iter_ends_line(&ins))
          gtk_text_iter_forward_to_line_end(&ins);
        gtk_text_iter_forward_char(&ins);
      }
      gtk_text_buffer_select_range(tbuf, &ins, &bound);
      goto scroll_and_done;
    }
    case IDE_ACTION_COLLAPSE_SELECTION:
      gtk_text_buffer_place_cursor(tbuf, &ins);
      goto done;
    case IDE_ACTION_MOVE_LINE_START:
      gtk_text_iter_set_line_offset(&ins, 0);
      break;
    case IDE_ACTION_MOVE_LINE_END:
      if (!gtk_text_iter_ends_line(&ins))
        gtk_text_iter_forward_to_line_end(&ins);
      break;
    case IDE_ACTION_MOVE_BUFFER_START:
      gtk_text_buffer_get_start_iter(tbuf, &ins);
      break;
    case IDE_ACTION_MOVE_BUFFER_END:
      gtk_text_buffer_get_end_iter(tbuf, &ins);
      break;
    case IDE_ACTION_PAGE_DOWN:
      gtk_text_iter_forward_lines(&ins, 18);
      break;
    case IDE_ACTION_PAGE_UP:
      gtk_text_iter_backward_lines(&ins, 18);
      break;
    case IDE_ACTION_SELECT_TEXT_OBJECT: {
      g_autofree char *full = ide_buffer_get_text(buf);
      gsize cur_byte = 0, s_byte = 0, e_byte = 0;
      GtkTextIter start_it;
      gtk_text_buffer_get_start_iter(tbuf, &start_it);
      g_autofree char *prefix =
          gtk_text_buffer_get_text(tbuf, &start_it, &ins, TRUE);
      cur_byte = strlen(prefix);
      gboolean inside = (act->arg1[0] == 'i');
      if (ide_find_text_object(full,
                               cur_byte,
                               inside,
                               act->arg2[0],
                               &s_byte,
                               &e_byte)) {
        glong s_char = g_utf8_pointer_to_offset(full, full + s_byte);
        glong e_char = g_utf8_pointer_to_offset(full, full + e_byte);
        GtkTextIter sit, eit;
        gtk_text_buffer_get_iter_at_offset(tbuf, &sit, (int)s_char);
        gtk_text_buffer_get_iter_at_offset(tbuf, &eit, (int)e_char);
        gtk_text_buffer_select_range(tbuf, &eit, &sit);
      }
      goto done;
    }
    case IDE_ACTION_INSERT_CHAR: {
      gtk_text_buffer_begin_user_action(tbuf);
      if (gtk_text_buffer_get_selection_bounds(tbuf, &ins, &bound))
        gtk_text_buffer_delete(tbuf, &ins, &bound);
      gtk_text_buffer_insert_at_cursor(tbuf, act->str, -1);
      gtk_text_buffer_end_user_action(tbuf);
      goto post_edit;
    }
    case IDE_ACTION_INSERT_NEWLINE: {
      gtk_text_buffer_begin_user_action(tbuf);
      if (g_strcmp0(act->str, "below") == 0) {
        if (!gtk_text_iter_ends_line(&ins))
          gtk_text_iter_forward_to_line_end(&ins);
        gtk_text_buffer_place_cursor(tbuf, &ins);
        gtk_text_buffer_insert_at_cursor(tbuf, "\n", 1);
      } else if (g_strcmp0(act->str, "above") == 0) {
        int line = gtk_text_iter_get_line(&ins);
        gtk_text_iter_set_line_offset(&ins, 0);
        gtk_text_buffer_insert(tbuf, &ins, "\n", 1);
        gtk_text_buffer_get_iter_at_line_offset(tbuf, &ins, line, 0);
        gtk_text_buffer_place_cursor(tbuf, &ins);
      } else {
        if (gtk_text_buffer_get_selection_bounds(tbuf, &ins, &bound))
          gtk_text_buffer_delete(tbuf, &ins, &bound);
        gtk_text_buffer_insert_at_cursor(tbuf, "\n", 1);
      }
      gtk_text_buffer_end_user_action(tbuf);
      goto post_edit;
    }
    case IDE_ACTION_INSERT_TAB: {
      gtk_text_buffer_begin_user_action(tbuf);
      if (gtk_text_buffer_get_selection_bounds(tbuf, &ins, &bound))
        gtk_text_buffer_delete(tbuf, &ins, &bound);
      gtk_text_buffer_insert_at_cursor(tbuf, "  ", 2);
      gtk_text_buffer_end_user_action(tbuf);
      goto post_edit;
    }
    case IDE_ACTION_INDENT:
    case IDE_ACTION_OUTDENT: {
      GtkTextIter s_it, e_it;
      if (!gtk_text_buffer_get_selection_bounds(tbuf, &s_it, &e_it)) {
        s_it = ins;
        e_it = ins;
      }
      int s_line = gtk_text_iter_get_line(&s_it);
      int e_line = gtk_text_iter_get_line(&e_it);
      if (e_line > s_line && gtk_text_iter_get_line_offset(&e_it) == 0)
        e_line--;
      gtk_text_buffer_begin_user_action(tbuf);
      for (int l = s_line; l <= e_line; l++) {
        GtkTextIter lit;
        gtk_text_buffer_get_iter_at_line_offset(tbuf, &lit, l, 0);
        if (act->type == IDE_ACTION_INDENT) {
          gtk_text_buffer_insert(tbuf, &lit, "  ", 2);
        } else {
          for (int k = 0; k < 2; k++) {
            gunichar ch = gtk_text_iter_get_char(&lit);
            if (ch == ' ' || ch == '\t') {
              GtkTextIter next = lit;
              gtk_text_iter_forward_char(&next);
              gtk_text_buffer_delete(tbuf, &lit, &next);
              if (ch == '\t') break;
            } else {
              break;
            }
          }
        }
      }
      gtk_text_buffer_end_user_action(tbuf);
      goto post_edit;
    }
    case IDE_ACTION_BACKSPACE: {
      gtk_text_buffer_begin_user_action(tbuf);
      if (gtk_text_buffer_get_selection_bounds(tbuf, &ins, &bound))
        gtk_text_buffer_delete(tbuf, &ins, &bound);
      else
        gtk_text_buffer_backspace(tbuf, &ins, TRUE, TRUE);
      gtk_text_buffer_end_user_action(tbuf);
      goto post_edit;
    }
    case IDE_ACTION_DELETE_CHAR:
    case IDE_ACTION_DELETE_SELECTION:
    case IDE_ACTION_CHANGE_SELECTION: {
      gtk_text_buffer_begin_user_action(tbuf);
      if (!gtk_text_buffer_get_selection_bounds(tbuf, &ins, &bound)) {
        bound = ins;
        gtk_text_iter_forward_cursor_position(&bound);
      }
      g_free(self->yank_register);
      self->yank_register = gtk_text_buffer_get_text(tbuf, &ins, &bound, TRUE);
      GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
      if (cb && self->yank_register)
        gdk_clipboard_set_text(cb, self->yank_register);
      gtk_text_buffer_delete(tbuf, &ins, &bound);
      gtk_text_buffer_end_user_action(tbuf);
      goto post_edit;
    }
    case IDE_ACTION_YANK: {
      if (!gtk_text_buffer_get_selection_bounds(tbuf, &ins, &bound)) {
        gtk_text_iter_set_line_offset(&ins, 0);
        bound = ins;
        gtk_text_iter_forward_line(&bound);
      }
      g_free(self->yank_register);
      self->yank_register = gtk_text_buffer_get_text(tbuf, &ins, &bound, TRUE);
      GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
      if (cb) gdk_clipboard_set_text(cb, self->yank_register);
      show_toast(self, "Yanked selection to clipboard");
      goto done;
    }
    case IDE_ACTION_PASTE_AFTER:
    case IDE_ACTION_PASTE_BEFORE: {
      gtk_text_buffer_begin_user_action(tbuf);
      if (gtk_text_buffer_get_selection_bounds(tbuf, &ins, &bound))
        gtk_text_buffer_delete(tbuf, &ins, &bound);
      if (self->yank_register && *self->yank_register) {
        gtk_text_buffer_insert_at_cursor(tbuf, self->yank_register, -1);
      } else {
        GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
        if (cb) gtk_text_buffer_paste_clipboard(tbuf, cb, NULL, TRUE);
      }
      gtk_text_buffer_end_user_action(tbuf);
      goto post_edit;
    }
    case IDE_ACTION_UNDO:
      if (gtk_text_buffer_get_can_undo(tbuf)) {
        gtk_text_buffer_undo(tbuf);
        show_toast(self, "↶ Undo");
      }
      goto post_edit;
    case IDE_ACTION_REDO:
      if (gtk_text_buffer_get_can_redo(tbuf)) {
        gtk_text_buffer_redo(tbuf);
        show_toast(self, "↷ Redo");
      }
      goto post_edit;
    default:
      return;
  }

  if (extend)
    gtk_text_buffer_select_range(tbuf, &ins, &bound);
  else
    gtk_text_buffer_place_cursor(tbuf, &ins);

scroll_and_done:
  gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(self->source_view),
                                     gtk_text_buffer_get_insert(tbuf));
  goto done;

post_edit:
  ide_buffer_recompute_diffs(buf);
  ide_lsp_manager_notify_change(self->lsp, buf);
  sync_markdown_views(self);
  rebuild_tab_ribbon(self);
  gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(self->source_view),
                                     gtk_text_buffer_get_insert(tbuf));
  gtk_widget_queue_draw(self->source_view);

done:
  update_status_bar_and_hud(self);
}

static void dispatch_ide_action(KajoIdeWindow *self, const IdeAction *act) {
  if (!self || !act || act->type == IDE_ACTION_NONE) {
    update_status_bar_and_hud(self);
    return;
  }

  IdeState cur_st = ide_fsm_get_state(self->fsm);
  IdeBuffer *buf = get_active_buffer(self);
  guint cur_line = 1, cur_col = 1;
  get_cursor_line_col_1based(self, &cur_line, &cur_col);

  /* Route text input & navigation when inside editable Tree prompt or Drawer */
  if (cur_st == IDE_STATE_TREE && ide_fsm_get_tree_editable(self->fsm)) {
    if (act->type == IDE_ACTION_INSERT_CHAR) {
      ide_tree_prompt_append_char(self->tree, act->str);
      update_status_bar_and_hud(self);
      return;
    }
    if (act->type == IDE_ACTION_BACKSPACE) {
      ide_tree_prompt_backspace(self->tree);
      update_status_bar_and_hud(self);
      return;
    }
  } else if (cur_st == IDE_STATE_DRAWER) {
    if (act->type == IDE_ACTION_MOVE_DOWN) {
      ide_search_drawer_move_down(self->search_drawer);
      update_status_bar_and_hud(self);
      return;
    }
    if (act->type == IDE_ACTION_MOVE_UP) {
      ide_search_drawer_move_up(self->search_drawer);
      update_status_bar_and_hud(self);
      return;
    }
    if (ide_fsm_get_drawer_editable(self->fsm)) {
      if (act->type == IDE_ACTION_INSERT_CHAR) {
        ide_search_drawer_append_char(self->search_drawer, act->str);
        update_status_bar_and_hud(self);
        return;
      }
      if (act->type == IDE_ACTION_BACKSPACE) {
        ide_search_drawer_backspace(self->search_drawer);
        update_status_bar_and_hud(self);
        return;
      }
    }
  }

  switch (act->type) {
    /* File Tree & Panes */
    case IDE_ACTION_TOGGLE_FILE_TREE:
      if (cur_st == IDE_STATE_TREE) {
        gtk_widget_set_visible(self->tree_container, TRUE);
      } else {
        gtk_widget_set_visible(self->tree_container,
                               !gtk_widget_get_visible(self->tree_container));
        gtk_widget_grab_focus(self->source_view);
      }
      break;
    case IDE_ACTION_PANE_FOCUS_TREE:
      gtk_widget_set_visible(self->tree_container, TRUE);
      break;
    case IDE_ACTION_PANE_FOCUS_BUFFER:
      if (ide_fsm_get_tree_editable(self->fsm)) {
        ide_tree_prompt_cancel(self->tree);
        ide_fsm_set_tree_editable(self->fsm, FALSE);
      }
      gtk_widget_grab_focus(self->source_view);
      break;
    case IDE_ACTION_PANE_FOCUS_DRAWER:
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), TRUE);
      break;
    case IDE_ACTION_PANE_CYCLE:
      if (cur_st == IDE_STATE_TREE) {
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer)))
          ide_fsm_set_state(self->fsm, IDE_STATE_DRAWER);
        else
          ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
      } else if (cur_st == IDE_STATE_DRAWER) {
        ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
      } else {
        if (gtk_widget_get_visible(self->tree_container))
          ide_fsm_set_state(self->fsm, IDE_STATE_TREE);
        else if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer)))
          ide_fsm_set_state(self->fsm, IDE_STATE_DRAWER);
      }
      gtk_widget_grab_focus(self->source_view);
      break;
    case IDE_ACTION_TOGGLE_DRAWER:
      if (!gtk_revealer_get_reveal_child(GTK_REVEALER(self->drawer_revealer))) {
        show_drawer_page(self, "search");
      } else if (cur_st != IDE_STATE_DRAWER) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), FALSE);
      }
      break;
    case IDE_ACTION_DRAWER_CLOSE:
      gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), FALSE);
      gtk_widget_grab_focus(self->source_view);
      break;
    case IDE_ACTION_DRAWER_JUMP:
      ide_search_drawer_jump_selected(self->search_drawer);
      gtk_widget_grab_focus(self->source_view);
      break;
    case IDE_ACTION_RESIZE_TREE_GROW:
      if (self->tree_paned) {
        int pos = gtk_paned_get_position(GTK_PANED(self->tree_paned));
        gtk_paned_set_position(GTK_PANED(self->tree_paned), CLAMP(pos + 24, 150, 520));
      }
      break;
    case IDE_ACTION_RESIZE_TREE_SHRINK:
      if (self->tree_paned) {
        int pos = gtk_paned_get_position(GTK_PANED(self->tree_paned));
        gtk_paned_set_position(GTK_PANED(self->tree_paned), CLAMP(pos - 24, 150, 520));
      }
      break;
    case IDE_ACTION_RESIZE_DRAWER_GROW:
      if (self->vertical_paned) {
        int pos = gtk_paned_get_position(GTK_PANED(self->vertical_paned));
        gtk_paned_set_position(GTK_PANED(self->vertical_paned), MAX(200, pos - 30));
      }
      break;
    case IDE_ACTION_RESIZE_DRAWER_SHRINK:
      if (self->vertical_paned) {
        int pos = gtk_paned_get_position(GTK_PANED(self->vertical_paned));
        gtk_paned_set_position(GTK_PANED(self->vertical_paned), pos + 30);
      }
      break;

    /* Graphical Markdown & Cairo Vector Mermaid */
    case IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW: {
      gboolean next_vis = !gtk_widget_get_visible(self->md_split_container);
      gtk_widget_set_visible(self->md_split_container, next_vis);
      gtk_widget_set_visible(self->source_map, !next_vis);
      if (next_vis && self->editor_md_paned) {
        int avail_w = gtk_widget_get_width(self->editor_md_paned);
        if (avail_w <= 200) avail_w = 1040;
        gtk_paned_set_position(GTK_PANED(self->editor_md_paned), avail_w / 2);
      }
      sync_markdown_views(self);
      gtk_widget_grab_focus(self->source_view);
      break;
    }
    case IDE_ACTION_TOGGLE_MERMAID_MODE:
      show_drawer_page(self, "markdown");
      sync_markdown_views(self);
      break;
    case IDE_ACTION_TOGGLE_CHECKLIST:
      if (buf) {
        g_autofree char *txt = ide_buffer_get_text(buf);
        g_autofree char *toggled =
            ide_markdown_toggle_checklist_line(txt, cur_line);
        ide_buffer_set_text(buf, toggled);
        move_cursor_to_line_col(self, cur_line, cur_col);
        sync_markdown_views(self);
      }
      break;

    /* Fuzzy Picker */
    case IDE_ACTION_PICKER_OPEN_FILES:
      ide_picker_open_files(self->picker, self->workspace_root);
      break;
    case IDE_ACTION_PICKER_OPEN_BUFFERS:
      ide_picker_open_buffers(self->picker, self->buffers);
      break;
    case IDE_ACTION_PICKER_OPEN_GREP:
      ide_picker_open_grep(self->picker, self->workspace_root);
      break;
    case IDE_ACTION_HELP_OPEN:
      ide_fsm_set_state(self->fsm, IDE_STATE_HELP);
      ide_picker_open_help(self->picker);
      break;
    case IDE_ACTION_HELP_TOGGLE:
      if (ide_picker_is_visible(self->picker) &&
          ide_picker_get_mode(self->picker) == IDE_PICKER_HELP) {
        ide_picker_close(self->picker);
        ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
      } else {
        ide_fsm_set_state(self->fsm, IDE_STATE_HELP);
        ide_picker_open_help(self->picker);
      }
      break;
    case IDE_ACTION_PICKER_NEXT:
    case IDE_ACTION_HELP_NEXT:
      ide_picker_move_next(self->picker);
      break;
    case IDE_ACTION_PICKER_PREV:
    case IDE_ACTION_HELP_PREV:
      ide_picker_move_prev(self->picker);
      break;
    case IDE_ACTION_HELP_PAGE_DOWN:
      ide_picker_page_down(self->picker);
      break;
    case IDE_ACTION_HELP_PAGE_UP:
      ide_picker_page_up(self->picker);
      break;
    case IDE_ACTION_PICKER_INPUT:
    case IDE_ACTION_HELP_INPUT:
      ide_picker_append_input(self->picker, act->str);
      break;
    case IDE_ACTION_PICKER_BACKSPACE:
    case IDE_ACTION_HELP_BACKSPACE:
      ide_picker_backspace(self->picker);
      break;
    case IDE_ACTION_PICKER_SELECT:
    case IDE_ACTION_HELP_SELECT:
      ide_picker_confirm(self->picker);
      gtk_widget_grab_focus(self->source_view);
      break;
    case IDE_ACTION_PICKER_CANCEL:
    case IDE_ACTION_HELP_CANCEL:
      ide_picker_close(self->picker);
      gtk_widget_grab_focus(self->source_view);
      break;

    /* File Tree Navigation & CRUD */
    case IDE_ACTION_TREE_UP:
      ide_tree_move_up(self->tree);
      break;
    case IDE_ACTION_TREE_DOWN:
      ide_tree_move_down(self->tree);
      break;
    case IDE_ACTION_TREE_PAGE_UP:
      ide_tree_page_up(self->tree);
      break;
    case IDE_ACTION_TREE_PAGE_DOWN:
      ide_tree_page_down(self->tree);
      break;
    case IDE_ACTION_TREE_TOP:
      ide_tree_move_top(self->tree);
      break;
    case IDE_ACTION_TREE_BOTTOM:
      ide_tree_move_bottom(self->tree);
      break;
    case IDE_ACTION_TREE_TOGGLE_OR_OPEN:
      if (ide_fsm_get_tree_editable(self->fsm)) {
        ide_tree_prompt_confirm(self->tree);
        ide_fsm_set_tree_editable(self->fsm, FALSE);
      } else {
        ide_tree_toggle_or_open(self->tree);
      }
      break;
    case IDE_ACTION_TREE_COLLAPSE_OR_PARENT:
      ide_tree_collapse_or_parent(self->tree);
      break;
    case IDE_ACTION_TREE_REFRESH:
      ide_tree_refresh(self->tree);
      break;
    case IDE_ACTION_TREE_CREATE_FILE:
      ide_tree_prompt_create_file(self->tree);
      break;
    case IDE_ACTION_TREE_CREATE_DIR:
      ide_tree_prompt_create_dir(self->tree);
      break;
    case IDE_ACTION_TREE_DELETE:
      ide_tree_delete_selected(self->tree);
      break;

    /* Tabs & Save */
    case IDE_ACTION_TAB_NEXT:
    case IDE_ACTION_STEP_FORWARD:
      if (self->buffers->len > 1) {
        self->active_buf_idx = (self->active_buf_idx + 1) % self->buffers->len;
        bind_active_buffer_to_view(self);
      }
      break;
    case IDE_ACTION_TAB_PREV:
    case IDE_ACTION_STEP_BACKWARD:
      if (self->buffers->len > 1) {
        self->active_buf_idx =
            (self->active_buf_idx + self->buffers->len - 1) % self->buffers->len;
        bind_active_buffer_to_view(self);
      }
      break;
    case IDE_ACTION_TAB_SWITCH:
      if (act->int_val >= 1 && (guint)act->int_val <= self->buffers->len) {
        self->active_buf_idx = (guint)(act->int_val - 1);
        bind_active_buffer_to_view(self);
      }
      break;
    case IDE_ACTION_SAVE:
      if (buf && ide_buffer_save(buf, NULL)) {
        ide_lsp_manager_notify_save(self->lsp, buf);
        ide_tree_refresh(self->tree);
        rebuild_tab_ribbon(self);
        show_toast(self, "✓ Saved file atomically");
      }
      break;
    case IDE_ACTION_SAVE_ALL:
      for (guint i = 0; i < self->buffers->len; i++) {
        IdeBuffer *b = g_ptr_array_index(self->buffers, i);
        if (ide_buffer_get_file_path(b)) ide_buffer_save(b, NULL);
      }
      ide_tree_refresh(self->tree);
      rebuild_tab_ribbon(self);
      show_toast(self, "✓ Saved all open buffers");
      break;
    case IDE_ACTION_BUFFER_CLOSE:
    case IDE_ACTION_BUFFER_FORCE_CLOSE:
      if (self->buffers->len > 1) {
        g_ptr_array_remove_index(self->buffers, self->active_buf_idx);
        if (self->active_buf_idx >= self->buffers->len)
          self->active_buf_idx = self->buffers->len - 1;
        bind_active_buffer_to_view(self);
      }
      break;

    /* Alternate File (Space a) & Jumplist (Ctrl+o / Ctrl+i) */
    case IDE_ACTION_ALTERNATE_FILE:
      if (buf && ide_buffer_get_file_path(buf)) {
        g_autofree char *alt =
            ide_resolve_alternate_path(ide_buffer_get_file_path(buf));
        if (alt) kajo_ide_window_open_path(self, alt, 1, 1);
      }
      break;
    case IDE_ACTION_JUMPLIST_BACK:
      if (self->jumplist_idx > 0) {
        self->jumplist_idx--;
        IdeJumpEntry *e =
            g_ptr_array_index(self->jumplist, self->jumplist_idx);
        kajo_ide_window_open_path(self, e->path, e->line_1based, e->col_1based);
      }
      break;
    case IDE_ACTION_JUMPLIST_FORWARD:
      if (self->jumplist_idx + 1 < (int)self->jumplist->len) {
        self->jumplist_idx++;
        IdeJumpEntry *e =
            g_ptr_array_index(self->jumplist, self->jumplist_idx);
        kajo_ide_window_open_path(self, e->path, e->line_1based, e->col_1based);
      }
      break;

    /* LSP 3.17 Actions & Diagnostics Navigation */
    case IDE_ACTION_LSP_HOVER:
      if (buf)
        ide_lsp_manager_request_hover(
            self->lsp, self->source_view, buf, cur_line, cur_col);
      break;
    case IDE_ACTION_LSP_COMPLETE:
      if (buf)
        ide_lsp_manager_request_completion(
            self->lsp, self->source_view, buf, cur_line, cur_col);
      break;
    case IDE_ACTION_LSP_DEFINITION:
      if (buf)
        ide_lsp_manager_request_definition(self->lsp, buf, cur_line, cur_col);
      break;
    case IDE_ACTION_LSP_CANCEL:
      ide_lsp_manager_cancel_popover(self->lsp);
      break;
    case IDE_ACTION_LSP_SELECT_NEXT:
      ide_lsp_manager_select_next(self->lsp);
      break;
    case IDE_ACTION_LSP_SELECT_PREV:
      ide_lsp_manager_select_prev(self->lsp);
      break;
    case IDE_ACTION_LSP_SELECT_CONFIRM:
      if (buf) ide_lsp_manager_confirm_selection(self->lsp, buf);
      break;
    case IDE_ACTION_NEXT_DIAGNOSTIC:
    case IDE_ACTION_PREV_DIAGNOSTIC:
      if (buf) {
        const GPtrArray *diags = ide_buffer_get_diagnostics(buf);
        if (diags && diags->len > 0) {
          const IdeDiagnostic *target = NULL;
          if (act->type == IDE_ACTION_NEXT_DIAGNOSTIC) {
            for (guint i = 0; i < diags->len; i++) {
              const IdeDiagnostic *d = g_ptr_array_index(diags, i);
              if (d->line > cur_line) {
                target = d;
                break;
              }
            }
            if (!target) target = g_ptr_array_index(diags, 0);
          } else {
            for (int i = (int)diags->len - 1; i >= 0; i--) {
              const IdeDiagnostic *d = g_ptr_array_index(diags, (guint)i);
              if (d->line < cur_line) {
                target = d;
                break;
              }
            }
            if (!target) target = g_ptr_array_index(diags, diags->len - 1);
          }
          if (target) {
            move_cursor_to_line_col(self, target->line, target->col);
            show_toast(self, target->message);
          }
        }
      }
      break;

    /* Search, Replace & Build Quickfix Drawer */
    case IDE_ACTION_SEARCH_OPEN:
      g_string_truncate(self->search_query, 0);
      break;
    case IDE_ACTION_SEARCH_INPUT:
      g_string_append(self->search_query, act->str);
      break;
    case IDE_ACTION_SEARCH_BACKSPACE:
      if (self->search_query->len > 0)
        g_string_truncate(self->search_query, self->search_query->len - 1);
      break;
    case IDE_ACTION_SEARCH_CLEAR_HIGHLIGHT:
      g_string_truncate(self->search_query, 0);
      if (buf) {
        GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
        GtkTextIter ins;
        gtk_text_buffer_get_iter_at_mark(
            tbuf, &ins, gtk_text_buffer_get_insert(tbuf));
        gtk_text_buffer_place_cursor(tbuf, &ins);
      }
      break;
    case IDE_ACTION_SEARCH_CONFIRM:
    case IDE_ACTION_SEARCH_NEXT:
      if (buf && self->search_query->len > 0) {
        GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
        GtkTextIter ins, mstart, mend;
        gtk_text_buffer_get_iter_at_mark(
            tbuf, &ins, gtk_text_buffer_get_insert(tbuf));
        gtk_text_iter_forward_char(&ins);
        if (!gtk_text_iter_forward_search(&ins,
                                          self->search_query->str,
                                          GTK_TEXT_SEARCH_CASE_INSENSITIVE,
                                          &mstart,
                                          &mend,
                                          NULL)) {
          GtkTextIter start_it;
          gtk_text_buffer_get_start_iter(tbuf, &start_it);
          if (!gtk_text_iter_forward_search(&start_it,
                                            self->search_query->str,
                                            GTK_TEXT_SEARCH_CASE_INSENSITIVE,
                                            &mstart,
                                            &mend,
                                            NULL)) {
            break;
          }
        }
        gtk_text_buffer_select_range(tbuf, &mend, &mstart);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view),
                                     &mstart,
                                     0.2,
                                     FALSE,
                                     0.0,
                                     0.5);
      }
      break;
    case IDE_ACTION_SEARCH_PREV:
      if (buf && self->search_query->len > 0) {
        GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
        GtkTextIter s_it, e_it, mstart, mend;
        if (!gtk_text_buffer_get_selection_bounds(tbuf, &s_it, &e_it)) {
          gtk_text_buffer_get_iter_at_mark(
              tbuf, &s_it, gtk_text_buffer_get_insert(tbuf));
        }
        if (!gtk_text_iter_backward_search(&s_it,
                                           self->search_query->str,
                                           GTK_TEXT_SEARCH_CASE_INSENSITIVE,
                                           &mstart,
                                           &mend,
                                           NULL)) {
          GtkTextIter end_it;
          gtk_text_buffer_get_end_iter(tbuf, &end_it);
          if (!gtk_text_iter_backward_search(&end_it,
                                             self->search_query->str,
                                             GTK_TEXT_SEARCH_CASE_INSENSITIVE,
                                             &mstart,
                                             &mend,
                                             NULL)) {
            break;
          }
        }
        gtk_text_buffer_select_range(tbuf, &mend, &mstart);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->source_view),
                                     &mstart,
                                     0.2,
                                     FALSE,
                                     0.0,
                                     0.5);
      }
      break;
    case IDE_ACTION_REPLACE_OPEN:
      show_drawer_page(self, "search");
      ide_search_drawer_open_replace(self->search_drawer, buf, FALSE);
      break;
    case IDE_ACTION_PROJECT_REPLACE_OPEN:
      show_drawer_page(self, "search");
      ide_search_drawer_open_replace(self->search_drawer, buf, TRUE);
      break;
    case IDE_ACTION_REPLACE_NEXT_FIELD:
      ide_search_drawer_next_field(self->search_drawer);
      break;
    case IDE_ACTION_REPLACE_PREV_FIELD:
      ide_search_drawer_prev_field(self->search_drawer);
      break;
    case IDE_ACTION_REPLACE_TOGGLE_CASE:
      ide_search_drawer_toggle_case(self->search_drawer);
      break;
    case IDE_ACTION_REPLACE_TOGGLE_WORD:
      ide_search_drawer_toggle_word(self->search_drawer);
      break;
    case IDE_ACTION_REPLACE_TOGGLE_REGEX:
      ide_search_drawer_toggle_regex(self->search_drawer);
      break;
    case IDE_ACTION_REPLACE_APPLY_ALL:
      ide_search_drawer_apply_all(self->search_drawer, self->buffers);
      break;
    case IDE_ACTION_NEXT_ERROR:
      ide_search_drawer_step_error(self->search_drawer, 1);
      break;
    case IDE_ACTION_PREV_ERROR:
      ide_search_drawer_step_error(self->search_drawer, -1);
      break;

    /* DAP Debugger */
    case IDE_ACTION_DAP_TOGGLE_BREAKPOINT:
      if (buf) {
        ide_buffer_toggle_breakpoint(buf, cur_line);
        gtk_widget_queue_draw(self->source_view);
      }
      break;
    case IDE_ACTION_DAP_TOGGLE_VIEW:
      show_drawer_page(self, "dap");
      break;
    case IDE_ACTION_DAP_START:
      show_drawer_page(self, "dap");
      ide_dap_view_start_session(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;
    case IDE_ACTION_DAP_CONTINUE:
      ide_dap_view_continue(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;
    case IDE_ACTION_DAP_STEP_OVER:
      ide_dap_view_step_over(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;
    case IDE_ACTION_DAP_STEP_INTO:
      ide_dap_view_step_into(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;
    case IDE_ACTION_DAP_STEP_OUT:
      ide_dap_view_step_out(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;
    case IDE_ACTION_DAP_STOP:
      ide_dap_view_stop(self->dap, buf);
      gtk_widget_queue_draw(self->source_view);
      break;

    /* Agent IPC & Batch Transaction Review (Space d s / d a / d r) */
    case IDE_ACTION_AGENT_REVIEW_OPEN:
      show_drawer_page(self, "agent");
      break;
    case IDE_ACTION_AGENT_ACCEPT_ALL:
      ide_agent_ipc_accept_all(self->agent_ipc);
      break;
    case IDE_ACTION_AGENT_REVERT_ALL:
      ide_agent_ipc_reject_all(self->agent_ipc);
      break;

    default:
      execute_buffer_motion_or_edit(self, act);
      break;
  }

  update_status_bar_and_hud(self);
}

static char *translate_gdk_key_to_fsm(guint keyval, GdkModifierType state) {
  gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
  gboolean alt = (state & GDK_ALT_MASK) != 0;
  gboolean shift = (state & GDK_SHIFT_MASK) != 0;

  if (alt) {
    if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9)
      return g_strdup_printf("alt+%c", (char)('1' + (keyval - GDK_KEY_1)));
    switch (keyval) {
      case GDK_KEY_Left:  return g_strdup("alt+left");
      case GDK_KEY_Right: return g_strdup("alt+right");
      default: break;
    }
    gunichar uc = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
    if (uc >= 'a' && uc <= 'z')
      return g_strdup_printf("alt+%c", (char)uc);
  }

  if (ctrl) {
    switch (keyval) {
      case GDK_KEY_Left:  return g_strdup("ctrl+left");
      case GDK_KEY_Right: return g_strdup("ctrl+right");
      case GDK_KEY_Up:    return g_strdup("ctrl+up");
      case GDK_KEY_Down:  return g_strdup("ctrl+down");
      case GDK_KEY_Home:  return g_strdup("ctrl+home");
      case GDK_KEY_End:   return g_strdup("ctrl+end");
      case GDK_KEY_space: return g_strdup("ctrl+space");
      default: break;
    }
    gunichar uc = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
    if (uc >= 'a' && uc <= 'z') {
      if (shift && uc == 'z')
        return g_strdup("ctrl+shift+z");
      return g_strdup_printf("ctrl+%c", (char)uc);
    }
  }

  if (shift) {
    switch (keyval) {
      case GDK_KEY_Tab:
      case GDK_KEY_ISO_Left_Tab: return g_strdup("shift+tab");
      case GDK_KEY_Left:         return g_strdup("shift+left");
      case GDK_KEY_Right:        return g_strdup("shift+right");
      case GDK_KEY_Up:           return g_strdup("shift+up");
      case GDK_KEY_Down:         return g_strdup("shift+down");
      case GDK_KEY_F5:           return g_strdup("shift+f5");
      case GDK_KEY_F11:          return g_strdup("shift+f11");
      default: break;
    }
  }

  switch (keyval) {
    case GDK_KEY_Escape:       return g_strdup("esc");
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:     return g_strdup("enter");
    case GDK_KEY_Tab:          return g_strdup("tab");
    case GDK_KEY_ISO_Left_Tab: return g_strdup("shift+tab");
    case GDK_KEY_BackSpace:    return g_strdup("backspace");
    case GDK_KEY_Delete:       return g_strdup("delete");
    case GDK_KEY_space:        return g_strdup("Space");
    case GDK_KEY_Up:           return g_strdup("up");
    case GDK_KEY_Down:         return g_strdup("down");
    case GDK_KEY_Left:         return g_strdup("left");
    case GDK_KEY_Right:        return g_strdup("right");
    case GDK_KEY_Page_Up:      return g_strdup("pgup");
    case GDK_KEY_Page_Down:    return g_strdup("pgdown");
    case GDK_KEY_Home:         return g_strdup("home");
    case GDK_KEY_End:          return g_strdup("end");
    case GDK_KEY_F1:           return g_strdup("f1");
    case GDK_KEY_F5:           return g_strdup("f5");
    case GDK_KEY_F9:           return g_strdup("f9");
    case GDK_KEY_F10:          return g_strdup("f10");
    case GDK_KEY_F11:          return g_strdup("f11");
    case GDK_KEY_F12:          return g_strdup("f12");
    default: break;
  }

  gunichar uc = gdk_keyval_to_unicode(keyval);
  if (uc >= 32 && uc != 127) {
    char utf8[8] = {0};
    int len = g_unichar_to_utf8(uc, utf8);
    utf8[len] = '\0';
    return g_strdup(utf8);
  }
  return NULL;
}

static gboolean on_window_key_pressed(GtkEventControllerKey *controller,
                                      guint                  keyval,
                                      guint                  keycode,
                                      GdkModifierType        state,
                                      gpointer               user_data) {
  (void)controller;
  (void)keycode;
  KajoIdeWindow *self = user_data;

  /* Live Editor Font Zoom (Ctrl++ / Ctrl+=, Ctrl+-, Ctrl+0) */
  if ((state & GDK_CONTROL_MASK) != 0) {
    if (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal ||
        keyval == GDK_KEY_KP_Add) {
      zoom_editor_font(self, +1);
      return TRUE;
    }
    if (keyval == GDK_KEY_minus || keyval == GDK_KEY_KP_Subtract) {
      zoom_editor_font(self, -1);
      return TRUE;
    }
    if (keyval == GDK_KEY_0 || keyval == GDK_KEY_KP_0) {
      zoom_editor_font(self, 0);
      return TRUE;
    }
  }

  /* F1 toggles Help Viewer & Command Palette from any mode */
  if (keyval == GDK_KEY_F1 &&
      (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK)) == 0) {
    IdeAction act = { .type = IDE_ACTION_HELP_TOGGLE };
    dispatch_ide_action(self, &act);
    return TRUE;
  }

  /* In HELP mode, 'q' only closes Help when the search query is empty;
   * if the user has already typed a filter query, 'q' appends to the filter. */
  if (ide_fsm_get_state(self->fsm) == IDE_STATE_HELP &&
      ide_picker_has_query(self->picker) &&
      (keyval == GDK_KEY_q || keyval == GDK_KEY_Q) &&
      (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) == 0) {
    ide_picker_append_input(self->picker, keyval == GDK_KEY_Q ? "Q" : "q");
    update_status_bar_and_hud(self);
    return TRUE;
  }

  g_autofree char *key_str = translate_gdk_key_to_fsm(keyval, state);
  if (!key_str) return FALSE;

  IdeAction act = ide_fsm_handle_key(self->fsm, key_str);
  dispatch_ide_action(self, &act);
  return TRUE;
}

/* Subsystem Callbacks */
static void on_tree_open_file(const char *abs_path, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  kajo_ide_window_open_path(self, abs_path, 1, 1);
  ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  update_status_bar_and_hud(self);
}

static void on_picker_open_file(const char *abs_path,
                                guint       line_1based,
                                guint       col_1based,
                                gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  kajo_ide_window_open_path(self, abs_path, line_1based, col_1based);
  ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  update_status_bar_and_hud(self);
}

static void on_picker_command(IdeActionType action, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  if (action == IDE_ACTION_PICKER_OPEN_FILES ||
      action == IDE_ACTION_PICKER_OPEN_BUFFERS ||
      action == IDE_ACTION_PICKER_OPEN_GREP) {
    ide_fsm_set_state(self->fsm, IDE_STATE_PICKER);
  } else if (action == IDE_ACTION_SEARCH_OPEN) {
    ide_fsm_set_state(self->fsm, IDE_STATE_SEARCH);
  } else if (action == IDE_ACTION_REPLACE_OPEN ||
             action == IDE_ACTION_PROJECT_REPLACE_OPEN) {
    ide_fsm_set_state(self->fsm, IDE_STATE_DRAWER);
    ide_fsm_set_drawer_editable(self->fsm, TRUE);
  } else {
    ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  }
  IdeAction act = { .type = action };
  dispatch_ide_action(self, &act);
}

static void on_lsp_status_changed(const char *server_name,
                                  gboolean    active,
                                  gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  g_autofree char *lbl =
      g_strdup_printf("LSP: %s%s",
                      server_name ? server_name : "none",
                      active ? " ●" : "");
  gtk_label_set_text(GTK_LABEL(self->lsp_status_badge), lbl);
}

static void on_agent_reload_cb(const char *path,
                               const char *old_snapshot_opt,
                               gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  if (!path || !*path) return;
  for (guint i = 0; i < self->buffers->len; i++) {
    IdeBuffer *b = g_ptr_array_index(self->buffers, i);
    if (g_strcmp0(ide_buffer_get_file_path(b), path) == 0) {
      if (old_snapshot_opt) ide_buffer_record_agent_snapshot(b);
      else ide_buffer_accept_agent_changes(b);
      ide_buffer_load_from_disk(b, NULL);
    }
  }
  rebuild_tab_ribbon(self);
  sync_markdown_views(self);
  gtk_widget_queue_draw(self->source_view);
}

static void on_agent_status_cb(const char *badge_text,
                               const char *toast_msg,
                               guint       pending_tx_count,
                               gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  if (badge_text && *badge_text) {
    gtk_label_set_text(GTK_LABEL(self->agent_hdr_badge), badge_text);
    gtk_widget_set_visible(self->agent_hdr_badge, TRUE);
    if (self->agent_status_badge) {
      gtk_label_set_text(GTK_LABEL(self->agent_status_badge), badge_text);
      gtk_widget_set_visible(self->agent_status_badge, TRUE);
    }
  } else if (pending_tx_count > 0) {
    g_autofree char *txt =
        g_strdup_printf("🤖 AGENT: %u HUNKS", pending_tx_count);
    gtk_label_set_text(GTK_LABEL(self->agent_hdr_badge), txt);
    gtk_widget_set_visible(self->agent_hdr_badge, TRUE);
    if (self->agent_status_badge) {
      gtk_label_set_text(GTK_LABEL(self->agent_status_badge), txt);
      gtk_widget_set_visible(self->agent_status_badge, TRUE);
    }
  } else {
    gtk_widget_set_visible(self->agent_hdr_badge, FALSE);
    if (self->agent_status_badge)
      gtk_widget_set_visible(self->agent_status_badge, FALSE);
  }
  if (toast_msg && *toast_msg) show_toast(self, toast_msg);
}

static void on_dap_action_cb(const char *action, gpointer user_data) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(user_data);
  if (!self || !action) return;
  IdeAction act = { .type = IDE_ACTION_NONE };
  if (g_strcmp0(action, "continue") == 0)
    act.type = IDE_ACTION_DAP_CONTINUE;
  else if (g_strcmp0(action, "step_over") == 0)
    act.type = IDE_ACTION_DAP_STEP_OVER;
  else if (g_strcmp0(action, "step_into") == 0)
    act.type = IDE_ACTION_DAP_STEP_INTO;
  else if (g_strcmp0(action, "step_out") == 0)
    act.type = IDE_ACTION_DAP_STEP_OUT;
  else if (g_strcmp0(action, "stop") == 0)
    act.type = IDE_ACTION_DAP_STOP;
  if (act.type != IDE_ACTION_NONE)
    dispatch_ide_action(self, &act);
}

static void on_markdown_node_jump(guint line_1based, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  move_cursor_to_line_col(self, line_1based, 1);
}

static void on_markdown_checklist_edit(const char *new_full_text,
                                       gpointer    user_data) {
  KajoIdeWindow *self = user_data;
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf || !new_full_text) return;
  ide_buffer_set_text(buf, new_full_text);
  rebuild_tab_ribbon(self);
}

/* Header Bar & Drawer Switcher Callbacks */
static void on_hdr_tree_clicked(GtkButton *b, gpointer d) {
  (void)b;
  IdeAction act = { .type = IDE_ACTION_TOGGLE_FILE_TREE };
  dispatch_ide_action(KAJO_IDE_WINDOW(d), &act);
}

static void on_hdr_map_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  if (!self || !self->source_map) return;
  gboolean next = !gtk_widget_get_visible(self->source_map);
  self->config_minimap_enabled = next;
  gtk_widget_set_visible(self->source_map, next);
  update_status_bar_and_hud(self);
}

static void on_hdr_find_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  ide_fsm_set_state(self->fsm, IDE_STATE_PICKER);
  ide_picker_open_files(self->picker, self->workspace_root);
  update_status_bar_and_hud(self);
}

static void on_hdr_md_clicked(GtkButton *b, gpointer d) {
  (void)b;
  IdeAction act = { .type = IDE_ACTION_TOGGLE_MARKDOWN_PREVIEW };
  dispatch_ide_action(KAJO_IDE_WINDOW(d), &act);
}

static void on_hdr_help_clicked(GtkButton *b, gpointer d) {
  (void)b;
  IdeAction act = { .type = IDE_ACTION_HELP_TOGGLE };
  dispatch_ide_action(KAJO_IDE_WINDOW(d), &act);
}

static void on_hdr_build_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  show_drawer_page(self, "search");
  ide_search_drawer_run_build(self->search_drawer);
}

static void on_hdr_float_clicked(GtkButton *b, gpointer d) {
  (void)b;
  (void)d;
  niri_toggle_window_floating();
}

static void on_hdr_close_clicked(GtkButton *b, gpointer d) {
  (void)b;
  gtk_window_close(GTK_WINDOW(d));
}

static void on_drawer_tab_clicked(GtkButton *b, gpointer d) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  const char *page = g_object_get_data(G_OBJECT(b), "drawer-page");
  if (!page) return;
  show_drawer_page(self, page);
  if (g_strcmp0(page, "markdown") == 0)
    sync_markdown_views(self);
}

static void on_drawer_close_clicked(GtkButton *b, gpointer d) {
  (void)b;
  KajoIdeWindow *self = KAJO_IDE_WINDOW(d);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), FALSE);
  gtk_widget_grab_focus(self->source_view);
}

static void kajo_ide_window_dispose(GObject *object) {
  KajoIdeWindow *self = KAJO_IDE_WINDOW(object);
  if (self->config_monitor) {
    g_file_monitor_cancel(self->config_monitor);
    g_clear_object(&self->config_monitor);
  }
  g_clear_pointer(&self->active_scheme_id, g_free);
  g_clear_pointer(&self->active_font_family, g_free);
  g_clear_pointer(&self->fsm, ide_fsm_free);
  g_clear_pointer(&self->tree, ide_tree_free);
  g_clear_pointer(&self->picker, ide_picker_free);
  g_clear_pointer(&self->search_drawer, ide_search_drawer_free);
  g_clear_pointer(&self->lsp, ide_lsp_manager_free);
  g_clear_pointer(&self->dap, ide_dap_view_free);
  g_clear_pointer(&self->agent_ipc, ide_agent_ipc_free);
  g_clear_pointer(&self->md_split_view, ide_markdown_view_free);
  g_clear_pointer(&self->md_drawer_view, ide_markdown_view_free);
  if (self->buffers) {
    g_ptr_array_free(self->buffers, TRUE);
    self->buffers = NULL;
  }
  if (self->jumplist) {
    g_ptr_array_free(self->jumplist, TRUE);
    self->jumplist = NULL;
  }
  if (self->search_query) {
    g_string_free(self->search_query, TRUE);
    self->search_query = NULL;
  }
  g_clear_pointer(&self->yank_register, g_free);
  g_clear_pointer(&self->workspace_root, g_free);
  G_OBJECT_CLASS(kajo_ide_window_parent_class)->dispose(object);
}

static void kajo_ide_window_class_init(KajoIdeWindowClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = kajo_ide_window_dispose;
}

static void on_editor_vadj_changed(GtkAdjustment *vadj, gpointer user_data) {
  KajoIdeWindow *self = user_data;
  if (!self || !gtk_widget_get_visible(self->md_split_container)) return;
  double upper = gtk_adjustment_get_upper(vadj);
  double page = gtk_adjustment_get_page_size(vadj);
  if (upper > page) {
    double frac = gtk_adjustment_get_value(vadj) / (upper - page);
    ide_markdown_view_sync_scroll(self->md_split_view, frac);
  }
}

static void on_source_view_pressed(GtkGestureClick *gesture,
                                   int              n_press,
                                   double           x,
                                   double           y,
                                   gpointer         user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  KajoIdeWindow *self = user_data;
  if (!self) return;
  gtk_widget_grab_focus(self->source_view);
  IdeState st = ide_fsm_get_state(self->fsm);
  if (st == IDE_STATE_TREE || st == IDE_STATE_DRAWER ||
      st == IDE_STATE_LEADER || st == IDE_STATE_MATCH_PENDING) {
    ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
    update_status_bar_and_hud(self);
  }
}

static void on_source_view_released(GtkGestureClick *gesture,
                                    int              n_press,
                                    double           x,
                                    double           y,
                                    gpointer         user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  KajoIdeWindow *self = user_data;
  if (!self) return;
  IdeBuffer *buf = get_active_buffer(self);
  if (!buf) return;
  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(ide_buffer_get_source_buffer(buf));
  gboolean has_sel = gtk_text_buffer_get_has_selection(tbuf);
  IdeState st = ide_fsm_get_state(self->fsm);
  if (has_sel && st == IDE_STATE_NORMAL) {
    ide_fsm_set_state(self->fsm, IDE_STATE_SELECT);
  } else if (!has_sel && st == IDE_STATE_SELECT) {
    ide_fsm_set_state(self->fsm, IDE_STATE_NORMAL);
  }
  update_status_bar_and_hud(self);
}

static void kajo_ide_window_init(KajoIdeWindow *self) {
  gtk_window_set_title(GTK_WINDOW(self), "Kajo IDE");
  gtk_window_set_default_size(GTK_WINDOW(self), 1320, 840);
  gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(self), "shell-settings-window");
  gtk_widget_add_css_class(GTK_WIDGET(self), "shell-ide-window");

  self->workspace_root = g_get_current_dir();
  self->fsm = ide_fsm_new();
  self->buffers = g_ptr_array_new_with_free_func((GDestroyNotify)ide_buffer_free);
  self->jumplist = g_ptr_array_new_with_free_func(jump_entry_free);
  self->jumplist_idx = -1;
  self->search_query = g_string_new("");
  self->config_minimap_enabled = TRUE;

  GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(self), root_vbox);

  /* 1. Aero + Metro Window Chrome Drag Header (3-Zone Layout) */
  GtkWidget *win_handle = gtk_window_handle_new();
  GtkWidget *hdr_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(hdr_box, "settings-header-bar");
  gtk_widget_set_valign(hdr_box, GTK_ALIGN_CENTER);

  /* Left Zone: Title + View Toggles */
  GtkWidget *title_lbl = gtk_label_new("⌨ KAJO IDE");
  gtk_widget_add_css_class(title_lbl, "settings-header-title");
  gtk_widget_set_valign(title_lbl, GTK_ALIGN_CENTER);

  GtkWidget *nav_cluster = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign(nav_cluster, GTK_ALIGN_CENTER);

  self->btn_hdr_tree = gtk_button_new_with_label("🌲 TREE");
  gtk_widget_set_focusable(self->btn_hdr_tree, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_tree, "files-nav-btn");
  gtk_widget_add_css_class(self->btn_hdr_tree, "active");
  gtk_widget_set_valign(self->btn_hdr_tree, GTK_ALIGN_CENTER);
  g_signal_connect(
      self->btn_hdr_tree, "clicked", G_CALLBACK(on_hdr_tree_clicked), self);

  self->btn_hdr_map = gtk_button_new_with_label("🗺 MAP");
  gtk_widget_set_focusable(self->btn_hdr_map, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_map, "files-nav-btn");
  gtk_widget_add_css_class(self->btn_hdr_map, "active");
  gtk_widget_set_valign(self->btn_hdr_map, GTK_ALIGN_CENTER);
  g_signal_connect(
      self->btn_hdr_map, "clicked", G_CALLBACK(on_hdr_map_clicked), self);

  self->btn_hdr_md = gtk_button_new_with_label("📖 MD");
  gtk_widget_set_focusable(self->btn_hdr_md, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_md, "files-nav-btn");
  gtk_widget_set_valign(self->btn_hdr_md, GTK_ALIGN_CENTER);
  g_signal_connect(
      self->btn_hdr_md, "clicked", G_CALLBACK(on_hdr_md_clicked), self);

  GtkWidget *btn_find = gtk_button_new_with_label("🔍 FIND");
  gtk_widget_set_focusable(btn_find, FALSE);
  gtk_widget_add_css_class(btn_find, "files-nav-btn");
  gtk_widget_set_valign(btn_find, GTK_ALIGN_CENTER);
  g_signal_connect(btn_find, "clicked", G_CALLBACK(on_hdr_find_clicked), self);

  self->btn_hdr_help = gtk_button_new_with_label("❓ HELP");
  gtk_widget_set_focusable(self->btn_hdr_help, FALSE);
  gtk_widget_add_css_class(self->btn_hdr_help, "files-nav-btn");
  gtk_widget_set_valign(self->btn_hdr_help, GTK_ALIGN_CENTER);
  g_signal_connect(
      self->btn_hdr_help, "clicked", G_CALLBACK(on_hdr_help_clicked), self);

  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_tree);
  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_map);
  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_md);
  gtk_box_append(GTK_BOX(nav_cluster), btn_find);
  gtk_box_append(GTK_BOX(nav_cluster), self->btn_hdr_help);

  /* Center Zone: Active File Breadcrumb + Git Branch & Status Summary Badge */
  GtkWidget *hdr_center = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_hexpand(hdr_center, TRUE);
  gtk_widget_set_halign(hdr_center, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(hdr_center, GTK_ALIGN_CENTER);

  self->hdr_breadcrumb = gtk_label_new("[No Name]");
  gtk_widget_add_css_class(self->hdr_breadcrumb, "ide-header-breadcrumb");
  gtk_widget_set_valign(self->hdr_breadcrumb, GTK_ALIGN_CENTER);

  self->ws_badge = gtk_label_new("⎇ main");
  gtk_widget_add_css_class(self->ws_badge, "shell-popover-badge");
  gtk_widget_add_css_class(self->ws_badge, "accent");
  gtk_widget_set_valign(self->ws_badge, GTK_ALIGN_CENTER);

  self->agent_hdr_badge = gtk_label_new("");
  gtk_widget_add_css_class(self->agent_hdr_badge, "shell-popover-badge");
  gtk_widget_add_css_class(self->agent_hdr_badge, "accent");
  gtk_widget_set_valign(self->agent_hdr_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->agent_hdr_badge, FALSE);

  gtk_box_append(GTK_BOX(hdr_center), self->hdr_breadcrumb);
  gtk_box_append(GTK_BOX(hdr_center), self->ws_badge);
  gtk_box_append(GTK_BOX(hdr_center), self->agent_hdr_badge);

  /* Right Zone: Primary Build Action + Window Controls */
  GtkWidget *btn_build = gtk_button_new_with_label("▶ BUILD");
  gtk_widget_set_focusable(btn_build, FALSE);
  gtk_widget_add_css_class(btn_build, "action-btn-primary");
  gtk_widget_set_valign(btn_build, GTK_ALIGN_CENTER);
  g_signal_connect(btn_build, "clicked", G_CALLBACK(on_hdr_build_clicked), self);

  GtkWidget *btn_float = gtk_button_new_with_label("🗗 FLOAT / TILE");
  gtk_widget_set_focusable(btn_float, FALSE);
  gtk_widget_add_css_class(btn_float, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_float, GTK_ALIGN_CENTER);
  g_signal_connect(btn_float, "clicked", G_CALLBACK(on_hdr_float_clicked), self);

  GtkWidget *btn_close = gtk_button_new_with_label("✕");
  gtk_widget_set_focusable(btn_close, FALSE);
  gtk_widget_add_css_class(btn_close, "settings-close-btn");
  gtk_widget_set_valign(btn_close, GTK_ALIGN_CENTER);
  g_signal_connect(btn_close, "clicked", G_CALLBACK(on_hdr_close_clicked), self);

  gtk_box_append(GTK_BOX(hdr_box), title_lbl);
  gtk_box_append(GTK_BOX(hdr_box), nav_cluster);
  gtk_box_append(GTK_BOX(hdr_box), hdr_center);
  gtk_box_append(GTK_BOX(hdr_box), btn_build);
  gtk_box_append(GTK_BOX(hdr_box), btn_float);
  gtk_box_append(GTK_BOX(hdr_box), btn_close);
  gtk_window_handle_set_child(GTK_WINDOW_HANDLE(win_handle), hdr_box);
  gtk_box_append(GTK_BOX(root_vbox), win_handle);

  /* 2. Multi-Buffer Tab Ribbon */
  self->tab_ribbon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(self->tab_ribbon_box, "ide-tab-ribbon");
  gtk_widget_set_valign(self->tab_ribbon_box, GTK_ALIGN_FILL);
  gtk_box_append(GTK_BOX(root_vbox), self->tab_ribbon_box);

  /* 3. Main Workspace Overlay & Resizable GtkPaned Splitters */
  GtkWidget *stage_overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(stage_overlay, TRUE);
  gtk_widget_set_vexpand(stage_overlay, TRUE);

  self->tree_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->tree_paned, "ide-paned");
  gtk_widget_set_hexpand(self->tree_paned, TRUE);
  gtk_widget_set_vexpand(self->tree_paned, TRUE);
  gtk_paned_set_wide_handle(GTK_PANED(self->tree_paned), TRUE);

  /* Left File Tree Sidebar */
  self->tree = ide_tree_new(self->workspace_root, on_tree_open_file, self);
  self->tree_container = ide_tree_get_widget(self->tree);
  gtk_paned_set_start_child(GTK_PANED(self->tree_paned), self->tree_container);
  gtk_paned_set_resize_start_child(GTK_PANED(self->tree_paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(self->tree_paned), FALSE);
  gtk_paned_set_position(GTK_PANED(self->tree_paned), 240);

  /* Right of Tree: Horizontal GtkPaned between Code Editor and Markdown Split */
  self->editor_md_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(self->editor_md_paned, "ide-paned");
  gtk_widget_set_hexpand(self->editor_md_paned, TRUE);
  gtk_widget_set_vexpand(self->editor_md_paned, TRUE);
  gtk_paned_set_wide_handle(GTK_PANED(self->editor_md_paned), TRUE);

  GtkWidget *editor_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(editor_hbox, TRUE);
  gtk_widget_set_vexpand(editor_hbox, TRUE);

  self->source_scroll = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(self->source_scroll, TRUE);
  gtk_widget_set_vexpand(self->source_scroll, TRUE);

  GtkAdjustment *ed_vadj =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->source_scroll));
  if (ed_vadj) {
    g_signal_connect(ed_vadj, "value-changed", G_CALLBACK(on_editor_vadj_changed), self);
  }

  self->source_view = gtk_source_view_new();
  gtk_widget_add_css_class(self->source_view, "ide-source-view");
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->source_view), TRUE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(self->source_view), 10);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(self->source_view), 10);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(self->source_view), 6);
  gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(self->source_view), TRUE);
  gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(self->source_view), 2);
  gtk_source_view_set_insert_spaces_instead_of_tabs(
      GTK_SOURCE_VIEW(self->source_view), TRUE);

  GtkGesture *ed_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(ed_click), 1);
  g_signal_connect(
      ed_click, "pressed", G_CALLBACK(on_source_view_pressed), self);
  g_signal_connect(
      ed_click, "released", G_CALLBACK(on_source_view_released), self);
  gtk_widget_add_controller(self->source_view, GTK_EVENT_CONTROLLER(ed_click));

  GtkSourceGutter *left_gutter = gtk_source_view_get_gutter(
      GTK_SOURCE_VIEW(self->source_view), GTK_TEXT_WINDOW_LEFT);
  self->gutter_renderer =
      KAJO_IDE_GUTTER_RENDERER(kajo_ide_gutter_renderer_new(NULL));
  gtk_source_gutter_insert(
      left_gutter, GTK_SOURCE_GUTTER_RENDERER(self->gutter_renderer), 0);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->source_scroll),
                                self->source_view);
  gtk_box_append(GTK_BOX(editor_hbox), self->source_scroll);

  self->source_map = gtk_source_map_new();
  gtk_widget_set_focusable(self->source_map, FALSE);
  gtk_widget_add_css_class(self->source_map, "ide-minimap");
  gtk_source_map_set_view(GTK_SOURCE_MAP(self->source_map),
                          GTK_SOURCE_VIEW(self->source_view));
  gtk_widget_set_size_request(self->source_map, 84, -1);
  gtk_box_append(GTK_BOX(editor_hbox), self->source_map);

  /* Side-by-Side Graphical Markdown & Vector Mermaid Split (Space m p) */
  self->md_split_view = ide_markdown_view_new(
      on_markdown_node_jump, on_markdown_checklist_edit, self);
  ide_markdown_view_set_open_file_cb(self->md_split_view, on_picker_open_file);
  self->md_split_container = ide_markdown_view_get_widget(self->md_split_view);
  gtk_widget_set_size_request(self->md_split_container, 280, -1);
  gtk_widget_set_visible(self->md_split_container, FALSE);

  gtk_paned_set_start_child(GTK_PANED(self->editor_md_paned), editor_hbox);
  gtk_paned_set_end_child(GTK_PANED(self->editor_md_paned), self->md_split_container);
  gtk_paned_set_resize_start_child(GTK_PANED(self->editor_md_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(self->editor_md_paned), FALSE);
  gtk_paned_set_resize_end_child(GTK_PANED(self->editor_md_paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(self->editor_md_paned), FALSE);
  gtk_paned_set_position(GTK_PANED(self->editor_md_paned), 540);

  gtk_paned_set_end_child(GTK_PANED(self->tree_paned), self->editor_md_paned);
  gtk_paned_set_resize_end_child(GTK_PANED(self->tree_paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(self->tree_paned), FALSE);

  gtk_overlay_set_child(GTK_OVERLAY(stage_overlay), self->tree_paned);

  /* Centered Fuzzy Picker Overlay */
  self->picker = ide_picker_new(
      self->workspace_root, on_picker_open_file, on_picker_command, self);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage_overlay),
                          ide_picker_get_widget(self->picker));

  /* Bottom-Right Which-Key Chord Helper HUD */
  self->chord_hud_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(self->chord_hud_box, "ide-chord-hud");
  gtk_widget_set_halign(self->chord_hud_box, GTK_ALIGN_END);
  gtk_widget_set_valign(self->chord_hud_box, GTK_ALIGN_END);
  gtk_widget_set_margin_end(self->chord_hud_box, 18);
  gtk_widget_set_margin_bottom(self->chord_hud_box, 18);
  gtk_widget_set_visible(self->chord_hud_box, FALSE);

  self->chord_hud_title = gtk_label_new("LEADER CHORDS");
  gtk_widget_add_css_class(self->chord_hud_title, "shell-popover-badge");
  gtk_widget_set_halign(self->chord_hud_title, GTK_ALIGN_START);

  self->chord_hud_grid = gtk_label_new(
      "f / SPC  Find Files      b  Buffers         /  Live Grep\n"
      "e        File Tree       a  Alternate C/H   w  Save File\n"
      "m p      Markdown GUI    s  Search/Replace  d  Agent / Debug\n"
      "c        Code Actions    o  Build Output    q  Close Buffer");
  gtk_widget_set_halign(self->chord_hud_grid, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(self->chord_hud_box), self->chord_hud_title);
  gtk_box_append(GTK_BOX(self->chord_hud_box), self->chord_hud_grid);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage_overlay), self->chord_hud_box);

  /* Top-Right Toast / External Reload Banner */
  self->toast_banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->toast_banner, "ide-chord-hud");
  gtk_widget_set_halign(self->toast_banner, GTK_ALIGN_END);
  gtk_widget_set_valign(self->toast_banner, GTK_ALIGN_START);
  gtk_widget_set_margin_top(self->toast_banner, 12);
  gtk_widget_set_margin_end(self->toast_banner, 16);
  gtk_widget_set_visible(self->toast_banner, FALSE);
  self->toast_label = gtk_label_new("");
  gtk_box_append(GTK_BOX(self->toast_banner), self->toast_label);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage_overlay), self->toast_banner);

  /* 4. Contextual Bottom Drawer inside Resizable Vertical GtkPaned */
  self->drawer_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(self->drawer_revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->drawer_revealer), FALSE);

  GtkWidget *drawer_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(drawer_outer, "ide-drawer-shell");

  /* Unified Drawer Tab Switcher Bar */
  GtkWidget *drawer_tab_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(drawer_tab_bar, "ide-drawer-tab-strip");
  gtk_widget_set_valign(drawer_tab_bar, GTK_ALIGN_CENTER);

  self->drawer_tab_agent = gtk_button_new_with_label("🤖 AGENT DIFF (Space d s)");
  gtk_widget_set_focusable(self->drawer_tab_agent, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_agent, "ide-drawer-tab");
  gtk_widget_set_valign(self->drawer_tab_agent, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_agent), "drawer-page", (gpointer)"agent");
  g_signal_connect(
      self->drawer_tab_agent, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  self->drawer_tab_dap = gtk_button_new_with_label("🐞 DAP DEBUGGER (Space d v)");
  gtk_widget_set_focusable(self->drawer_tab_dap, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_dap, "ide-drawer-tab");
  gtk_widget_set_valign(self->drawer_tab_dap, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_dap), "drawer-page", (gpointer)"dap");
  g_signal_connect(
      self->drawer_tab_dap, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  self->drawer_tab_search = gtk_button_new_with_label("🔎 SEARCH / BUILD (Space s r)");
  gtk_widget_set_focusable(self->drawer_tab_search, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_search, "ide-drawer-tab");
  gtk_widget_add_css_class(self->drawer_tab_search, "active");
  gtk_widget_set_valign(self->drawer_tab_search, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_search), "drawer-page", (gpointer)"search");
  g_signal_connect(
      self->drawer_tab_search, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  self->drawer_tab_md = gtk_button_new_with_label("❖ MERMAID / MD (Space m m)");
  gtk_widget_set_focusable(self->drawer_tab_md, FALSE);
  gtk_widget_add_css_class(self->drawer_tab_md, "ide-drawer-tab");
  gtk_widget_set_valign(self->drawer_tab_md, GTK_ALIGN_CENTER);
  g_object_set_data(G_OBJECT(self->drawer_tab_md), "drawer-page", (gpointer)"markdown");
  g_signal_connect(
      self->drawer_tab_md, "clicked", G_CALLBACK(on_drawer_tab_clicked), self);

  GtkWidget *dr_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(dr_spacer, TRUE);

  GtkWidget *dr_close = gtk_button_new_with_label("✕ CLOSE (Esc)");
  gtk_widget_set_focusable(dr_close, FALSE);
  gtk_widget_add_css_class(dr_close, "ide-toolbar-btn");
  gtk_widget_set_valign(dr_close, GTK_ALIGN_CENTER);
  g_signal_connect(dr_close, "clicked", G_CALLBACK(on_drawer_close_clicked), self);

  gtk_box_append(GTK_BOX(drawer_tab_bar), self->drawer_tab_agent);
  gtk_box_append(GTK_BOX(drawer_tab_bar), self->drawer_tab_dap);
  gtk_box_append(GTK_BOX(drawer_tab_bar), self->drawer_tab_search);
  gtk_box_append(GTK_BOX(drawer_tab_bar), self->drawer_tab_md);
  gtk_box_append(GTK_BOX(drawer_tab_bar), dr_spacer);
  gtk_box_append(GTK_BOX(drawer_tab_bar), dr_close);

  self->drawer_stack = gtk_stack_new();
  self->search_drawer = ide_search_drawer_new(
      self->workspace_root, on_picker_open_file, NULL, self);
  self->dap = ide_dap_view_new(on_dap_action_cb, self);
  self->agent_ipc = ide_agent_ipc_new(
      on_picker_open_file, on_agent_reload_cb, on_agent_status_cb, self);
  ide_agent_ipc_start_default(self->agent_ipc);
  self->md_drawer_view = ide_markdown_view_new(
      on_markdown_node_jump, on_markdown_checklist_edit, self);
  ide_markdown_view_set_open_file_cb(self->md_drawer_view, on_picker_open_file);

  gtk_stack_add_named(GTK_STACK(self->drawer_stack),
                      ide_search_drawer_get_widget(self->search_drawer),
                      "search");
  gtk_stack_add_named(GTK_STACK(self->drawer_stack),
                      ide_dap_view_get_widget(self->dap),
                      "dap");
  gtk_stack_add_named(GTK_STACK(self->drawer_stack),
                      ide_agent_ipc_get_drawer_widget(self->agent_ipc),
                      "agent");
  gtk_stack_add_named(GTK_STACK(self->drawer_stack),
                      ide_markdown_view_get_widget(self->md_drawer_view),
                      "markdown");

  gtk_box_append(GTK_BOX(drawer_outer), drawer_tab_bar);
  gtk_box_append(GTK_BOX(drawer_outer), self->drawer_stack);
  gtk_revealer_set_child(GTK_REVEALER(self->drawer_revealer), drawer_outer);

  self->vertical_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class(self->vertical_paned, "ide-paned");
  gtk_widget_set_hexpand(self->vertical_paned, TRUE);
  gtk_widget_set_vexpand(self->vertical_paned, TRUE);
  gtk_paned_set_wide_handle(GTK_PANED(self->vertical_paned), TRUE);
  gtk_paned_set_start_child(GTK_PANED(self->vertical_paned), stage_overlay);
  gtk_paned_set_end_child(GTK_PANED(self->vertical_paned), self->drawer_revealer);
  gtk_paned_set_resize_start_child(GTK_PANED(self->vertical_paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(self->vertical_paned), FALSE);
  gtk_paned_set_resize_end_child(GTK_PANED(self->vertical_paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(self->vertical_paned), FALSE);

  gtk_box_append(GTK_BOX(root_vbox), self->vertical_paned);

  /* 5. Inline Search Bar (/ query) */
  self->inline_search_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->inline_search_bar, "ide-drawer-header");
  gtk_widget_set_visible(self->inline_search_bar, FALSE);
  self->inline_search_entry = gtk_label_new("/");
  gtk_box_append(GTK_BOX(self->inline_search_bar), self->inline_search_entry);
  gtk_box_append(GTK_BOX(root_vbox), self->inline_search_bar);

  /* 6. Bottom Status Bar (Flush Mode Pill on Far Left) */
  GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(status_bar, "ide-status-bar");
  gtk_widget_set_valign(status_bar, GTK_ALIGN_FILL);

  self->mode_pill = gtk_label_new("NORMAL");
  gtk_widget_add_css_class(self->mode_pill, "ide-mode-pill");
  gtk_widget_add_css_class(self->mode_pill, "mode-normal");
  gtk_widget_set_valign(self->mode_pill, GTK_ALIGN_FILL);

  self->chord_badge = gtk_label_new("");
  gtk_widget_add_css_class(self->chord_badge, "shell-popover-badge");
  gtk_widget_set_valign(self->chord_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->chord_badge, FALSE);

  self->file_status_label = gtk_label_new("[No Name]");
  gtk_widget_add_css_class(self->file_status_label, "ide-status-file");
  gtk_widget_set_valign(self->file_status_label, GTK_ALIGN_CENTER);

  GtkWidget *st_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(st_spacer, TRUE);

  self->agent_status_badge = gtk_label_new("");
  gtk_widget_add_css_class(self->agent_status_badge, "shell-popover-badge");
  gtk_widget_add_css_class(self->agent_status_badge, "accent");
  gtk_widget_set_valign(self->agent_status_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->agent_status_badge, FALSE);

  self->lsp_status_badge = gtk_label_new("LSP: READY");
  gtk_widget_add_css_class(self->lsp_status_badge, "shell-popover-badge");
  gtk_widget_set_valign(self->lsp_status_badge, GTK_ALIGN_CENTER);

  self->diag_status_label = gtk_label_new("✖ 0  ⚠ 0");
  gtk_widget_add_css_class(self->diag_status_label, "shell-popover-badge");
  gtk_widget_set_valign(self->diag_status_label, GTK_ALIGN_CENTER);

  self->cursor_pos_label = gtk_label_new("UTF-8 • LF • Ln 1, Col 1");
  gtk_widget_add_css_class(self->cursor_pos_label, "ide-status-cursor");
  gtk_widget_set_valign(self->cursor_pos_label, GTK_ALIGN_CENTER);

  gtk_box_append(GTK_BOX(status_bar), self->mode_pill);
  gtk_box_append(GTK_BOX(status_bar), self->chord_badge);
  gtk_box_append(GTK_BOX(status_bar), self->file_status_label);
  gtk_box_append(GTK_BOX(status_bar), st_spacer);
  gtk_box_append(GTK_BOX(status_bar), self->agent_status_badge);
  gtk_box_append(GTK_BOX(status_bar), self->lsp_status_badge);
  gtk_box_append(GTK_BOX(status_bar), self->diag_status_label);
  gtk_box_append(GTK_BOX(status_bar), self->cursor_pos_label);
  gtk_box_append(GTK_BOX(root_vbox), status_bar);

  /* LSP Manager */
  self->lsp = ide_lsp_manager_new(
      self->workspace_root, on_picker_open_file, on_lsp_status_changed, self);

  /* Initial empty buffer */
  IdeBuffer *init_buf = ide_buffer_new(NULL);
  g_ptr_array_add(self->buffers, init_buf);
  self->active_buf_idx = 0;

  /* Apply IDE Config & Start Live GFileMonitor on ~/.config/kajo/config.json */
  apply_ide_config(self);
  g_autofree char *cfg_path =
      g_build_filename(g_get_user_config_dir(), "kajo", "config.json", NULL);
  g_autoptr(GFile) cfg_file = g_file_new_for_path(cfg_path);
  self->config_monitor =
      g_file_monitor_file(cfg_file, G_FILE_MONITOR_NONE, NULL, NULL);
  if (self->config_monitor) {
    g_signal_connect(self->config_monitor,
                     "changed",
                     G_CALLBACK(on_config_file_changed),
                     self);
  }

  bind_active_buffer_to_view(self);

  /* Capture-phase Modal FSM Key Controller */
  GtkEventController *key_ctrl = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
  g_signal_connect(
      key_ctrl, "key-pressed", G_CALLBACK(on_window_key_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), key_ctrl);
}

KajoIdeWindow *kajo_ide_window_new(GtkApplication *app,
                                   const char     *workspace_dir) {
  KajoIdeWindow *win =
      g_object_new(KAJO_TYPE_IDE_WINDOW, "application", app, NULL);
  if (workspace_dir && *workspace_dir &&
      g_file_test(workspace_dir, G_FILE_TEST_IS_DIR)) {
    g_free(win->workspace_root);
    win->workspace_root = g_strdup(workspace_dir);
    ide_tree_set_workspace_root(win->tree, workspace_dir);
    ide_search_drawer_set_workspace_root(win->search_drawer, workspace_dir);
    update_status_bar_and_hud(win);
  }
  return win;
}
