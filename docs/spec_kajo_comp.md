# Kajo Compositor (`kajo-comp`) & Domain-Split TOML Configuration Specification

> Repository copy: `docs/spec_kajo_comp.md` is the maintained version. The copy at `~/KAJO_COMPOSITOR_SPEC.md` is the pre-move original and is no longer updated.

> **Status:** Reference Architecture & Engineering Blueprint — **Revision 7 (HDR10 moved to v2 after the Phase 0 spike; v1 is SDR-only)**
> **Target Repository:** `/home/mikolas/src/Kajo`  
> **Implementation Language:** Pure C11 (Meson, GLib-2.0, `wlroots-0.20`, Vulkan, `tomlc17`)

### Revision 7 — HDR10 deferred to v2 (owner decision, 2026-11-10)

Phase 0 ran on Host A in two rounds (logs in `scratch/hdr-spike/logs/`, not committed):

- **Round 1:** HDR never switched on. The cause is the installed `libdisplay-info` 0.3.0, which misses the panel's HDR blocks inside a DisplayID 2.0 extension; 0.4.0 finds them (§8.4).
- **Round 2 (`--force-hdr-caps`):** the panel accepted BT.2020/PQ with a 10-bit (`XR30`) and an FP16 (`XB4H`) render format, with and without mastering metadata, and toggled between HDR and SDR at runtime. **But the image was wrong:** the owner saw a totally wrong gamma curve, and the self-test read back SDR white as PQ code 1023 (10000 cd/m²) and `#808080` as code 513 (≈94 cd/m² instead of ≈44). The cause is a **bug in the spike, not in wlroots**: wlroots recomputes the scene's output colour transform only when the image-description change passes through `wlr_scene_output_build_state()` in the same output state (`types/scene/wlr_scene.c` ≈ line 2505). The spike committed the description directly with `wlr_output_commit_state()`, so the panel was in PQ while the scene kept encoding gamma 2.2. Sway avoids this by building the scene state as part of output configuration.

**Decision:** HDR10 is a **v2** task. v1 is SDR only. The spike's findings and the two pitfalls (libdisplay-info 0.3.0 DisplayID v2 gap; image description must go through the scene's build-state path) are kept in §8.4 and §10 for v2. Fixing `libdisplay-info` (local package) is also deferred.

Changes in this revision: §1 goals and non-goals; §2 (forced Vulkan not required); §3.1 renderer row; §3.2 marked v2; §8 plan (Phase 0 closed, Phase 4 without HDR); §10 HDR entry.


### Revision 6 — fixes applied on branch `revert/poppler-removal` (nothing committed except the poppler revert)

Everything in the Revision 5 "Still open or new" table was addressed. Verified after a **full clean rebuild: 0 warnings; `meson test`: 22/22**; `test-config` and `test-autohide` also clean under ASan/UBSan with leak detection.

| Revision 5 item | Change | How verified |
| :-: | :--- | :--- |
| 1 Patcher writes invalid files | `kajo_cfg_domain_save_patched()` now parses the patched text and compares it with a freshly serialized draft. On any mismatch (quoted key, dotted key, inline table) it copies the original to `<file>.bak`, logs a warning and writes a regenerated file. A broken file is never written. | New test `patcher-never-writes-unparsable-file` covers `"ui_font_size_px" = 15`, `panel.height = 50` and `panel = { … }`: save succeeds, `.bak` equals the original, a fresh store sees the new value, a second save is accepted. |
| 2 Hot reload resets strip width | `shell_autohide_reload_config()` reads `autohide_edge_strip_px` like the constructor. | New test `edge-strip-survives-reload`. |
| 3 Comments inside `[layout]` arrays | **Not fixed; documented** as a limitation in `docs/spec_toml_config.md` §5 and §11. | — |
| 4 Callbacks on the writer's thread | Subscriptions are reference-counted, record the subscriber's thread-default `GMainContext`, and are delivered with `g_main_context_invoke_full()`: inline when that context is owned by the caller, queued otherwise; a callback queued before `unsubscribe()` or store destruction is dropped. | New test `subscriber-runs-on-subscriber-thread` (writer thread, same-thread, unsubscribe-while-queued). The test acquires the default context itself to emulate a running loop. |
| 5 Vendored licence notices | `src/vendor/tomlc17/LICENSE` added; README credits `tomlc17` and `tinyexpr`. | Files read back. The `tinyexpr` URL in the README was written from memory. |
| 6 `KAJO_SOURCE_DIR` in binaries | Compiled-in source-tree fallback removed from `kajo_config_find_default_file()`. Without an installed defaults file the compiled defaults apply (the parity test keeps them identical to `data/defaults/*.toml`); `KAJO_DEFAULTS_DIR` overrides the search. The macro is still defined by `meson.build` and used only by the tests. | Full test run. |
| Poppler | Reverted (commit `47c7e2a`); see Revision 5 text. | Build and tests as above; the PDF preview itself was not run in a UI. |
| Greeter wrapper (from §6.5.6) | `data/greetd/kajo-greeter-session`: `USER=""` clobbering fixed (variable renamed), unknown arguments now reported, missing `cage` after missing `niri` now exits with an error. The Niri-specific exit path in `kajo-greeter` is **unchanged** (no compositor to replace it yet). | Ran the script against a stub `cage`: `$USER` and `KAJO_GREETER_USER` correct; the "neither installed" path prints the error and returns 1. |
| Docs | `docs/spec_toml_config.md` rewritten from the code (its tables are generated from the compiled schemas); `docs/spec_files_phase2.md` and `docs/spec_kajo_mod.md` updated from `ShellConfig`/`config.json` to the TOML keys; `docs/kajo-settings-mockup.html` footer strings; `DEVELOPMENT.md` (tests, `KAJO_CONFIG_DIR`/`KAJO_DEFAULTS_DIR`); `docs/script_widgets.md` (`exec_cmd` → `exec`). `docs/codebase_health_plan.md` is a dated audit and was left as history. | Read back; schema tables generated by a scratch program linked against the real schemas. |


### Revision 5 — code state re-check (branch `refactor/remove-pdf-poppler`, on top of PR #32)

Checked: `git log`, `meson compile` after a full clean (**0 warnings**), `meson test` (**22/22**, including a new `fuzz-toml-config` smoke test), the vendored parser's size, and scratch programs linked against the real config store (single-process, two-thread and two-process runs). Kajo's own `LICENSE` is MIT and `PKGBUILD` says `license=('MIT')`.

**Resolved since Revision 3 (verified by running it):**

| Revision 3 finding | Now |
| :--- | :--- |
| Vendored parser was an in-house subset | `src/vendor/tomlc17/` is the upstream source: `tomlc17.c` 3,350 and `tomlc17.h` 291 lines, matching the upstream clone I read earlier; a file containing a datetime now loads. |
| Syntax error reset the snapshot, then a GUI save overwrote the file | Snapshot keeps the last good value; `update_*()` returns `Refusing to overwrite … TOML syntax error`; file untouched. |
| GUI save discarded comments and unknown keys | Comments, unknown keys and inline `# …` are kept (surgical patching). |
| No write lock | In-process mutex plus `flock()` on `.<file>.lock`: 2 threads × 100 updates and 2 processes × 150 updates lost nothing. |
| Migration gate and race | `shell.toml` is written last, the check is re-done under `.migrate.lock`. |
| CWD default-file lookup | Removed. Order is now `KAJO_DEFAULTS_DIR`, `~/.local/share/kajo/defaults`, `/usr/local/share/…`, `/usr/share/…`, then a compiled-in `KAJO_SOURCE_DIR` fallback. |
| `-Wcast-function-type` warning, `autohide_edge_strip_px` loss, unbuilt fuzz target, impure displays mutator | Warning gone; `edge_strip_px` is back in the schema and in `autohide_new()`; fuzz target is a meson test; the displays page collects UI state first and mutates a plain struct. |

**Still open or new (verified):**

| # | Finding | Evidence | Consequence |
| :-: | :--- | :--- | :--- |
| 1 | **The patcher can write an invalid file and still report success.** For hand-written but legal TOML forms (a quoted key `"height" = 50`, a dotted key `panel.height = 50`, an inline table `panel = { height = 50 }`) it appends a second `height` or a second `[panel]` instead of editing the existing one. | `update_shell()` returned `ok=1`; the log then shows `duplicate key`, `table defined before`, `table defined more than once`; the reloaded value stayed at the old `50`. | The user's change is silently lost, and every later GUI save is refused until the file is fixed by hand. Required fix: re-parse the patched text before writing; if it does not parse, fall back to full regeneration or return an error. |
| 2 | `shell_autohide_reload_config()` still sets `edge_strip_px = 2` (`src/autohide.c:186`), only the constructor honours the configured value. | Read in source. | Hot-reload resets the strip width. One-line fix. |
| 3 | Comments **inside** the `[layout]` widget arrays are lost when the layout is saved. | Test file with `# keep me` inside `left = [ … ]` lost it. | Acceptable only if documented. |
| 4 | Cross-process `flock` and the in-process mutex are verified; **subscriber callbacks still run on the thread that called `reload()`**, not on a target `GMainContext`. | Read in source (`notify_subscribers_on_caller_thread`). | Safe only while every `update_*()` caller is on the main thread (true today). |
| 5 | ~~The vendored `tomlc17` had no `LICENSE` file and the README did not credit it~~ | **Fixed on branch `revert/poppler-removal` (uncommitted):** `src/vendor/tomlc17/LICENSE` added (text fetched from upstream `main`), README "License & Attribution" now credits `tomlc17` (MIT) and `tinyexpr` (zlib). Not yet merged. | Done on the branch. |
| 6 | `KAJO_SOURCE_DIR` (the absolute build-tree path) is compiled into every binary as a last-resort defaults location. | `meson.build:18`, `kajo_config_schema.c:1213`. | Harmless when the path does not exist; leaks a developer path into release builds. |

