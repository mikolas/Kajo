#include "ide_gutter.h"

#include <math.h>

struct _KajoIdeGutterRenderer {
  GtkSourceGutterRenderer      parent_instance;
  IdeBuffer                   *ide_buf;
  IdeGutterBreakpointToggledCb bp_cb;
  gpointer                     bp_cb_data;
};

G_DEFINE_FINAL_TYPE(KajoIdeGutterRenderer,
                    kajo_ide_gutter_renderer,
                    GTK_SOURCE_TYPE_GUTTER_RENDERER)

static void
kajo_ide_gutter_renderer_measure(GtkWidget      *widget,
                                 GtkOrientation  orientation,
                                 int             for_size,
                                 int            *minimum,
                                 int            *natural,
                                 int            *minimum_baseline,
                                 int            *natural_baseline)
{
  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = 36;
    *natural = 36;
  } else {
    *minimum = 0;
    *natural = 0;
  }
  *minimum_baseline = -1;
  *natural_baseline = -1;
}

static void
kajo_ide_gutter_renderer_snapshot_line(GtkSourceGutterRenderer *renderer,
                                       GtkSnapshot             *snapshot,
                                       GtkSourceGutterLines    *lines,
                                       guint                    line)
{
  KajoIdeGutterRenderer *self = KAJO_IDE_GUTTER_RENDERER(renderer);
  if (self->ide_buf == NULL) {
    return;
  }

  double y = 0.0;
  double h = 0.0;
  gtk_source_gutter_lines_get_line_extent(lines,
                                          line,
                                          GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_CELL,
                                          &y,
                                          &h);
  if (h <= 0.0) {
    return;
  }

  guint line_1based = line + 1;
  gboolean has_bp = ide_buffer_has_breakpoint(self->ide_buf, line_1based);
  const IdeDiagnostic *diag = ide_buffer_get_line_diagnostic(self->ide_buf, line_1based);
  guint pc_line = ide_buffer_get_dap_pc_line(self->ide_buf);
  IdeDiffState diff_state = ide_buffer_get_line_diff(self->ide_buf, line_1based);

  if (!has_bp && diag == NULL && pc_line != line_1based && diff_state == IDE_DIFF_NONE) {
    return;
  }

  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0.0f, (float)y, 36.0f, (float)h);
  cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &bounds);
  double cy = y + h * 0.5;

  /* 1. DAP Breakpoint circle (● #e81123) at x = 6 */
  if (has_bp) {
    cairo_set_source_rgb(cr, 0.910, 0.067, 0.137); /* #e81123 */
    cairo_arc(cr, 6.0, cy, 4.0, 0.0, 2.0 * G_PI);
    cairo_fill(cr);
  }

  /* 2. Diagnostic indicator at x = 16 */
  if (diag != NULL) {
    if (diag->severity == IDE_DIAG_ERROR) {
      cairo_set_source_rgb(cr, 0.910, 0.067, 0.137); /* #e81123 */
      cairo_set_line_width(cr, 1.8);
      cairo_move_to(cr, 13.0, cy - 3.5);
      cairo_line_to(cr, 19.0, cy + 3.5);
      cairo_move_to(cr, 19.0, cy - 3.5);
      cairo_line_to(cr, 13.0, cy + 3.5);
      cairo_stroke(cr);
    } else if (diag->severity == IDE_DIAG_WARNING) {
      cairo_set_source_rgb(cr, 1.0, 0.549, 0.0); /* #ff8c00 */
      cairo_move_to(cr, 16.0, cy - 4.0);
      cairo_line_to(cr, 20.0, cy + 3.5);
      cairo_line_to(cr, 12.0, cy + 3.5);
      cairo_close_path(cr);
      cairo_fill(cr);
    } else {
      cairo_set_source_rgb(cr, 0.0, 0.847, 0.941); /* #00d8f0 */
      cairo_arc(cr, 16.0, cy, 3.0, 0.0, 2.0 * G_PI);
      cairo_fill(cr);
    }
  }

  /* 3. DAP Execution PC arrow (▶ #ffd700) at x = 26 */
  if (pc_line == line_1based) {
    cairo_set_source_rgb(cr, 1.0, 0.843, 0.0); /* #ffd700 */
    cairo_move_to(cr, 23.0, cy - 4.5);
    cairo_line_to(cr, 29.5, cy);
    cairo_line_to(cr, 23.0, cy + 4.5);
    cairo_close_path(cr);
    cairo_fill(cr);
  }

  /* 4. Git / Agent Diff vertical bar at x = 32..35 */
  if (diff_state != IDE_DIFF_NONE) {
    switch (diff_state) {
      case IDE_DIFF_ADDED:
        cairo_set_source_rgb(cr, 0.063, 0.537, 0.243); /* #10893e */
        break;
      case IDE_DIFF_MODIFIED:
        cairo_set_source_rgb(cr, 0.0, 0.471, 0.831); /* #0078d4 */
        break;
      case IDE_DIFF_DELETED:
        cairo_set_source_rgb(cr, 0.910, 0.067, 0.137); /* #e81123 */
        break;
      case IDE_DIFF_AGENT:
      default:
        cairo_set_source_rgb(cr, 0.706, 0.0, 0.620); /* #b4009e */
        break;
    }
    cairo_rectangle(cr, 32.0, y + 1.0, 3.0, MAX(2.0, h - 2.0));
    cairo_fill(cr);
  }

  cairo_destroy(cr);
}

