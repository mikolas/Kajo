#!/usr/bin/env bash
# Build Kajo with AddressSanitizer + UBSan in a throwaway directory and run the
# full meson test suite. Usage: scripts/asan-test.sh [builddir]
#
# Leak reports that originate in fontconfig / pango / GTK global caches are
# library noise; detect_leaks is therefore off by default. Export
# ASAN_OPTIONS=detect_leaks=1 to see them anyway.
set -euo pipefail

src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-/tmp/kajo-asan}"

if [ ! -d "$build_dir" ]; then
    meson setup "$build_dir" "$src_dir" -Db_sanitize=address,undefined -Db_lundef=false
fi

meson compile -C "$build_dir"

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"
export G_SLICE=always-malloc
export G_DEBUG=gc-friendly

meson test -C "$build_dir" --print-errorlogs
