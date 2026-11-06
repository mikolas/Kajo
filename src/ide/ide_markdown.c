#include "ide_markdown.h"
#include "../common/pdf_render.h"
#include "ide_buffer.h"
#include <archive.h>
#include <archive_entry.h>
#include <cairo-svg.h>
#include <gio/gio.h>
#include <gtksourceview/gtksource.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <poppler.h>
#include <string.h>

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
  GtkWidget              *task_badge;
  char                   *current_markdown;
  gboolean                updating_checklists;
};

typedef struct {
  IdeMarkdownView *md;
  IdeMermaidGraph *graph;
  GtkWidget       *drawing_area;
  GtkWidget       *zoom_label;
  double           zoom;
} MermaidCardState;

typedef struct {
  IdeMarkdownView *md;
  GdkTexture      *texture;
  char            *resolved_path;
  char            *link_url;
  GtkWidget       *picture;
  GtkWidget       *scroll_win;
  GtkWidget       *zoom_label;
  int              tex_width;
  int              tex_height;
  double           zoom; /* 0.0 = FIT mode, >0.0 = explicit scale */
} ImageCardState;

typedef struct {
  IdeMarkdownView *md;
  PopplerDocument *doc;
  char            *resolved_path;
  GtkWidget       *picture;
  GtkWidget       *page_label;
  GtkWidget       *prev_btn;
  GtkWidget       *next_btn;
  int              page_idx;
  int              n_pages;
} PdfCardState;

typedef struct {
  GtkWidget *stack;
  GtkWidget *slide_label;
  GtkWidget *prev_btn;
  GtkWidget *next_btn;
  int        cur_idx;
  int        n_slides;
} CarouselCardState;

static void render_markdown_into_box(IdeMarkdownView *md,
                                     GtkWidget       *target_box,
                                     const char      *markdown_text,
                                     guint            base_line_offset,
                                     guint           *io_tasks_done,
                                     guint           *io_tasks_total);

char *ide_markdown_toggle_checklist_line(const char *markdown_text,
                                         guint       line_1based) {
  if (!markdown_text || line_1based == 0) return g_strdup(markdown_text ? markdown_text : "");
  g_auto(GStrv) lines = g_strsplit(markdown_text, "\n", -1);
  guint n = g_strv_length(lines);
  if (line_1based > n) return g_strdup(markdown_text);

  char *line = lines[line_1based - 1];
  char *unck = strstr(line, "[ ]");
  char *ck_low = strstr(line, "[x]");
  char *ck_up = strstr(line, "[X]");

  if (unck) {
    unck[1] = 'x';
  } else if (ck_low) {
    ck_low[1] = ' ';
  } else if (ck_up) {
    ck_up[1] = ' ';
  }
  return g_strjoinv("\n", lines);
}

/* ─── 2. Asset Classification, Embed Extraction & Path Resolution ─── */

IdeMarkdownEmbedKind ide_markdown_classify_asset(const char *path_or_ref) {
  if (!path_or_ref || !*path_or_ref) return IDE_MD_EMBED_NONE;

  g_autofree char *clean = g_strdup(path_or_ref);
  char *q = strpbrk(clean, "?#");
  if (q) *q = '\0';

  const char *dot = strrchr(clean, '.');
  if (!dot || !dot[1]) return IDE_MD_EMBED_DOCUMENT;

  g_autofree char *ext = g_ascii_strdown(dot + 1, -1);

  if (g_strcmp0(ext, "svg") == 0 || g_strcmp0(ext, "png") == 0 ||
      g_strcmp0(ext, "jpg") == 0 || g_strcmp0(ext, "jpeg") == 0 ||
      g_strcmp0(ext, "webp") == 0 || g_strcmp0(ext, "gif") == 0 ||
      g_strcmp0(ext, "bmp") == 0 || g_strcmp0(ext, "ico") == 0 ||
      g_strcmp0(ext, "avif") == 0 || g_strcmp0(ext, "xpm") == 0) {
    return IDE_MD_EMBED_IMAGE;
  }

  if (g_strcmp0(ext, "pdf") == 0) {
    return IDE_MD_EMBED_PDF;
  }

  if (g_strcmp0(ext, "tar") == 0 || g_strcmp0(ext, "gz") == 0 ||
      g_strcmp0(ext, "tgz") == 0 || g_strcmp0(ext, "xz") == 0 ||
      g_strcmp0(ext, "zst") == 0 || g_strcmp0(ext, "bz2") == 0 ||
      g_strcmp0(ext, "zip") == 0 || g_strcmp0(ext, "7z") == 0) {
    return IDE_MD_EMBED_ARCHIVE;
  }

  if (g_strcmp0(ext, "mp4") == 0 || g_strcmp0(ext, "mkv") == 0 ||
      g_strcmp0(ext, "webm") == 0 || g_strcmp0(ext, "mov") == 0 ||
      g_strcmp0(ext, "avi") == 0 || g_strcmp0(ext, "mp3") == 0 ||
      g_strcmp0(ext, "flac") == 0 || g_strcmp0(ext, "ogg") == 0 ||
      g_strcmp0(ext, "opus") == 0 || g_strcmp0(ext, "wav") == 0 ||
      g_strcmp0(ext, "m4a") == 0) {
    return IDE_MD_EMBED_MEDIA;
  }

  return IDE_MD_EMBED_DOCUMENT;
}

static char *extract_html_attr(const char *tag, const char *attr_name) {
  if (!tag || !attr_name) return NULL;
  g_autofree char *pat_dq = g_strdup_printf("%s=\"", attr_name);
  g_autofree char *pat_sq = g_strdup_printf("%s='", attr_name);

  const char *p = strstr(tag, pat_dq);
  if (p) {
    p += strlen(pat_dq);
    const char *end = strchr(p, '"');
    if (end) return g_strndup(p, end - p);
  }
  p = strstr(tag, pat_sq);
  if (p) {
    p += strlen(pat_sq);
    const char *end = strchr(p, '\'');
    if (end) return g_strndup(p, end - p);
  }
  return NULL;
}

static char *strip_optional_markdown_title(const char *raw_src, char **out_title) {
  if (out_title) *out_title = NULL;
  if (!raw_src) return g_strdup("");
  g_autofree char *s = g_strstrip(g_strdup(raw_src));
  if (*s == '<') {
    char *gt = strchr(s + 1, '>');
    if (gt) return g_strndup(s + 1, gt - (s + 1));
  }
  /* Look for space followed by quote: path/to/img.svg "Title" */
  const char *q = strchr(s, '"');
  if (!q) q = strchr(s, '\'');
  if (q && q > s && (q[-1] == ' ' || q[-1] == '\t')) {
    const char *end_q = strrchr(q + 1, *q);
    if (out_title && end_q && end_q > q + 1) {
      *out_title = g_strndup(q + 1, end_q - (q + 1));
    }
    g_autofree char *path_part = g_strndup(s, q - s);
    return g_strstrip(g_strdup(path_part));
  }
  return g_strdup(s);
}

gboolean ide_markdown_extract_embed(const char  *line,
                                    char       **out_alt,
                                    char       **out_src,
                                    char       **out_link_url) {
  if (out_alt) *out_alt = NULL;
  if (out_src) *out_src = NULL;
  if (out_link_url) *out_link_url = NULL;
  if (!line || !*line) return FALSE;

  const char *p = line;
  while (*p == ' ' || *p == '\t') p++;

  /* 1. Linked Markdown Image Badge: [![alt](src)](link_url) */
  if (p[0] == '[' && p[1] == '!') {
    const char *alt_start = p + 3;
    const char *alt_end = strstr(alt_start, "](");
    if (alt_end) {
      const char *src_start = alt_end + 2;
      const char *src_end = strstr(src_start, ")](");
      if (src_end) {
        const char *lnk_start = src_end + 3;
        const char *lnk_end = strchr(lnk_start, ')');
        if (lnk_end) {
          g_autofree char *raw_src = g_strndup(src_start, src_end - src_start);
          g_autofree char *title = NULL;
          char *clean_src = strip_optional_markdown_title(raw_src, &title);
          if (out_alt) {
            *out_alt = (alt_end > alt_start)
                           ? g_strndup(alt_start, alt_end - alt_start)
                           : (title ? g_strdup(title) : g_path_get_basename(clean_src));
          }
          if (out_src) *out_src = clean_src;
          else g_free(clean_src);
          if (out_link_url)
            *out_link_url = g_strndup(lnk_start, lnk_end - lnk_start);
          return TRUE;
        }
      }
    }
  }

  /* 2. Standard Markdown Image / Embed: ![alt](src "optional title") */
  if (p[0] == '!' && p[1] == '[') {
    const char *alt_start = p + 2;
    const char *alt_end = strstr(alt_start, "](");
    if (alt_end) {
      const char *src_start = alt_end + 2;
      /* Find matching closing ')' */
      int depth = 1;
      const char *cur = src_start;
      gboolean in_quotes = FALSE;
      while (*cur && depth > 0) {
        if (*cur == '"') in_quotes = !in_quotes;
        else if (!in_quotes) {
          if (*cur == '(') depth++;
          else if (*cur == ')') depth--;
        }
        if (depth > 0) cur++;
      }
      if (*cur == ')' && cur > src_start) {
        /* Ensure the rest of the line is only whitespace */
        const char *rest = cur + 1;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (*rest == '\0') {
          g_autofree char *raw_src = g_strndup(src_start, cur - src_start);
          g_autofree char *title = NULL;
          char *clean_src = strip_optional_markdown_title(raw_src, &title);
          if (out_alt) {
            *out_alt = (alt_end > alt_start)
                           ? g_strndup(alt_start, alt_end - alt_start)
                           : (title ? g_strdup(title) : g_path_get_basename(clean_src));
          }
          if (out_src) *out_src = clean_src;
          else g_free(clean_src);
          return TRUE;
        }
      }
    }
  }

  /* 3. HTML <img ...>, <embed ...>, <video ...>, <audio ...> */
  const char *img_tag = strstr(p, "<img ");
  if (!img_tag) img_tag = strstr(p, "<embed ");
  if (!img_tag) img_tag = strstr(p, "<video ");
  if (!img_tag) img_tag = strstr(p, "<audio ");
  if (img_tag) {
    char *src = extract_html_attr(img_tag, "src");
    if (src && *src) {
      char *alt = extract_html_attr(img_tag, "alt");
      if (!alt) alt = extract_html_attr(img_tag, "title");
      if (!alt) alt = g_path_get_basename(src);

      const char *a_tag = strstr(p, "<a ");
      char *href = a_tag ? extract_html_attr(a_tag, "href") : NULL;

      if (out_alt) *out_alt = alt;
      else g_free(alt);
      if (out_src) *out_src = src;
      else g_free(src);
      if (out_link_url) *out_link_url = href;
      else g_free(href);
      return TRUE;
    }
    g_free(src);
  }

  return FALSE;
}

