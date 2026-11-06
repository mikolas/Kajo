# Codebase Health Audit & Cleanup Plan (`docs/codebase_health_plan.md`)

**Status:** Open — tracking branch `chore/codebase-health-cleanup`
**Audit baseline:** `master` @ `0ae2359` (tree identical to `6c1d04e`)
**Scope:** `kajo`, `kajo-settings`, `kajo-files`, `kajo-player`, `kajo-ide`, `kajo-greeter`, build system, docs, tests

This document records the findings of a full read-only audit and a prioritized plan to fix them. No source code was changed by the audit.

---

## 0. Method & Baseline

| Check | Result |
| :--- | :--- |
| `meson compile -C builddir` | 0 errors, 0 warnings |
| `meson test -C builddir` | 15 / 15 pass |
| ASan + UBSan build, full test run | **1 real failure** (heap use-after-free, §1.1). `ide-ipc` leak reports were all fontconfig / pango caches (library noise, not project leaks). |
| Extra warnings (`-Wshadow -Wformat=2 -Wnull-dereference -Wduplicated-cond -Wlogical-op`) | none |
| Extra warnings (`-Wcast-qual`, `-Wstrict-prototypes`) | 58 / 57 (mostly sort comparators and `registry.c`) |
| clang-tidy `clang-analyzer-*` over `src/` and `tests/` | no findings |
| Hand review | threading, async lifetimes, GVariant ownership, credential handling, CSS/chrome conformance, duplication, structure |
| Scripted scans (ad hoc) | 8-line clone detector, unmatched-ownership scan (3 hits, all false positives), async-call/cancellable audit (107 sites), function-length scan |

**Not verified:** runtime behaviour of any app (nothing was launched); race conditions were assessed by reading code, not by TSan. PGS subtitle support claimed in README was not verified.

---

## 1. Correctness & Memory Findings

### 1.1 Heap use-after-free in `ide_buffer_new` — CONFIRMED (ASan)
- `ide_buffer_apply_style_and_language()` (`src/ide/ide_buffer.c:1513`) passes `buf->scheme_id` to `ide_buffer_set_style_scheme()`.
- That function runs `g_free(buf->scheme_id); buf->scheme_id = g_strdup(wanted);` (`:1478`) where `wanted` aliases the freed pointer.
- Fires on every new buffer. The `ide-buffer` test fails under ASan at `tests/test_ide_buffer.c:120`.
- **Fix:** duplicate `wanted` before freeing the field (or `g_set_str()`-style replace).
- Audit other "free field, then copy from argument" sites for the same aliasing pattern.

### 1.2 Callbacks hold raw object pointers (possible use-after-free)
- `kajo_files_op_*_async` (`src/files/files_ops.c`) pass `KajoFilesWindow *self` as `user_data` with no ref / weak ref and no cancellable. `on_file_op_completed()` (`src/files/files_window.c:217`) then runs `KAJO_IS_FILES_WINDOW(self)` on a possibly freed pointer. Closing the window during a trash / copy / extract triggers it.
- `src/files/files_sidebar.c:259, 261, 605`: `g_mount_eject_with_operation`, `g_mount_unmount_with_operation`, `g_volume_mount` called with `NULL` cancellable and raw `self`; `kajo_files_sidebar_free()` can run first.
- `src/player/player_window.c:946, 989`: `gtk_file_dialog_open` with raw `self`.
- `src/dbus_helpers.c:279, 292`: `g_dbus_connection_call` with caller-supplied `user_data`, no cancellable.
- **Fix:** `g_weak_ref` (or `g_object_weak_ref`) in the op context, cancellable owned by the window and cancelled in `dispose()`.

### 1.3 IDE agent socket can freeze the IDE
- `src/ide/ide_agent_ipc.c:344–367`: `GSocketService` emits `incoming` on the main loop and the handler does a blocking `g_data_input_stream_read_line(..., NULL, NULL)`. A client that connects and never sends `\n` hangs the UI.
- `chmod(0600)` runs after `bind` (small window). Wrap the bind in `umask(077)`.
- `src/ipc/socket.c` already does this correctly (async read, 1 s timeout) — mirror it.