static gboolean
kajo_ide_gutter_renderer_query_activatable(GtkSourceGutterRenderer *renderer,
                                           GtkTextIter             *iter,
                                           GdkRectangle            *area)
{
  return TRUE;
}

static void
kajo_ide_gutter_renderer_activate(GtkSourceGutterRenderer *renderer,
                                  GtkTextIter             *iter,
                                  GdkRectangle            *area,
                                  guint                    button,
                                  GdkModifierType          state,
                                  gint                     n_presses)
{
  KajoIdeGutterRenderer *self = KAJO_IDE_GUTTER_RENDERER(renderer);
  if (self->ide_buf == NULL || iter == NULL || button != GDK_BUTTON_PRIMARY) {
    return;
  }

  guint line_1based = (guint)gtk_text_iter_get_line(iter) + 1;
  gboolean enabled = ide_buffer_toggle_breakpoint(self->ide_buf, line_1based);
  gtk_widget_queue_draw(GTK_WIDGET(self));
  if (self->bp_cb != NULL) {
    self->bp_cb(self->ide_buf, line_1based, enabled, self->bp_cb_data);
  }
}

static void
kajo_ide_gutter_renderer_class_init(KajoIdeGutterRendererClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GtkSourceGutterRendererClass *renderer_class = GTK_SOURCE_GUTTER_RENDERER_CLASS(klass);

  widget_class->measure = kajo_ide_gutter_renderer_measure;
  renderer_class->snapshot_line = kajo_ide_gutter_renderer_snapshot_line;
  renderer_class->query_activatable = kajo_ide_gutter_renderer_query_activatable;
  renderer_class->activate = kajo_ide_gutter_renderer_activate;
}

static void
kajo_ide_gutter_renderer_init(KajoIdeGutterRenderer *self)
{
  gtk_widget_add_css_class(GTK_WIDGET(self), "ide-gutter-renderer");
}

GtkSourceGutterRenderer *
kajo_ide_gutter_renderer_new(IdeBuffer *buf)
{
  KajoIdeGutterRenderer *self = g_object_new(KAJO_TYPE_IDE_GUTTER_RENDERER, NULL);
  self->ide_buf = buf;
  return GTK_SOURCE_GUTTER_RENDERER(self);
}

void
kajo_ide_gutter_renderer_set_buffer(KajoIdeGutterRenderer *self, IdeBuffer *buf)
{
  g_return_if_fail(KAJO_IS_IDE_GUTTER_RENDERER(self));
  self->ide_buf = buf;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
kajo_ide_gutter_renderer_set_breakpoint_callback(KajoIdeGutterRenderer       *self,
                                                 IdeGutterBreakpointToggledCb cb,
                                                 gpointer                     user_data)
{
  g_return_if_fail(KAJO_IS_IDE_GUTTER_RENDERER(self));
  self->bp_cb = cb;
  self->bp_cb_data = user_data;
}