char *ide_markdown_resolve_asset_path(const char *raw_ref,
                                      const char *doc_dir,
                                      const char *workspace_root,
                                      guint      *out_line_1based) {
  if (out_line_1based) *out_line_1based = 0;
  if (!raw_ref || !*raw_ref) return NULL;

  g_autofree char *work = g_strstrip(g_strdup(raw_ref));

  /* Extract optional #L123 or #123 line anchor */
  char *hash = strchr(work, '#');
  if (hash) {
    const char *frag = hash + 1;
    if (*frag == 'L' || *frag == 'l') frag++;
    if (g_ascii_isdigit(*frag)) {
      guint ln = (guint)g_ascii_strtoull(frag, NULL, 10);
      if (out_line_1based) *out_line_1based = ln;
    }
    *hash = '\0';
  }

  /* Strip query string if any (?raw=true) */
  char *query = strchr(work, '?');
  if (query) *query = '\0';

  if (!*work) return NULL;

  /* Do not resolve remote http(s):// URLs to local disk paths */
  if (g_str_has_prefix(work, "http://") || g_str_has_prefix(work, "https://") ||
      g_str_has_prefix(work, "mailto:")) {
    return NULL;
  }

  g_autofree char *clean_path = NULL;
  if (g_str_has_prefix(work, "file://")) {
    clean_path = g_filename_from_uri(work, NULL, NULL);
    if (!clean_path) clean_path = g_strdup(work + 7);
  } else if (work[0] == '~' && (work[1] == '/' || work[1] == '\0')) {
    clean_path = g_build_filename(g_get_home_dir(), work + 1, NULL);
  } else {
    clean_path = g_strdup(work);
  }

  if (g_path_is_absolute(clean_path)) {
    return g_steal_pointer(&clean_path);
  }

  /* Strip leading "./" */
  const char *rel = clean_path;
  while (rel[0] == '.' && rel[1] == '/') rel += 2;

  /* 1. Try relative to the Markdown file's directory */
  if (doc_dir && *doc_dir) {
    g_autofree char *cand1 = g_build_filename(doc_dir, rel, NULL);
    if (g_file_test(cand1, G_FILE_TEST_EXISTS))
      return g_steal_pointer(&cand1);
  }

  /* 2. Try relative to the IDE workspace root */
  if (workspace_root && *workspace_root) {
    g_autofree char *cand2 = g_build_filename(workspace_root, rel, NULL);
    if (g_file_test(cand2, G_FILE_TEST_EXISTS))
      return g_steal_pointer(&cand2);
  }

  /* 3. Try relative to current working directory */
  g_autofree char *cwd = g_get_current_dir();
  if (cwd) {
    g_autofree char *cand3 = g_build_filename(cwd, rel, NULL);
    if (g_file_test(cand3, G_FILE_TEST_EXISTS))
      return g_steal_pointer(&cand3);
  }

  /* Fallback: return path joined with doc_dir or workspace_root for diagnostics */
  if (doc_dir && *doc_dir)
    return g_build_filename(doc_dir, rel, NULL);
  if (workspace_root && *workspace_root)
    return g_build_filename(workspace_root, rel, NULL);
  return g_strdup(rel);
}

/* ─── 3. Rich Recursive Inline Markdown + HTML -> Pango Markup Formatter ─── */

static gboolean is_word_char(char c) {
  return g_ascii_isalnum(c) || c == '_';
}

char *ide_markdown_format_inline_pango(const char *raw) {
  if (!raw || !*raw) return g_strdup("");
  GString *out = g_string_new("");
  const char *p = raw;

  while (*p) {
    /* 1. Escaped character \* \_ \` \[ */
    if (p[0] == '\\' && p[1] != '\0' && strchr("*_`~[]()<>#+-!", p[1]) != NULL) {
      if (p[1] == '&') g_string_append(out, "&amp;");
      else if (p[1] == '<') g_string_append(out, "&lt;");
      else if (p[1] == '>') g_string_append(out, "&gt;");
      else g_string_append_c(out, p[1]);
      p += 2;
      continue;
    }

    /* 2. Inline `code` or ``code`` */
    if (*p == '`') {
      gsize ticks = 1;
      while (p[ticks] == '`') ticks++;
      g_autofree char *delim = g_strnfill(ticks, '`');
      const char *end = strstr(p + ticks, delim);
      if (end && end > p + ticks) {
        g_autofree char *inner = g_strndup(p + ticks, end - (p + ticks));
        g_autofree char *esc = g_markup_escape_text(inner, -1);
        g_string_append_printf(
            out,
            "<span font_family=\"Fira Code\" background=\"#1e1e1e\" foreground=\"#7dd3fc\"> %s </span>",
            esc);
        p = end + ticks;
        continue;
      }
    }

    /* 3. HTML inline tags: <kbd>, </kbd>, <mark>, </mark>, <code>, </code>, <br>, <b>, <i>, <s>, <sub>, <sup> */
    if (*p == '<') {
      if (g_ascii_strncasecmp(p, "<br>", 4) == 0) {
        g_string_append_c(out, '\n');
        p += 4;
        continue;
      }
      if (g_ascii_strncasecmp(p, "<br/>", 5) == 0 ||
          g_ascii_strncasecmp(p, "<br />", 6) == 0) {
        g_string_append_c(out, '\n');
        p = strchr(p, '>') + 1;
        continue;
      }
      if (g_ascii_strncasecmp(p, "<kbd>", 5) == 0) {
        const char *end = strstr(p + 5, "</kbd>");
        if (!end) end = strstr(p + 5, "</KBD>");
        if (end) {
          g_autofree char *inner = g_strndup(p + 5, end - (p + 5));
          g_autofree char *esc = g_markup_escape_text(inner, -1);
          g_string_append_printf(
              out,
              "<span font_family=\"Fira Code\" weight=\"bold\" background=\"#262626\" foreground=\"#ffffff\"> [%s] </span>",
              esc);
          p = end + 6;
          continue;
        }
      }
      if (g_ascii_strncasecmp(p, "<mark>", 6) == 0) {
        const char *end = strstr(p + 6, "</mark>");
        if (end) {
          g_autofree char *inner = g_strndup(p + 6, end - (p + 6));
          g_autofree char *fmt = ide_markdown_format_inline_pango(inner);
          g_string_append_printf(
              out,
              "<span background=\"#3a2e00\" foreground=\"#ffe66d\"> %s </span>",
              fmt);
          p = end + 7;
          continue;
        }
      }
      if (g_ascii_strncasecmp(p, "<code>", 6) == 0) {
        const char *end = strstr(p + 6, "</code>");
        if (end) {
          g_autofree char *inner = g_strndup(p + 6, end - (p + 6));
          g_autofree char *esc = g_markup_escape_text(inner, -1);
          g_string_append_printf(
              out,
              "<span font_family=\"Fira Code\" background=\"#1e1e1e\" foreground=\"#7dd3fc\"> %s </span>",
              esc);
          p = end + 7;
          continue;
        }
      }
      /* Auto-link <https://...> or <http://...> or <file:///...> */
      if (g_str_has_prefix(p, "<https://") || g_str_has_prefix(p, "<http://") ||
          g_str_has_prefix(p, "<file://")) {
        const char *end = strchr(p + 1, '>');
        if (end && !strchr(p + 1, ' ')) {
          g_autofree char *url = g_strndup(p + 1, end - (p + 1));
          g_autofree char *esc = g_markup_escape_text(url, -1);
          g_string_append_printf(
              out,
              "<a href=\"%s\"><span foreground=\"#00d8f0\" underline=\"single\">%s</span></a>",
              esc,
              esc);
          p = end + 1;
          continue;
        }
      }
    }

    /* 4. Inline Image inside paragraph: ![alt](src) */
    if (p[0] == '!' && p[1] == '[') {
      const char *alt_end = strstr(p + 2, "](");
      if (alt_end) {
        const char *src_start = alt_end + 2;
        const char *src_end = strchr(src_start, ')');
        if (src_end) {
          g_autofree char *alt = g_strndup(p + 2, alt_end - (p + 2));
          g_autofree char *raw_src = g_strndup(src_start, src_end - src_start);
          g_autofree char *clean_src = strip_optional_markdown_title(raw_src, NULL);
          g_autofree char *esc_alt = g_markup_escape_text(*alt ? alt : clean_src, -1);
          g_autofree char *esc_src = g_markup_escape_text(clean_src, -1);
          g_string_append_printf(
              out,
              "<a href=\"%s\"><span font_family=\"Fira Code\" background=\"#16222c\" foreground=\"#00d8f0\"> [🖼 %s] </span></a>",
              esc_src,
              esc_alt);
          p = src_end + 1;
          continue;
        }
      }
    }

    /* 5. Clickable Markdown Link: [Label](url) */
    if (*p == '[') {
      /* Find matching ']' allowing nested `code` or brackets */
      const char *cur = p + 1;
      int br_depth = 1;
      while (*cur && br_depth > 0) {
        if (*cur == '[') br_depth++;
        else if (*cur == ']') br_depth--;
        if (br_depth > 0) cur++;
      }
      if (*cur == ']' && cur[1] == '(') {
        const char *url_start = cur + 2;
        const char *url_end = strchr(url_start, ')');
        if (url_end && url_end > url_start) {
          g_autofree char *lbl_raw = g_strndup(p + 1, cur - (p + 1));
          g_autofree char *url_raw = g_strndup(url_start, url_end - url_start);
          g_autofree char *clean_url = strip_optional_markdown_title(url_raw, NULL);
          g_autofree char *lbl_fmt = ide_markdown_format_inline_pango(lbl_raw);
          g_autofree char *esc_url = g_markup_escape_text(clean_url, -1);
          g_string_append_printf(
              out,
              "<a href=\"%s\"><span foreground=\"#00d8f0\" underline=\"single\">%s</span></a>",
              esc_url,
              lbl_fmt);
          p = url_end + 1;
          continue;
        }
      }
    }

    /* 6. Bold + Italic: ***text*** */
    if (p[0] == '*' && p[1] == '*' && p[2] == '*') {
      const char *end = strstr(p + 3, "***");
      if (end && end > p + 3) {
        g_autofree char *inner = g_strndup(p + 3, end - (p + 3));
        g_autofree char *fmt = ide_markdown_format_inline_pango(inner);
        g_string_append_printf(out, "<b><i>%s</i></b>", fmt);
        p = end + 3;
        continue;
      }
    }

    /* 7. Bold: **text** or __text__ */
    if ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_')) {
      char delim[3] = { p[0], p[1], '\0' };
      const char *end = strstr(p + 2, delim);
      if (end && end > p + 2) {
        g_autofree char *inner = g_strndup(p + 2, end - (p + 2));
        g_autofree char *fmt = ide_markdown_format_inline_pango(inner);
        g_string_append_printf(out, "<b>%s</b>", fmt);
        p = end + 2;
        continue;
      }
    }

    /* 8. Strikethrough: ~~text~~ */
    if (p[0] == '~' && p[1] == '~') {
      const char *end = strstr(p + 2, "~~");
      if (end && end > p + 2) {
        g_autofree char *inner = g_strndup(p + 2, end - (p + 2));
        g_autofree char *fmt = ide_markdown_format_inline_pango(inner);
        g_string_append_printf(out, "<s>%s</s>", fmt);
        p = end + 2;
        continue;
      }
    }

    /* 9. Italic: *text* or _text_ (avoiding snake_case_identifiers) */
    if ((*p == '*' || *p == '_') && p[1] != *p && p[1] != ' ' && p[1] != '\0') {
      gboolean prev_word = (p > raw && is_word_char(p[-1]));
      if (!prev_word) {
        char d = *p;
        const char *end = strchr(p + 1, d);
        if (end && end > p + 1 && end[-1] != ' ' &&
            !(d == '_' && is_word_char(end[1]))) {
          g_autofree char *inner = g_strndup(p + 1, end - (p + 1));
          g_autofree char *fmt = ide_markdown_format_inline_pango(inner);
          g_string_append_printf(out, "<i>%s</i>", fmt);
          p = end + 1;
          continue;
        }
      }
    }

    /* 10. Escape XML special characters */
    if (*p == '&') g_string_append(out, "&amp;");
    else if (*p == '<') g_string_append(out, "&lt;");
    else if (*p == '>') g_string_append(out, "&gt;");
    else g_string_append_c(out, *p);
    p++;
  }

  return g_string_free(out, FALSE);
}

