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

#include "../config.h"
#include "../ide/ide_buffer.h"
#include "../ide/ide_editor_ops.h"
#include "../ide/ide_fsm.h"
#include "../ide/ide_gutter.h"
#include "../ide/ide_lsp_client.h"
#include "../ide/ide_tree.h"

G_BEGIN_DECLS

typedef struct {
  char     *file;
  guint     line;
  uint32_t  addr;
  bool      armed;
} AmigaSourceBp;

struct _KajoAmigaWindow {
  GtkApplicationWindow  parent_instance;

  /* Core non-UI state */
  AmigaProject         *project;
  AmigaToolchainStatus *toolchain;
  AmigaSymbolTable     *symbols;
  AmigaRunner          *runner;
  AmigaRspClient       *rsp;
  GCancellable         *build_cancel;
  GHashTable           *bp_addrs;   /* GUINT_TO_POINTER(even_addr) -> 1 */
  GPtrArray            *source_bps; /* AmigaSourceBp* */
  GPtrArray            *last_disasm_rows; /* AmigaDisasmRow* */

  bool                  attach_mode;
  char                 *attach_host;
  uint16_t              attach_port;
  bool                  building;
  bool                  launch_after_build;
  bool                  narrow_mode;
  guint                 toast_timer_id;
  guint                 cfg_sub_id;
  guint                 connect_retry_timer_id;
  int                   connect_retries_left;
  char                 *pending_entry_exe;
  uint32_t              temp_run_to_cursor_addr;
  uint8_t               exc_mask;
  bool                  updating_exc_ui;

  AmigaCpuRegs          last_regs;
  bool                  has_last_regs;
  AmigaExecStatus       last_exec_st;
  bool                  has_last_exec_st;

  /* Global amiga.toml / ide.toml preferences */
  char                 *cfg_amiberry_bin;
  char                 *cfg_kickstart_a500;
  char                 *cfg_kickstart_a1200;
  char                 *cfg_gdb_host;
  uint16_t              cfg_gdb_port;
  KajoAmigaEntryBreak   cfg_entry_break_mode;
  bool                  cfg_capture_guest_output;
  bool                  cfg_enable_trace_history;
  uint8_t               cfg_default_exc_mask;
  bool                  cfg_trace_blitter_writes;
  int                   cfg_hex_grouping;
  bool                  cfg_bitplane_grid_16px;
  bool                  cfg_enable_m68k_lsp;
  bool                  cfg_enable_clangd;
  bool                  show_cycle_gutter;
  bool                  show_sel_cycles;
  bool                  enable_hover_decoders;
  char                 *inherited_scheme;
  int                   inherited_tab_width;

  /* Optional LSP clients */
  IdeLspClient         *lsp_clangd;
  IdeLspClient         *lsp_m68k;
  char                 *lsp_root_clangd;
  char                 *lsp_root_m68k;
  char                 *lsp_open_uri_c;
  char                 *lsp_open_uri_m68k;
  gint                  lsp_doc_ver;

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
  GtkWidget            *btn_pause;
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
void amiga_window_open_file_at_line(KajoAmigaWindow *self, const char *abs_path, guint line_1based);
void amiga_window_append_console(KajoAmigaWindow *self, const char *line);
void amiga_window_trigger_build(KajoAmigaWindow *self);
const AmigaTarget    *amiga_window_get_selected_target(KajoAmigaWindow *self);
const AmigaHwProfile *amiga_window_get_selected_profile(KajoAmigaWindow *self);

/* Debug / RSP / LSP engine (amiga_window_debug.c) */
void amiga_window_debug_init(KajoAmigaWindow *self);
void amiga_window_debug_dispose(KajoAmigaWindow *self);
void amiga_window_debug_reload_symbols(KajoAmigaWindow *self);
void amiga_window_debug_sync_ui(KajoAmigaWindow *self);
void amiga_window_debug_refresh_snapshot(KajoAmigaWindow *self);
void amiga_window_debug_start_or_continue(KajoAmigaWindow *self);
void amiga_window_debug_pause(KajoAmigaWindow *self);
void amiga_window_debug_stop(KajoAmigaWindow *self);
void amiga_window_debug_step_over(KajoAmigaWindow *self);
void amiga_window_debug_step_into(KajoAmigaWindow *self);
void amiga_window_debug_launch_after_build(KajoAmigaWindow *self);
void amiga_window_debug_connect_with_retry(KajoAmigaWindow *self,
                                           const char      *host,
                                           uint16_t         port,
                                           bool             attach_mode,
                                           const char      *entry_exe);
void amiga_window_debug_toggle_source_bp(KajoAmigaWindow *self,
                                         IdeBuffer       *buf,
                                         guint            line_1based);
void amiga_window_debug_sync_buffer_breakpoints(KajoAmigaWindow *self, IdeBuffer *buf);
void amiga_window_debug_on_rsp_stop(AmigaRspClient       *client,
                                    const AmigaStopEvent *ev,
                                    gpointer              ud);
void amiga_window_debug_on_disasm_rcmd(GObject *src, GAsyncResult *res, gpointer ud);
void amiga_window_debug_fill_disasm_callbacks(AmigaDisasmCallbacks *cbs);
void amiga_window_debug_on_exc_toggled(GtkCheckButton *cb, gpointer ud);
void amiga_window_debug_apply_exc_mask(KajoAmigaWindow *self, uint8_t mask);

void amiga_window_lsp_sync_project(KajoAmigaWindow *self);
void amiga_window_lsp_notify_open(KajoAmigaWindow *self, IdeBuffer *buf);
void amiga_window_lsp_notify_save(KajoAmigaWindow *self, IdeBuffer *buf);
void amiga_window_lsp_shutdown(KajoAmigaWindow *self);

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
