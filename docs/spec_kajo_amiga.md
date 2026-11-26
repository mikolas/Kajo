# Specification: `kajo-amiga` — Self-Contained Aero + Metro Amiga 68000 / C / OCS–AGA IDE (`docs/spec_kajo_amiga.md`)

**Status:** Approved Architecture — Design Specification (Not Yet Implemented)  
**Target Binary:** `kajo-amiga` (`org.kajo.Amiga`, `src/amiga/`)  
**Architecture:** **100% Self-Contained C17 / GTK4** — zero runtime dependency on external `hunks` or embedded Amiga VM  
**Shared Cores:** Shared editor core promoted from `src/ide/` (Tree-sitter `m68k` & `c`, `m68k-lsp-server` & `clangd`, modal FSM, `IdeEditorHost`, Cairo gutter, Markdown/Mermaid preview)  
**Config Integration:** `KajoConfigStore` domain `amiga.toml` (`KAJO_CONFIG_DOMAIN_AMIGA` + `kajo-settings --page amiga`) + per-project `kajo-amiga.toml`  
**Core Dependencies:** `gtk4`, `gtksourceview-5`, `gio-2.0`, `gio-unix-2.0`, `cairo`, `pangocairo`, `json-glib-1.0`, `tree-sitter` (`m68k` + `c`), **`adflib`** ($\ge$ 0.10.7, system `pkg-config` with Meson CMake `subprojects/adflib.wrap` fallback — zero vendored source; `GPL-2.0-or-later` dynamic linkage, matching `kajo-mod` linking `libsidplayfp`)  
**Optional Dependencies (`required: false`):** `libelf` / `libdw` (`elfutils`, for in-process DWARF3 symbol/line parsing; fallback to `.map` + `m68k-amiga-elf-objdump`)  
**External Runtime Tools:** `amiberry` ($\ge$ 8.5.0 with `gdbserver`/`segtracker`), `vasmm68k_mot`, `m68k-amiga-elf-gcc` + `m68k-amiga-elf-ld` + `elf2hunk` (or optional `vc`/`vbcc` + `vlink`), `m68k-lsp-server`, `clangd`  
**Interactive Mockup:** [`docs/kajo-amiga-mockup.html`](kajo-amiga-mockup.html)  
**Design & IDE References:** [`docs/design_system.md`](design_system.md), [`docs/kajo_ide_manual.md`](kajo_ide_manual.md)

---

## 1. Motivation, Two Operating Modes & Relationship to `kajo-ide`

`kajo-amiga` is a dedicated, self-contained Amiga 68000 / C + ASM / OCS–ECS–AGA integrated development environment and live hardware inspector for the Kajo Desktop Suite. It supports two first-class workflows on native Linux/Wayland: building, packaging (`DH0:` or `.adf`), and source-debugging C + 68000 projects, **and** attaching directly over GDB RSP to an already-running Amiberry instance to inspect closed-source demos, games, or system state with Motorola-syntax disassembly and zero source files required.

### 1.1 Project Mode vs. Attach Mode

| Area | **Project / Source Mode** (`kajo-amiga <dir>`) | **Attach / Live Inspection Mode** (`kajo-amiga --attach [host:port]` or `[ 🔌 ATTACH ]`) |
| :--- | :--- | :--- |
| **Amiberry Ownership** | Launched by `kajo-amiga` (`owns_amiberry_process = true`). `Shift+F5` stops Amiberry; `[ 🔌 DETACH ]` releases RSP cleanly. | Launched externally by user (`owns_amiberry_process = false`). **Closing `kajo-amiga` or clicking `[ 🔌 DETACH ]` never kills Amiberry** — `D` removes `kajo-amiga` breakpoints/watchpoints/monitor state, resumes the guest, and closes TCP. |
| **Left Sidebar** | Workspace `FILES`, C/ASM `OUTLINE`, + relocated `HUNKS` (`qRcmd,segments`) + `⚙ SETUP` trigger | Live **`TASKS & SEGMENTS`** from `qRcmd,tasks` + `qRcmd,segments` (falling back to `qXfer:memory-map:read` / `memory-map` raw RAM ranges if a demo kills AmigaOS and `segments` fails/is empty). |
| **Center Pane** | Shared `src/ide/` C + 68000 editor (`[ SOURCE \| DISASM ]`, auto-switches to `DISASM` when `PC` enters Kickstart ROM or symbolless code). | **Live Motorola-Syntax Disassembly View** (`qRcmd,disasm`) centered on `PC` (or any jumped segment/address) with clickable `F9` physical breakpoints (`Z0`), opcode words, live EA annotations, static line-by-line `n(r/w)` cycle column, and `$DFFxxx` / Exec LVO comments. |
| **Right Inspector** | All 5 tabs (`Bitplanes`, `Copperlist`, `Chips & Blitter`, `DMA Budget`, `HRM Docs`) seeded from `m dff000,200` + project symbols or manual/scanned addresses. | Same 5 tabs — works directly against live Chip RAM and Amiberry's full `$DFF000..$DFF1FF` `save_custom()` snapshot (`m dff000,200`), plus Copper scanning (`[ 🔎 SCAN COP ]`) and non-breaking `log_only=1` watch logs. |
| **Bottom Drawer** | `DEBUG & WATCH`, `MEMORY HEX`, `BUILD & GUEST OUTPUT` | `DEBUG & WATCH` + `MEMORY HEX` (`BUILD & GUEST OUTPUT` hidden when no source project is open). |

### 1.2 Shared Design & Resource Discipline
1. **Why a Dedicated Sibling App Instead of Bloating `kajo-ide`:** [`kajo-ide`](kajo_ide_manual.md) stays clean and general-purpose (Go, Java, C/C++, Rust, Python, TS), while `kajo-amiga` houses Amiga-specific tooling (68000/Amiga-C builder, `adflib` packaging, 68000 RSP debug, hunk relocation, bitplane/Copper/Blitter/DMA hardware inspectors) and tiles beside Amiberry in `kajo-comp` / Niri.
2. **One Source of Truth — Zero Code Duplication (`AGENTS.md §7`):** `kajo-amiga` **must not** fork `src/ide/`. Buffer management, Tree-sitter `m68k` + `c`, `m68k-lsp-server` + `clangd`, the modal FSM, Cairo gutter, picker/ripgrep, and Markdown/Mermaid rendering link from a promoted shared static core library (`libkajo_ide_core`).
3. **Self-Contained Only (No `hunks` Tool or Embedded VM):** `kajo-amiga` does **not** shell out to `hunks` or bundle a decompiler / custom Amiga CPU VM. Decompilation remains in `hunks` as an independent workflow.
4. **Stop-Driven & Zero-Idle-Polling Resource Discipline (`AGENTS.md §4.D`, `§8.4`):** When Amiberry is disconnected or running freely (`running`), `kajo-amiga` installs **no** periodic RSP polling timers (`< 0.05%` idle CPU). Hardware views refresh strictly on debugger stops or user-clicked **`[ ↻ SNAPSHOT ]`** (`Pause → Read → Continue`). All async `GSubprocess`, `GSocketClient`, and `GFileMonitor` handles use `GCancellable` / `GWeakRef`.

