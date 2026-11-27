#ifndef KAJO_IDE_GUTTER_H
#define KAJO_IDE_GUTTER_H

#include "ide_buffer.h"
#include <gtksourceview/gtksource.h>

G_BEGIN_DECLS

#define KAJO_TYPE_IDE_GUTTER_RENDERER (kajo_ide_gutter_renderer_get_type())
G_DECLARE_FINAL_TYPE(KajoIdeGutterRenderer,
                     kajo_ide_gutter_renderer,
                     KAJO,
                     IDE_GUTTER_RENDERER,
                     GtkSourceGutterRenderer)

typedef void (*IdeGutterBreakpointToggledCb)(IdeBuffer *buf,
                                             guint      line_1based,
                                             gboolean   enabled,
                                             gpointer   user_data);

typedef void (*IdeGutterConditionalBpCb)(IdeBuffer *buf,
                                         guint      line_1based,
                                         gpointer   user_data);

typedef gboolean (*IdeGutterAnnotateLineCb)(IdeBuffer *buf,
                                            guint      line_1based,
                                            char      *out_text,
                                            gsize      out_cap,
                                            double    *out_r,
                                            double    *out_g,
                                            double    *out_b,
                                            gpointer   user_data);

GtkSourceGutterRenderer *kajo_ide_gutter_renderer_new(IdeBuffer *buf);
void                     kajo_ide_gutter_renderer_set_buffer(KajoIdeGutterRenderer *self,
                                                             IdeBuffer             *buf);
void                     kajo_ide_gutter_renderer_set_breakpoint_callback(
                             KajoIdeGutterRenderer       *self,
                             IdeGutterBreakpointToggledCb cb,
                             gpointer                     user_data);
void                     kajo_ide_gutter_renderer_set_conditional_bp_callback(
                             KajoIdeGutterRenderer   *self,
                             IdeGutterConditionalBpCb cb,
                             gpointer                 user_data);
void                     kajo_ide_gutter_renderer_set_annotation_callback(
                             KajoIdeGutterRenderer  *self,
                             IdeGutterAnnotateLineCb cb,
                             int                     width_px,
                             gpointer                user_data);
void                     kajo_ide_gutter_renderer_set_font(
                             KajoIdeGutterRenderer *self,
                             const char            *font_family,
                             int                    font_size_px);
void                     kajo_ide_gutter_renderer_set_relative_numbers(
                             KajoIdeGutterRenderer *self,
                             gboolean               relative);
gboolean                 kajo_ide_gutter_renderer_get_relative_numbers(
                             KajoIdeGutterRenderer *self);
guint                    ide_gutter_display_line_number(
                             guint    line_1based,
                             guint    cursor_line_1based,
                             gboolean relative);

G_END_DECLS

#endif /* KAJO_IDE_GUTTER_H */