### 1.4 Synchronous subprocess / D-Bus calls on the GTK main thread
| Site | Impact |
| :--- | :--- |
| `src/ide/ide_search.c:532` (`ide_search_drawer_run_build`) | `meson compile` / `cargo check` / `go build` freezes the IDE for the whole build (README says "async") |
| `src/ide/ide_picker.c:405` | live grep |
| `src/ide/ide_tree.c:117, 360` | `git status --porcelain -uall`, slow on large repos |
| `src/ide/ide_search.c:222` | `rg -l` |
| `src/ide/ide_lsp.c:313, 586` | `rg` on hover / go-to-definition |
| `src/dbus_helpers.c`, `settings/pages/page_network.c`, `widgets/media.c`, others | ~25 `g_bus_get_sync` / `*_call_sync` / `*_new_for_bus_sync` |
- **Fix:** `GSubprocess` + async completion with a cancellable; `*_async` D-Bus variants.

### 1.5 Minor issues
- `src/widgets/custom-script.c`: a changed `interval_sec` is never applied to the running timer (`configure()` is called on reused widgets; the timer is only created in `enable()`).
- `src/widgets/privacy.c:80–120`: walks `/proc/*/fd` every 3 s on the main thread (~6 ms here, stops at first match). It is already skipped when no `/sys/class/video4linux` device exists, so it only runs on machines with a camera. Left as is (low cost); revisit if idle-CPU measurements flag it.
- `src/player/player_mpv.c`: global `setlocale(LC_NUMERIC, "C")` can affect number formatting in `kajo-files` (reuses this module for Quick Look).
- `src/ide/ide_buffer.c`: full-buffer re-tokenize on every `changed` and full-text `ide_compute_line_diffs()` — O(n) per keystroke. Fine for small files, degrades on large ones; debounce / limit to the edited range.
- `src/widgets/*`: panel widgets reach into `ShellApp` internals (`widget->app->idle_manager`, `->notifications`); `shell.c` dispatches volume / mute by comparing `klass->id` strings.

### 1.7 Niri action JSON rejected — FLOAT / TILE dead in settings, files, player — FIXED
- `kajo-settings`, `kajo-files` and `kajo-player` sent `{"Action":"ToggleWindowFloating"}`. `ToggleWindowFloating` is a struct variant (`{ id: Option<u64> }`), so Niri answers `{"Err":"error parsing request"}`; only `kajo-ide` used the valid object form. `niri_send_action()` discarded the reply, so the failure was silent.
- `page_displays.c` sent `{"Action":"ReloadConfig"}`, which is not a Niri action at all (same silent failure).
- **Fix:** shared `niri_toggle_window_floating()` / `niri_reload_config()` in `compositor/niri.c`; all four apps and the three settings pages that reload config use them; `niri_send_action()` now logs a `g_warning` on any `{"Err":...}` reply.

### 1.8 Settings emitted a forced-floating window rule — FIXED
- `page_niri.c` (`save_and_reload_niri_config`) appended `window-rule { match app-id="org.kajo.Settings"; open-floating true }` to the generated `~/.config/niri/config.d/00-kajo.kdl` on every save (present since the initial commit).
- It only became visible when a config reorganisation started including `00-kajo.kdl`. The duplicate in `50-window-rules.kdl` is not emitted by this repo (hand/tool-managed).
- **Fix:** the generated file no longer contains the rule, so `kajo-settings` opens tiled by default.
- **Existing installs:** `00-kajo.kdl` keeps the old rule until the next save on the Niri settings page; remove any rule in `50-window-rules.kdl` manually.

### 1.6 Verified clean (no action)
- mpv wakeup / render callbacks cleared before `mpv_terminate_destroy`; pending idles removed.
- GTask workers clone inputs; no shared mutable globals across threads (`g_clip_files` is main-thread only).
- Thumbnail callbacks bail before touching freed widget state.
- Credential handling matches AGENTS §4.E (copy → clear entry → `explicit_bzero`, no `JsonBuilder` for secrets, PAM service fallback correct, `PR_SET_DUMPABLE 0` in greeter).
- Archive extraction uses `ARCHIVE_EXTRACT_SECURE_NODOTDOT | SECURE_SYMLINKS`.
- `rg` / `git` invoked with argv arrays and `--`; no shell injection. The only `sh -c` is a fixed string.
- Timer, signal, and D-Bus subscription teardown is consistently paired in widgets.

---

## 2. Documentation vs Code Drift

