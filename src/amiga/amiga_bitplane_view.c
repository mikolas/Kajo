#include "amiga_bitplane_view.h"
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct _AmigaBitplaneView {
  GtkBox                 parent_instance;

  AmigaBitplaneCallbacks cbs;
  gpointer               user_data;
  bool                   updating;

  /* Data model */
  AmigaBitplaneParams    params;
  GBytes                *chip_bytes;
  uint32_t               chip_base;
  AmigaCustomSnapshot    last_snap;
  bool                   has_snap;
  char                  *symbol_hint;
  bool                   grid_16px;
  bool                   back_buffer;

  /* Last rendered target rect for mouse probe mapping */
  double                 draw_x;
  double                 draw_y;
  double                 draw_w;
  double                 draw_h;

  /* Widgets */
  GtkWidget             *prov_badge;
  GtkWidget             *dd_source;
  GtkWidget             *entry_addr;
  GtkWidget             *dd_depth;
  GtkWidget             *dd_layout;
  GtkWidget             *dd_size;
  GtkWidget             *plane_btns[6];
  GtkWidget             *btn_grid;
  GtkWidget             *btn_buf;
  GtkWidget             *canvas;
  GtkWidget             *palette_area;
  GtkWidget             *lbl_probe;
};

G_DEFINE_FINAL_TYPE(AmigaBitplaneView, amiga_bitplane_view, GTK_TYPE_BOX)

static const uint16_t k_preset_w[4] = { 320, 320, 640, 160 };
static const uint16_t k_preset_h[4] = { 256, 200, 256, 128 };

static uint32_t
parse_hex_or_sym(AmigaBitplaneView *self, const char *txt, bool *ok)
{
  if (ok) *ok = false;
  if (!txt || !txt[0]) return 0;
  const char *p = txt;
  while (*p == ' ' || *p == '\t') p++;
  if (p[0] == '$') p++;
  else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
  else if (self->cbs.resolve_symbol) {
    bool sym_ok = false;
    uint32_t sv = self->cbs.resolve_symbol(self, txt, &sym_ok, self->user_data);
    if (sym_ok) {
      if (ok) *ok = true;
      return sv;
    }
  }
  char *end = NULL;
  unsigned long v = strtoul(p, &end, 16);
  if (end && end != p) {
    if (ok) *ok = true;
    return (uint32_t)v;
  }
  return 0;
}

static void
update_provenance_badge(AmigaBitplaneView *self, const char *cls, const char *text)
{
  gtk_widget_remove_css_class(self->prov_badge, "amiga-badge-snapshot");
  gtk_widget_remove_css_class(self->prov_badge, "amiga-badge-live");
  gtk_widget_remove_css_class(self->prov_badge, "amiga-badge-watched");
  gtk_widget_add_css_class(self->prov_badge, cls);
  gtk_label_set_text(GTK_LABEL(self->prov_badge), text);
}

static size_t
calc_fetch_bytes(const AmigaBitplaneParams *p)
{
  uint32_t bpr = (uint32_t)((p->width + 7u) / 8u);
  uint32_t stride = (p->layout == AMIGA_BPL_INTERLEAVED) ? bpr * MAX(1u, p->planes) : bpr;
  size_t one_buf = (size_t)stride * p->height * (p->layout == AMIGA_BPL_INTERLEAVED ? 1u : p->planes);
  return MIN(MAX(one_buf * 2u, (size_t)10240u), (size_t)49152u);
}

static bool
build_adjusted_params(AmigaBitplaneView *self, AmigaBitplaneParams *out)
{
  if (!self->chip_bytes) return false;
  *out = self->params;
  uint32_t bpr = (uint32_t)((out->width + 7u) / 8u);
  uint32_t frame_step = (out->layout == AMIGA_BPL_INTERLEAVED)
                            ? bpr * out->height * MAX(1u, out->planes)
                            : bpr * out->height;
  uint32_t extra = self->back_buffer ? frame_step : 0u;
  uint32_t base0 = out->plane_ptrs[0];
  for (uint8_t pl = 0; pl < out->planes && pl < 6; pl++) {
    uint32_t phys = out->plane_ptrs[pl];
    if (phys == 0 && pl > 0 && base0 != 0) {
      phys = base0 + (out->layout == AMIGA_BPL_INTERLEAVED ? pl * bpr : pl * bpr * out->height);
    }
    phys += extra;
    if (phys >= self->chip_base) {
      out->plane_ptrs[pl] = phys - self->chip_base;
    } else {
      out->plane_ptrs[pl] = 0;
    }
    /* Prevent sample_pixel_raw() from re-multiplying offset 0 on pl > 0 */
    if (pl > 0 && out->plane_ptrs[pl] == 0) {
      out->plane_ptrs[pl] = out->plane_ptrs[0] +
          (out->layout == AMIGA_BPL_INTERLEAVED ? pl * bpr : pl * bpr * out->height);
    }
  }
  /* Guarantee pl > 0 lookup respects exact offsets when plane 0 == 0 */
  if (out->plane_ptrs[0] == 0 && out->planes > 1) {
    for (uint8_t pl = 1; pl < out->planes && pl < 6; pl++) {
      if (self->params.plane_ptrs[pl] >= self->chip_base + extra) {
        out->plane_ptrs[pl] = (self->params.plane_ptrs[pl] + extra) - self->chip_base;
      }
    }
  }
  return true;
}

