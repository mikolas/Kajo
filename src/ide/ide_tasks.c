#include "ide_tasks.h"
#include "ide_native_exe.h"

#include <string.h>

static gboolean
exists_in(const char *dir, const char *name)
{
  g_autofree char *p = g_build_filename(dir, name, NULL);
  return g_file_test(p, G_FILE_TEST_EXISTS);
}

static gboolean
executable_in(const char *dir, const char *name)
{
  g_autofree char *p = g_build_filename(dir, name, NULL);
  return g_file_test(p, G_FILE_TEST_IS_EXECUTABLE);
}

typedef enum {
  SYS_NONE,
  SYS_MAVEN,
  SYS_GRADLE,
  SYS_MESON,
  SYS_CMAKE,
  SYS_CARGO,
  SYS_GO,
  SYS_MAKE,
} BuildSystem;

/* Which build system a directory is the root of, given the language of the file
 * being edited (a directory can hold several, e.g. Meson + a Maven helper). */
static BuildSystem
system_in_dir(const char *dir, const char *file)
{
  gboolean java = file && g_str_has_suffix(file, ".java");
  gboolean go = file && g_str_has_suffix(file, ".go");
  gboolean rust = file && g_str_has_suffix(file, ".rs");

  gboolean maven = exists_in(dir, "pom.xml");
  gboolean gradle = exists_in(dir, "build.gradle") || exists_in(dir, "build.gradle.kts") ||
                    exists_in(dir, "settings.gradle") || exists_in(dir, "settings.gradle.kts");
  gboolean meson = exists_in(dir, "meson.build");
  gboolean cmake = exists_in(dir, "CMakeLists.txt");
  gboolean cargo = exists_in(dir, "Cargo.toml");
  gboolean gomod = exists_in(dir, "go.mod");
  gboolean make = exists_in(dir, "Makefile") || exists_in(dir, "makefile") || exists_in(dir, "GNUmakefile");

  if (java) {
    if (maven) return SYS_MAVEN;
    if (gradle) return SYS_GRADLE;
  } else if (go && gomod) {
    return SYS_GO;
  } else if (rust && cargo) {
    return SYS_CARGO;
  }
  if (maven) return SYS_MAVEN;
  if (gradle) return SYS_GRADLE;
  if (meson) return SYS_MESON;
  if (cmake) return SYS_CMAKE;
  if (cargo) return SYS_CARGO;
  if (gomod) return SYS_GO;
  if (make) return SYS_MAKE;
  return SYS_NONE;
}

char *
ide_java_main_class(const char *java_path)
{
  if (java_path == NULL || !g_str_has_suffix(java_path, ".java")) {
    return NULL;
  }
  g_autofree char *base = g_path_get_basename(java_path);
  base[strlen(base) - strlen(".java")] = '\0';

  g_autofree char *contents = NULL;
  if (g_file_get_contents(java_path, &contents, NULL, NULL)) {
    g_autoptr(GRegex) rx = g_regex_new("^\\s*package\\s+([\\w.]+)\\s*;", G_REGEX_MULTILINE, 0, NULL);
    g_autoptr(GMatchInfo) mi = NULL;
    if (rx != NULL && g_regex_match(rx, contents, 0, &mi)) {
      g_autofree char *pkg = g_match_info_fetch(mi, 1);
      return g_strdup_printf("%s.%s", pkg, base);
    }
  }
  return g_strdup(base);
}

/* Nearest ancestor of `file` (bounded by `ws`) that is the root of a build system. */
static char *
find_project_dir(const char *ws, const char *file, BuildSystem *sys_out)
{
  g_autofree char *dir = file ? g_path_get_dirname(file) : g_strdup(ws);
  while (dir != NULL && dir[0] != '\0') {
    BuildSystem sys = system_in_dir(dir, file);
    if (sys != SYS_NONE) {
      *sys_out = sys;
      return g_strdup(dir);
    }
    if (ws != NULL && g_strcmp0(dir, ws) == 0) {
      break;
    }
    g_autofree char *parent = g_path_get_dirname(dir);
    if (g_strcmp0(parent, dir) == 0) {
      break;
    }
    g_free(dir);
    dir = g_steal_pointer(&parent);
  }
  *sys_out = SYS_NONE;
  return g_strdup(ws ? ws : ".");
}

static IdeTask *
task_new(const char *system, const char *what, const char *cwd, char *command)
{
  IdeTask *t = g_new0(IdeTask, 1);
  t->system = g_strdup(system);
  t->label = g_strdup_printf("%s: %s", system, what);
  t->cwd = g_strdup(cwd);
  t->command = command;
  return t;
}

/* Per-user scratch path ($XDG_CACHE_HOME/kajo/<name>); never a predictable name in the
 * shared, world-writable temp directory (symlink clobbering, cross-user collisions). */