| # | Claim | Reality |
| :-: | :--- | :--- |
| 2.1 | "Async LSP 3.17 client (`clangd`, `gopls`, `rust-analyzer`, `pyright`)" | `src/ide/ide_lsp.c` spawns no server. No `initialize` / `textDocument/*`. Hover and definition shell out to `rg`; completion scans identifiers in the buffer; rename is a buffer text replace. `ide_jsonrpc_frame_message` / `_extract_body` are tested but unused. |
| 2.2 | "DAP debugger (`gdb -i=dap`, `lldb-dap`, `dlv dap`) with 3-pane inspector" | `src/ide/ide_dap.c` spawns nothing. `start_session` sets `running = TRUE`, shows "PAUSED AT BREAKPOINT", and fills call stack / locals with a fake row and `active_breakpoints = N`. Violates AGENTS §5.2 (no mock data). |
| 2.3 | IDE settings in `~/.config/kajo/ide.toml` (README, AGENTS, docs; ~10 mentions) | No `ide.toml` anywhere in `src/`; IDE reads `~/.config/kajo/config.json` (`ide_window.c:2503`). |
| 2.4 | App IDs `org.kajo.Ide` / `org.kajo.Shell` / `org.kajo.Greeter` | Code and `.desktop`: `org.kajo.IDE` / `org.kajo.shell` / `org.kajo.greeter`. |
| 2.5 | `ide_mermaid.h/c` listed in DEVELOPMENT.md | File does not exist; Mermaid lives in `ide_markdown.c`. |
| 2.6 | ROADMAP paths | `src/fold/fold_manager.c`, `src/touch/touch_gestures.c`, `src/osk/osk_window.c`, `settings/pages/page_shaders.c` do not exist. |
| 2.7 | `PKGBUILD` | `pkgver` 0.4.0 vs `meson.build` 0.1.0; deps omit gtksourceview5, poppler-glib, libarchive, mpv, pam, wayland, xkbcommon; URL `github.com/mikolas/desktop`. |
| 2.8 | PA auto-reconnect "2-second timer" (AGENTS §4.C) | All four implementations use 1000 ms. |
| 2.9 | "11 automated suites" (DEVELOPMENT.md) | 15 suites; `ide-fsm`, `ide-buffer`, `ide-ipc`, `player-queue` missing from prose / table. |
| 2.10 | Desktop integration | No `.desktop` for `kajo-settings` or `kajo-greeter`; icons are installed but entries are not. |

---

## 3. Visual-Guideline Conformance ("Aero + Metro")

### Passing
- Zero corner radius: the only non-zero `border-radius` in `data/defaults/style.css` is the `3px` OSK keycap (permitted exception). None in C.
- No `opacity: 0`; `#panel-container.autohide-hidden` is transparent, not invisible.
- No un-scoped dynamic `GtkCssProvider` (three runtime providers all use per-swatch classes).
- All six apps follow the chrome contract (undecorated, `shell-settings-window` + app class, `GtkWindowHandle` header, `🗗 FLOAT / TILE`, `.settings-close-btn`); files and player via Blueprint, settings and IDE via C.
- Every standalone `main.c` initializes `ShellTheme` + `ShellIcons`.
- Repo `style.css` is in sync with `~/.local/share/kajo/defaults/` and `~/.config/kajo/`.
- Lock and greeter windows call `gtk_window_set_interactive_debugging(FALSE)`.

### Deviations
- **Off-palette literals:** >100 distinct hex values in `style.css`, including many near-duplicate grays (`#202020`, `#1c1c1c`, `#151515`, `#121212`, `#0d0d0d`, `#0c0c0c`, `#050505`, …). Danger red appears as `#e81123`, `#ff5555`, and `#ff6b6b`; IDE tree icons use `#98a2b3`, `#f14c28`, `#e8a838`.
- **Hard-coded accent:** `style.css:2616` (`.ide-md-alert-note` border) uses literal `#0078d4`; will not follow the accent theme.
- **Sans font:** `style.css:2714` (`.ide-help-desc`) uses `"Inter", "Cantarell"`; the design system specifies Fira Code for technical text.
- **Token coverage:** only 9 `@define-color` tokens exist; surface-0 … surface-4 are literals.
- **Duplicated window classes:** IDE adds both `shell-settings-window` and `shell-ide-window` (consistent with files / player — documented as intentional).

---

## 4. Code Duplication

Exact-clone scan: 181 duplicated 8-line windows (modest). The larger problem is whole subsystems implemented several times.

