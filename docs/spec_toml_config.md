# Domain-Split TOML Configuration & `KajoConfigStore` (`docs/spec_toml_config.md`)

**Status:** Implemented (PRs #31 and #32, plus the follow-up fixes on branch `revert/poppler-removal`).
This file describes the code as it is. Where it quotes values (defaults, ranges), they are generated from the compiled schemas in `src/config/schemas/`, not typed by hand.

**Modules:** `src/vendor/tomlc17/`, `src/config/`, `data/defaults/*.toml`, built into the shared `kajo_core` library (`meson.build`) and used by `kajo`, `kajo-settings`, `kajo-ide`, `kajo-files`, `kajo-player`, `kajo-mod` and `kajo-greeter`.
**Dependencies:** `glib-2.0`, `gio-2.0`; `json-glib-1.0` only inside the one-time legacy importer `kajo_config_migrate.c`; vendored upstream `tomlc17` (MIT, `src/vendor/tomlc17/LICENSE`).

---

## 1. What it replaces and why

Everything used to live in one `~/.config/kajo/config.json` (`src/config.c`, 1,068 lines before removal) that every process rewrote in full. Problems that motivated the change: concurrent writers could lose each other's updates; mode fields were heap strings compared with `g_strcmp0()`; `kajo-ide` and `kajo-mod` each watched the file by hand; shipped defaults had drifted from compiled defaults; JSON has no comments.

Now: small per-domain TOML files, typed enums, one table-driven schema engine, an in-process store with immutable snapshots, and a test that fails if the compiled defaults and `data/defaults/*.toml` ever differ.

---

## 2. Files and responsibilities

| File | Responsibility |
| :--- | :--- |
| `src/vendor/tomlc17/tomlc17.{c,h}` | Upstream `cktan/tomlc17` parser, unmodified (3,350 + 291 lines). Its headers mention TOML 1.1 behaviour (optional seconds, trailing commas and newlines in inline tables). |
| `src/config/kajo_config_keys.h` | Environment variable names, file names, section and key names. |
| `src/config/kajo_config_types.h` | Enums (`ShellPanelPosition`, `ShellPanelLayer`, `ShellWidgetMode`, `ShellToastClickAction`, `ShellSwipe*Action`, `ShellOskAutoShowMode`, `ShellScreensaverMode`, `KajoFilesViewMode`, `KajoFilesSortField`, `KajoMod*`), `ShellWidgetConfig`, the domain structs `KajoThemeCfg`, `KajoShellCfg`, `KajoIdeCfg`, `KajoFilesCfg`, `KajoModCfg`, and `g_autoptr` helpers (`Kajo*CfgRef` for acquired snapshots, `Kajo*Cfg` for mutable copies). |
| `src/config/kajo_config_schema.{h,c}` | The schema engine: `KajoFieldSpec` (section, key, type, offset, default, range or normalizer, enum map) and `KajoDomainSchema` (file name, domain table name, struct size, fields, custom hooks for the widget arrays). Implements defaults, parsing with validation and clamping, `conf.d` overlay application, full serialization, in-place patching, structural equality, default-file lookup. |
| `src/config/kajo_config_store.{h,c}` | `KajoConfigStore`: snapshots, transactional `update_<domain>()`, reload, directory watching, subscriptions. |
| `src/config/kajo_config_migrate.{h,c}` | One-time `config.json` → `*.toml` importer (the only code using `json-glib`). |
| `src/config/kajo_config_comp.c` | Shared compositor TOML read/write helpers (`comp.toml` `[layout] gap_px`, `rules.toml` `[[layer_rule]]`, `outputs.toml` `[[output]]`) used by `kajo-settings`. |
| `src/config/schemas/schema_{theme,shell,ide,files,mod}.c` | One `KajoDomainSchema` per domain. `schema_shell.c` also parses and writes the `[layout]` widget arrays. |
| `tests/test_config.c` | 12 GTest cases (section 8). |
| `tests/fuzz/fuzz_toml_config.c` | Fuzz entry point over all schemas; also built as the meson test `fuzz-toml-config`. |

---

## 3. Public API (as in the headers)

```c
/* Store */
KajoConfigStore *kajo_config_store_get_default(void);              /* $KAJO_CONFIG_DIR or ~/.config/kajo */
KajoConfigStore *kajo_config_store_new_for_dir(const gchar *dir);  /* hermetic store for tests */
void             kajo_config_store_destroy(KajoConfigStore *store);  /* g_autoptr(KajoConfigStore) */
const gchar     *kajo_config_store_get_config_dir(KajoConfigStore *store);

/* Snapshots: usable from any thread, release with the matching *_unref or g_autoptr(Kajo*CfgRef) */
const KajoThemeCfg *kajo_config_store_acquire_theme(KajoConfigStore *store);
const KajoShellCfg *kajo_config_store_acquire_shell(KajoConfigStore *store);
const KajoIdeCfg   *kajo_config_store_acquire_ide  (KajoConfigStore *store);
const KajoFilesCfg *kajo_config_store_acquire_files(KajoConfigStore *store);
const KajoModCfg   *kajo_config_store_acquire_mod  (KajoConfigStore *store);

/* Transactional writes of the BASE file (never bakes conf.d values into it) */
gboolean kajo_config_store_update_theme(KajoConfigStore *store, KajoThemeMutateFn fn, gpointer user_data, GError **error);
/* ... and _shell, _ide, _files, _mod with the matching Kajo*MutateFn */

/* Reload and notification */
void  kajo_config_store_reload(KajoConfigStore *store, KajoConfigDomain domains);
guint kajo_config_store_subscribe(KajoConfigStore *store, KajoConfigDomain domains,
                                  KajoConfigChangedCb cb, gpointer user_data, GDestroyNotify destroy_notify);
void  kajo_config_store_unsubscribe(KajoConfigStore *store, guint sub_id);
```

`KajoConfigDomain` is a bit mask: `KAJO_CONFIG_DOMAIN_THEME | _SHELL | _IDE | _FILES | _MOD` (`_ALL` = all five). Passing `NULL` as `store` selects the default store.

Environment variables: `KAJO_CONFIG_DIR` (config directory), `KAJO_DEFAULTS_DIR` (shipped-defaults directory, searched first).

---

## 4. Resolution order and what is read when

For each domain the effective configuration is built as follows (`build_effective_rc_snapshot()`):

1. Start from the **compiled defaults** of the schema.
2. Layer the first **shipped default file** (`defaults/<domain>.toml`) found in: `$KAJO_DEFAULTS_DIR` (fallback `$KAJO_COMP_DEFAULTS_DIR`), `$XDG_DATA_HOME/kajo/defaults` (or `~/.local/share/kajo/defaults`), each entry in `$XDG_DATA_DIRS` (`/kajo/defaults`, defaulting to `/usr/local/share:/usr/share`), and compiled `KAJO_DATA_DIR/defaults`. The current working directory and the source tree are not searched.
3. Layer the **user file** `<config dir>/<domain>.toml` (`$KAJO_CONFIG_DIR`, `$XDG_CONFIG_HOME/kajo`, or `~/.config/kajo`) if it exists, overriding only the keys it specifies.
4. Apply **drop-ins** `<config dir>/conf.d/*.toml` in lexicographic order. A drop-in contributes to a domain via its top-level domain table (`[theme]`, `[shell.<section>]`, `[ide]`, `[files]`, `[mod]`, `[comp.<section>]`). A drop-in with a syntax error is skipped with a warning.

Validation: integers and floats are clamped to the schema range (or run through the field's normalizer); wrong value types, unknown enum strings and invalid hex colours log a warning and keep the previous value; unknown keys are ignored.

**Syntax errors.** If the base file does not parse, the *previous snapshot is kept* on reload (only the very first load at process start falls back to defaults), and `update_<domain>()` refuses to write and returns an error, leaving the file untouched.

---

## 5. Writing (`update_<domain>()`)

1. Take the in-process `write_lock` and an exclusive `flock()` on `<config dir>/.<file>.lock` (so concurrent writers in other processes serialize).
2. Read and parse the base file. If it has a syntax error: return an error, change nothing.
3. Copy it to a draft, call the mutator, and compute which fields changed.
4. **Patch the existing text in place**: for each changed scalar, replace the value of its `key = value` line inside its `[section]` (or insert it), keeping comments, unknown keys and ordering. Untouched defaults are not written into the user's file. The `[layout]` widget arrays are rewritten as a block when they change.
5. **Verify the patched text.** It is parsed and compared with what a freshly serialized file would yield. If it does not parse or differs (this happens for keys written as quoted keys, dotted keys or inline tables, which the patcher does not recognise), the original text is saved to `<file>.bak`, a warning is logged, and a regenerated file is written instead. A broken file is never written.
6. Reload the domain and notify subscribers.

Limits to know about: comments *inside* the `[layout]` arrays are lost when the layout is saved; the `.bak` fallback loses comments and unknown keys from the live file (they remain in `.bak`); a file that has two definitions of one table is invalid TOML and is refused like any syntax error.

---

## 6. Reading, hot reload and notifications

- Snapshots are `g_atomic_rc_box` objects, immutable after publication. `acquire_*()` takes a reference under a short mutex; readers on any thread (for example the `kajo-mod` audio thread) never see a half-updated struct.
- `GFileMonitor` watches the config directory and `conf.d`. Events are coalesced for 50 ms (`g_timeout_add`), then the affected domains are rebuilt. The snapshot is replaced only if the new value differs structurally (`kajo_cfg_domain_equal()`).
- **Subscriber callbacks run on the GMainContext that was the thread-default context when `subscribe()` was called.** If that context is owned by the calling thread the callback runs immediately; otherwise it is queued there. Each subscription is reference-counted and has an `active` flag: a callback queued before `unsubscribe()` (or before the store is destroyed) is dropped rather than delivered, and `destroy_notify` runs when the last reference is released.
- The store needs a GLib main loop (`GFileMonitor`, `g_timeout_add`). It is not usable inside a program whose only event loop is a `wl_event_loop` (such as the planned `kajo-comp`); that program should link the schema engine and read files directly.

---

## 7. Domain files (generated from the compiled schemas)

Shipped default files live in `data/defaults/{theme,shell,ide,files,mod}.toml`. `shell.toml` additionally contains the `[layout]` tables `left`, `center` and `right`; each entry is a bare widget id string or an inline table with `id`, `mode` (`"icon"`, `"label"`, `"full"`), and for `custom-script` widgets `interval`, `exec`, `click_left`, `click_right` (see `docs/script_widgets.md`).

#### `theme.toml`

| Section | Key | Type | Default | Range / values |
| :-- | :-- | :-- | :-- | :-- |
| `(top level)` | `accent_color` | hex color | `"#0078d4"` | `#rrggbb` |
| `(top level)` | `ui_font_size_px` | int | `14` | 11..20 |
| `(top level)` | `font_heading_size_px` | int | `13` | 10..18 |
| `(top level)` | `font_subtitle_size_px` | int | `12` | 10..18 |
| `(top level)` | `font_badge_size_px` | int | `11` | 9..16 |
| `(top level)` | `css_path` | string | `""` | |
| `(top level)` | `settings_width` | int | `900` | 480..3840 |
| `(top level)` | `settings_height` | int | `640` | 360..2160 |

#### `shell.toml`

| Section | Key | Type | Default | Range / values |
| :-- | :-- | :-- | :-- | :-- |
| `panel` | `position` | enum | `"bottom"` | `"top"`, `"bottom"` |
| `panel` | `height` | int | `39` | 24..96 |
| `panel` | `layer` | enum | `"top"` | `"top"`, `"overlay"`, `"bottom"`, `"background"` |
| `panel` | `icon_size` | int | `24` | 12..64 |
| `autohide` | `enabled` | bool | `true` | |
| `autohide` | `reveal_delay` | int | `200` | 0..5000 |
| `autohide` | `hide_delay` | int | `1000` | 0..10000 |
| `autohide` | `reveal_duration` | int | `150` | 0..2000 |
| `autohide` | `hide_duration` | int | `200` | 0..2000 |
| `autohide` | `edge_strip_px` | int | `2` | 1..16 |
| `notifications` | `toast_timeout_ms` | int | `8000` | 1000..30000 |
| `notifications` | `max_history` | int | `50` | 10..200 |
| `notifications` | `click_action` | enum | `"focus"` | `"focus"`, `"dismiss"` |
| `notifications` | `dnd_enabled` | bool | `false` | |
| `touch` | `enabled` | bool | `true` | |
| `touch` | `swipe_center` | enum | `"launcher"` | `"launcher"`, `"overview"`, `"osk"`, `"none"` |
| `touch` | `swipe_right` | enum | `"control-center"` | `"control-center"`, `"notifications"`, `"osk"`, `"none"` |
| `touch` | `swipe_horizontal` | enum | `"column"` | `"column"`, `"workspace"`, `"none"` |
| `touch` | `dwell_ms` | int | `4000` | 500..15000 |
| `osk` | `auto_show_mode` | enum | `"tablet"` | `"tablet"`, `"always"`, `"manual"` |
| `osk` | `push_windows` | bool | `true` | |
| `osk` | `height_px` | int | `260` | 180..400 |
| `lock` | `screensaver_mode` | enum | `"overlay"` | `"overlay"`, `"dpms"`, `"disabled"` |
| `lock` | `screensaver_timeout_sec` | int | `300` | 0..86400 |
| `lock` | `dpms_timeout_sec` | int | `600` | 0..86400 |
| `lock` | `lock_timeout_sec` | int | `900` | 0..86400 |
| `lock` | `lock_on_suspend` | bool | `true` | |
| `hardware` | `kbd_backlight_level` | int | `2` | 0..2 |
| `hardware` | `kbd_backlight_timeout_sec` | int | `0` | 0..86400 |
| `hardware` | `posture_auto_rotate` | bool | `true` | |
| `hardware` | `posture_disable_kbd_in_tablet` | bool | `true` | |
| `hardware` | `posture_disable_touch_in_laptop` | bool | `false` | |

#### `ide.toml`

| Section | Key | Type | Default | Range / values |
| :-- | :-- | :-- | :-- | :-- |
| `(top level)` | `color_scheme` | string | `"kajo-metro-dark"` | |
| `(top level)` | `font_family` | string | `"Fira Code"` | |
| `(top level)` | `font_size_px` | int | `13` | 9..28 |
| `(top level)` | `tab_width` | int | `4` | only 2, 4 or 8 (anything else becomes the default) |
| `(top level)` | `insert_spaces` | bool | `true` | |
| `(top level)` | `word_wrap` | bool | `false` | |
| `(top level)` | `show_margin_guide` | bool | `true` | |
| `(top level)` | `show_minimap` | bool | `true` | |
| `(top level)` | `show_line_numbers` | bool | `true` | |
| `(top level)` | `highlight_current_line` | bool | `true` | |

#### `files.toml`

| Section | Key | Type | Default | Range / values |
| :-- | :-- | :-- | :-- | :-- |
| `(top level)` | `view_mode` | enum | `"grid"` | `"grid"`, `"list"` |
| `(top level)` | `sort_field` | enum | `"name"` | `"name"`, `"size"`, `"modified"`, `"type"` |
| `(top level)` | `sort_ascending` | bool | `true` | |
| `(top level)` | `sort_folders_first` | bool | `true` | |
| `(top level)` | `show_hidden` | bool | `false` | |
| `(top level)` | `show_inspector` | bool | `true` | |

#### `mod.toml`

| Section | Key | Type | Default | Range / values |
| :-- | :-- | :-- | :-- | :-- |
| `(top level)` | `filter_mode` | enum | `"a500"` | `"a500"`, `"a1200"`, `"near"`, `"linear"`, `"lin"`, `"sinc"` |
| `(top level)` | `led_mode` | enum | `"auto"` | `"auto"`, `"on"`, `"off"` |
| `(top level)` | `stereo_sep_pct` | int | `70` | 0..100 |
| `(top level)` | `end_action` | enum | `"next-sub"` | `"next-sub"`, `"loop"`, `"next-mod"` |
| `(top level)` | `sid_model` | enum | `"auto"` | `"auto"`, `"6581"`, `"8580"` |
| `(top level)` | `uade_timeout_sec` | int | `180` | 0 (no timeout) or 60..600 |
| `(top level)` | `auto_extract_archives` | bool | `true` | |
| `(top level)` | `hvsc_path` | string | `""` | |


---

## 8. One-time legacy migration (`kajo_config_migrate.c`)

- Runs when the store is created, only if `<config dir>/config.json` exists and at least one of the five domain files is missing.
- Takes an exclusive `flock()` on `<config dir>/.migrate.lock` and re-checks the condition under the lock, so several processes starting together migrate once.
- Parses the JSON (structured objects and the old flat keys) and writes each *missing* domain file; `shell.toml` is written last. When all five exist, `config.json` is renamed to `config.json.bak`. If a write fails, `config.json` stays and the next start retries.
- Ranges used while importing are the legacy ones and can differ from the schema ranges for some keys; the schema clamps again on the next load.

Other JSON files are not configuration and stay JSON: the launcher frequency cache, Niri IPC, per-project `.kajo/{tasks,debug}.json`.

---

## 9. Who uses what

| Component | Uses |
| :--- | :--- |
| `kajo` (`src/shell.c`) | Subscribes to shell + theme; applies a new snapshot to the panel, notification daemon, autohide, widgets. `widgets/theme.c` writes `theme.toml`; `widgets/backlight.c` writes `shell.toml`. |
| `kajo-settings` | Each page reads its domain and writes it with `update_<domain>()`; the Displays page collects UI state first and only then mutates a plain struct. |
| `kajo-ide` | Subscribes to `ide.toml`; zoom writes it. |
| `kajo-files` | Reads and writes `files.toml`. |
| `kajo-mod` | UI subscribes to `mod.toml`; audio and probe threads call `acquire_mod()`. |
| `kajo-player`, `kajo-greeter` | Read theme through `kajo_app_init_theme()` / `acquire_theme()`. The greeter runs as another user and therefore reads that user's config directory, then the shipped defaults. |

`00-accent.css` is generated from `KajoThemeCfg` only; `kajo-ide` applies its own font CSS from `KajoIdeCfg`.

---

## 10. Tests

`meson test` runs, among others:

- `config-roundtrip` (`tests/test_config.c`, 11 cases): defaults-file parity; per-domain round trips and clamping; `conf.d` overlay with base-save isolation; concurrent readers during reload; syntax-error preservation and save refusal; comment/unknown-key/datetime-preserving patch; **patcher never writes an unparsable file** (quoted key, dotted key, inline table); **subscriber callbacks arrive on the subscriber's thread and are dropped after unsubscribe**; legacy migration.
- `fuzz-toml-config`: smoke run of the fuzz entry point.
- `autohide-state`: includes a check that `edge_strip_px` survives a live reload.

Also verified by hand (scratch programs, not in the suite): two threads and two processes updating different keys of one file lose no updates (the `flock()` path).

Not covered by tests: the file monitor path end to end, `conf.d` syntax-error handling beyond the warning, and migration with two processes racing.

---

## 11. Known limitations

- Comments inside `[layout]` arrays are not preserved on save.
- `KajoConfigStore` is GLib-main-loop based (see section 6).
- Subscribers on a thread without a running main loop receive nothing until that context is iterated.
- The shipped-defaults lookup is by file name; there is no merge of a partial user file with shipped values (compiled defaults fill the gaps).
