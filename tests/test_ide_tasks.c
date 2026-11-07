#include "../src/ide/ide_tasks.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static char *
make_ws(void)
{
  return g_dir_make_tmp("kajo-tasks-XXXXXX", NULL);
}

static void
touch(const char *dir, const char *name, const char *contents)
{
  g_autofree char *p = g_build_filename(dir, name, NULL);
  g_autofree char *parent = g_path_get_dirname(p);
  g_mkdir_with_parents(parent, 0755);
  g_file_set_contents(p, contents, -1, NULL);
}

static void
test_maven_java(void)
{
  g_autofree char *ws = make_ws();
  touch(ws, "pom.xml", "<project/>");
  touch(ws, "src/main/java/demo/App.java", "package demo;\n\npublic class App { public static void main(String[] a) {} }\n");
  g_autofree char *file = g_build_filename(ws, "src/main/java/demo/App.java", NULL);

  IdeTask *b = ide_task_resolve(ws, file, IDE_TASK_BUILD);
  g_assert_nonnull(b);
  g_assert_cmpstr(b->system, ==, "Maven");
  g_assert_cmpstr(b->command, ==, "mvn -B -ntp compile");
  g_assert_cmpstr(b->cwd, ==, ws);
  ide_task_free(b);

  IdeTask *t = ide_task_resolve(ws, file, IDE_TASK_TEST);
  g_assert_cmpstr(t->command, ==, "mvn -B -ntp test");
  ide_task_free(t);

  IdeTask *r = ide_task_resolve(ws, file, IDE_TASK_RUN);
  g_assert_nonnull(r);
  g_assert_cmpstr(r->command, ==, "mvn -B -ntp -q compile && java -cp target/classes 'demo.App'");
  ide_task_free(r);

  /* the wrapper wins when present and executable */
  touch(ws, "mvnw", "#!/bin/sh\n");
  g_autofree char *mvnw = g_build_filename(ws, "mvnw", NULL);
  g_chmod(mvnw, 0755);
  IdeTask *w = ide_task_resolve(ws, file, IDE_TASK_BUILD);
  g_assert_cmpstr(w->command, ==, "./mvnw -B -ntp compile");
  ide_task_free(w);
}

static void
test_gradle_and_nested_project(void)
{
  g_autofree char *ws = make_ws();
  touch(ws, "services/api/build.gradle.kts", "");
  touch(ws, "services/api/src/main/java/com/acme/Main.java", "package com.acme;\nclass Main {}\n");
  g_autofree char *file = g_build_filename(ws, "services/api/src/main/java/com/acme/Main.java", NULL);
  g_autofree char *api = g_build_filename(ws, "services/api", NULL);

  IdeTask *t = ide_task_resolve(ws, file, IDE_TASK_TEST);
  g_assert_cmpstr(t->system, ==, "Gradle");
  g_assert_cmpstr(t->cwd, ==, api); /* nearest build file, not the workspace root */
  g_assert_cmpstr(t->command, ==, "gradle --console=plain test");
  ide_task_free(t);

  IdeTask *r = ide_task_resolve(ws, file, IDE_TASK_RUN);
  g_assert_cmpstr(r->command, ==, "gradle --console=plain -q classes && java -cp build/classes/java/main 'com.acme.Main'");
  ide_task_free(r);
}

static void
test_native_and_go_projects(void)
{
  g_autofree char *ws = make_ws();
  touch(ws, "meson.build", "project('x','c')\n");
  touch(ws, "src/a.c", "int main(void){return 0;}\n");
  g_autofree char *c = g_build_filename(ws, "src/a.c", NULL);
  IdeTask *t = ide_task_resolve(ws, c, IDE_TASK_TEST);
  g_assert_cmpstr(t->system, ==, "Meson");
  g_assert_nonnull(strstr(t->command, "meson test -C builddir"));
  ide_task_free(t);
  /* nothing built yet: RUN degrades to a build with a hint */
  IdeTask *r0 = ide_task_resolve(ws, c, IDE_TASK_RUN);
  g_assert_nonnull(r0);
  g_assert_nonnull(strstr(r0->label, "no executable built yet"));
  ide_task_free(r0);
  /* with a built executable (ELF) next to the source, RUN builds then runs it */
  touch(ws, "builddir/app", "\x7f" "ELF....");
  g_autofree char *app = g_build_filename(ws, "builddir/app", NULL);
  g_chmod(app, 0755);
  IdeTask *r1 = ide_task_resolve(ws, c, IDE_TASK_RUN);
  g_assert_nonnull(strstr(r1->command, "meson compile -C builddir && "));
  g_assert_nonnull(strstr(r1->command, "builddir/app"));
  ide_task_free(r1);

  g_autofree char *cm = make_ws();
  touch(cm, "CMakeLists.txt", "project(x)\n");
  touch(cm, "main.cpp", "int main(){}\n");
  g_autofree char *cpp = g_build_filename(cm, "main.cpp", NULL);
  IdeTask *cb = ide_task_resolve(cm, cpp, IDE_TASK_BUILD);
  g_assert_cmpstr(cb->system, ==, "CMake");
  g_assert_nonnull(strstr(cb->command, "cmake --build build"));
  g_assert_nonnull(strstr(cb->command, "CMAKE_EXPORT_COMPILE_COMMANDS"));
  ide_task_free(cb);

  g_autofree char *go = make_ws();
  touch(go, "go.mod", "module m\n");
  touch(go, "cmd/tool/main.go", "package main\nfunc main(){}\n");
  g_autofree char *gf = g_build_filename(go, "cmd/tool/main.go", NULL);
  IdeTask *gr = ide_task_resolve(go, gf, IDE_TASK_RUN);
  g_assert_cmpstr(gr->command, ==, "go run './cmd/tool'");
  ide_task_free(gr);
  IdeTask *gt = ide_task_resolve(go, gf, IDE_TASK_TEST);
  g_assert_cmpstr(gt->command, ==, "go test ./...");
  ide_task_free(gt);
}