/* ─── 4. Link Activation & External / Internal File Navigation ─── */

static void open_or_navigate_target(IdeMarkdownView *md, const char *uri_or_ref) {
  if (!md || !uri_or_ref || !*uri_or_ref) return;

  /* 1. In-document heading anchor (#section-name) */
  if (uri_or_ref[0] == '#') {
    if (g_ascii_isdigit(uri_or_ref[1]) ||
        ((uri_or_ref[1] == 'L' || uri_or_ref[1] == 'l') && g_ascii_isdigit(uri_or_ref[2]))) {
      const char *num = (uri_or_ref[1] == 'L' || uri_or_ref[1] == 'l') ? uri_or_ref + 2 : uri_or_ref + 1;
      guint ln = (guint)g_ascii_strtoull(num, NULL, 10);
      if (ln > 0 && md->jump_cb) md->jump_cb(ln, md->user_data);
      return;
    }
    /* Search current_markdown for matching heading slug */
    if (md->current_markdown && md->jump_cb) {
      g_autofree char *slug = g_ascii_strdown(uri_or_ref + 1, -1);
      g_strdelimit(slug, "-_", ' ');
      g_auto(GStrv) lines = g_strsplit(md->current_markdown, "\n", -1);
      for (guint i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '#') {
          g_autofree char *low_line = g_ascii_strdown(lines[i], -1);
          g_strdelimit(low_line, "-_", ' ');
          if (strstr(low_line, slug) != NULL) {
            md->jump_cb(i + 1, md->user_data);
            return;
          }
        }
      }
    }
    return;
  }

  /* 2. Remote Web URL (http://, https://, mailto:) */
  if (g_str_has_prefix(uri_or_ref, "http://") ||
      g_str_has_prefix(uri_or_ref, "https://") ||
      g_str_has_prefix(uri_or_ref, "mailto:")) {
    if (!g_app_info_launch_default_for_uri(uri_or_ref, NULL, NULL)) {
      char *argv[] = { (char *)"xdg-open", (char *)uri_or_ref, NULL };
      g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    return;
  }

  /* 3. Local File or file:// URI (with optional #L123 line anchor) */
  guint target_line = 1;
  g_autofree char *resolved = ide_markdown_resolve_asset_path(
      uri_or_ref, md->doc_dir, md->workspace_root, &target_line);
  if (!resolved) return;

  IdeMarkdownEmbedKind kind = ide_markdown_classify_asset(resolved);
  if ((kind == IDE_MD_EMBED_DOCUMENT || g_str_has_suffix(resolved, ".svg")) &&
      md->open_file_cb && g_file_test(resolved, G_FILE_TEST_IS_REGULAR)) {
    md->open_file_cb(resolved, target_line > 0 ? target_line : 1, 1, md->user_data);
    return;
  }

  g_autofree char *file_uri = g_filename_to_uri(resolved, NULL, NULL);
  if (file_uri) {
    if (!g_app_info_launch_default_for_uri(file_uri, NULL, NULL)) {
      char *argv[] = { (char *)"xdg-open", resolved, NULL };
      g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
  }
}

static gboolean on_markdown_label_activate_link(GtkLabel   *label,
                                                const char *uri,
                                                gpointer    user_data) {
  (void)label;
  IdeMarkdownView *md = user_data;
  open_or_navigate_target(md, uri);
  return TRUE;
}

static GtkWidget *create_rich_markdown_label(IdeMarkdownView *md,
                                             const char      *raw_markdown,
                                             const char      *css_class) {
  GtkWidget *lbl = gtk_label_new(NULL);
  g_autofree char *mk = ide_markdown_format_inline_pango(raw_markdown);
  gtk_label_set_markup(GTK_LABEL(lbl), mk);
  if (css_class && *css_class)
    gtk_widget_add_css_class(lbl, css_class);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
  g_signal_connect(lbl,
                   "activate-link",
                   G_CALLBACK(on_markdown_label_activate_link),
                   md);
  return lbl;
}

/* ─── 5. Mermaid Card Widget ─── */

static void mermaid_card_state_free(gpointer data) {
  MermaidCardState *st = data;
  if (!st) return;
  ide_mermaid_graph_free(st->graph);
  g_free(st);
}

static void on_mermaid_draw(GtkDrawingArea *area,
                            cairo_t        *cr,
                            int             width,
                            int             height,
                            gpointer        user_data) {
  (void)area;
  MermaidCardState *st = user_data;
  if (!st || !st->graph) return;
  ide_mermaid_render_cairo(st->graph, cr, st->zoom, width, height);
}

static void update_mermaid_zoom(MermaidCardState *st, double new_zoom) {
  st->zoom = CLAMP(new_zoom, 0.5, 2.5);
  g_autofree char *lbl = g_strdup_printf("%d%%", (int)round(st->zoom * 100.0));
  gtk_label_set_text(GTK_LABEL(st->zoom_label), lbl);
  gtk_drawing_area_set_content_width(
      GTK_DRAWING_AREA(st->drawing_area),
      (int)(st->graph->canvas_width * st->zoom));
  gtk_drawing_area_set_content_height(
      GTK_DRAWING_AREA(st->drawing_area),
      (int)(st->graph->canvas_height * st->zoom));
  gtk_widget_queue_draw(st->drawing_area);
}

static void on_zoom_out_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  MermaidCardState *st = user_data;
  update_mermaid_zoom(st, st->zoom - 0.2);
}

static void on_zoom_reset_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  MermaidCardState *st = user_data;
  update_mermaid_zoom(st, 1.0);
}

static void on_zoom_in_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  MermaidCardState *st = user_data;
  update_mermaid_zoom(st, st->zoom + 0.2);
}

static void on_export_svg_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  MermaidCardState *st = user_data;
  g_autofree char *out =
      g_build_filename(g_get_tmp_dir(), "kajo-mermaid-diagram.svg", NULL);
  ide_mermaid_export_svg(st->graph, out);
}

static void on_mermaid_canvas_clicked(GtkGestureClick *gesture,
                                      int              n_press,
                                      double           x,
                                      double           y,
                                      gpointer         user_data) {
  (void)gesture;
  (void)n_press;
  MermaidCardState *st = user_data;
  if (!st || !st->graph || !st->md) return;
  const IdeMermaidNode *hit = ide_mermaid_hit_test_node(st->graph, st->zoom, x, y);
  if (hit && st->md->jump_cb && hit->source_line_1based > 0) {
    st->md->jump_cb(hit->source_line_1based, st->md->user_data);
  }
}

static GtkWidget *build_mermaid_card_widget(IdeMarkdownView *md,
                                            const char      *mermaid_code,
                                            guint            start_line_1based) {
  MermaidCardState *st = g_new0(MermaidCardState, 1);
  st->md = md;
  st->zoom = 1.0;
  st->graph = ide_mermaid_parse_graph(mermaid_code, start_line_1based);

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "ide-mermaid-card");
  g_object_set_data_full(G_OBJECT(card),
                         "mermaid-state",
                         st,
                         mermaid_card_state_free);

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(hdr, "ide-mermaid-toolbar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *title = gtk_label_new("❖ MERMAID VECTOR DIAGRAM");
  gtk_widget_add_css_class(title, "ide-tree-header-title");
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);

  GtkWidget *kind_lbl = gtk_label_new(st->graph->kind_badge);
  gtk_widget_add_css_class(kind_lbl, "shell-popover-badge");
  gtk_widget_set_valign(kind_lbl, GTK_ALIGN_CENTER);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);

  GtkWidget *btn_zout = gtk_button_new_with_label("🔍 −");
  gtk_widget_set_focusable(btn_zout, FALSE);
  gtk_widget_add_css_class(btn_zout, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_zout, GTK_ALIGN_CENTER);
  g_signal_connect(btn_zout, "clicked", G_CALLBACK(on_zoom_out_clicked), st);

  st->zoom_label = gtk_label_new("100%");
  gtk_widget_add_css_class(st->zoom_label, "shell-popover-badge");
  gtk_widget_add_css_class(st->zoom_label, "accent");
  gtk_widget_set_valign(st->zoom_label, GTK_ALIGN_CENTER);

  GtkWidget *btn_zin = gtk_button_new_with_label("🔍 +");
  gtk_widget_set_focusable(btn_zin, FALSE);
  gtk_widget_add_css_class(btn_zin, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_zin, GTK_ALIGN_CENTER);
  g_signal_connect(btn_zin, "clicked", G_CALLBACK(on_zoom_in_clicked), st);

  GtkWidget *btn_fit = gtk_button_new_with_label("⛶ FIT");
  gtk_widget_set_focusable(btn_fit, FALSE);
  gtk_widget_add_css_class(btn_fit, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_fit, GTK_ALIGN_CENTER);
  g_signal_connect(btn_fit, "clicked", G_CALLBACK(on_zoom_reset_clicked), st);

  GtkWidget *btn_svg = gtk_button_new_with_label("💾 EXPORT SVG");
  gtk_widget_set_focusable(btn_svg, FALSE);
  gtk_widget_add_css_class(btn_svg, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_svg, GTK_ALIGN_CENTER);
  g_signal_connect(btn_svg, "clicked", G_CALLBACK(on_export_svg_clicked), st);

  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), kind_lbl);
  gtk_box_append(GTK_BOX(hdr), spacer);
  gtk_box_append(GTK_BOX(hdr), btn_zout);
  gtk_box_append(GTK_BOX(hdr), st->zoom_label);
  gtk_box_append(GTK_BOX(hdr), btn_zin);
  gtk_box_append(GTK_BOX(hdr), btn_fit);
  gtk_box_append(GTK_BOX(hdr), btn_svg);
  gtk_box_append(GTK_BOX(card), hdr);

  st->drawing_area = gtk_drawing_area_new();
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(st->drawing_area),
                                     (int)st->graph->canvas_width);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(st->drawing_area),
                                      (int)st->graph->canvas_height);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(st->drawing_area),
                                 on_mermaid_draw,
                                 st,
                                 NULL);

  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click,
                   "pressed",
                   G_CALLBACK(on_mermaid_canvas_clicked),
                   st);
  gtk_widget_add_controller(st->drawing_area, GTK_EVENT_CONTROLLER(click));

  GtkWidget *canvas_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(canvas_scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(canvas_scroll), FALSE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(canvas_scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(canvas_scroll),
                                st->drawing_area);

  gtk_box_append(GTK_BOX(card), canvas_scroll);
  return card;
}

