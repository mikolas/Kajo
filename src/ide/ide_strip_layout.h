#ifndef KAJO_IDE_STRIP_LAYOUT_H
#define KAJO_IDE_STRIP_LAYOUT_H

/* Pure GTK-free Code Strip layout model, depth stack, vertical Trails,
 * magnetic viewport-band follow state machine, and analytical spring camera. */

#include <glib.h>

#include "../comp/core/comp_spring.h"

G_BEGIN_DECLS

#define IDE_STRIP_SPINE_WIDTH      28.0
#define IDE_STRIP_BRIDGE_WIDTH     12.0
#define IDE_STRIP_D1_TILE_STRIDE   34.0
#define IDE_STRIP_D1_PEEK_EXTRA_H 104.0
#define IDE_STRIP_D2_TILE_STRIDE   28.0

typedef enum {
  IDE_STRIP_LENS_CALLS = 0,
  IDE_STRIP_LENS_TYPES,
  IDE_STRIP_LENS_IMPACT,
  IDE_STRIP_LENS_DIFF,
} IdeStripLens;

typedef enum {
  IDE_STRIP_TILE_FUNCTION = 0,
  IDE_STRIP_TILE_TYPE,
  IDE_STRIP_TILE_FILE,
  IDE_STRIP_TILE_MACRO,
} IdeStripTileKind;

typedef struct {
  char *id;
  char *name;
  char *file_path;
  char *rel_path;
  guint start_line;
  guint end_line;
  guint call_line; /* 1-based line in D0 calling this tile */
  guint call_count;
  guint child_count;
  IdeStripTileKind kind;
  char *peek_code;
  guint diag_errors;
  guint diag_warnings;
  gboolean is_dirty;
  gboolean is_agent_modified;
  gboolean in_viewport_band;
  gboolean is_fallback;
} IdeStripTile;

typedef struct {
  IdeStripTile *tile;
  guint cursor_line;
  gint active_d1_idx;
  double d1_scroll_y;
  gboolean show_full_file;
} IdeStripAncestor;

typedef struct {
  double spine_w;    /* 28.0 when depth > 0 && !zen, else 0.0 */
  double bridge_w;   /* 12.0 each (2 bridges when !zen, else 0.0) */
  double d0_w;       /* 60% of usable width (or 96% in zen) */
  double d1_w;       /* 25% of usable width (or 0 in zen) */
  double d2_w;       /* 15% of usable width (or 0 in zen) */
  double zen_rail_w; /* 4% of viewport in zen, else 0 */
} IdeStripGeometry;

typedef struct {
  guint id;
  char *title;
  GPtrArray *ancestors;       /* IdeStripAncestor* */
  IdeStripTile *root_tile;    /* currently in D0 */
  IdeStripTile *pinned_tile;  /* optional vertical 30% pin in D0 */
  GPtrArray *d1_tiles;        /* IdeStripTile* */
  GPtrArray *d2_tiles;        /* IdeStripTile* */
  gint active_d1_idx;         /* -1 if empty */
  gint active_d2_idx;         /* -1 if empty */
  IdeStripLens lens;
  gboolean zen_mode;
  gboolean overview_mode;
  gboolean show_full_file;
  gboolean d1_unlinked;
  gboolean using_fallback;
  guint ext_suppressed_count;
  char *d1_filter;
  double d1_scroll_y;
  double d1_target_scroll_y;
  CompSpring d1_scroll_spring;
  gboolean d1_scroll_animating;
  CompSpring depth_slide_spring;
  double depth_slide_offset;
  gboolean depth_slide_animating;
} IdeStripTrail;

typedef struct {
  GPtrArray *trails; /* IdeStripTrail* */
  guint active_trail_idx;
  gboolean strip_active;
  guint next_trail_id;
} IdeStripModel;

/* Tile & ancestor helpers */
IdeStripTile *ide_strip_tile_new(const char *id,
                                 const char *name,
                                 const char *file_path,
                                 const char *rel_path,
                                 guint start_line,
                                 guint end_line,
                                 guint call_line,
                                 IdeStripTileKind kind);
IdeStripTile *ide_strip_tile_copy(const IdeStripTile *src);
void          ide_strip_tile_free(gpointer p);
void          ide_strip_ancestor_free(gpointer p);

/* Geometry */
void ide_strip_compute_geometry(double viewport_w,
                                guint depth,
                                gboolean zen_mode,
                                IdeStripGeometry *out);

/* Model & vertical Trails */
IdeStripModel *ide_strip_model_new(void);
void           ide_strip_model_free(IdeStripModel *m);
IdeStripTrail *ide_strip_model_get_active_trail(IdeStripModel *m);
IdeStripTrail *ide_strip_model_add_trail(IdeStripModel *m,
                                         const char *title,
                                         IdeStripTile *root_tile);
IdeStripTrail *ide_strip_model_branch_trail(IdeStripModel *m);
gboolean       ide_strip_model_switch_trail(IdeStripModel *m, int delta);
gboolean       ide_strip_model_select_trail(IdeStripModel *m, guint idx);
gboolean       ide_strip_model_close_trail(IdeStripModel *m, guint idx);

/* Trail root, tile columns & depth stack */
void     ide_strip_trail_set_root(IdeStripTrail *t,
                                  IdeStripTile *root_tile,
                                  gboolean clear_ancestors);
void     ide_strip_trail_set_d1_tiles(IdeStripTrail *t,
                                      GPtrArray *tiles,
                                      guint ext_suppressed,
                                      gboolean using_fallback);
void     ide_strip_trail_set_d2_tiles(IdeStripTrail *t, GPtrArray *tiles);
gboolean ide_strip_trail_promote_d1(IdeStripTrail *t,
                                    guint d1_idx,
                                    guint current_cursor_line,
                                    double now_sec);
gboolean ide_strip_trail_promote_d2(IdeStripTrail *t,
                                    guint d2_idx,
                                    guint current_cursor_line,
                                    double now_sec);
gboolean ide_strip_trail_surface_to(IdeStripTrail *t,
                                    guint ancestor_idx,
                                    double now_sec);
gboolean ide_strip_trail_surface_one(IdeStripTrail *t, double now_sec);
void     ide_strip_trail_pin_tile(IdeStripTrail *t, const IdeStripTile *tile);

/* Magnetic viewport-band follow, manual override & spring tick */
gboolean ide_strip_trail_update_viewport_band(IdeStripTrail *t,
                                              guint vis_top_line,
                                              guint vis_bot_line,
                                              guint cursor_line,
                                              double d0_cursor_rel_y,
                                              double d1_viewport_h,
                                              gboolean magnetic_enabled,
                                              double now_sec);
void     ide_strip_trail_manual_scroll_d1(IdeStripTrail *t, double scroll_y);
gboolean ide_strip_trail_resync_d1(IdeStripTrail *t,
                                   guint vis_top_line,
                                   guint vis_bot_line,
                                   guint cursor_line,
                                   double d0_cursor_rel_y,
                                   double d1_viewport_h,
                                   double now_sec);
gboolean ide_strip_trail_step_springs(IdeStripTrail *t, double now_sec);

/* Inline column filter */
void    ide_strip_trail_set_filter(IdeStripTrail *t, const char *query);
GArray *ide_strip_trail_filtered_d1_indices(const IdeStripTrail *t);

/* Display metadata */
const char *ide_strip_lens_name(IdeStripLens lens);
const char *ide_strip_tile_kind_badge(IdeStripTileKind kind);

G_END_DECLS

#endif /* KAJO_IDE_STRIP_LAYOUT_H */
