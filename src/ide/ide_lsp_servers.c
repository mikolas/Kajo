#include "ide_lsp_servers.h"

#include <json-glib/json-glib.h>
#include <string.h>

static const char *const kNoArgs[] = { NULL };
static const char *const kStdioArg[] = { "--stdio", NULL };
static const char *const kBashArgs[] = { "start", NULL };
static const char *const kClangdArgs[] = { "--background-index", "--header-insertion=never", NULL };
static const char *const kTaploArgs[] = { "lsp", "stdio", NULL };
static const char *const kMesonArgs[] = { "--lsp", NULL };

static const char *const kGoMarkers[] = { "go.work", "go.mod", NULL };
static const char *const kJavaMarkers[] = { "pom.xml", "settings.gradle", "settings.gradle.kts",
                                            "build.gradle", "build.gradle.kts", ".project",
                                            "mvnw", "gradlew", NULL };
static const char *const kCMarkers[] = { "compile_commands.json", "compile_flags.txt", ".clangd", NULL };
static const char *const kRustMarkers[] = { "Cargo.toml", NULL };
static const char *const kPyMarkers[] = { "pyproject.toml", "setup.py", "setup.cfg", "requirements.txt", NULL };
static const char *const kJsMarkers[] = { "tsconfig.json", "jsconfig.json", "package.json", NULL };
static const char *const kMesonMarkers[] = { "meson.build", NULL };
static const char *const kM68kMarkers[] = { ".m68krc.json", ".m68k-format.json", "Makefile", NULL };
static const char *const kNoMarkers[] = { NULL };

static const IdeLspServerSpec kGopls = { "gopls", "gopls", kNoArgs, kGoMarkers };
static const IdeLspServerSpec kJdtls = { "jdtls", "jdtls", kNoArgs, kJavaMarkers };
static const IdeLspServerSpec kClangd = { "clangd", "clangd", kClangdArgs, kCMarkers };
static const IdeLspServerSpec kRustAnalyzer = { "rust-analyzer", "rust-analyzer", kNoArgs, kRustMarkers };
static const IdeLspServerSpec kPyright = { "pyright", "pyright-langserver", kStdioArg, kPyMarkers };
static const IdeLspServerSpec kBashLs = { "bash-ls", "bash-language-server", kBashArgs, kNoMarkers };
static const IdeLspServerSpec kTsLs = { "typescript-ls", "typescript-language-server", kStdioArg, kJsMarkers };
static const IdeLspServerSpec kJsonLs = { "json-ls", "vscode-json-language-server", kStdioArg, kNoMarkers };
static const IdeLspServerSpec kYamlLs = { "yaml-ls", "yaml-language-server", kStdioArg, kNoMarkers };
static const IdeLspServerSpec kTaplo = { "taplo", "taplo", kTaploArgs, kNoMarkers };
static const IdeLspServerSpec kLuaLs = { "lua-ls", "lua-language-server", kNoArgs, kNoMarkers };
static const IdeLspServerSpec kMesonLs = { "mesonlsp", "mesonlsp", kMesonArgs, kMesonMarkers };
static const IdeLspServerSpec kM68kLs = { "m68k-lsp", "m68k-lsp-server", kStdioArg, kM68kMarkers };
/* Test hook: KAJO_LSP_MOCK_CMD names a command line serving *.kajo-mock files. */
static const IdeLspServerSpec kMock = { "mock", "mock", kNoArgs, kNoMarkers };

typedef struct {
  const char             *suffix;       /* extension or base name */
  const char             *language_id;
  const IdeLspServerSpec *spec;
} FileRule;

