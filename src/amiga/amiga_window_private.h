#ifndef KAJO_AMIGA_WINDOW_PRIVATE_H
#define KAJO_AMIGA_WINDOW_PRIVATE_H

#include "amiga_bitplane_view.h"
#include "amiga_build.h"
#include "amiga_chips_view.h"
#include "amiga_copper_view.h"
#include "amiga_debug_sidebar.h"
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

typedef enum {
  AMIGA_SESSION_WELCOME = 0,
  AMIGA_SESSION_EDIT,
  AMIGA_SESSION_BUILD_ERR,
  AMIGA_SESSION_SETUP,
  AMIGA_SESSION_RUNNING,
  AMIGA_SESSION_PAUSED,
  AMIGA_SESSION_ATTACH,
} AmigaSessionState;

struct _KajoAmigaWindow {
  GtkApplicationWindow  parent_instance;

  /* Core non-UI state */
  AmigaProject         *project;
  AmigaToolchainStatus *toolchain;
  AmigaSymbolTable     *symbols;
  AmigaRunner          *runner;
  AmigaRspClient       *rsp;
  GCancellable         *build_cancel;
  GCancellable         *search_cancel;
  GHashTable           *bp_addrs;         /* GUINT_TO_POINTER(even_addr) -> 1 */
  GPtrArray            *source_bps;       /* AmigaSourceBp* */
  GPtrArray            *watches;          /* AmigaWatchItem* */
  GPtrArray            *watchpoints;      /* AmigaWatchpointItem* */
  GPtrArray            *last_disasm_rows; /* AmigaDisasmRow* */

  AmigaSessionState     session_state;
  bool                  attach_mode;
  char                 *attach_host;
  uint16_t              attach_port;
  bool                  building;
  bool                  launch_after_build;
  bool                  launch_without_entry_break;
  bool                  snapshot_auto_resume;
  int                   snap_pending_ops;
  bool                  inspector_frozen_snap;
  uint32_t              frozen_snap_frame;
  uint32_t              frozen_snap_vpos;
  bool                  step_insn_mode;
  bool                  user_sidebar_open;
  bool                  user_drawer_open;
  bool                  user_inspector_open;
  bool                  updating_layout_toggles;
  bool                  setup_tab_open;
  bool                  disasm_tab_open;
  bool                  narrow_mode;
  int                   last_alloc_w;
  int                   active_half_pane;
  bool                  reflow_idle_queued;
  guint                 toast_timer_id;
  guint                 cfg_sub_id;
  guint                 connect_retry_timer_id;
  int                   connect_retries_left;
  char                 *pending_entry_exe;
  uint32_t              temp_run_to_cursor_addr;
  uint32_t              pending_step_out_addr;
  uint8_t               exc_mask;
  uint32_t              first_hunk_addr;

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
  char                 *cfg_c_include_dirs;
  char                 *cfg_asm_include_dirs;
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

  /* Multi-file open editor buffers */
  GPtrArray            *open_bufs;        /* IdeBuffer* */
  IdeBuffer            *active_buf;       /* borrowed from open_bufs */
  IdeFsm               *fsm;
  char                 *yank_reg;
  IdeEditorHost         editor_host;

  /* Top 3-Zone Header Bar */
  GtkWidget            *root_stack;       /* "welcome" | "workspace" */
  GtkWidget            *welcome_slot;
  GtkWidget            *btn_proj_menu;
  GtkWidget            *dd_target;
  GtkWidget            *dd_profile;
  GtkWidget            *compat_badge;

  /* Center fixed transport cluster */
  GtkWidget            *transport_box;
  GtkWidget            *offline_grp;
  GtkWidget            *live_grp;
  GtkWidget            *btn_build;
  GtkWidget            *btn_run;
  GtkWidget            *btn_debug;
  GtkWidget            *lbl_state_badge;
  GtkWidget            *btn_resume_pause;
  GtkWidget            *btn_step_over;
  GtkWidget            *btn_step_into;
  GtkWidget            *btn_step_out;
  GtkWidget            *btn_restart;
  GtkWidget            *btn_snapshot;
  GtkWidget            *btn_stop;
  GtkWidget            *btn_attach_detach;

  /* Right header toggles & icon-only chrome */
  GtkWidget            *layout_toggles_box;
  GtkWidget            *btn_tog_left;
  GtkWidget            *btn_tog_bottom;
  GtkWidget            *btn_tog_right;

  /* Half-screen (<900px) switcher strip */
  GtkWidget            *half_strip_box;
  GtkWidget            *btn_half_panes[5];

