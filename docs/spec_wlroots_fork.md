# Spec: Kajo's wlroots fork (`kajo-comp` effects base)

**Status:** Milestone 1 implemented and verified (M1.1–M1.6). `kajo-comp` builds against the fork's `kajo-0.20.2-fx2` (upstream 0.20.2 plus `FORK.md`, no code changes) as a static subproject. Kajo branch: `feat/wlroots-fork`. Roadmap: [`ROADMAP.md`](../ROADMAP.md) §3.6.

Until Milestone 1, `kajo-comp` linked the system `wlroots0.20` package (0.20.2) through `dependency('wlroots-0.20')`; it now builds the fork as a static subproject (§4, K2). Effects such as blur, transitions, shadows and HDR tone mapping need render passes and scene features that stock wlroots does not offer, so Kajo builds from its own wlroots branch. Why a fork and not SceneFX or a replacement is recorded in ROADMAP §3.6.

**Principle:** the fork is upstream wlroots, unchanged, plus additive extensions. It can be swapped into any build that uses upstream wlroots and everything keeps working; no upstream functionality is ever removed. Breaking API or ABI compatibility is only allowed as a documented exception (C7).

---

## 1. Repositories

| Role | Location | Remote name in `../wlroots` |
| :--- | :--- | :--- |
| Upstream (source of truth) | `https://gitlab.freedesktop.org/wlroots/wlroots` | `upstream` (only fetched from) |
| Kajo's fork (what Kajo builds) | `git@github.com:mikolas/wlroots.git`; a plain public repository filled from upstream (upstream is not on GitHub, so it is not a GitHub fork) | `origin` |
| Submission fork (only if contributing) | a personal fork on gitlab.freedesktop.org, created when the first merge request is opened | `fdo` |

- **F1 (Mirror):** the fork carries upstream's full history and all release tags, so rebases, `git log` and `git range-diff` work as on upstream.
- **F2 (Sync):** `git fetch upstream --tags`, then push upstream's `master` and the new tags to `origin`. In `origin`, `master` and every upstream tag are exact copies of upstream: never committed to, only updated from upstream.
- **F3 (Licence):** wlroots is MIT. Kajo's patches are MIT as well and keep upstream's `LICENSE`; Kajo's `THIRD_PARTY_NOTICES.md` gains a wlroots entry when the submodule lands.
- **F4 (Separate repository):** the fork is maintained outside the Kajo repository, in the standalone clone `../wlroots` (sibling of `Kajo/`), which pushes to `origin` over SSH. Kajo stores only the submodule URL and one pinned commit. Nobody develops inside Kajo's `subprojects/wlroots` (detached HEAD; commits there are easily lost).
- **F5 (Tag before pin):** a `kajo-*` tag is pushed to `origin` before the Kajo commit that pins it, so every Kajo commit can be cloned. Bumping the pin is an ordinary Kajo commit: `git -C subprojects/wlroots fetch --tags`, `git -C subprojects/wlroots checkout kajo-<base>-fx<N>`, `git add subprojects/wlroots`, commit.

## 2. Branches, patch series and tags

### 2.1 Refs

| Ref in `origin` | Based on | Purpose |
| :--- | :--- | :--- |
| `master`, upstream tags | upstream | Mirrors only (F2) |
| `kajo/<upstream-tag>` (first `kajo/0.20.2`) | the upstream release tag (`0.20.2` = `d7835334`) | What Kajo builds: a patch series on top of the release |
| `kajo-<base>-fx<N>` tags (first `kajo-0.20.2-fx1`) | a commit on `kajo/<base>` | The exact commit Kajo's submodule pins |
| `topic/<name>` | upstream `master` | One generic change prepared for an upstream merge request (§2.4) |

- **B1 (Where work happens):** all Kajo development is committed to the current `kajo/<base>` branch, or to short-lived feature branches merged into it. Nothing is committed to `master`; a `topic/*` branch only ever holds a port of an upstreamable patch.
- **B2 (Pinned = immutable):** every commit Kajo pins gets a `kajo-<base>-fx<N>` tag. A tagged commit is never rewritten, and a `kajo/*` branch that a tag points into is never force-pushed, so every older Kajo revision still builds. Rework of an earlier patch therefore lands as a new commit on top.

### 2.2 Series order

On a freshly created `kajo/<base>` branch the series has two layers on top of the release tag:

1. **Upstreamable patches:** bug fixes and generic, mechanism-only API (for example an extensible render pass or a custom-shader hook). Each is self-contained, builds in every configuration of upstream's CI (`.builds/`, `.gitlab-ci.yml`; upstream has no test suite), follows wlroots' code and commit-message style, and does not mention Kajo.
2. **Kajo-only patches:** effects and policy that only `kajo-comp` uses (blur, transitions, experiments).