static cairo_surface_t *
render_surface(AmigaBitplaneView *self)
{
  if (!self->chip_bytes || self->params.width == 0 || self->params.height == 0) return NULL;
  AmigaBitplaneParams adj;
  if (!build_adjusted_params(self, &adj)) return NULL;

  gsize chip_len = 0;
  const uint8_t *chip = g_bytes_get_data(self->chip_bytes, &chip_len);
  size_t npix = (size_t)adj.width * adj.height;
  g_autofree uint8_t *rgba = g_malloc0(npix * 4u);
  if (!amiga_bitplane_render(chip, chip_len, &adj, rgba)) return NULL;

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, adj.width, adj.height);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    return NULL;
  }
  cairo_surface_flush(surf);
  uint8_t *dst_base = cairo_image_surface_get_data(surf);
  int stride = cairo_image_surface_get_stride(surf);
  for (uint16_t y = 0; y < adj.height; y++) {
    uint32_t *row = (uint32_t *)(dst_base + (size_t)y * (size_t)stride);
    const uint8_t *src = rgba + ((size_t)y * adj.width) * 4u;
    for (uint16_t x = 0; x < adj.width; x++) {
      row[x] = (0xFF000000u) |
               ((uint32_t)src[x * 4u + 0] << 16) |
               ((uint32_t)src[x * 4u + 1] << 8) |
               ((uint32_t)src[x * 4u + 2]);
    }
  }
  cairo_surface_mark_dirty(surf);
  return surf;
}

static void
on_canvas_draw(GtkDrawingArea *area G_GNUC_UNUSED,
               cairo_t        *cr,
               int             w,
               int             h,
               gpointer        ud)
{
  AmigaBitplaneView *self = AMIGA_BITPLANE_VIEW(ud);
  cairo_set_source_rgb(cr, 0.05, 0.05, 0.06);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  cairo_surface_t *img = render_surface(self);
  if (!img) {
    cairo_set_source_rgb(cr, 0.55, 0.58, 0.62);
    cairo_select_font_face(cr, "Fira Code", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
    cairo_move_to(cr, 12.0, h * 0.5);
    cairo_show_text(cr, "No Chip RAM snapshot loaded (Pause / ↻ SNAP or click ↻)");
    return;
  }

  double iw = (double)self->params.width;
  double ih = (double)self->params.height;
  double scale = MIN((double)w / iw, (double)h / ih);
  if (scale <= 0.0) scale = 1.0;
  double dw = iw * scale;
  double dh = ih * scale;
  double dx = ((double)w - dw) * 0.5;
  double dy = ((double)h - dh) * 0.5;
  self->draw_x = dx; self->draw_y = dy;
  self->draw_w = dw; self->draw_h = dh;

  cairo_save(cr);
  cairo_translate(cr, dx, dy);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, img, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint(cr);

  if (self->grid_16px) {
    cairo_set_source_rgba(cr, 0.0, 0.85, 0.94, 0.25);
    cairo_set_line_width(cr, 1.0 / scale);
    for (uint16_t gx = 16; gx < self->params.width; gx += 16) {
      cairo_move_to(cr, gx, 0);
      cairo_line_to(cr, gx, ih);
    }
    for (uint16_t gy = 16; gy < self->params.height; gy += 16) {
      cairo_move_to(cr, 0, gy);
      cairo_line_to(cr, iw, gy);
    }
    cairo_stroke(cr);
  }
  cairo_restore(cr);

  cairo_surface_destroy(img);
}

static void
on_palette_draw(GtkDrawingArea *area G_GNUC_UNUSED,
                cairo_t        *cr,
                int             w,
                int             h,
                gpointer        ud)
{
  AmigaBitplaneView *self = AMIGA_BITPLANE_VIEW(ud);
  double sw = (double)w / 16.0;
  double sh = (double)h / 2.0;
  for (int i = 0; i < 32; i++) {
    uint8_t r = 0, g = 0, b = 0;
    amiga_decode_ocs_rgb12(self->params.palette_ocs[i], &r, &g, &b);
    int col = i % 16;
    int row = i / 16;
    cairo_set_source_rgb(cr, r / 255.0, g / 255.0, b / 255.0);
    cairo_rectangle(cr, col * sw, row * sh, sw, sh);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.18, 0.18, 0.20, 0.9);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, col * sw + 0.5, row * sh + 0.5, MAX(1.0, sw - 1.0), MAX(1.0, sh - 1.0));
    cairo_stroke(cr);
  }
}