  /* Workspace layout */
  GtkWidget            *main_paned;       /* sidebar | center+inspector */
  GtkWidget            *vert_paned;       /* workspace body | bottom drawer */
  GtkWidget            *center_paned;     /* center_col | inspector */
  GtkWidget            *sidebar_box;
  GtkWidget            *sidebar_tabs_bar;
  GtkWidget            *btn_sb_files;
  GtkWidget            *btn_sb_search;
  GtkWidget            *btn_sb_debug;
  GtkWidget            *btn_sb_tasks;
  GtkWidget            *sidebar_stack;    /* "files" | "search" | "debug" */
  GtkWidget            *tree_slot;
  IdeTree              *file_tree;
  GtkWidget            *outline_list;
  GtkWidget            *search_entry;
  GtkWidget            *search_list;
  AmigaDebugSidebar    *debug_sidebar;

  /* Center multi-tab ribbon & views */
  GtkWidget            *center_col;
  GtkWidget            *center_tabs_bar;
  GtkWidget            *center_tabs_box;  /* scrollable file tabs + setup tab */
  GtkWidget            *btn_step_granularity;
  GtkWidget            *btn_toggle_disasm;
  GtkWidget            *center_stack;     /* "source" | "disasm" | "setup" */
  GtkWidget            *source_view;
  KajoIdeGutterRenderer *gutter;
  AmigaDisasmView      *disasm_view;
  AmigaProjectSetupView *setup_view;

  /* Right Custom-Chip-Only Hardware Inspector */
  GtkWidget            *inspector_box;
  GtkWidget            *lbl_insp_snap_badge;
  GtkWidget            *insp_stack;       /* "bpl" | "cop" | "chips" | "dma" | "hrm" */
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

  /* Bottom Ephemeral Drawer */
  GtkWidget            *drawer_box;
  GtkWidget            *drawer_stack;     /* "mem" | "trace" | "rcmd" | "build" */
  GtkWidget            *btn_drw_mem;
  GtkWidget            *btn_drw_trace;
  GtkWidget            *btn_drw_rcmd;
  GtkWidget            *btn_drw_build;
  GtkWidget            *console_view;
  GtkWidget            *diag_list;
  GtkWidget            *btn_diag_all;
  GtkWidget            *btn_diag_err;
  GtkWidget            *btn_diag_warn;
  int                   diag_filter;      /* 0=all, 1=err, 2=warn */
  int                   diag_cursor;
  GPtrArray            *diags;            /* AmigaBuildDiag* */
  AmigaMemView         *mem_view;
  GtkWidget            *trace_list;
  GtkWidget            *btn_trace_toggle;
  GtkWidget            *lbl_trace_count;
  GtkWidget            *rcmd_view;
  GtkWidget            *rcmd_entry;

  /* Interactive Status Bar & Toast */
  GtkWidget            *mode_pill;
  GtkWidget            *lbl_ws_state;
  GtkWidget            *lbl_lsp_status;
  GtkWidget            *btn_sel_cycles;
  GtkWidget            *btn_diag_count;
  GtkWidget            *btn_hunk_status;
  GtkWidget            *lbl_cursor;
  GtkWidget            *toast_bar;
  GtkWidget            *toast_lbl;
};

/* Core window & perspective helpers (amiga_window.c) */
void amiga_window_show_toast(KajoAmigaWindow *self, const char *msg);
void amiga_window_apply_session_state(KajoAmigaWindow *self, AmigaSessionState st);
void amiga_window_sync_mode_pill(KajoAmigaWindow *self);
void amiga_window_populate_project_dropdowns(KajoAmigaWindow *self);
void amiga_window_refresh_source_gutter_and_outline(KajoAmigaWindow *self);
void amiga_window_rebuild_center_tabs(KajoAmigaWindow *self);
void amiga_window_open_setup_tab(KajoAmigaWindow *self);
void amiga_window_close_setup_tab(KajoAmigaWindow *self);
void amiga_window_switch_center_page(KajoAmigaWindow *self, const char *page);
void amiga_window_show_hrm_quickdoc(KajoAmigaWindow *self, const char *query);
void amiga_window_jump_mem(KajoAmigaWindow *self, uint32_t addr);
void amiga_window_jump_disasm(KajoAmigaWindow *self, uint32_t addr);
void amiga_window_act_new_project(KajoAmigaWindow *self);
void amiga_window_act_open_project(KajoAmigaWindow *self);
void amiga_window_act_attach(KajoAmigaWindow *self);
void amiga_window_act_settings(KajoAmigaWindow *self);
void amiga_window_open_file_at_line(KajoAmigaWindow *self, const char *abs_path, guint line_1based);
void amiga_window_trigger_build(KajoAmigaWindow *self);
void amiga_window_sync_status_bar(KajoAmigaWindow *self);
void amiga_window_sync_compat_badge(KajoAmigaWindow *self);
void amiga_window_apply_half_screen_visibility(KajoAmigaWindow *self);
void amiga_window_save_active_buffer(KajoAmigaWindow *self);
const AmigaTarget    *amiga_window_get_selected_target(KajoAmigaWindow *self);
const AmigaHwProfile *amiga_window_get_selected_profile(KajoAmigaWindow *self);