static char *
task_scratch_path(const char *name)
{
  g_autofree char *dir = g_build_filename(g_get_user_cache_dir(), "kajo", NULL);
  g_mkdir_with_parents(dir, 0700);
  return g_build_filename(dir, name, NULL);
}

void
ide_task_free(IdeTask *t)
{
  if (t != NULL) {
    g_free(t->system);
    g_free(t->label);
    g_free(t->command);
    g_free(t->cwd);
    g_free(t);
  }
}

/* java -cp <classes> <main> after the project's own compile step. */
static char *
java_run_command(const char *compile, const char *classes_dir, const char *file)
{
  g_autofree char *cls = ide_java_main_class(file);
  if (cls == NULL) {
    return NULL;
  }
  g_autofree char *q = g_shell_quote(cls);
  return g_strdup_printf("%s && java -cp %s %s", compile, classes_dir, q);
}

IdeTask *
ide_task_resolve(const char *workspace_root, const char *active_file, IdeTaskKind kind)
{
  BuildSystem sys;
  g_autofree char *dir = find_project_dir(workspace_root, active_file, &sys);

  const char *what[] = { "build", "test", "run", "clean" };

  switch (sys) {
    case SYS_MAVEN: {
      const char *mvn = executable_in(dir, "mvnw") ? "./mvnw" : "mvn";
      g_autofree char *base = g_strdup_printf("%s -B -ntp", mvn);
      g_autofree char *compile = g_strdup_printf("%s -q compile", base);
      switch (kind) {
        case IDE_TASK_BUILD: return task_new("Maven", what[kind], dir, g_strdup_printf("%s compile", base));
        case IDE_TASK_TEST:  return task_new("Maven", what[kind], dir, g_strdup_printf("%s test", base));
        case IDE_TASK_CLEAN: return task_new("Maven", what[kind], dir, g_strdup_printf("%s clean", base));
        case IDE_TASK_RUN: {
          char *cmd = java_run_command(compile, "target/classes", active_file);
          return cmd ? task_new("Maven", what[kind], dir, cmd) : NULL;
        }
      }
      break;
    }
    case SYS_GRADLE: {
      const char *gr = executable_in(dir, "gradlew") ? "./gradlew" : "gradle";
      g_autofree char *base = g_strdup_printf("%s --console=plain", gr);
      g_autofree char *compile = g_strdup_printf("%s -q classes", base);
      switch (kind) {
        case IDE_TASK_BUILD: return task_new("Gradle", what[kind], dir, g_strdup_printf("%s classes", base));
        case IDE_TASK_TEST:  return task_new("Gradle", what[kind], dir, g_strdup_printf("%s test", base));
        case IDE_TASK_CLEAN: return task_new("Gradle", what[kind], dir, g_strdup_printf("%s clean", base));
        case IDE_TASK_RUN: {
          char *cmd = java_run_command(compile, "build/classes/java/main", active_file);
          return cmd ? task_new("Gradle", what[kind], dir, cmd) : NULL;
        }
      }
      break;
    }
    case SYS_MESON: {
      const char *bd = "builddir";
      g_autofree char *setup = g_strdup_printf("[ -d %s ] || meson setup %s", bd, bd);
      switch (kind) {
        case IDE_TASK_BUILD: return task_new("Meson", what[kind], dir, g_strdup_printf("%s && meson compile -C %s", setup, bd));
        case IDE_TASK_TEST:  return task_new("Meson", what[kind], dir, g_strdup_printf("%s && meson compile -C %s && meson test -C %s --print-errorlogs", setup, bd, bd));
        case IDE_TASK_CLEAN: return task_new("Meson", what[kind], dir, g_strdup_printf("meson compile -C %s --clean", bd));
        case IDE_TASK_RUN: {
          g_autofree char *exe = ide_find_native_executable(dir, active_file);
          if (exe == NULL) {
            return task_new("Meson", "build (no executable built yet; run again)", dir,
                            g_strdup_printf("%s && meson compile -C %s", setup, bd));
          }
          g_autofree char *q = g_shell_quote(exe);
          return task_new("Meson", what[kind], dir, g_strdup_printf("%s && meson compile -C %s && %s", setup, bd, q));
        }
      }
      break;
    }
    case SYS_CMAKE: {
      g_autofree char *cfg = g_strdup("[ -f build/CMakeCache.txt ] || cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
      switch (kind) {
        case IDE_TASK_BUILD: return task_new("CMake", what[kind], dir, g_strdup_printf("%s && cmake --build build", cfg));
        case IDE_TASK_TEST:  return task_new("CMake", what[kind], dir, g_strdup_printf("%s && cmake --build build && ctest --test-dir build --output-on-failure", cfg));
        case IDE_TASK_CLEAN: return task_new("CMake", what[kind], dir, g_strdup("cmake --build build --target clean"));
        case IDE_TASK_RUN: {
          g_autofree char *exe = ide_find_native_executable(dir, active_file);
          if (exe == NULL) {
            return task_new("CMake", "build (no executable built yet; run again)", dir,
                            g_strdup_printf("%s && cmake --build build", cfg));
          }
          g_autofree char *q = g_shell_quote(exe);
          return task_new("CMake", what[kind], dir, g_strdup_printf("%s && cmake --build build && %s", cfg, q));
        }
      }
      break;
    }
    case SYS_CARGO:
      switch (kind) {
        case IDE_TASK_BUILD: return task_new("Cargo", what[kind], dir, g_strdup("cargo build --color never"));
        case IDE_TASK_TEST:  return task_new("Cargo", what[kind], dir, g_strdup("cargo test --color never"));
        case IDE_TASK_RUN:   return task_new("Cargo", what[kind], dir, g_strdup("cargo run --color never"));
        case IDE_TASK_CLEAN: return task_new("Cargo", what[kind], dir, g_strdup("cargo clean"));
      }
      break;
    case SYS_GO:
      switch (kind) {
        case IDE_TASK_BUILD: return task_new("Go", what[kind], dir, g_strdup("go build ./..."));
        case IDE_TASK_TEST:  return task_new("Go", what[kind], dir, g_strdup("go test ./..."));
        case IDE_TASK_RUN: {
          /* the package of the active file */
          if (active_file != NULL && g_str_has_suffix(active_file, ".go")) {
            g_autofree char *pkg_dir = g_path_get_dirname(active_file);
            g_autofree char *rel = g_strcmp0(pkg_dir, dir) == 0 ? g_strdup(".")
                                   : g_strdup_printf("./%s", pkg_dir + strlen(dir) + 1);
            g_autofree char *q = g_shell_quote(rel);
            return task_new("Go", what[kind], dir, g_strdup_printf("go run %s", q));
          }
          return task_new("Go", what[kind], dir, g_strdup("go run ."));
        }
        case IDE_TASK_CLEAN: return task_new("Go", what[kind], dir, g_strdup("go clean ./..."));
      }
      break;
    case SYS_MAKE:
      switch (kind) {
        case IDE_TASK_BUILD: return task_new("Make", what[kind], dir, g_strdup("make"));
        case IDE_TASK_TEST:  return task_new("Make", what[kind], dir, g_strdup("make test"));
        case IDE_TASK_RUN: {
          g_autofree char *exe = ide_find_native_executable(dir, active_file);
          if (exe == NULL) {
            return task_new("Make", what[kind], dir, g_strdup("make run"));
          }
          g_autofree char *q = g_shell_quote(exe);
          return task_new("Make", what[kind], dir, g_strdup_printf("make && %s", q));
        }
        case IDE_TASK_CLEAN: return task_new("Make", what[kind], dir, g_strdup("make clean"));
      }
      break;
    case SYS_NONE:
      break;
  }

  /* No build system: a lone .java file can still be compiled / run directly. */
  if (active_file != NULL && g_str_has_suffix(active_file, ".java")) {
    g_autofree char *q = g_shell_quote(active_file);
    g_autofree char *out = task_scratch_path("java-out");
    g_autofree char *qo = g_shell_quote(out);
    g_autofree char *cwd = g_path_get_dirname(active_file);
    switch (kind) {
      case IDE_TASK_BUILD: return task_new("Java", what[kind], cwd, g_strdup_printf("javac -d %s %s", qo, q));
      case IDE_TASK_RUN:   return task_new("Java", what[kind], cwd, g_strdup_printf("java %s", q));
      default:             return NULL;
    }
  }
  /* Motorola 68000 (Amiga) assembly. Only offered when the vasm assembler is installed:
   * .s/.S/.asm are also GNU/NASM sources for other CPUs, and silently running an m68k
   * assembler on those would just produce confusing errors. The output is an Amiga hunk
   * executable, written to the per-user cache. */
  if (active_file != NULL && kind == IDE_TASK_BUILD &&
      (g_str_has_suffix(active_file, ".s") || g_str_has_suffix(active_file, ".S") ||
       g_str_has_suffix(active_file, ".asm") || g_str_has_suffix(active_file, ".ASM"))) {
    g_autofree char *vasm = g_find_program_in_path("vasmm68k_mot");
    if (vasm == NULL) {
      return NULL;
    }
    g_autofree char *q = g_shell_quote(active_file);
    g_autofree char *out = task_scratch_path("m68k-out.hunk");
    g_autofree char *qo = g_shell_quote(out);
    g_autofree char *cwd = g_path_get_dirname(active_file);
    return task_new("vasm", what[kind], cwd,
                    g_strdup_printf("vasmm68k_mot -Fhunkexe -kick1hunks -nosym -no-opt -o %s %s", qo, q));
  }
  return NULL;
}