static const FileRule kRules[] = {
  { ".go", "go", &kGopls },
  { ".java", "java", &kJdtls },
  { ".c", "c", &kClangd },
  { ".h", "c", &kClangd },
  { ".cpp", "cpp", &kClangd },
  { ".cc", "cpp", &kClangd },
  { ".cxx", "cpp", &kClangd },
  { ".hpp", "cpp", &kClangd },
  { ".hh", "cpp", &kClangd },
  { ".rs", "rust", &kRustAnalyzer },
  { ".py", "python", &kPyright },
  { ".sh", "shellscript", &kBashLs },
  { ".bash", "shellscript", &kBashLs },
  { ".ts", "typescript", &kTsLs },
  { ".tsx", "typescriptreact", &kTsLs },
  { ".js", "javascript", &kTsLs },
  { ".jsx", "javascriptreact", &kTsLs },
  { ".mjs", "javascript", &kTsLs },
  { ".json", "json", &kJsonLs },
  { ".yaml", "yaml", &kYamlLs },
  { ".yml", "yaml", &kYamlLs },
  { ".toml", "toml", &kTaplo },
  { ".lua", "lua", &kLuaLs },
  { ".s", "m68k", &kM68kLs },
  { ".S", "m68k", &kM68kLs },
  { ".asm", "m68k", &kM68kLs },
  { ".ASM", "m68k", &kM68kLs },
  { ".i", "m68k", &kM68kLs },
  { ".I", "m68k", &kM68kLs },
  { "meson.build", "meson", &kMesonLs },
  { ".kajo-mock", "mock", &kMock },
};

/* `.s` / `.S` / `.asm` / `.i` are shared with GNU, NASM and other tools, so the m68k rules
 * only apply inside a project that opts in: an ancestor directory holding `.m68krc.json` or
 * `.m68k-format.json`, or KAJO_M68K_ASM=1. INTERIM: a proper per-project language setting
 * should replace this marker probe. */
static gboolean
is_m68k_project_file(const char *file_path)
{
  const char *force = g_getenv("KAJO_M68K_ASM");
  if (force != NULL && g_strcmp0(force, "1") == 0) {
    return TRUE;
  }
  g_autofree char *dir = g_path_get_dirname(file_path);
  while (dir != NULL && dir[0] != '\0') {
    g_autofree char *a = g_build_filename(dir, ".m68krc.json", NULL);
    g_autofree char *b = g_build_filename(dir, ".m68k-format.json", NULL);
    if (g_file_test(a, G_FILE_TEST_IS_REGULAR) || g_file_test(b, G_FILE_TEST_IS_REGULAR)) {
      return TRUE;
    }
    g_autofree char *parent = g_path_get_dirname(dir);
    if (g_strcmp0(parent, dir) == 0) {
      break;
    }
    g_free(dir);
    dir = g_steal_pointer(&parent);
  }
  return FALSE;
}

static const FileRule *
rule_for(const char *file_path)
{
  if (file_path == NULL || file_path[0] == '\0') {
    return NULL;
  }
  for (gsize i = 0; i < G_N_ELEMENTS(kRules); i++) {
    if (g_str_has_suffix(file_path, kRules[i].suffix)) {
      if (kRules[i].spec == &kMock && g_getenv("KAJO_LSP_MOCK_CMD") == NULL) {
        continue;
      }
      if (kRules[i].spec == &kM68kLs && !is_m68k_project_file(file_path)) {
        continue;
      }
      return &kRules[i];
    }
  }
  return NULL;
}

const IdeLspServerSpec *
ide_lsp_server_spec_for_file(const char *file_path)
{
  const FileRule *r = rule_for(file_path);
  return r != NULL ? r->spec : NULL;
}

/* A header is C++ when C++ sources live beside it (`.h` is ambiguous). */
static gboolean
dir_has_cxx_sources(const char *file_path)
{
  g_autofree char *dir = g_path_get_dirname(file_path);
  g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
  const char *name;
  while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
    if (g_str_has_suffix(name, ".cpp") || g_str_has_suffix(name, ".cc") || g_str_has_suffix(name, ".cxx") ||
        g_str_has_suffix(name, ".hpp") || g_str_has_suffix(name, ".hh")) {
      return TRUE;
    }
  }
  return FALSE;
}

const char *
ide_lsp_language_id_for_file(const char *file_path)
{
  const FileRule *r = rule_for(file_path);
  if (r == NULL) {
    return "plaintext";
  }
  if (g_str_has_suffix(file_path, ".h") && dir_has_cxx_sources(file_path)) {
    return "cpp";
  }
  return r->language_id;
}

char *
ide_lsp_fallback_pattern(const char *file_path, const char *symbol, gboolean definition)
{
  g_autofree char *sym = g_regex_escape_string(symbol != NULL ? symbol : "", -1);
  if (g_strcmp0(ide_lsp_language_id_for_file(file_path), "m68k") == 0) {
    /* label ("name:" / "name::"), assignment ("name = x") or directive ("name equ x") */
    return g_strdup_printf("^\\s*%s(?::{1,2}|\\s*=|\\s+(?:equ|fequ|macro|set|reg|equr)\\b)", sym);
  }
  if (definition) {
    return g_strdup_printf("(?:^|\\s)(?:func\\s+|def\\s+|struct\\s+|#define\\s+)?%s\\s*[\\(\\{=]", sym);
  }
  return g_strdup_printf("\\b%s\\s*[\\(;\\{]", sym);
}

