#ifndef KAJO_IDE_TS_H
#define KAJO_IDE_TS_H

/* Tree-sitter syntax engine for kajo-ide.
 *
 * Pure (no GTK): parses UTF-8 text into a concrete syntax tree, re-parses
 * incrementally after edits and classifies leaf nodes into the 11 semantic
 * token classes shared with the heuristic lexer (ide_syntax.c). Languages
 * whose grammar library was not found at build time are simply unavailable;
 * callers fall back to the heuristic lexer. */

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  IDE_TS_LANG_NONE = 0,
  IDE_TS_LANG_C,
  IDE_TS_LANG_JAVA,
  IDE_TS_LANG_GO,
  IDE_TS_LANG_CPP,
  IDE_TS_LANG_RUST,
  IDE_TS_LANG_BASH,
  IDE_TS_LANG_JAVASCRIPT,
  IDE_TS_LANG_TYPESCRIPT,
  IDE_TS_LANG_TSX,
  IDE_TS_LANG_JSON,
  IDE_TS_LANG_TOML,
  IDE_TS_LANG_YAML,
  IDE_TS_LANG_CSS,
  IDE_TS_LANG_HTML,
  IDE_TS_LANG_LUA,
  IDE_TS_LANG_M68K,
  IDE_TS_LANG_COUNT
} IdeTsLang;

typedef enum {
  IDE_TS_TOKEN_NONE = 0,
  IDE_TS_TOKEN_KEYWORD,    /* storage / declaration keywords */
  IDE_TS_TOKEN_CONTROL,    /* if, for, return, defer, ... */
  IDE_TS_TOKEN_FUNCTION,   /* function / method names (declaration and call) */
  IDE_TS_TOKEN_TYPE,       /* types, classes, primitives */
  IDE_TS_TOKEN_MACRO,      /* constants, enum members, annotations, #define names */
  IDE_TS_TOKEN_VARIABLE,   /* variables, parameters, fields */
  IDE_TS_TOKEN_STRING,     /* string / char literal (escapes apply) */
  IDE_TS_TOKEN_STRING_RAW, /* raw string literal (no escapes) */
  IDE_TS_TOKEN_NUMBER,
  IDE_TS_TOKEN_COMMENT,
  IDE_TS_TOKEN_OPERATOR,
} IdeTsToken;

typedef struct _IdeTsDoc IdeTsDoc;

typedef void (*IdeTsTokenFunc)(gsize start_byte, gsize end_byte, IdeTsToken token, gpointer user_data);

/* Query-driven languages carry their highlight rules as tree-sitter queries
 * (ide_ts_queries.c). Returns the query source, or NULL for languages that use
 * the hand-written classifier (C, Java, Go). */
const char *ide_ts_query_source(IdeTsLang lang);

/* Compiles the highlight query of `lang`; returns TRUE when the language has no
 * query or it compiled cleanly. On failure *error_offset receives the byte
 * offset into the query source. Used by tests to keep the bundled queries
 * valid against the installed grammars. */
gboolean    ide_ts_query_check(IdeTsLang lang, guint32 *error_offset);

/* Maps a file path (or a bare language id such as "go") to a language that is
 * compiled in. Returns IDE_TS_LANG_NONE when unsupported or unavailable. */
IdeTsLang   ide_ts_lang_for_path(const char *path_or_id);
gboolean    ide_ts_lang_available(IdeTsLang lang);
const char *ide_ts_lang_name(IdeTsLang lang);

IdeTsDoc   *ide_ts_doc_new(IdeTsLang lang);
void        ide_ts_doc_free(IdeTsDoc *doc);
IdeTsLang   ide_ts_doc_get_lang(const IdeTsDoc *doc);

/* Parses `text`. When the document already holds a tree the edit is derived
 * from the common prefix/suffix of old and new text and the parse is
 * incremental. *dirty_start / *dirty_end receive the byte range whose tokens
 * may have changed (the whole text on the first parse; empty when unchanged). */
gboolean    ide_ts_doc_update(IdeTsDoc   *doc,
                              const char *text,
                              gsize       len,
                              gsize      *dirty_start,
                              gsize      *dirty_end);

/* Emits the tokens overlapping [from, to) in source order. */
void        ide_ts_doc_walk(const IdeTsDoc *doc,
                            gsize           from,
                            gsize           to,
                            IdeTsTokenFunc  cb,
                            gpointer        user_data);

/* Syntax-tree text objects for `m i/a`: target 'f' function, 'c' class / struct /
 * interface / enum, 'b' brace block, 'a' argument / parameter / list element.
 * Selects the innermost enclosing object around `cursor_byte`; `inside` excludes
 * braces and signatures (body only; for 'a', the element without its comma).
 * Returns FALSE when nothing encloses the cursor, so callers can fall back to
 * the text-based objects. */
gboolean    ide_ts_doc_find_object(const IdeTsDoc *doc,
                                   gsize           cursor_byte,
                                   gboolean        inside,
                                   char            target,
                                   gsize          *out_start,
                                   gsize          *out_end);

/* True when the tree contains syntax errors (diagnostic for tests / UI). */
gboolean    ide_ts_doc_has_error(const IdeTsDoc *doc);

G_END_DECLS

#endif /* KAJO_IDE_TS_H */
