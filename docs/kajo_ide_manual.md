# Kajo IDE (`kajo-ide`) — User & Architecture Manual

**Binary:** `kajo-ide` (`org.kajo.Ide`)  
**Design System:** Kajo Aero + Metro (`0px` sharp geometry, high-contrast `#111111`–`#1a1a1a` dark surfaces, `Fira Code` monospace typography)  
**Settings Integration:** `kajo-settings` $\rightarrow$ **Kajo IDE & Code** (`~/.config/kajo/ide.toml`)

---

## 1. Overview & Philosophy

**Kajo IDE (`kajo-ide`)** is the native GTK4 / `GtkSourceView-5` modal code editor and integrated development environment for the Kajo Desktop Suite. Designed as the graphical evolution of `koodieditori`, it combines:

1. **Selection-First Modal Editing** — A deterministic 10-state finite state machine (`NORMAL`, `SELECT`, `INSERT`, `LEADER`, `MATCH`, `SEARCH`, `PICKER`, `TREE`, `DRAWER`, `HELP`) where motions highlight selections first and actions (`d`, `c`, `y`, `s`) operate on the active selection.
2. **Nordic Keyboard Ergonomics** — First-class bindings for Finnish/Swedish keyboards (`ä` / `ö` diagnostic & hunk stepping, `m i/a` structural text objects that avoid Dead-Key / AltGr finger gymnastics, and `jk` fast-exit from `INSERT` mode) alongside standard desktop shortcuts (`Ctrl+S`, `Ctrl+Z`, `Ctrl+Y`, `Ctrl+C/X/V/A`, `F1`, `F5`–`F12`).
3. **Zero-Config Language Server (LSP 3.17) & Debugger (DAP)** — Asynchronous JSON-RPC 2.0 clients for `clangd`, `gopls`, `rust-analyzer`, and `pyright`, plus native Debug Adapter Protocol support for `gdb -i=dap`, `lldb-dap`, and `dlv dap`.
4. **Native GUI Markdown & Vector Cairo Mermaid Diagrams** — Real-time side-by-side graphical Markdown preview with interactive task checkboxes, GitHub-flavored callout alerts (`[!NOTE]`, `[!TIP]`, `[!IMPORTANT]`, `[!WARNING]`, `[!CAUTION]`), and a native C11 Cairo/Pango layout engine for Mermaid diagrams (`flowchart`, `sequenceDiagram`, `stateDiagram-v2`, `classDiagram`, `erDiagram`, `pie`) with interactive pan/zoom and `.svg` export.
5. **AI Agent JSON-RPC 2.0 Socket & Atomic Batch Diff Review** — External coding agents can query buffers, diagnostics, and apply multi-file edits over `$XDG_RUNTIME_DIR/kajo-ide-<pid>.sock`, while the developer reviews, accepts (`Space d a`), or reverts (`Space d r`) every AI-modified buffer with inline gutter diff markers.

---

## 2. Launching `kajo-ide`

```bash
# Open current working directory as workspace root
kajo-ide

# Open a specific project directory
kajo-ide ~/src/Kajo

# Open one or more files directly
kajo-ide src/ide/ide_window.c docs/kajo_ide_manual.md
```

When launched with a file path, `kajo-ide` automatically detects the enclosing Git repository root (or parent directory) to populate the **Project File Explorer** and **Git Status** badges.

---

## 3. Window Anatomy

