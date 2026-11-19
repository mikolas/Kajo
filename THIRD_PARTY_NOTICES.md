# Third-Party Notices

Kajo itself is MIT-licensed (see [LICENSE](LICENSE)). The repository also contains
the third-party works below, each under its own permissive licence. Their copyright
and permission notices are reproduced here as those licences require.

Libraries that Kajo links dynamically (for example `poppler-glib`, `libmpv`,
`libsidplayfp`, `uade`) are not part of this repository; see the License section of
the [README](README.md).

## Vendored and statically linked source code

| Component | Path | Licence | Notice |
| :--- | :--- | :--- | :--- |
| tomlc17 (TOML parser) | `src/vendor/tomlc17/` | MIT, Copyright (c) 2024-2026 CK Tan | `src/vendor/tomlc17/LICENSE` |
| tinyexpr (expression evaluator) | `src/vendor/tinyexpr/` | Zlib, Copyright (c) 2015-2020 Lewis Van Winkle | header of `tinyexpr.h` / `tinyexpr.c` (`SPDX-License-Identifier: Zlib`) |
| wlroots (Kajo's fork, linked statically into `kajo-comp`) | `subprojects/wlroots/` (git submodule of `https://github.com/mikolas/wlroots`) | MIT, Copyright (c) 2017, 2018 Drew DeVault, Copyright (c) 2014 Jari Vetoniemi, Copyright (c) 2023 The wlroots contributors | `subprojects/wlroots/LICENSE`, installed by `PKGBUILD` as `wlroots-LICENSE` |

## Wayland protocol definitions

`protocol/*.xml` are unmodified protocol descriptions. Each file keeps its own
`<copyright>` block with the copyright holders and the permission notice
(MIT-style):

- `input-method-unstable-v2.xml`, `virtual-keyboard-unstable-v1.xml`
- `wlr-layer-shell-unstable-v1.xml`, `wlr-output-power-management-unstable-v1.xml`,
  `wlr-screencopy-unstable-v1.xml`

## Icons (`data/icons/hicolor/scalable/apps/`)

| Prefix | Set | Licence |
| :--- | :--- | :--- |
| `tb-*` | [Tabler Icons](https://github.com/tabler/tabler-icons) | MIT, Copyright (c) 2020-2026 Paweł Kuna |
| `ld-*` | [Lucide](https://github.com/lucide-icons/lucide) (`lucide-static` v0.479.0) | ISC, Copyright (c) Lucide Icons and Contributors; some icons derive from Feather, MIT, Copyright (c) 2013-present Cole Bemis |
| `si-*` | [Simple Icons](https://github.com/simple-icons/simple-icons) | CC0 1.0 Universal (the brand logos themselves remain trademarks of their owners) |
| icons imported by `scripts/import_fluent_icons.py` | [Fluent UI System Icons](https://github.com/microsoft/fluentui-system-icons) | MIT, Copyright (c) 2020 Microsoft Corporation |

### MIT License (Tabler Icons, Feather, Fluent UI System Icons)

Permission is hereby granted, free of charge, to any person obtaining a copy
of these icons and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notices and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### ISC License (Lucide)

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