static void
on_canvas_motion(GtkEventControllerMotion *m G_GNUC_UNUSED, double x, double y, gpointer ud)
{
  AmigaBitplaneView *self = AMIGA_BITPLANE_VIEW(ud);
  if (!self->chip_bytes || self->draw_w <= 1.0 || self->draw_h <= 1.0) return;
  if (x < self->draw_x || y < self->draw_y ||
      x >= self->draw_x + self->draw_w || y >= self->draw_y + self->draw_h) {
    return;
  }
  uint16_t px = (uint16_t)(((x - self->draw_x) / self->draw_w) * self->params.width);
  uint16_t py = (uint16_t)(((y - self->draw_y) / self->draw_h) * self->params.height);
  if (px >= self->params.width) px = (uint16_t)(self->params.width - 1u);
  if (py >= self->params.height) py = (uint16_t)(self->params.height - 1u);

  AmigaBitplaneParams adj;
  if (!build_adjusted_params(self, &adj)) return;
  gsize chip_len = 0;
  const uint8_t *chip = g_bytes_get_data(self->chip_bytes, &chip_len);
  AmigaPixelProbe pr;
  if (amiga_bitplane_probe_pixel(chip, chip_len, &adj, px, py, &pr)) {
    g_autofree char *txt = g_strdup_printf(
        "X:%3u Y:%3u | IDX:%2u | $%03X | $%06X b%u",
        pr.x, pr.y, pr.color_index, pr.ocs_rgb & 0x0FFFu,
        pr.chip_addr + self->chip_base, pr.bit_index);
    gtk_label_set_text(GTK_LABEL(self->lbl_probe), txt);
  }
}

static void
sync_params_from_controls(AmigaBitplaneView *self)
{
  if (self->updating) return;
  guint d_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_depth));
  self->params.ham6 = (d_idx == 6);
  self->params.ehb = (d_idx == 7);
  self->params.planes = (d_idx < 6) ? (uint8_t)(d_idx + 1u) : 6u;

  guint l_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_layout));
  self->params.layout = (AmigaBplLayout)MIN(l_idx, 2u);

  guint s_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_size));
  if (s_idx >= 4) s_idx = 0;
  self->params.width = k_preset_w[s_idx];
  self->params.height = k_preset_h[s_idx];

  uint8_t mask = 0;
  for (int i = 0; i < 6; i++) {
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->plane_btns[i]))) {
      mask |= (uint8_t)(1u << i);
    }
  }
  self->params.plane_mask = mask;
  self->grid_16px = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_grid));
  self->back_buffer = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_buf));

  gtk_widget_queue_draw(self->canvas);
  gtk_widget_queue_draw(self->palette_area);
}

static void
on_control_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *ps G_GNUC_UNUSED, gpointer ud)
{
  sync_params_from_controls(AMIGA_BITPLANE_VIEW(ud));
}

static void
on_toggle_changed(GtkToggleButton *tb G_GNUC_UNUSED, gpointer ud)
{
  sync_params_from_controls(AMIGA_BITPLANE_VIEW(ud));
}

