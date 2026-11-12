# Config Harmonization Plan (`kajo-comp` ⇄ `KajoConfigStore`)

Status: **plan, not implemented.** Findings were verified with `rg` against branch `feat/kajo-comp` at `ce300c0`. Line numbers will drift; search by symbol.

Related: `docs/spec_toml_config.md` (store, describes the code as it is), `docs/spec_kajo_comp.md` §5 (compositor config).

## 0. The problem in one table

The suite reads and writes configuration through four separate mechanisms:

| | Suite store (`src/config/kajo_config_store.c`) | Compositor (`src/comp/core/comp_config.c`) | Settings' compositor writers | Niri KDL |
|---|---|---|---|---|
| Users | shell, settings, ide, files, mod, player, greeter | kajo-comp | `page_displays.c`, `page_niri.c` | `page_niri.c`, `page_displays.c` |
| Files | `{theme,shell,ide,files,mod}.toml` | `theme.toml`, `shell.toml`, `comp.toml`, `outputs.toml`, `binds.toml`, `rules.toml` | `outputs.toml`, `conf.d/40-layer-rules.toml` | `niri/config.d/*.kdl` |
| Schema | `KajoFieldSpec` tables (`src/config/schemas/*.c`) | hand-written section parsers | none (`g_string_append_printf`) | none |
| Writes | locked, patched in place, atomic | read-only | full rewrite, `g_file_set_contents` | full rewrite |
| Keys | `kajo_config_keys.h` | `comp_schema_keys.h` | string literals | string literals |

The compositor has no store domain, so settings has no shared way to read, write or validate compositor keys.

## 1. Target behaviour

All loaders resolve the same way, lowest to highest:

1. Compiled field defaults (one schema table per key).
2. Shipped defaults file: `$KAJO_DEFAULTS_DIR`, then `$XDG_DATA_HOME/kajo/defaults`, then `$XDG_DATA_DIRS/kajo/defaults` (`/usr/local/share`, `/usr/share`). Never the working directory.
3. *(optional, not present today)* `/etc/kajo/<file>`.
4. User base file in `$KAJO_CONFIG_DIR`, else `$XDG_CONFIG_HOME/kajo`. `include = [...]` applies where it appears.
5. `conf.d/*.toml`, sorted lexicographically. Each drop-in contributes **only** through its domain table (`[theme]`, `[shell.*]`, `[comp.*]`, …).
6. Runtime overrides (CLI `--config`, IPC output changes), with an explicit lifetime.

Rule for layer 2 vs 4: pick **one** behaviour for every domain. Today the store uses either the user file or the shipped file (documented in `spec_toml_config.md` §4, guarded by a parity test). The compositor layers shipped files under user files for `comp`/`binds`/`rules` only. Recommendation: keep the store's rule, extend the parity test to compositor domains, and drop the compositor's per-file special case. Layering only matters if shipped and compiled defaults are allowed to differ.

Each key has one owner domain. Other components read it through that domain's field spec, so it has the same range, enum and fallback everywhere.

## Batch 1: load order and discovery (read path)

| # | Fix | Evidence |
|---|---|---|
| 1.1 | The compositor must honour `KAJO_CONFIG_DIR`. It currently uses `--config`, `$XDG_CONFIG_HOME/kajo`, `~/.config/kajo`, `/etc/kajo`. Settings writes `outputs.toml` and `conf.d/` using `KAJO_CONFIG_DIR`, so the two can point at different directories. Also route `theme.c` (style.css, `style.d/`) and `launcher_surface.c` (`stats.json`) through the same resolver. | `comp_server_resolve_config_dir` (comp_server.c:~559), `kajo_config_store_new_for_dir` (kajo_config_store.c:~456), theme.c:116/128, launcher_surface.c:56 |
| 1.2 | One defaults search. The compositor uses `KAJO_COMP_DEFAULTS_DIR`, then **the working directory's `data/defaults/comp.toml`**, then `/usr/local`, then `/usr`, and skips `~/.local/share/kajo/defaults` (the dev sync target in AGENTS.md §3.3). The store uses `KAJO_DEFAULTS_DIR`, `~/.local/share`, `/usr/local`, `/usr`. Neither reads `XDG_DATA_DIRS`. Write one plain-C resolver (GLib-free, so `src/comp/core` can use it) and drop the working-directory lookup. | `resolve_defaults_dir` (comp_server.c:~571-585), `kajo_config_find_default_file` (kajo_config_schema.c:~1248) |
| 1.3 | One "shipped vs user" rule (see §1). For `theme.toml`/`shell.toml` the compositor never reads shipped defaults and falls back to C literals (`comp_config_new_defaults`: accent `#0078d4`, touch defaults). | comp_config.c:~285, ~315-319, ~1087-1090; kajo_config_store.c `load_base_domain_into` |
| 1.4 | Drop-ins must be scoped by domain table. The store applies a drop-in only through `[theme]`/`[shell]`/…; the compositor merges every drop-in as top-level compositor config. So `conf.d/x.toml` with `[theme] accent_color` recolours GTK but not borders, and `[touch] enabled=false` affects only the compositor. Introduce `[comp.*]` and keep bare top-level keys working for one release, with a deprecation diagnostic. | kajo_config_store.c:~129, comp_config.c:~1142 |
| 1.5 | Resolve the `[layout]` clash. `shell.toml [layout]` holds panel widgets and `comp.toml [layout]` holds tiling. The compositor parses `shell.toml`'s `[layout]` and relies on skipping `left`/`center`/`right`, so any tiling key placed in `shell.toml` is applied silently. Fixed by 1.4 plus reading only the theme/shell keys the compositor actually owns a consumer for. | comp_config.c:~398 |
| 1.6 | Same syntax-error policy everywhere. On a bad base file the store keeps the previous snapshot. The compositor treats a broken `theme.toml`/`shell.toml` as non-fatal and drops back to literals, so shell and borders show different accents. | `load_one_file_with_fallback(..., fail_on_syntax_error=false)` for THEME/SHELL (comp_config.c:~1087) |
| 1.7 | Define the lifetime of runtime output overrides. `comp_output_apply_config` prefers `[[output]]` rules over `manual_*`, so any reload silently reverts an IPC `set-output-config`. Either persist through the store (Batch 2) or let runtime overrides win until the next session. | comp_output.c:~810-811 |
| 1.8 | Make shadowed saves visible. Settings writes base files only; a drop-in that sets the same key wins with no message. Example: `conf.d/40-layer-rules.toml [layout] gap_px` overrides a hand-edited `comp.toml`. Store `update_*()` should report keys shadowed by `conf.d`, and settings should show it. | kajo_config_store.c `update_*`, page_niri.c:~691 |