```
┌──────────────────────────────────────────────────────────────────────────────────────────────────┐
│ ⌨ KAJO IDE  [🌲 TREE][🗺 MAP][📖 MD][🔍 FIND][❓ HELP]   src/ide/ide_window.c [⎇ main ●2]  [🗗][✕] │
├────────────────────────┬─────────────────────────────────────────────────────────────────────────┤
│ EXPLORER     [a][A][r] │ [ ● ide_window.c × ] [ ide_fsm.c × ] [ kajo_ide_manual.md × ]           │
│ ▾ 📁 src               ├────────┬───────────────────────────────────────────────────────┬────────┤
│   ▾ 📁 ide             │ ●▎ 42  │ static void update_status_bar(KajoIdeWindow *self) {  │ ▒▒▒▒▒▒ │
│     📄 ide_fsm.c     M │  │ 43  │   IdeState st = ide_fsm_get_state(self->fsm);         │ ▒▒▒▒▒▒ │
│     📄 ide_picker.c  M │ ◆│ 44  │   gtk_label_set_text(GTK_LABEL(self->mode_pill), ...  │ ███▒▒▒ │
│     📄 ide_window.c  M │  │ 45  │ }                                                     │ ▒▒▒▒▒▒ │
│   ▸ 📁 settings        │  │ 46  │                                                       │        │
├────────────────────────┴────────┴───────────────────────────────────────────────────────┴────────┤
│ [ 🐞 DEBUGGER (DAP) ] [ ⚠ DIAGNOSTICS ] [ 🔍 SEARCH & BUILD ] [ 📖 MARKDOWN ]              [✕]   │
│  ▸ Call Stack                   │ ▸ Local Scopes (Variables)  │ ▸ Debug Console / REPL          │
├──────────────────────────────────────────────────────────────────────────────────────────────────┤
│  NORMAL  src/ide/ide_window.c [+]   Space → _                    LSP: clangd ●   UTF-8  C  44:12 │
└──────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Three-Zone Window Header Bar
- **Left Cluster:** Application title (`⌨ KAJO IDE`) and quick-toggle pills:
  - `🌲 TREE` (`Space e`) — Focus or toggle the Project File Explorer.
  - `🗺 MAP` — Toggle the right-hand `GtkSourceMap` code minimap.
  - `📖 MD` (`Space m p`) — Toggle the 50/50 side-by-side Graphical Markdown & Mermaid Preview pane.
  - `🔍 FIND` (`Space f f`) — Open the Fuzzy File Finder modal.
  - `❓ HELP` (`?` / `F1` / `Space h`) — Open the full-width Interactive Help Viewer & Command Palette.
- **Center Cluster:** Active workspace-relative file breadcrumb (`src/ide/ide_window.c`), live Git branch & dirty counter badge (`⎇ feat/kajo-ide ●3 +1`), and active AI Agent modification indicator (`🤖 AGENT MODIFIED`).
- **Right Cluster:** `🔨 BUILD` quick-trigger (`meson compile -C builddir`), Niri `🗗 FLOAT / TILE` window toggle, and `✕` close button.

### 3.2 Six-Column Cairo Editor Gutter (`68px`)
Every open buffer renders a custom hardware-accelerated 6-column gutter (`src/ide/ide_gutter.c`) to the left of the source view:
1. **Col 1 — Breakpoint Slot (`12px`):** Click to toggle DAP breakpoints (`●` crimson `#e81123`) or view the active instruction pointer arrow (`▶` amber `#ffb900`).
2. **Col 2 — LSP Diagnostic Pill (`12px`):** Displays `●` Error (`#e81123`), `▲` Warning (`#ff8c00`), or `◆` Hint/Info (`#00d8f0`).
3. **Col 3 — Git Diff Bar (`4px`):** Vertical bar showing uncommitted line status against `HEAD` (`#10893e` Added, `#0078d4` Modified, `#e81123` Deleted).
4. **Col 4 — AI Agent Diff Bar (`4px`):** Vertical bar (`#b4009e` magenta / `#00d8f0` cyan) highlighting lines modified by an external AI agent awaiting review.
5. **Col 5 — Line Number (`28px`):** Right-aligned `Fira Code` line number (supports `absolute`, `relative`, or `none` via `kajo-settings`).
6. **Col 6 — Code Folding Chevron (`8px`):** Indicates foldable AST/brace blocks (`▾` / `▸`, toggled with `z a`).