---

## 2. Shared Core Extraction & `src/amiga/` Module Architecture

Non-Amiga editor subsystems are reused from `src/ide/` and `src/common/`, while Amiga project, build, ADF packaging, protocol, decoding, and inspector modules live under `src/amiga/`.

### 2.1 Shared Cores Reused from `src/ide/` & `src/common/`

| Shared Module | Current Coupling Check | Extraction / Seam Needed for `kajo-amiga` |
| :--- | :--- | :--- |
| **`ide_buffer`, `ide_syntax`, `ide_ts`, `ide_ts_queries`** | **0** `IdeWindow` refs | Promote unchanged to `libkajo_ide_core` (`.s`/`.S`/`.asm`/`.ASM`/`.i`/`.I` → `m68k` and `.c`/`.h` → `c` Tree-sitter queries already present). |
| **`ide_fsm`, `ide_fsm_tables`** | **0** `IdeWindow` refs | Promote and expose extensible leader-chord / function-key hooks so `kajo-amiga` registers `Space a *` and `F5`–`F12`. |
| **`ide_actions`** | **79 `IdeWindow` refs** | Extract a thin **`IdeEditorHost`** callback struct (active buffer, focus pane, drawer/tree toggle, status toast, save/build hooks) implemented by both `IdeWindow` and `AmigaWindow` so modal editing actions are never duplicated. |
| **`ide_lsp_client`, `ide_lsp_parse`, `ide_lsp_servers`, `ide_lsp`** | **0** `IdeWindow` refs | Allow unconditional host opt-in so `kajo-amiga` runs stdio `m68k-lsp-server` (for ASM) and `clangd` (for C, via generated `compile_commands.json`) inside any `kajo-amiga.toml` workspace. |
| **`ide_gutter`** | **0** `IdeWindow` refs | Add an optional generic 7th Cairo column callback (`IdeGutterAnnotateLineCb`), keeping line-by-line 68000 cycle calculation inside `amiga_decode.c`. |
| **`ide_tree`, `ide_picker`, `ide_search`** | **0** `IdeWindow` refs | Reuse directly for file tree, Git badges, `Space f f`, `Space f g`, symbol outline (`Space f s`), and clickable compiler/assembler diagnostics. |
| **`ide_markdown`, `ide_markdown_embeds`, `ide_mermaid`** | **0** `IdeWindow` refs | Already shared with `kajo-viewer`; hosts local Amiga HRM Markdown docs (`K` symbol lookup). |
| **`kajo_spawn_detached`, `KajoConfigStore`** | `src/common/`, `src/config/` | Shared launch helpers and `amiga.toml` domain (`KajoAmigaCfg`). |

### 2.2 `src/amiga/` Module Layout

