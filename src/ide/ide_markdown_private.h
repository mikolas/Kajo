#ifndef KAJO_IDE_MARKDOWN_PRIVATE_H
#define KAJO_IDE_MARKDOWN_PRIVATE_H

/* Internal to src/ide/: IdeMarkdownView struct and helpers shared between
 * ide_markdown.c and ide_markdown_embeds.c. Not a public API. */

#include "ide_markdown.h"

G_BEGIN_DECLS

struct _IdeMarkdownView {
  IdeMarkdownJumpCb       jump_cb;
  IdeMarkdownBufferEditCb edit_cb;
  IdeMarkdownOpenFileCb   open_file_cb;
  gpointer                user_data;

  char                   *doc_file_path;
  char                   *doc_dir;
  char                   *workspace_root;

  GtkWidget              *root_box;
  GtkWidget              *scroll_win;
  GtkWidget              *doc_box;
  GtkWidget              *doc_badge;
  GtkWidget              *task_badge;
  char                   *current_markdown;
  gboolean                updating_checklists;
  gboolean                context_dirty;
  guint                   tasks_done;
  guint                   tasks_total;
  guint                   scroll_restore_id;
  double                  pending_scroll_frac;
  gboolean                has_pending_scroll;
};

/* Defined in ide_markdown.c */
void       ide_markdown_open_or_navigate_target(IdeMarkdownView *md,
                                                const char      *uri_or_ref);
void       ide_markdown_render_into_box(IdeMarkdownView *md,
                                        GtkWidget       *target_box,
                                        const char      *markdown_text,
                                        guint            base_line_offset,
                                        guint           *io_tasks_done,
                                        guint           *io_tasks_total);

/* Defined in ide_markdown_embeds.c */
char      *ide_markdown_strip_optional_title(const char  *raw_src,
                                             char       **out_title);
GtkWidget *ide_markdown_build_mermaid_card(IdeMarkdownView *md,
                                           const char      *mermaid_code,
                                           guint            start_line_1based);
GtkWidget *ide_markdown_build_raw_svg_card(IdeMarkdownView *md,
                                           const char      *svg_code);
GtkWidget *ide_markdown_build_embed_card(IdeMarkdownView *md,
                                         const char      *alt,
                                         const char      *src,
                                         const char      *link_url);
GtkWidget *ide_markdown_build_carousel_card(IdeMarkdownView *md,
                                            const char      *carousel_body,
                                            guint            base_line);

G_END_DECLS

#endif /* KAJO_IDE_MARKDOWN_PRIVATE_H */