static void
test_single_java_file_and_none(void)
{
  g_autofree char *ws = make_ws();
  touch(ws, "Hello.java", "public class Hello { public static void main(String[] a){} }\n");
  g_autofree char *file = g_build_filename(ws, "Hello.java", NULL);
  IdeTask *r = ide_task_resolve(ws, file, IDE_TASK_RUN);
  g_assert_cmpstr(r->system, ==, "Java");
  g_assert_nonnull(strstr(r->command, "java "));
  g_assert_nonnull(strstr(r->command, "Hello.java"));
  ide_task_free(r);
  IdeTask *b = ide_task_resolve(ws, file, IDE_TASK_BUILD);
  g_assert_true(g_str_has_prefix(b->command, "javac -d "));
  ide_task_free(b);

  g_autofree char *empty = make_ws();
  touch(empty, "notes.txt", "x");
  g_autofree char *nf = g_build_filename(empty, "notes.txt", NULL);
  g_assert_null(ide_task_resolve(empty, nf, IDE_TASK_BUILD));

  g_autofree char *cls = ide_java_main_class(file);
  g_assert_cmpstr(cls, ==, "Hello");
  g_assert_null(ide_java_main_class("/tmp/readme.md"));
}

static void
test_quoting_of_unusual_paths(void)
{
  g_autofree char *ws = make_ws();
  touch(ws, "my dir/Hello World.java", "public class Hello {}\n");
  g_autofree char *file = g_build_filename(ws, "my dir/Hello World.java", NULL);
  IdeTask *r = ide_task_resolve(ws, file, IDE_TASK_BUILD);
  g_assert_nonnull(r);
  /* a path with a space must be a single shell word */
  g_assert_nonnull(strstr(r->command, "'"));
  g_auto(GStrv) argv = NULL;
  g_assert_true(g_shell_parse_argv(r->command, NULL, &argv, NULL));
  g_assert_cmpstr(argv[0], ==, "javac");
  g_assert_cmpstr(argv[argv ? g_strv_length(argv) - 1 : 0], ==, file);
  ide_task_free(r);
}

static void
test_m68k_build_requires_vasm_and_uses_cache_dir(void)
{
  g_autofree char *ws = make_ws();
  touch(ws, "main.asm", "main:\n  rts\n");
  g_autofree char *file = g_build_filename(ws, "main.asm", NULL);

  /* No assembler on PATH: no task is offered (x86/ARM .asm projects are not hijacked). */
  g_autofree char *old_path = g_strdup(g_getenv("PATH"));
  g_setenv("PATH", ws, TRUE);
  g_assert_null(ide_task_resolve(ws, file, IDE_TASK_BUILD));

  /* With vasm installed the task is offered, builds only, and avoids the shared temp dir. */
  g_autofree char *fake = g_build_filename(ws, "vasmm68k_mot", NULL);
  g_assert_true(g_file_set_contents(fake, "#!/bin/sh\nexit 0\n", -1, NULL));
  g_assert_cmpint(g_chmod(fake, 0755), ==, 0);
  IdeTask *t = ide_task_resolve(ws, file, IDE_TASK_BUILD);
  g_assert_nonnull(t);
  g_assert_nonnull(strstr(t->command, "vasmm68k_mot"));
  g_assert_nonnull(strstr(t->command, g_get_user_cache_dir()));
  g_assert_null(strstr(t->command, "kajo-m68k.o"));
  g_assert_nonnull(strstr(t->command, "/kajo/m68k-out.hunk"));
  ide_task_free(t);
  g_assert_null(ide_task_resolve(ws, file, IDE_TASK_RUN));
  g_assert_null(ide_task_resolve(ws, file, IDE_TASK_CLEAN));

  g_setenv("PATH", old_path != NULL ? old_path : "/usr/bin:/bin", TRUE);
  g_unlink(fake);
  g_unlink(file);
  g_rmdir(ws);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ide/tasks/maven-java", test_maven_java);
  g_test_add_func("/ide/tasks/gradle-nested", test_gradle_and_nested_project);
  g_test_add_func("/ide/tasks/native-and-go", test_native_and_go_projects);
  g_test_add_func("/ide/tasks/single-java-and-none", test_single_java_file_and_none);
  g_test_add_func("/ide/tasks/quoting", test_quoting_of_unusual_paths);
  g_test_add_func("/ide/tasks/m68k-vasm-gated", test_m68k_build_requires_vasm_and_uses_cache_dir);
  return g_test_run();
}