| File | Layer | Responsibility |
| :--- | :--- | :--- |
| `src/amiga/main.c` | App | `GtkApplication` (`org.kajo.Amiga`, `G_APPLICATION_HANDLES_OPEN`), CLI flags (`--attach [host:port]`, `--build [target]`, `--preview-mode ...`, `--effect <name>`), `kajo_app_init_theme()`, `ShellIcons` init, `~/.config/kajo/amiga.toml` `GFileMonitor` hot-reload. |
| `src/amiga/amiga_project.{h,c}` | **GTK-free Core** | Parses & writes `kajo-amiga.toml` (`build_mode`, orthogonal `[[target]]`, `[[asset]]`, `[[profile]]` tables; backward-compatible `.kajo-amiga.toml` / `Makefile` import), starter templates, auto-derives `SDKDIR` (`../m68k-amiga-elf/sys-include`) from `m68k-amiga-elf-ld`, generates `compile_commands.json` (`clangd`) & `m68k-lsp-server` include args, checks toolchain paths, and exports optional standalone `Makefile`s. |
| `src/amiga/amiga_build.{h,c}` | Async Runtime | **Option D Hybrid Builder:** Native incremental C17 DAG builder over parallel `GSubprocess` jobs — parses `vasmm68k_mot -dependall=make -depfile` and `m68k-amiga-elf-gcc -MMD -MF` `.d` depfiles, runs mtime-cached `[[asset]]` / `pre_build` steps, compiles C + 68k ASM (shared base objects + per-target sources + `-D` defines), links (`elf2hunk` or `vlink`/`vc`), stages `DH0:` or invokes `amiga_adf`, and powers `kajo-amiga --build`. Falls back to `external` (`build_cmd`) for Makefile repos. |
| `src/amiga/amiga_adf.{h,c}` | **GTK-free Core** | In-process `.adf` packager built on **`adflib`**: formats OFS/FFS floppy images (`adfCreateFlop`, `adfVolCreate`/`adfVolMount`), writes `s/startup-sequence`, `.exe`, libs, and assets (`adfCreateDir`, `adfFileOpen`, `adfFileWrite`), installs DOS bootblocks (`adfVolInstallBootBlock`), and writes raw 901,120-byte trackdisk/bootblock `.adf` images (`adf_raw`) with computed 68000 boot checksums. Zero `xdftool` dependency. |
| `src/amiga/amiga_rsp.{h,c}` | **GTK-free Core** | Single-socket async GDB RSP TCP client over `GSocketClient` (`127.0.0.1:<port>`): `$...#cs` framing, `QStartNoAckMode`, `g`/`G` (18 × 32-bit BE: `D0–D7, A0–A7, SR, PC`), `m`/`M` (including `$DFF000..$DFF1FF` `save_custom(..., full=1)` snapshot reads), `Z0`/`z0` (`len=2`, even `< 16MB` in 24-bit), `Z2–Z4` (up to `0x10000`, `!mmu_enabled`), `vCont;c`/`s`/`r` (address-range stepping for C lines), `D` (detach + resume), and exact `qRcmd` wrappers returning hex-encoded UTF-8 `$<hex>#cs` packets. |
| `src/amiga/amiga_symbols.{h,c}` | **GTK-free Core** | Parses ELF/DWARF3 (`libelf`/`libdw`, fallback `.map` + `m68k-amiga-elf-objdump`) and Amiga Hunk `HUNK_SYMBOL`/`HUNK_DEBUG LINE` blocks; applies `qRcmd,segments` runtime hunk deltas (`PC ↔ file:line:symbol`); computes C statement line address ranges for `vCont;r`; extracts `.asm`/`.c` document outlines and `$DFFxxx`/LVO metadata. |
| `src/amiga/amiga_decode.{h,c}` | **GTK-free Core** | Pure OCS/ECS/AGA decoders: 68000 `SR` flags (`T/S/I/X/N/Z/V/C`), line-by-line 68000 static cycle LUT (`n(r/w)`, ranges `38–70`, shift `6+2n`, branch `10/14` — no subroutine/loop detection in v1), literal / OCS RGB12 / `BLTCON0` minterm decoders, Copperlist disassembler (`WAIT`/`MOVE`/`SKIP`, `BFD=0`, `$FFDFFFFE`, `COPCON`), Chip RAM Copper scanner, Blitter decoder, Planar/Interleaved/`HAM6`/`EHB` bitplane renderer, and parameterized PAL/NTSC 227-clock DMA budget math. |
| `src/amiga/amiga_disasm.{h,c}` | **GTK-free Core** | Motorola-syntax disassembly coordinator: parses Amiberry's **plain-text** `qRcmd,disasm <hex_addr> <dec_count>` responses (from `m68k_disasm_2`) and enriches every row with line-by-line static `n(r/w)` cycles, `$DFFxxx` names, Exec/DOS/Graphics LVO labels, and relocated symbols. |
| `src/amiga/amiga_runner.{h,c}` | Async Runtime | Amiberry launcher consuming the active `[[profile]]` (`--model`, Kickstart, RAM, PAL/NTSC, cycle-exact, `-m DH0:...` or `-0 ...adf`, `-s debugging_features=segtracker,gdbserver -s gdb_port=<port>`, `-s cpu_threaded=false`, and `-s uaeboard=1` on floppy boots when Trap 86 `guest-output` is enabled). |
| `src/amiga/amiga_project_ui.{h,c}` | GTK4 UI | **Four Aero + Metro project-management UI surfaces** manipulating `kajo-amiga.toml` cleanly: (1) **Welcome / Start Screen** (New, Open, Recent Projects, Attach `:2345`, toolchain + `adflib` readiness); (2) **Header `📁 PROJ` Menu**; (3) **New Project Wizard Modal (`Ctrl+Shift+N`)** with starter templates & live tool readiness; (4) **Center-Pane `[⚙ kajo-amiga.toml]` Project Setup Tab (`Ctrl+Shift+P` / sidebar `SETUP`)** with **`[ FORM | RAW TOML ]`** toggle and 4 tabs (*Targets & Packaging*, *Hardware Profiles*, *Assets & Hooks*, *Debug & Symbols*) + `EXPORT MAKEFILE`. |
| `src/amiga/amiga_window.{h,c}` | GTK4 UI | Frameless Aero + Metro `GtkApplicationWindow` (`.shell-settings-window.shell-amiga-window`), 5-zone layout, Project + Attach mode switching, Target + Hardware Profile selectors, `[ SOURCE \| DISASM ]` stack, responsive `< 900px` Niri `[ CODE \| INSPECTOR ]` collapse, and `IdeEditorHost`. |
| `src/amiga/amiga_disasm_view.{h,c}` | GTK4 UI | Live Motorola disassembly view: physical address, opcode bytes, line-by-line `n(r/w)` cycles + Selection Cycle Sum, `F9` `Z0` breakpoints, `Ctrl+G`, `Enter` target follow + `Alt+Left/Right` history nav, `Follow PC`, `Run to Cursor`, `Set PC Here`, inline word patch (`M`), and `history read` 128-entry trace viewer. |
| `src/amiga/amiga_cpu_strip.{h,c}` | GTK4 UI | Compact 48px `D0–D7`/`A0–A7`/`PC`/`SR` strip with amber diffs, inline editing (`G`), clickable `SR` bit toggles, multi-format hover + `An` deref, exact cycle delta (`execution-status.cycles`), `VPOS`/`HPOS`/frame, and `💾 CP` / `⏮ CP` checkpoints. |
| `src/amiga/amiga_bitplane_view.{h,c}` | GTK4 UI | Bitplane viewer (`1..6 BPL`, `HAM6`, `EHB`, planar/interleaved/line×2), fed by `m dff000,200` (`BPL1PT..BPL8PT`, `BPLCON0..4`, `BPL1/2MOD`, `COLOR00..31`), Symbol, or Manual `$Addr`; `[P0..P5]`, `16px` grid, 32-swatch palette, cursor probe, and PNG export. |
| `src/amiga/amiga_copper_view.{h,c}` | GTK4 UI | Decoded Copperlist view seeded by `COP1LC`/`COP2LC`/`COPCON` (`m dff000,200`), Manual `$Addr`, `[ 🔎 SCAN COP ]`, `[ 👁 WATCH $DFF080 ]` (`log_only=1`), filters (`ALL`/`WAITS`/`BLITTER`/`DISPLAY`), and slot tally. |
| `src/amiga/amiga_chips_view.{h,c}` | GTK4 UI | Custom chips & Blitter inspector (`LIVE` readable regs, `EMU SNAPSHOT` via `m dff000,200`, opt-in `WATCHED` `log_only=1` blit trace, and honest `WO — UNAVAILABLE` for `$0A0–$0DF` audio & AGA colors 32–255). |
| `src/amiga/amiga_dma_view.{h,c}` | GTK4 UI | Cairo 227-color-clock PAL/NTSC scanline visualizer + fully parameterized frame odd-slot & 68000 CPU budget calculator. |
| `src/amiga/amiga_mem_view.{h,c}` | GTK4 UI | Virtualized Chip/Fast RAM dump (`[ 8b \| 16b \| 32b ]` + ASCII), amber stop diffs, inline edit (`M`), search (`Ctrl+F`), register jump, `📌 FOLLOW`, CPU (`Z2–Z4`/`watch range`) + DMA (`dma-watch`) watches, and `.bin` export. |
| `src/settings/pages/page_amiga.c` | Settings | Modular `kajo-settings --page amiga` page (`13 / 14 • KAJO AMIGA`), matching `page_ide.c` / `page_mod.c` / `page_web.c` (`SAVE & APPLY` + `REVERT`, live tool/ROM probes, editor inheritance from `ide.toml`). |
| `tests/test_amiga_*.c` & `tests/mock_amiberry_rsp.py` | Verification | GTK-free GTest suites (`amiga_decode`, `amiga_project`, `amiga_build`, `amiga_adf`, `amiga_symbols`, `amiga_rsp`, `amiga_disasm`) + mock Amiberry 8.5.0 RSP server runnable under ASan/UBSan. |

---

## 3. Build System (Option D), Mixed C + ASM, Packaging & Amiberry Protocol

### 3.1 Option D Hybrid Build System & Supported Toolchains

`kajo-amiga` uses **Option D**: a native declarative `kajo-amiga.toml` incremental builder (`amiga_build.c`) by default, paired with an `external` escape hatch for pre-existing `Makefile` repositories (`seeandseedee`, Aminet/Bartman templates):

1. **Native Mode (`build_mode = "native"` — Default):**
   - `amiga_build.c` runs parallel `GSubprocess` compile jobs with no external `make`/`ninja` requirement, also exposed headlessly as `kajo-amiga --build [target]`.
   - **Incremental Dependencies:** Parses `vasmm68k_mot -dependall=make -depfile obj/<unit>.d` and `m68k-amiga-elf-gcc -MMD -MF obj/<unit>.d` depfiles covering all `INCLUDE`, `INCBIN`, and `#include` graphs.
   - **Auto-Derived NDK Includes & C LSP Sync:** Automatically resolves `SDKDIR = <dir_of(m68k-amiga-elf-ld)>/../m68k-amiga-elf/sys-include` and writes `.kajo-amiga/compile_commands.json` whenever project config changes so `clangd` and `m68k-lsp-server` match exact build flags (`-I`, `-D`, `-mcpu=68000`) with zero drift.
   - **Optional `Export Makefile`:** Generates a standalone GNU `Makefile` mirroring `kajo-amiga.toml` if a contributor wants to build without `kajo-amiga`.