- **B3 (Dependencies between layers):** a Kajo-only patch may depend on an upstreamable one, never the other way round. Because of B2, commits added between base moves append on top in any order; the two-layer order is restored at the next base move (B6).
- **B4 (Additive changes):** new functionality lives in new files (`render/vulkan/kfx_*` for targets and passes, `types/scene/kfx_*` for scene traversal using only the generic render-pass API, never Vulkan internals; public headers under `include/wlr/kfx/`, the fork's own internal shaders under `render/vulkan/shaders/`); upstream files only get small, commented hooks. Effect shaders are never in the fork; they are Kajo's (effects spec §7.2). This keeps the replay onto a new release mechanical and makes C1–C6 easy to check.

### 2.3 `FORK.md`

The fork's root holds `FORK.md`: the refs, the current base, the compatibility exceptions (C7), the notes from the last base move, and one line per patch in series order:

| Field | Content |
| :--- | :--- |
| Patch | Commit subject |
| Layer | `upstreamable` or `kajo-only` |
| Status | `local`, `submitted !<MR>`, `merged in <version>`, `dropped (<reason>)` |
| Touches | Upstream files it modifies (new files are not listed) |

### 2.4 Contributing back

1. Port the patch onto a `topic/<name>` branch from current upstream `master` (upstream takes merge requests against `master`, not release tags) and adapt it to the API there.
2. Push the topic branch to the submission fork on gitlab.freedesktop.org and open the merge request there (GitHub cannot open merge requests on GitLab). The GitHub fork stays the build source.
3. Mark the patch `submitted !<MR>` in `FORK.md`. Review changes reach the `kajo/*` series as a new commit (B2) or at the next base move.
4. Once the change is in an upstream release, the patch is dropped at the next base move and marked `merged in <version>`. The series shrinks over time.

Realistic candidates, as an impression not confirmed with the maintainers: fixes, colour-management and HDR work, and small generic render API extensions. Ready-made effects in `wlr_scene` (blur and similar) have historically been left to compositors and projects like SceneFX, so they stay `kajo-only` unless upstream asks for them. Upstreamable patches use upstream naming, not the `wlr_kfx_` prefix (C4); the prefix is renamed when porting to `topic/*`.

### 2.5 Moving to a new upstream release

- **B5 (Cadence):** upstream minor releases come about yearly (0.19.0 2025-05, 0.20.0 2026-03), patch releases every few months. Kajo moves to each patch release (a cheap replay) and to a new minor release when it adopts it (the replay then also absorbs upstream API changes). Between moves, upstream security and crash fixes may be cherry-picked onto the current `kajo/*` branch.
- **B6 (Replay):** create `kajo/<new-tag>` from the new tag and replay the series onto it (rebase or cherry-pick): drop patches upstream has merged, squash fixups into their patch, restore the two-layer order (B3). Check each patch with `git range-diff`, then run the compatibility checks (§3.2). The old branch and its tags stay untouched. Kajo then bumps its submodule to `kajo-<new-tag>-fx1` (F5).

### 2.6 Alternatives considered

- **One long-lived branch that merges each upstream tag:** fewer conflict rounds, but tangled history makes clean patches for upstream hard to extract.
- **Tracking upstream `master`:** easiest upstreaming, but the API moves between releases; only worth it if most work goes upstream.
- **A patch-file series (quilt):** works, but is clumsier than branches with git and GitHub available.

## 3. Compatibility with upstream

### 3.1 Rules

- **C1 (Defaults):** every meson option keeps upstream's default value, and no upstream option, renderer, backend, allocator or protocol implementation is removed. A build of the fork with no options is upstream's build plus the additive extensions.
- **C2 (API and ABI):** existing public struct layouts, function signatures, enum values and macro meanings are never changed. Adding a field to a public struct breaks ABI for programs linked against the shared library, so new state lives in new structs, side tables or hooks. A program built against upstream `<base>` runs unchanged against the fork's library.
- **C3 (Behaviour when unused):** with no extension enabled, rendering, damage tracking and performance are identical to upstream: no extra render passes, allocations or wake-ups.
- **C4 (Naming and export):** `wlroots.syms` exports every symbol matching `wlr_*` or `_wlr_*`. New public symbols therefore start with `wlr_kfx_` (exported, and a prefix upstream is unlikely to use, so a future upstream name cannot collide); new public headers live in `include/wlr/kfx/`. Private helpers do not start with `wlr_` and stay unexported.
- **C5 (Feature detection):** the generated `wlr/config.h` gains `WLR_HAS_KFX` next to upstream's `WLR_HAS_*` macros, and the meson dependency / pkg-config file gains `have_kfx`. Both are added with the first API patch (Milestone 2), not before.
- **C6 (Version and name):** the project version (`0.20.2`), pkg-config name (`wlroots-0.20`) and library name stay upstream's, so version checks in other compositors pass and the fork can replace the system package.
- **C7 (Exceptions):** any break of C1–C6 is a deliberate, documented exception: an entry in `FORK.md` (what breaks, why, which alternatives were rejected) and its own feature macro so dependents can detect it.

### 3.2 Verification (run on every `kajo-*` tag that changes code)

- **V1 (Upstream configurations):** the fork builds as a shared library with upstream's default options, plus `tinywl` and the examples.
- **V2 (Exports):** the exported symbol list of the fork's library equals upstream `<base>`'s, plus only `wlr_kfx_*` symbols.
- **V3 (ABI, optional):** `abidiff` against upstream `<base>`'s library reports no changed or removed types or symbols. Needs `libabigail` (not installed on Host A).
- **V4 (Unused = upstream):** Kajo's headless suite passes with all extensions disabled.

## 4. Build integration in Kajo

- **K1 (Submodule):** `subprojects/wlroots` is a git submodule with the URL `https://github.com/mikolas/wlroots.git` in `.gitmodules`, so clones, `PKGBUILD` and CI need no credentials (the fork is public). Pushing happens only from `../wlroots` (F4). The submodule is checked out at a `kajo-*` tag.
- **K2 (Explicit subproject):** `meson.build` uses
  `comp_wlroots_dep = subproject('wlroots', default_options: [...]).get_variable('wlroots')`
  instead of `dependency('wlroots-0.20')`, so the system package can never be picked by accident. wlroots 0.20.2 also calls `meson.override_dependency('wlroots-0.20', …)`, so other `dependency('wlroots-0.20')` lookups in the same build resolve to the subproject.
- **K3 (Static, not installed):** `default_library=static`, so `kajo-comp` carries its wlroots and never conflicts with the system package. wlroots declares its library with `install: true`, so Kajo installs with `meson install --skip-subprojects`; without the flag 126 wlroots files (headers under `include/wlroots-0.20`, the static library, pkg-config files) would be installed and collide with the system package (measured with `meson install --dry-run`).
- **K4 (Options passed to the subproject):** upstream's defaults (C1) with only these changes:
  - `default_library=static` (K3);
  - `examples=false` (adds no functionality to `kajo-comp`);
  - `werror=false` (upstream defaults to `werror=true`, which would let a newer compiler fail Kajo's build on upstream code);
  - `xwayland=enabled`, `session=enabled`, `color-management=enabled`: features `kajo-comp` relies on are turned from `auto` into required, so a missing dependency fails the build instead of silently dropping the feature.
- **K5 (Meson version):** wlroots 0.20.2 requires meson `>= 1.3`; Kajo's `project()` declares `>= 0.60.0` and must be raised. Local meson is 1.12.1.
- **K6 (Dependencies):** the build dependencies of upstream's default build become Kajo build dependencies: `wayland` / `wayland-protocols`, `libdrm`, `libinput`, `libxkbcommon`, `pixman`, `seatd` (libseat), `libdisplay-info`, `libliftoff`, `hwdata`, `lcms2`, `egl` / `gbm`, Vulkan headers and loader, `glslang`, and the `xcb` libraries for XWayland and the X11 backend. The exact list is taken from the subproject's configure output in Milestone 1. wlroots' bundled wraps are not used (`wrap_mode=nodownload`).
- **K7 (Kajo still builds without the fork):** the effect code sits behind one facade (`src/comp/fx/comp_fx.h`); meson builds the real runtime when the wlroots dependency reports `have_kfx` and a no-op stub otherwise, so the rest of `src/comp/` never needs a guard (effects spec §15, CB1–CB2). Inside `src/comp/fx/`, extension-only code is guarded with `#if defined(WLR_HAS_KFX) && WLR_HAS_KFX` (wlroots emits feature macros with `set10`, so a disabled feature is defined as 0 and `#ifdef` would be wrong; `-Wundef` is added to `comp_strict_args`), so it also builds against plain upstream or the system package, with effects unavailable. This keeps P3 possible and is the Kajo-side half of the drop-in principle.

## 5. Packaging and developer setup

- **P1 (`PKGBUILD`):** add the fork as a second `source=` entry without a ref (the commit that is built is the submodule pin recorded in Kajo, so there is one place to bump, F5), point the submodule at that local mirror in `prepare()`, add the build and runtime dependencies (K6), install with `--skip-subprojects`, and install `THIRD_PARTY_NOTICES.md` and wlroots' `LICENSE` next to Kajo's (wlroots is linked into `kajo-comp`). `wlroots0.20` is not a dependency.
- **P2 (`DEVELOPMENT.md`):** `git clone --recursive`, `git submodule update --init` for existing clones, how to bump the pin (F5), and where the fork is developed (F4).
- **P3 (Plain-upstream build):** for comparisons, the submodule can be pointed at the plain upstream tag (`0.20.2`), or a meson option `-Dwlroots=system` (default `fork`, added in Milestone 2) builds against the system package; `kajo-comp` then builds without effects (K7). The system package is never used without the option (K2).

## 6. Milestone 1: plumbing, no behaviour change

Tasks:

1. ✅ Fill `git@github.com:mikolas/wlroots.git` from upstream (F1, F2: `master` and all tags), create `kajo/0.20.2` from `0.20.2` with only `FORK.md` (§2.3), tag `kajo-0.20.2-fx1`, push from `../wlroots` (F4). Done 2026-11-13: `master` mirrored at `72956e71`, 48 upstream tags; `kajo-0.20.2-fx1` = `8e598f9f` (first `FORK.md`), `kajo-0.20.2-fx2` = `70893923` (`FORK.md` with the compatibility rules and exceptions table, §2.3).
2. ✅ Add the submodule (K1) at `kajo-0.20.2-fx2`, switch `meson.build` (K2–K5), update `PKGBUILD` (P1), `DEVELOPMENT.md` (P2), `THIRD_PARTY_NOTICES.md` (F3) and ROADMAP §3.6.

Acceptance (all must hold):

- **M1.1:** `meson compile -C builddir && meson test -C builddir` with 0 warnings from Kajo's sources and all suites passing; `scripts/asan-test.sh` passing.
- **M1.2:** `comp-headless` passes on the subproject build (pixman).
- **M1.3:** `ldd builddir/kajo-comp` does not list `libwlroots-0.20.so`.
- **M1.4:** a fresh `git clone --recursive` builds with only the documented dependencies.
- **M1.5:** `meson install --skip-subprojects` into a temporary prefix installs no wlroots files.
- **M1.6:** a session on Host A behaves as before (keyboard, touchpad, screenshots, lock, XWayland). Manual check.

Results (2026-11-13):

- M1.1 ✅ 0 compiler warnings (Kajo and wlroots sources); 30/30 suites; ASan + UBSan 30/30 from a fresh build directory, no sanitizer reports.
- M1.2 ✅ `comp-headless` passes, also under ASan.
- M1.3 ✅ no `libwlroots` in `ldd`; 847 `wlr_*` functions linked in; debug binary 5.4 MB (was 1.1 MB), LTO package build 1.7 MB.
- M1.4 ✅ with limits: `makepkg` on a throwaway clone fetched the fork anonymously over HTTPS (global git config disabled), `prepare()` checked out the pinned `70893923`, the LTO build had 0 compiler warnings. Run with `--nodeps` on Host A, not in a clean chroot; every declared dependency is installed here, so a missing declaration would not have been caught.
- M1.5 ✅ `meson install --skip-subprojects` installs 358 files, none from wlroots; the package contains 389 entries, the only wlroots file is `usr/share/licenses/kajo-git/wlroots-LICENSE`. Without the flag, 126 wlroots files would be installed.
- M1.6 ✅ confirmed by the user on Host A: a live session on the new build works.

## 7. Later milestones

The effect pipeline, the fork's extension points (X1–X6, all `wlr_kfx_*`) and the milestones M2–M5 are specified in [`docs/spec_comp_effects.md`](spec_comp_effects.md). In short: M2 custom fragment shaders, node hooks and soft shadows; M3 glassy backdrop blur; M4 the output stage (SDR white, tone mapping, per-pixel colour filters); M5 transitions without new fork code. Each milestone adds patches to `kajo/0.20.2` under §2–§3 and ends with a new `kajo-0.20.2-fx<N>` tag and Kajo pin bump (F5).

Effects need the Vulkan renderer. On GLES2 or pixman they are unavailable and the renderer works as upstream (C3).

## 8. Decisions

- **D1 (Renderers):** all upstream renderers stay (C1); effects are Vulkan-only.
- **D2 (Repository):** `git@github.com:mikolas/wlroots.git` (submodule URL `https://github.com/mikolas/wlroots.git`); upstream fetched from gitlab.freedesktop.org; developed in `../wlroots`; all work on `kajo/*` branches, never on `master` (§1, §2).
- **D3 (libliftoff):** stays, as upstream's default (C1).
- **D4 (Prefix):** new public API uses `wlr_kfx_` and `include/wlr/kfx/` (C4).

No decision is open.

## 9. Not verified yet

- A build in a clean chroot (`extra-x86_64-build` or `makechrootpkg`), which would prove the dependency lists complete (M1.4).
- Upstream's contribution requirements (commit-message format, sign-off, CI expectations): read `CONTRIBUTING.md` before the first `topic/*` branch.
- V3 needs `libabigail`, which is not installed.