| Duplicated subsystem | Where | Proposed consolidation |
| :--- | :--- | :--- |
| PulseAudio context / mainloop / subscribe / reconnect | `widgets/volume.c`, `widgets/control-center.c`, `widgets/media.c`, `settings/pages/page_audio.c` | New `src/audio/pa_client.{h,c}`; enforce 2 s reconnect, `MASK_SERVER`, `sink_channels` in one place |
| Accent preset table (8 colors) | `settings/pages/page_theme.c`, `widgets/theme.c` (identical) | Move to `theme.{h,c}` |
| Poppler page → `GdkTexture` | `files/files_quicklook.c:154–183`, `ide/ide_markdown.c:1606–1631` | Shared `src/common/pdf_render.{h,c}` |
| Shell socket client | `ipc/socket.c:325`, `settings/settings_common.c:277` | Settings calls `shell_ipc_send_command()` |
| Password-entry focus / reset / auth-state | `greeter/main.c`, `lock/session_lock.c` | Shared auth-surface helper (also centralizes credential hardening) |
| CLI flag handling | `src/main.c` (~100 lines, 13 identical branches) | `{flag, alias, command}` table + one loop |
| Theme / icon boot | `files/`, `player/`, `ide/`, `settings/` `main.c` | `kajo_app_init_theme()` |
| GVariant property reads | ~60 manual read + unref sites (`battery.c`, `bluetooth.c`, `network.c`, `tray.c`, `dbus_helpers.c`, `page_network.c`, `page_bluetooth.c`) | `shell_dbus_get_{string,bool,uint}()` with `g_autoptr(GVariant)` |
| Format size / time | `kajo_files_format_size`, `kajo_player_format_size` | One implementation |
| Niri socket connect | `niri.c` (two paths), `greeter/main.c:291` | Route through `niri_send_ipc_request()` |
| `emit_*_callbacks` | `compositor/niri.c` (4 copies) | Macro / table |
| Meson source lists | `sources` vs `widget_sources`; `config.c`, `theme.c`, `icons.c`, `niri.c` compiled into 5 executables | `static_library('kajo-core')` + widgets library |

---

## 5. File & Function Size

### Oversized files
| File | Lines | Proposed split |
| :--- | :-: | :--- |
| `src/files/files_window.c` | 2651 | `files_window_ctxmenu.c`, `files_window_grid.c` (grid / list factories), `files_window_banners.c` (connect / trash / prompt), `files_window_keys.c`, breadcrumbs; keep controller + navigation |
| `src/ide/ide_markdown.c` | 2626 | `ide_mermaid.c` (layout, Cairo render, SVG / PNG export — already promised by DEVELOPMENT.md), embed cards (image / PDF / archive), inline Pango formatting |
| `src/ide/ide_window.c` | 2539 | `ide_actions.c` (`dispatch_ide_action`, `execute_buffer_motion_or_edit`), `ide_header.c` (header, tab ribbon, status bar, HUD), `ide_config.c` |
| `src/ide/ide_buffer.c` | 1952 | `ide_syntax.c` (~600 lines), `ide_textobjects.c`, `ide_diff.c` |
| `src/ide/ide_fsm.c` | 1872 | per-state handler files; generated lookup for `ide_action_type_from_name` and remaps |
| `src/player/player_window.c` | 1703 | popovers (subs / audio / speed), key handling, HUD |
| `src/widgets/media.c` | 1275 | MPRIS, FFT / VU meter, popover |
| `src/launcher/emoji_dataset.h` | 1928 | data in a `.c` or GResource instead of a header |

### Longest functions (lines)
`dispatch_ide_action` 567 · `kajo_ide_window_init` 469 · `shell_config_load` 394 · `build_page_displays` 387 · `render_markdown_into_box` 357 · `execute_buffer_motion_or_edit` 327 · `handle_leader` 321 · `build_greeter_surface` 246 · `handle_normal` 241 · `ide_markdown_format_inline_pango` 222.
7 functions > 300 lines, 23 > 150, 65 > 100 (heuristic scan; K&R-style `ide/` files may be slightly mis-measured).

Strategy: table-drive `config.c` load / save; move UI-building functions to Blueprint templates (already used by shell, files, player, and some settings pages); replace giant `switch` ladders with dispatch tables.

---

## 6. Project Structure

