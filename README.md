<p align="center">
  <img src="docs/kajo-banner.svg" alt="KAJO // Personal Native Application Suite &amp; Wayland Environment" width="100%">
</p>

# Kajo — Native Desktop Suite & Wayland Environment

**Kajo** is a self-contained suite of eleven lightweight C17/C++20 applications, a custom scrollable-tiling Wayland compositor (**`kajo-comp`**), and Wayland session components—designed around a sharp `0px` dark **Aero + Metro** aesthetic ([`docs/design_system.md`](docs/design_system.md)), Nordic ergonomics, and first-class support for **`kajo-comp`** (`kajo.desktop`) and [Niri](https://github.com/niri-wm/niri) (`kajo-niri.desktop`).

- **Unified Design:** Zero corner radius, dark `#111111`–`#1a1a1a` surfaces, `1px` live `@metro-accent` window borders, and `Fira Code` telemetry headers.
- **Native Footprint:** Direct C17/GTK4/wlroots-Vulkan/libmpv/Chromium CEF binaries with popout/map-gated polling (`<0.05%` idle shell CPU).

---

## 🧩 Suite Highlights & CLI Commands

### `kajo-comp` (`org.kajo.comp`) — Wayland Compositor
Scrollable-tiling Wayland compositor (`wlroots-0.20` + Vulkan `kajo-0.20.2-fx11`) with analytical springs, tabbed columns, 3-finger touch/touchpad strip drags, Vulkan SDF shadows, 7 glass backdrop shaders, window transitions, 10-bit PQ HDR10 (`off`/`auto`/`on`), `Mod+O` zoomed overview, `Alt+Tab` live switcher, and screenshot UI ([Spec](docs/spec_kajo_comp.md) • **[Full CLI & IPC Reference](docs/kajo_comp_cli.md)**).

```bash
kajo-comp [--session] [--greeter-mode] [-c|--config PATH] [--mod alt|super] \
          [--exit-with-command] [-v|--verbose] [-q|--quiet] [-- COMMAND [ARGS...]]

# Runtime IPC via $XDG_RUNTIME_DIR/kajo-comp.sock ($KAJO_COMP_SOCKET):
kajo-comp msg get-outputs | get-workspaces | get-windows | get-keyboard-layouts | debug-mem
kajo-comp msg subscribe [workspaces windows keyboard_layouts outputs touch_gestures screenshots]
kajo-comp msg action <ACTION> [KEY=VALUE ...]   # 59 actions; see docs/kajo_comp_cli.md
```

### `kajo` (`org.kajo.shell`) — Desktop Shell & Settings Portal
Wayland panel with 22 widgets, `Mod+Space` multi-plugin launcher, 30 FPS popout FFT spectrum visualizer, SNI tray, notification daemon, `ShellOSK` virtual keyboard, bezel touch gestures, foldable posture manager, `org.freedesktop.impl.portal.Settings`, screensaver, and PAM session lock.

```bash
kajo                    # Start the shell daemon
kajo -t | --toggle-launcher
kajo --volume-up | --volume-down | --volume-mute
kajo --brightness-up | --brightness-down
kajo --lock | --screensaver
kajo --toggle-osk | --show-osk | --hide-osk
kajo --reload-config | --reload-theme
# Also accepts bare IPC names without "--" (e.g. `kajo launcher-toggle`, `kajo volume-up`).
```

### `kajo-ide` (`org.kajo.IDE`) — Modal Code Editor & IDE
Selection-first GTK4 + `GtkSourceView-5` modal IDE with Nordic bindings (`ä`/`ö` diagnostics, `m i/a` text objects), 6-column Cairo gutter, 16-language Tree-sitter highlighting, async LSP + DAP debugger, `Space t s` Call-Depth Code Strip & Dependency Lenses, native Cairo Mermaid diagrams, 120-command `?` palette, and AI Agent JSON-RPC IPC ([Manual](docs/kajo_ide_manual.md) • [Code Strip Spec](docs/spec_ide_code_strip.md)).

```bash
kajo-ide [DIR | FILES...]
```

### `kajo-amiga` (`org.kajo.Amiga`) — Amiga M68k/C IDE & Hardware Debugger
Standalone Amiga 500/1200 C & Motorola 68000 IDE and hardware debugger built on shared `kajo-ide` editor/LSP cores (`clangd` + `m68k-lsp-server`), supporting relocatable `hunk_exe` and fixed-address `raw_bin` targets, `vasm`/`vlink`/`m68k-amigaos-gcc`/`vbcc`/external `make` builds, `adflib` OFS/FFS/raw bootable ADF creation, Amiberry 8.5+ GDB RSP debugging, live Bitplane/Copper/Chips/DMA/Memory visualizers, and offline HRM register reference ([Spec](docs/spec_kajo_amiga.md) • [Toolchain Guide](docs/kajo_amiga_toolchain.md)).

```bash
kajo-amiga [PROJECT_DIR]
kajo-amiga --attach [HOST:PORT]            # Attach directly to Amiberry RSP (default 127.0.0.1:2345)
kajo-amiga --build [TARGET [PROJECT_DIR]]  # Headless CLI build
kajo-amiga --preview-mode project|setup|attach|wizard|bitplanes|copper|chips|dma|hrm
```

### `kajo-files` (`org.kajo.Files`) — File Manager & FileChooser Portal
URI-first GIO/GVFS/UDisks2 browser with virtual `archive://` folders, ISO loop-mounting, live `network:///` discovery, Wayland Drag-and-Drop, `Space` Quick Look (images, `libmpv`, Poppler PDF, archives, code), and D-Bus `org.freedesktop.impl.portal.FileChooser`.

```bash
kajo-files [PATH_OR_URI...]                # Local path or trash:/// recent:/// network:/// smb://
kajo-files --portal                        # Run D-Bus FileChooser portal service
kajo-files --chooser-preview [DIR]         # Preview FileChooser dialog standalone
```

### `kajo-player` (`org.kajo.Player`) — Hardware Video & Audio Player
Hardware-accelerated `libmpv` + `GtkGLArea` player with subtitle/audio stream selectors, external `.srt`/`.ass` loading, sync delay, chapters, sibling folder queue, fullscreen HUD, and MPRIS2 integration.

```bash
kajo-player [FILE_OR_URI...]
```

### `kajo-mod` (`org.kajo.Mod`) — Retro Amiga, SID & Tracker Player
5-tier retro audio router (`libuade`, `libsidplayfp`/reSIDfp + HVSC, `libgme`, `libopenmpt`, `libxmp`) with Paula/SID filter switches, `$D400`/sample inspector, `.lha`/`.zip` extraction, and 60 FPS oscilloscope + 16-band FFT ([Spec](docs/spec_kajo_mod.md)).

```bash
kajo-mod [--column-width|--col-width <PX>] [FILES | ARCHIVES | DIRS...]
```

### `kajo-web` (`org.kajo.Web`) — Chromium/CEF Wayland Browser
Alloy Views CEF browser on native Wayland + VA-API with `autohide`/`compact`/`pinned` vertical tab rail, isolated multi-profiles + startup picker, shared-profile Web Apps, IO-thread ABP/cosmetic ad blocker, Downloads popout, idle tab suspension, and SQLite FTS5 storage ([Spec](docs/spec_kajo_web.md)).

```bash
kajo-web [--profile <ID>] [--app-id <ID> | --app <URL>] [-p|--private] \
         [--no-hw-accel] [--no-adblock-sync] [--] [URL...]
```

### `kajo-viewer` (`org.kajo.Viewer`) — Read-Only PDF, Markdown & Image Viewer
Strictly read-only viewer for continuous `1P`/`2P SPREAD` PDFs (`PAPER`/`SEPIA`/`DARK`, outline, `Ctrl+F`, encrypted unlock), live-reloaded Markdown + Cairo Mermaid (`RAW SOURCE` toggle), and `glycin`/`librsvg`/`libexif` pan-zoom images + filmstrip ([Spec](docs/spec_kajo_viewer.md)).

```bash
kajo-viewer [--preview-mode pdf|md|image] [FILE_OR_URI...]
```

### `kajo-settings` (`org.kajo.Settings`) — 14-Page Control Deck
Modular settings for displays (HDR/VRR/posture), NetworkManager Wi-Fi, BlueZ Bluetooth, PipeWire audio, Metro themes (+ revertible GTK/Qt external theming), panel/widgets, notifications, `kajo-comp`/Niri effects & shaders, IDE, Mod, Web, Amiga, and system telemetry.

```bash
kajo-settings [-p|--page <PAGE>] [-w|--column-width <PX>]
# Pages: displays, network, bluetooth, audio, theme, panel, widgets,
#        notifications, niri, ide, mod, web, amiga, about
```

### `kajo-greeter` & `kajo-session` — Login Greeter & Session Launcher
Hardened `greetd` Wayland greeter (`explicit_bzero`, `PR_SET_DUMPABLE=0`) and multi-profile session wrapper.

```bash
kajo-greeter [-c|--cmd <CMD>] [-u|--user <USER>] [-t|--test] [-f|--fullscreen]
kajo-session [--compositor kajo-comp|niri] [--profile release|debug|asan] \
             [--config-dir <PATH>] [--nested] [--resolution <WxH>] [-- COMMAND...]
```

---

## 📚 Documentation Index

| Document | Contents |
| :--- | :--- |
| **[`docs/kajo_comp_cli.md`](docs/kajo_comp_cli.md)** | Complete `kajo-comp` CLI, `msg` IPC subcommands, all 59 actions, subscribe topics, and `kajo-session` options. |
| **[`docs/design_system.md`](docs/design_system.md)** & **[`docs/ui-gallery.html`](docs/ui-gallery.html)** | Aero + Metro visual spec and interactive GTK4/HTML component gallery. |
| **[`docs/brand-logo-gallery.html`](docs/brand-logo-gallery.html)** | Vector application icon gallery across all 10 Kajo binaries. |
| **[`docs/spec_kajo_comp.md`](docs/spec_kajo_comp.md)** | `kajo-comp` architecture (scrolling strip, springs, Wayland protocols, TOML, IPC). |
| **[`docs/spec_comp_effects.md`](docs/spec_comp_effects.md)** & **[`spec_comp_hdr.md`](docs/spec_comp_hdr.md)** | Vulkan visual effects, 7 glass materials ([styles](docs/spec_comp_glass_styles.md)), transitions, `wlroots` fork ([spec](docs/spec_wlroots_fork.md)), HDR10 PQ pipeline, and [storyboard](docs/kajo-comp-storyboard.html). |
| **[`docs/spec_toml_config.md`](docs/spec_toml_config.md)** | Per-domain TOML 1.1 config store and `conf.d/` overlay specification. |
| **[`docs/kajo_ide_manual.md`](docs/kajo_ide_manual.md)** & **[`spec_ide_code_strip.md`](docs/spec_ide_code_strip.md)** | `kajo-ide` manual, Call-Depth Code Strip spec, [mockup](docs/kajo-ide-strip-mockup.html), and [storyboard](docs/kajo-ide-strip-storyboard.html). |
| **[`docs/spec_kajo_amiga.md`](docs/spec_kajo_amiga.md)** & **[`docs/kajo_amiga_toolchain.md`](docs/kajo_amiga_toolchain.md)** | `kajo-amiga` IDE/RSP architecture, target artifacts, `seeandseedee` workflow, toolchain guide, and [mockup](docs/kajo-amiga-mockup.html). |
| **[`docs/spec_kajo_mod.md`](docs/spec_kajo_mod.md)** / **[`spec_kajo_web.md`](docs/spec_kajo_web.md)** / **[`spec_kajo_viewer.md`](docs/spec_kajo_viewer.md)** | App specs (plus interactive mockups/storyboards under `docs/`). |
| **[`docs/script_widgets.md`](docs/script_widgets.md)** | Custom panel script widget guide for `kajo`. |
| **[`DEVELOPMENT.md`](DEVELOPMENT.md)** / **[`ROADMAP.md`](ROADMAP.md)** / **[`HARDWARE_FINDINGS.md`](HARDWARE_FINDINGS.md)** | Build/test handbook, roadmap, and 2-in-1 sensor/EC register notes. |

---

## 🛠️ Building & Installing (Arch Linux / CachyOS)

```bash
sudo pacman -S --needed gtk4 gtksourceview5 gtk4-layer-shell wayland wayland-protocols \
    talloc pcre2 libinput libxkbcommon cairo pango libpng vulkan-icd-loader \
    libdrm seatd libdisplay-info libliftoff lcms2 pixman mesa vulkan-headers glslang hwdata \
    libxcb xcb-util-wm xcb-util-renderutil xcb-util-errors \
    xorg-xwayland json-glib glib2 libpulse pam poppler-glib libarchive mpv sqlite \
    libgcrypt libsidplayfp libgme libopenmpt libxmp gvfs gvfs-smb udisks2 \
    blueprint-compiler meson ninja cmake \
    tree-sitter tree-sitter-c tree-sitter-bash tree-sitter-javascript tree-sitter-lua tree-sitter-rust
paru -S --needed uade bencodetools tree-sitter-cpp tree-sitter-css tree-sitter-go \
    tree-sitter-html tree-sitter-java tree-sitter-json tree-sitter-toml tree-sitter-typescript \
    tree-sitter-yaml tree-sitter-m68k
```

- **Optional dependencies:** Install CEF Linux x86\_64 under `/usr/{lib,include,src}/cef` (or `-Dcef_lib_dir=... -Dcef_include_dir=... -Dcef_src_dir=...`) for `kajo-web`; `glycin-2`/`glycin-gtk4-2`/`librsvg`/`libexif` for full `kajo-viewer` image support; `elfutils` (`libelf`/`libdw`) for `kajo-amiga` DWARF3 symbol loading; `ripgrep`/`clang`/`gopls`/`rust-analyzer`/`pyright`/`gdb`/`lldb`/`delve` (plus `m68k-lsp-server`/`vasmm68k_mot`/`vlink`/`m68k-amigaos-gcc`/`amiberry`) for `kajo-ide` and `kajo-amiga`. Missing optional Tree-sitter grammars fall back to the built-in lexer. (`adflib` is built via `cmake` from `subprojects/adflib.wrap` if not installed system-wide.)

```bash
git clone --recursive https://github.com/mikolas/Kajo.git
meson setup builddir
meson compile -C builddir
meson test -C builddir
meson install -C builddir --skip-subprojects   # installs into ~/.local (or configured --prefix)
```

If upgrading in a live session so portal updates take effect without logging out:
```bash
systemctl --user daemon-reload && systemctl --user reload dbus-broker && systemctl --user restart xdg-desktop-portal
```

---

## 🖥️ Sessions & Configuration Paths

Installed Wayland sessions (`data/session/`): **`kajo.desktop`** (`kajo-session`), **`kajo-debug.desktop`** (`kajo-session --profile debug`), and **`kajo-niri.desktop`** (`niri-session`). Defaults live in `data/defaults/` (installed to `<datadir>/kajo/defaults/`).

| Path | Managed By | Purpose |
| :--- | :--- | :--- |
| `~/.config/kajo/{comp,outputs,binds,rules}.toml` | `kajo-settings` / User | `kajo-comp` layout, animations, effects, outputs, keybinds, and window/layer rules. |
| `~/.config/kajo/{theme,shell,ide,files,mod,web,amiga}.toml` | `kajo-settings` / User | Suite domain configs (`theme`, `shell`, `ide`, `files`, `mod`, `web`, `amiga`). |
| `~/.config/kajo/conf.d/*.toml` | User / Dotfiles | Lexicographical read-only drop-in overrides merged after base TOML files. |
| `~/.config/kajo/style.d/00-accent.css` & `theme-override.css` | `kajo-settings` (`theme`) | Live `@metro-*` CSS tokens hot-reloaded across all Kajo processes. |
| `~/.config/kajo/external-theme-backup.json` | `kajo-settings` (`theme`) | Bit-exact snapshot for opt-in external GTK/Qt app theme revert. |
| `~/.config/niri/config.kdl` | `kajo-settings` | Output/shader config when using `kajo-niri.desktop`. |
| `/var/cache/kajo-greeter/state.json` | `kajo-greeter` | Last logged-in user and session. |

---

## 📄 License & Author

- **Icons:** [Fluent UI System Icons](https://github.com/microsoft/fluentui-system-icons) (Microsoft Corporation, MIT).
- **Vendored code:** [`tomlc17`](src/vendor/tomlc17/LICENSE) (MIT) and [`tinyexpr`](src/vendor/tinyexpr/tinyexpr.h) (zlib).
- **License:** MIT — see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Distributed as source only; dynamically links system libraries (including copyleft libraries such as `poppler-glib`, `libmpv`, `libsidplayfp`, `uade`).

Mikolas Hämäläinen — [mikolas@mikolas.net](mailto:mikolas@mikolas.net)