static void
on_refresh_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaBitplaneView *self = AMIGA_BITPLANE_VIEW(ud);
  guint src = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->dd_source));
  uint32_t addr = self->has_snap ? self->last_snap.bplpt[0] : self->params.plane_ptrs[0];
  if (src == 1 && self->symbol_hint && self->cbs.resolve_symbol) {
    bool ok = false;
    uint32_t a = self->cbs.resolve_symbol(self, self->symbol_hint, &ok, self->user_data);
    if (ok) {
      addr = a;
      update_provenance_badge(self, "amiga-badge-live", "SYMBOL");
    }
  } else if (src == 2) {
    bool ok = false;
    uint32_t a = parse_hex_or_sym(self, gtk_editable_get_text(GTK_EDITABLE(self->entry_addr)), &ok);
    if (ok) {
      addr = a;
      update_provenance_badge(self, "amiga-badge-watched", "MANUAL $ADDR");
    }
  } else {
    update_provenance_badge(self, "amiga-badge-snapshot", "EMU SNAPSHOT");
  }

  for (int i = 0; i < 6; i++) {
    if (src != 0 || !self->has_snap) {
      self->params.plane_ptrs[i] = (i == 0) ? addr : 0u;
    }
  }
  if (self->cbs.request_mem && addr > 0) {
    self->cbs.request_mem(self, addr, calc_fetch_bytes(&self->params), self->user_data);
  }
  gtk_widget_queue_draw(self->canvas);
}

static void
on_png_save_done(GObject *src, GAsyncResult *res, gpointer ud)
{
  GWeakRef *weak = ud;
  g_autoptr(GFile) f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
  g_autoptr(AmigaBitplaneView) self = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  if (!self || !f) return;
  g_autofree char *path = g_file_get_path(f);
  if (!path) return;
  cairo_surface_t *surf = render_surface(self);
  if (surf) {
    cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);
  }
}

static void
on_export_png_clicked(GtkButton *b G_GNUC_UNUSED, gpointer ud)
{
  AmigaBitplaneView *self = AMIGA_BITPLANE_VIEW(ud);
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Export Bitplanes PNG");
  gtk_file_dialog_set_initial_name(dlg, "bitplanes.png");
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, self);
  gtk_file_dialog_save(dlg, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
                       NULL, on_png_save_done, weak);
}

static void
amiga_bitplane_view_dispose(GObject *obj)
{
  AmigaBitplaneView *self = AMIGA_BITPLANE_VIEW(obj);
  g_clear_pointer(&self->chip_bytes, g_bytes_unref);
  g_clear_pointer(&self->symbol_hint, g_free);
  G_OBJECT_CLASS(amiga_bitplane_view_parent_class)->dispose(obj);
}

static void
amiga_bitplane_view_class_init(AmigaBitplaneViewClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = amiga_bitplane_view_dispose;
}

