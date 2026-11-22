#ifndef KAJO_IDE_STRIP_VIEW_PRIVATE_H
#define KAJO_IDE_STRIP_VIEW_PRIVATE_H

/* Internal to src/ide/: IdeStripView struct and helpers shared between
 * ide_strip_view.c, ide_strip_tiles.c, and ide_strip_canvas.c. Not a public API. */

#include "ide_strip_view.h"
#include "../theme.h"
#include "ide_gutter.h"

#include <cairo.h>
#include <gtksourceview/gtksource.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

G_BEGIN_DECLS

#define STRIP_D1_MAX_ROWS          200u
#define STRIP_D2_MAX_ROWS          120u
#define STRIP_DEBOUNCE_MS           50u
#define STRIP_BRIDGE_D2_MAX          6u
#define STRIP_BRIDGE_SECONDARY_MAX   5u

typedef struct {
  GtkWidget *row;        /* GtkListBoxRow */
  GtkWidget *peek_label; /* collapsed unless active_d1_idx == tile_idx */
  guint      tile_idx;
} D1RowEntry;

typedef struct {
  double x;
  double y;
  double w;
  double h;
  int    col_type; /* -1=ancestor, 0=D0, 1=D+1, 2=D+2 */
  guint  idx;
} OverviewHitbox;

typedef struct {
  IdeStripView *sv;
  GCancellable *alive;
  guint64       gen;
  IdeStripLens  lens;
} StripLspCtx;

struct _IdeStripView {
  IdeStripModel        *model;
  IdeStripGraph        *graph;
  IdeLspManager        *lsp; /* borrowed */
  IdeStripOpenBufferCb  open_buf_cb;
  IdeStripGetBuffersCb  get_bufs_cb;
  IdeStripStatusCb      status_cb;
  IdeStripModeExitCb    exit_cb;
  gpointer              user_data;

  IdeStripBpCb          bp_cb;
  IdeStripCondBpCb      cond_bp_cb;
  gpointer              gutter_user_data;

  GCancellable         *alive;

  /* Editor settings */
  char                 *font_family;
  int                   font_size_px;
  int                   tab_width;
  gboolean              insert_spaces;
  gboolean              word_wrap;
  gboolean              show_margin_guide;
  gboolean              highlight_current_line;
  gboolean              show_line_numbers;
  gboolean              relative_line_numbers;
  gboolean              magnetic_follow;

  /* Bound D0 / pin buffers */
  IdeBuffer            *d0_buf;        /* borrowed */
  GtkTextBuffer        *d0_tbuf;       /* reffed while signals are connected */
  gulong                d0_mark_sig_id;
  gulong                d0_changed_sig_id;
  IdeBuffer            *pin_buf;       /* borrowed */
  GtkSourceBuffer      *empty_d0_buf;  /* owned fallback buffer when D0 unbound */
  GtkSourceBuffer      *empty_pin_buf; /* owned fallback buffer when unpinned */

  /* Async, timers & re-entrancy guards */
  guint64               query_gen;
  guint                 debounce_id;
  guint                 tick_id;
  guint                 paned_idle_id;
  gboolean              programmatic_d1_scroll;
  gboolean              programmatic_paned_pos;
  gboolean              paned_layout_initialized;
  gboolean              user_dragged_outer_paned;
  gboolean              user_dragged_right_paned;
  int                   last_alloc_w;
  int                   focused_col; /* 0=D0, 1=D+1, 2=D+2 */
  gboolean              rebuilding_d1;
  gboolean              rebuilding_d2;
  gboolean              updating_filter_text;
  gboolean              binding_internal;

  /* Top-level & toolbar widgets */
  GtkWidget            *root_box;
  GtkWidget            *toolbar;
  GtkWidget            *trails_box;
  GtkWidget            *branch_btn;
  GtkWidget            *lens_btns[4];
  GtkWidget            *ws_badge;
  GtkWidget            *engine_badge;
  GtkWidget            *zen_btn;
  GtkWidget            *overview_btn;
  GtkWidget            *exit_btn;

  /* Stack: "strip" vs "overview" */
  GtkWidget            *view_stack;

  /* Strip view widgets */
  GtkWidget            *strip_overlay;
  GtkWidget            *strip_hbox;
  GtkWidget            *bridge_overlay;
  GtkWidget            *spine_box;
  GtkWidget            *outer_paned;
  GtkWidget            *right_paned;
  GtkWidget            *zen_rail_btn;
  GtkWidget            *zen_rail_label;

  /* Column D0 */
  GtkWidget            *d0_vpaned;
  GtkWidget            *pin_box;
  GtkWidget            *pin_title_label;
  GtkWidget            *pin_close_btn;
  GtkWidget            *pin_scroll;
  GtkWidget            *pin_source_view;
  GtkWidget            *d0_main_box;
  GtkWidget            *d0_header;
  GtkWidget            *d0_kind_badge;
  GtkWidget            *d0_title_label;
  GtkWidget            *d0_loc_label;
  GtkWidget            *d0_fullfile_btn;
  GtkWidget            *d0_editor_overlay;
  GtkWidget            *d0_scroll;
  GtkWidget            *d0_source_view;
  KajoIdeGutterRenderer *d0_gutter;
  GtkWidget            *d0_veil_overlay;