/* ─── 6. Embedded Image & Vector SVG Figure Card (`GtkPicture` + `GdkTexture`) ─── */

static void image_card_state_free(gpointer data) {
  ImageCardState *st = data;
  if (!st) return;
  g_clear_object(&st->texture);
  g_free(st->resolved_path);
  g_free(st->link_url);
  g_free(st);
}

static void apply_image_card_sizing(ImageCardState *st) {
  if (!st || !st->picture) return;
  int tw = MAX(1, st->tex_width);
  int th = MAX(1, st->tex_height);

  if (st->zoom <= 0.0) {
    /* FIT mode: scale proportionally to fit preview pane width (~520px) */
    gtk_label_set_text(GTK_LABEL(st->zoom_label), "FIT");
    double target_w = MIN((double)tw, 540.0);
    double scale = target_w / (double)tw;
    int nat_h = CLAMP((int)round((double)th * scale), 36, 420);
    gtk_picture_set_can_shrink(GTK_PICTURE(st->picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(st->picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_size_request(st->picture, -1, nat_h);
    if (st->scroll_win) {
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(st->scroll_win),
                                     GTK_POLICY_NEVER,
                                     GTK_POLICY_NEVER);
    }
  } else {
    g_autofree char *ztxt = g_strdup_printf("%d%%", (int)round(st->zoom * 100.0));
    gtk_label_set_text(GTK_LABEL(st->zoom_label), ztxt);
    int w = MAX(24, (int)round((double)tw * st->zoom));
    int h = MAX(24, (int)round((double)th * st->zoom));
    gtk_picture_set_can_shrink(GTK_PICTURE(st->picture), FALSE);
    gtk_widget_set_size_request(st->picture, w, h);
    if (st->scroll_win) {
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(st->scroll_win),
                                     GTK_POLICY_AUTOMATIC,
                                     GTK_POLICY_AUTOMATIC);
      gtk_widget_set_size_request(st->scroll_win, -1, MIN(h + 12, 460));
    }
  }
}

static void on_img_zoom_out(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ImageCardState *st = user_data;
  double cur = (st->zoom <= 0.0) ? 1.0 : st->zoom;
  st->zoom = CLAMP(cur - 0.25, 0.25, 3.0);
  apply_image_card_sizing(st);
}

static void on_img_zoom_in(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ImageCardState *st = user_data;
  double cur = (st->zoom <= 0.0) ? 1.0 : st->zoom;
  st->zoom = CLAMP(cur + 0.25, 0.25, 3.0);
  apply_image_card_sizing(st);
}

static void on_img_zoom_fit(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ImageCardState *st = user_data;
  st->zoom = 0.0;
  apply_image_card_sizing(st);
}

static void on_img_open_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ImageCardState *st = user_data;
  if (!st || !st->md) return;
  if (st->link_url && *st->link_url) {
    open_or_navigate_target(st->md, st->link_url);
  } else if (st->resolved_path && *st->resolved_path) {
    g_autofree char *uri = g_filename_to_uri(st->resolved_path, NULL, NULL);
    if (uri && !g_app_info_launch_default_for_uri(uri, NULL, NULL)) {
      char *argv[] = { (char *)"xdg-open", st->resolved_path, NULL };
      g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
  }
}

static GtkWidget *build_image_card_from_texture(IdeMarkdownView *md,
                                                GdkTexture      *tex,
                                                const char      *alt_text,
                                                const char      *resolved_path,
                                                const char      *link_url,
                                                const char      *fmt_override) {
  ImageCardState *st = g_new0(ImageCardState, 1);
  st->md = md;
  st->texture = g_object_ref(tex);
  st->resolved_path = g_strdup(resolved_path ? resolved_path : "");
  st->link_url = g_strdup(link_url ? link_url : "");
  st->tex_width = gdk_texture_get_width(tex);
  st->tex_height = gdk_texture_get_height(tex);
  st->zoom = 0.0; /* Default to responsive FIT */

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "ide-mermaid-card");
  gtk_widget_add_css_class(card, "ide-md-figure-card");
  gtk_widget_set_hexpand(card, TRUE);
  g_object_set_data_full(G_OBJECT(card), "image-card-state", st, image_card_state_free);

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(hdr, "ide-mermaid-toolbar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  const char *ext = fmt_override;
  if (!ext && resolved_path) {
    const char *dot = strrchr(resolved_path, '.');
    ext = (dot && dot[1]) ? dot + 1 : "IMG";
  }
  g_autofree char *up_ext = g_ascii_strup(ext ? ext : "IMG", -1);
  g_autofree char *dim_txt =
      g_strdup_printf("🖼 %s • %d×%d", up_ext, st->tex_width, st->tex_height);

  GtkWidget *dim_badge = gtk_label_new(dim_txt);
  gtk_widget_add_css_class(dim_badge, "shell-popover-badge");
  gtk_widget_set_valign(dim_badge, GTK_ALIGN_CENTER);

  const char *caption_str = (alt_text && *alt_text) ? alt_text : "";
  GtkWidget *cap_lbl = gtk_label_new(caption_str);
  gtk_widget_add_css_class(cap_lbl, "ide-tree-header-title");
  gtk_label_set_ellipsize(GTK_LABEL(cap_lbl), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(cap_lbl), 0.0f);
  gtk_widget_set_hexpand(cap_lbl, TRUE);
  gtk_widget_set_valign(cap_lbl, GTK_ALIGN_CENTER);

  GtkWidget *btn_zout = gtk_button_new_with_label("🔍 −");
  gtk_widget_set_focusable(btn_zout, FALSE);
  gtk_widget_add_css_class(btn_zout, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_zout, GTK_ALIGN_CENTER);
  g_signal_connect(btn_zout, "clicked", G_CALLBACK(on_img_zoom_out), st);

  st->zoom_label = gtk_label_new("FIT");
  gtk_widget_add_css_class(st->zoom_label, "shell-popover-badge");
  gtk_widget_add_css_class(st->zoom_label, "accent");
  gtk_widget_set_valign(st->zoom_label, GTK_ALIGN_CENTER);

  GtkWidget *btn_zin = gtk_button_new_with_label("🔍 +");
  gtk_widget_set_focusable(btn_zin, FALSE);
  gtk_widget_add_css_class(btn_zin, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_zin, GTK_ALIGN_CENTER);
  g_signal_connect(btn_zin, "clicked", G_CALLBACK(on_img_zoom_in), st);

  GtkWidget *btn_fit = gtk_button_new_with_label("⛶ FIT");
  gtk_widget_set_focusable(btn_fit, FALSE);
  gtk_widget_add_css_class(btn_fit, "ide-toolbar-btn");
  gtk_widget_set_valign(btn_fit, GTK_ALIGN_CENTER);
  g_signal_connect(btn_fit, "clicked", G_CALLBACK(on_img_zoom_fit), st);

  gtk_box_append(GTK_BOX(hdr), dim_badge);
  gtk_box_append(GTK_BOX(hdr), cap_lbl);
  gtk_box_append(GTK_BOX(hdr), btn_zout);
  gtk_box_append(GTK_BOX(hdr), st->zoom_label);
  gtk_box_append(GTK_BOX(hdr), btn_zin);
  gtk_box_append(GTK_BOX(hdr), btn_fit);

  if ((resolved_path && *resolved_path) || (link_url && *link_url)) {
    GtkWidget *btn_open = gtk_button_new_with_label("↗ OPEN");
    gtk_widget_set_focusable(btn_open, FALSE);
    gtk_widget_add_css_class(btn_open, "ide-toolbar-btn");
    gtk_widget_set_valign(btn_open, GTK_ALIGN_CENTER);
    g_signal_connect(btn_open, "clicked", G_CALLBACK(on_img_open_clicked), st);
    gtk_box_append(GTK_BOX(hdr), btn_open);
  }

  gtk_box_append(GTK_BOX(card), hdr);

  st->picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));
  gtk_widget_set_hexpand(st->picture, TRUE);
  gtk_widget_set_margin_top(st->picture, 8);
  gtk_widget_set_margin_bottom(st->picture, 8);
  gtk_widget_set_margin_start(st->picture, 8);
  gtk_widget_set_margin_end(st->picture, 8);

  st->scroll_win = gtk_scrolled_window_new();
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(st->scroll_win), FALSE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(st->scroll_win), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(st->scroll_win), st->picture);

  apply_image_card_sizing(st);
  gtk_box_append(GTK_BOX(card), st->scroll_win);
  return card;
}

static GtkWidget *build_missing_asset_card(const char *kind_label,
                                           const char *alt_text,
                                           const char *raw_src,
                                           const char *resolved_path) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(box, "ide-md-alert");
  gtk_widget_add_css_class(box, "ide-md-alert-warning");

  g_autofree char *msg = g_strdup_printf(
      "<b>⚠ %s ASSET NOT FOUND:</b> <tt>%s</tt>\n"
      "<span foreground=\"#888888\">Alt: %s • Resolved: %s</span>",
      kind_label ? kind_label : "EMBEDDED",
      raw_src ? raw_src : "(empty)",
      alt_text && *alt_text ? alt_text : "—",
      resolved_path ? resolved_path : "unresolved");
  GtkWidget *lbl = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(lbl), msg);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  gtk_box_append(GTK_BOX(box), lbl);
  return box;
}