- **Good:** subsystem directories map cleanly to features; tests map well to modules; `.gitignore` covers `builddir/`, `scratch/`, `docs/archive/`, `configure/`, `setup/`.
- **`KAJO_SOURCE_DIR` baked into binaries** and used as a runtime search path (`ide_buffer.c:1472`, `ide/main.c:18`, `settings/pages/page_niri.c:190`). Packaged installs would point at a non-existent directory; scheme registration is also duplicated in two places. Use the install `datadir`.
- **Stray build dirs:** `configure/` and `setup/` (ignored, ~570 KB each) next to `builddir/`.
- **`scratch/`:** ~2.3 MB of DSDT dumps and probes (ignored); overlaps `HARDWARE_FINDINGS.md` / `spec_foldable_sensors.md`.
- **Style:** `src/ide/` uses 2-space indent, everything else 4-space; no `.clang-format`.
- **Layering:** see §1.5 (widgets reach into `ShellApp`).

---

## 7. Test Gaps

- No tests for `kajo-settings`, panel / shell wiring, notifications, OSD, launcher.
- `compositor/niri.c` JSON event handling is a good unit-test target (no coverage).
- No LSP / DAP behaviour tests (there is no real implementation — §2.1, §2.2). `ide-ipc` covers only framing helpers.
- `ide-buffer` passes in the normal build while containing a heap use-after-free (§1.1) — it only shows under ASan.
- No sanitizer configuration is part of the workflow.

---

## 8. Fix Plan

Each phase is its own branch / PR off `master` (this branch carries the plan only). Every phase must pass `meson compile -C builddir && meson test -C builddir` with 0 warnings, and — once Phase 0 lands — the sanitizer run.

