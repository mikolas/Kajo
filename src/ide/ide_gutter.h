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

GtkSourceGutterRenderer *kajo_ide_gutter_renderer_new(IdeBuffer *buf);
void                     kajo_ide_gutter_renderer_set_buffer(KajoIdeGutterRenderer *self,
                                                             IdeBuffer             *buf);
void                     kajo_ide_gutter_renderer_set_breakpoint_callback(
                             KajoIdeGutterRenderer       *self,
                             IdeGutterBreakpointToggledCb cb,
                             gpointer                     user_data);
void                     kajo_ide_gutter_renderer_set_font(
                             KajoIdeGutterRenderer *self,
                             const char            *font_family,
                             int                    font_size_px);

G_END_DECLS

#endif /* KAJO_IDE_GUTTER_H */