static GtkWidget *build_image_embed_card(IdeMarkdownView *md,
                                         const char      *alt_text,
                                         const char      *raw_src,
                                         const char      *resolved_path,
                                         const char      *link_url) {
  if (!resolved_path || !g_file_test(resolved_path, G_FILE_TEST_EXISTS)) {
    return build_missing_asset_card("IMAGE", alt_text, raw_src, resolved_path);
  }

  g_autoptr(GError) err = NULL;
  g_autoptr(GdkTexture) tex = gdk_texture_new_from_filename(resolved_path, &err);
  if (!tex) {
    return build_missing_asset_card("IMAGE DECODE ERROR", alt_text, raw_src, resolved_path);
  }

  return build_image_card_from_texture(md, tex, alt_text, resolved_path, link_url, NULL);
}

static GtkWidget *build_raw_svg_block_card(IdeMarkdownView *md, const char *svg_code) {
  if (!svg_code || !*svg_code) {
    return build_missing_asset_card("INLINE SVG", "Empty SVG block", "```svg", "");
  }
  g_autoptr(GBytes) bytes = g_bytes_new(svg_code, strlen(svg_code));
  g_autoptr(GdkTexture) tex = gdk_texture_new_from_bytes(bytes, NULL);
  if (!tex) {
    return build_missing_asset_card("INLINE SVG", "Invalid SVG XML", "```svg", "");
  }
  return build_image_card_from_texture(md, tex, "INLINE VECTOR SVG BLOCK", NULL, NULL, "SVG");
}

/* ─── 7. Embedded Multi-Page PDF Card (`poppler-glib`) ─── */

static void pdf_card_state_free(gpointer data) {
  PdfCardState *st = data;
  if (!st) return;
  g_clear_object(&st->doc);
  g_free(st->resolved_path);
  g_free(st);
}


static void update_pdf_card_page(PdfCardState *st) {
  if (!st || !st->doc) return;
  g_autoptr(GdkTexture) tex = kajo_pdf_render_page_texture(st->doc, st->page_idx, 420);
  if (tex) {
    gtk_picture_set_paintable(GTK_PICTURE(st->picture), GDK_PAINTABLE(tex));
    gtk_widget_set_size_request(st->picture, -1, 360);
  }
  g_autofree char *lbl =
      g_strdup_printf("PAGE %d / %d", st->page_idx + 1, st->n_pages);
  gtk_label_set_text(GTK_LABEL(st->page_label), lbl);
  gtk_widget_set_sensitive(st->prev_btn, st->page_idx > 0);
  gtk_widget_set_sensitive(st->next_btn, st->page_idx + 1 < st->n_pages);
}

static void on_pdf_card_prev(GtkButton *btn, gpointer user_data) {
  (void)btn;
  PdfCardState *st = user_data;
  if (st && st->page_idx > 0) {
    st->page_idx--;
    update_pdf_card_page(st);
  }
}

static void on_pdf_card_next(GtkButton *btn, gpointer user_data) {
  (void)btn;
  PdfCardState *st = user_data;
  if (st && st->page_idx + 1 < st->n_pages) {
    st->page_idx++;
    update_pdf_card_page(st);
  }
}

static void on_pdf_card_open(GtkButton *btn, gpointer user_data) {
  (void)btn;
  PdfCardState *st = user_data;
  if (!st || !st->resolved_path) return;
  open_or_navigate_target(st->md, st->resolved_path);
}

static GtkWidget *build_pdf_embed_card(IdeMarkdownView *md,
                                       const char      *alt_text,
                                       const char      *raw_src,
                                       const char      *resolved_path) {
  if (!resolved_path || !g_file_test(resolved_path, G_FILE_TEST_EXISTS)) {
    return build_missing_asset_card("PDF DOCUMENT", alt_text, raw_src, resolved_path);
  }

  g_autoptr(GFile) gf = g_file_new_for_path(resolved_path);
  PopplerDocument *doc = poppler_document_new_from_gfile(gf, NULL, NULL, NULL);
  if (!doc) {
    return build_missing_asset_card("PDF DECODE ERROR", alt_text, raw_src, resolved_path);
  }

  PdfCardState *st = g_new0(PdfCardState, 1);
  st->md = md;
  st->doc = doc;
  st->resolved_path = g_strdup(resolved_path);
  st->page_idx = 0;
  st->n_pages = MAX(1, poppler_document_get_n_pages(doc));

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "ide-mermaid-card");
  g_object_set_data_full(G_OBJECT(card), "pdf-card-state", st, pdf_card_state_free);

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(hdr, "ide-mermaid-toolbar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *badge = gtk_label_new("📄 EMBEDDED PDF");
  gtk_widget_add_css_class(badge, "shell-popover-badge");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

  g_autofree char *base = g_path_get_basename(resolved_path);
  GtkWidget *title = gtk_label_new((alt_text && *alt_text) ? alt_text : base);
  gtk_widget_add_css_class(title, "ide-tree-header-title");
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_set_hexpand(title, TRUE);
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);

  st->prev_btn = gtk_button_new_with_label("◀ PREV");
  gtk_widget_set_focusable(st->prev_btn, FALSE);
  gtk_widget_add_css_class(st->prev_btn, "ide-toolbar-btn");
  gtk_widget_set_valign(st->prev_btn, GTK_ALIGN_CENTER);
  g_signal_connect(st->prev_btn, "clicked", G_CALLBACK(on_pdf_card_prev), st);

  st->page_label = gtk_label_new("PAGE 1 / 1");
  gtk_widget_add_css_class(st->page_label, "shell-popover-badge");
  gtk_widget_add_css_class(st->page_label, "accent");
  gtk_widget_set_valign(st->page_label, GTK_ALIGN_CENTER);

  st->next_btn = gtk_button_new_with_label("NEXT ▶");
  gtk_widget_set_focusable(st->next_btn, FALSE);
  gtk_widget_add_css_class(st->next_btn, "ide-toolbar-btn");
  gtk_widget_set_valign(st->next_btn, GTK_ALIGN_CENTER);
  g_signal_connect(st->next_btn, "clicked", G_CALLBACK(on_pdf_card_next), st);

  GtkWidget *open_btn = gtk_button_new_with_label("↗ OPEN PDF");
  gtk_widget_set_focusable(open_btn, FALSE);
  gtk_widget_add_css_class(open_btn, "ide-toolbar-btn");
  gtk_widget_set_valign(open_btn, GTK_ALIGN_CENTER);
  g_signal_connect(open_btn, "clicked", G_CALLBACK(on_pdf_card_open), st);

  gtk_box_append(GTK_BOX(hdr), badge);
  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), st->prev_btn);
  gtk_box_append(GTK_BOX(hdr), st->page_label);
  gtk_box_append(GTK_BOX(hdr), st->next_btn);
  gtk_box_append(GTK_BOX(hdr), open_btn);
  gtk_box_append(GTK_BOX(card), hdr);

  st->picture = gtk_picture_new();
  gtk_picture_set_can_shrink(GTK_PICTURE(st->picture), TRUE);
  gtk_picture_set_content_fit(GTK_PICTURE(st->picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_margin_top(st->picture, 8);
  gtk_widget_set_margin_bottom(st->picture, 8);
  gtk_box_append(GTK_BOX(card), st->picture);

  update_pdf_card_page(st);
  return card;
}

/* ─── 8. Embedded Archive Manifest Card (`libarchive`) & Media/File Cards ─── */

static GtkWidget *build_archive_embed_card(const char *alt_text,
                                           const char *raw_src,
                                           const char *resolved_path) {
  if (!resolved_path || !g_file_test(resolved_path, G_FILE_TEST_EXISTS)) {
    return build_missing_asset_card("ARCHIVE", alt_text, raw_src, resolved_path);
  }

  struct archive *a = archive_read_new();
  archive_read_support_filter_all(a);
  archive_read_support_format_all(a);

  if (archive_read_open_filename(a, resolved_path, 16384) != ARCHIVE_OK) {
    archive_read_free(a);
    return build_missing_asset_card("ARCHIVE READ ERROR", alt_text, raw_src, resolved_path);
  }

  GString *manifest = g_string_new("");
  struct archive_entry *entry = NULL;
  guint count = 0;
  guint64 total_bytes = 0;

  while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
    const char *pathname = archive_entry_pathname(entry);
    gint64 sz = archive_entry_size(entry);
    if (sz > 0) total_bytes += (guint64)sz;
    if (count < 16 && pathname) {
      g_autofree char *sz_str = g_format_size((guint64)MAX(0, sz));
      g_string_append_printf(manifest, "  %-44s %10s\n", pathname, sz_str);
    }
    count++;
    archive_read_data_skip(a);
  }
  archive_read_free(a);

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "ide-mermaid-card");

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr, "ide-mermaid-toolbar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  g_autofree char *tot_str = g_format_size(total_bytes);
  g_autofree char *bdg_txt = g_strdup_printf("📦 ARCHIVE • %u FILES • %s", count, tot_str);
  GtkWidget *badge = gtk_label_new(bdg_txt);
  gtk_widget_add_css_class(badge, "shell-popover-badge");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

  g_autofree char *base = g_path_get_basename(resolved_path);
  GtkWidget *title = gtk_label_new((alt_text && *alt_text) ? alt_text : base);
  gtk_widget_add_css_class(title, "ide-tree-header-title");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_set_hexpand(title, TRUE);
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);

  gtk_box_append(GTK_BOX(hdr), badge);
  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(card), hdr);

  GtkWidget *body = gtk_label_new(manifest->len > 0 ? manifest->str : "  (empty archive)");
  gtk_widget_add_css_class(body, "ide-source-view");
  gtk_label_set_xalign(GTK_LABEL(body), 0.0f);
  gtk_widget_set_margin_top(body, 6);
  gtk_widget_set_margin_bottom(body, 6);
  gtk_widget_set_margin_start(body, 8);
  gtk_box_append(GTK_BOX(card), body);

  g_string_free(manifest, TRUE);
  return card;
}

static void on_embed_launch_clicked(GtkButton *btn, gpointer user_data) {
  IdeMarkdownView *md = user_data;
  const char *path = g_object_get_data(G_OBJECT(btn), "target-path");
  if (md && path) open_or_navigate_target(md, path);
}