> **Status (this branch / PR #25):** all phases were worked on one branch with one commit per phase instead of separate PRs. `[x]` = done, verified with `meson compile` (0 warnings), `meson test` (16/16) and `scripts/asan-test.sh` (16/16, clean). `[ ]` = deferred, with the reason.

### Phase 0 — Safety net
- [x] Sanitizer workflow: `scripts/asan-test.sh` (+ `DEVELOPMENT.md` section).
- [x] `.clang-format` for new code (4-space; existing files are not bulk-reformatted).

### Phase 1 — Memory & lifetime bugs
- [x] §1.1 `scheme_id` aliasing in `ide_buffer_set_style_scheme()`; `ide-buffer` now passes under ASan.
- [x] §1.2 file ops: `GWeakRef` instead of a raw window pointer. Sidebar mount/eject/unmount/enclosing-mount and the mount-operation `ask-password` handler: `alive` cancellable token. Player file dialogs: ref held for the dialog's lifetime. (`dbus_helpers` async callers already pass `GWeakRef`s — no change needed.)
- [x] §1.5 `custom_script_widget_configure()` now restarts the timer when `interval_sec` changes.
- [x] §1.5 `privacy.c`: no change — already skipped when no V4L2 device exists (finding corrected in §1.5).

### Phase 2 — Main-thread blocking
- [x] §1.3 agent socket: async read with a 2 s timeout per connection, `umask(077)` around bind, alive-token for in-flight connections. Regression test added (`ide-ipc`: round-trip + stalled client keeps the loop ticking).
- [x] §1.4 build runner → `GSubprocess` + async completion, cancellable, superseded builds cancelled.
- [x] §1.4 picker live grep → async, cancels the previous query, keeps old results visible until the new ones arrive.
- [ ] §1.4 remaining synchronous subprocesses: `ide_tree` (`git status`/`rev-parse`), `ide_search` (`rg -l`), `ide_lsp` (`rg` on hover/definition). These are short-lived and their callers (and tests) rely on synchronous results; converting them needs a small async result API. **Deferred.**
- [ ] §1.4 synchronous D-Bus calls (`dbus_helpers.c`, `page_network.c`, `media.c`, …, ~25 sites). They are part of synchronous helper APIs used by many callers; converting needs an API redesign. **Deferred.**

### Phase 3 — Truth in documentation
- [x] §2.1 / §2.2 README, ROADMAP, AGENTS, DEVELOPMENT, `docs/kajo_ide_manual.md`, `.desktop` comment now describe the real LSP-lite / debugger-UI state; real LSP, DAP and key remapping added to the ROADMAP as planned. Bound-but-unhandled chords marked *(planned)* in the manual.
- [x] §2.3 `ide.toml` → `config.json` (`ide` object) everywhere; manual §12 rewritten.
- [x] §2.4 App IDs: docs aligned to the code (`org.kajo.IDE`, `org.kajo.shell`, `org.kajo.greeter`).
- [x] §2.5 / §2.6 / §2.9 DEVELOPMENT.md test table (16 suites) and ROADMAP paths fixed; `ide_mermaid.c` now exists (Phase 5).
- [x] §2.7 `PKGBUILD` refreshed (URL, dependencies, optdepends).
- [x] §2.8 PulseAudio reconnect is now 2 s (`SHELL_PA_RECONNECT_MS`), matching AGENTS.md.
- [x] §2.10 `org.kajo.Settings.desktop` added and installed. No entry for `kajo-greeter` on purpose: it is started by greetd, not from a launcher.

### Phase 4 — Shared modules
- [x] Accent presets → `theme.{h,c}` (`shell_accent_presets`).
- [x] Shell socket client → `src/ipc/ipc_client.{h,c}` (used by the `kajo` CLI and `kajo-settings`).
- [x] `src/main.c` CLI flags → table-driven.
- [x] `kajo_app_init_theme()` / `kajo_app_shutdown_theme()` (`src/app_init.{h,c}`) for files/player/ide/settings.
- [x] Poppler rasterizer → `src/common/pdf_render.{h,c}`.
- [x] PulseAudio connection → `src/audio/pa_conn.{h,c}` (volume, control-center, media, settings audio page).
- [x] `niri.c` `emit_*_callbacks` → one macro.
- [x] Meson: `kajo-core` static library (config, theme, icons, niri, app_init) instead of 5 recompiles; IDE scheme search paths use user dir, install prefix (`KAJO_DATA_DIR`), XDG system dirs, then the source tree as a dev fallback only.
- [ ] GVariant property-read helpers (~60 sites). **Deferred** — mechanical but wide; best done together with the D-Bus async work.
- [ ] Greeter/lock auth-surface helper. **Deferred** — security-sensitive; needs its own reviewed change.
- [ ] Format-size helpers (files vs player differ in semantics) and the second Niri connect path in `greeter/main.c`. **Deferred** — low value.

### Phase 5 — File splits (behaviour-preserving)
- [x] `ide_markdown.c` → `ide_mermaid.{h,c}` (2585 → 2123 lines; Mermaid engine 470).
- [x] `ide_buffer.c` → `ide_syntax.c` (semantic highlighter; 1952 → 1186).
- [x] `files_window.c` → `files_window_ctxmenu.c` + `files_window_keys.c` + `files_window_private.h` (2651 → 1654).
- [x] `ide_window.c` → `ide_actions.c` + `ide_window_private.h` (2539 → 1517).
- [ ] `ide_fsm.c`, `player_window.c`, `widgets/media.c`, banners/grid factories of `files_window.c`, `emoji_dataset.h`, and breaking up functions > 300 lines (`dispatch_ide_action` is still ~560 lines, now in `ide_actions.c`). **Deferred.**

### Phase 6 — Visual token cleanup
- [x] `@define-color` tokens for surface 0/2/3/4, warning, success, telemetry and `metro-urgent-text`; 167 literal hex uses in `style.css` replaced by tokens; CSS parses with 0 errors.
- [x] The three danger-red variants collapsed to `@metro-urgent` (fills) + `@metro-urgent-text` (text).
- [x] Hard-coded `#0078d4` in the Markdown note border → `@metro-accent`.
- [x] `Inter` / `Cantarell` → Fira Code.
- [x] `style.css` synced to `~/.local/share/kajo/defaults/` and `~/.config/kajo/`; `docs/design_system.md` token table updated.
- [ ] Remaining one-off grays (`#202020`, `#1c1c1c`, `#151515`, …) are left as literals — folding them would change pixels and needs a visual review. `docs/ui-gallery.html` not regenerated.

### Phase 7 — Tests
- [x] `niri-ipc` suite (`tests/test_niri.c`, mock Niri socket): exact JSON of `ToggleWindowFloating` / `LoadConfigFile`, rejection logging, EventStream parsing. Verified to fail against the old `"ToggleWindowFloating"` string.
- [x] Agent-socket round-trip and stalled-client test (`ide-ipc`).
- [x] §1.1 regression is covered by `ide-buffer` run under `scripts/asan-test.sh`.
- [ ] Test for file-op callbacks after window destruction (needs a display / GTK window harness). **Deferred.**
- [ ] LSP / DAP integration tests — N/A until a real client exists.

### Phase 8 — Settings UI conformance (kajo-settings vs `docs/ui-gallery.html`)
Review method: every page was rendered headlessly (cage + grim, isolated config) and compared with the gallery; README claims were checked against the UI. Rule applied: if a feature is complete and contradicts the README, keep it and fix the README; if the README claims something that is not built, build it or drop the claim.

- [x] New component **toggle card** (`.toggle-card`, `GtkToggleButton`, 4 px left indicator bar lit with the accent) added to the gallery, `docs/design_system.md` and `style.css`; the panel Control Center quick tiles share it. Rule of thumb documented: switch row = plain preference, toggle card = feature with status/unsupported/sub-settings, segmented group = 2–4 short exclusive choices.
- [x] VRR and Autohide → toggle cards. On/off dropdowns → switches (touch swipes, Niri XRay, HP keylight wake). A/B and 3-option dropdowns → segmented groups (panel edge, OSK push, tab width, keyboard backlight level/timeout, widget display mode ICON-ONLY | TEXT-ONLY | FULL).
- [x] Removed dead/duplicate controls: Panel Blur and Outer Margins tiles (not backed by any config or code), Panel Edge tile (duplicated the control above), placeholder HDR tile.
- [x] Dropdown double surface fixed (the settings page-stack background leaked into every dropdown). Slider rows: aligned label column per page and one readout style (`.shell-popover-badge`). `flat` removed from settings buttons; primary actions use one wording (`[ ✓ SAVE & APPLY … ]`), secondary actions have no brackets. Widgets headers lose their emoji dots. IDE page no longer overflows (`create_action_row` labels wrap).
- [x] Missing functionality built: **Wi-Fi inline passphrase banner** (real AP security detection, saved-profile reconnect, `AddAndActivateConnection` for new WPA/WPA2/WPA3-Personal and open networks; compiled and rendered, **not tested against a real access point**), **output/input mute switches** (synced with external changes), **IDE word wrap, margin guide, spaces-vs-tabs** (config fields + editor + round-trip test).
- [x] README/manual wording fixed where the function lives elsewhere (per-app volume in the panel popover, DND/clear-history in the panel widgets, posture controls on Displays).
- [ ] Relative line numbers, custom accent hex picker, DND switch on the Notifications page: moved to the ROADMAP as planned (decisions taken: panel widget stays the DND control).
- [ ] Untested by hand: clicking through the new controls and the Control Center restyle (needs a live session); Wi-Fi connect against a real network.

### Phase 9 — Tooling & guidance
- [x] `scripts/ui-shot.sh` (isolated headless render + screenshot), `scripts/audit.sh` (warnings, tidy, sizes, dups, blocking, lifetimes, colors); documented in `DEVELOPMENT.md` §6–§7 together with the screenshot-analysis checklist and UI-control usage guidance. `AGENTS.md` gained live-session-safety and visual-verification rules, the control-selection rule and a concise Dos & Don'ts section.
- [x] `audit.sh tidy` triage: two real latent null dereferences fixed (`cpu_mem_enable`, `battery_enable` null-checked the widget and then dereferenced it unconditionally). The remaining findings were reviewed as false positives or low value (`g_autofree` modelled as a leak in `ide_mermaid.c`, flag-enum casts in pulse code, `sizeof` in `g_new0`, kind-dependent fields in `files_ops.c`).
- [x] Lesson recorded: a visual-inspection helper must not use `dbus-run-session` (it activates installed user services inside the private bus and leaves FUSE mounts); the first version did and was fixed before landing.

### Out of scope / separate decisions
- Implementing a real LSP / DAP client (large feature work; Phase 3 only aligns the docs).
- Replacing the widget ↔ `ShellApp` coupling with a service / event interface (§1.5, design discussion first).

---

## 9. Priority Summary

| Priority | Item | Phase |
| :-: | :--- | :-: |
| P0 | `scheme_id` use-after-free | 1 |
| P0 | File-op / sidebar / dialog callback lifetime | 1 |
| P1 | IDE agent socket blocking read; synchronous build / grep / git on UI thread | 2 |
| P1 | Docs overstate LSP / DAP; wrong config file and app IDs | 3 |
| P2 | Deduplicate PA client, accent presets, poppler, IPC client, auth surface | 4 |
| P2 | Split oversized files and functions | 5 |
| P3 | Palette tokens, font exception, tests, packaging | 6–7 |