### 3.3 Ephemeral Bottom Drawer & Switcher Strip
When diagnostics, search/replace, build output, or debugging is triggered, the bottom drawer slides open with a top `@metro-accent` border and four switcher tabs:
- **`🐞 DEBUGGER (DAP)`** — Transport toolbar (`▶ Continue`, `↷ Step Over`, `↓ Step Into`, `↑ Step Out`, `⏹ Stop`) and 3-pane split (Call Stack, Local Scopes, Interactive GDB/DAP REPL).
- **`⚠ DIAGNOSTICS`** — Workspace-wide LSP errors/warnings and symbol references list.
- **`🔍 SEARCH & BUILD`** — Live regex search & project-wide replace bar + asynchronous compiler build output log.
- **`📖 MARKDOWN`** — Bottom-docked Markdown & Mermaid preview (`Space m m`).

---

## 4. Modal State Machine & Editing Model

The bottom-left status pill color-codes the current state of the editor:

| Mode Pill | Color | Entry Key(s) | Exit Key(s) | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **`NORMAL`** | `#0078d4` (Blue) | `Esc`, `jk` (from `INSERT`) | `i`, `a`, `v`, `Space`, `:`, `/` | Default navigation, line/word selection, and structural commands |
| **`SELECT`** | `#b4009e` (Purple) | `v`, `x`, or mouse drag | `Esc`, `d`, `c`, `y` | Extend selection continuously with any motion before mutating |
| **`INSERT`** | `#10893e` (Green) | `i`, `a`, `o`, `O`, `c` | `Esc` or fast `jk` | Direct text entry, live LSP completion, and standard shortcuts |
| **`LEADER`** | `#ff8c00` (Amber) | `Space` (in `NORMAL`/`SELECT`) | `Esc` or chord completion | Prefix for two- and three-key workspace chords (`Space f f`, etc.) |
| **`MATCH`** | `#ff8c00` (Amber) | `m` (in `NORMAL`/`SELECT`) | `Esc` or object key | Nordic structural text-object selector (`m i f`, `m a c`, `m m`) |
| **`SEARCH`** | `#00b7c3` (Teal) | `/` | `Enter` (commit), `Esc` (cancel) | Incremental regex search across the active buffer |
| **`PICKER`** | `#0078d4` (Blue) | `Space f f`, `Space b b`, `Space f g` | `Esc`, `Enter` | Modal Fuzzy File Finder, Buffer Switcher, or Live Ripgrep |
| **`TREE`** | `#00b7c3` (Teal) | `Space e` | `Esc`, `Space e`, `Enter` | Keyboard navigation and file CRUD inside the Project File Explorer |
| **`DRAWER`** | `#ff8c00` (Amber) | `Space x x`, `Space s r`, `F5` | `Esc`, `q` | Keyboard navigation inside the bottom Diagnostics/Search/DAP drawer |
| **`HELP`** | `#00d8f0` (Cyan) | `?`, `F1`, `Space h` | `Esc`, `F1`, `q` (empty query) | Interactive Help Viewer & Command Palette |

> [!TIP]
> **Fast `jk` Escape from `INSERT` Mode:** Typing `j` followed immediately by `k` (within `200ms`) in `INSERT` mode exits back to `NORMAL` mode without reaching for `Escape`.

---

## 5. Complete Keybinding Reference

### 5.1 Basic Navigation & Scrolling (`NORMAL` & `SELECT`)