static GtkWidget *build_media_or_doc_embed_card(IdeMarkdownView     *md,
                                                IdeMarkdownEmbedKind kind,
                                                const char          *alt_text,
                                                const char          *raw_src,
                                                const char          *resolved_path) {
  if (!resolved_path || !g_file_test(resolved_path, G_FILE_TEST_EXISTS)) {
    return build_missing_asset_card(
        kind == IDE_MD_EMBED_MEDIA ? "MEDIA STREAM" : "EMBEDDED FILE",
        alt_text,
        raw_src,
        resolved_path);
  }

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(card, "ide-mermaid-toolbar");
  gtk_widget_set_valign(card, GTK_ALIGN_CENTER);

  const char *icon_prefix = (kind == IDE_MD_EMBED_MEDIA) ? "🎬 MEDIA" : "📎 FILE";
  GtkWidget *badge = gtk_label_new(icon_prefix);
  gtk_widget_add_css_class(badge, "shell-popover-badge");
  gtk_widget_add_css_class(badge, "accent");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

  g_autofree char *base = g_path_get_basename(resolved_path);
  g_autofree char *disp = g_strdup_printf(
      "%s (%s)", (alt_text && *alt_text) ? alt_text : base, base);
  GtkWidget *lbl = gtk_label_new(disp);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(lbl, TRUE);
  gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);

  const char *btn_txt =
      (kind == IDE_MD_EMBED_MEDIA) ? "▶ PLAY IN KAJO PLAYER" : "📂 OPEN IN IDE";
  GtkWidget *btn = gtk_button_new_with_label(btn_txt);
  gtk_widget_set_focusable(btn, FALSE);
  gtk_widget_add_css_class(btn, "ide-toolbar-btn");
  gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
  g_object_set_data_full(G_OBJECT(btn), "target-path", g_strdup(resolved_path), g_free);
  g_signal_connect(btn, "clicked", G_CALLBACK(on_embed_launch_clicked), md);

  gtk_box_append(GTK_BOX(card), badge);
  gtk_box_append(GTK_BOX(card), lbl);
  gtk_box_append(GTK_BOX(card), btn);
  return card;
}

static GtkWidget *build_embed_widget_for_line(IdeMarkdownView *md,
                                              const char      *alt,
                                              const char      *src,
                                              const char      *link_url) {
  g_autofree char *resolved = ide_markdown_resolve_asset_path(
      src, md ? md->doc_dir : NULL, md ? md->workspace_root : NULL, NULL);
  IdeMarkdownEmbedKind kind = ide_markdown_classify_asset(resolved ? resolved : src);

  switch (kind) {
    case IDE_MD_EMBED_IMAGE:
      return build_image_embed_card(md, alt, src, resolved, link_url);
    case IDE_MD_EMBED_PDF:
      return build_pdf_embed_card(md, alt, src, resolved);
    case IDE_MD_EMBED_ARCHIVE:
      return build_archive_embed_card(alt, src, resolved);
    case IDE_MD_EMBED_MEDIA:
    case IDE_MD_EMBED_DOCUMENT:
    default:
      return build_media_or_doc_embed_card(md, kind, alt, src, resolved);
  }
}

/* ─── 9. Code Block & Slide Carousel Cards ─── */

static void on_copy_code_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  const char *code = user_data;
  if (!code) return;
  GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
  if (cb) gdk_clipboard_set_text(cb, code);
}

static GtkWidget *build_code_block_widget(const char *lang_id, const char *code) {
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "ide-mermaid-card");
  gtk_widget_set_hexpand(card, TRUE);

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr, "ide-mermaid-toolbar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  g_autofree char *upper_lang =
      g_ascii_strup(lang_id && *lang_id ? lang_id : "CODE", -1);
  GtkWidget *badge = gtk_label_new(upper_lang);
  gtk_widget_add_css_class(badge, "shell-popover-badge");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);

  GtkWidget *copy_btn = gtk_button_new_with_label("📋 COPY CODE");
  gtk_widget_set_focusable(copy_btn, FALSE);
  gtk_widget_add_css_class(copy_btn, "ide-toolbar-btn");
  gtk_widget_set_valign(copy_btn, GTK_ALIGN_CENTER);
  char *dup_code = g_strdup(code ? code : "");
  g_object_set_data_full(G_OBJECT(copy_btn), "code-str", dup_code, g_free);
  g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_code_clicked), dup_code);

  gtk_box_append(GTK_BOX(hdr), badge);
  gtk_box_append(GTK_BOX(hdr), spacer);
  gtk_box_append(GTK_BOX(hdr), copy_btn);
  gtk_box_append(GTK_BOX(card), hdr);

  GtkSourceBuffer *sbuf = gtk_source_buffer_new(NULL);
  if (lang_id && *lang_id) {
    GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
    GtkSourceLanguage *lang = gtk_source_language_manager_get_language(lm, lang_id);
    if (lang) gtk_source_buffer_set_language(sbuf, lang);
  }
  GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
  GtkSourceStyleScheme *scheme =
      gtk_source_style_scheme_manager_get_scheme(sm, "kajo-metro-dark");
  if (scheme) gtk_source_buffer_set_style_scheme(sbuf, scheme);

  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(sbuf), code ? code : "", -1);
  ide_highlight_source_buffer(sbuf, lang_id, "kajo-metro-dark");
  GtkWidget *sview = gtk_source_view_new_with_buffer(sbuf);
  g_object_unref(sbuf);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(sview), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(sview), TRUE);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(sview), 8);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(sview), 8);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(sview), 10);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(sview), 10);
  gtk_widget_add_css_class(sview, "ide-source-view");

  GtkWidget *code_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(code_scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(code_scroll), FALSE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(code_scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(code_scroll), sview);

  gtk_box_append(GTK_BOX(card), code_scroll);
  return card;
}

static void update_carousel_state(CarouselCardState *st) {
  if (!st) return;
  g_autofree char *name = g_strdup_printf("slide-%d", st->cur_idx);
  gtk_stack_set_visible_child_name(GTK_STACK(st->stack), name);
  g_autofree char *lbl =
      g_strdup_printf("SLIDE %d / %d", st->cur_idx + 1, st->n_slides);
  gtk_label_set_text(GTK_LABEL(st->slide_label), lbl);
  gtk_widget_set_sensitive(st->prev_btn, st->cur_idx > 0);
  gtk_widget_set_sensitive(st->next_btn, st->cur_idx + 1 < st->n_slides);
}

static void on_carousel_prev(GtkButton *btn, gpointer user_data) {
  (void)btn;
  CarouselCardState *st = user_data;
  if (st && st->cur_idx > 0) {
    st->cur_idx--;
    update_carousel_state(st);
  }
}

static void on_carousel_next(GtkButton *btn, gpointer user_data) {
  (void)btn;
  CarouselCardState *st = user_data;
  if (st && st->cur_idx + 1 < st->n_slides) {
    st->cur_idx++;
    update_carousel_state(st);
  }
}

static GtkWidget *build_carousel_card(IdeMarkdownView *md,
                                      const char      *carousel_body,
                                      guint            base_line) {
  CarouselCardState *st = g_new0(CarouselCardState, 1);

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(card, "ide-mermaid-card");
  g_object_set_data_full(G_OBJECT(card), "carousel-state", st, g_free);

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(hdr, "ide-mermaid-toolbar");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *title = gtk_label_new("🎞 INTERACTIVE SLIDE CAROUSEL");
  gtk_widget_add_css_class(title, "ide-tree-header-title");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);

  st->prev_btn = gtk_button_new_with_label("◀ PREV");
  gtk_widget_set_focusable(st->prev_btn, FALSE);
  gtk_widget_add_css_class(st->prev_btn, "ide-toolbar-btn");
  gtk_widget_set_valign(st->prev_btn, GTK_ALIGN_CENTER);
  g_signal_connect(st->prev_btn, "clicked", G_CALLBACK(on_carousel_prev), st);

  st->slide_label = gtk_label_new("SLIDE 1 / 1");
  gtk_widget_add_css_class(st->slide_label, "shell-popover-badge");
  gtk_widget_add_css_class(st->slide_label, "accent");
  gtk_widget_set_valign(st->slide_label, GTK_ALIGN_CENTER);

  st->next_btn = gtk_button_new_with_label("NEXT ▶");
  gtk_widget_set_focusable(st->next_btn, FALSE);
  gtk_widget_add_css_class(st->next_btn, "ide-toolbar-btn");
  gtk_widget_set_valign(st->next_btn, GTK_ALIGN_CENTER);
  g_signal_connect(st->next_btn, "clicked", G_CALLBACK(on_carousel_next), st);

  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), st->prev_btn);
  gtk_box_append(GTK_BOX(hdr), st->slide_label);
  gtk_box_append(GTK_BOX(hdr), st->next_btn);
  gtk_box_append(GTK_BOX(card), hdr);

  st->stack = gtk_stack_new();
  g_auto(GStrv) slides = g_strsplit(carousel_body ? carousel_body : "", "<!-- slide -->", -1);
  st->n_slides = 0;

  for (guint i = 0; slides[i] != NULL; i++) {
    GtkWidget *slide_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(slide_box, 10);
    gtk_widget_set_margin_bottom(slide_box, 10);
    gtk_widget_set_margin_start(slide_box, 12);
    gtk_widget_set_margin_end(slide_box, 12);

    guint dummy_done = 0, dummy_tot = 0;
    render_markdown_into_box(md, slide_box, slides[i], base_line, &dummy_done, &dummy_tot);

    g_autofree char *sname = g_strdup_printf("slide-%d", st->n_slides);
    gtk_stack_add_named(GTK_STACK(st->stack), slide_box, sname);
    st->n_slides++;
  }
  if (st->n_slides == 0) st->n_slides = 1;

  gtk_box_append(GTK_BOX(card), st->stack);
  update_carousel_state(st);
  return card;
}

/* ─── 10. Block-Level Markdown & HTML Document Renderer ─── */

static void on_checklist_toggled(GtkCheckButton *chk, gpointer user_data) {
  IdeMarkdownView *md = user_data;
  if (!md || md->updating_checklists || !md->current_markdown) return;
  guint line_1based =
      GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(chk), "md-line"));
  if (line_1based == 0) return;

  char *updated =
      ide_markdown_toggle_checklist_line(md->current_markdown, line_1based);
  g_free(md->current_markdown);
  md->current_markdown = updated;
  if (md->edit_cb) md->edit_cb(md->current_markdown, md->user_data);
}

static void append_inline_embeds_from_text(IdeMarkdownView *md,
                                           GtkWidget       *target_box,
                                           const char      *text) {
  if (!text || !*text) return;
  const char *scan = text;
  while ((scan = strstr(scan, "![")) != NULL) {
    const char *alt_end = strstr(scan + 2, "](");
    if (!alt_end) break;
    const char *src_start = alt_end + 2;
    const char *src_end = strchr(src_start, ')');
    if (!src_end || src_end <= src_start) break;

    g_autofree char *inline_alt = g_strndup(scan + 2, alt_end - (scan + 2));
    g_autofree char *inline_raw_src = g_strndup(src_start, src_end - src_start);
    g_autofree char *inline_src =
        strip_optional_markdown_title(inline_raw_src, NULL);
    gtk_box_append(GTK_BOX(target_box),
                   build_embed_widget_for_line(md, inline_alt, inline_src, NULL));
    scan = src_end + 1;
  }
}