**Poppler/PDF removal — reverted on branch `revert/poppler-removal`.** The removal had landed on `master` as squash commit `eb4cf58` (#33; originally `3462a81`, 13 files, +19/−425). Under the owner's source-only, dynamic-linking policy it was not required, so it was reverted with `git revert` (commit `47c7e2a`, local, not pushed). After the revert: `poppler-glib` is back in `meson.build`, `src/common/pdf_render.{c,h}` exist again, PDF Quick Look, the inspector thumbnail and the Markdown PDF card are restored, and `PKGBUILD`, `AGENTS.md`, `DEVELOPMENT.md` and `docs/ui-gallery.html` are accurate again without further edits. Verified on the branch: the build compiles with no warnings in the rebuilt files and `meson test` passes 22/22. I did not run the UI to exercise the restored PDF preview. A full clean rebuild after the revert was **not** repeated.

**Licence policy (owner's statement, recorded here):** Kajo is MIT; every dependency is linked **dynamically**; nothing is vendored unless unavoidable; **only source is distributed** (no binaries, no packaging, no CI), and builds are for the owner's private use. Under that model no GPL obligation is triggered for Kajo's own source: copyleft attaches to distributed combined binaries, and none are distributed. The earlier removal of poppler was therefore not required by this policy and has been reverted on a branch (see above). Copyleft libraries that Kajo binaries link on this machine (including `poppler-glib` GPL again after the revert) (per `pacman -Qi`, `meson.build`): `libsidplayfp` GPL-2.0-only and `uade` (GPL/LGPL mix) in `kajo-mod`; `mpv` GPL-2.0-or-later (Arch build, with a GPL ffmpeg behind it) in the player and Quick Look; LGPL libraries (GTK, GLib, GtkSourceView, libpulse, libgme, …). Anyone who builds and *distributes* a binary of this combination would have to do so under GPL terms; that is theirs to comply with. This is not legal advice.

### Revision 4 — greeter host mode (`kajo-greeter` on `kajo-comp`)

Source for everything below: `data/greetd/{kajo-greeter-session,niri-greeter.kdl,install-greeter.sh}`, `src/greeter/{main.c,greeter_users.c}`. I did not run the greeter or greetd in this revision. Statements about greetd's own behaviour come from prior knowledge, not from reading greetd's source, and are labelled **(greetd, unverified here)**.

| # | Earlier statement | Finding | Resolution |
| :-: | :--- | :--- | :--- |
| 1 | §6.5: greeter mode has "virtual terminals disabled" | Contradicts §7.5 (the rescue TTY must always work). The `cage` fallback used today keeps VT switching. | VT switching stays **enabled**; §6.5.3. |
| 2 | §6.5: compositor "exits when `quit_greeter_compositor_and_app()` signals session handoff" | The signalling mechanism was unspecified. Today it is `niri msg action quit` run through a **blocking `g_spawn_sync` on the GTK main thread** (`main.c:294`), which also violates the repo's no-blocking-calls rule. | Child-exit model (cage-style); the greeter's compositor call is deleted; §6.5.2, §6.5.6. |
| 3 | §3.1: Vulkan is forced | Only HDR needs Vulkan. A greeter in SDR does not. | Greeter mode uses the automatic renderer and SDR; §6.5.4. |
| 4 | §1: multi-monitor is a non-goal | `kajo-greeter` builds one surface per monitor and rebuilds on hotplug (`main.c:827–910`). | Explicit exception for greeter mode (placement only); §6.5.4. |
| 5 | (not covered) | `kajo-greeter-session` assigns `USER=""` for its `--user` option; in `sh` an imported variable stays exported, so the compositor and greeter start with an **empty `$USER`** (reproduced with `USER=greeter sh -c 'USER=""; env'`). | Listed as an existing bug to fix; §6.5.6. |
| 6 | (not covered) | The greeter has no on-screen keyboard, so password entry in tablet mode is impossible today, under Niri as well. | Documented gap; §6.5.8. |
| 7 | (not covered) | The greeter's config comes from the `greeter` user's environment, not the logged-in user's, and default-file lookup checks the current working directory first (§8 Phase 1 defect). | §6.5.5. |

### Revision 3 — impact of the merged TOML config refactor (commit `42abe01`, PR #31)

*Historical: this table describes commit `42abe01`. Items 1–4 and 6 are fixed in PR #32; see the Revision 5 status tables above for the current state.*

Revision 2's config statements described a plan; PR #31 has since landed on `master`. Everything below was checked by reading `src/config/`, `src/vendor/tomlc17/`, `data/defaults/*.toml`, the call sites, and by running the build (`meson compile`: succeeds with **one new warning** at `src/mod/mod_window.c:1494`), `meson test` (21/21 pass) and `test-config` under ASan/UBSan with leak detection (clean). A scratch program linked against the real store reproduced the defects marked **(reproduced)**.

| # | Revision 2 statement | Finding | Resolution |
| :-: | :--- | :--- | :--- |
| 1 | "Vendor `tomlc17`" (upstream, ~3.6k lines) | The vendored `src/vendor/tomlc17/` is **not upstream**: 1,168 + 84 lines, header "Self-contained C17 TOML 1.0 Parser for Kajo", API-compatible subset (`toml_parse`, `toml_get`, `toml_seek`, …). The PR's own spec calls it "unmodified"; the commit message says "TOML 1.1". Measured with a 50-case probe under ASan: no date/time support (a valid TOML 1.0 file containing a datetime is rejected **as a whole**), no TOML 1.1 features (inline-table newlines, `\e`, `\xHH`, seconds-less times), accepts `a.b=1` followed by `[a]` (invalid in 1.0). ASan clean; nesting capped at 32. | §4.1, §9.4 rewritten. Replace with upstream, or rename and document as in-house. |
| 2 | "Zero cross-process write races" (§4.2) | `update_domain_generic()` does read-modify-write with **no write mutex and no file lock** (the PR's spec promises a `write_mutex`). Two writers of one domain (`kajo` widgets and `kajo-settings` both write `shell.toml`/`theme.toml`) can lose an update. | §4.2 corrected; open defect. |
| 3 | GUI+Human files keep their comments | Every GUI save **regenerates the whole file** from the struct: comments, unknown keys and ordering are discarded. **(reproduced)** | §4.2.1 added; constrains how `comp.toml` can be written. |
| 4 | "Syntax errors preserve valid settings" | On a syntax error the **snapshot reverts to compiled defaults** (not the last good one), and the next GUI save **overwrites the user's recoverable file with defaults**. **(reproduced)** | Open defect; §4.2.1 requires the fix. |
| 5 | `--config-dir` is new | `KAJO_CONFIG_DIR` already exists in `kajo_config_store_new_for_dir()`. | §6.3 maps `--config-dir` to it. |
| 6 | Phase 1 is future work | Domain split, store, migration, defaults-parity test and a libFuzzer target are merged; the fuzz target is **not wired into `meson.build`**; `kajo-ide` emergency buffer recovery is not in the diff. | §8 Phase 1 rewritten as status + open items. |
| 7 | Domain schemas in §4.3 | Shipped defaults differ (`ide.toml` `tab_width=4`, `show_minimap=true`, `show_margin_guide=true`; `files.toml` `show_inspector=true`; theme keys `font_heading_size_px`, …). | §4.3.4 now defers to `data/defaults/*.toml`. |
| 8 | (not covered) | `autohide_edge_strip_px` was functional before the PR and was dropped (`edge_strip_px = 2` hardcoded). The other dropped keys (`osd_*`, `launcher_*`, `settings_transition_ms`, `default_widget_mode`) had no readers (grepped the parent commit). | Noted in §8 Phase 1. |

### Revision 2 — what changed and why

Revision 1 assumed `wlroots` could host HDR10 **and** a custom effects pipeline (Dual-Kawase blur, SPIR-V window shaders, texture crossfades). Reading the `wlroots 0.20.2` and `sway 1.12` sources showed that it cannot without a renderer fork. Revision 2 keeps **HDR10 on the stock `wlroots` Vulkan renderer** as the one rendering goal and moves all eye candy to §10 (Deferred). Corrections made after checking claims against source, installed headers and the repo:

| # | Revision 1 claim | Finding (how verified) | Resolution |
| :-: | :--- | :--- | :--- |
| 1 | Per-surface `luminance_multiplier = sdr_nits / 203` through `wlr_scene` | `wlr_scene.c` derives the multiplier itself from fixed default luminances (sRGB ref 80, PQ ref 203). `wlr_scene_output_state_options.color_transform` "cannot be used when the output has a non-NULL image description" (`wlr_scene.h`). | SDR white is **fixed at 203 cd/m²** in v1. Adjustable SDR nits needs a wlroots patch → §3.2.3, §10. |
| 2 | Blur, SPIR-V open/close/resize shaders, snapshot textures in `wlr_scene` | `wlr/render/pass.h` exposes only `add_texture` and `add_rect`; `wlr_scene.h` has no blur/rounded/shadow. `scenefx` 0.5 depends on `mesa`/`libglvnd` (GLES-only), so it cannot coexist with the Vulkan HDR path. | Deferred to §10. |
| 3 | `wlr_vk_renderer` is used because we create it | `renderer_autocreate()` (`render/wlr_renderer.c`) tries **GLES2 first**; Vulkan only if GLES2 is unavailable or `WLR_RENDERER=vulkan`. | Create/force Vulkan explicitly and refuse HDR otherwise → §3.1. |
| 4 | FP16 or 10-bit swapchain, EDID mastering metadata populated | Sway 1.12 sets **only** `primaries=BT2020`, `transfer_function=PQ` and a 10-bit render format; mastering luminance / MaxCLL / MaxFALL are left zero. wlroots DRM code `abort()`s on any primaries other than none/BT2020. | v1 follows Sway exactly; FP16 and EDID metadata are experiments → §3.2.2. |
| 5 | `libliftoff` is part of our scanout design | It is an optional dependency **inside wlroots' DRM backend**; the Arch `libwlroots-0.20.so` links it (`ldd`). Our code never calls it. | Removed from our dependency list; noted as transitive. |
| 6 | Direct scanout works for fullscreen video/games | `color_management_is_scanout_allowed()` (`wlr_scene.c`): with an HDR output active only buffers whose transfer function and primaries match the output scan out. SDR fullscreen windows will **not** scan out on an HDR output. | Documented in §3.1 and §3.4. |
| 7 | `kajo-player` hits full HDR peak | `kajo-player` renders with `libmpv` into `GtkGLArea`. The installed GTK 4.24.1 headers expose `GdkColorState` only on texture builders, not on `GtkGLArea`. HDR pass-through for the player is **not established**. | Explicit gap → §3.2.5. |
| 8 | `config.c` is 1,210 lines | `wc -l src/config.c` = **1,068** (`config.h` 142). | Corrected in §5. |
| 9 | 18 custom GLSL shader presets | `data/shaders/` has **15** (`open` 6, `close` 6, `resize` 3), written against Niri's shader API. | Corrected; they are not portable and are deferred (§10). |
| 10 | Animation numbers "Niri-style" | Spring values match the user's live `~/.config/niri/config.d/30-layout-animations.kdl`. The `window_open` figures in rev. 1 (220 ms, `ease-out-expo`) do **not**: live config is `800 ms ease-out-quad`, close `350 ms linear`. Live layout also uses `gaps 6`, `center-focused-column "never"`, presets `0.33333 / 0.5 / 0.66667`. | `comp.toml` defaults now mirror the live config. |
| 11 | `tomlc17` is "~2,500 LOC" | Cloned upstream: `src/tomlc17.c` 3,350 + `tomlc17.h` 291 lines, MIT license. | Corrected in §4.1. |
| 12 | `compositor` is "~8,000 LOC" | Measured: `sway/` + `include/` of Sway 1.12 is **~44,300 LOC** (a full i3 clone). No measured basis exists for our figure. | Marked as an unmeasured estimate (§2, §3.8). |
| 13 | "18 of 22 widgets have zero compositor dependency" | A `grep` for `ShellCompositor`/`shell_compositor` matches nearly every widget file, so the claim could not be confirmed. | Unverified; audit before relying on it (§3.6). |
| 14 | Watchdog thread may call `libseat_disable_seat()` | Not verified safe from a non-main thread. | Redesigned → §7.5. |
| 15 | Nothing about text rendering | `wlr_scene` has no text. Sway uses Pango/Cairo (`sway_text_node.c`) for titles. Any in-compositor label (tab header, HUD readout) needs `pangocairo`. | Added to dependencies (§9). |

Every other statement in this document is either carried over from revision 1 (design intent) or explicitly marked **(unverified)**, **(planned)**, or **(deferred)**.

---

## 1. Executive Summary & Design Goals

Kajo evolved from a panel/shell for Niri into a complete pure-C Aero + Metro desktop suite (`kajo`, `kajo-settings`, `kajo-greeter`, `kajo-files`, `kajo-player`, `kajo-mod`, `kajo-ide`). Replacing Niri with a first-party Wayland compositor (**`kajo-comp`**, `org.kajo.Compositor`) and retiring the monolithic `~/.config/kajo/config.json` removes the remaining structural bottlenecks:

1. **HDR10 — moved to v2 (Revision 7).** v1 composites in SDR. The v1 rendering goal is correct SDR output on the stock `wlroots` scene graph with the automatically selected renderer. The HDR10 design in §3.2 is kept as the v2 starting point.
2. **Niri-style scrolling tiling with spring-driven movement:** horizontal column strip, vertical workspace stack, 2nd-order spring animation of camera and window **positions** (`workspace-switch`, `horizontal-view-movement`, `window-movement`). Resize crossfades, open/close effects, shader presets, backdrop blur and overview zoom are **not v1** (§10).
3. **Single-display focus across two co-equal form factors** (no multi-monitor spanning work):
   - **Host Profile A — 14" 16:10 Convertible OLED Laptop:** HP OmniBook X Flip 14 (`eDP-1`, Samsung `0x41AA`, `2880×1800 @ 120Hz`, scale `1.75` → `1646×1029` logical px, HDR10 `616 cd/m²` peak / `400 cd/m²` sustained / `0.0005 cd/m²` black — **panel figures carried over from rev. 1, not re-read from EDID in this revision**), Intel Lunar Lake Xe2, touchpad + touchscreen + IIO hinge/posture rotation + EC backlight.
   - **Host Profile B — 34" 21:9 Ultrawide Desktop:** external `DP-*` / `HDMI-A-*` (`3440×1440` or `5120×2160`, scale `1.0–1.25`, discrete GPU, HDR10, `vrr = "on-demand"`).
4. **Non-monolithic, hackable plain-text TOML configuration:** `~/.config/kajo/` split into small domain-owned `.toml` files plus `conf.d/*.toml` overrides, separating GUI-writable preferences from human-edited keybindings and window rules.

### Non-goals for v1 (explicit)

- Backdrop blur, rounded corners, shadows, SPIR-V/GLSL window effects, resize crossfade, close/open shader animations, overview zoom. (§10)
- **HDR10 output of any kind** (Revision 7), and therefore also: user-adjustable SDR white level (§3.2.3), HDR capture handling (§3.5.4), FP16, mastering metadata.
- HDR pass-through inside `kajo-player`'s `GtkGLArea`. (§3.2.5)
- Multi-monitor layout, `sway`/`scroll` as selectable backends in `kajo-session` (only `kajo-comp` and `niri` are in scope). **Exception:** greeter host mode enables every connected output and places them left to right, because `kajo-greeter` draws one surface per monitor (§6.5.4).

---

## 2. Why Pure C11 + `wlroots 0.20` (Vulkan)

| Option | Language | Engineering Verdict |
| :--- | :---: | :--- |
| **`wlroots 0.20.2` + Vulkan (`wlr_vk_renderer`)** | **Pure C11** | **Selected.** Aligned with Kajo's C11/Meson/ASAN stack. Installed (`wlroots0.20 0.20.2`, `wayland-protocols 1.50`). Provides the color-management protocols, KMS `HDR_OUTPUT_METADATA`, explicit sync, tearing, XWayland, layer-shell, session-lock, input-method. Sway 1.12 (`wlroots >=0.20.0 <0.21.0`) is the reference for the HDR10 path. **Size: unmeasured.** Sway's core is ~44k LOC because it implements i3 semantics, config language and IPC compatibility; `kajo-comp` v1 omits those but adds a spring engine, TOML config, rules and IPC. Plan for tens of thousands, not ~8k, until measured. |
| **`scenefx` on `wlroots`** | C | **Rejected for v1.** Adds blur/rounded/shadows, but is GLES-only (depends on `mesa`/`libglvnd`) and HDR10 exists only on the Vulkan renderer. Re-evaluate if it gains a Vulkan backend. |
| **Fork/patch `wlroots` Vulkan renderer** | C | **Deferred (§10).** Required for blur/effects; makes every `wlroots` release a rebase. |
| **C++ wrapping `wlroots`** | C++ | **Avoid.** `wlroots` headers use C99/C11 array-parameter declarators (`const float color[static 4]`) that need shims under C++. |
| **`Aquamarine` + custom C++** | C++23 | **Avoid.** Only abstracts DRM/KMS and `libinput`; Wayland protocols, XWayland, damage tracking and color management would all be reimplemented. |
| **`libkwin` / `Louvre` / `Mir`** | C++ | **Avoid.** `libkwin` pulls Qt6/KDE Frameworks; the others lack a mainline Vulkan HDR10 pipeline. |

---

## 3. `kajo-comp` Technical Architecture

### 3.1 Core `wlroots 0.20` Subsystem Map

| Subsystem | `wlroots 0.20` API / Protocol | Role in Kajo |
| :--- | :--- | :--- |
| **Backend & Seat** | `wlr_backend_autocreate()`, `libseat`, DRM, `libinput`, Wayland backend, headless backend | Bare-metal KMS/DRM via `greetd`; nested (`WLR_BACKENDS=wayland`) for development; `headless` for local automated tests. |
| **Renderer** | `wlr_renderer_autocreate()` (v1) | **v1 (SDR):** automatic selection (GLES2 first, then Vulkan, then pixman); `WLR_RENDERER` may override. Forcing Vulkan is only needed for HDR10, which is v2 (Revision 7). The headless backend can create a Vulkan renderer on Host A (§8.4), useful for tests. |
| **Render pass** | `wlr_render_pass_add_texture()` / `add_rect()` (via `wlr_scene`) | The **only** drawing primitives: sampled texture with alpha, source/dest box, transfer function, primaries, YCbCr encoding/range, luminance multiplier; and a solid rectangle. This is the rendering ceiling for v1. |
| **GPU Sync & Scanout** | `wlr_linux_drm_syncobj_v1`, `wlr_tearing_control_v1`, `wlr_output_layer` / scene direct scanout | Explicit sync; optional tearing flips; scanout is decided inside `wlr_scene`. `libliftoff` is a wlroots-internal optional dependency (the Arch build links it); `kajo-comp` does not call it. **HDR caveat:** with an HDR output active, only buffers whose transfer function and primaries equal the output's scan out; SDR fullscreen windows are composited. |
| **Scene Graph** | `wlr_scene`, `wlr_scene_tree`, `wlr_scene_rect`, `wlr_scene_buffer`, `wlr_scene_surface` | Automatic damage tracking. `wlr_scene_node_set_position()` takes **integer logical coordinates**: at scale 1.75 sub-pixel spring motion is quantised (see §3.3). 1px `0px`-radius borders via `wlr_scene_rect` (`#2b2b2b` inactive, `@metro-accent` focused, `#e81123` urgent). |
| **Color & HDR10** | `wlr_color_manager_v1_create()`, `wlr_scene_set_color_manager_v1()`, `wlr_color_representation_v1`, `wlr_output_state_set_image_description()` | Advertises supported transfer functions/primaries to clients (lists derived from the renderer, as in Sway `server.c`), and drives the output description. EDID parsing for capability flags is done **inside wlroots' DRM backend** via `libdisplay-info`. |
| **Shell Surfaces** | `wlr_xdg_shell`, `wlr_layer_shell_v1`, `wlr_session_lock_v1` | Hosts all GTK4 windows, the `kajo` panel/launcher/OSD/notifications, `kajo --lock`, `kajo-greeter`. |
| **X11 Compatibility** | `wlr_xwayland` | Native XWayland (Steam, Wine/Proton, legacy X11). Requires `xorg-xwayland` and the XCB libraries wlroots already links. |
| **OSK & Text Input** | `wlr_input_method_v2`, `wlr_virtual_keyboard_v1`, `wlr_text_input_v3` | Relay for `ShellOSK` (`src/osk/osk.c`). Window push-up geometry in tablet mode is **(planned)**. |
| **Scaling & Capture** | `wlr_fractional_scale_v1`, `wlr_viewporter`, `wlr_ext_image_copy_capture_v1`, `wlr_ext_image_capture_source_v1`, `wlr_screencopy_v1` | Fractional scale `1.75` / `1.0` / `1.25`. Window capture uses `wlr_ext_image_capture_source_v1_create_with_scene_node()` (the call Sway 1.12 uses in `sway/server.c`). |
| **Idle & DPMS** | `wlr_idle_notify_v1`, `wlr_idle_inhibit_v1`, `wlr_output_power_management_v1` | Drives `src/lock/screensaver.c`, DPMS timers, pre-suspend lock. |

---

### 3.2 HDR10 & Mixed SDR/HDR Color Pipeline (stock `wlroots 0.20.2`) — **v2, not in v1 (Revision 7)**

Everything below was checked against `wlroots` tag `0.20.2` (`backend/drm`, `types/scene/wlr_scene.c`, `render/color.c`, `render/vulkan`) and Sway 1.12 (`sway/config/output.c`, `sway/server.c`). `kajo-comp` adds a small output-policy module `src/comp/comp_color.c`; the size of that module is an **unmeasured estimate**.

#### 3.2.1 Capability probe and enabling
1. wlroots' DRM backend fills `wlr_output.supported_primaries` / `supported_transfer_functions` when it creates the connector:
   - `BT2020` is set if the connector has a `Colorspace` property **and** the EDID (parsed with `libdisplay-info`) signals BT.2020 colorimetry.
   - `ST2084_PQ` is set if the connector has `HDR_OUTPUT_METADATA` **and** the EDID HDR static metadata advertises PQ.
2. `kajo-comp` enables HDR for an output only when `hdr = true` in `comp.toml` **and** BT2020 and PQ are both supported **and** `renderer->features.output_color_transform` is true (identical to Sway's `output_supports_hdr()`). Otherwise it stays SDR and posts a notification naming the missing capability. It never pretends HDR is on.
3. When enabled, in one atomic output state: `wlr_output_state_set_render_format(DRM_FORMAT_XRGB2101010)` (10-bit, as Sway does) and `wlr_output_state_set_image_description({ .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020, .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ })`.

#### 3.2.2 What is deliberately *not* in v1
- **EDID mastering luminance / MaxCLL / MaxFALL:** the DRM backend would forward them into `HDR_OUTPUT_METADATA` (`atomic.c`), but Sway leaves them zero. Whether setting them to the panel's `616 / 400 / 0.0005` values improves or worsens the panel's own tone mapping is **untested**; it is a Phase 3 experiment, off by default.
- **FP16 swapchain (`ABGR16161616F`):** present in the Vulkan renderer's format table and in the DRM `max_bpc` table, but not used by Sway and **not verified** to scan out on Intel Xe2 or the desktop GPUs. v1 uses 10-bit; FP16 is an experiment.
- **Any primaries other than BT.2020:** `convert_primaries_to_colorspace()` in the DRM backend calls `abort()` for anything but "none" or BT2020. `kajo-comp` must never pass other primaries in an output image description.
- **Compositor-side tone mapping** (e.g. BT.2390 knee): no tone-mapping code exists in `wlroots 0.20.2` render/types (searched). Content brighter than the panel is left to the display's own tone mapping. Deferred.

#### 3.2.3 SDR content: fixed 203 cd/m² white
`wlr_scene` converts untagged/sRGB buffers to the output description with `(dst.reference / src.reference) × (src.max / dst.max)`, using wlroots' built-in defaults (sRGB reference/max 80, PQ reference 203, max 10000). In v1 SDR white is therefore **203 cd/m²**, matching ITU-R BT.2408, and cannot be changed at runtime.
- The `sdr_brightness_nits` key and the planned Settings slider are **not implemented in v1**. They stay out of `comp.toml` (or, if present, are ignored with a logged warning) and out of the UI.
- Making it adjustable needs a small patch to `get_luminance_multiplier()`/the scene's output color transform (carried in `PKGBUILD`, or proposed upstream). That decision is **open** (§10).
- Consequence for Host B: external HDR monitors usually lock hardware brightness while HDR is on, so v1 offers **no** software brightness control for them. `/dev/i2c-*` DDC/CI (`ddcutil`) is a separate, **(planned)** feature and only valid with `hdr = false`.
- Laptop `eDP-1` brightness continues to use `/sys/class/backlight/*`.

#### 3.2.4 HDR content
Clients that use `wp-color-management-v1` (browsers, Vulkan/Proton games, `mpv --vo=gpu-next` standalone) attach a transfer function and primaries; `wlr_scene` records them on the `wlr_scene_buffer` and the Vulkan shaders convert into the output space. A fullscreen PQ/BT.2020 surface on an HDR output is eligible for direct scanout.

#### 3.2.5 Known gap: `kajo-player`
`kajo-player` draws through `libmpv` into a `GtkGLArea` (`src/player/player_mpv.c`). In the installed GTK 4.24.1 headers, `GdkColorState` is exposed only on texture builders (`gdkdmabuftexturebuilder.h`, `gdkgltexturebuilder.h`, …), not on `GtkGLArea`. Treat HDR output from `kajo-player` as **unsupported** until a path is demonstrated (candidates, all untested: a `GdkDmabufTexture` with a color state fed from mpv, or a standalone `mpv --vo=gpu-next` window). The compositor-side HDR pipeline does not depend on this.

---

### 3.3 Scrolling-Tiling Engine (14" 16:10 Laptop + 34" 21:9 Ultrawide)

#### Scene Tree Hierarchy
```text
root (wlr_scene)
├── layer_background       (zwlr_layer_shell_v1: BACKGROUND)
├── layer_bottom           (zwlr_layer_shell_v1: BOTTOM)
├── workspace_viewport     (active workspace; camera_x animated by the spring engine)
│   └── column[0..N-1]     (x_pos, target_width_px, tabbed_mode)
│       └── window[0..M-1] (1px 0px-radius border wlr_scene_rect + wlr_scene_surface + xdg popups)
├── floating_tree          (centered modals, PiP, calculators, Steam toasts, 🗗 FLOAT windows)
├── layer_top              (kajo panel, launcher)
├── fullscreen_tree        (direct-scanout eligible when a single matching surface covers the output)
├── layer_overlay          (kajo OSD, notifications, Alt+Tab switcher, hotkey overlay)
└── session_lock_tree      (ext-session-lock-v1: blocks all trees below when locked)
```

#### Critical Layout Rules for Both 16:10 (`1646px`) and 21:9 Ultrawide (`3440px+`)
1. **`always_center_single_column = true`:** when a workspace contains a single column—or when the sum of all column widths + gaps is narrower than the logical viewport width—center the strip horizontally instead of pinning it to `x = 0`.
2. **`center_focused_column` modes (`"never" | "on-overflow" | "always"`):** the live Niri config uses `"never"`, so that is the default in `comp.toml`. `"on-overflow"` pans minimally so the focused column is fully visible with `gap_px` margin; `Mod+C` centers the active column; `Mod+Ctrl+C` (`center-visible-columns`) centers the visible 2–3 column group.
3. **Width presets `[0.33333, 0.5, 0.66667]` via `Mod+R`** (live-config values). A `0.25` stop may be added by the user. Widths are proportions of the viewport **minus gaps**, so on the 3440px ultrawide three `0.33333` columns are ≈1,147px each **before** gap deduction (rev. 1's "1140px" was a rounding; the exact value depends on `gap_px`); `0.25 + 0.5 + 0.25` gives `860 + 1720 + 860` before gaps. On the 14" laptop (`1646px`) `0.5 + 0.5` gives two split columns.
4. **VRR tri-state (`"off" | "on" | "on-demand"`):** `"on-demand"` keeps the desktop at fixed max refresh and enables adaptive sync (`wlr_output_state_set_adaptive_sync_enabled`) only while a fullscreen window, or a window matching a rule with `vrr = true`, is visible. This is compositor policy, not a `wlroots` feature.
5. **Window lifecycle (v1 scope):**
   - States: `WIN_OPENING` → `WIN_LIVE` → `WIN_MOVING` → `WIN_CLOSING`.
   - **Animated in v1:** camera/strip scroll, workspace switch and window/column **position** changes, all driven by one analytical 2nd-order spring engine (`damping_ratio`, `stiffness`, `epsilon`) that **preserves instantaneous velocity when retargeted mid-flight** (verify with a unit test: two retargets within one frame must not discontinuously change velocity). Parameters are the user's live Niri values:
     - `workspace_switch`: `damping_ratio = 0.80`, `stiffness = 523`, `epsilon = 0.0001`
     - `horizontal_scroll`: `damping_ratio = 0.85`, `stiffness = 423`, `epsilon = 0.0001`
     - `window_movement`: `damping_ratio = 0.75`, `stiffness = 323`, `epsilon = 0.0001`
   - **Resize:** the client receives the final size immediately; neighbours slide by spring. There is **no** prev/next crossfade (`wlr_scene` cannot blend two textures with a progress value). Window content may therefore jump to its final size while the frame moves.
   - **Open:** window appears in place; an opacity fade via `wlr_scene_buffer_set_opacity()` on the window's buffers is **(planned)**.
   - **Close:** no animation. An unmapped client surface cannot be drawn by `wlr_scene`, and holding a frozen snapshot requires a texture copy; both are **(deferred)**.
   - **Sub-pixel limitation:** `wlr_scene_node_set_position()` takes `int` logical coordinates. At scale `1.75` a logical pixel is 1.75 physical pixels, so spring motion is quantised more coarsely than on a `scale = 1.0` desktop. Whether this is visibly worse than Niri must be judged in `scripts/ui-shot.sh`-style screenshots/video, **not assumed**.
6. **Overview (`toggle-overview`) is not v1.** Zooming a column tree needs per-buffer destination sizes for every surface and subsurface; `wlr_scene` has no tree transform. Bindings that reference it are commented **(planned)** in `binds.toml`.
7. **Direct in-process input & tablet posture:** bezel swipe gestures, 3-finger touchpad swipes, discrete wheel scrolling (`cooldown_ms = 150`) and `0°/90°/180°/270°` tablet rotation run natively inside `kajo-comp`. Today `src/hardware/touch_gestures.c` opens `/dev/input/event*` itself (multitouch slots, bezel ratio) and forwards Niri JSON actions with `niri_send_action()`; `kajo-comp` would use its `libinput` touch events instead. The 2px (6px when touch gestures are on) reveal strip belongs to the autohide panel (`src/autohide.c`), not to `touch_gestures.c`, and is only removable if autohide reveal is also driven by the compositor. `src/posture/posture_manager.c` currently spawns `niri msg output eDP-1 transform`. While both backends are supported (Phases 1–3) the existing client-side paths stay active under Niri and are disabled only when `kajo-comp` reports it handles them. Gestures that map to overview are inert until overview exists.

---

### 3.4 Window Decorations (`prefer-no-csd`) & GPU Buffer Sharing

1. **Buffer sharing:** GTK4 (`ngl`/`vulkan`), Kitty, browsers and VA-API/NVDEC decoders hand `dmabuf` fds to `wlr_vk_renderer` via `linux-dmabuf-v1` and `linux-drm-syncobj-v1`. Hardware cursor plane use and fullscreen direct scanout are handled by wlroots/`wlr_scene`. **HDR caveat:** direct scanout only for buffers matching the output's colorimetry (§3.1).
2. **Server-side decoration negotiation (`wlr_xdg_decoration_manager_v1` + `wlr_server_decoration`):** advertise `SERVER_SIDE` (`prefer-no-csd`) and set `tiled` edges on all windows:
   - Kajo suite apps keep their frameless Aero+Metro `GtkWindowHandle` chrome.
   - Terminals, browsers, Qt, Electron, MPV and XWayland drop toolkit titlebars.
   - Libadwaita/GTK4 CSD-only utilities honour `tiled` by stripping rounded corners and shadows (behaviour of those toolkits; **unverified per app**).
3. `kajo-comp` draws a `1px`/`2px` `0px`-radius `wlr_scene_rect` frame (`@metro-accent` active, `#2b2b2b` inactive, `#e81123` urgent). A tabbed-column header (`Mod+W`) needs **text rendering**, which `wlr_scene` does not provide; use `pangocairo` rendered into a `wlr_buffer` (as Sway's `sway_text_node.c` does) or ship a side-indicator without text first.

---

### 3.5 Screenshots (Area / Window / Full Output)

Rather than shelling out to `grim` + `slurp`, `kajo` provides screenshots through the capture protocols `kajo-comp` exposes. Which parts are built in the compositor and which in a client is a decision made for v1 below.

1. **`screenshot-window` (`Alt+Print`):** `wlr_ext_image_capture_source_v1_create_with_scene_node()` on the focused window's scene node, as Sway 1.12 does, captured through `ext-image-copy-capture-v1`. Whether the captured tree includes open popups and the `1px` border, and whether overlapping notifications are excluded, is **unverified** and must be tested.
2. **`screenshot-screen` (`Ctrl+Print`):** capture the output through `ext-image-copy-capture-v1`, then PNG-encode and write **off the compositor's event-loop thread** (client side or worker thread).
3. **`screenshot` (`Mod+Shift+S` / `Print`), interactive HUD:** implemented as a layer-shell client **`kajo-capture`** (GTK4, Aero+Metro) that first captures the output, then displays the frozen frame with a dim overlay, a `1px` `@metro-accent` selection rectangle, a pixel-size readout and `[ AREA ] [ WINDOW ] [ SCREEN ]` mode switch (`A`/`W`/`S`/`Tab`; `Enter`/`Space` copy + save; `Ctrl+C` copy only; `Escape` cancel). Rationale: an in-compositor HUD would need `pangocairo` text, input grabs and a mode state machine inside `kajo-comp`; a client keeps the compositor small. An in-compositor HUD remains an option if latency of the client proves visible.
4. **HDR correctness is unverified.** On an HDR10 output the captured buffer's encoding is not established in this revision. Before any screenshot feature is called supported, test on a real HDR output: (a) SDR UI colours round-trip (`#111111`, `#0078d4`), (b) what pixel format and transfer function the capture returns. If it returns PQ/BT.2020, conversion to sRGB (clip or BT.2390) is client-side work that must be written and tested. Clipboard (`image/png`) and `screenshots.path` strftime save are client features.

---

### 3.6 Full Kajo Bar, CSS & Widget Parity

1. **GTK4 CSS (`style.css` + `style.d/00-accent.css`):** rendered by GTK into premultiplied-alpha dmabufs; `wlr_scene` composites them unchanged.
2. **Frosted glass is degraded in v1.** Today `~/.config/niri/config.d/40-layer-rules.kdl` gives the Kajo namespaces (`kajo`, `kajo-launcher`, `kajo-notification`, `kajo-osk`, and their popups) `opacity 0.85` plus `blur true`, `xray false`. In v1 `kajo-comp` implements the `opacity` part only (via `wlr_scene_buffer_set_opacity()` on layer and popup buffers). **Blur is not rendered**, so panels look translucent rather than frosted and legibility over busy windows is worse. The Niri `blur` setting remains a visible gap until §10 is done; `style.css` may need a more opaque fallback while running under `kajo-comp`.
3. **`zwlr_layer_shell_v1` behaviours used by `kajo`** (from the code): animated margin commits in `src/autohide.c`; exclusive zones in `src/panel.c`, `src/osk/osk.c` (dynamic strut when `osk_push_windows = true` is **(planned)**), `src/lock/screensaver.c`; keyboard interactivity `EXCLUSIVE` for the launcher (`launcher_surface.c:650`) and `NONE` for the OSK (`osk.c:839`).
4. **`ShellCompositor` IPC contract (`src/compositor/compositor.h`):** `shell_compositor_*` already has compositor-neutral names (`get_workspaces`, `get_windows`, `focus_window`, `focus_app`, `close_window`, `switch_workspace`, `switch_keyboard_layout_idx`, change callbacks); only `niri_send_action`, `niri_toggle_window_floating`, `niri_reload_config`, `niri_send_ipc_request` carry the Niri name. Which of the 22 widgets actually depend on the compositor was **not established** (a grep matches almost every widget file through the shared header). Run an audit before claiming how many are independent. The event/request schema for `$XDG_RUNTIME_DIR/kajo-comp.sock` (`WorkspacesChanged`, `WindowsChanged`, `WindowFocusChanged`, `KeyboardLayoutsChanged`; `switch-workspace`, `focus-window`, `focus-app`, `close-window`, `toggle-floating`, `switch-keyboard-layout`) must be written down as a document and covered by a mock-compositor test modelled on `tests/test_niri.c` **before** the Niri backend is retired.

---

### 3.7 Real-World Application Compatibility & XDG Portal Checklist

To prevent everyday breakage across games, browsers, Electron apps, VMs, and Flatpaks, `kajo-comp` must wire the following from day one:

1. **XDG Desktop Portal & Zero Startup Timeout (`xdg-desktop-portal-wlr` + `kajo-portals.conf`):**
   - Install `xdg-desktop-portal-wlr` alongside `xdg-desktop-portal-gtk`, and ship `data/kajo-portals.conf` (`default=gtk`, `org.freedesktop.impl.portal.ScreenCast=wlr`, `org.freedesktop.impl.portal.Screenshot=wlr`, `org.freedesktop.impl.portal.Secret=gnome-keyring`).
   - Expose `wlr_screencopy_v1`, `wlr_ext_image_copy_capture_v1`, `wlr_output_management_v1`, and `wlr_xdg_output_v1` for OBS / Discord / Zoom / WebRTC PipeWire screen sharing.
   - Export `XDG_CURRENT_DESKTOP=kajo` and invoke `dbus-update-activation-environment --systemd WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP` as soon as sockets initialize so GTK/Electron/Flatpak apps never stall on a 25-second D-Bus portal timeout.
2. **3D Games, Proton/Steam & Blender (`pointer-constraints-v1` + `relative-pointer-v1`):**
   - Wire `wlr_pointer_constraints_v1` (pointer lock & confine) and `wlr_relative_pointer_v1` (raw unaccelerated deltas) so FPS cameras, WebGL pointer-lock, and Blender 3D viewport navigation never hit screen edges.
3. **Cross-Process Portal Dialog Parenting (`xdg-foreign-v2` + `xdg-dialog-v1`):**
   - Enable `wlr_xdg_foreign_registry` + `wlr_xdg_foreign_v2` so native GTK file-chooser dialogs spawned by `xdg-desktop-portal-gtk` on behalf of Firefox, Chromium, VS Code, or Slack parent and float centered above the calling window rather than tiling into a separate column.
4. **Media & Call Idle Inhibition (`idle-inhibit-v1`):**
   - Track mapped/visible `wlr_idle_inhibitor_v1` surfaces (Firefox/Chromium YouTube, `kajo-player`/`mpv`, Steam games, Discord/Zoom calls) and toggle `wlr_idle_notifier_v1_set_inhibited()` so `kajo-screensaver` and DPMS never fire mid-playback.
5. **Crisp Native-DPI XWayland on HiDPI Outputs (`xwayland.force_zero_scaling`):**
   - On `scale = 1.0` (34" desktop), XWayland is 1:1 automatically. On `scale = 1.75` (14" OLED), support `force_zero_scaling = true` (advertising native `2880×1800` to XWayland + setting `Xft.dpi` via `xrdb`) so DPI-aware X11 apps (Steam, Wine) render razor-sharp instead of bilinearly stretched.
6. **Essential Client Toolkit Globals:**
   - `wlr_single_pixel_buffer_v1` + `wlr_viewporter` + `wlr_subcompositor` + `wlr_alpha_modifier_v1` (required by GTK4, `wp-fractional-scale-v1`, Firefox video subsurfaces, and `mpv` letterbox bars).
   - `wlr_presentation_time` (`wp-presentation` for `mpv`/`kajo-player` A/V sync and Vulkan `VK_KHR_present_wait`).
   - `wlr_pointer_gestures_v1` (forwards 2-finger touchpad pinch/rotate to browsers and PDF/image viewers).
   - `wlr_keyboard_shortcuts_inhibit_v1` (lets QEMU/`virt-manager` and RDP/Moonlight capture `Alt+Tab` / `Mod+*`, with `Mod+Escape` force-ungrab).
   - `wlr_tablet_v2` (stylus pressure/tilt for HP OmniBook `ELAN2514` pen in Krita/Inkscape/Rnote, rotated with output transform).
   - `wlr_drm_lease_v1` (VR headset KMS leasing on desktop).
7. **Session Environment & Security Agents (`[env]` & `[autostart]`):**
   - Set `ELECTRON_OZONE_PLATFORM_HINT = "auto"` so Electron/Chromium apps (VS Code, Discord, Slack, Signal, Obsidian, Brave/Thorium) run native Wayland + color management automatically.
   - Autostart `gnome-keyring-daemon`, `/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1` (required for `kajo-files` UDisks2 LUKS/ISO loop mounting), and `kajo`.

---

### 3.8 Clean-Slate Strategy, MIT Reference Implementations & Zero-Leak Memory Model

#### 1. Clean-Slate Core + Proven MIT Protocol Reference (`sway` / `dawsers/scroll`)
- **Do NOT fork `sway`/`scroll`'s recursive container tree (Sway 1.12's `sway/` + `include/` measure ~44,300 lines; `scroll`'s size and whether it bundles a `wlroots` fork were not checked here, though its author says in discussion #229 that he modified the `wlroots` renderer) or any `wlroots` fork:** Keep `kajo-comp` a clean codebase (size not measured; see §2) linked directly against mainline system `libwlroots-0.20.so`.
- **DO reference/adapt MIT-licensed protocol handlers from `sway` and `dawsers/scroll`:** Both are MIT-licensed (`scroll`'s LICENSE read; it carries Drew DeVault's copyright). Use their implementations as direct references (sizes vary: Sway's `input/tablet.c` is 376 lines, `desktop/idle_inhibit_v1.c` 194) for subtle Wayland edge cases (`pointer-constraints-v1`, unscaled `xwayland`, `xdg-foreign-v2`, `idle-inhibit-v1`, `tablet-v2`, `input-method-v2` relay, and `keyboard-shortcuts-inhibit-v1`).

#### 2. Why Conservative Tracing GC (`bdw-gc`) Is Rejected—and Why `talloc` Wins
A conservative tracing GC (`libgc` / `bdw-gc`) clashes badly with `wlroots` and Vulkan:
- `wlroots` stores pointers to compositor structs inside its own `malloc` heap via `void *data` and intrusive `struct wl_listener` doubly-linked lists—invisible to `bdw-gc` root scanning unless manually pinned, risking premature collection and list corruption.
- Most compositor "leaks" are **not** heap bytes—they are forgotten `wl_list_remove(&listener.link)` calls (instant crash on signal emission), GPU `wlr_texture` / `dmabuf` fd leaks, and `wlr_scene_node` leaks that require **strict, deterministic single-thread teardown order**.
- Tracing GC introduces unpredictable collection pauses on a 120Hz/240Hz frame loop and conflicts with Kajo's `AddressSanitizer` (`scripts/asan-test.sh`) gate.
- `talloc` and the Wayland/wlroots lifetimes are separate systems: `talloc_free()` of a `CompWindow` must not free a `wlr_scene_node` still referenced by wlroots. Free order is explicit: destroy the scene node (`wlr_scene_node_destroy`) in the destructor **before** the parent context releases siblings, and never let a `talloc` parent outlive the `wl_display`.

**Licence note (§9.8):** `talloc` is **not** MIT. Its installed header states LGPL version 3 or (at your option) any later version; the Arch package metadata says GPL-3.0-or-later, which disagrees with the header and is probably a packaging simplification (not verified against the upstream tarball). It is linked dynamically, so under the source-only policy in §9.8 it is acceptable.

Instead, `kajo-comp` uses **`talloc` (`libtalloc` — hierarchical ownership pool allocator with deterministic destructors)** paired with **Generational Slot IDs** and a **Per-Frame Bump Arena**:

```text
CompServer (root talloc context)
├── CompConfig              # Reloaded atomically: talloc_free(old_cfg) frees ALL strings, rules, binds & regexes in 1 call
├── CompOutput              # Freed on unplug -> destructor releases swapchain state & color transforms
├── CompWorkspace[0..W-1]
│   └── CompColumn[0..C-1]
│       └── CompWindow      # talloc_free(win) runs destructor -> unlinks ALL wl_listeners & destroys its scene_tree
│           ├── title / app_id (talloc_strdup(win, ...))
│           ├── CompPopup[0..P-1]
│           └── CompAnimState   (spring state; auto-freed with the window, even mid-animation)
├── CompLayerSurface
├── CompIpcClient           # Socket EOF -> talloc_free(client) frees buffers, write queue & event subscriptions in 1 call
└── frame_scratch_arena     # Fixed 64 KB bump buffer reset at the end of every wlr_output frame (design intent; not benchmarked)
```

#### 3. Four Structural Rules That Prevent Memory Leaks & Use-After-Free Bugs by Construction
1. **Auto-Unlinking `comp_listen()` Helper (~20 LOC over `talloc`):**
   - Never embed raw `struct wl_listener` fields that must be manually cleaned up in 12 places. `comp_listen(owner_ctx, &wlr_obj->events.foo, fn)` allocates a child listener on `owner_ctx` whose `talloc` destructor **automatically calls `wl_list_remove(&l->link)`** whenever `owner_ctx` is freed. Forgetting `wl_list_remove()` becomes structurally impossible.
2. **Fixed-Depth Strongly Typed Layout Structs (No Recursive `sway_container` God-Struct):**
   - Sway/i3 use a single recursive `container` union for outputs, workspaces, splits, columns, and windows. Because `kajo-comp` is strictly scrolling-tiling, the hierarchy is flat and strongly typed: `CompOutput` $\rightarrow$ `CompWorkspace` $\rightarrow$ `CompColumn` $\rightarrow$ `CompWindow` (plus `CompOutput.floating_windows`).
3. **`uint64_t` Generational Handle IDs (`{ uint32_t slot, uint32_t gen }`) Across IPC, Focus History & Timers:**
   - Never store raw `CompWindow *` pointers inside IPC requests, `Alt+Tab` MRU lists, timers, or async screenshot tasks. Store a `uint64_t` generational ID resolved in `O(1)` via `comp_win_from_id(server, id)`—returning `NULL` cleanly if that window closed 1ms earlier.
4. **Live Zero-Overhead Leak Introspection (`kajo-comp msg debug-mem`):**
   - Expose `talloc_report_full(server, ...)` over debug IPC to print the exact live ownership tree and byte count of every allocation in the running compositor in `< 1 ms`.

---

## 4. Domain-Split TOML Configuration Specification

### 4.1 Parser & Serialization Strategy

- **Parser:** `src/vendor/tomlc17/tomlc17.{c,h}` is the **upstream `cktan/tomlc17` source** (3,350 + 291 lines; header carries `Copyright (c) 2024-2026, CK Tan` and `TOMLC17_RELEASE_AFTER "260618"`). A document with a datetime loads. I did not re-run my 50-case probe against it and did not test its TOML 1.1 features; the project documentation says "TOML 1.1" and that is **unverified**. `kajo-comp` links the same file. A parse error still rejects the whole file.
- **Licence housekeeping:** upstream is MIT. `src/vendor/tomlc17/LICENSE` and a README credit were added on branch `revert/poppler-removal` (§9.8); until that branch is merged, `master` lacks both.
- **Library split:** `src/config/kajo_config_schema.c` (table-driven field engine) is GLib-only and reusable by `kajo-comp`. `kajo_config_store.c` (snapshots, `GFileMonitor`, `g_timeout_add`, `GMutex`) assumes a GLib main loop and is **not** usable inside a `wl_event_loop`-only compositor (§4.2.2).
- **Wire IPC vs Config Separation:**
  - **Human Configuration (`~/.config/kajo/*.toml`):** 100% TOML.
  - **Runtime State (`~/.local/state/kajo/stats.json`):** Ephemeral telemetry/history moved out of `~/.config/kajo/` into `XDG_STATE_HOME`.
  - **Live IPC Sockets (`$XDG_RUNTIME_DIR/kajo-comp.sock`, `kajo-ide` IPC):** Keep `json-glib` JSON-RPC / newline-delimited JSON strictly on Unix sockets where machine-to-machine serialization belongs.

### 4.2 Directory Layout & Strict Ownership Contract

```text
~/.config/kajo/
├── theme.toml          # [GUI + Human] IMPLEMENTED: accent & typography tokens
├── comp.toml           # [GUI + Human] PLANNED: layout, animations, input, [output.*] (not safely GUI-writable yet, see 4.2.1)
├── binds.toml          # [HUMAN ONLY]  PLANNED: key, wheel, media & switch bindings (never overwritten by GUI)
├── rules.toml          # [HUMAN ONLY]  PLANNED: window & layer rules with raw single-quoted regexes (never overwritten)
├── shell.toml          # [GUI + Human] IMPLEMENTED: panel, autohide, widgets, toasts, touch, OSK, lock, posture, kbd backlight
├── ide.toml            # [IDE / GUI]   IMPLEMENTED: color scheme, font, tab width, wrap, minimap, margin guide
├── files.toml          # [Files Only]  IMPLEMENTED: view mode, sort, hidden files, inspector
├── mod.toml            # [Mod / GUI]   IMPLEMENTED: Paula filter, SID model, stereo separation, HVSC path
├── style.css           # [Theme]       GTK4 Aero+Metro stylesheet
├── style.d/            # [Theme]       Generated 00-accent.css & user CSS snippets
└── conf.d/             # [HUMAN ONLY]  IMPLEMENTED: lexicographic drop-ins using top-level domain tables, e.g. [shell.panel], [theme]
```

**Why this contract never breaks:**
1. **Write races (verified fixed):** each domain is its own file written with `g_file_set_contents()`. `update_domain_generic()` now takes an in-process mutex and an exclusive `flock()` on a sidecar `.<file>.lock` for the whole read-modify-write. Test results: two threads × 100 updates to different keys of `theme.toml` ended at exactly +100 each; two processes × 150 updates ended at exactly +150 each.
2. **Comments in GUI-owned files (fixed, with one limit):** saves patch the existing text, so comments, unknown keys and trailing `# …` survive. Files that use quoted keys, dotted keys or inline tables cannot be patched; for those the original is saved to `<file>.bak` and a regenerated file is written (Revision 6). Comments inside the `[layout]` widget arrays are lost on a layout save. `binds.toml`, `rules.toml` and `conf.d/*.toml` are never written by a GUI.
3. **Instant Hot-Reload:** `kajo` watches `~/.config/kajo/` via `GFileMonitor`. `kajo-comp` runs on `wl_event_loop`, **not** a `GMainLoop`, so it watches the directory with a raw `inotify` fd registered through `wl_event_loop_add_fd()` (GIO file monitors need a `GMainContext` and must not be introduced into the compositor's loop). In the GUI processes, `KajoConfigStore` debounces (50 ms) and swaps refcounted snapshots, so saving a domain file re-applies it. Hot-reload of layout/keybindings/rules inside `kajo-comp` is **(planned)**; there is no `sdr_brightness_nits` (§3.2.3).
4. **Git-Synced Dotfiles Across Laptop & 34" Desktop:** Because `[output."..."]` sections in `comp.toml` only activate when a matching connector or EDID `Make Model` is plugged in (and laptop sensor sections in `shell.toml` are gated by `ShellHwCaps`), **one identical `~/.config/kajo/` directory works cleanly on both the 14" OmniBook and the 34" Desktop PC**, with `conf.d/10-local.toml` available for any machine-local tweak. **Caveat for `comp.toml`:** this only holds if a GUI save never deletes the `[output.*]` tables of the other host (see 4.2.1).

#### 4.2.1 Constraints the implemented config layer places on `comp.toml` (status after PR #32)

1. **Whole-file regeneration: fixed.** Saves patch scalars in place; if the patched result would not parse or would differ from the intended values, the original is kept in `<file>.bak` and a regenerated file is written, so a broken file is never produced (Revision 6, covered by a test). Cost: such a file loses its comments and unknown keys in the live copy.
2. **Syntax error → defaults then overwrite: fixed** (snapshot keeps the last good value; saves are refused with a clear error). **Reproduced and covered by a test.**
3. **Fixed-field schema.** `KajoFieldSpec` still describes scalars at fixed offsets. Dynamic tables (`[output."DP-1"]`, `[[rule]]`) would need custom hooks like `shell.toml`'s widget arrays, and a `comp.toml` schema must retain output tables for hardware that is not attached, or a save on one host deletes the other host's section. Not addressed.
4. **Double formatting: fixed.** Doubles are now written with `%.15g` (`kajo_config_schema.c:733`); I did not exercise a double field in `comp.toml` round trips.
5. **Mutators:** the displays page now gathers UI state first and mutates a plain struct. Keep that pattern.

**Recommended structure (proposal, unchanged):** keep `comp.toml`, `binds.toml`, `rules.toml` human-owned and never GUI-written; let `kajo-settings` write only a separate GUI-owned file (for example `outputs.toml`). With the syntax-error refusal and the comment-preserving patcher this is less urgent than before, but the quoted/dotted/inline-table defect and the dynamic-table problem still argue for it.

#### 4.2.2 Using the config library from `kajo-comp`

`KajoConfigStore` cannot run inside the compositor's `wl_event_loop` (it uses `GFileMonitor`, `g_timeout_add` and a `GMainContext`). `kajo-comp` should link the GLib-only schema engine and parser, read files directly, watch the directory with `inotify` on the `wl_event_loop`, and swap an immutable struct on reload. `KAJO_CONFIG_DIR` is already honoured by the store and should be honoured by `kajo-comp` too.

---

### 4.3 Reference TOML Schemas

#### 1. `~/.config/kajo/comp.toml` (Portable Laptop + 34" Ultrawide Config)
```toml
# Kajo Compositor Configuration — Outputs, HDR10, Input & Scrolling Layout

[layout]
gap_px = 6                              # matches live Niri config (`gaps 6`)
border_px = 1
active_border_color = "accent"          # Tracks @metro-accent from theme.toml, or explicit "#0078d4"
inactive_border_color = "#2b2b2b"
urgent_border_color = "#e81123"
always_center_single_column = true      # Centers single/narrow columns on 34" 21:9 ultrawide
center_focused_column = "never"         # live Niri value; "never" | "on-overflow" | "always"
default_column_width = 0.5
preset_widths = [0.33333, 0.5, 0.66667]   # live Niri presets; add 0.25 if wanted
preset_heights = [0.33333, 0.5, 0.66667, 1.0]

[animations]
enabled = true
workspace_switch    = { damping_ratio = 0.80, stiffness = 523, epsilon = 0.0001 }
horizontal_scroll   = { damping_ratio = 0.85, stiffness = 423, epsilon = 0.0001 }
window_movement     = { damping_ratio = 0.75, stiffness = 323, epsilon = 0.0001 }
# Not in v1 (deferred, see section 10): window_resize crossfade, window_open / window_close
# effects and shader presets. Unsupported keys log a warning and are ignored.
# (Live Niri values for reference: open 800 ms ease-out-quad, close 350 ms linear.)

[switcher]
active_color = "accent"
urgent_color = "#e81123"

[screenshots]
path = "~/Pictures/Screenshots/Screenshot from %Y-%m-%d %H-%M-%S.png"   # "" to disable disk save by default
copy_to_clipboard = true
show_notification = true
include_window_border = true

[xwayland]
enabled = true
force_zero_scaling = true               # Crisp native DPI for Steam/Wine on 1.75x HiDPI laptop (unverified)

[env]
XDG_CURRENT_DESKTOP = "kajo"
ELECTRON_OZONE_PLATFORM_HINT = "auto"

[autostart]
commands = [
    ["gnome-keyring-daemon", "--start", "--components=secrets"],
    ["/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1"],
    ["kajo"],
]

[input.keyboard]
xkb_layout = "us"                       # Or comma-separated "us,fi"
xkb_options = ""
numlock_on_startup = true
repeat_rate = 35
repeat_delay = 250

[input.touchpad]
tap = true
natural_scroll = true
dwt = true                              # Disable while typing

[input.mouse]
accel_profile = "flat"                  # Raw 1:1 desktop mouse response
accel_speed = 0.0
natural_scroll = false

# ─── Host 1: HP OmniBook X Flip 14 OLED (matched by EDID Make/Model or "eDP-1") ───
[output."Samsung Display Corp. 0x41AA"]
mode = "2880x1800@120.001"
scale = 1.75
transform = "normal"
vrr = "on"
hdr = true
# sdr_brightness_nits: not implemented in v1; SDR white is fixed at 203 cd/m2 (section 3.2.3)

# ─── Host 2: 34" Ultrawide Desktop Monitor (matched by connector or EDID Make/Model) ───
[output."DP-1"]
mode = "preferred"                      # e.g. "3440x1440@165.000" or "3440x1440@240.000"
scale = 1.0
transform = "normal"
vrr = "on-demand"                       # Fixed refresh on desktop; VRR engages for fullscreen games/video (compositor policy)
hdr = true
# sdr_brightness_nits: not implemented in v1; SDR white is fixed at 203 cd/m2 (section 3.2.3)
```

#### 2. `~/.config/kajo/binds.toml` (Human-Owned Key & Wheel Map)
```toml
# Keyboard, Mouse Wheel & Hardware Key Bindings
# Simple commands take a string; options (spawn, repeat, cooldown_ms, delta) take an inline table.

[binds]
# === Shell, Launcher, Lock & Built-In Screenshots ===
"Mod+Space"       = { spawn = ["kajo", "--toggle-launcher"] }
# Screenshots use the planned `kajo-capture` client (section 3.5); the binary and its flags do not exist yet.
"Mod+Shift+S"     = { spawn = ["kajo-capture", "--interactive"] }
"Print"           = { spawn = ["kajo-capture", "--interactive"] }
"Alt+Print"       = { spawn = ["kajo-capture", "--window"] }
"Ctrl+Print"      = { spawn = ["kajo-capture", "--screen"] }
"Mod+Ctrl+L"      = { spawn = ["kajo", "--lock"], allow_when_locked = true }
"Mod+Ctrl+S"      = { spawn = ["kajo", "--screensaver"] }
"Mod+T"           = { spawn = ["kitty"] }
# "Mod+Shift+Slash" = "show-hotkey-overlay"    # planned: needs text rendering inside kajo-comp

# === Audio, Brightness & Media (brightness keys act on the laptop backlight only in v1; no software control on external HDR monitors, section 3.2.3) ===
"XF86AudioRaiseVolume"  = { spawn = ["kajo", "--volume-up"], allow_when_locked = true }
"XF86AudioLowerVolume"  = { spawn = ["kajo", "--volume-down"], allow_when_locked = true }
"XF86AudioMute"         = { spawn = ["kajo", "--volume-mute"], allow_when_locked = true }
"XF86MonBrightnessUp"   = { spawn = ["kajo", "--brightness-up"], allow_when_locked = true }
"XF86MonBrightnessDown" = { spawn = ["kajo", "--brightness-down"], allow_when_locked = true }
"XF86AudioPlay"         = { spawn = ["playerctl", "play-pause"], allow_when_locked = true }
"XF86AudioNext"         = { spawn = ["playerctl", "next"], allow_when_locked = true }
"XF86AudioPrev"         = { spawn = ["playerctl", "previous"], allow_when_locked = true }

# === Overview & Window Switcher ===
# "Mod+D"         = { action = "toggle-overview", repeat = false }   # planned, not v1 (section 3.3.6)
# "Mod+Tab"       = { action = "toggle-overview", repeat = false }   # planned, not v1
"Alt+Tab"         = { action = "next-window", scope = "output" }
"Alt+Shift+Tab"   = { action = "previous-window", scope = "output" }
"Alt+grave"       = { action = "next-window", filter = "app-id" }
"Alt+Shift+grave" = { action = "previous-window", filter = "app-id" }

# === Window Lifecycle & State ===
"Mod+Q"           = { action = "close-window", repeat = false }
"Mod+F"           = "maximize-column"
"Mod+Shift+F"     = "fullscreen-window"
"Mod+Shift+T"     = "toggle-window-floating"
"Mod+Shift+V"     = "switch-focus-floating-tiling"
"Mod+W"           = "toggle-column-tabbed"

# === Directional Focus & Movement ===
"Mod+Left"        = "focus-column-left"
"Mod+Right"       = "focus-column-right"
"Mod+Down"        = "focus-window-down"
"Mod+Up"          = "focus-window-up"
"Mod+H"           = "focus-column-left"
"Mod+L"           = "focus-column-right"
"Mod+J"           = "focus-window-down"
"Mod+K"           = "focus-window-up"

"Mod+Shift+Left"  = "move-column-left"
"Mod+Shift+Right" = "move-column-right"
"Mod+Shift+Down"  = "move-window-down"
"Mod+Shift+Up"    = "move-window-up"
"Mod+Shift+H"     = "move-column-left"
"Mod+Shift+L"     = "move-column-right"
"Mod+Shift+J"     = "move-window-down"
"Mod+Shift+K"     = "move-window-up"

"Mod+Home"        = "focus-column-first"
"Mod+End"         = "focus-column-last"
"Mod+Ctrl+Home"   = "move-column-to-first"
"Mod+Ctrl+End"    = "move-column-to-last"

# === Vertical Workspaces ===
"Mod+Page_Down"       = "focus-workspace-down"
"Mod+Page_Up"         = "focus-workspace-up"
"Mod+U"               = "focus-workspace-down"
"Mod+I"               = "focus-workspace-up"
"Mod+Ctrl+Down"       = "move-column-to-workspace-down"
"Mod+Ctrl+Up"         = "move-column-to-workspace-up"
"Mod+Ctrl+U"          = "move-column-to-workspace-down"
"Mod+Ctrl+I"          = "move-column-to-workspace-up"
"Mod+Shift+Page_Down" = "move-workspace-down"
"Mod+Shift+Page_Up"   = "move-workspace-up"
"Mod+Shift+U"         = "move-workspace-down"
"Mod+Shift+I"         = "move-workspace-up"

# === Mouse Wheel Navigation ===
"Mod+WheelScrollDown"            = { action = "focus-workspace-down", cooldown_ms = 150 }
"Mod+WheelScrollUp"              = { action = "focus-workspace-up", cooldown_ms = 150 }
"Mod+Ctrl+WheelScrollDown"       = { action = "move-column-to-workspace-down", cooldown_ms = 150 }
"Mod+Ctrl+WheelScrollUp"         = { action = "move-column-to-workspace-up", cooldown_ms = 150 }
"Mod+WheelScrollRight"           = "focus-column-right"
"Mod+WheelScrollLeft"            = "focus-column-left"
"Mod+Ctrl+WheelScrollRight"      = "move-column-right"
"Mod+Ctrl+WheelScrollLeft"       = "move-column-left"
"Mod+Shift+WheelScrollDown"      = "focus-column-right"
"Mod+Shift+WheelScrollUp"        = "focus-column-left"
"Mod+Ctrl+Shift+WheelScrollDown" = "move-column-right"
"Mod+Ctrl+Shift+WheelScrollUp"   = "move-column-left"

# === Column & Window Sizing (16:10 & 21:9 Ultrawide) ===
"Mod+R"           = "switch-preset-column-width"
"Mod+Shift+R"     = "switch-preset-window-height"
"Mod+Ctrl+R"      = "reset-window-height"
# "Mod+Ctrl+F"    = "expand-column-to-available-width"   # planned, not v1 core yet
"Mod+C"           = "center-column"
# "Mod+Ctrl+C"    = "center-visible-columns"             # planned, not v1 core yet
"Mod+Minus"       = { action = "set-column-width", delta = "-10%" }
"Mod+Equal"       = { action = "set-column-width", delta = "+10%" }
"Mod+Shift+Minus" = { action = "set-window-height", delta = "-10%" }
"Mod+Shift+Equal" = { action = "set-window-height", delta = "+10%" }
```

#### 3. `~/.config/kajo/rules.toml` (Human-Owned Window & Layer Rules)
```toml
# Window & Layer Rules — single quotes '...' are literal strings (no backslash escaping needed)

[[rule]]
app_id = ['^gnome-control-center$', '^pavucontrol$', '^nm-connection-editor$']
width = 0.5
floating = false

[[rule]]
app_id = [
    '^org\.gnome\.Calculator$',
    '^gnome-calculator$',
    '^galculator$',
    '^blueman-manager$',
    '^xdg-desktop-portal$',
]
floating = true

[[rule]]
app_id = '^steam$'
title  = '^notificationtoasts_\d+_desktop$'
floating = true
anchor = { corner = "bottom-right", x = 10, y = 10 }
focus = false

[[rule]]
app_id = 'firefox$'
title  = '^Picture-in-Picture$'
floating = true

[[rule]]
app_id = '^zoom$'
floating = true

[[rule]]
title = ['Brave', 'WhatsApp', 'Signal', 'Thorium', 'Chrome', 'Chromium', 'Code']
scroll_factor = 0.15

# Fullscreen / VRR hint for media & games on 34" ultrawide
[[rule]]
app_id = ['^org\.kajo\.Player$', '^mpv$', '^steam_app_\d+$']
vrr = true

# Layer Rules (Panel, Popovers, Launcher, Toasts, OSK, OSD) - translucency only in v1, no blur
[[layer_rule]]
namespace = ['^kajo$', '^kajo-launcher$', '^kajo-notification$', '^kajo-osk$', '^kajo-osd$']
opacity = 0.85
popup_opacity = 0.85
# blur / xray / blur_popups (present in the live Niri config) are not rendered in v1: deferred, section 10.
```

#### 4. Domain App Files (`theme.toml`, `shell.toml`, `ide.toml`, `files.toml`, `mod.toml`): IMPLEMENTED

The shipped files `data/defaults/{theme,shell,ide,files,mod}.toml` are the **single source of truth**; a `test-config` case enforces parity between them and the compiled defaults. They are not copied here because copies drift. Facts read from them:

- `theme.toml`: `accent_color`, `ui_font_size_px`, `font_heading_size_px`, `font_subtitle_size_px`, `font_badge_size_px`, `css_path`, `settings_width`, `settings_height`.
- `shell.toml`: tables `[panel]`, `[autohide]`, `[notifications]`, `[touch]`, `[osk]`, `[lock]`, `[hardware]`, `[layout]` (widget arrays `left`/`center`/`right`; per-widget keys `id`, `mode`, `interval`, `exec`, `click_left`, `click_right`). There is no `[osd]`, `[launcher]`, `[idle]` or `[panel].widget_mode`.
- `ide.toml` defaults include `tab_width = 4`, `show_minimap = true`, `show_margin_guide = true`; `files.toml` default `show_inspector = true`.
- Drop-ins in `conf.d/*.toml` use a top-level domain table: `[shell.panel]`, `[theme]`, `[ide]`, `[files]`, `[mod]`.

---

## 5. Existing Kajo Codebase Touchpoints to Refactor

When implementation begins in `/home/mikolas/src/Kajo`, the blast radius is already well-contained:

| File / Module | Current Behavior | Target Change |
| :--- | :--- | :--- |
| `src/config.h`, `src/config/*`, `src/vendor/tomlc17/*` | **DONE in PR #31:** monolithic `config.c` (1,068 lines) replaced by a per-domain TOML store (`src/config/`, about 3.9k lines including schemas); `config.h` is now a thin include. | Remaining: the defects in §4.2.1, the parser decision in §4.1, and a `comp` domain / reader for `kajo-comp` (§4.2.2). |
| `src/compositor/compositor.h`, `src/compositor/niri.c` | `ShellCompositor` speaks Niri's socket protocol; exposes `niri_toggle_window_floating()`, `niri_reload_config()`, `niri_send_action()`. | Rename helpers to compositor-neutral `shell_compositor_toggle_floating()`, `shell_compositor_reload_config()`, `shell_compositor_send_action()`. Implement `src/compositor/kajo_comp_ipc.c` (auto-detecting `KAJO_COMP_SOCKET` first, falling back to `NIRI_SOCKET` during transition). |
| `src/settings/pages/page_displays.c` | Queries `niri msg -j outputs`, falls back to hardcoded `"eDP-1"` at lines 160/251/490, and lacks HDR controls. | Query `kajo-comp` IPC dynamically (working identically on `eDP-1` and `DP-1`), add an **HDR10 toggle card** (disabled with a reason when the probe in §3.2.1 fails) and a **VRR segmented selector (`Off | On | On-Demand`)**, writing `[output."..."]` to `comp.toml`. No SDR-brightness slider in v1 (§3.2.3). |
| `src/settings/pages/page_niri.c` | Generates `~/.config/niri/config.d/*.kdl` (1,129 lines) including the 15 Niri-API fragment shaders in `data/shaders/` (`open` 6, `close` 6, `resize` 3). | Replace with **Compositor & Tiling** page (`page_compositor.c`) editing `[layout]` & `[input]` in `comp.toml` (gaps, column width presets, `always_center_single_column`, `center_focused_column`, spring parameters, mouse/touchpad profiles). The shader presets are not portable and are dropped from the UI until §10 is implemented. |
| `src/posture/posture_manager.c` | Spawns `niri msg output eDP-1 transform ...` subprocess on tablet rotation (line 62). | Send IPC `set-output-transform` directly to `kajo-comp` (or handle posture/IIO directly inside `kajo-comp` when present per `ShellHwCaps`). |
| `src/hardware/touch_gestures.c`, `src/autohide.c` | `touch_gestures.c` opens `/dev/input/event*` directly and sends Niri JSON actions (including `ToggleOverview`) via `niri_send_action()`; the 2px/6px reveal strip is the autohide panel (`autohide.c`, `#panel-container.autohide-hidden` in `style.css`). | Move bezel gesture recognition into `kajo-comp`'s `libinput` touch handler; the strip goes away only if autohide reveal is also compositor-driven. Overview gestures stay inert until overview exists. |
| `src/greeter/main.c`, `src/greeter/greeter_users.c`, `data/greetd/*` | Greeter exits its compositor with `niri msg action quit` (blocking `g_spawn_sync`, `main.c:294`); fallback session `niri-session` in `main.c:392` and `install-greeter.sh`; hard-coded Niri fallback entry in `greeter_users.c`; `kajo-greeter-session` hosts the greeter on `niri` or `cage` and corrupts `$USER`. | Implement §6.5: `kajo-comp --greeter-mode -- kajo-greeter`, child-exit handoff, `comp-greeter.toml`, fallback chain `kajo-comp → niri → cage`, `kajo-session` default; remove the Niri-specific code in the greeter. |

---

## 6. Session Launcher (`kajo-session`), Systemd Lifecycle & Multi-Session Testing

To behave like a first-class Linux desktop environment—and let you switch effortlessly between **Kajo (`kajo-comp`)**, **Kajo (Debug / Validation)**, **Niri**, **COSMIC**, or **nested test instances** without environment pollution—Kajo ships a dedicated session orchestrator (`kajo-session`), `wayland-sessions` desktop entries, and `systemd --user` units.

### 6.1 Installed Session Files & Layout
```text
/usr/bin/kajo-session                               # Login-shell & multi-compositor session launcher
/usr/share/wayland-sessions/kajo.desktop            # Production Kajo Desktop (kajo-comp)
/usr/share/wayland-sessions/kajo-debug.desktop      # Kajo Desktop w/ Vulkan Validation + verbose logs
/usr/share/wayland-sessions/kajo-niri.desktop       # Side-by-side Kajo on Niri session (zero-risk fallback)
/usr/share/xdg-desktop-portal/kajo-portals.conf     # Routes ScreenCast/Screenshot -> wlr, default -> gtk
/usr/lib/systemd/user/kajo-comp.service             # Type=notify compositor unit bound to graphical-session.target
/usr/lib/systemd/user/kajo.service                  # Kajo panel/shell unit ordered After=kajo-comp.service
/usr/lib/systemd/user/kajo-shutdown.target          # Clean session teardown target
```

Greeter-related files (see §6.5; the first two exist today, the rest are **(planned)**):
```text
/usr/local/bin/kajo-greeter                          # existing, installed by data/greetd/install-greeter.sh
/usr/local/bin/kajo-greeter-session                  # existing wrapper; gains --compositor kajo-comp and a fallback chain
/etc/greetd/niri-greeter.kdl                         # existing Niri greeter config; kept until kajo-comp greeter mode has soaked (§8)
/etc/greetd/comp-greeter.toml                        # (planned) system-wide output/input policy for kajo-comp --greeter-mode
/usr/local/share/kajo/greetd/comp-greeter.toml       # (planned) copy used by the lookup order in §6.5.5
/var/cache/kajo-greeter/state.json                   # existing: last user + session, written by the greeter
```

### 6.2 `kajo-greeter` Wayland Session Entries (`/usr/share/wayland-sessions/`)
`kajo-greeter` (`src/greeter/greeter_users.c`) scans `/usr/local/share/wayland-sessions`, `/usr/share/wayland-sessions`, `/usr/local/share/xsessions` and `/usr/share/xsessions` for `*.desktop` files and populates its session selector, persisting the last-used session per user in `/var/cache/kajo-greeter/state.json` (falling back to `~/.cache/kajo/greeter-state.json`). Entries from `xsessions` are listed as type `x11`; this spec does not claim they can be started. If no session is found the greeter adds a hard-coded `niri-session` entry, which must become `kajo-session` (§6.5.6):

```ini
# /usr/share/wayland-sessions/kajo.desktop
[Desktop Entry]
Name=Kajo
Comment=Kajo Aero + Metro Desktop (kajo-comp Wayland Compositor)
Exec=kajo-session
Type=Application
DesktopNames=kajo
```

```ini
# /usr/share/wayland-sessions/kajo-debug.desktop
[Desktop Entry]
Name=Kajo (Debug & Validation)
Comment=Kajo Compositor with Vulkan Validation Layers & Session Trace Logging
Exec=kajo-session --profile debug
Type=Application
DesktopNames=kajo
```

```ini
# /usr/share/wayland-sessions/kajo-niri.desktop
[Desktop Entry]
Name=Kajo (Niri Backend)
Comment=Kajo Desktop Suite running on Niri Compositor
Exec=kajo-session --compositor niri
Type=Application
DesktopNames=niri
```

### 6.3 `kajo-session` CLI & Multi-Environment Testing Modes

`kajo-session` supports four distinct execution modes from a single script/binary:

```text
Usage: kajo-session [OPTIONS]

Options:
  --compositor <kajo-comp|niri>               Select compositor backend (default: kajo-comp; others are out of scope)
  --profile <release|debug|asan>              Enable validation layers / sanitizer options / trace logs
  --config-dir <PATH>                         Sets KAJO_CONFIG_DIR (already honoured by KajoConfigStore) for isolated config sandboxing
  --nested                                    Launch isolated nested window inside current live desktop
  --resolution <WxH>                          Nested window geometry (default: 1920x1080)
```

1. **Standard Login Mode (`greetd` / `kajo-greeter` / bare TTY):**
   - Re-execs through the user's login shell (`$SHELL -l`) if not already a login shell so `~/.profile`, `PATH`, and `SSH_AUTH_SOCK` are loaded identically across TTY and greeter logins.
   - Exports session identity (`XDG_SESSION_TYPE=wayland`, `XDG_CURRENT_DESKTOP=kajo`, `XDG_SESSION_DESKTOP=kajo`, `ELECTRON_OZONE_PLATFORM_HINT=auto`, `QT_WAYLAND_RECONNECT=1`).
   - Runs `systemctl --user reset-failed`, imports login environment (`systemctl --user import-environment` + `dbus-update-activation-environment --systemd --all`), and blocks on `systemctl --user --wait start kajo-comp.service`.
   - **Critical Teardown Scrub on Logout:** Starts `kajo-shutdown.target` and runs:
     ```bash
     systemctl --user unset-environment WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP \
         XDG_SESSION_DESKTOP KAJO_COMP_SOCKET NIRI_SOCKET SWAYSOCK
     ```
     This guarantees switching between `Kajo`, `Kajo (Niri)`, `COSMIC`, and `GNOME` in `kajo-greeter` **never leaks stale socket or `XDG_CURRENT_DESKTOP` portal variables** into the next login.
2. **Debug / Validation Profile (`--profile debug`):**
   - Enables `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, `WLR_DEBUG=1`, and tees timestamped stdout/stderr into `~/.local/state/kajo/logs/session-%Y%m%d-%H%M%S.log` (keeping the last 10 session logs automatically).
3. **Isolated Nested Testing Mode (`kajo-session --nested [--config-dir /tmp/test-cfg]`):**
   - Launches `kajo-comp` with `WLR_BACKENDS=wayland` in a window inside your live desktop using private `WAYLAND_DISPLAY` and `KAJO_COMP_SOCKET` names—**explicitly skipping** `systemctl --user import-environment` and `dbus-update-activation-environment` so the nested test instance never hijacks your host desktop's portals, notifications, or D-Bus environment.
   - Spawns a nested `kajo` bar + test terminal directly against the private socket so full session changes can be verified safely before ever logging out.

### 6.4 `systemd --user` Unit Coordination (`Type=notify` — Zero Startup Races)

```ini
# /usr/lib/systemd/user/kajo-comp.service
[Unit]
Description=Kajo Wayland Compositor
BindsTo=graphical-session.target
Before=graphical-session.target xdg-desktop-autostart.target
Wants=graphical-session-pre.target xdg-desktop-autostart.target
After=graphical-session-pre.target

[Service]
Slice=session.slice
Type=notify
ExecStart=/usr/bin/kajo-comp --session
TimeoutStopSec=5
```

- Once `kajo-comp --session` binds `WAYLAND_DISPLAY`, `DISPLAY` (`wlr_xwayland`), and `KAJO_COMP_SOCKET`, it pushes those three variables to `systemd --user` / D-Bus activation environment and calls `sd_notify(0, "READY=1")`.
- Only *after* `READY=1` does `systemd` advance `graphical-session.target`, starting `kajo.service` (panel, notifications, OSK, posture), `xdg-desktop-portal.service`, `polkit-gnome`, and `gnome-keyring` with zero socket-not-found races.

### 6.5 Greeter Host Mode (`kajo-comp --greeter-mode`)

#### 6.5.1 Current behaviour (read from the repo, not run)
```text
greetd → kajo-greeter-session [--compositor niri|cage] [--cmd CMD] [--user U]
           ├─ cage requested, or niri missing:  exec cage -s -- kajo-greeter
           └─ else: exec niri -c <first existing of
                /etc/greetd/niri-greeter.kdl,
                /usr/local/share/kajo/greetd/niri-greeter.kdl,
                /usr/share/kajo/greetd/niri-greeter.kdl,
                $HOME/.local/share/kajo/greetd/niri-greeter.kdl>   (else: plain `exec niri`)
                  └─ spawn-at-startup: sh -c "kajo-greeter; niri msg action quit --skip-confirmation"
```
- `niri-greeter.kdl`: no binds, animations off, no focus ring or border, gaps 0, hotkey overlay skipped, touchpad tap + natural scroll, **no `output` section** (so no explicit scale, mode, or transform).
- `kajo-greeter` draws one `GTK_LAYER_SHELL_LAYER_OVERLAY` window per monitor (`main.c:595`), anchored on all four edges, exclusive zone −1; the primary monitor holds the login card and exclusive keyboard focus. It rebuilds its surfaces when monitors change (`main.c:827–910`).
- Login: PAM and session start go through `greetd_authenticate_and_start_async()` using `GREETD_SOCK`; the command and environment come from the selected `.desktop` entry (`Exec`, `DesktopNames`).
- Handoff: after greetd accepts the session the greeter saves user and session to its state file and calls `quit_greeter_compositor_and_app()`, which runs `niri msg action quit --skip-confirmation` when `NIRI_SOCKET` is set (`main.c:289–300`).
- `install-greeter.sh` installs the binaries to `/usr/local/bin`, the Niri config to `/etc/greetd/` and `/usr/local/share/kajo/greetd/`, the TOML defaults, CSS and icons to `/usr/local/share/kajo/`, and creates `/var/cache/kajo-greeter` owned by `greeter`. `--enable` edits `/etc/greetd/config.toml` (backs it up first) so `command = "kajo-greeter-session --cmd <DEFAULT_CMD>"`; the default command is `niri-session`.

#### 6.5.2 Target process model (cage-style: the compositor owns the client's lifetime)
```text
greetd → kajo-greeter-session --compositor kajo-comp --cmd kajo-session
           └─ kajo-comp --greeter-mode [--config PATH] -- kajo-greeter
                  └─ forks kajo-greeter; leaves its event loop when the child exits
```
1. `kajo-comp` starts the client with `fork`/`exec` (not a shell), passes through `GREETD_SOCK`, `KAJO_GREETER_CMD`, `KAJO_GREETER_USER` and the Wayland variables it sets, and watches the child with a `SIGCHLD` source on its `wl_event_loop` (no `waitpid` blocking, §7.2.3).
2. When the child exits, the compositor stops the loop, tears down outputs and **fully releases the DRM master and the seat**, then exits with the **child's exit status**. **(greetd, unverified here):** greetd launches the user session only after the greeter process (here the compositor) has exited, so this exit must be prompt and complete. Measure the time from `start_session` to compositor exit on bare metal.
3. If the child cannot be started, or the compositor itself fails before the client is running, it exits with a dedicated status (proposed: **70**) so the wrapper can tell "compositor failed" from "greeter finished or failed" (§6.5.7).
4. `kajo-greeter` does nothing compositor-specific on exit: it simply exits after a successful `start_session`. This replaces `niri msg action quit` and the blocking `g_spawn_sync`.
5. Crash handling: if the greeter crashes, the compositor exits non-zero and greetd restarts the greeter command. **(greetd, unverified here):** greetd's restart policy is not documented in this spec; the wrapper must therefore avoid a tight crash loop of a broken compositor (§6.5.7).

#### 6.5.3 What greeter mode turns off, keeps, and restricts

| Area | In greeter mode | Reason / evidence |
| :--- | :--- | :--- |
| Keybindings (`binds.toml`), `spawn` actions | **Off.** No bind is loaded; there is no way to start a program from the keyboard. | Equivalent of the empty `binds {}` in `niri-greeter.kdl`. |
| `$XDG_RUNTIME_DIR/kajo-comp.sock` IPC | **Not created.** | The greeter does not need it; it removes a control surface on a pre-login screen. |
| `rules.toml`, `[autostart]`, `[env]` | **Not read.** | They belong to the user session. |
| XWayland | **Off.** | No X11 client is started. |
| `systemd` handoff (§6.4), `sd_notify`, D-Bus environment import | **Skipped.** | There is no user session or unit; `kajo-comp.service` is not used. |
| Screenshots, portals, `wlr_screencopy`, `ext-image-copy-capture`, virtual keyboard, tablet/other privileged globals | **Not advertised**, except what the greeter needs (below). | Reduces what a single client can do to the seat. Sway 1.12 already restricts privileged globals per client (`sway/server.c:118–119` compares globals in its filter), so the mechanism exists; the allow-list for `kajo-comp` is **(planned)**. |
| Always present | `xdg_shell`, `wlr_layer_shell_v1`, `xdg_output`/`wlr_output_management` as needed, `wlr_fractional_scale_v1`, `wlr_viewporter`, `linux-dmabuf`, `wl_seat` (keyboard, pointer, touch), cursor shape. | `kajo-greeter` uses `gtk4-layer-shell` and GTK4. |
| **VT switching** | **Stays enabled.** | Corrects the earlier text. §7.5 requires `Ctrl+Alt+F2` to work, and `cage -s` (today's fallback) keeps it. |
| Stall watchdog (§7.5.3) | **Open.** There is no systemd user unit, so `WatchdogSec=` is unavailable; decide whether a supervisor process (`kajo-greeter-session` stays resident) or nothing covers the greeter. | Not designed. |
| Idle / DPMS blanking | **Open.** The Niri greeter config sets none; `kajo-comp` greeter mode should do the same unless the owner decides otherwise. | Not specified in the repo. |
| Debug logging | `--profile debug` and `WLR_DEBUG` must not be selectable here, and no key or text event may be logged. | The compositor sees every password keystroke before the greeter does. Verify wlroots and our code log none. |
| Core dumps | Set `RLIMIT_CORE = 0` and `PR_SET_DUMPABLE = 0` (planned). | Same reason: process memory may contain keystrokes. Not implemented. |

#### 6.5.4 Outputs, scale and renderer in greeter mode
- **All outputs enabled**, preferred mode, placed left to right in connector order, so each `kajo-greeter` monitor window has an output. No layout engine, workspaces, or columns. Hotplug re-runs the same policy.
- **Scale:** `wlroots` supplies no default scale. The Niri greeter relies on Niri's own default (which the repo does not document). `kajo-comp` needs its own rule. Sway 1.12 uses a DPI and height threshold (constants `HIDPI_DPI_LIMIT = 2 * 96` and `HIDPI_MIN_HEIGHT = 1200` in `sway/config/output.c`; I did not read the rest of that function), which yields whole-number scales and so cannot produce the laptop's configured `1.75`. Decision needed: accept that the greeter's scale may differ from the session, or let `comp-greeter.toml` carry explicit per-output scales (§6.5.5).
- **SDR only:** `hdr = false`, `vrr = "off"`, 8-bit. The greeter is plain GTK, and avoiding an HDR mode set also avoids a visible mode change between greeter and session.
- **Renderer:** automatic selection (`wlr_renderer_autocreate()`: GLES2, then Vulkan, then pixman), not forced Vulkan. A broken Vulkan stack must not remove the login screen. This is the only place §3.1's "force Vulkan" does not apply.
- **Touch and rotation:** transform follows `comp-greeter.toml`; the IIO posture daemon runs inside the user session and is not available here. A convertible left in tablet mode at the login screen is therefore not auto-rotated unless the compositor handles posture itself (§3.3.7, **planned**).

#### 6.5.5 Configuration in greeter mode (who reads what)
- `kajo-comp --greeter-mode` runs as the `greeter` user. It cannot and must not read the user's `~/.config/kajo/comp.toml`.
- **Proposed lookup order for `comp-greeter.toml`** (same directories the wrapper already uses for the Niri file): `--config PATH`, `/etc/greetd/comp-greeter.toml`, `/usr/local/share/kajo/greetd/comp-greeter.toml`, `/usr/share/kajo/greetd/comp-greeter.toml`, `$HOME/.local/share/kajo/greetd/comp-greeter.toml`; if none exists, built-in defaults (all outputs on, SDR, no binds).
- Allowed content: `[output."<connector or EDID>"]` (mode, scale, transform), `[input.*]` (xkb layout, touchpad, mouse), nothing else. Unknown keys log a warning.
- The greeter application itself reads `KajoThemeCfg` through `KajoConfigStore` with no argument (`main.c:888`), i.e. the `greeter` user's config directory, then shipped defaults. The user's accent colour therefore does **not** appear on the login screen; this is the current behaviour and is not changed here. `install-greeter.sh` installs the defaults to `/usr/local/share/kajo/defaults/`. The current-directory lookup that used to affect this was removed (Revision 6), so the greeter's working directory no longer matters for defaults.
- `XDG_RUNTIME_DIR` must exist for the `greeter` user. If it does not, `wlroots` cannot create its socket and `kajo-comp` must exit with the "compositor failed" status (70), not hang. Whether greetd's PAM stack provides it on this system was **not verified**.

#### 6.5.6 Changes needed in existing code and scripts
| File | Change |
| :--- | :--- |
| `src/greeter/main.c:289–300` | Delete the `NIRI_SOCKET` / `KAJO_GREETER_QUIT_NIRI` branch and its blocking `g_spawn_sync`; exit the application after a successful `start_session`. |
| `src/greeter/main.c:392`, `data/greetd/install-greeter.sh` (`DEFAULT_CMD`, `--cmd`, help text) | `niri-session` → `kajo-session` once `kajo.desktop` and `kajo-session` ship; keep `niri-session` only as an explicit option. |
| `src/greeter/greeter_users.c` (empty-list fallback) | Hard-coded `niri` entry → `kajo-session`. |
| `data/greetd/kajo-greeter-session` | Add `kajo-comp` to `--compositor`; implement the fallback chain (§6.5.7); `USER=""` clobbering, silent unknown arguments and the missing-`cage` case are **already fixed** (Revision 6). |
| `data/greetd/install-greeter.sh` | Install `comp-greeter.toml` to `/etc/greetd/` and `/usr/local/share/kajo/greetd/`; keep installing the Niri config until §8 Phase 3 retires it. |
| `tests` | Replace the `KAJO_GREETER_QUIT_NIRI` test hook with a test of the exit path that no longer needs a compositor. |

#### 6.5.7 Fallback chain and failure policy
- Today the machine always has a login screen if `niri` or `cage` works. A new compositor must not remove that safety net.
- `kajo-greeter-session` (not `exec`-ing the first choice) runs, in order: `kajo-comp --greeter-mode`, then `niri -c …`, then `cage -s`. It falls through **only** when the previous step exits with the "compositor failed" status (70) or with a signal before the client was running; a normal greeter exit (success or failure) ends the wrapper with that status.
- To avoid a tight loop of a failing compositor, the wrapper records the failing step in `/var/cache/kajo-greeter/` and skips it on the next start until the wrapper's own version changes or the file is removed. **(Design proposal; greetd's restart behaviour is unverified here.)**
- The Niri greeter files stay installed until `kajo-comp --greeter-mode` has passed the checks in §6.5.9 on **both** hosts.

#### 6.5.8 Known gap: tablet / touch login
`kajo-greeter` has no on-screen keyboard (no OSK code in `src/greeter/`), so a convertible in tablet mode cannot enter a password at the login screen today, with Niri either. Greeter mode could host `ShellOSK` over `wlr_input_method_v2` / `wlr_virtual_keyboard_v1`, but that is **(planned, not specified)** and would require allowing those two globals for the greeter client only. Until then the documented requirement is a physical keyboard.

#### 6.5.9 Verification plan (nothing below has been run)
1. **Nested:** run `WLR_BACKENDS=wayland kajo-comp --greeter-mode -- kajo-greeter` inside a live session, in a disposable terminal. Without `--test` the greeter takes its real layer-shell path (`use_layer_shell` is true when layer-shell is supported, `main.c:895`) and without `GREETD_SOCK` it cannot complete a login; what it does on submit in that case was **not checked**. `--test` is a different mode (a preview window, PAM against the real user, no layer-shell); it is switched off whenever `GREETD_SOCK` is set (`main.c:950–954`). Use both: `--test` for the PAM/UI flow, no flag for the layer-shell/compositor flow.
2. **Exit contract:** success path exits 0; kill the greeter mid-authentication; kill the compositor; start with an unusable renderer; start with `XDG_RUNTIME_DIR` unset. Each must produce the documented status and no hang.
3. **Bare metal, with a rescue path ready:** a second TTY or SSH session is logged in before testing. Check that VT switching works from the greeter, that unplugging and replugging a monitor rebuilds surfaces, and the time from `start_session` to compositor exit.
4. **Handoff:** the user session starts with a clean environment (§6.3 teardown scrub), a working `WAYLAND_DISPLAY`, and no stale greeter variables.
5. **Fallback:** break `kajo-comp` deliberately (for example rename the binary) and confirm the Niri greeter appears; then remove Niri as well and confirm `cage` appears.
6. **Both hosts:** repeat on the 14" laptop and the 34" desktop. Record the scale and mode each shows.
7. **Security checks:** no key or text events in any log at default and debug settings; core dumps disabled; the compositor advertises no screencopy, capture, or IPC socket in greeter mode (`wayland-info` / socket listing).

---

## 7. Hardened Development, Verification & Crash-Survival Protocol

Because a compositor crash kills every connected Wayland client (`EPIPE` on `WAYLAND_DISPLAY`) and a compositor deadlock can trap the physical seat/keyboard, `kajo-comp` is engineered under mission-critical reliability rules from the very first commit.

### 7.1 Required Hardening Toolchain
The complete, verified list of packages (installed vs. to install, with versions) is in **§9**. Do not duplicate it here.

### 7.2 Defensive C11 Coding Invariants (Enforced by `scripts/audit.sh`)
1. **Zero `assert()` / `abort()` / `g_error()` in `src/comp/`:**
   - All runtime invariants use `COMP_WARN_RETURN(cond, fallback)` (logs a rate-limited backtrace once and recovers cleanly; never calls `abort()`).
   - Wrap `wlroots` code paths known to `abort()`/`assert()`: `convert_primaries_to_colorspace()` in the DRM backend aborts for any primaries other than none/BT2020; `wlr_scene_set_color_manager_v1()` asserts if called twice; `wlr_scene_output` asserts `supplied == NULL` when an image description is set. Validate inputs before calling and call each once. This invariant covers **our** code only; `wlroots` and Mesa internals can still abort.
   - Ignore `SIGPIPE` (`signal(SIGPIPE, SIG_IGN)`) on startup so writing to a disconnected Wayland or IPC socket never terminates the compositor.
   - Clamp all client geometry/surface dimensions (`1..32767`), guard `width * stride` against integer overflow, and cap incoming IPC frames (`<= 64 KB`).
2. **Compiler Memory Hardening Flags (`src/comp/meson.build`):**
   - `-Wall -Wextra -Werror -Wshadow -Wnull-dereference -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wsign-conversion -Wvla -fno-common`
   - `-ftrivial-auto-var-init=zero` (eliminates uninitialized stack variable/pointer bugs across all code paths at `< 0.5%` CPU cost).
   - `-fstack-protector-strong -D_FORTIFY_SOURCE=3` (with VLAs banned via `-Wvla`).
3. **Zero Blocking Calls on the Main Event Loop:**
   - All sockets run `O_NONBLOCK`; screenshot PNG compression/disk writes and external spawns run asynchronously off the `wl_event_loop` thread.
4. **Pure-C Core Decoupling (`comp_layout.c`, `comp_spring.c`, `comp_rules.c`):**
   - Layout, workspace/column state machines, centering math, and spring physics have **zero** `wlroots` or Vulkan headers, enabling deterministic microsecond unit and fuzz testing.

### 7.3 Static Analysis & Dynamic Sanitizer Gates (Mandatory Per-Commit Gate)
1. **Triple Static Analysis (`scripts/audit.sh`):**
   - `clang-tidy` (`clang-analyzer-*`, `bugprone-*`), GCC `-fanalyzer` (interprocedural double-free, NULL-deref, and fd-leak analysis), and `cppcheck`, plus an automated `audit.sh` check rejecting `assert(`, `abort(`, `g_error(`, `g_spawn_sync`, or blocking `waitpid(` in `src/comp/`.
2. **Strict ASan + UBSan + LSan (`scripts/asan-test.sh`):**
   - All compositor tests execute under `ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1` and `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. Start with **zero suppressions** for our code, but expect to need a documented suppression file for leaks inside Mesa/Vulkan drivers when running on a real GPU; any suppression must name the library and be reviewed.
3. **Khronos Vulkan Validation Layers (`VK_LAYER_KHRONOS_validation`):**
   - Enabled in local debug runs (no CI is used) to catch Vulkan misuse inside the HDR output path and any custom Vulkan code (none in v1; the stock renderer's own errors will also surface). Requires `vulkan-validation-layers` (§9).
4. **File-Descriptor Leak Audits (`valgrind --track-fds=yes`):**
   - Verifies zero leaked `dmabuf`, `drm_syncobj`, `inotify`, or client socket fds across 1,000 window open/close and config hot-reload cycles.

### 7.4 Fuzzing (`libFuzzer`) & Headless Wayland Chaos Suite
1. **Three `clang -fsanitize=fuzzer,address,undefined` Targets (`tests/fuzz/`):**
   - `fuzz_toml_config`: Mutated/truncated/hostile `.toml` files + regex rules $\rightarrow$ guarantees a typo during live config hacking never crashes or leaks.
   - `fuzz_ipc_socket`: Malformed/partial/oversized JSON frames against `kajo-comp.sock`.
   - `fuzz_layout_monkey`: Stateful property fuzzer executing millions of random sequences (`open`, `close_mid_anim`, `float`, `fullscreen`, `tabbed`, `move_col`, `switch_ws`, `lock`, `step_springs`; `overview` once implemented) and asserting structural invariants + baseline `talloc` block count after every cycle.
2. **Headless GPU Chaos Integration Test (`WLR_BACKENDS=headless` + `WLR_RENDERER=vulkan`; whether the headless backend can create a Vulkan renderer on this machine is **unverified**):**
   - Automated `meson test` harness launching `kajo-comp` on the `wlroots` headless backend + Vulkan validation layers while 20 stress-test Wayland/XWayland clients rapidly map popups, lock pointers, trigger screenshots, resize `1×1` $\leftrightarrow$ `8192×8192` buffers, and `kill -9` themselves mid-frame.

### 7.5 Runtime Hardware Fault Recovery & Blast-Radius Isolation
1. **Atomic KMS Test-Before-Commit Fallback Cascade (`wlr_output_test_state`):**
   - Test every display state (`HDR10 + 10-bit + VRR + high refresh`) via `wlr_output_test_state()` before committing. On failure (e.g., DP link bandwidth limit), step down (`HDR10 10-bit` $\rightarrow$ SDR `8-bit`, VRR off, lower refresh) and emit a user notification rather than leaving a black screen. FP16 is not in the v1 cascade (§3.2.2).
2. **Vulkan `VK_ERROR_DEVICE_LOST` Reset Recovery:**
   - Recreate `wlr_vk_renderer`, allocator, and output swapchains cleanly on GPU driver reset instead of exiting the session.
3. **Stall Watchdog (Never Lock Out the Rescue TTY) — design open:**
   - Goal: if the main thread stalls for >10s, the user must still be able to switch to a text VT. A heartbeat updated from `wl_event_loop` and checked by a separate thread or process is the detection mechanism.
   - **The recovery action is not settled.** Rev. 1 had the watchdog thread call `libseat_disable_seat()`; whether libseat's API may be called from a non-main thread while the main thread is blocked was **not verified**, and doing so could race the main loop. Candidate designs, to be evaluated in a spike: (a) a tiny supervisor **process** that, on missed heartbeats, `SIGKILL`s `kajo-comp` so the kernel releases DRM master (and `logind`/`seatd` reclaim the seat), (b) `systemd` `WatchdogSec=` on `kajo-comp.service` (`Type=notify` already planned) using `sd_notify("WATCHDOG=1")` from the loop. Option (b) needs no custom code and is the first thing to try; (a) is the fallback.
   - Either way the watchdog is **not** an in-process thread that touches seat state until that has been shown safe.
   - Greeter mode has no systemd user unit (§6.5.3), so option (b) does not apply there; coverage for the greeter is an open decision.
4. **Zero-Work-Loss Session Isolation:**
   - **Emergency Buffer Dump in `kajo-ide`:** Hook `GdkDisplay::closed` / `SIGHUP` / `SIGTERM` in `kajo-ide` to atomically flush any unsaved editor buffers to `~/.local/state/kajo/ide-recovery/` before process exit.
   - **Detached App Scopes & Reconnect:** Launch user apps in `systemd --user` transient scopes (`app-*.scope`) so CLI/background/`tmux` workloads survive compositor termination; export `QT_WAYLAND_RECONNECT=1`; build with `-g` (`debugoptimized`) for `coredumpctl` post-mortem inspection.

---

## 8. Implementation Plan

This section replaces the earlier "phased rollout". It is the working plan for branch `feat/kajo-comp`. Each phase ends with a **gate**; if a gate fails we stop and re-plan instead of continuing.

### 8.0 Decisions adopted (defaults; change here if the owner overrides them)

| # | Decision | Adopted default |
| :-: | :--- | :--- |
| 1 | GUI-written output settings | `comp.toml`, `binds.toml`, `rules.toml` stay **human-owned and never GUI-written**. `kajo-settings` writes only a separate GUI-owned file (`outputs.toml`) that `comp.toml` may override (§4.2.1). |
| 2 | Allocator | `talloc` (LGPL-3.0+, dynamic; acceptable under the source-only policy, §9.8). |
| 3 | Screenshot HUD | Separate `kajo-capture` client, not drawn inside the compositor (§3.5). |
| 4 | Source layout | `src/comp/`, built as its own meson target `kajo-comp`, **outside** `kajo_core`. Links `wlroots-0.20` (`-DWLR_USE_UNSTABLE`), `wayland-server`, `xkbcommon`, `talloc`, `libsystemd` (`sd_notify`), and the GLib-only config schema engine (not `KajoConfigStore`, §4.2.2). Protocol headers not shipped by wlroots (for example `xdg-shell-protocol.h`) are generated with `wayland-scanner`. |
| 5 | Decision gate | **Closed (Revision 7): HDR10 is v2; v1 is SDR.** |

### 8.1 Rules that apply to every phase

- `meson compile -C builddir && meson test -C builddir` passes with 0 warnings; anything touching lifetimes, async callbacks, threads or sockets also runs `scripts/asan-test.sh`.
- No command acts on the owner's live session (AGENTS.md rule 6). Compositor runs are nested (`WLR_BACKENDS=wayland` or `headless`) with private `WAYLAND_DISPLAY`/socket names; bare-metal runs are done by the owner from a TTY with a rescue path (SSH or a second TTY).
- The spec and user docs change in the same change as the code. Counts quoted in docs are verified by command.
- Throwaway spikes live in `scratch/` (git-ignored) and are never committed.

### 8.2 Phases

**Phase 0 — HDR10 feasibility spike (throwaway, `scratch/hdr-spike/`)**
- A tinywl-sized compositor on the stock `wlroots 0.20.2` stack, Vulkan renderer forced, with: capability probe and logging, HDR10 enable (10-bit render format + BT.2020/PQ image description) with `wlr_output_test_state()` before commit and SDR fallback, optional FP16 and mastering-metadata experiments, color-management/dmabuf/syncobj globals as Sway sets them up, VT switching, a hard auto-exit timer, and an HDR on/off hot key for A/B comparison.
- Questions to answer, each with an observed result written down:
  1. Does the output report `BT2020` + `ST2084_PQ`, and does enabling `XRGB2101010` + image description commit?
  2. Do GTK4 windows (`kajo-settings`) keep exact colours (`#111111`, `#0078d4`) at 203 cd/m² SDR white (measured, not judged by eye)?
  3. Does an HDR10 client (`mpv --vo=gpu-next --gpu-context=waylandvk` with an HDR10 clip, or a Vulkan HDR test app) exceed SDR white without clipping?
  4. What does `ext-image-copy-capture` return on an HDR output (format, transfer function)? (§3.5.4)
  5. Do `mastering_luminance` / `max_cll` / `max_fall` change the picture? (§3.2.2)
  6. Is FP16 (`ABGR16161616F`) accepted by KMS on this GPU? (§3.2.2)
  7. How does integer-position spring motion look at scale `1.75`? (§3.3.5)
  8. (added) Can the `headless` backend create a Vulkan renderer here? (§7.4)
- Nested and headless runs are done by the assistant; questions 1–3 and 5–7 need the owner on a TTY. Repeat 1–3 on Host B when available.
- **Gate result (Revision 7):** HDR mode switching works with `--force-hdr-caps`, colour mapping was wrong because of the spike bug described in Revision 7; owner decided to move HDR10 to v2. **Phase 0 is closed.** Kept for v2: questions 2–7 and the fixes needed (§8.4).

**Phase 1 — Pure core (no wlroots or Vulkan headers)**
- `comp_spring.c`: analytical 2nd-order spring that preserves velocity when retargeted.
- `comp_layout.c`: output → workspace → column → window model, centering rules, width presets, floating, tabbed columns, and the operations behind the binds.
- `comp_rules.c`, `comp_binds.c`: parse `rules.toml` / `binds.toml` with the vendored parser (single-quoted regexes, inline-table options).
- Tests for spring continuity and layout invariants; `fuzz_layout_monkey` (random operation sequences with invariant checks) and a fuzz target for the rules/binds parsers.
- **Gate:** all tests and fuzz smoke runs pass under ASan/UBSan with zero leaks.

**Phase 2 — Nested compositor bring-up (`WLR_BACKENDS=wayland`)**
- 2a. Server skeleton: display, backend, renderer, allocator, scene, outputs, seat/keyboard/pointer, cursor, `comp_listen()` auto-unlinking listeners over `talloc`, rate-limited `COMP_WARN_RETURN`, `SIGPIPE` ignored, compiler flags from §7.2.
- 2b. Windows: xdg-shell and popups placed by the Phase 1 layout, focus, 1px borders, springs driving node positions, server-side decoration negotiation, fractional scale.
- 2c. Shell integration: layer-shell for `kajo`, session-lock, idle-notify/inhibit, input-method/virtual-keyboard for the OSK, pointer-constraints, xdg-foreign, the other §3.7 globals, XWayland.
- 2d. Config and IPC: the `comp` config domain via the schema engine and inotify on the `wl_event_loop`; `$XDG_RUNTIME_DIR/kajo-comp.sock` with a **written message schema** and a mock-compositor test in the style of `tests/test_niri.c`; `kajo/src/compositor/kajo_comp_ipc.c` detecting `KAJO_COMP_SOCKET`.
- Headless chaos test: clients that map popups, lock pointers, resize and `kill -9` themselves.
- **Gate:** `kajo` panel, `kajo-settings`, `kajo-files` and `kitty` usable nested; chaos test passes; no `assert`/`abort`/blocking call in `src/comp/` (checked by `scripts/audit.sh`).

**Phase 3 — Screenshots**
- Capture globals, window capture through a scene-node source, and the `kajo-capture` client (area, window, screen; clipboard; PNG on a worker thread).
- **Gate:** SDR correctness verified; HDR behaviour recorded from Phase 0 question 4.

**Phase 4 — Bare metal (SDR)**
- DRM backend, `wlr_output_test_state()` fallback cascade (mode, refresh, VRR off), `vrr = "on-demand"`, hotplug, renderer/GPU-reset recovery, session lock, stall handling via `sd_notify("WATCHDOG=1")` / `WatchdogSec=` (§7.5.3), in-compositor touch gestures and posture rotation. No HDR (v2).
- **Gate:** owner-verified on the laptop, then on the desktop, including VT switching and unplug/replug.

**Phase 5 — Session and greeter**
- `kajo-session`, `wayland-sessions` entries, user units (`Type=notify`), `kajo-comp --greeter-mode`, fallback chain `kajo-comp → niri → cage` (§6.5).
- **Gate:** the checks in §6.5.9 pass; Niri greeter files stay until then.

**Phase 6 — Switch-over**
- Rename the `niri_*` helpers to compositor-neutral names, move `page_displays` to the new IPC, add a compositor settings page writing `outputs.toml`, retire the Niri-specific settings page.
- **Gate:** the owner signs off a parity checklist that lists the accepted regressions: no backdrop blur, no shader open/close/resize animations (15 presets unused), no overview, no software SDR-brightness control on external HDR monitors, no HDR pass-through in `kajo-player`.

### 8.3 Status

| Phase | State |
| :--- | :--- |
| Config split (previously "Phase 1") | **Done** (PRs #31, #32, #34); open items listed in the Revision 6 table. |
| 0 | **Closed.** HDR10 moved to v2 (Revision 7). |
| 1 | **Done** (pending commit). `src/comp/core/`: `comp_spring`, `comp_layout`, `comp_binds`, `comp_rules`, `comp_diag`, built as `libkajo-comp-core.a` with `-Werror` and the §7.2.2 set (dependencies: `xkbcommon`, `libpcre2-8`, vendored `tomlc17`; no wlroots/Vulkan/GLib). Tests `comp-spring` (7), `comp-layout` (11, incl. randomized invariants shown to fail on an injected bug), `comp-config` (8, incl. spec-example parity), fuzz smoke drivers. libFuzzer with ASan/UBSan: layout 554k runs / 61 s, config 5.2M runs / 61 s unseeded plus 2.05M runs / 91 s seeded from the spec examples (1150 edges); no crashes or leaks. Full suite 27/27, 0 warnings. |
| 2a | **Done.** `src/comp/{main,comp_server,comp_output,comp_input,comp_spawn,comp_listen}.c`, `comp_log.h`: display, backend (bare metal / nested / headless), automatic renderer, allocator, scene + output layout, outputs with the preferred mode, seat with keyboards and pointers, cursor, clipboard and primary selection, built-in Ctrl+Alt+Fn VT switch, startup command without a shell (`-- COMMAND`), SIGCHLD reaping, SIGINT/SIGTERM shutdown, `SIGPIPE` ignored. Globals: `wl_compositor`, `wl_subcompositor`, `wl_shm`, `zwp_linux_dmabuf_v1` (if the renderer has a DRM fd), `wl_data_device_manager`, primary selection, viewporter, single-pixel buffer, fractional scale, presentation. Strict flags + `-Werror` with system headers as `-isystem`. Tests: `comp-headless` (6 checks), `comp-audit`. ASan+UBSan+LSan clean for pixman, GLES2 and Vulkan headless runs, with one suppression for Mesa's EGL driver (`scripts/lsan-comp.supp`). **Not yet run nested or on bare metal.** No windows yet (xdg-shell is 2b). |
| 2b–6 | Not started. |

### 8.3.1 Phase 1 notes

- **Layout action names** live in one table (`comp_action_name()` in `comp_layout.c`), tested for completeness and uniqueness. They cover the layout-level binds of §4.3.2 plus `consume-window-into-column` and `expel-window-from-column`. Binds in §4.3.2 that are **compositor-level, not layout** (`close-window`, `fullscreen-window`, `next-window`/`previous-window`, `screenshot*`, `spawn`, `show-hotkey-overlay`) or **not yet modelled** (`expand-column-to-available-width`, `center-visible-columns`, `set-column-width`/`set-window-height` with a delta are covered by `comp_layout_adjust_column_width()` / `_window_height()`) will be resolved by `comp_binds.c`; unknown action names must be rejected with a clear error there.
- **Centering semantics as implemented:** `never` pans minimally and left-aligns a column wider than the view; `on-overflow` is the same but centers such a column; `always` keeps the focused column centered; `always_center_single_column` centers the whole strip whenever it is narrower than the view. `center-column` is a one-shot request consumed by the next camera-target query.
- **Column width formula:** `(view_w − gap) × proportion − gap`, so two 0.5 columns plus three gaps fill the view exactly (tested). A maximized column is `view_w − 2 × gap`.
- **Workspaces:** exactly one empty workspace at the end; empty workspaces elsewhere are removed when they lose focus (tested).
- **Binds as implemented:** key specs are `Modifier+...+Key`; `Mod` means Super (the compositor passes the meaning, so a nested session can map it to Alt). Keys are xkbcommon keysym names, case-insensitive, stored lower-case and matched against the keysym the key produces **without** Shift, with an exact modifier match: `Mod+Shift+S` fires for Super+Shift+s on any layout. Wheel triggers: `WheelScroll{Up,Down,Left,Right}`. `spawn` takes an argv array only (no shell). Planned actions (`toggle-overview`, `show-hotkey-overlay`, `screenshot*`, `expand-column-to-available-width`, `center-visible-columns`) are rejected with "planned but not available in this version"; the two layout ones were commented out of the §4.3.2 example for that reason.
- **Rules as implemented:** PCRE2 regexes (UTF-8, invalid UTF-8 input tolerated), unanchored unless anchored; all given match fields must match; later rules override per property. `blur`, `xray`, `blur_popups` in `[[layer_rule]]` are reported as not supported in this version.
- **2a implementation notes:** server-level listeners (on backend, cursor, seat signals) live in a separate talloc context that the server destructor frees *before* destroying those objects; wlroots 0.20 asserts that listener lists are empty on destroy, and freeing them afterwards would be a use-after-free. Live event sources (signals) are removed explicitly because `wl_display_destroy()` does not free them. Spawned clients run in their own session and outlive the compositor (§7.5.4).
- The model uses plain `malloc`/`realloc` arrays rather than `talloc`; `talloc` is planned for the compositor object tree in Phase 2, not for this self-contained core.

### 8.4 Phase 0 findings so far (assistant-run, nested/headless only; 2026-11-10)

| Question | Result | Evidence |
| :--- | :--- | :--- |
| 8. Can the `headless` backend create a Vulkan renderer? | **Yes** on Host A (Intel Lunar Lake, Mesa 26.2.6, device "Intel(R) Graphics (LNL)", Vulkan API 1.4.354). | Spike log: renderer reports `VULKAN` with `input_color_transform=1`, `output_color_transform=1`, `timeline=1`. So the §7.4 headless chaos test can use the Vulkan renderer. |
| Globals offered by the stock stack | `wp_color_manager_v1` v2 (parametric; transfer functions gamma22, st2084_pq, ext_linear, bt1886; primaries srgb, bt2020; intent perceptual), `wp_color_representation_manager_v1`, `zwp_linux_dmabuf_v1` v5, `wp_linux_drm_syncobj_manager_v1`, viewporter, single-pixel-buffer, fractional-scale, presentation. | `wayland-info` against the spike. Note: no plain sRGB piecewise function is advertised. |
| Does the nested `wayland` backend expose HDR? | **No**: `supported_primaries=0` and `supported_transfer_functions=0`, so HDR stays off. Nested runs cannot test HDR. The `headless` backend likewise. | Spike log with `--hdr`; it fell back to SDR with the reason printed. |
| Does a client see the color manager? | **Yes**: `mpv 0.41 --vo=gpu-next --gpu-context=waylandvk` registers `wp_color_manager_v1`, reports "parametric image description creator" and "setting mastering display primaries" supported, and reads the output description (gamma2.2 / bt.709, 0.2–80 cd/m²) on the SDR headless output. | mpv `-v` log. mpv's behaviour on a real PQ output is not yet observed. |
| SDR render path correct? | **Yes**: patches `#000000 #111111 #1a1a1a #0078d4 #808080 #ffffff` read back from the output buffer as exactly the same 8-bit codes. | Spike `--selftest` (reads the committed output buffer through `wlr_texture_read_pixels`). |
| HDR pixel values (203 cd/m² mapping) | **Not yet measured.** The self-test decodes PQ codes to cd/m² and prints the expected values for sRGB-piecewise and gamma 2.2; needs a DRM output. | Owner run B on a TTY. |
| 1. Does `eDP-1` report BT2020 + PQ? (bare metal, Host A, owner-run 2026-11-10) | **No — because of an EDID-parsing gap, not the hardware.** wlroots reported `supported_primaries=0`, `supported_transfer_functions=0`, so HDR was never attempted and the Alt+F2 toggle had no visible effect. The panel's HDR data (Colorimetry BT2020RGB; HDR static metadata type 1 with SMPTE ST2084; 616.9 / 400 / 0.0004 cd/m²) lives in CTA data blocks embedded in a **DisplayID 2.0** extension. Installed `libdisplay-info 0.3.0` does not search there; **0.4.0 does** (commit `73ec53d`, "info: search CTA blocks in DisplayID v2 extensions"). Kernel side is ready: the `xe` driver exposes `Colorspace` (incl. `BT2020_RGB`), `HDR_OUTPUT_METADATA` and `max bpc` 6–12 on the connector. | Logs `scratch/hdr-spike/logs/A–F`; `edid-decode` of `/sys/class/drm/card0-eDP-1/edid`; a test program against the installed 0.3.0 (`pq=0 bt2020_rgb=0`) and against 0.4.0 built in `/tmp` (`type1=1 pq=1 bt2020_rgb=1, 616.9/400.0 cd/m²`); `modetest -c`. SDR self-test on the real panel at 2880×1800: all six patches exact. |
| 2, 3, 5, 6, 7 | **Waiting** for the second bare-metal round with `--force-hdr-caps` (`./run.sh all forced`, `./run.sh e forced`). That flag sets the two capability bits itself, only when the kernel connector has the `Colorspace` and `HDR_OUTPUT_METADATA` properties. | — |
| 4 | Not implemented in the spike yet. | — |

| 2–7 (round 2, `--force-hdr-caps`, owner-run) | HDR mode commits with `XR30` and `XB4H`, with or without mastering metadata; runtime toggling works. **Colour mapping wrong** (owner: gamma curve totally wrong; self-test: SDR white encoded as PQ 1023). Root cause: spike bug, the image description bypassed `wlr_scene_output_build_state()`. | `logs/*-forced.log`. |

**For v2:** (1) apply output image-description changes through the scene's build-state path; (2) fix or work around `libdisplay-info` 0.3.0 (0.4.0 has soname `.so.4` and needs a wlroots rebuild; a local package is the planned route); (3) re-run the self-test, which already decodes PQ and prints the expected 203 cd/m² values.

---

## 9. Dependency Inventory

Checked with `pacman -Q` / `pacman -Si` / `pkg-config` on the development machine (Arch/CachyOS) on 2026-11-09. "Installed" means installed on this machine at the version shown. Anything not listed here is not a dependency of this spec.

### 9.1 Build & link dependencies of `kajo-comp` (all installed)

| Package | Installed | Purpose |
| :--- | :--- | :--- |
| `wlroots0.20` | 0.20.2 | Compositor library (`pkg-config wlroots-0.20`; build with `-DWLR_USE_UNSTABLE`). Arch build links `libvulkan`, `libEGL`/`libGLESv2`, `libseat`, `libdisplay-info`, `libliftoff`, `libinput`, `libdrm`, XCB (`ldd`). |
| `wayland` / `wayland-protocols` | 1.26.0 / 1.50 | Server library, `wayland-scanner`, protocol XMLs. |
| `libdrm` | 2.4.134 | DRM/KMS. |
| `libinput` | 1.32.0 | Input (also `libevdev` 1.14.0, `mtdev` 1.1.7). |
| `libxkbcommon` | 1.13.2 | Keymaps for binds and XWayland. |
| `pixman` | 0.46.4 | Damage regions. |
| `seatd` (`libseat.so.1`) | 0.9.3 | Seat/session access without root. |
| `libdisplay-info` | 0.3.0 | EDID parsing — used by wlroots; direct use by `kajo-comp` only if §3.2.2 EDID metadata experiment is adopted. |
| `libliftoff` | 0.5.0 | Linked by wlroots' DRM backend. **Transitive; not called by `kajo-comp`.** |
| `hwdata` | 0.412 | PNP/EDID vendor names used by wlroots. |
| `lcms2` | 2.20 | Installed; wlroots ICC support. Not used directly by v1. |
| `talloc` | 2.5.0 | Hierarchical allocator (§3.8). Package name is `talloc`, pkg-config `talloc`. **Licence:** LGPL-3.0-or-later per the header (package metadata says GPL-3.0-or-later); dynamic link only; not MIT (§9.8). |
| `glib2` | 2.90.1 | `GString`/`GHashTable`/regex helpers only; **no `GMainLoop`** inside `kajo-comp` (§4.2). |
| `json-glib` | 1.10.8 | IPC JSON on the socket; `src/config/kajo_config_migrate.c` also uses it for the legacy `config.json` importer only. |
| `systemd-libs` | 262.1 | `sd_notify` (`Type=notify`, watchdog); provides `libsystemd`/`libudev`. |
| `vulkan-icd-loader` | 1.4.363.0 | Vulkan loader (`pkg-config vulkan`). |
| `mesa` + `vulkan-intel` | 26.2.6 | GPU driver for Host A. `vulkan-radeon` and NVIDIA drivers are **not installed here**; Host B's driver is a per-machine requirement. |
| `xorg-xwayland` | 24.1.14 | X11 server for `wlr_xwayland`. |
| `libxcb`, `xcb-util-wm`, `xcb-util-errors` | 1.17.0 / 0.4.2 / 1.0.1 | XCB bits wlroots' XWM uses. |
| `pango` (`pangocairo`) + `cairo` | 1.58.2 / 1.18.6 | **Only if** the compositor draws text (tab headers, hotkey overlay) — §3.4.3. Not needed for v1 if those are omitted. |
| `libpng` | 1.6.59 | PNG encode in `kajo-capture` (client), not in the compositor. |

### 9.2 Build tooling (installed)

| Tool | Installed | Purpose |
| :--- | :--- | :--- |
| `meson` / `ninja` / `pkgconf` | 1.12.1 / 1.13.2 / 3.0.10 | Build system. |
| `gcc` / `clang` / `llvm` | 16.2.1 / 23.1.4 / 23.1.4 | Compilers; `-fanalyzer` (gcc), sanitizers and **libFuzzer** (clang). Verified: `clang -fsanitize=fuzzer,address,undefined` links and runs a trivial target. |
| `clang-tidy` | with `clang` 23.1.4 | Static analysis (`bugprone-*`, `clang-analyzer-*`). |
| `gdb`, `coredumpctl` | 18.1 / present | Post-mortem debugging. |
| `shaderc` (`glslc`), `glslang`, `spirv-tools` | 2026.4 / 1.4.363 | **Not needed for v1** (no custom shaders). Needed only for §10. |

### 9.3 Additional packages (all installed on the development machine as of 2026-11-10)

| Package | Installed | Why it is needed |
| :--- | :--- | :--- |
| `vulkan-headers` | 1:1.4.363.0-1 | `<wlr/render/vulkan.h>` includes `<vulkan/vulkan_core.h>` (`/usr/include/vulkan/vulkan_core.h` now present). |
| `vulkan-validation-layers` | 1.4.363.0-1.1 | `VK_LAYER_KHRONOS_validation` for local debug runs (§7.3). |
| `vulkan-tools` | 1.4.363.0-1.1 | `vulkaninfo` to confirm HDR-capable surface/formats on each host (Phase 0). |
| `xdg-desktop-portal-wlr` | 0.8.4-1.1 | ScreenCast/Screenshot portal backend (§3.7.1). |
| `cppcheck` | 2.22.0-1.1 | Third static analyser in `scripts/audit.sh` (§7.3). |
| `valgrind` | 3.25.1-9.1 | `--track-fds=yes` descriptor audits (§7.3.4). |
| `ddcutil` | 3.0.2-1 | **(planned)** DDC/CI brightness for external monitors (§3.2.3). Installed; not used by v1. Run the executable, never link `libddcutil`. |

On a fresh machine: `paru -S --needed vulkan-headers vulkan-validation-layers vulkan-tools xdg-desktop-portal-wlr cppcheck valgrind ddcutil`.

### 9.4 Vendored

| Component | Source | Licence | Notes |
| :--- | :--- | :--- | :--- |
| `tomlc17` | `src/vendor/tomlc17/`, **upstream `cktan/tomlc17`** | MIT (`LICENSE` added on branch `revert/poppler-removal`) | 3,350 + 291 lines; header `Copyright (c) 2024-2026, CK Tan`. README credit added on the same branch (§9.8). |
| `tinyexpr` | `src/vendor/tinyexpr/` | zlib (SPDX header and notice in `tinyexpr.c`) | Pre-existing; unrelated to this spec; not credited in README. |

### 9.5 Runtime programs referenced by configs (already installed)

`kitty` 0.49.2, `playerctl` 2.4.1, `gnome-keyring` 51.1, `polkit-gnome` 0.105, `pipewire` 1.6.10, `wireplumber` 0.5.18, `dbus` 1.16.2, `xdg-utils`, `libei` 1.6.0 (installed; not referenced by this spec). Informational: `wlr-randr` 0.5.0 and `wayland-utils` 1.3.0 are installed and useful for Phase 0. `grim`/`slurp` are installed but superseded by §3.5 once implemented.

### 9.6 Evaluated and rejected

| Package | Reason |
| :--- | :--- |
| `scenefx` 0.5 (`extra/scenefx`, depends on `wlroots0.20`, `mesa`, `libglvnd`) | GLES-only; HDR10 exists only on the Vulkan renderer (§2). Revisit if a Vulkan backend appears. |
| `libgc` (`bdw-gc`) | Conflicts with wlroots listener lists and ASan (§3.8.2). `libgccpp` is present on disk but unused. |
| `Aquamarine`, `libkwin`, `Louvre`, `Mir` | §2. |

### 9.7 Drift guard

This table is only trustworthy if it can fail. Add a `scripts/audit.sh deps` check that (a) runs `pkg-config --exists` for every library in §9.1 and (b) fails if `/usr/include/vulkan/vulkan_core.h` is missing; run it locally before Phase 0 and before each release of the source.

### 9.8 Licence policy and audit of this spec's dependencies (from `pacman -Qi` / `-Si` / file headers; not legal advice)

**Policy (stated by the owner):** Kajo's own code is MIT. All dependencies are linked dynamically. Nothing is vendored unless unavoidable. Only source is distributed; there are no binary releases, no packaging effort and no CI. Builds are for private use.

**What follows from the policy:** copyleft (GPL, and the LGPL's relinking terms) applies when a *combined binary is distributed*. Since none is, Kajo's source stays MIT regardless of which libraries it links. Anyone else who builds and redistributes a binary is responsible for their own compliance. Optional courtesy: a README sentence saying that binaries linking `libmpv`, `libsidplayfp` or `uade` are GPL as a combination. The policy rules out static linking of any LGPL or GPL library.

| Package | Licence (source of the statement) | Use | Note |
| :--- | :--- | :--- | :--- |
| `wlroots0.20`, `wayland`, `wayland-protocols`, `libdrm`, `libinput`, `libxkbcommon`, `pixman`, `seatd`, `libdisplay-info`, `libliftoff`, `lcms2` | MIT (package metadata) | linked | None. |
| `vulkan-icd-loader`, `vulkan-headers`, `vulkan-validation-layers`, `vulkan-tools`, `shaderc` | Apache-2.0 (headers: Apache-2.0 OR MIT) (metadata) | linked / dev | Permissive. |
| `mesa` | MIT AND BSD-3-Clause AND SGI-B-2.0 (metadata) | runtime driver | None. |
| `xdg-desktop-portal-wlr` | MIT (metadata) | separate process | None. |
| `xorg-xwayland` | permissive X11-style mix (metadata) | separate process | None. |
| `glib2`, `json-glib`, `systemd-libs` | LGPL-2.1-or-later (metadata) | dynamic | Same as every existing Kajo binary. |
| `pango`, `cairo` | LGPL-2.0+ / LGPL-2.1 OR MPL-1.1 (metadata) | dynamic, only if the compositor draws text (§3.4.3) | — |
| **`talloc`** | **LGPL-3.0-or-later per the installed `talloc.h` header; package metadata says GPL-3.0-or-later** | dynamic (`libtalloc.so.2`) | **Not MIT.** Acceptable under the policy above. The header is the notice from the authors; the metadata mismatch is unresolved. |
| `libpng` | libpng-2.0 (metadata) | `kajo-capture` (planned) | Permissive. |
| `cppcheck` (GPL-3.0+), `valgrind` (GPL-2.0+) | GPL (metadata) | dev tools, run as programs | Not linked into anything. |
| `ddcutil` | GPL-2.0-or-later (metadata) | **(planned)** DDC/CI brightness (installed) | Run the executable; do not link `libddcutil`. |
| `hwdata` | GPL-2.0-or-later (metadata) | PNP-ID data wlroots uses | Data file. |

**Existing Kajo dependencies that are copyleft** (for reference, no action required by this policy): `libsidplayfp`, `uade`, `mpv`/ffmpeg, plus LGPL libraries; see Revision 5.

**Vendored code (the one place the policy asks for something):** `tomlc17` is vendored (MIT, `Copyright (c) 2024-2026, CK Tan`) and `tinyexpr` is vendored (zlib, Lewis Van Winkle). Their notices must stay with the copies. **Done on branch `revert/poppler-removal`:** `src/vendor/tomlc17/LICENSE` added and both credited in the README's "License & Attribution" section (the `tinyexpr` repository URL in the README was written from memory and not checked). If you would rather not vendor `tomlc17`, a Meson `subprojects/` wrap that fetches upstream at build time would remove the copy; whether a wrap for it exists was **not checked**.

---

## 10. Deferred (Eye Candy and Other Post-v1 Work)

Nothing in this section is implemented or promised. Each item states what blocks it, using only facts verified above.

| Feature | Blocker (verified) | Possible approach |
| :--- | :--- | :--- |
| **HDR10 output (v2)** | Owner decision (Revision 7). Known blockers: `libdisplay-info` 0.3.0 misses DisplayID v2 HDR blocks (Host A panel); output image description must be applied through `wlr_scene_output_build_state()`. | §3.2 design + §8.4 findings; spike in `scratch/hdr-spike/` (uncommitted). |
| **Backdrop blur** (Dual-Kawase, Aero frosted glass) | `wlr_render_pass` offers only `add_texture` / `add_rect`; no multi-pass or offscreen API. `scenefx` is GLES-only. Scroll's author reports blur needs "big modifications" and "architectural modifications to render multiple passes" in the renderer, and that the Vulkan renderer still changes frequently. | (a) Patch `render/vulkan` in a carried `wlroots` fork; (b) run own Vulkan passes on the shared `VkDevice` (`wlr_vk_renderer_get_device()` etc.) and hand back a texture; (c) adopt `ext-background-effect-v1` once wlroots supports it. The cost of maintaining a fork must be decided by the project owner first. |
| **Window open/close/resize effects, SPIR-V/GLSL shader presets** | Same renderer limits. The 15 existing presets are written against Niri's shader API (`niri_tex`, `niri_geo_to_tex`, …) and are not directly portable. A close effect also needs a frozen texture of an already-unmapped surface; a resize crossfade needs two textures blended by progress. | Same options as blur; presets must be ported. |
| **Overview zoom** | `wlr_scene` has no tree transform; every surface and subsurface needs recomputed positions/destination sizes. | Manual per-`wlr_scene_buffer` scaling via `wlr_scene_buffer_set_dest_size()`; popups and subsurfaces need special handling. |
| **Adjustable SDR white (`sdr_brightness_nits`)** | Scene fixes SDR white at 203 cd/m² and refuses `color_transform` in HDR (§3.2.3). | Small `wlroots` patch to the luminance computation, or an upstream proposal. |
| **Compositor-side HDR tone mapping (BT.2390)** | No tone-mapping code in `wlroots 0.20.2` (searched `render/`, `types/`). | Custom shader in a patched renderer. |
| **HDR in `kajo-player`** | `GtkGLArea` has no color-state API in GTK 4.24.1 headers (§3.2.5). | `GdkDmabufTexture` with color state, or standalone `mpv` window. |
| **Rounded corners / shadows** | Not available in `wlr_scene`; also contradict the Aero+Metro `0px` rule (rounded corners are not wanted by the design system). | Shadows only, via renderer patch. |
| **DDC/CI brightness for external monitors** | Not in compositor; needs `ddcutil` and works only with `hdr = false` per rev. 1's analysis (unverified). | `ddcutil` helper in `kajo`. |
| **Window push-up when OSK is shown** | Requires layout reflow with a dynamic strut; not designed. | Layer-shell exclusive zone + layout reflow. |