const char *const *
ide_lsp_fallback_globs(const char *file_path)
{
  static const char *const kNone[] = { NULL };
  static const char *const kM68k[] = {
    "--glob=*.s", "--glob=*.S", "--glob=*.asm", "--glob=*.ASM", "--glob=*.i", "--glob=*.I", NULL
  };
  return g_strcmp0(ide_lsp_language_id_for_file(file_path), "m68k") == 0 ? kM68k : kNone;
}

char *
ide_lsp_find_root(const IdeLspServerSpec *spec, const char *file_path, const char *workspace_root)
{
  if (spec == NULL || spec->root_markers == NULL || spec->root_markers[0] == NULL ||
      file_path == NULL) {
    return g_strdup(workspace_root ? workspace_root : ".");
  }

  g_autofree char *dir = g_path_get_dirname(file_path);
  while (dir != NULL && dir[0] != '\0') {
    for (gsize i = 0; spec->root_markers[i] != NULL; i++) {
      g_autofree char *probe = g_build_filename(dir, spec->root_markers[i], NULL);
      if (g_file_test(probe, G_FILE_TEST_EXISTS)) {
        return g_strdup(dir);
      }
    }
    /* Never climb above the workspace root. */
    if (workspace_root != NULL && g_strcmp0(dir, workspace_root) == 0) {
      break;
    }
    g_autofree char *parent = g_path_get_dirname(dir);
    if (g_strcmp0(parent, dir) == 0) {
      break;
    }
    g_free(dir);
    dir = g_steal_pointer(&parent);
  }
  return g_strdup(workspace_root ? workspace_root : ".");
}

/* Microsoft's java-debug bundle (a jdtls plugin that serves DAP). Looked up in
 * $KAJO_JAVA_DEBUG_JAR, ~/.local/share/kajo/java-debug and a Mason install. */
char *
ide_lsp_find_java_debug_jar(void)
{
  const char *forced = g_getenv("KAJO_JAVA_DEBUG_JAR");
  if (forced != NULL && forced[0] != '\0') {
    return g_file_test(forced, G_FILE_TEST_EXISTS) ? g_strdup(forced) : NULL;
  }
  g_autofree char *kajo_dir = g_build_filename(g_get_user_data_dir(), "kajo", "java-debug", NULL);
  g_autofree char *mason = g_build_filename(g_get_user_data_dir(), "nvim", "mason", "packages",
                                            "java-debug-adapter", "extension", "server", NULL);
  const char *dirs[] = { kajo_dir, mason, NULL };
  for (gsize i = 0; dirs[i] != NULL; i++) {
    g_autoptr(GDir) d = g_dir_open(dirs[i], 0, NULL);
    const char *name;
    char *best = NULL;
    while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
      if (g_str_has_prefix(name, "com.microsoft.java.debug.plugin") && g_str_has_suffix(name, ".jar")) {
        char *p = g_build_filename(dirs[i], name, NULL);
        if (best == NULL || g_strcmp0(p, best) > 0) {
          g_free(best);
          best = p;
        } else {
          g_free(p);
        }
      }
    }
    if (best != NULL) {
      return best;
    }
  }
  return NULL;
}

JsonNode *
ide_lsp_server_init_options(const IdeLspServerSpec *spec)
{
  if (spec == NULL || g_strcmp0(spec->server_id, "jdtls") != 0) {
    return NULL;
  }
  g_autofree char *jar = ide_lsp_find_java_debug_jar();
  if (jar == NULL) {
    return NULL;
  }
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "bundles");
  json_builder_begin_array(b);
  json_builder_add_string_value(b, jar);
  json_builder_end_array(b);
  json_builder_end_object(b);
  return json_builder_get_root(b);
}

