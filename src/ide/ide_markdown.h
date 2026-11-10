#ifndef KAJO_IDE_MARKDOWN_H
#define KAJO_IDE_MARKDOWN_H

#include "ide_buffer.h"
#include "ide_mermaid.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS


typedef enum {
  IDE_MD_EMBED_NONE = 0,
  IDE_MD_EMBED_IMAGE,
  IDE_MD_EMBED_PDF,
  IDE_MD_EMBED_ARCHIVE,
  IDE_MD_EMBED_MEDIA,
  IDE_MD_EMBED_DOCUMENT,
} IdeMarkdownEmbedKind;


typedef void (*IdeMarkdownJumpCb)(guint line_1based, gpointer user_data);
typedef void (*IdeMarkdownBufferEditCb)(const char *new_full_text, gpointer user_data);
typedef void (*IdeMarkdownOpenFileCb)(const char *path,
                                      guint       line_1based,
                                      guint       col_1based,
                                      gpointer    user_data);

typedef struct _IdeMarkdownView IdeMarkdownView;


char            *ide_markdown_toggle_checklist_line(const char *markdown_text,
                                                    guint       line_1based);

/* Pure testable inline Pango formatter, embed extractor, and asset path resolver */
char            *ide_markdown_format_inline_pango(const char *raw);
gboolean         ide_markdown_extract_embed(const char  *line,
                                            char       **out_alt,
                                            char       **out_src,
                                            char       **out_link_url);
char            *ide_markdown_resolve_asset_path(const char *raw_ref,
                                                 const char *doc_dir,
                                                 const char *workspace_root,
                                                 guint      *out_line_1based);
IdeMarkdownEmbedKind ide_markdown_classify_asset(const char *path_or_ref);

/* Full Graphical Markdown, Embedded Media/Image/PDF/Archive & Cairo Vector Mermaid View */
IdeMarkdownView *ide_markdown_view_new(IdeMarkdownJumpCb       jump_cb,
                                       IdeMarkdownBufferEditCb edit_cb,
                                       gpointer                user_data);
void             ide_markdown_view_set_open_file_cb(IdeMarkdownView      *md,
                                                    IdeMarkdownOpenFileCb open_file_cb);
void             ide_markdown_view_set_context(IdeMarkdownView *md,
                                               const char      *doc_file_path,
                                               const char      *workspace_root);
void             ide_markdown_view_free(IdeMarkdownView *md);

GtkWidget       *ide_markdown_view_get_widget(IdeMarkdownView *md);
void             ide_markdown_view_render(IdeMarkdownView *md,
                                          const char      *markdown_text);
void             ide_markdown_view_sync_scroll(IdeMarkdownView *md,
                                               double           scroll_fraction);

G_END_DECLS

#endif /* KAJO_IDE_MARKDOWN_H */