2. **External Escape Hatch (`build_mode = "external"`):**
   - Invokes configured `build_cmd` / `clean_cmd` (e.g. `make -j4 effect=${target}`) while consuming `exe_path` / `elf_path` / `map_path` for relocation and debugging.

| Pipeline Toolchain | Tools (`C` + `68k ASM`) | Debug Info Source | C Debugging Scope (`AGENTS.md §8`) |
| :--- | :--- | :--- | :--- |
| **`elf2hunk`** *(default)* | `vasmm68k_mot -Felf -dwarf=3` + `m68k-amiga-elf-gcc -g -MMD` → `m68k-amiga-elf-ld --emit-relocs` → `elf2hunk` | Full DWARF3 `.debug_line` & ELF symbols in `<target>.elf`, executed as `<target>.exe` | **v1:** C editing (`Tree-sitter c` + `clangd`), DWARF3 line breakpoints, `vCont;r` C statement stepping, global/typed watches. **v1.5:** DWARF `.debug_info` locals/structs & CFI/A5 call-stack unwind. |
| **`hunk_vlink`** | `vasmm68k_mot -Fhunk -linedebug` (and optional `vc` / `vbcc` for OS NDK C) → `vlink -bamigahunk` | Native Amiga `HUNK_SYMBOL` & `HUNK_DEBUG` (`LINE`) inside `<target>.exe` | ASM / `vbcc` line-level debug + globals (`HUNK_DEBUG LINE`; no DWARF types). |
| **`single_asm`** | `vasmm68k_mot -Fhunkexe -linedebug -kick1hunks` | `HUNK_SYMBOL` / `LINE` blocks inside standalone executable | Quick single-file 68k ASM experiments. |

- Diagnostics from `vasmm68k_mot`, `m68k-amiga-elf-gcc`, `vc`, `m68k-amiga-elf-ld`, and `vlink` parse into clickable rows in **`🔨 BUILD & GUEST OUTPUT`** + editor gutter marks.
- Missing binaries surface an actionable banner pointing to `kajo-settings --page amiga` (`extra_bin_paths`).

### 3.2 Orthogonal Project Model (`[[target]]`, `[[asset]]`, `[[profile]]`) & `adflib` Packaging

`kajo-amiga.toml` cleanly separates *what is built* (`[[target]]`, `[[asset]]`) from *which Amiga runs it* (`[[profile]]`):

1. **`[[target]]` (1..N per workspace, selectable in header dropdown):**
   - Shared base objects (`shared_sources`, e.g. `devharness.asm`, `setsystem.asm`, `debug.asm`) + per-target `sources` (`.asm` and/or `.c`), `cpu` (`68000`–`68060`), `toolchain`, `includes`, `defines` (e.g. `TRACKMO=1`), `extra_files` (assets/libs staged onto disk), and **`kind`**:
     - `os_exe` *(Default)*: Links `<target>.exe` and stages `$XDG_CACHE_HOME/kajo/amiga/<project>/dh0/` with generated `s/startup-sequence`. Launches Amiberry with `-m DH0:<staging_dir> -G --log`.
     - `adf_dos`: Builds an in-process bootable OFS/FFS `.adf` image directly via **`adflib`** (`amiga_adf.c`: `adfCreateFlop`, `adfVolCreate`/`adfVolMount`, `adfCreateDir("s")`, `adfFileOpen`/`adfFileWrite` for `startup-sequence`, `.exe`, `extra_files`, `adfVolInstallBootBlock`). Launches `amiberry -0 <out>.adf -G --log` (plus `-s uaeboard=1` if `guest-output` is enabled).
     - `adf_raw`: Writes a raw 901,120-byte bootblock + trackdisk `.adf` image directly in C17 with valid 68000 bootblock checksum (`$0000` magic `DOS\0`, `$04` checksum, `$08` rootblock, trackloader payload sectors). Uses physical entry-address breakpoint instead of `process-break`.
2. **`[[asset]]` (Mtime-Cached Generators & Crunchers):**
   - Declarative steps (`inputs`, `outputs`, `cmd` — e.g. PNG→planar bitplanes, trig/sine table generators, `shrinkler` fast/slow crunching) executed automatically before dependent targets when inputs are newer than outputs.
3. **`[[profile]]` (Hardware Presets in `amiga.toml` & Overridable in `kajo-amiga.toml`):**
   - Built-in defaults: `a500_ocs` (`A500`, `68000`, OCS, 512K Chip + 512K Slow, Kick 1.3, PAL, cycle-exact), `a500p_ecs` (1M Chip, Kick 2.04), `a1200_aga` (`A1200`, `68020`, AGA, 2M Chip, Kick 3.1), `a1200_fast` (`68020`, +8M Fast), plus NTSC variants.
   - Launching a `68020`/AGA target against an `A500` (`68000`/OCS) profile displays an immediate inline warning before booting.
4. **Project Creation, Switching & Editing UI (`amiga_project_ui.c`):**
   - **Welcome / Start Screen:** Shown when `kajo-amiga` launches without a workspace (`+ New Project`, `Open Folder`, `Recent Projects`, `Attach to Running Amiberry :2345`, live toolchain + `adflib` readiness card, and `kajo-settings --page amiga` link).
   - **Header `📁 PROJ ▾` Menu & Left Sidebar Triggers:** Always-visible project menu (`New Project`, `Open Folder`, `Recent`, `Project Setup`, `Export Makefile`, `Welcome Screen`) paired with compact `+ NEW` / `SETUP` buttons in the sidebar header.
   - **New Project Wizard Modal (`Ctrl+Shift+N`):** Centered `0px` Aero + Metro modal with Name/Folder, 4 template cards (*AmigaDOS 68k ASM (`os_exe`)*, *Bare-Metal OCS/ECS Demo (`os_exe`)*, *Bootable Floppy (`adf_dos` / `adf_raw` via `adflib`)*, *Mixed C + 68k ASM (`m68k-amiga-elf-gcc` + `elf2hunk`)*), initial Hardware Profile & Packaging pickers, and live toolchain readiness strip.
   - **Center-Pane `[⚙ kajo-amiga.toml]` Project Setup Tab (`Ctrl+Shift+P`):** Opens directly as a center editor tab with a **`[ FORM | RAW TOML ]`** toggle — so it stays readable inside `< 900px` Niri splits, keeps the workspace tree accessible, and exposes 4 tabs (*Targets & Packaging*, *Hardware Profiles*, *Assets & Hooks*, *Debug & Symbols*) + explicit `SAVE` and `EXPORT MAKEFILE`.

### 3.3 Verified Amiberry 8.5.0 GDB RSP Protocol (Single Socket)

All protocol shapes below were verified directly against upstream Amiberry (`/home/mikolas/src/amiberry`):

- **Launch Flags:**
  ```bash
  amiberry ... -s debugging_features=segtracker,gdbserver -s gdb_port=2345 -s cpu_threaded=false
  ```
  - Amiberry automatically disables `cpu_threaded` whenever `DEBUG_FEATURE_GDBSERVER` is set (`-s cpu_threaded=false` is explicit insurance). Keeping `segtracker` enables `m68k_disasm_2` segment annotations.
  - Amiberry permits only one active GDB connection and refuses simultaneous IPC monitor state mutations while connected, so `amiga_rsp.c` owns the RSP TCP socket directly (`D` detaches cleanly when released for CLI `gdb`).
