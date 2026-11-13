<p align="center">
  <img src="docs/kajo-banner.svg" alt="KAJO // Personal Native Application Suite &amp; Wayland Environment" width="100%">
</p>

# Kajo — Personal Native Application Suite & Wayland Environment

**Kajo** is a self-contained suite of lightweight, native C17 / GTK4 applications, a custom scrollable-tiling Wayland compositor (**`kajo-comp`**), and Wayland session components—built around a single, uncompromising visual and ergonomic vision with first-class support for both **`kajo-comp`** (`kajo.desktop`) and [Niri](https://github.com/niri-wm/niri) (`kajo-niri.desktop`).

---

## 🎯 Purpose & Philosophy

Kajo has evolved far beyond its origins as a simple Wayland panel or launcher. **Its purpose is not to be a generic, catch-all replacement for mainstream Linux desktop environments or mass-market applications.** Instead, Kajo is a **purpose-built, cohesive suite of tools engineered specifically for a personal workflow, Nordic keyboard ergonomics, and hardware setup**:

1. **A Unified "Aero + Metro" Design Language** ([`docs/design_system.md`](docs/design_system.md) • [`docs/ui-gallery.html`](docs/ui-gallery.html))
   - Rejects bloated, rounded toolkit defaults in favor of **strict `0px` sharp corner geometry**, OLED-native dark charcoal surfaces (`#111111`–`#1a1a1a`), `1px solid #2b2b2b` structural hairlines, live `@metro-accent` window framing, and high-density `Fira Code` monospace telemetry badges.
   - Every standalone application in the suite shares the exact same custom frameless window chrome, scrollable-tiling / floating compositor IPC (`🗗 FLOAT / TILE` across `kajo-comp` and Niri), and instant live CSS/accent hot-reload.
2. **Pure Native C17/C++20 Performance, Zero Runtime Bloat**
   - Built directly on `C17` (`C++20` for `libsidplayfp`), `wlroots-0.20`, `talloc`, `libinput`, `libpcre2-8`, `GTK4`, `GtkSourceView-5`, `Cairo`, `Pango`, `libpng`, `libmpv`, `libuade`, `libsidplayfp`, `libgme`, `libopenmpt`, `libxmp`, `libpulse`, `poppler-glib`, and `libarchive`—with zero Electron, CEF, or web-view wrappers.
   - Strict map- and popout-gated resource discipline keeps background CPU usage below `0.05%`.
3. **Tailored to Personal Workflows**
   - Whether driving the infinite horizontal tiling strip in **`kajo-comp`**, writing C/Go/Rust/Python/M68k in **`kajo-ide`** with Nordic modal keybindings (`ä`/`ö` diagnostic stepping, `m i/a` structural text objects, native Cairo Mermaid diagrams, and AI agent JSON-RPC diff review), inspecting archives/PDFs/videos with `Space` Quick Look in **`kajo-files`**, watching hardware-decoded media in **`kajo-player`**, listening to cycle-exact Amiga 68000 / C64 SID / tracker modules in **`kajo-mod`**, or configuring multi-monitor outputs, tiling physics, Niri GLSL shaders, and 2-in-1 foldable sensors in **`kajo-settings`**, every tool is optimized to do exactly what is needed—nothing more, nothing less.

---

## 🧩 The Kajo Application Suite

The repository builds and installs **eight integrated native executables** (plus `kajo-session` and `kajo-greeter-session` wrappers):

| Executable | App ID | Role in the Suite |
| :--- | :--- | :--- |
| **`kajo-comp`** | `org.kajo.comp` | **Scrollable-Tiling Wayland Compositor** (`wlroots-0.20` + Vulkan) with an infinite horizontal strip, analytical spring physics, tabbed/stacked columns, live-preview `Alt+Tab`, and in-compositor screenshots. |
| **`kajo-ide`** | `org.kajo.IDE` | **Modal Code Editor & IDE** (`GtkSourceView-5`) with selection-first Nordic keybindings, 16-language Tree-sitter highlighting, async LSP & DAP debugger, Cairo Mermaid diagrams, and AI Agent IPC. |
| **`kajo-files`** | `org.kajo.Files` | **URI-First File Manager** (`GIO` / `GVFS` / `UDisks2`) with virtual `archive://` browsing, ISO/disk-image mounting, batch clipboard operations, and `Space` Quick Look preview. |
| **`kajo-player`** | `org.kajo.Player` | **Hardware-Accelerated Media Player** (`libmpv` + `GtkGLArea`) with VA-API/NVDEC decoding, subtitle/audio track selectors, chapter queue, and MPRIS2 integration. |
| **`kajo-mod`** | `org.kajo.Mod` | **Retro Amiga, C64 SID, Chiptune & Tracker Player** (`libuade`, `libsidplayfp`, `libgme`, `libopenmpt`, `libxmp`) with hardware filter controls, HVSC lookup, and 60 FPS scope/FFT. |
| **`kajo-settings`** | `org.kajo.Settings` | **12-Page System & Suite Control Deck** managing displays, Wi-Fi, Bluetooth, PipeWire audio, Metro themes, panel/widgets, compositor layout, IDE/Mod preferences, and hardware telemetry. |
| **`kajo`** | `org.kajo.shell` | **Wayland Desktop Shell & Hardware Daemon** providing the status panel (22 widgets), app launcher, OSDs, virtual touch keyboard (`ShellOSK`), notifications, screen saver, and PAM session lock. |
| **`kajo-greeter`** | `org.kajo.greeter` | **Wayland Login Greeter for `greetd`** supporting `kajo-comp --greeter-mode`, `niri`, `cage`, and desktop `--test` preview mode. |

---

## ✨ Suite Components in Detail

### ❖ Kajo Compositor (`kajo-comp`)
> 📖 **Full Specification & Storyboard**: [`docs/spec_kajo_comp.md`](docs/spec_kajo_comp.md) • [`docs/kajo-comp-storyboard.html`](docs/kajo-comp-storyboard.html)

- **Infinite Scrolling Strip & Spring Physics**: Per-output dynamic workspaces with variable-width columns (`1/3`, `1/2`, `2/3`), vertical window stacking, tabbed columns (`24px` `Fira Code` tab headers), and a closed-form 2nd-order damped spring solver. Resizes animate, and workspaces form one continuous vertical strip (both stay visible while switching).
- **3-Finger Drag & Flick**: On the touchscreen and touchpad the view follows three fingers along the strip or between workspaces and snaps on release: a slow drag switches by position, a quick flick by velocity, and a short drag springs back (a glance).
- **Vulkan / Pixman Scene Graph (`wlroots-0.20`)**: `0px` corner geometry with `1px` live `@metro-accent` active borders, DRM atomic test-before-commit cascade, `vrr = "on-demand"`, and automatic `VK_ERROR_DEVICE_LOST` GPU reset recovery.
- **Built-In Switcher & Screenshot UI**: Live-thumbnail `Alt+Tab` MRU overlay and interactive frozen-screen `Region` / `Screen` / `Window` screenshot capture (`Print`) to PNG + Wayland clipboard.
- **TOML Configuration & IPC**: Configured via `~/.config/kajo/{comp,outputs,binds,rules}.toml` (+ `conf.d/*.toml`) with live hot-reload, PCRE2 window/layer rules, `--greeter-mode` lockdown, and `$XDG_RUNTIME_DIR/kajo-comp.sock` JSON IPC.

### ⌨️ Kajo IDE (`kajo-ide`)
> 📖 **Full Manual**: [`docs/kajo_ide_manual.md`](docs/kajo_ide_manual.md)

- **Selection-First Modal Editing & Nordic Ergonomics**: 10-state modal engine with Nordic bindings (`ä`/`ö` diagnostics, `Alt+ä`/`Alt+ö` Git hunks, `m i/a` structural text objects) and a 6-column `68px` Cairo gutter (breakpoints, diagnostics, Git diff, agent bars, folds).
- **Tree-Sitter, LSP & DAP Debugger**: Incremental 16-language Tree-sitter highlighting, async JSON-RPC 2.0 LSP client (`clangd`, `gopls`, `rust-analyzer`, `jdtls`, `pyright`, …), project build/test runner (`Space r b/t/r/c`), and integrated DAP debugger (`gdb`, `lldb-dap`, `dlv`, Java via `jdtls`).
- **Markdown, Vector Mermaid & Fuzzy Picker**: Side-by-side Markdown preview with interactive task checkboxes, native Cairo/Pango Mermaid diagram renderer with `.svg` export, `ripgrep` project search (`Space f g`), and a 102-command Help Palette (`?`).
- **AI Agent IPC & Diff Review**: Exposes `$KAJO_IDE_SOCKET` so external agents can query diagnostics and apply multi-file edits with atomic snapshot review (`Space d s/a/r`).

### 📁 Kajo Files (`kajo-files`)
- **URI-First Navigation & Breadcrumbs**: Non-blocking GIO/GVFS enumeration (`trash:///`, `recent:///`, `smb://`, `sftp://`, UDisks2 drives, inline LUKS/SMB auth) with auto-scrolling root-to-leaf breadcrumbs and right-click subfolder jump popovers.
- **Seamless Archives & Disk Images**: Browse `.zip`, `.tar.*`, `.7z`, `.rar`, and `.iso` files directly as read-only `archive://` folders, loop-mount disk images via UDisks2, or run background `libarchive` extraction and compression (`Ctrl+Shift+A`).
- **`Space` Quick Look & Batch Operations**: Instant modal previews for images, hardware-decoded video/audio (`libmpv`), PDFs (`poppler-glib`), archives, and code, alongside rubber-band multi-selection, natural filename sorting, and Wayland clipboard transfers.

### 🎬 Kajo Player (`kajo-player`)
- **Hardware-Accelerated `libmpv` + `GtkGLArea`**: OpenGL/EGL rendering with VA-API/NVDEC decoding, live header codec/resolution badges, and a toggleable technical stats overlay (`I` / `Ctrl+J`).
- **Subtitles, Audio & Transport**: Embedded/external subtitle and audio track selectors, A/V & subtitle sync offsets, pitch-corrected speed (`0.5×`–`2.0×`), single-frame stepping, A–B loops, sibling folder queue drawer, auto-hiding fullscreen HUD, and MPRIS2 export.

### 🎹 Kajo Mod (`kajo-mod`)
> 📖 **Full Specification**: [`docs/spec_kajo_mod.md`](docs/spec_kajo_mod.md)

- **5-Tier Retro Audio Router**: Priority dispatch across **`libuade`** (cycle-exact Amiga 68000 + Paula BLEP for 150+ formats), **`libsidplayfp`/`reSIDfp`** (C64 6581/8580 SID + HVSC `Songlengths.md5` & `STIL.txt`), **`libgme`** (console chiptunes), **`libopenmpt`**, and **`libxmp`**.
- **Hardware DSP & 60 FPS Telemetry**: Live Paula `A500`/`A1200` & LED filter switches, SID chip selector, stereo crossfeed, 4-voice Mute/Solo, `$D400` SID register / sample inspector, `.lha`/`.zip` bundle extraction, and a 60 FPS stereo oscilloscope + 16-band FFT analyzer.

### ⚙️ Kajo Settings (`kajo-settings`)
- **12 Live System & Suite Pages**: Modular control deck (`displays`, `network`, `bluetooth`, `audio`, `theme`, `panel`, `widgets`, `notifications`, `ide`, `mod`, `niri`, `about`) with responsive 3-tier column reflow and zero mock data.
- **Dual-Compositor & Live Theme Sync**: Synchronizes display modes/scale/VRR and tiling layout settings across both `kajo-comp` (`comp.toml` / `outputs.toml`) and Niri (`config.kdl`), and hot-reloads `@metro-accent` palettes across all running Kajo apps and compositor borders.

### 🖥️ Kajo Desktop Shell (`kajo`) & Greeter (`kajo-greeter`)
- **Panel, Launcher & Notifications**: 22 live panel widgets (including a popout-corked 30 FPS FFT media visualizer and SNI system tray), `Mod+D` multi-plugin launcher (apps, `:emoji`, `=calc`, `>shell`, `w:window`), OSDs, and a `freedesktop.org` notification daemon.
- **Touch, Lock & Convertible Hardware**: Proportional 28-column virtual keyboard (`ShellOSK`), bezel edge swipe gestures, `ext-idle-notify-v1` OLED screen saver, `ext-session-lock-v1` PAM lock screen, and 360° foldable posture / EC backlight management.
- **`greetd` Login Greeter (`kajo-greeter`)**: Hardened login screen (`explicit_bzero`, `PR_SET_DUMPABLE=0`) running on `kajo-comp --greeter-mode`, `niri`, `cage`, or desktop `--test` preview mode.

---

## 📚 Documentation Index

| Document | Contents |
| :--- | :--- |
| **[`docs/design_system.md`](docs/design_system.md)** | Authoritative **Aero + Metro** visual design specification (color tokens, `0px` geometry, typography, component anatomy). |
| **[`docs/ui-gallery.html`](docs/ui-gallery.html)** | Interactive HTML/CSS visual reference gallery for every Kajo component and application layout. |
| **[`docs/brand-logo-gallery.html`](docs/brand-logo-gallery.html)** | Brand & application vector icon system gallery across all 8 Kajo executables. |
| **[`docs/spec_kajo_comp.md`](docs/spec_kajo_comp.md)** | Complete architecture specification for **`kajo-comp`** (scrollable-tiling strip, analytical springs, Wayland protocols, TOML schemas, IPC). |
| **[`docs/kajo-comp-storyboard.html`](docs/kajo-comp-storyboard.html)** | Interactive visual storyboard for **`kajo-comp`** strip mechanics, tabbed columns, `Alt+Tab` switcher, and screenshot UI. |
| **[`docs/spec_toml_config.md`](docs/spec_toml_config.md)** | Specification for the per-domain TOML 1.1 configuration store (`theme`, `shell`, `ide`, `files`, `mod`, `comp`, `outputs`, `binds`, `rules`) and `conf.d/` overlays. |
| **[`docs/kajo_ide_manual.md`](docs/kajo_ide_manual.md)** | Complete user & architecture manual for **`kajo-ide`** (modal states, Nordic chords, LSP/DAP, Mermaid, AI Agent IPC). |
| **[`docs/spec_kajo_mod.md`](docs/spec_kajo_mod.md)** | Architecture specification for **`kajo-mod`** (5-tier audio priority router, DSP pipeline, column reflow). |
| **[`docs/script_widgets.md`](docs/script_widgets.md)** | Guide for authoring custom panel script widgets for **`kajo`**. |
| **[`DEVELOPMENT.md`](DEVELOPMENT.md)** | Developer architecture guide, build instructions, and subsystem testing handbook. |
| **[`ROADMAP.md`](ROADMAP.md)** | Suite milestone tracking and upcoming engineering phases. |
| **[`HARDWARE_FINDINGS.md`](HARDWARE_FINDINGS.md)** | Convertible 2-in-1 sensor (`Intel ISH`), input inhibition, and Embedded Controller (`ec_sys`) register documentation. |

---

## 🛠️ Building and Installing

### 1. System Dependencies (Arch Linux / CachyOS)
```bash
sudo pacman -S --needed gtk4 gtksourceview5 gtk4-layer-shell wayland wayland-protocols \
    talloc pcre2 libinput libxkbcommon cairo pango libpng vulkan-icd-loader \
    libdrm seatd libdisplay-info libliftoff lcms2 pixman mesa vulkan-headers glslang hwdata \
    libxcb xcb-util-wm xcb-util-renderutil xcb-util-errors \
    xorg-xwayland json-glib glib2 libpulse pam poppler-glib libarchive mpv \
    libsidplayfp libgme libopenmpt libxmp gvfs gvfs-smb udisks2 blueprint-compiler meson ninja
paru -S --needed uade bencodetools
```

**Tree-sitter syntax engine** for `kajo-ide` (incremental parsing, AST-based highlighting). The runtime comes from the official repositories; the grammars are split between the official/CachyOS repositories and the AUR (use `paru` or `yay`):
```bash
# official / CachyOS repositories
sudo pacman -S --needed tree-sitter tree-sitter-c tree-sitter-bash tree-sitter-javascript \
    tree-sitter-lua tree-sitter-rust
# AUR
paru -S --needed tree-sitter-cpp tree-sitter-css tree-sitter-go tree-sitter-html \
    tree-sitter-java tree-sitter-json tree-sitter-toml tree-sitter-typescript tree-sitter-yaml \
    tree-sitter-m68k
```
Every grammar is optional at build time: `meson setup` links the ones it finds (`tree-sitter-<lang>`; `tree-sitter-typescript` provides TypeScript and TSX) and `kajo-ide` falls back to its built-in heuristic lexer for any language whose grammar is missing. Install all of them for full highlighting of C, C++, Java, Go, Rust, JavaScript/JSX, TypeScript/TSX, Bash/Zsh, Lua, Motorola 68000 (`tree-sitter-m68k`), JSON, TOML, YAML, CSS and HTML.

Optional runtime tools for **`kajo-ide`** language intelligence, search, and debugging:
```bash
sudo pacman -S --needed ripgrep clang gopls rust-analyzer pyright gdb lldb delve
```
For Motorola 68000 / Amiga assembly (`.s`, `.S`, `.asm`, `.i`): `m68k-lsp-server` (language server) and `vasmm68k_mot` from the [vasm](http://sun.hasenbraten.de/vasm/) distribution (build task and diagnostics) must be on `PATH`; both are optional.

### 2. Build, Verify & Install (`~/.local`)
By default, `meson.build` configures `prefix=~/.local`, installing all eight binaries (`kajo-comp`, `kajo`, `kajo-ide`, `kajo-files`, `kajo-player`, `kajo-mod`, `kajo-settings`, `kajo-greeter`) plus `kajo-session` and `kajo-greeter-session` to `~/.local/bin` and theme/syntax/icon/default config assets to `~/.local/share/kajo/`:

```bash
git clone --recursive https://github.com/mikolas/Kajo.git   # existing clones: git submodule update --init
meson setup builddir
meson compile -C builddir
meson test -C builddir
meson install -C builddir --skip-subprojects   # keeps the statically linked wlroots out of the install tree
```

---

## 🖥️ Compositor Sessions & Configuration (`kajo-comp` & Niri)

Kajo installs three Wayland session entries (`data/session/*.desktop`) and selects the active compositor IPC backend automatically at runtime (`$KAJO_COMP_SOCKET` first, falling back to `$NIRI_SOCKET`):
- **`kajo.desktop` ("Kajo")**: Launches `kajo-session` $\rightarrow$ `kajo-comp`, which automatically spawns the `kajo` desktop shell (`startup.command = ["kajo"]` in `data/defaults/comp.toml`) and binds all Kajo suite shortcuts out of the box (`data/defaults/binds.toml`).
- **`kajo-debug.desktop` ("Kajo (Debug)")**: Launches `kajo-session --debug` with verbose compositor logging to `$XDG_STATE_HOME/kajo/kajo-comp.log`.
- **`kajo-niri.desktop` ("Kajo (on Niri)")**: Launches `niri-session` for running the Kajo suite on the [Niri](https://github.com/niri-wm/niri) compositor.

### A. Kajo Compositor Configuration (`~/.config/kajo/{comp,outputs,binds,rules}.toml`)
`kajo-comp` works with zero user configuration out of the box using the shipped defaults in `~/.local/share/kajo/defaults/{comp,outputs,binds,rules}.toml`. Override any setting in `~/.config/kajo/comp.toml` or drop-in `~/.config/kajo/conf.d/*.toml`:

```toml
[layout]
gaps = 8
center_focused_column = "never"          # "never" | "always" | "on-overflow"
preset_column_widths = [0.33333, 0.5, 0.66667]
default_column_width = 0.5

[border]
enable = true
width = 1
active_color = "accent"                  # tracks live @metro-accent from theme.toml

[animations]
enable = true
horizontal_view_movement = { damping_ratio = 1.0, stiffness = 800.0, epsilon = 0.0001 }
```

### B. Niri Configuration (`~/.config/niri/config.kdl`)
When running the **`Kajo (on Niri)`** session (`kajo-niri.desktop`), add the Kajo shell startup and suite keybindings to `~/.config/niri/config.kdl`:

```kdl
// 1. Launch Kajo shell daemon at session startup
spawn-at-startup "kajo"

// 2. Kajo Suite Keybindings
binds {
    // Kajo Application Suite
    Mod+D             { spawn "kajo" "--toggle-launcher"; }
    Mod+C             { spawn "kajo-ide"; }
    Mod+E             { spawn "kajo-files"; }
    Mod+M             { spawn "kajo-player"; }
    Mod+Shift+M       { spawn "kajo-mod"; }
    Mod+S             { spawn "kajo-settings"; }
    Mod+K             { spawn "kajo" "--toggle-osk"; }

    // Session Lock & Screen Saver
    Mod+Shift+L       { spawn "kajo" "--lock"; }
    Mod+Shift+S       { spawn "kajo" "--screensaver"; }

    // Audio Volume & Mute (with Kajo OSD)
    XF86AudioRaiseVolume allow-when-locked=true { spawn "kajo" "--volume-up"; }
    XF86AudioLowerVolume allow-when-locked=true { spawn "kajo" "--volume-down"; }
    XF86AudioMute        allow-when-locked=true { spawn "kajo" "--volume-mute"; }

    // Display Backlight Brightness (with Kajo OSD)
    XF86MonBrightnessUp   allow-when-locked=true { spawn "kajo" "--brightness-up"; }
    XF86MonBrightnessDown allow-when-locked=true { spawn "kajo" "--brightness-down"; }
}
```

---

## 💻 Command-Line Interface (CLI) Summary

| Binary | Invocation | Description |
| :--- | :--- | :--- |
| **`kajo-comp`** | `kajo-comp [--config-dir <DIR>] [--renderer vulkan\|gles2\|pixman] [--mod super\|alt] [--greeter-mode] [--exit-with-command] [-- <CMD>...]` | Starts the Kajo scrollable-tiling Wayland compositor (`org.kajo.comp`) and listens on `$XDG_RUNTIME_DIR/kajo-comp.sock`. |
| **`kajo-ide`** | `kajo-ide [DIR \| FILES...]` | Opens a project workspace directory or specific source files in Kajo IDE. |
| **`kajo-files`** | `kajo-files [PATH_OR_URI...]` | Opens local folders or GVFS URIs (`trash:///`, `recent:///`, `smb://`, `sftp://`). |
| **`kajo-player`** | `kajo-player [FILE_OR_URI...]` | Opens video/audio media files in Kajo Player with sibling folder queue discovery. |
| **`kajo-mod`** | `kajo-mod [FILES \| ARCHIVES \| DIRS...]` | Opens Amiga, C64 SID, chiptune, or tracker modules, `.lha`/`.zip` archives, or folders in Kajo Mod. |
| **`kajo-settings`** | `kajo-settings [-p <PAGE>]` | Opens System Settings (`displays`, `network`, `bluetooth`, `audio`, `theme`, `panel`, `widgets`, `notifications`, `ide`, `mod`, `niri`, `about`). |
| **`kajo`** | `kajo [--toggle-launcher \| --lock \| --screensaver \| --toggle-osk \| --volume-up \| ...]` | Starts the Wayland shell daemon (no args) or dispatches an instant command over `$XDG_RUNTIME_DIR/kajo.sock`. |
| **`kajo-greeter`** | `kajo-greeter [--test] [--fullscreen] [--cmd <SESSION>]` | Runs the `greetd` login greeter (use `--test` to preview safely in a `1280×800` desktop window). |

---

## 📄 Configuration File Locations

| Path | Managed By | Purpose |
| :--- | :--- | :--- |
| **`~/.config/kajo/{comp,outputs,binds,rules}.toml`** | `kajo-settings` (`displays` / `niri`) / User | `kajo-comp` compositor configuration (`comp.toml` layout/springs/borders/input, `outputs.toml` monitor modes/scales/positions/VRR, `binds.toml` key & mouse bindings, `rules.toml` window & layer rules). |
| **`~/.config/kajo/{theme,shell,ide,files,mod}.toml`** | `kajo-settings` / User | Per-domain TOML 1.1 configuration files (`theme.toml` accent & typography, `shell.toml` panel/gestures/OSK/lock/widgets, `ide.toml` editor preferences, `files.toml` view/sort state, `mod.toml` DSP & HVSC settings). |
| **`~/.config/kajo/conf.d/*.toml`** | User / Dotfiles | Lexicographical read-only drop-in overrides (`[comp.*]`, `[output.*]`, `[binds]`, `[window_rule]`, `[theme]`, `[shell.*]`, `[ide]`, `[files]`, `[mod]`) merged on top of base `.toml` files. |
| **`~/.config/kajo/style.d/00-accent.css`** & **`theme-override.css`** | `kajo-settings` (`theme`) | Live Metro accent color definitions (`@define-color metro-accent ...`) hot-reloaded across all Kajo processes. |
| **`~/.config/niri/config.kdl`** | `kajo-settings` (`displays` / `niri`) | Output resolution/scale/transform/VRR blocks and layout/GLSL shader settings for Niri (`kajo-niri.desktop`). |
| **`/var/cache/kajo-greeter/state.json`** | `kajo-greeter` | Persists `last_user` and `last_session` across logins. |

---

## 📄 License & Attribution

- **Icons**: Incorporates [Fluent UI System Icons](https://github.com/microsoft/fluentui-system-icons) by Microsoft Corporation (MIT License).
- **Vendored code**:
  - [`tomlc17`](https://github.com/cktan/tomlc17) by CK Tan (MIT License) in `src/vendor/tomlc17/`; its license text is in `src/vendor/tomlc17/LICENSE`.
  - [`tinyexpr`](https://github.com/codeplea/tinyexpr) by Lewis Van Winkle (zlib License) in `src/vendor/tinyexpr/`; the notice is in the source header.
- **License**: MIT License — see [LICENSE](LICENSE); bundled third-party code and icons and their notices are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Kajo is distributed as source only and links its dependencies dynamically. Some of those libraries are copyleft (for example `poppler-glib`, `libmpv`, `libsidplayfp`, `uade`); a binary that links them and is redistributed by someone else is a combined work under those libraries' terms, which is that redistributor's responsibility.

## Author

Mikolas Hämäläinen — [mikolas@mikolas.net](mailto:mikolas@mikolas.net)
