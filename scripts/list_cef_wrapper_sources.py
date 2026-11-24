#!/usr/bin/env python3
"""Emit Linux libcef_dll_wrapper .cc sources from /usr/src/cef/libcef_dll/CMakeLists.txt."""

import os
import re
import sys


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write("usage: list_cef_wrapper_sources.py <CMakeLists.txt>\n")
        return 2
    cmake_path = sys.argv[1]
    base_dir = os.path.dirname(os.path.abspath(cmake_path))
    with open(cmake_path, "r", encoding="utf-8") as f:
        text = f.read()

    seen: set[str] = set()
    out: list[str] = []
    block_re = re.compile(r"set\(\s*(LIBCEF_[A-Z0-9_]+)\s+(.*?)\)", re.DOTALL)
    for m in block_re.finditer(text):
        var_name = m.group(1)
        if var_name.endswith("_MAC") or var_name.endswith("_WINDOWS"):
            continue
        for token in m.group(2).split():
            if not token.endswith(".cc") and not token.endswith(".cpp"):
                continue
            full = os.path.join(base_dir, token)
            if os.path.isfile(full) and full not in seen:
                seen.add(full)
                out.append(full)

    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