static void
amiga_bitplane_view_init(AmigaBitplaneView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 0);

  self->params.width = 320;
  self->params.height = 256;
  self->params.planes = 4;
  self->params.plane_mask = 0x3Fu;
  self->params.palette_ocs[1] = 0x0FFF;

  GtkWidget *scr = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scr, TRUE);
  gtk_widget_set_hexpand(scr, TRUE);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top(vbox, 5);
  gtk_widget_set_margin_bottom(vbox, 5);
  gtk_widget_set_margin_start(vbox, 8);
  gtk_widget_set_margin_end(vbox, 8);

  /* Top row: Provenance + Source + Manual addr + Refresh + PNG */
  GtkWidget *r0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign(r0, GTK_ALIGN_CENTER);
  self->prov_badge = gtk_label_new("EMU SNAPSHOT");
  gtk_widget_add_css_class(self->prov_badge, "amiga-badge-snapshot");
  gtk_widget_set_valign(self->prov_badge, GTK_ALIGN_CENTER);

  self->dd_source = gtk_drop_down_new_from_strings(
      (const char *[]){"SNAP", "SYM", "MANUAL", NULL});
  gtk_widget_add_css_class(self->dd_source, "amiga-header-dd");

  self->entry_addr = gtk_entry_new();
  gtk_widget_add_css_class(self->entry_addr, "amiga-entry");
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_addr), "$030000");
  gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_addr), 8);
  gtk_widget_set_hexpand(self->entry_addr, TRUE);
  g_signal_connect(self->entry_addr, "activate", G_CALLBACK(on_refresh_clicked), self);

  GtkWidget *btn_ref = gtk_button_new_with_label("↻");
  gtk_widget_add_css_class(btn_ref, "ide-toolbar-btn");
  g_signal_connect(btn_ref, "clicked", G_CALLBACK(on_refresh_clicked), self);

  GtkWidget *btn_png = gtk_button_new_with_label("💾 PNG");
  gtk_widget_add_css_class(btn_png, "ide-toolbar-btn");
  g_signal_connect(btn_png, "clicked", G_CALLBACK(on_export_png_clicked), self);

  gtk_box_append(GTK_BOX(r0), self->prov_badge);
  gtk_box_append(GTK_BOX(r0), self->dd_source);
  gtk_box_append(GTK_BOX(r0), self->entry_addr);
  gtk_box_append(GTK_BOX(r0), btn_ref);
  gtk_box_append(GTK_BOX(r0), btn_png);
  gtk_box_append(GTK_BOX(vbox), r0);

  /* Config row 1: Depth, Layout, Size */
  GtkWidget *r1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  self->dd_depth = gtk_drop_down_new_from_strings(
      (const char *[]){"1 BPL", "2 BPL", "3 BPL", "4 BPL", "5 BPL", "6 BPL", "HAM6", "EHB", NULL});
  gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_depth), 3);
  gtk_widget_add_css_class(self->dd_depth, "amiga-header-dd");

  self->dd_layout = gtk_drop_down_new_from_strings(
      (const char *[]){"PLANAR", "INTERLV", "LINE×2", NULL});
  gtk_widget_add_css_class(self->dd_layout, "amiga-header-dd");

  self->dd_size = gtk_drop_down_new_from_strings(
      (const char *[]){"320×256", "320×200", "640×256", "160×128", NULL});
  gtk_widget_add_css_class(self->dd_size, "amiga-header-dd");

  gtk_box_append(GTK_BOX(r1), self->dd_depth);
  gtk_box_append(GTK_BOX(r1), self->dd_layout);
  gtk_box_append(GTK_BOX(r1), self->dd_size);
  gtk_box_append(GTK_BOX(vbox), r1);

  /* Config row 2: P0..P5 + Grid + Buffer flip */
  GtkWidget *r2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  for (int i = 0; i < 6; i++) {
    g_autofree char *pl = g_strdup_printf("P%d", i);
    self->plane_btns[i] = gtk_toggle_button_new_with_label(pl);
    gtk_widget_add_css_class(self->plane_btns[i], "amiga-insp-tab");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->plane_btns[i]), TRUE);
    g_signal_connect(self->plane_btns[i], "toggled", G_CALLBACK(on_toggle_changed), self);
    gtk_box_append(GTK_BOX(r2), self->plane_btns[i]);
  }
  self->btn_grid = gtk_toggle_button_new_with_label("#16");
  gtk_widget_add_css_class(self->btn_grid, "amiga-insp-tab");
  g_signal_connect(self->btn_grid, "toggled", G_CALLBACK(on_toggle_changed), self);

  self->btn_buf = gtk_toggle_button_new_with_label("BACK");
  gtk_widget_add_css_class(self->btn_buf, "amiga-insp-tab");
  g_signal_connect(self->btn_buf, "toggled", G_CALLBACK(on_toggle_changed), self);

  gtk_box_append(GTK_BOX(r2), self->btn_grid);
  gtk_box_append(GTK_BOX(r2), self->btn_buf);
  gtk_box_append(GTK_BOX(vbox), r2);

  g_signal_connect(self->dd_depth, "notify::selected", G_CALLBACK(on_control_changed), self);
  g_signal_connect(self->dd_layout, "notify::selected", G_CALLBACK(on_control_changed), self);
  g_signal_connect(self->dd_size, "notify::selected", G_CALLBACK(on_control_changed), self);

  /* Canvas */
  GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(frame, "amiga-bitplane-frame");
  gtk_widget_set_hexpand(frame, TRUE);
  gtk_widget_set_vexpand(frame, TRUE);
  self->canvas = gtk_drawing_area_new();
  gtk_widget_set_size_request(self->canvas, -1, 148);
  gtk_widget_set_hexpand(self->canvas, TRUE);
  gtk_widget_set_vexpand(self->canvas, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->canvas), on_canvas_draw, self, NULL);
  GtkEventController *mot = gtk_event_controller_motion_new();
  g_signal_connect(mot, "motion", G_CALLBACK(on_canvas_motion), self);
  gtk_widget_add_controller(self->canvas, mot);
  gtk_box_append(GTK_BOX(frame), self->canvas);
  gtk_box_append(GTK_BOX(vbox), frame);

  /* 32-swatch OCS Palette */
  self->palette_area = gtk_drawing_area_new();
  gtk_widget_set_size_request(self->palette_area, -1, 22);
  gtk_widget_set_hexpand(self->palette_area, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->palette_area), on_palette_draw, self, NULL);
  gtk_box_append(GTK_BOX(vbox), self->palette_area);

  /* Probe Bar */
  self->lbl_probe = gtk_label_new("Hover canvas: X:--- Y:--- | IDX:-- | $--- | CHIP $------");
  gtk_widget_add_css_class(self->lbl_probe, "amiga-probe-bar");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_probe), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_probe), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(vbox), self->lbl_probe);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), vbox);
  gtk_box_append(GTK_BOX(self), scr);
}