| Key / Chord | Command | Description |
| :--- | :--- | :--- |
| `h` / `j` / `k` / `l` | Move Left / Down / Up / Right | Character and line navigation (arrow keys also work in all modes) |
| `w` / `b` / `e` | Word Forward / Back / End | Move and select by word boundaries |
| `g h` / `Home` / `0` | Line Start | Jump to column 0 of the current line |
| `g l` / `End` / `$` | Line End | Jump to the end of the current line |
| `g s` / `^` | First Non-Blank | Jump to the first non-whitespace character on the line |
| `g g` | File Top | Jump to line 1 of the buffer |
| `G` | File Bottom | Jump to the last line of the buffer |
| `Ctrl+d` / `PgDn` | Half-Page Down | Scroll down half a viewport |
| `Ctrl+u` / `PgUp` | Half-Page Up | Scroll up half a viewport |
| `z z` | Center Cursor | Vertically center the active cursor line in the viewport |
| `z a` | Toggle Fold | Fold or unfold the current brace/AST block |
| `Ctrl+o` | Jump Back | Navigate backward in the cross-file jump history stack |
| `Ctrl+i` | Jump Forward | Navigate forward in the cross-file jump history stack |
| `n` / `N` | Next / Prev Search Match | Cycle forward or backward through `/` regex matches |

### 5.2 Nordic Quick-Step Navigation (`ä` / `ö` & Bracket Equivalents)

Designed for zero-Dead-Key navigation on Nordic layouts (with `[` / `]` brackets also supported):

| Key / Chord | Command | Description |
| :--- | :--- | :--- |
| `ä` / `]d` | Next Diagnostic | Jump to the next LSP Error/Warning in the buffer |
| `ö` / `[d` | Previous Diagnostic | Jump to the previous LSP Error/Warning in the buffer |
| `Alt+ä` / `]g` | Next Git Hunk | Jump to the next uncommitted Git diff hunk |
| `Alt+ö` / `[g` | Previous Git Hunk | Jump to the previous uncommitted Git diff hunk |

### 5.3 Selection, Editing & Clipboard (`NORMAL`, `SELECT`, `INSERT`)

| Key / Chord | Command | Description |
| :--- | :--- | :--- |
| `i` / `a` | Insert / Append | Enter `INSERT` mode before or after the cursor |
| `I` / `A` | Insert at Start / End of Line | Enter `INSERT` mode at first non-blank or end of line |
| `o` / `O` | Open Line Below / Above | Insert a new indented line below or above and enter `INSERT` mode |
| `v` | Toggle `SELECT` Mode | Toggle continuous selection extension mode |
| `x` | Select Line | Select the entire current line; press `x` repeatedly to extend downward |
| `%` | Select All | Select the entire buffer |
| `s` | Select Inside Matching | Select regex matches inside the active selection |
| `d` | Delete Selection | Delete the active selection (or current character) into the clipboard |
| `c` | Change Selection | Delete the active selection and immediately enter `INSERT` mode |
| `y` | Yank (Copy) | Copy the active selection to the system Wayland clipboard |
| `p` / `P` | Paste After / Before | Paste system clipboard contents after or before the cursor |
| `J` | Join Lines | Join the current line with the line below |
| `>` / `<` | Indent / Outdent | Shift selected lines right or left by one tab stop |
| `u` / `Ctrl+z` | Undo | Undo the last buffer modification (works in both `NORMAL` and `INSERT`) |
| `U` / `Ctrl+r` / `Ctrl+y` | Redo | Redo the last undone modification |
| `Ctrl+s` | Save File | Write the current buffer to disk |
| `Ctrl+c` / `Ctrl+x` / `Ctrl+v` | Copy / Cut / Paste | Standard desktop clipboard shortcuts (active in all modes) |
| `Ctrl+a` | Select All | Select the entire buffer |

### 5.4 Nordic Structural Text Objects (`m` Match Mode)

Pressing `m` enters `MATCH` mode (`#ff8c00` pill), allowing structural selection without awkward `AltGr` brace chords:

| Chord | Target Structure | Scope |
| :--- | :--- | :--- |
| `m m` | Matching Bracket `()` / `[]` / `{}` | Jump cursor to matching delimiter |
| `m i f` / `m a f` | **Function** (`func`, `fn`, `def`, C/C++ block) | Inside (`i`) or Around (`a`) function body |
| `m i c` / `m a c` | **Class / Struct / Type** | Inside (`i`) or Around (`a`) struct/class definition |
| `m i b` / `m a b` | **Block / Braces `{ ... }`** | Inside (`i`) or Around (`a`) curly brace block |
| `m i a` / `m a a` | **Argument / Parameter** | Inside (`i`) or Around (`a`) comma-separated argument |
| `m i p` / `m a p` | **Paragraph** | Inside (`i`) or Around (`a`) contiguous text paragraph |
| `m i q` / `m a q` | **Quoted String** (`"..."`, `'...'`, `` `...` ``) | Inside (`i`) or Around (`a`) string literal |
| `m i w` / `m a w` | **Word** | Inside (`i`) or Around (`a`) identifier word |

*Example:* Press `m i f` to select the body of the current C function, then press `d` to delete it or `y` to copy it.

### 5.5 `Space` Leader Chords

Pressing `Space` in `NORMAL` or `SELECT` mode opens the **Bottom-Right Chord HUD** displaying all available completions:

| Chord | Category | Action |
| :--- | :--- | :--- |
| `Space f f` | Files | Open **Fuzzy File Finder** across workspace |
| `Space f g` | Search | Open **Live Ripgrep (`rg`)** project-wide text search |
| `Space b b` | Buffers | Open **Active Buffer Switcher** |
| `Space w` | Files | Save active buffer to disk (`:w`) |
| `Space q` | Window | Close buffer / Quit IDE (`:q`) |
| `Space e` | Explorer | Focus or toggle the **Project File Explorer** sidebar |
| `Space h` / `?` / `F1` | Help | Open the **Interactive Help Viewer & Command Palette** |
| `Space c a` | LSP | Trigger **LSP Code Actions** (`quickfix` / refactor) |
| `Space c r` | LSP | Trigger **LSP Rename Symbol** across workspace |
| `Space c f` | LSP | Format current buffer via LSP (`clang-format` / `gofmt` / `rustfmt`) |
| `Space g c` | LSP | Inspect **LSP Incoming / Outgoing Call Hierarchy** |
| `Space x x` | Diagnostics | Open **Workspace Diagnostics Drawer** |
| `Space s r` | Search | Open **Buffer Search & Replace** in bottom drawer |
| `Space s R` | Search | Open **Project-Wide Search & Replace** in bottom drawer |
| `Space m p` | Markdown | Toggle **Side-by-Side Graphical Markdown & Mermaid Split** |
| `Space m m` | Markdown | Toggle **Bottom-Docked Markdown Preview Drawer** |
| `Space m t` | Markdown | Toggle `- [ ]` / `- [x]` **Markdown Task Checkbox** on current line |
| `Space d b` | Debugger | Toggle **DAP Breakpoint** on current line (same as `F9`) |
| `Space d c` | Debugger | Start or Continue **DAP Debugger Session** (same as `F5`) |
| `Space d s` | AI Agent | Open **Agent Diff Review** for AI-modified buffers |
| `Space d a` | AI Agent | **Accept All** pending AI agent edits in current buffer |
| `Space d r` | AI Agent | **Revert All** pending AI agent edits in current buffer to pre-agent snapshot |

---

## 6. Project File Explorer & Git Status (`Space e`)

The left-hand **Project File Explorer** (`src/ide/ide_tree.c`) provides a scroll-stable tree view of your workspace with live `git status --porcelain=v1 -b` integration:

- **Visual Color Coding:**
  - Folders (`📁`) are highlighted in warm amber (`#dcb67a`), while source files use crisp slate (`#8a9ab0`).
  - Modified files tint their filename label and display a sharp 1px boxed badge:
    - `M` (`#ff8c00` Amber) — Modified in working tree or index
    - `A` (`#10893e` Green) — Added to Git index
    - `U` (`#00d8f0` Cyan) — Untracked file
    - `D` (`#e81123` Crimson) — Deleted
