#ifndef KAJO_IDE_NATIVE_EXE_H
#define KAJO_IDE_NATIVE_EXE_H

/* Finds the executable built from a native (C / C++ / Rust) project: Cargo
 * target, Meson introspection, a same-named binary next to the source, or the
 * newest ELF in the usual build directories. NULL when nothing is built. */

#include <glib.h>

G_BEGIN_DECLS

char *ide_find_native_executable(const char *workspace_root, const char *active_file);

G_END_DECLS

#endif
