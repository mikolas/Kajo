#ifndef KAJO_AMIGA_WINDOW_PRIVATE_H
#define KAJO_AMIGA_WINDOW_PRIVATE_H

#include "amiga_bitplane_view.h"
#include "amiga_build.h"
#include "amiga_chips_view.h"
#include "amiga_copper_view.h"
#include "amiga_cpu_strip.h"
#include "amiga_decode.h"
#include "amiga_disasm.h"
#include "amiga_disasm_view.h"
#include "amiga_dma_view.h"
#include "amiga_hrm_view.h"
#include "amiga_mem_view.h"
#include "amiga_project.h"
#include "amiga_project_ui.h"
#include "amiga_rsp.h"
#include "amiga_runner.h"
#include "amiga_symbols.h"
#include "amiga_window.h"

#include "../ide/ide_buffer.h"
#include "../ide/ide_editor_ops.h"
#include "../ide/ide_fsm.h"
#include "../ide/ide_gutter.h"
#include "../ide/ide_tree.h"

G_BEGIN_DECLS

struct _KajoAmigaWindow {
  GtkApplicationWindow  parent_instance;

  /* Core non-UI state */
  AmigaProject         *project;
  AmigaToolchainStatus *toolchain;
  AmigaSymbolTable     *symbols;
  AmigaRunner          *runner;
  AmigaRspClient       *rsp;
  GCancellable         *build_cancel;
  GHashTable           *bp_addrs; /* GUINT_TO_POINTER(even_addr) -> 1 */

  bool                  attach_mode;
  char                 *attach_host;
  uint16_t              attach_port;
  bool                  building;
  bool                  narrow_mode;
  guint                 toast_timer_id;

  /* Shared editor state */
  IdeBuffer            *active_buf;
  IdeFsm               *fsm;
  char                 *yank_reg;
  IdeEditorHost         editor_host;

  /* Top chrome */
  GtkWidget            *root_stack;     /* "welcome" | "workspace" */
  GtkWidget            *welcome_slot;
  GtkWidget            *btn_proj_menu;
  GtkWidget            *btn_hdr_tree;
  GtkWidget            *dd_target;
  GtkWidget            *dd_profile;
  GtkWidget            *compat_badge;
  GtkWidget            *btn_build;
  GtkWidget            *btn_debug;
  GtkWidget            *btn_stop;
  GtkWidget            *btn_attach;
  GtkWidget            *btn_snapshot;
  GtkWidget            *btn_narrow_switch;
  AmigaCpuStrip        *cpu_strip;

  /* Workspace layout */
  GtkWidget            *main_paned;     /* sidebar | right-half */
  GtkWidget            *vert_paned;     /* center+inspector | drawer */
  GtkWidget            *center_paned;   /* center_stack | inspector */
  GtkWidget            *sidebar_box;
  GtkWidget            *sidebar_stack;  /* "project" | "attach" */
  GtkWidget            *tree_slot;
  IdeTree              *file_tree;
  GtkWidget            *outline_list;
  GtkWidget            *attach_list;

  GtkWidget            *center_stack;   /* "source" | "disasm" | "setup" */
  GtkWidget            *btn_tab_source;
  GtkWidget            *btn_tab_disasm;
  GtkWidget            *btn_tab_setup;
  GtkWidget            *source_view;
  KajoIdeGutterRenderer *gutter;
  AmigaDisasmView      *disasm_view;
  AmigaProjectSetupView *setup_view;

  /* Right 5-Tab Hardware Inspector (M4) */
  GtkWidget            *inspector_box;
  GtkWidget            *insp_stack;     /* "bpl" | "cop" | "chips" | "dma" | "hrm" */
  GtkWidget            *insp_tab_btns[5];
  AmigaBitplaneView    *bpl_view;
  AmigaCopperView      *cop_view;
  AmigaChipsView       *chips_view;
  AmigaDmaView         *dma_view;
  AmigaHrmView         *hrm_view;
  AmigaCustomSnapshot   last_custom;
  bool                  has_custom;
  GBytes               *last_chip_ram;
  uint32_t              last_chip_base;

  /* Drawer */
  GtkWidget            *drawer_stack;   /* "build" | "hex" | "debug" */
  GtkWidget            *btn_drw_build;
  GtkWidget            *btn_drw_hex;
  GtkWidget            *btn_drw_debug;
  GtkWidget            *console_view;
  GtkWidget            *diag_list;
  GPtrArray            *diags;          /* AmigaBuildDiag* */
  AmigaMemView         *mem_view;
  GtkWidget            *exc_checks[5];
  GtkWidget            *lbl_exc_last;

  /* Status bar & Toast */
  GtkWidget            *mode_pill;
  GtkWidget            *lbl_ws_state;
  GtkWidget            *lbl_cursor;
  GtkWidget            *lbl_sel_cycles;
  GtkWidget            *toast_bar;
  GtkWidget            *toast_lbl;
};

/* Shared internal helpers exposed across amiga_window*.c */
void amiga_window_show_toast(KajoAmigaWindow *self, const char *msg);
void amiga_window_sync_header_workspace(KajoAmigaWindow *self, bool in_ws);
void amiga_window_sync_mode_pill(KajoAmigaWindow *self);
void amiga_window_populate_project_dropdowns(KajoAmigaWindow *self);
void amiga_window_refresh_source_gutter_and_outline(KajoAmigaWindow *self);
void amiga_window_jump_mem(KajoAmigaWindow *self, uint32_t addr);
void amiga_window_act_new_project(KajoAmigaWindow *self);

/* Right Inspector module (amiga_window_inspector.c) */
GtkWidget *amiga_window_inspector_build(KajoAmigaWindow *self);
void       amiga_window_inspector_select_tab(KajoAmigaWindow *self, int idx);
void       amiga_window_inspector_apply_snapshot(KajoAmigaWindow           *self,
                                                 const AmigaCustomSnapshot *snap,
                                                 bool                       fetch_live_ram);
void       amiga_window_inspector_lookup_hrm(KajoAmigaWindow *self, const char *query);

gboolean   amiga_window_gutter_cycle_annotate(IdeBuffer *buf,
                                              guint      line_1based,
                                              char      *out_text,
                                              gsize      out_cap,
                                              double    *out_r,
                                              double    *out_g,
                                              double    *out_b,
                                              gpointer   ud);
gboolean   amiga_window_source_query_tooltip(GtkWidget  *widget,
                                             gint        x,
                                             gint        y,
                                             gboolean    keyboard_mode,
                                             GtkTooltip *tooltip,
                                             gpointer    ud);

G_END_DECLS

#endif /* KAJO_AMIGA_WINDOW_PRIVATE_H */