  /* Column D+1 */
  GtkWidget            *d1_wrap;
  GtkWidget            *d1_col;
  GtkWidget            *d1_header_label;
  GtkWidget            *d1_resync_btn;
  GtkWidget            *d1_filter_entry;
  GtkWidget            *d1_scroll;
  GtkWidget            *d1_list_box;
  GPtrArray            *d1_rows; /* D1RowEntry* */

  /* Column D+2 */
  GtkWidget            *d2_wrap;
  GtkWidget            *d2_col;
  GtkWidget            *d2_header_label;
  GtkWidget            *d2_scroll;
  GtkWidget            *d2_list_box;

  /* Overview page */
  GtkWidget            *overview_page;
  GtkWidget            *overview_entry;
  GtkWidget            *overview_match_label;
  GtkWidget            *overview_close_btn;
  GtkWidget            *overview_canvas;
  GArray               *overview_hitboxes; /* OverviewHitbox */
};

/* Defined in ide_strip_view.c */
double      ide_strip_view_now_sec(void);
void        ide_strip_view_emit_status(IdeStripView *sv, const char *msg);
const char *ide_strip_view_lens_css_class(IdeStripLens lens);
IdeBuffer  *ide_strip_view_find_open_buffer(const IdeStripView *sv, const char *file_path);
void        ide_strip_view_clear_box(GtkWidget *box);
GtkWidget  *ide_strip_view_make_toolbar_btn(const char *label,
                                            const char *css_class,
                                            const char *tooltip);
guint       ide_strip_view_get_d0_cursor_line(const IdeStripView *sv);
guint       ide_strip_view_get_d0_cursor_col(const IdeStripView *sv);
void        ide_strip_view_scroll_source_to_line(GtkWidget *source_view,
                                                 guint      line_1based,
                                                 gboolean   move_cursor,
                                                 double     yalign);
gboolean    ide_strip_view_open_and_attach_d0(IdeStripView *sv,
                                              const char   *file_path,
                                              guint         line_1based,
                                              gboolean      focus_buffer);
void        ide_strip_view_ensure_spring_tick(IdeStripView *sv);
void        ide_strip_view_schedule_paned_layout(IdeStripView *sv);
void        ide_strip_view_rebuild_toolbar(IdeStripView *sv);
void        ide_strip_view_rebuild_d0_header_and_pin(IdeStripView *sv);
void        ide_strip_view_rebuild_all_ui(IdeStripView *sv);

/* Defined in ide_strip_tiles.c */
void        ide_strip_tiles_enrich_tile(const IdeStripView *sv, IdeStripTile *tile);
GtkWidget  *ide_strip_tiles_build_d1_column(IdeStripView *sv);
GtkWidget  *ide_strip_tiles_build_d2_column(IdeStripView *sv);
void        ide_strip_tiles_rebuild_spine(IdeStripView *sv);
void        ide_strip_tiles_sync_d1_active(IdeStripView *sv);
void        ide_strip_tiles_rebuild_d1(IdeStripView *sv);
void        ide_strip_tiles_rebuild_d2(IdeStripView *sv);
void        ide_strip_tiles_refresh_d1(IdeStripView *sv);
void        ide_strip_tiles_refresh_d2(IdeStripView *sv);
void        ide_strip_tiles_run_viewport_band(IdeStripView *sv, gboolean force_resync);
void        ide_strip_tiles_schedule_viewport_band(IdeStripView *sv);

/* Defined in ide_strip_canvas.c */
GtkWidget  *ide_strip_canvas_build_overview_page(IdeStripView *sv);
void        ide_strip_canvas_on_bridge_resize(GtkDrawingArea *area,
                                              int             width,
                                              int             height,
                                              gpointer        user_data);
void        ide_strip_canvas_draw_d0_veil(GtkDrawingArea *area,
                                          cairo_t        *cr,
                                          int             width,
                                          int             height,
                                          gpointer        user_data);
void        ide_strip_canvas_draw_bridges(GtkDrawingArea *area,
                                          cairo_t        *cr,
                                          int             width,
                                          int             height,
                                          gpointer        user_data);
void        ide_strip_canvas_draw_overview(GtkDrawingArea *area,
                                           cairo_t        *cr,
                                           int             width,
                                           int             height,
                                           gpointer        user_data);
void        ide_strip_canvas_on_overview_pressed(GtkGestureClick *gesture,
                                                 int              n_press,
                                                 double           x,
                                                 double           y,
                                                 gpointer         user_data);

G_END_DECLS

#endif /* KAJO_IDE_STRIP_VIEW_PRIVATE_H */