## Batch 2: one way to manipulate config (schema and write path)

Depends on 1.4 (domain tables) and 2.1 (shared registry).

| # | Fix | Evidence |
|---|---|---|
| 2.1 | One key registry. `comp_schema_keys.h` duplicates `accent_color` and all `[touch]` keys from `kajo_config_keys.h`. `KAJO_CFG_FILE_COMP = "compositor.toml"` is dead and names a file nothing loads. | kajo_config_keys.h:10, comp_schema_keys.h:17, 93-96 |
| 2.2 | Add a compositor store domain. Move compositor field declarations (section, key, type, range, enum map, default) into a plain-C table shared by the compositor parser and a new `KAJO_CONFIG_DOMAIN_COMP` schema. `src/comp/core` stays GLib-free (`scripts/audit.sh comp`). Array blocks (`[[output]]`, `[[bind]]`, `[[rule]]`, `[[layer_rule]]`) get custom hooks the way `shell.toml [layout]` does. | comp_config.c parsers, kajo_config_schema.h `KajoDomainSchema` |
| 2.3 | Replace settings' hand-written compositor writers with store updates. Current bugs:<br>• `page_niri.c` writes and reads `match_namespace = ["^kajo$"]`. The compositor's key is `namespace` (comp_rules.c:300), so every layer-opacity rule is rejected ("needs a namespace"). Read-back compares `"^kajo$"` to `"kajo"`, so the slider never reflects the file.<br>• `page_displays.c` rebuilds `outputs.toml` from scratch: comments and unknown keys are lost, and missing fields of preserved entries are invented (`scale = 1.00`, `vrr = "off"`). Absent and `"off"` differ.<br>• Strings are interpolated with `%s`, unescaped. A `"` in an EDID name produces invalid TOML, which rejects the whole compositor reload (`outputs.toml` is a base file).<br>**Hotfix allowed before 2.2:** rename to `namespace`, fix the read-back, and escape strings with `kajo_toml_append_quoted_string()`. | page_niri.c:~380, ~683-706; page_displays.c:~275-345 |
| 2.4 | One validation per key. `[touch] dwell_ms`: compositor 200–30000, schema 500–15000. Swipe enums: the compositor accepts all 8 values for every key, the schema allows 3–4 per key. Colours: the compositor accepts `#rrggbbaa`, the theme schema only `#RRGGBB`. Invalid values: the compositor keeps the previous value, the schema uses its default. Solved by 2.1/2.2 (one field spec). | comp_config.c:~168-177, ~964; schema_shell.c:24-45, ~220-226 |
| 2.5 | `style.d/00-accent.css` is output only. `page_theme.c` falls back to parsing it for the accent; the compositor reads only `theme.toml`. Remove the CSS read path. | page_theme.c:~447-456, theme.c:~132 |
| 2.6 | Consumers must not re-encode defaults. `autohide.c` falls back to `hide_delay` 300 (schema 1000). Similar: `osk.c` 260, `settings_window.c` 900/640, `page_ide.c` `config ? x : TRUE`. A snapshot is never NULL after store init, so read the field, or `schema->fields[i].def` where needed. | autohide.c:138-142, 182-186; osk.c:902; settings_window.c:830-831 |
| 2.7 | Write only for the active backend. `page_niri.c` writes Niri KDL and compositor TOML on every save, then calls `shell_compositor_reload_config()` although inotify already reloads. | page_niri.c:~646-706 |
| 2.8 | One style rule. The compositor spec forbids inline tables; shipped `shell.toml [layout]` uses `{ id = …, mode = … }`. Pick one for the suite and document it in `spec_toml_config.md`. | data/defaults/shell.toml:~51 |
| 2.9 | Remove stale "CONFIG.JSON" storage badges. | settings_window.c:73, 81 |