- **Mouse & Keyboard Navigation:**
  - **Single-Click** selects a row without scrolling or jumping the viewport.
  - **Double-Click** or `Enter` / `l` expands/collapses a directory in place (preserving exact vertical scroll position) or opens a file into the editor.
  - **In-Place File CRUD (when focused via `Space e` or header toolbar buttons):**
    - `a` — Create a new file inside the selected directory (inline entry prompt).
    - `A` — Create a new folder (`mkdir -p`) inside the selected directory.
    - `r` — Rename the selected file or directory in place.
    - `d` — Delete the selected file or directory.
    - `h` — Collapse the parent directory.
    - `Esc` / `Space e` — Return focus to the code editor.

---

## 7. Fuzzy Finder, Live Ripgrep & Interactive Help Palette

`kajo-ide` includes a multi-mode overlay modal (`src/ide/ide_picker.c`) styled with sharp `0px` Aero + Metro borders:

### 7.1 Fuzzy File Finder (`Space f f`), Buffer Switcher (`Space b b`), and Live Ripgrep (`Space f g`)
- Displays a **2-pane split**:
  - **Left Pane (`420px`):** Ranked fuzzy file matches or asynchronous `rg --vimgrep` search results.
  - **Right Pane:** Live syntax-highlighted `GtkSourceView` preview centered on the target line (`PREVIEW: <file>:<line>`).
- **Navigation:** `↑` / `↓`, `Tab` / `Shift+Tab`, `Ctrl+n` / `Ctrl+p`, or mouse click to select; `Enter` or double-click to open the file at the exact line/column.

### 7.2 Interactive Help Viewer & Command Palette (`?`, `F1`, `Space h`, or `❓ HELP`)
- Displays a **full-width (`920px`) 4-column interactive reference table** (`CHORD / KEYBINDING`, `COMMAND`, `CATEGORY`, `ACTION DESCRIPTION`) covering all **85 commands** across 11 categories.
- **Category Filter Pills:** Click any category pill (`ALL`, `NAVIGATION`, `SELECT & EDIT`, `OBJECTS`, `LEADER`, `FILE TREE`, `LSP`, `DEBUGGER`, `MARKDOWN`, `AGENT`, `LAYOUT`, `MOUSE`) to filter the table by subsystem.
- **Instant Fuzzy Filter:** Type any key chord, command name, or description keyword to filter rows in real time.
- **Executable Command Palette:** Pressing `Enter` or double-clicking any row immediately executes that command in the editor.
- **Dismissing:** Press `Esc`, `F1`, click `✕ ESC`, or press `q` when the search query is empty (typing `q` when a filter query is already active appends `q` to the search string so you can search for `"quote"` or `"quickfix"`).

---

## 8. LSP 3.17 Code Intelligence

`kajo-ide` automatically spawns the matching language server over asynchronous `GSubprocess` stdio pipes based on buffer filetype (`src/ide/ide_lsp.c`):

| Language(s) | File Extensions | Language Server Binary |
| :--- | :--- | :--- |
| **C / C++ / ObjC** | `.c`, `.h`, `.cpp`, `.hpp`, `.cc` | `clangd --background-index` |
| **Go** | `.go` | `gopls` |
| **Rust** | `.rs` | `rust-analyzer` |
| **Python** | `.py` | `pyright-langserver --stdio` (fallback: `pylsp`) |

### Key LSP Workflows
- **Hover Documentation (`K`):** Opens a floating documentation popover with formatted type signatures and docstrings at the cursor.
- **Autocompletion (`Ctrl+Space` or automatic in `INSERT` mode):** Displays ranked `textDocument/completion` items (`Function`, `Method`, `Variable`, `Struct`, `Snippet`) with `Tab` / `Enter` insertion.
- **Go to Definition (`g d`) & References (`g r`):** Jumps directly to the symbol definition (recording the location in the `Ctrl+o` / `Ctrl+i` jumplist) or populates the bottom Diagnostics/References drawer.
- **Code Actions (`Space c a`) & Rename (`Space c r`):** Applies workspace-wide `WorkspaceEdit` patches across open buffers and disk files.