AmigaBitplaneView *
amiga_bitplane_view_new(void)
{
  return g_object_new(AMIGA_TYPE_BITPLANE_VIEW, NULL);
}

void
amiga_bitplane_view_set_callbacks(AmigaBitplaneView            *self,
                                  const AmigaBitplaneCallbacks *cbs,
                                  gpointer                      ud)
{
  g_return_if_fail(AMIGA_IS_BITPLANE_VIEW(self));
  if (cbs) self->cbs = *cbs;
  else memset(&self->cbs, 0, sizeof(self->cbs));
  self->user_data = ud;
}

void
amiga_bitplane_view_set_snapshot(AmigaBitplaneView         *self,
                                 const AmigaCustomSnapshot *snap,
                                 const uint8_t             *chip_ram,
                                 size_t                     chip_len,
                                 uint32_t                   chip_base)
{
  g_return_if_fail(AMIGA_IS_BITPLANE_VIEW(self));
  if (snap) {
    self->last_snap = *snap;
    self->has_snap = true;
    self->updating = true;

    uint16_t c0 = snap->bplcon[0];
    uint8_t bpu = (uint8_t)((c0 >> 12) & 0x7u);
    if (bpu == 0) bpu = 1;
    if (bpu > 6) bpu = 6;
    bool ham = (c0 & 0x0800u) != 0 && bpu == 6;
    bool ehb = !ham && bpu == 6 && ((snap->bplcon[2] & 0x0200u) == 0);
    bool hires = (c0 & 0x8000u) != 0;

    self->params.planes = bpu;
    self->params.ham6 = ham;
    self->params.ehb = ehb;
    self->params.bpl1mod = snap->bpl1mod;
    self->params.bpl2mod = snap->bpl2mod;
    self->params.width = hires ? 640u : 320u;
    self->params.height = 256u;
    for (int i = 0; i < 6; i++) self->params.plane_ptrs[i] = snap->bplpt[i];
    for (int i = 0; i < 32; i++) self->params.palette_ocs[i] = snap->color[i];

    guint d_idx = ham ? 6u : (ehb ? 7u : (guint)(bpu - 1u));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_depth), d_idx);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->dd_size), hires ? 2u : 0u);
    g_autofree char *astr = g_strdup_printf("$%06X", snap->bplpt[0]);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_addr), astr);
    update_provenance_badge(self, "amiga-badge-snapshot", "EMU SNAPSHOT");
    self->updating = false;
  }
  if (chip_ram && chip_len > 0) {
    amiga_bitplane_view_set_chip_ram(self, chip_base, chip_ram, chip_len);
  } else {
    gtk_widget_queue_draw(self->canvas);
    gtk_widget_queue_draw(self->palette_area);
  }
}

void
amiga_bitplane_view_set_chip_ram(AmigaBitplaneView *self,
                                 uint32_t           chip_base,
                                 const uint8_t     *chip_ram,
                                 size_t             chip_len)
{
  g_return_if_fail(AMIGA_IS_BITPLANE_VIEW(self));
  g_clear_pointer(&self->chip_bytes, g_bytes_unref);
  self->chip_base = chip_base;
  if (chip_ram && chip_len > 0) {
    self->chip_bytes = g_bytes_new(chip_ram, chip_len);
  }
  gtk_widget_queue_draw(self->canvas);
  gtk_widget_queue_draw(self->palette_area);
}

void
amiga_bitplane_view_set_symbol_hint(AmigaBitplaneView *self, const char *sym_name)
{
  g_return_if_fail(AMIGA_IS_BITPLANE_VIEW(self));
  g_free(self->symbol_hint);
  self->symbol_hint = g_strdup(sym_name);
}

void
amiga_bitplane_view_set_grid(AmigaBitplaneView *self, bool enabled)
{
  g_return_if_fail(AMIGA_IS_BITPLANE_VIEW(self));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_grid), enabled);
}
