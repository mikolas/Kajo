#ifndef KAJO_IDE_LSP_SERVERS_H
#define KAJO_IDE_LSP_SERVERS_H

/* Language-server registry (GTK-free): which server handles which file, how to
 * find its project root and how to launch it. */

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct {
  const char        *server_id;     /* "gopls" */
  const char        *binary;        /* executable looked up on PATH */
  const char *const *args;          /* argv after the binary (NULL-terminated) */
  const char *const *root_markers;  /* files/dirs that mark a project root */
} IdeLspServerSpec;

/* Returns the spec for `file_path` (by extension / base name) or NULL. */
const IdeLspServerSpec *ide_lsp_server_spec_for_file(const char *file_path);

/* LSP languageId for a file ("go", "java", "cpp", "typescriptreact", ...). */
const char *ide_lsp_language_id_for_file(const char *file_path);

/* Nearest ancestor of `file_path` (bounded by `workspace_root` and its parents'
 * markers) that contains one of the spec's root markers; falls back to
 * `workspace_root`. */
char *ide_lsp_find_root(const IdeLspServerSpec *spec,
                        const char             *file_path,
                        const char             *workspace_root);

/* Path of the java-debug jdtls bundle, or NULL when not installed. */
char *ide_lsp_find_java_debug_jar(void);

/* initializationOptions for the server (jdtls: {"bundles":[java-debug jar]}),
 * or NULL. */
JsonNode *ide_lsp_server_init_options(const IdeLspServerSpec *spec);

/* JDK used to run jdtls: KAJO_JDTLS_JAVA, else the newest JDK found in JAVA_HOME,
 * ~/.sdkman and /usr/lib/jvm. NULL when none is found (jdtls then uses PATH). */
char *ide_lsp_find_jdtls_java(void);

/* Full argv for launching the server for `root` (NULL-terminated, g_strfreev). */
char **ide_lsp_build_argv(const IdeLspServerSpec *spec, const char *root);

G_END_DECLS

#endif