- **Transport & Packet Rules:**
  - Accepting a TCP connection on `:2345` immediately pauses the 68000 (`accept()` calls `debug_gdb_target().interrupt()`).
  - Supports `QStartNoAckMode`, `vCont;c` / `vCont;s` / **`vCont;r<start>,<end>`** (address-range stepping via `GdbTarget::resume_range()`, used to step C source lines), `g`/`G`, `m`/`M`, `Z0`–`Z4` / `z0`–`z4`, `qXfer:memory-map:read`, and `D`.
  - **Every `qRcmd,<hex>` reply** returns as a **single standard RSP reply packet** `$<hex-encoded UTF-8>#cs` (NOT `O...` packets!): empty result returns `"OK"`, unsupported returns `""`, invalid args return `"E01"`, and commands requiring halted state while running return `"E16"`.
  - **Running vs. Halted commands:** Only `capabilities`, `execution-status`, and `input ...` succeed while the CPU is running; every other `qRcmd` and `g`/`G`/`m`/`M`/`Z`/`z` returns `E16`.
- **Registers (`g` / `G`):**
  - 18 × 32-bit big-endian registers (144 hex chars): `0..7 = D0..D7`, `8..15 = A0..A7`, **`16 = SR`**, **`17 = PC`**.
- **Breakpoints & Watchpoints (`Z0`–`Z4`):**
  - `Z0,<addr>,2` / `Z1,<addr>,2`: **length MUST be `2`**, address must be even (and `< 0x01000000` in 24-bit mode), else `E01`.
  - `Z2` (write), `Z3` (read), `Z4` (access): CPU data access watchpoints supporting byte lengths up to `0x10000` (requires `!mmu_enabled`).
- **Full Custom-Chip Shadow Read (`m dff000,200`) — Key Finding:**
  - Reading `$DFF000..$DFF1FF` over normal RSP `m` does **not** touch live hardware I/O: Amiberry intercepts the range and serves the 512-byte **`save_custom(..., full=1)`** shadow snapshot.
  - Yields `DMACONR`/`VPOSR`/`VHPOSR`/`CLXDAT`/`ADKCONR`/`INTENAR`/`INTREQR`, `COPCON`, **`COP1LC`/`COP2LC`**, **`BLTCON0/1`**, `BLTAFWM/LWM`, `BLTxPT`, `BLTSIZE/V/H`, `BLTxMOD`, `BLTxDAT`, `DIW*`, `DDF*`, `DMACON`/`INTENA`/`INTREQ`, **`BPL1PT..BPL8PT`**, **`BPLCON0..4`**, `BPL1/2MOD`, `SPR0PT..7PT`, `SPRxPOS/CTL`, **`COLOR00..31`** (ECS 12-bit or AGA upper nibbles), `FMODE`, and PAL/NTSC flag at `$DFF1FA` (`0x8000 | ntscmode`).
  - **Honest gaps in `save_custom()`:** Audio channel words `$0A0–$0DF` are zeroed, and AGA 24-bit low nibbles / colors 32–255 are not part of the 512-byte block (IPC `GET_CUSTOM_REGS` is also unneeded because it returns far less than `m dff000,200`).