/* Major version of the JDK rooted at `home` (from its `release` file), or 0. */
static guint
jdk_major(const char *home)
{
  g_autofree char *release = g_build_filename(home, "release", NULL);
  g_autofree char *contents = NULL;
  if (!g_file_get_contents(release, &contents, NULL, NULL)) {
    return 0;
  }
  const char *v = strstr(contents, "JAVA_VERSION=\"");
  if (v == NULL) {
    return 0;
  }
  v += strlen("JAVA_VERSION=\"");
  guint major = (guint)g_ascii_strtoull(v, NULL, 10);
  if (major == 1) { /* legacy 1.8 style */
    const char *dot = strchr(v, '.');
    major = dot ? (guint)g_ascii_strtoull(dot + 1, NULL, 10) : 1;
  }
  return major;
}

/* jdtls tracks the newest JDK (its OSGi bundles require a recent runtime), while
 * the default `java` on PATH is often an older LTS. Pick the newest installed
 * JDK; KAJO_JDTLS_JAVA (path to a java executable) overrides the search. */
char *
ide_lsp_find_jdtls_java(void)
{
  const char *forced = g_getenv("KAJO_JDTLS_JAVA");
  if (forced != NULL && forced[0] != '\0') {
    return g_strdup(forced);
  }

  g_autoptr(GPtrArray) roots = g_ptr_array_new_with_free_func(g_free);
  const char *jh = g_getenv("JAVA_HOME");
  if (jh != NULL && jh[0] != '\0') {
    g_ptr_array_add(roots, g_strdup(jh));
  }
  const char *scan_dirs[2] = { NULL, "/usr/lib/jvm" };
  g_autofree char *sdk = g_build_filename(g_get_home_dir(), ".sdkman", "candidates", "java", NULL);
  scan_dirs[0] = sdk;
  for (gsize i = 0; i < G_N_ELEMENTS(scan_dirs); i++) {
    g_autoptr(GDir) dir = g_dir_open(scan_dirs[i], 0, NULL);
    const char *name;
    while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
      g_ptr_array_add(roots, g_build_filename(scan_dirs[i], name, NULL));
    }
  }

  char *best = NULL;
  guint best_major = 0;
  for (guint i = 0; i < roots->len; i++) {
    const char *home = g_ptr_array_index(roots, i);
    g_autofree char *java = g_build_filename(home, "bin", "java", NULL);
    if (!g_file_test(java, G_FILE_TEST_IS_EXECUTABLE)) {
      continue;
    }
    guint major = jdk_major(home);
    if (major > best_major) {
      best_major = major;
      g_free(best);
      best = g_steal_pointer(&java);
    }
  }
  return best; /* NULL: let jdtls use whatever `java` is on PATH */
}

char **
ide_lsp_build_argv(const IdeLspServerSpec *spec, const char *root)
{
  g_return_val_if_fail(spec != NULL, NULL);

  if (spec == &kMock) {
    char **argv = NULL;
    const char *cmd = g_getenv("KAJO_LSP_MOCK_CMD");
    if (cmd != NULL && g_shell_parse_argv(cmd, NULL, &argv, NULL)) {
      return argv;
    }
    return NULL;
  }

  g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(argv, g_strdup(spec->binary));
  for (gsize i = 0; spec->args[i] != NULL; i++) {
    g_ptr_array_add(argv, g_strdup(spec->args[i]));
  }

  if (spec == &kJdtls) {
    /* jdtls needs a per-project data directory outside the source tree. */
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, root ? root : "", -1);
    g_autofree char *data = g_build_filename(g_get_user_cache_dir(), "kajo", "jdtls", hash, NULL);
    g_mkdir_with_parents(data, 0700);
    g_ptr_array_add(argv, g_strdup("-data"));
    g_ptr_array_add(argv, g_steal_pointer(&data));
    char *java = ide_lsp_find_jdtls_java();
    if (java != NULL) {
      g_ptr_array_add(argv, g_strdup("--java-executable"));
      g_ptr_array_add(argv, java);
    }
  } else if (spec == &kClangd && root != NULL) {
    static const char *const kBuildDirs[] = { "builddir", "build", "_build", "out", NULL };
    for (gsize i = 0; kBuildDirs[i] != NULL; i++) {
      g_autofree char *cc = g_build_filename(root, kBuildDirs[i], "compile_commands.json", NULL);
      if (g_file_test(cc, G_FILE_TEST_EXISTS)) {
        g_ptr_array_add(argv, g_strdup_printf("--compile-commands-dir=%s/%s", root, kBuildDirs[i]));
        break;
      }
    }
  }

  g_ptr_array_add(argv, NULL);
  return (char **)g_ptr_array_free(g_steal_pointer(&argv), FALSE);
}