static gboolean is_pure_html_wrapper_line(const char *trimmed) {
  if (!trimmed || trimmed[0] != '<') return FALSE;
  if (g_str_has_prefix(trimmed, "<p>") || g_str_has_prefix(trimmed, "<p ") ||
      g_strcmp0(trimmed, "</p>") == 0 || g_str_has_prefix(trimmed, "<div") ||
      g_strcmp0(trimmed, "</div>") == 0 || g_str_has_prefix(trimmed, "<center>") ||
      g_strcmp0(trimmed, "</center>") == 0 || g_str_has_prefix(trimmed, "<!--")) {
    /* Only skip if it does not also contain an <img or text payload */
    if (strstr(trimmed, "<img ") == NULL && strstr(trimmed, "<embed ") == NULL &&
        strstr(trimmed, "<video ") == NULL && strstr(trimmed, "<audio ") == NULL) {
      const char *gt = strchr(trimmed, '>');
      if (gt && (gt[1] == '\0' || g_str_has_prefix(gt + 1, "</")))
        return TRUE;
    }
  }
  return FALSE;
}

static void render_markdown_into_box(IdeMarkdownView *md,
                                     GtkWidget       *target_box,
                                     const char      *markdown_text,
                                     guint            base_line_offset,
                                     guint           *io_tasks_done,
                                     guint           *io_tasks_total) {
  g_auto(GStrv) lines = g_strsplit(markdown_text ? markdown_text : "", "\n", -1);
  guint n_lines = g_strv_length(lines);

  for (guint i = 0; i < n_lines; i++) {
    const char *line = lines[i];
    const char *trimmed = line;
    guint indent_spaces = 0;
    while (*trimmed == ' ' || *trimmed == '\t') {
      indent_spaces += (*trimmed == '\t') ? 4 : 1;
      trimmed++;
    }

    if (*trimmed == '\0') continue;

    /* 1. Multi-backtick Fenced Blocks (````carousel or ```mermaid / ```svg / ```lang) */
    if (g_str_has_prefix(trimmed, "```")) {
      gsize fence_len = 3;
      while (trimmed[fence_len] == '`') fence_len++;
      g_autofree char *fence_str = g_strnfill(fence_len, '`');
      g_autofree char *lang = g_strstrip(g_strdup(trimmed + fence_len));
      guint block_start = base_line_offset + i + 2;
      GString *block = g_string_new("");
      i++;
      while (i < n_lines) {
        const char *btr = lines[i];
        while (*btr == ' ' || *btr == '\t') btr++;
        if (g_str_has_prefix(btr, fence_str)) break;
        if (block->len > 0) g_string_append_c(block, '\n');
        g_string_append(block, lines[i]);
        i++;
      }

      if (g_ascii_strcasecmp(lang, "mermaid") == 0) {
        gtk_box_append(GTK_BOX(target_box),
                       build_mermaid_card_widget(md, block->str, block_start));
      } else if (g_ascii_strcasecmp(lang, "svg") == 0) {
        gtk_box_append(GTK_BOX(target_box),
                       build_raw_svg_block_card(md, block->str));
      } else if (g_ascii_strcasecmp(lang, "carousel") == 0) {
        gtk_box_append(GTK_BOX(target_box),
                       build_carousel_card(md, block->str, block_start));
      } else {
        gtk_box_append(GTK_BOX(target_box),
                       build_code_block_widget(lang, block->str));
      }
      g_string_free(block, TRUE);
      continue;
    }

    /* 2. Collapsible HTML <details><summary>Title</summary> ... </details> */
    if (g_str_has_prefix(trimmed, "<details")) {
      g_autofree char *sum_title = g_strdup("DETAILS");
      GString *inner_md = g_string_new("");
      i++;
      while (i < n_lines) {
        const char *dtr = lines[i];
        while (*dtr == ' ' || *dtr == '\t') dtr++;
        if (g_str_has_prefix(dtr, "</details>")) break;
        const char *s_open = strstr(dtr, "<summary>");
        const char *s_close = strstr(dtr, "</summary>");
        if (s_open && s_close && s_close > s_open + 9) {
          g_free(sum_title);
          sum_title = g_strndup(s_open + 9, s_close - (s_open + 9));
        } else {
          if (inner_md->len > 0) g_string_append_c(inner_md, '\n');
          g_string_append(inner_md, lines[i]);
        }
        i++;
      }

      GtkWidget *exp = gtk_expander_new(sum_title);
      gtk_widget_add_css_class(exp, "ide-mermaid-card");
      GtkWidget *exp_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
      gtk_widget_set_margin_start(exp_box, 12);
      gtk_widget_set_margin_end(exp_box, 12);
      gtk_widget_set_margin_top(exp_box, 8);
      gtk_widget_set_margin_bottom(exp_box, 8);
      render_markdown_into_box(
          md, exp_box, inner_md->str, base_line_offset + i, io_tasks_done, io_tasks_total);
      gtk_expander_set_child(GTK_EXPANDER(exp), exp_box);
      gtk_box_append(GTK_BOX(target_box), exp);
      g_string_free(inner_md, TRUE);
      continue;
    }

    /* 3. Standalone Embedded Asset Line: ![alt](src), [![alt](src)](url), or <img src="..."> */
    {
      g_autofree char *alt = NULL;
      g_autofree char *src = NULL;
      g_autofree char *link_url = NULL;
      if (ide_markdown_extract_embed(trimmed, &alt, &src, &link_url)) {
        gtk_box_append(GTK_BOX(target_box),
                       build_embed_widget_for_line(md, alt, src, link_url));
        continue;
      }
    }

    /* 4. Skip pure HTML layout wrapper lines (<p align="center">, </p>, <div>, </div>) */
    if (is_pure_html_wrapper_line(trimmed)) {
      continue;
    }

    /* 5. Headings (# H1 through ###### H6) */
    if (trimmed[0] == '#') {
      guint h_level = 0;
      while (trimmed[h_level] == '#') h_level++;
      if (h_level >= 1 && h_level <= 6 && trimmed[h_level] == ' ') {
        const char *h_text = trimmed + h_level + 1;
        const char *h_cls = "ide-md-h3";
        if (h_level == 1) h_cls = "ide-md-h1";
        else if (h_level == 2) h_cls = "ide-md-h2";
        else if (h_level == 3) h_cls = "ide-md-h3";
        else if (h_level == 4) h_cls = "ide-md-h4";
        else h_cls = "ide-md-h5";

        GtkWidget *lbl = create_rich_markdown_label(md, h_text, h_cls);
        gtk_box_append(GTK_BOX(target_box), lbl);
        continue;
      }
    }

    /* 6. GitHub Alert Callouts (> [!NOTE], [!TIP], [!IMPORTANT], [!WARNING], [!CAUTION]) & Blockquotes */
    if (g_str_has_prefix(trimmed, ">")) {
      const char *first_body = (trimmed[1] == ' ') ? trimmed + 2 : trimmed + 1;
      const char *alert_cls = "ide-md-alert-note";
      const char *alert_hdr = NULL;

      if (strstr(first_body, "[!NOTE]")) {
        alert_cls = "ide-md-alert-note";
        alert_hdr = "ℹ NOTE";
        first_body = strstr(first_body, "[!NOTE]") + 7;
      } else if (strstr(first_body, "[!TIP]")) {
        alert_cls = "ide-md-alert-tip";
        alert_hdr = "💡 TIP";
        first_body = strstr(first_body, "[!TIP]") + 6;
      } else if (strstr(first_body, "[!IMPORTANT]")) {
        alert_cls = "ide-md-alert-important";
        alert_hdr = "✦ IMPORTANT";
        first_body = strstr(first_body, "[!IMPORTANT]") + 12;
      } else if (strstr(first_body, "[!WARNING]")) {
        alert_cls = "ide-md-alert-warning";
        alert_hdr = "⚠ WARNING";
        first_body = strstr(first_body, "[!WARNING]") + 10;
      } else if (strstr(first_body, "[!CAUTION]")) {
        alert_cls = "ide-md-alert-caution";
        alert_hdr = "🛑 CAUTION";
        first_body = strstr(first_body, "[!CAUTION]") + 10;
      }

      while (*first_body == ' ') first_body++;
      GString *quote = g_string_new(first_body);

      while (i + 1 < n_lines) {
        const char *nq = lines[i + 1];
        while (*nq == ' ' || *nq == '\t') nq++;
        if (!g_str_has_prefix(nq, ">")) break;
        i++;
        const char *qbody = (nq[1] == ' ') ? nq + 2 : nq + 1;
        if (quote->len > 0) g_string_append_c(quote, '\n');
        g_string_append(quote, qbody);
      }

      GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_add_css_class(box, "ide-md-alert");
      gtk_widget_add_css_class(box, alert_cls);

      if (alert_hdr) {
        GtkWidget *hdr_lbl = gtk_label_new(alert_hdr);
        gtk_widget_add_css_class(hdr_lbl, "shell-popover-badge");
        gtk_widget_set_halign(hdr_lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(box), hdr_lbl);
      }

      if (quote->len > 0) {
        GtkWidget *lbl = create_rich_markdown_label(md, quote->str, NULL);
        gtk_box_append(GTK_BOX(box), lbl);
      }

      gtk_box_append(GTK_BOX(target_box), box);
      g_string_free(quote, TRUE);
      continue;
    }

    /* 7. Horizontal Rules (---, ***, ___, <hr>) */
    if (g_strcmp0(trimmed, "---") == 0 || g_strcmp0(trimmed, "***") == 0 ||
        g_strcmp0(trimmed, "___") == 0 || g_str_has_prefix(trimmed, "<hr")) {
      GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
      gtk_widget_add_css_class(sep, "ide-md-hr");
      gtk_box_append(GTK_BOX(target_box), sep);
      continue;
    }

    /* 8. Interactive Task Checklists (- [ ] / - [x]) */
    if (g_str_has_prefix(trimmed, "- [ ] ") ||
        g_str_has_prefix(trimmed, "- [x] ") ||
        g_str_has_prefix(trimmed, "- [X] ") ||
        g_str_has_prefix(trimmed, "* [ ] ") ||
        g_str_has_prefix(trimmed, "* [x] ")) {
      gboolean checked = (trimmed[3] == 'x' || trimmed[3] == 'X');
      if (io_tasks_total) (*io_tasks_total)++;
      if (checked && io_tasks_done) (*io_tasks_done)++;

      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
      gtk_widget_set_margin_start(row, (int)(indent_spaces * 8));

      GtkWidget *chk = gtk_check_button_new();
      gtk_widget_set_focusable(chk, FALSE);
      gtk_widget_set_valign(chk, GTK_ALIGN_CENTER);
      md->updating_checklists = TRUE;
      gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), checked);
      md->updating_checklists = FALSE;
      g_object_set_data(G_OBJECT(chk),
                        "md-line",
                        GUINT_TO_POINTER(base_line_offset + i + 1));
      g_signal_connect(chk, "toggled", G_CALLBACK(on_checklist_toggled), md);

      GtkWidget *lbl = create_rich_markdown_label(md, trimmed + 6, NULL);
      gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand(lbl, TRUE);

      gtk_box_append(GTK_BOX(row), chk);
      gtk_box_append(GTK_BOX(row), lbl);
      gtk_box_append(GTK_BOX(target_box), row);
      continue;
    }

    /* 9. Unordered Bullet Lists (- item, * item, + item) */
    if ((trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') &&
        trimmed[1] == ' ') {
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_valign(row, GTK_ALIGN_START);
      gtk_widget_set_margin_start(row, (int)(indent_spaces * 8));

      const char *bullet_glyph =
          (indent_spaces >= 4) ? "▪" : ((indent_spaces >= 2) ? "◦" : "•");
      GtkWidget *b_lbl = gtk_label_new(bullet_glyph);
      gtk_widget_add_css_class(b_lbl, "ide-md-list-bullet");
      gtk_widget_set_valign(b_lbl, GTK_ALIGN_START);

      GtkWidget *item_lbl = create_rich_markdown_label(md, trimmed + 2, NULL);
      gtk_widget_set_hexpand(item_lbl, TRUE);

      gtk_box_append(GTK_BOX(row), b_lbl);
      gtk_box_append(GTK_BOX(row), item_lbl);
      gtk_box_append(GTK_BOX(target_box), row);
      append_inline_embeds_from_text(md, target_box, trimmed + 2);
      continue;
    }

    /* 10. Ordered Numbered Lists (1. item, 2. item) */
    if (g_ascii_isdigit(trimmed[0])) {
      const char *d = trimmed;
      while (g_ascii_isdigit(*d)) d++;
      if (d[0] == '.' && d[1] == ' ') {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(row, GTK_ALIGN_START);
        gtk_widget_set_margin_start(row, (int)(indent_spaces * 8));

        g_autofree char *num_txt = g_strndup(trimmed, (d - trimmed) + 1);
        GtkWidget *n_lbl = gtk_label_new(num_txt);
        gtk_widget_add_css_class(n_lbl, "ide-md-list-bullet");
        gtk_widget_set_valign(n_lbl, GTK_ALIGN_START);

        GtkWidget *item_lbl = create_rich_markdown_label(md, d + 2, NULL);
        gtk_widget_set_hexpand(item_lbl, TRUE);

        gtk_box_append(GTK_BOX(row), n_lbl);
        gtk_box_append(GTK_BOX(row), item_lbl);
        gtk_box_append(GTK_BOX(target_box), row);
        append_inline_embeds_from_text(md, target_box, d + 2);
        continue;
      }
    }

    /* 11. Native GtkGrid Pipe Tables (| Col 1 | Col 2 |) */
    if (trimmed[0] == '|' && strchr(trimmed + 1, '|') != NULL) {
      GtkWidget *grid = gtk_grid_new();
      gtk_widget_add_css_class(grid, "ide-md-table");
      gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
      gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
      int row_idx = 0;

      while (i < n_lines) {
        const char *tline = lines[i];
        while (*tline == ' ') tline++;
        if (tline[0] != '|') {
          i--;
          break;
        }
        if (strstr(tline, "---") == NULL) {
          g_auto(GStrv) cells = g_strsplit(tline, "|", -1);
          int col_idx = 0;
          for (guint c = 1; cells[c] != NULL && cells[c + 1] != NULL; c++) {
            g_autofree char *cell_txt = g_strstrip(g_strdup(cells[c]));
            g_autofree char *c_alt = NULL;
            g_autofree char *c_src = NULL;
            g_autofree char *c_lnk = NULL;
            GtkWidget *cell_widget = NULL;

            if (ide_markdown_extract_embed(cell_txt, &c_alt, &c_src, &c_lnk)) {
              g_autofree char *res = ide_markdown_resolve_asset_path(
                  c_src,
                  md ? md->doc_dir : NULL,
                  md ? md->workspace_root : NULL,
                  NULL);
              if (res && g_file_test(res, G_FILE_TEST_EXISTS) &&
                  ide_markdown_classify_asset(res) == IDE_MD_EMBED_IMAGE) {
                g_autoptr(GFile) gf = g_file_new_for_path(res);
                GdkTexture *tex = gdk_texture_new_from_file(gf, NULL);
                if (tex) {
                  GtkWidget *pic =
                      gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));
                  gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
                  gtk_picture_set_content_fit(GTK_PICTURE(pic),
                                              GTK_CONTENT_FIT_CONTAIN);
                  gtk_widget_set_size_request(pic, 32, 32);
                  gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
                  gtk_widget_set_valign(pic, GTK_ALIGN_CENTER);
                  if (c_alt && *c_alt)
                    gtk_widget_set_tooltip_text(pic, c_alt);
                  g_object_unref(tex);
                  cell_widget = pic;
                }
              }
            }

            if (!cell_widget) {
              cell_widget = create_rich_markdown_label(
                  md, cell_txt, row_idx == 0 ? "shell-popover-badge" : NULL);
              gtk_widget_set_valign(cell_widget, GTK_ALIGN_CENTER);
            }
            gtk_grid_attach(
                GTK_GRID(grid), cell_widget, col_idx++, row_idx, 1, 1);
          }
          row_idx++;
        }
        i++;
      }
      gtk_box_append(GTK_BOX(target_box), grid);
      continue;
    }

    /* 12. Regular Paragraph (with automatic inline ![alt](src) figure extraction) */
    {
      GtkWidget *p_lbl = create_rich_markdown_label(md, trimmed, NULL);
      gtk_box_append(GTK_BOX(target_box), p_lbl);
      append_inline_embeds_from_text(md, target_box, trimmed);
    }
  }
}