- **Exact `qRcmd` Monitor Commands Supported:**
  - `capabilities` → supported remote features JSON
  - `execution-status` → `{"execution":"running"|"stopped"|"host-paused","debugger_stopped":bool,"host_pause":int,"frame":N,"vpos":N,"hpos":N,"cycles":"<u64_str>"}` (`cycles` is a string; read `PC` from `g` reg 17)
  - `tasks` → `{"tasks":[{"address":N,"name":"...","type":N,"state":N,"list":"current"|"ready"|"waiting","signals_wait":N,"saved_sp":N,"stack_lower":N,"stack_upper"},...]}` (note: no per-task PC)
  - `segments` / `segments <hex_task>` → walks Exec `ThisTask` (or specific `NT_PROCESS`) `pr_CLI->cli_Module` / `pr_SegList` directly in guest RAM (**works even without Amiga-side SegTracker**): `{"process":N,"name":"...","command":"...","segments":[{"index":0,"address":N,"size":N},...]}`
  - `process-break name <exe>` / `process-break address <hex>` / `clear` → **one-shot AmigaDOS process entry break** (`pc == segments + 4`), halts automatically with `T05winuae-entry:<proc>;11:<pc>;` (**eliminates boot polling for `os_exe` / `adf_dos`**)
  - `disasm <hex_addr> <dec_count>` → **plain-text** lines from `m68k_disasm_2` (`count` 1..100, hex addr without `0x`/`$`): lowercase Motorola mnemonics/sizes/registers, `$` hex prefix, opcode words, live `== $addr` EA values, `(T)/(F)` branch truth, and optional segment/symbol header lines
  - `step-over` → **two-stage reply**: immediately returns `"OK"`, then emits an async `T05...` stop packet when the target instruction is reached (`amiga_rsp.c` expects both packets)
  - `watch range <addr> <len> <rwi> <src_mask> <val_en> <val> <val_mask> <chg_only> <log_only>` (9 space-separated hex args; `rwi`: `1`=r, `2`=w, `3`=rw; `log_only=1` records up to 64 hits in `watch events`/`watch last` **without stopping the CPU**), `watch events`, `watch last`, `watch clear-events`, `watch remove <slot>`
  - `dma-watch add <addr> <len> <rwi> <dma_src_mask>` / `list` / `remove <slot>` → watch Blitter/Copper/Bitplane DMA accesses
  - `condition add <reg> <op> <val> <mask> <signed>` / `list` / `remove <slot>` → hardware conditional breakpoints (`Shift+F9`)
  - `exception mask <hex>` / `exception last` → trap 68000 vectors 2..63 (Bus, Addr, Illegal, Div0, TrapV, Privilege) capturing fault address/access/size, `VBR`, `USP`, `ISP`, `MSP`
  - `history on|off` + `history read <hex_count>` → 128-entry executed instruction ring buffer (`{pc, frame, vpos, hpos, registers[16]}`)
  - `guest-output on|off|clear|read` → JSON ring buffer (64 records / 4096B) capturing `uaelib` **Trap 86** strings (only installed when `uae_boot_rom_type || currprefs.uaeboard`, so floppy `.adf` boots require `-s uaeboard=1`; note Bartman's Trap 88 `$f0ff60` maps to `emulib_execute_on_host` in stock Amiberry, and classic serial `SERDAT` `KPrintF` does not trigger `guest-output`)
  - `checkpoint save <path>` / `checkpoint restore <path>` → `.uss` savestates preserving the active GDB session
  - `memory-map` (or `qXfer:memory-map:read`) → active Chip/Bogo/Fast/ROM XML or JSON regions
- **Project Startup & Attach Handshakes:**
  - *Project `F5` (`os_exe` / `adf_dos`):* connect TCP → `QStartNoAckMode` → `qRcmd,capabilities` → `qRcmd,process-break name <target.exe>` → `qRcmd,guest-output on` (optional) → `vCont;c`. Amiberry halts automatically at `segments + 4` (`T05winuae-entry:...`), then `amiga_rsp` fetches `qRcmd,segments`, relocates ELF/Hunk debug info, installs all `F9` `Z0` breakpoints, and either holds at entry or continues.
  - *Project `F5` (`adf_raw`):* installs configured physical `Z0` entry breakpoint instead of `process-break`.
  - *Attach Mode (`--attach`):* connect TCP → `QStartNoAckMode` → query `capabilities`, `execution-status`, `g`, `m dff000,200`, `segments`, `tasks`, `memory-map`. If `segments` fails (OS-killed demo), operate directly in physical address mode without failing.
- **Step Mapping (`F10` / `F11` / `F12`):**
  - `F11` **Step Into:** `vCont;s` (or `vCont;r<line_start>,<line_end>` / step until source line changes in C mode)
  - `F10` **Step Over:** `qRcmd,step-over` in ASM/disasm (`OK` then async `T05...`), or range step / temp `Z0` at next source line in C mode
  - `F12` **Step Out:** best-effort read `(A7)` longword + temp `Z0` (honestly warns if `(A7)` does not point to executable code)
- **M2 Live Smoke Verification Targets against `/usr/bin/amiberry`:**
  1. End-to-end `process-break name <target.exe>` → `segments` → `Z0` → `m dff000,200` → `watch range ... 1` (non-breaking blit log) round-trip against `/usr/bin/amiberry`.
  2. Live parsing of `m68k_disasm_2` output across branch `(T)/(F)`, EA `== $addr`, `movem`, and optional SegTracker header lines.
  3. In-process `adflib` `.adf` boot with `-s uaeboard=1` verifying both `process-break` and Trap 86 `guest-output`.

---

## 4. Source Editing, Disassembly, Register & Hardware Debugging Tools

All debugger views refresh **strictly on stop** or **`[ ↻ SNAPSHOT ]`** and carry explicit provenance tags (`AGENTS.md §8`).

### 4.1 Source Editing Tools (`ide_buffer` + `amiga_decode.c` + `amiga_symbols.c`)
- **Line-by-Line 68000 Static Cycle Gutter (Column 7):** Displays base `n(r/w)` (`18(3/0)`), data-dependent ranges (`38–70(1/0)` for `mulu`, `6+2n(1/0)` for dynamic shifts), taken/not-taken branch pairs (`10/14`), and blank for labels/directives/macros/C lines. Explicitly **line-by-line only** (no subroutine or loop multiplier heuristics in v1).
- **Selection Cycle Sum:** Selecting 2+ ASM/disasm lines calculates the exact sum across the selected lines (`SEL: 3 ln · 42–74(7/0) cyc`) in the status bar.
- **Rich Hover & Literal Inspector:** Hovering tokens shows:
  - `$DFFxxx` hardware register summary, RW/WO permissions, bitfields, and **live snapshot value** when paused (`m dff000,200`).
  - Exec / DOS / Graphics LVO annotations (`jsr -$84(a6)` → `exec.library:Forbid()`).
  - Instant numeric conversion (`#$3FCA` = `16330` dec · bin · `$FCA` minterm `(A&B)|(C&~B)`, or live **OCS RGB12 swatch** on `#$0ABD` / `COLORxx`).
  - Live value / relocated address for registers (`D0..A7`) and symbols when paused.
- **Navigation & Source Debug Actions:** Symbol outline (`Space f s`: sections, `EQU`/`=`, labels, macros, C functions), clickable build diagnostics, **`Run to Cursor`** (`Ctrl+F10`), **`Set PC Here`** (`Ctrl+Shift+F10`), **`Go to Disassembly at Line`**, **`Conditional Breakpoint`** (`Shift+F9` → `qRcmd,condition add`), and **`K`** HRM lookup.

### 4.2 Live Motorola Disassembly View (`amiga_disasm_view.c`)
- Feeds from Amiberry `qRcmd,disasm <addr> 100` parsed + enriched with line-by-line `n(r/w)` cycles, `$DFFxxx` hardware names, Exec/DOS/Graphics LVO labels, and relocated symbol labels.
- **Tools:** **`Go to Address / Symbol / Register`** (`Ctrl+G`), **`Enter` to Follow Branch / `JSR` / `BSR` / Target EA** with **`Alt+Left` / `Alt+Right` navigation history stack**, **`Follow PC`** lock toggle, clickable `F9` `Z0` breakpoints, `Run to Cursor`, `Set PC Here`, **Selection Cycle Sum**, **Inline Opcode Patch** (`M` write word when paused), **Copy Disasm Range**, and **`📜 TRACE HISTORY (128)`** drawer toggle backed by `qRcmd,history on|read`.

### 4.3 CPU Registers, Exceptions & Savestate Checkpoints (`amiga_cpu_strip.c`)
- **48px Register & Beam Strip:** `D0–D7`, `A0–A7`, `PC` (`symbol+offset`), amber diff highlights since last stop, and `VPOS`/`HPOS`/frame + exact **`+N CYC` delta** (`execution-status.cycles`).
- **Interactive Controls:** Inline value editing (`G` packet) and **clickable `SR` flag bit pills (`T S I X N Z V C`)** to flip bits while paused; multi-format hover (`Hex / Signed / Unsigned / Bin / ASCII` on `Dn`, 16-byte deref preview + memory region on `An`) with 1-click `Open in Memory Hex` or `Open in Disasm`.
- **Guru / Exception Traps & Checkpoints:** Bottom-drawer toggles for **`exception mask`** (`Bus #2`, `Addr #3`, `Illg #4`, `Div0 #5`, `Priv #8`) decoding `exception last` (`VBR`, `USP`/`ISP`/`MSP`, fault address/access), plus **`💾 CP` / `⏮ CP`** (`checkpoint save/restore`) for instant loop/effect re-testing.

### 4.4 Memory Hex Inspector (`amiga_mem_view.c`)
- **Grouping & Diffs:** Segmented **`[ 8b | 16b | 32b ]`** word width + ASCII column, amber highlight on bytes changed since last stop, inline hex/ASCII edit (`M`), and expression/register jump bar (`COP1LC`, `BPL1PT`, `BLTDPT`, `PC`, `A7`, `A0+$40`).
- **Search, Pins, Watches & Export:** Hex/ASCII pattern find (`Ctrl+F`), **`📌 FOLLOW REG`** pin + multi-tab snapshots, context menu to install **CPU watchpoints (`Z2–Z4`)**, **non-breaking log watches** (`watch range ... 1`), and **DMA watchpoints** (`dma-watch add`), **`Open in Bitplanes / Copper / Disasm`**, and **`💾 DUMP .BIN`** raw RAM export.

### 4.5 Live Bitplane, Copper, Chips & DMA Budget Inspectors
Every hardware card renders one explicit provenance pill:
- **`[ LIVE ]` (Green):** Readable hardware registers (`DMACONR $002`, `VPOSR $004`, `VHPOSR $006`, `CLXDAT $00E`, `ADKCONR $010`, `INTENAR $01C`, `INTREQR $01E`).
- **`[ EMU SNAPSHOT ]` (Cyan):** Full 512-byte `save_custom(..., full=1)` custom bank read via **`m dff000,200`** — powers Bitplanes (`BPL1PT..8PT`, `BPLCON0..4`, `BPL1/2MOD`, `COLOR00..31`), Copper (`COP1LC`, `COP2LC`, `COPCON`), Display (`DIW*`, `DDF*`), Sprites (`SPR0PT..7PT`, `SPRxPOS/CTL`), and active Blitter state (`BLTCON0/1`, `BLTxPT`, `BLTxMOD`, `BLTSIZE/V/H`, `BLTAFWM/LWM`).
- **`[ WATCHED ]` (Amber):** Opt-in **`[ 👁 TRACE BLITS ]`** (`watch range dff040 36 2 ff ff ffffffff 0 0 0 1` — `log_only=1` non-breaking 64-hit ring buffer) and **`[ 👁 WATCH $DFF080 ]`** (`watch range dff080 8 2 ... 1`) or `dma-watch`.
- **`[ WO — UNAVAILABLE ]` (Dim):** Honestly reserved for registers `save_custom()` omits (`AUD0–3` `$0A0–$0DF` and AGA 24-bit low nibbles / colors 32–255).

- **Bitplane Viewer (`amiga_bitplane_view.c`):** Source `[ EMU SNAPSHOT (m dff000,200) | SYMBOL | MANUAL $ADDR ]`, Depth (`1..6 BPL`, `HAM6`, `EHB`), Layout (`PLANAR`, `INTERLEAVED`, `LINE×2`), `[P0..P5]` solo toggles, `16px` word grid, `FRONT/BACK` buffer flip, interactive 32-swatch palette, live cursor probe (`X, Y | IDX | $RGB | CHIP $addr bN`), and `[ 💾 PNG ]`.
- **Copperlist Disassembler (`amiga_copper_view.c`):** Reads `COP1LC`/`COP2LC` directly from `m dff000,200`, plus Manual `$Addr`, `[ 🔎 SCAN COP ]`, `[ 👁 WATCH $DFF080 ]`, `BFD=0` blitter sync warnings, `$FFDFFFFE` line-255 barriers, `$040–$07E` `COPCON` checks, and `ALL`/`WAITS`/`BLITTER`/`DISPLAY` filters.
- **Parameterized PAL/NTSC DMA Budget (`amiga_dma_view.c`):** 227-color-clock Cairo scanline bar + user-editable preset/slider calculator computing exact odd-slot margin and 7.09 MHz 68000 headroom.
- **Bundled HRM Docs (`KajoMarkdownView`):** Configurable `[docs].hrm_dir`, `K` instant lookup on `$DFFxxx` or mnemonic.

---

## 5. Aero + Metro UI Layout & Deterministic Niri Reflow

Strictly `0px` corner radius, `@metro-surface-*` dark ladder, `1px solid @metro-border` hairlines, `1px solid @metro-accent` window border, `Fira Code` telemetry/chrome:
- **Wide ($\ge 1280\text{px}$):** 3 panes (`220px` Left Sidebar + Center Source/Disasm + `448px` Hardware Inspector).
- **Split Column ($900..1279\text{px}$):** Left Sidebar collapses behind `[🌲 TREE]` toggle (`Space e`); Center + Inspector stay side-by-side.
- **Half-Screen Niri Split ($< 900\text{px}$, e.g. `814px` beside Amiberry):** Left Sidebar collapses **and** Right Inspector collapses into **`[ 📝 CODE / DISASM | 🔧 INSPECTOR ]`** so neither the 7-column editor nor the bitplane/Copper view is crushed into `< 500px`, and the CPU strip wraps cleanly with zero clipping (`min-width <= 480px`).
- **UI Gallery & Styling:** New `data/defaults/styles/85-amiga.css` (`@import`ed in `style.css`), plus specimens in [`docs/ui-gallery.html`](ui-gallery.html) and [`docs/design_system.md`](design_system.md).

---

## 6. Configuration (`amiga.toml` + `kajo-amiga.toml`), Keybindings & Desktop Integration

### 6.1 Global Suite Configuration (`~/.config/kajo/amiga.toml`) & `kajo-settings --page amiga`

Just like `kajo-ide` (`page_ide.c`), `kajo-mod` (`page_mod.c`), and `kajo-web` (`page_web.c`), `kajo-amiga` is configured through a dedicated modular settings page in **`kajo-settings`**:

- **Registration & In-App Launch:**
  - `src/settings/pages/page_amiga.c` (`build_page_amiga()`), registered in `src/settings/settings_window.c` (`amiga`, `13 / 14 • KAJO AMIGA`, shifting `about` to `14 / 14`), `src/settings/settings_pages.h`, and `meson.build`.
  - Clicking **`⚙`** in the `kajo-amiga` header bar (or pressing `Ctrl+,` / `Space ,`) launches `kajo-settings --page amiga` via `kajo_spawn_detached()`.
  - `kajo-amiga` watches `~/.config/kajo/amiga.toml` via `GFileMonitor` (and `ide.toml` when editor inheritance is active) and hot-reloads preferences live without restarting.
- **Persistence & Controls (`KajoAmigaCfg` in `src/config/kajo_config_{types,keys,store}.{h,c}` + `src/config/schemas/schema_amiga.c`), using `AGENTS.md §3.4` widget selection + explicit `SAVE & APPLY` / `REVERT`:**
  - **Emulator, Profiles & ROMs:** `amiberry_bin` (path + live version/`gdbserver` probe badge), `default_profile` (`a500_ocs` | `a500p_ecs` | `a1200_aga` | `a1200_fast`), `default_video` segmented (`PAL` | `NTSC`), `kickstart_a500` / `kickstart_a1200` ROM pickers, `default_boot_mode` segmented (`DH0:` | `ADF`), `gdb_host` (`127.0.0.1`), `gdb_port` (`2345`), and `adf_uaeboard_guest_output` switch.
  - **Toolchain & Built-In Packager:** `default_toolchain` segmented (`elf2hunk` | `hunk_vlink` | `single_asm`), `extra_bin_paths` editable Add/Remove list, live probe badges (`amiberry`, `vasmm68k_mot`, `m68k-amiga-elf-gcc`, `m68k-amiga-elf-ld`, `elf2hunk`, `vlink`, `m68k-lsp-server`, `clangd`, plus built-in **`adflib`** version badge), and `hrm_dir` path picker.
  - **Editor & Disassembly:** `inherit_ide_editor_prefs` toggle card (default `true` — inherits font family/size, GtkSourceView scheme, tab width, and modal remaps from `KajoIdeCfg` `ide.toml`, with a jump button to `10 / 14 • KAJO IDE`), `show_cycle_gutter` switch (`true`), `show_selection_cycle_sum` switch (`true`), `enable_hover_decoders` switch (`true`), and `enable_m68k_lsp` / `enable_clangd` switches (`true`).
  - **Debugger & Inspector Defaults:** `entry_break_mode` segmented (`Break at Entry` | `Run to BP`), `capture_guest_output` switch (`true`), `enable_trace_history` switch (`false`), `default_exception_mask` toggle pills (`Bus #2`, `Addr #3`, `Illegal #4`, `Div0 #5`, `Priv #8`), `trace_blitter_writes` toggle card (default `false`, `log_only=1`), `default_hex_grouping` segmented (`8b` | `16b` | `32b`), `bitplane_grid_16px` switch (`false`), and `inspector_visible` switch (`true`).

### 6.2 Per-Project Manifest (`kajo-amiga.toml`)
Optional for Attach Mode; in Project Mode defines `[project]` (`name`, `build_mode = "native" | "external"`, `default_target`, `default_profile`, `shared_sources`), `[[target]]` (`name`, `kind = "os_exe" | "adf_dos" | "adf_raw"`, `cpu`, `toolchain`, `sources`, `includes`, `defines`, `extra_files`, `copper_symbol`, `bitplane_symbol`, `entry_addr`), `[[asset]]` (`name`, `inputs`, `outputs`, `cmd`), `[[profile]]` overrides, and `[docs]` (`hrm_dir`).

### 6.3 Shortcuts & Desktop Files
- **Shortcuts:** `Ctrl+Shift+N` (New Project Wizard), `Ctrl+O` (Open Project), `Ctrl+Shift+P` (`[⚙ kajo-amiga.toml]` Project Setup tab), `Space r b/r/c` (Build/Run/Clean), `F5` (Debug/Continue), `Shift+F5` (Stop or Detach), `Ctrl+Shift+A` (`--attach` prompt), `Ctrl+,` / `Space ,` (`kajo-settings --page amiga`), `F9` / `Shift+F9` (Toggle/Cond BP), `F10`/`F11`/`F12` (Step Over/Into/Out), `Ctrl+F10` (Run to Cursor), `Ctrl+G` (Go to Addr/Symbol), `Alt+Left/Right` (Disasm Nav Back/Fwd), `Space f s` (Symbol Outline), `Space a p/c/h/d/m/x` (Bitplanes/Copper/Chips/DMA/Hex/Disasm), `Space a s` (Snapshot), `Space a g` (Cycle gutter), `K` (HRM jump).
- **Desktop & Compositor Files:** `data/org.kajo.Amiga.desktop`, `data/icons/kajo-amiga.svg`, and `data/defaults/rules.toml` tiling rules (`org.kajo.Amiga` + optional `amiberry` non-floating companion tiling).

---

## 7. Milestones & Explicit v1 Non-Goals (`AGENTS.md §8`)

### 7.1 Phased Milestones
1. **M1 — Shared Core Extraction (`libkajo_ide_core`), `adflib` Subproject & GTK-Free Project/Build/Decoders:**
   - Extract `IdeEditorHost` seam from `ide_actions.c` and generic column-7 annotation callback from `ide_gutter.c`; keep all existing `kajo-ide` tests green.
   - Wire `dependency('adflib')` + Meson CMake `subprojects/adflib.wrap` fallback in `meson.build`.
   - Implement `amiga_project.{h,c}`, `amiga_build.{h,c}` (Option D native DAG + `--build` CLI + external fallback + `compile_commands.json` + `SDKDIR` discovery), `amiga_adf.{h,c}` (`adf_dos` via `adflib` + `adf_raw`), `amiga_decode.{h,c}`, and GTK-free unit tests (`test_amiga_decode`, `test_amiga_project`, `test_amiga_build`, `test_amiga_adf`).
2. **M2 — Amiberry 8.5.0 RSP Client, Symbol Relocation, Disasm & Live Smoke Test:**
   - Implement `amiga_runner.{h,c}`, `amiga_rsp.{h,c}` (including `vCont;r`), `amiga_symbols.{h,c}` (ELF/DWARF3 + Hunk + C line ranges), `amiga_disasm.{h,c}`, mock Amiberry 8.5.0 RSP server (`tests/mock_amiberry_rsp.py`), and ASan/UBSan tests (`scripts/asan-test.sh`).
   - Run live smoke tests against `/usr/bin/amiberry`: `process-break name`, `segments`, `Z0`, `m dff000,200`, `disasm`, `watch range ... 1`, `vCont;r`, and `adflib`-built `.adf` with `-s uaeboard=1` `guest-output`.
3. **M3 — Window Shell, Project UI, CPU Strip, Source/Disasm Views & Debug/Hex Drawer:**
   - Implement `main.c`, `amiga_window.c`, `amiga_project_ui.c` (New Project Wizard + 4-tab Project Setup), `amiga_disasm_view.c`, `amiga_cpu_strip.c`, `amiga_mem_view.c`, line-by-line cycle gutter + selection sum, rich hover, exception/checkpoint controls, `85-amiga.css`, Project + Attach mode switching, and `< 900px` Niri `[ CODE | INSPECTOR ]` reflow.
4. **M4 — Hardware Visualizers & HRM Integration:**
   - Implement `amiga_bitplane_view.c`, `amiga_copper_view.c` (`m dff000,200` + `SCAN COP` & `WATCH $DFF080`), `amiga_chips_view.c`, `amiga_dma_view.c`, and `K` HRM lookup.
5. **M5 — Suite Config, `kajo-settings` Page, Gallery, Desktop Rules & Visual Verification Gate:**
   - Wire `KajoAmigaCfg` (`schema_amiga.c`), `src/settings/pages/page_amiga.c` + `settings_window.c` (`--page amiga`), `GFileMonitor` hot-reload, `data/org.kajo.Amiga.desktop`, `data/icons/kajo-amiga.svg`, `rules.toml`, `docs/ui-gallery.html` + `docs/design_system.md`, full `meson compile && meson test`, and `scripts/ui-shot.sh` visual inspection across all preview modes (including `kajo-settings --page amiga`).

### 7.2 Explicit v1 Non-Goals (`AGENTS.md §8`)
- **No Decompiler:** Decompilation belongs to the standalone external `hunks` tool.
- **No External `hunks` / `xdftool` Runtime Dependency & No Embedded Amiga VM:** `kajo-amiga` is 100% self-contained (`adflib` linked cleanly in-process; stock **Amiberry $\ge$ 8.5.0** over standard GDB RSP).
- **DWARF C Local Variables & Call-Stack Unwinding Deferred to v1.5:** v1 C support covers editing (`Tree-sitter c` + `clangd` via `compile_commands.json`), building (`m68k-amiga-elf-gcc` / optional `vc`), DWARF3 `.debug_line` breakpoints, `vCont;r` statement stepping, and global/typed watches. DWARF `.debug_info` local-variable/struct evaluation and `.debug_frame`/`A5` call-stack unwinding are deferred to v1.5 (not faked).
- **No Static Subroutine/Loop Cycle Estimator:** Source and Disasm show line-by-line 68000 `n(r/w)` timings and multi-line selection sums without whole-function call-graph unrolling or loop trip-count guessing.
- **No Custom Amiberry Fork & No Bartman Command-88 Profiler:** Uses stock Amiberry; provides static `n(r/w)` gutters, parameterized DMA slot budgets, and exact `execution-status.cycles` stop deltas.
- **No Tracker / ProTracker Editor:** Audio module authoring belongs to `kajo-mod`.
- **No Live Continuous Polling While Running:** Keeps emulation cycle-accurate and idle CPU `< 0.05%`.