/* Chrome builders (amiga_window_chrome.c) */
GtkWidget *amiga_window_chrome_build_header(KajoAmigaWindow *self);
GtkWidget *amiga_window_chrome_build_sidebar(KajoAmigaWindow *self);
GtkWidget *amiga_window_chrome_build_half_strip(KajoAmigaWindow *self);
GtkWidget *amiga_window_chrome_build_status_bar(KajoAmigaWindow *self);

/* Search, Outline & Quick-Doc (amiga_window_search.c) */
GtkWidget *amiga_window_outline_build_box(KajoAmigaWindow *self);
GtkWidget *amiga_window_search_build_page(KajoAmigaWindow *self);

/* Bottom Drawer module (amiga_window_drawer.c) */
GtkWidget *amiga_window_drawer_build(KajoAmigaWindow *self);
void       amiga_window_drawer_sync_tabs(KajoAmigaWindow *self, AmigaSessionState st);
void       amiga_window_drawer_select(KajoAmigaWindow *self, const char *page);
void       amiga_window_append_console(KajoAmigaWindow *self, const char *line);
void       amiga_window_drawer_set_diags(KajoAmigaWindow *self, GPtrArray *diags);
void       amiga_window_drawer_jump_diag(KajoAmigaWindow *self, int delta);
void       amiga_window_drawer_set_trace(KajoAmigaWindow *self, GPtrArray *entries);
void       amiga_window_drawer_append_rcmd(KajoAmigaWindow *self, const char *cmd, const char *reply);

/* Debug / RSP / LSP engine (amiga_window_debug.c) */
void amiga_window_debug_init(KajoAmigaWindow *self);
void amiga_window_debug_dispose(KajoAmigaWindow *self);
void amiga_window_debug_reload_symbols(KajoAmigaWindow *self);
void amiga_window_debug_sync_ui(KajoAmigaWindow *self);
void amiga_window_debug_refresh_snapshot(KajoAmigaWindow *self);
void amiga_window_debug_snapshot_now(KajoAmigaWindow *self);
void amiga_window_debug_start_or_continue(KajoAmigaWindow *self);
void amiga_window_debug_run_free(KajoAmigaWindow *self);
void amiga_window_debug_pause(KajoAmigaWindow *self);
void amiga_window_debug_stop(KajoAmigaWindow *self);
void amiga_window_debug_restart(KajoAmigaWindow *self);
void amiga_window_debug_step_over(KajoAmigaWindow *self);
void amiga_window_debug_step_into(KajoAmigaWindow *self);
void amiga_window_debug_step_out(KajoAmigaWindow *self);
void amiga_window_debug_run_to_cursor_source(KajoAmigaWindow *self);
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
void amiga_window_debug_fill_sidebar_callbacks(AmigaDebugSidebarCallbacks *cbs);
void amiga_window_debug_apply_exc_mask(KajoAmigaWindow *self, uint8_t mask);
void amiga_window_debug_add_watch(KajoAmigaWindow *self, const char *expr);
void amiga_window_debug_add_dma_watch(KajoAmigaWindow *self, uint32_t addr, uint32_t len);
void amiga_window_debug_add_watchpoint(KajoAmigaWindow *self, uint8_t kind, uint32_t addr, uint32_t len);
void amiga_window_debug_sync_sidebar(KajoAmigaWindow *self);
void amiga_window_debug_request_history(KajoAmigaWindow *self);

void amiga_window_lsp_sync_project(KajoAmigaWindow *self);
void amiga_window_lsp_notify_open(KajoAmigaWindow *self, IdeBuffer *buf);
void amiga_window_lsp_notify_save(KajoAmigaWindow *self, IdeBuffer *buf);
void amiga_window_lsp_shutdown(KajoAmigaWindow *self);

/* Right Inspector module (amiga_window_inspector.c) */
GtkWidget *amiga_window_inspector_build(KajoAmigaWindow *self);
void       amiga_window_inspector_select_tab(KajoAmigaWindow *self, int idx);
void       amiga_window_inspector_set_badge(KajoAmigaWindow *self, const char *text, bool frozen);
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
