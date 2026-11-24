#ifndef KAJO_VIEWER_IMAGE_H
#define KAJO_VIEWER_IMAGE_H

#include "viewer_model.h"
#include "viewer_sidebar.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _ViewerImageView ViewerImageView;

typedef void (*ViewerImageStateCb)(gpointer user_data);

ViewerImageView *viewer_image_view_new(ViewerImageStateCb state_cb, gpointer user_data);
void             viewer_image_view_free(ViewerImageView *view);

GtkWidget       *viewer_image_view_get_widget(ViewerImageView *view);
gboolean         viewer_image_view_load_file(ViewerImageView *view, GFile *file);
void             viewer_image_view_load_texture(ViewerImageView *view,
                                              GdkTexture      *texture,
                                              const char      *virtual_name,
                                              const char      *decoder_label);

void             viewer_image_view_set_fit_mode(ViewerImageView *view, ViewerFitMode mode);
void             viewer_image_view_step_zoom(ViewerImageView *view, int direction);
double           viewer_image_view_get_zoom(const ViewerImageView *view);
ViewerFitMode    viewer_image_view_get_fit_mode(const ViewerImageView *view);

void             viewer_image_view_rotate(ViewerImageView *view, int delta_deg);
void             viewer_image_view_flip(ViewerImageView *view, gboolean horizontal);
void             viewer_image_view_toggle_checkerboard(ViewerImageView *view);
gboolean         viewer_image_view_get_checkerboard(const ViewerImageView *view);
void             viewer_image_view_toggle_filmstrip(ViewerImageView *view);
gboolean         viewer_image_view_get_filmstrip(const ViewerImageView *view);
void             viewer_image_view_toggle_animation(ViewerImageView *view);

void             viewer_image_view_step_sibling(ViewerImageView *view, int delta);
int              viewer_image_view_get_sibling_count(const ViewerImageView *view);
int              viewer_image_view_get_sibling_index(const ViewerImageView *view);
const char      *viewer_image_view_get_current_path(const ViewerImageView *view);

gboolean         viewer_image_view_copy_to_clipboard(ViewerImageView *view);
const ViewerImageMeta *viewer_image_view_get_meta(const ViewerImageView *view);

G_END_DECLS

#endif /* KAJO_VIEWER_IMAGE_H */