---

## 9. Interactive DAP Debugger (`F5`–`F12`)

The built-in Debug Adapter Protocol client (`src/ide/ide_dap.c`) connects to native debug adapters (`gdb -i=dap`, `lldb-dap`, `dlv dap`):

| Key / Chord | Transport Button | Action |
| :--- | :--- | :--- |
| **`F9`** / `Space d b` | Gutter Col 1 Click | Toggle source line breakpoint (`●`) |
| **`F5`** / `Space d c` | `▶ Continue` | Launch debug session or continue execution |
| **`F10`** | `↷ Step Over` | Step over the current statement (`next`) |
| **`F11`** | `↓ Step Into` | Step into the called function (`stepIn`) |
| **`F12`** | `↑ Step Out` | Step out of the current stack frame (`stepOut`) |
| **`Shift+F5`** | `⏹ Stop` | Terminate the active debuggee session |

When paused at a breakpoint or signal:
1. The editor jumps to the active stack frame and highlights the execution line with an amber `▶` gutter indicator.
2. The **`🐞 DEBUGGER (DAP)`** bottom drawer opens automatically with three synchronized panes:
   - **Call Stack:** Click any stack frame to inspect its source location and local variables.
   - **Local Scopes:** Inspect local variables, registers, and struct fields.
   - **Debug Console / REPL:** Type expressions in the bottom entry box and press `Enter` to evaluate them via DAP `evaluate`.

---

## 10. Graphical Markdown & Native Cairo Vector Mermaid

Unlike terminal editors that rely on ASCII approximations, `kajo-ide` includes a full graphical Markdown renderer (`src/ide/ide_markdown.c`) and a pure C11 Cairo/Pango vector diagram engine (`src/ide/ide_mermaid.c`):

- **Side-by-Side Split (`Space m p` or `📖 MD` header button):** Opens a live-synchronized 50/50 split beside the editor. Scrolling the source buffer proportionally scrolls the rendered document.
- **Supported Markdown Elements:**
  - Styled headings (`H1`–`H6`) with underline rules and Pango typography.
  - Interactive `- [ ]` / `- [x]` task list checkboxes — clicking a checkbox in the GUI preview mutates the underlying Markdown source line in real time (and adds an undo step).
  - GitHub-Flavored Callout Alerts (`> [!NOTE]`, `> [!TIP]`, `> [!IMPORTANT]`, `> [!WARNING]`, `> [!CAUTION]`) with semantic color-coded left borders.
  - Structured tables (`GtkGrid`) and syntax-highlighted fenced code cards.