## Batch 3: make sure every setting is read and applied

| # | Fix | Evidence |
|---|---|---|
| 3.1 | `autohide_reveal_delay` has no consumer outside settings. The settings page also copies the delay into `reveal_duration`, changing a different setting. Wire it into `autohide.c` and remove the copy. | `rg '(->\|\.)autohide_reveal_delay\b'` → only page_panel.c:142, 226-227 |
| 3.2 | The compositor parses `[touch] swipe_center`, `swipe_right` and `dwell_ms` but never uses them (the shell owns them in `panel.c`). Stop parsing them in the compositor after 2.1. | comp_config.c:~954-965; `rg 'touch_(swipe_center\|swipe_right\|dwell_ms)' src/comp --glob '!src/comp/core/comp_config.[ch]'` → 0 |
| 3.3 | `x > 0 ? x : fallback` turns a valid 0 into a magic default. The schema allows 0 for `hide_delay`, `reveal_duration` and `hide_duration`. | autohide.c:138-140, 182-184 |
| 3.4 | Narrow the compositor watcher. Any `*.toml` event in the config directory (or any subdirectory event) triggers a full compositor reload: keymap rebuild, output commits with possible step-down `notify-send`, and the 1.7 revert. kajo-files saving its view state or the IDE saving zoom triggers it. Filter to files actually loaded (track them as in `IncludeStack`) and watch include globs outside the directory. | comp_ipc.c `handle_inotify_fd` (~1416-1421) |
| 3.5 | Mark each key as live or restart-required. Only `kajo` (shell.c:94), `kajo-ide` (ide_window.c:1870) and `kajo-mod` (mod_window.c:1487) subscribe to store changes. kajo-files, kajo-player, kajo-settings and the greeter read once. Add a `live` flag to `KajoFieldSpec`, and have settings label restart-required keys. | `rg 'kajo_config_store_subscribe\('` |
| 3.6 | Expose compositor diagnostics. Today they are only `wlr_log`ged, which is why 2.3 went unnoticed. Add IPC `get-config-diagnostics` and show it in settings. | comp_server.c `comp_server_reload_config` |
| 3.7 | Store should warn on unknown keys and sections, as the compositor does (`warn_unknown_keys`). The store currently warns only on unknown enum values. | `rg -n 'unknown' src/config/kajo_config_schema.c` → enum warning only (~627) |
| 3.8 | Investigate the empty `~/.config/kajo/config.toml` that appeared on 2026-11-12. `rg 'config\.toml' src` finds nothing; probably a manual `--config` run. | — |

## Batch 4: tests that keep it from drifting

Land each with the batch it guards.

| Test | Guards | Shape |
|---|---|---|
| Defaults parity across loaders | 1.3, 2.4 | For every shared key: shipped TOML value == schema default == compositor default. Extend the existing `config-roundtrip` parity case to the compositor domain. |
| Precedence matrix | 1.1-1.6 | One fixture directory (shipped < user < include < conf.d, with and without `KAJO_CONFIG_DIR`). Store and `comp_config_load_dir` must resolve accent, `[touch]` and gap identically. |
| Writer round-trip | 2.3 | Each settings writer's output → `comp_config_load_dir` → zero diagnostics and expected values. Include an EDID name containing `"`. |
| Every field has a consumer | 3.1, 3.2 | Generate the list from the field tables; each field needs a consumer or an explicit `settings_only` flag (AGENTS.md §8.7). |
| Watcher scope | 3.4 | Write `files.toml` while kajo-comp runs headless; assert no reload log line. Write `comp.toml`; assert exactly one. |

## Suggested order

1. **Hotfix 2.3** (namespace key, read-back, string escaping) and **2.9**. Small and user-visible.
2. **Batch 1** with the parity and precedence tests. 1.1, 1.2 and 1.4 are mechanical.
3. **2.1 then 2.2**, then the rest of Batch 2.
4. **Batch 3.** 3.1, 3.3 and 3.4 can go at any time.

Each batch updates `docs/spec_toml_config.md` and `docs/spec_kajo_comp.md` §5 in the same commit (AGENTS.md §8.7).

## Not reviewed

`kajo_config_migrate.c`, the custom `[layout]` widget parser, and `schema_ide`/`schema_files`/`schema_mod` beyond the field-consumer check.