void ide_markdown_view_render(IdeMarkdownView *md, const char *markdown_text) {
  if (!md || !md->doc_box) return;

  g_free(md->current_markdown);
  md->current_markdown = g_strdup(markdown_text ? markdown_text : "");

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(md->doc_box)) != NULL)
    gtk_box_remove(GTK_BOX(md->doc_box), child);

  guint tasks_done = 0;
  guint tasks_total = 0;

  render_markdown_into_box(
      md, md->doc_box, md->current_markdown, 0, &tasks_done, &tasks_total);

  if (md->task_badge) {
    if (tasks_total > 0) {
      g_autofree char *tstr =
          g_strdup_printf("✓ %u / %u TASKS DONE", tasks_done, tasks_total);
      gtk_label_set_text(GTK_LABEL(md->task_badge), tstr);
      gtk_widget_set_visible(md->task_badge, TRUE);
    } else {
      gtk_widget_set_visible(md->task_badge, FALSE);
    }
  }
}

void ide_markdown_view_sync_scroll(IdeMarkdownView *md, double scroll_fraction) {
  if (!md || !md->scroll_win) return;
  GtkAdjustment *vadj =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(md->scroll_win));
  if (!vadj) return;
  double upper = gtk_adjustment_get_upper(vadj);
  double page = gtk_adjustment_get_page_size(vadj);
  if (upper > page) {
    gtk_adjustment_set_value(vadj, CLAMP(scroll_fraction, 0.0, 1.0) * (upper - page));
  }
}

GtkWidget *ide_markdown_view_get_widget(IdeMarkdownView *md) {
  return md ? md->root_box : NULL;
}

void ide_markdown_view_set_open_file_cb(IdeMarkdownView      *md,
                                        IdeMarkdownOpenFileCb open_file_cb) {
  if (!md) return;
  md->open_file_cb = open_file_cb;
}

void ide_markdown_view_set_context(IdeMarkdownView *md,
                                   const char      *doc_file_path,
                                   const char      *workspace_root) {
  if (!md) return;
  g_free(md->doc_file_path);
  g_free(md->doc_dir);
  g_free(md->workspace_root);

  md->doc_file_path = doc_file_path ? g_strdup(doc_file_path) : NULL;
  md->doc_dir = (doc_file_path && *doc_file_path)
                    ? g_path_get_dirname(doc_file_path)
                    : NULL;
  md->workspace_root = workspace_root ? g_strdup(workspace_root) : NULL;
}

IdeMarkdownView *ide_markdown_view_new(IdeMarkdownJumpCb       jump_cb,
                                       IdeMarkdownBufferEditCb edit_cb,
                                       gpointer                user_data) {
  IdeMarkdownView *md = g_new0(IdeMarkdownView, 1);
  md->jump_cb = jump_cb;
  md->edit_cb = edit_cb;
  md->user_data = user_data;

  md->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(md->root_box, "ide-markdown-pane");
  gtk_widget_set_hexpand(md->root_box, TRUE);
  gtk_widget_set_vexpand(md->root_box, TRUE);

  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(hdr, "ide-drawer-header");
  gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

  GtkWidget *title =
      gtk_label_new("📖 GRAPHICAL MARKDOWN, EMBEDS & VECTOR MERMAID");
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);

  md->task_badge = gtk_label_new("✓ 0 / 0 TASKS DONE");
  gtk_widget_add_css_class(md->task_badge, "shell-popover-badge");
  gtk_widget_add_css_class(md->task_badge, "success");
  gtk_widget_set_valign(md->task_badge, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(md->task_badge, FALSE);

  GtkWidget *sync_badge = gtk_label_new("SYNC SCROLL: ON");
  gtk_widget_add_css_class(sync_badge, "shell-popover-badge");
  gtk_widget_set_valign(sync_badge, GTK_ALIGN_CENTER);

  GtkWidget *badge = gtk_label_new("CAIRO + PANGO + POPPLER");
  gtk_widget_add_css_class(badge, "shell-popover-badge");
  gtk_widget_add_css_class(badge, "accent");
  gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

  gtk_box_append(GTK_BOX(hdr), title);
  gtk_box_append(GTK_BOX(hdr), md->task_badge);
  gtk_box_append(GTK_BOX(hdr), sync_badge);
  gtk_box_append(GTK_BOX(hdr), badge);
  gtk_box_append(GTK_BOX(md->root_box), hdr);

  md->scroll_win = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(md->scroll_win),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(md->scroll_win, TRUE);
  gtk_widget_set_vexpand(md->scroll_win, TRUE);

  md->doc_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(md->doc_box, 16);
  gtk_widget_set_margin_end(md->doc_box, 16);
  gtk_widget_set_margin_top(md->doc_box, 12);
  gtk_widget_set_margin_bottom(md->doc_box, 16);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(md->scroll_win), md->doc_box);
  gtk_box_append(GTK_BOX(md->root_box), md->scroll_win);

  return md;
}

void ide_markdown_view_free(IdeMarkdownView *md) {
  if (!md) return;
  g_free(md->current_markdown);
  g_free(md->doc_file_path);
  g_free(md->doc_dir);
  g_free(md->workspace_root);
  g_free(md);
}