- **Native Cairo Vector Mermaid Diagrams (` ```mermaid `):**
  - Renders **Flowcharts (`flowchart TD/LR`, `graph TD/LR`)**, **Sequence Diagrams (`sequenceDiagram`)**, **State Diagrams (`stateDiagram-v2`)**, **Class Diagrams (`classDiagram`)**, **Entity-Relationship Diagrams (`erDiagram`)**, and **Pie Charts (`pie`)** using crisp vector primitives, Bezier edges, and Metro accent palettes.
  - Every diagram card includes an interactive toolbar:
    - **`🔍+` / `🔍-` / `1:1`** — Zoom in/out (`25%` to `400%`) or reset to `100%`, plus `Ctrl + MouseWheel` zoom and middle-click/drag canvas panning.
    - **`💾 SVG`** — Export the diagram directly to a standalone vector `.svg` file via `cairo_svg_surface_create()`.

---

## 11. AI Agent JSON-RPC 2.0 IPC & Atomic Diff Review

External AI coding assistants can pair-program directly with a running `kajo-ide` instance over its Unix domain socket (`src/ide/ide_agent.c`):

- **Socket Path:** `$XDG_RUNTIME_DIR/kajo-ide-<pid>.sock` (also exported to child terminals as `$KAJO_IDE_SOCKET`).
- **Supported JSON-RPC 2.0 Methods:**
  - `ide.get_open_buffers` — List all open buffers, cursor positions, and dirty states.
  - `ide.get_buffer_content` — Read the live in-memory contents of a buffer.
  - `ide.get_diagnostics` — Retrieve current LSP errors and warnings across the workspace.
  - `ide.apply_edits` — Atomically apply single- or multi-file text edits while recording a pre-agent snapshot in each affected `IdeBuffer`.
  - `ide.open_file` — Open and focus a specific file/line in the IDE.
- **Developer Review Workflow:**
  1. When an external agent applies edits via `ide.apply_edits`, the header bar displays a `🤖 AGENT MODIFIED` badge and Col 4 of the editor gutter highlights the changed line ranges.
  2. Press **`Space d s`** to inspect the agent's modifications.
  3. Press **`Space d a`** to accept the changes and clear the snapshot, or **`Space d r`** to atomically revert the buffer back to its exact pre-agent state.

---

## 12. Appearance, Color Schemes, Font Zoom & Custom Keybindings

### 12.1 `kajo-settings` $\rightarrow$ **Kajo IDE & Code**
Open `kajo-settings` and select the **Kajo IDE & Code** page (`src/settings/pages/page_ide.c`) to configure:
- **Syntax Color Scheme** (with 5 bundled `GtkSourceView-5` schemes in `data/ide/*.xml`):
  1. **`kajo-metro-dark`** *(Default)* — Signature `#101010` charcoal background with `#569cd6` / `#c586c0` / `#dcdcaa` / `#4ec9b0` / `#9cdcfe` tokens.
  2. **`kajo-oled-pitch`** — True `#050505` OLED pitch-black high-contrast neon palette.
  3. **`kajo-nord-frost`** — Arctic Nord Frost dark palette (`#14171d`).
  4. **`kajo-amber-crt`** — Retro monochrome amber phosphor CRT terminal palette (`#0c0900`).
  5. **`kajo-monokai-metro`** — Monokai Pro token palette on a sharp `#121212` Metro dark surface.
- **Typography & Geometry:** Editor font family (`Fira Code`, `JetBrains Mono`, `Iosevka`, etc.), font size (`8pt`–`28pt`), tab width (`2`, `4`, `8`), spaces vs. tabs, line number style (`Absolute`, `Relative`, `None`), current line highlight, right margin guide, word wrap, and minimap visibility.

All changes written to `~/.config/kajo/ide.toml` are hot-reloaded instantaneously across all running `kajo-ide` windows via `GFileMonitor`.

### 12.2 Live On-the-Fly Editor Font Zoom
Adjust editor font size at any time directly inside `kajo-ide`:
- **`Ctrl + +` / `Ctrl + =`** — Increase editor font size (`+1pt`) and persist to `~/.config/kajo/ide.toml`.
- **`Ctrl + -`** — Decrease editor font size (`-1pt`) and persist to `~/.config/kajo/ide.toml`.
- **`Ctrl + 0`** — Reset editor font size to the default `13pt`.

### 12.3 Custom Key Remapping (`~/.config/kajo/ide.toml`)
You can override or add custom key bindings per mode in `~/.config/kajo/ide.toml`:

```toml
[editor]
scheme = "kajo-metro-dark"
font_family = "Fira Code"
font_size = 13
tab_width = 2
insert_spaces = true
line_numbers = "relative"
highlight_current_line = true
show_right_margin = true
right_margin_col = 100
word_wrap = false
show_minimap = true

[keys.normal]
"C-p" = "picker_files"
"F2" = "lsp_rename"

[keys.leader]
"g g" = "picker_grep"
```
