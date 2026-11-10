#ifndef KAJO_IDE_WINDOW_PRIVATE_H
#define KAJO_IDE_WINDOW_PRIVATE_H

/* Internal to src/ide/: the KajoIdeWindow instance struct and helpers shared
 * between ide_window.c and ide_actions.c. Not a public API. */

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


G_BEGIN_DECLS

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

  /* Right-click code menu */
  gboolean         code_menu_built;
  GActionGroup    *code_actions;

  /* Live Settings & Config Hot-Reload */
  guint            config_sub_id;
  GtkCssProvider  *editor_font_provider;
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

  /* Leader timeout & Drawer state */
  guint            leader_timer_id;
  gboolean         drawer_maximized;
  int              saved_drawer_pos;
};


/* Shared helpers (defined in ide_window.c) */
IdeBuffer *ide_window_get_active_buffer(KajoIdeWindow *self);
void       ide_window_record_jumplist_here(KajoIdeWindow *self);
void       ide_window_show_toast(KajoIdeWindow *self, const char *msg);
void       ide_window_sync_markdown_views(KajoIdeWindow *self);
void       ide_window_zoom_editor_font(KajoIdeWindow *self, int delta_or_zero);
void       ide_window_update_status_bar_and_hud(KajoIdeWindow *self);
void       ide_window_rebuild_tab_ribbon(KajoIdeWindow *self);
void       ide_window_bind_active_buffer_to_view(KajoIdeWindow *self);
void       ide_window_apply_config(KajoIdeWindow *self);
void       ide_window_get_cursor_line_col(KajoIdeWindow *self, guint *out_line, guint *out_col);
void       ide_window_move_cursor_to_line_col(KajoIdeWindow *self, guint line_1based, guint col_1based);

/* Defined in ide_actions.c */
void       ide_window_show_drawer_page(KajoIdeWindow *self, const char *page_name);
void       ide_window_dispatch_action(KajoIdeWindow *self, const IdeAction *act);

G_END_DECLS

#endif /* KAJO_IDE_WINDOW_PRIVATE_H */
