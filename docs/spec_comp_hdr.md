# Specification: `kajo-comp` HDR10 Output & Mixed SDR/HDR Pipeline (`docs/spec_comp_hdr.md`)

**Status:** Planned Specification — design-only; **not yet implemented**  
**Target Binary:** `kajo-comp` (`org.kajo.comp`, `src/comp/`) + wlroots fork (`subprojects/wlroots`) + `kajo-settings` (`--page displays`)  
**Prerequisites Already Complete:** [`docs/spec_wlroots_fork.md`](spec_wlroots_fork.md), [`docs/spec_comp_effects.md`](spec_comp_effects.md) (`kajo-0.20.2-fx10` Milestones 1–5)  
**Primary Hardware Baseline:** Host A (`HP OmniBook X Flip 14` OLED `eDP-1`, `Samsung Display Corp. 0x41AA`, `2880×1800`, Intel Lunar Lake Xe2, Mesa `26.2.6`, `libdisplay-info 0.3.0`)

---

## 1. Scope, Reference Hardware & Already-Built Inventory

This specification defines how `kajo-comp` will detect HDR10-capable panels cleanly on stock distributions (unblocking the `libdisplay-info 0.3.0` DisplayID 2.0 gap), negotiate 10-bit PQ DRM/KMS scanout atomically, compose mixed SDR and HDR surfaces alongside linear-light `kfx` effects, expose full Wayland `wp-color-management-v1` negotiation, and integrate cleanly with `kajo-settings` and screenshots.

### 1.1 Already Built & Verified on `master`

Much of the compositor + fork pipeline needed for HDR already ships cleanly via [`docs/spec_comp_effects.md`](spec_comp_effects.md) M4:

| Component | Current Implementation | Status |
| :--- | :--- | :--- |
| **Linear Blend Working Space** | `two_pass.blend_image` (`VK_FORMAT_R16G16B16A16_SFLOAT`), `1.0 = SDR reference white`, unclamped (`~49.26` peak for 10,000 cd/m² PQ), output primaries, premultiplied alpha | Implemented in Vulkan fork (`kajo-0.20.2-fx10`) |
| **Effect Colour Contract (`H1–H8`)** | All shadows/dim/glass/transition shaders operate in linear space; `wlr_kfx_output_get_hdr_headroom()` feeds `common.hdr_headroom` (`src/comp/fx/fx_{shadow,dim,glass,transition}.c`) | Implemented |
| **Output Stage (`X6`)** | `wlr_kfx_output_set_sdr_white_nits()`, `wlr_kfx_output_set_tonemap()` (ITU-R BT.2390 EETF), and `wlr_kfx_output_set_dither()` wired in `src/comp/fx/fx_runtime.c` + `comp.toml` (`sdr_white_nits`, `tonemap`, `tonemap_peak_nits`) | Implemented |
| **SDR Wayland Color Manager** | `init_color_management()` (`src/comp/comp_protocols.c:520`) creates `wlr_color_representation_manager_v1` + `wlr_color_manager_v1` with `.parametric` + `.set_mastering_display_primaries` and calls `wlr_scene_set_color_manager_v1()` | Implemented (SDR only) |
| **Atomic DRM State Plumbing** | `subprojects/wlroots/backend/drm/atomic.c` implements `create_hdr_output_metadata_blob()`, `convert_primaries_to_colorspace()`, `WLR_OUTPUT_STATE_IMAGE_DESCRIPTION`; `comp_output.c` already commits through `wlr_scene_output_build_state()` | Implemented (latent; unused until an output commits an HDR `wlr_output_image_description`) |
| **Host A Headless Vulkan Baseline** | `WLR_BACKENDS=headless WLR_RENDERER=vulkan` exposes `input_color_transform` + `output_color_transform` and passes bit-exact 6-patch SDR self-test | Verified in Phase 0 spike (`ROADMAP.md §3.7.1`) |

### 1.2 What Is Still Missing (This Spec)

1. **DisplayID 2.0 HDR capability detection** when system `libdisplay-info` is `0.3.0`.
2. **10-bit PQ DRM/KMS mode & `wlr_output_image_description` output commit + step-down cascade** in `src/comp/comp_output.c`.
3. **Output HDR policy (`off` / `on` / `auto`)**, EDID luminance fallback extraction (`max_cll`, `max_fall`, `min_luma`), and direct-scanout safety.
4. **Expanded `wp-color-management-v1` feature set** (`set_luminances`, `extended_target_volume`, `windows_scrgb`) and dynamic per-surface preferred image description feedback.
5. **Colour filter / night light safety in HDR** (folding into linear output matrix instead of `[0, 1]`-clamped 3D LUT) and **SDR tone-mapped screenshots / screencopy**.
6. **Per-output `[[output]]` config (`comp.toml`), `$KAJO_COMP_SOCKET` IPC, and `kajo-settings` (`--page displays`) UI.**

---

## 2. Unblocking DisplayID 2.0 EDID HDR Detection (`libdisplay-info 0.3.0`)

### 2.1 Exact Bottleneck (`subprojects/wlroots/backend/drm/util.c:63-109`)

On Host A (`eDP-1` OLED), the kernel exposes `Colorspace` and `HDR_OUTPUT_METADATA` connector properties, and the panel's EDID encodes its BT.2020 colorimetry and PQ static metadata (`616.9 cd/m²` peak, `400 cd/m²` sustained, `0.0004 cd/m²` min) in **CTA-861 data blocks nested inside a VESA DisplayID 2.0 (`0x70`) extension block**.

System `libdisplay-info 0.3.0` (`di_displayid2` in `/usr/include/libdisplay-info/displayid2.h`) only parses DisplayID v2 revision and product primary use case; `di_info_get_supported_signal_colorimetry()` and `di_info_get_hdr_static_metadata()` skip DisplayID 2.0 nested CTA blocks completely. As a result, `parse_edid()` leaves `output->supported_primaries` and `output->supported_transfer_functions` as `0`.

### 2.2 Selected Strategy & Alternative

| Option | Design | Pros & Cons |
| :--- | :--- | :--- |
| **A. DisplayID 2.0 CTA Fallback in `wlroots` Fork *(Primary / Zero New Submodules)*** | Add a ~60-line pure C parser `kfx_edid_scan_did2_cta_hdr()` invoked from `parse_edid()` (`subprojects/wlroots/backend/drm/util.c`) when `libdisplay-info` leaves BT.2020 or PQ empty, scanning `0x70` extension blocks → DisplayID v2 Data Block `0x81` (CTA Data Blocks) → extended CTA tags `0x05` (Colorimetry) & `0x06` (HDR Static Metadata). | Works on stock Arch/distros with system `libdisplay-info 0.3.0`; zero extra submodules or build deps; trivial to unit-test with raw EDID binaries; naturally inert once `libdisplay-info >= 0.4.0` fills the bits first. |
| **B. Static Subproject of Upstream `libdisplay-info` (`>= 73ec53d`) *(Fallback)*** | Populate `subprojects/libdisplay-info` pinned to an upstream tag/commit `>= 0.4.0` using the existing `fallback: 'libdisplay-info'` in `subprojects/wlroots/backend/drm/meson.build:12` with `default_library=static` (matching `wlroots`). | Full spec-compliant DisplayID 2.0 coverage for arbitrary monitors, at the cost of carrying a second forked/pinned submodule and updating `PKGBUILD` sources. |

*Decision:* Implement **Option A** first (cleanest, minimal footprint on Host A and CI). If external HDMI/DP DisplayID 2.0 monitors later need additional non-HDR DisplayID v2 blocks (e.g., Type VII/VIII/X timings), promote to **Option B**.

### 2.3 EDID Metadata Parsing Contract & `wlr_kfx` Exposure

1. **DisplayID 2.0 extension block validation (`parse_did2_cta_hdr`):**
   - Base EDID is 128-byte aligned; scan extension block `b` (`offset = 128 * k`, `k >= 1`) where `b[0] == 0x70`.
   - Validate **both** the outer EDID checksum (`sum(b[0..127]) mod 256 == 0`) **and** the DisplayID v2 section checksum (`sum(b[1 .. 5 + section_len]) mod 256 == 0`) where `section_len = b[2]` and `section_len <= 121` (`b[1]` revision, `b[3]` primary use case, `b[4]` extension count).
   - Iterate DisplayID Data Blocks starting at **`b + 5`** up to `b + 5 + section_len` (`tag = db[0]`, `rev = db[1]`, `len = db[2]`, total size `3 + len`, strictly bounds-checked against `5 + section_len`). Stop on `len == 0 && tag == 0` padding.
   - For `tag == 0x81` (**CTA-861 Data Block** inside DisplayID 2.0), iterate nested CTA Data Blocks (`header = cdb[0]`, `tag_code = header >> 5`, `len = header & 0x1F`, total size `1 + len`, strictly bounds-checked against `db` end):
     - When `tag_code == 7` (Extended Tag Block, `len >= 2`): check `ext_tag = cdb[1]`:
       - `ext_tag == 0x05` (**Colorimetry Data Block**, `len >= 3`): check `cdb[2] & 0xE0` (`BT2020cYCC | BT2020YCC | BT2020RGB`). Set `WLR_COLOR_NAMED_PRIMARIES_BT2020` if `conn->props.colorspace != 0`.
       - `ext_tag == 0x06` (**HDR Static Metadata Data Block**, `len >= 3`): check `eotf_mask = cdb[2]` bit `2` (`0x04`, SMPTE ST 2084 / PQ) and `sm_desc_mask = cdb[3]` bit `0` (`0x01`, Static Metadata Type 1). Set `WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ` if `conn->props.hdr_output_metadata != 0`. Extract optional bytes per ANSI/CTA-861-H §7.5.13 (**checking `CV == 0` → `0.0f` unset, never `50 * 2^0 = 50`!**):
         - `desired_max_luma` (`cdb[4]` if `len >= 4`): `(CV == 0) ? 0.0f : 50.0f * powf(2.0f, CV / 32.0f)` cd/m²
         - `desired_max_fall` (`cdb[5]` if `len >= 5`): `(CV == 0) ? 0.0f : 50.0f * powf(2.0f, CV / 32.0f)` cd/m²
         - `desired_min_luma` (`cdb[6]` if `len >= 6`): `(CV == 0 || max_luma <= 0.0f) ? 0.0f : max_luma * powf(CV / 255.0f, 2.0f) / 100.0f` cd/m²
   - **Verified Hardware Reference Vector (Host A `/sys/class/drm/card0-eDP-1/edid`):**
     - `ext[1]` has `tag=0x70`, `ver=0x20`, `section_len=121`, `tag=0x81` at offset `87` (`len=31`) containing CTA Extended Colorimetry (`e3 05 80 00` → `BT2020RGB`) and CTA HDR Static Metadata (`e6 06 05 01 74 60 02` → PQ + Type 1, `CV 0x74=116 -> 616.884 cd/m²`, `CV 0x60=96 -> 400.000 cd/m²`, `CV 0x02=2 -> 0.000379 cd/m²`), matching `edid-decode` output cleanly.
2. **Fork HDR capability query (`wlr/kfx/scene.h` or `wlr_output` addon in fork):**
   - Upstream `struct wlr_output` exposes `supported_primaries`, `supported_transfer_functions`, and `default_primaries`, **not** the EDID-reported HDR peak/sustained/min luminance numbers.
   - Add a read-only helper in `wlr/kfx/scene.h`:
     ```c
     struct wlr_kfx_output_hdr_caps {
         bool supported;          /* BT.2020 + ST2084 PQ + DRM props present */
         float max_cll_nits;      /* EDID desired content max luminance (0 if omitted) */
         float max_fall_nits;     /* EDID desired content max frame-avg (0 if omitted) */
         float min_luma_nits;     /* EDID desired content min luminance */
     };
     bool wlr_kfx_output_get_hdr_caps(struct wlr_output *output,
                                      struct wlr_kfx_output_hdr_caps *out);
     ```
   - `parse_edid()` populates `max_cll_nits` / `max_fall_nits` / `min_luma_nits` both when `di_info_get_hdr_static_metadata()` succeeds (future `0.4.0+` or CTA-861 base blocks) AND in the DisplayID 2.0 fallback, stored privately on the DRM connector so zero upstream public structs change layout (`spec_wlroots_fork.md` ABI compatibility).

### 2.4 Luminance Unit Bugs & "Supernova" Prevention Invariants

Inspection of `subprojects/wlroots` against Linux `<drm/drm_mode.h>` and `edid-decode` surfaced **two concrete unit/scaling bugs** that must be fixed in our `wlroots` fork before enabling HDR10 (otherwise brightness explodes or bottoms out):

1. **Bug 1 — DRM `min_display_mastering_luminance` unit inversion (`subprojects/wlroots/backend/drm/atomic.c:238`):**
   - Per `<drm/drm_mode.h:1062-1067>`, `min_display_mastering_luminance` is a `uint16_t` coded in units of **`0.0001 cd/m²`** (`0x0001 = 0.0001 cd/m²`), whereas `max_display_mastering_luminance`, `max_cll`, and `max_fall` are coded in `1 cd/m²`.
   - `wlroots 0.20.2` has `.min_display_mastering_luminance = img_desc->mastering_luminance.min * 0.0001;` (dividing instead of multiplying — truncating `0.0004 cd/m²` to `0`!).
   - **Fix in fork:** Encode as `(uint16_t)fmin(65535.0, round(fmax(0.0, img_desc->mastering_luminance.min) * 10000.0))`, and clamp `max_display_mastering_luminance`, `max_cll`, and `max_fall` to `[0, 65535]`.
2. **Bug 2 — Mixed SDR/HDR `sdr_white_nits` supernova coupling (`types/scene/wlr_scene.c:1554-1560` vs `2313-2320`):**
   - `wlr_scene.c` normalizes incoming HDR PQ buffers in `scene_entry_render()` using `srgb_lum / src_lum` where `ST2084_PQ` has hardcoded `reference = 203 cd/m²` (`src_lum.reference`), so HDR `600 cd/m²` enters the RGBA16F blend buffer as `600 / 203 = 2.9557`.
   - At output encoding (`scene_output_combine_color_transforms`), `dst_lum.reference` is set to **`sdr_white_nits`** (`kfx_output_get_sdr_white_nits`), multiplying **every** blend-buffer pixel (including HDR PQ clients!) by `sdr_white_nits / 10000`.
   - Therefore, raising `sdr_white_nits` (e.g. `400 cd/m²`) currently multiplies HDR video by `400 / 203 ≈ 1.97×` (**supernova**!), while lowering it dims HDR movies!
   - **Fix in fork:** In `scene_entry_render()` (`wlr_scene.c:1554`), when `scene_buffer->transfer_function` carries absolute luminance (`WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ` or `EXT_LINEAR` scRGB), normalize using the output's active `sdr_white_nits` (matching `dst_lum.reference`) rather than hardcoded `203` whenever no explicit client luminance overrides it — guaranteeing `L cd/m²` HDR content exits at exact `L cd/m²` regardless of `sdr_white_nits`, while untagged SDR (`1.0`) scales cleanly to `sdr_white_nits`.
3. **Hard Physical Clamps & Startup Telemetry Log:**
   - Reject/clamp implausible EDID or user values before touching KMS or shader matrices:
     - `max_cll_nits`: `clamp(v, 100.0, 10000.0)` (if `0` / unset, default `400.0` cd/m², never unscaled `10000` unless panel declares it)
     - `max_fall_nits`: `clamp(v, 50.0, effective_peak_nits)`
     - `min_luma_nits`: `clamp(v, 0.0001, 5.0)` (if `0` / unset, default `0.005`)
     - `sdr_white_nits`: `clamp(v, 80.0, fmin(500.0, effective_peak_nits))`
   - Log exact decoded EDID nits (`did2` vs `di`), active `sdr_white_nits`, `effective_peak_nits`, `hdr_headroom`, and InfoFrame fields (`WLR_INFO`) on every SDR↔HDR commit.

---

## 3. DRM/KMS 10-Bit PQ Pipeline & Atomic Commit Cascade

### 3.1 Renderer & Format Requirements

- **Vulkan-Only Gate (`ROADMAP §3.7.1`, `spec_comp_effects.md §5`):**
  - HDR10 output is enabled **only** when the compositor runs on Vulkan (`wlr_renderer_is_vk` / `renderer->features.input_color_transform && renderer->features.output_color_transform` in wlroots `0.20.2`).
  - GLES2 and pixman do **not** advertise `input_color_transform` / `output_color_transform` in `wlroots 0.20.2`; if running without Vulkan transforms, HDR outputs remain in SDR cleanly and report `"renderer_unsupported"` reason in IPC/`kajo-settings`.
- **10-Bit Output Render Format (`DRM_FORMAT_XRGB2101010`):**
  - When committing HDR10 state on a DRM output, configure `wlr_output_state_set_render_format(&st, DRM_FORMAT_XRGB2101010)` (or `DRM_FORMAT_ABGR2101010` / `XBGR2101010` fallback depending on primary plane modifier intersection via `wlr_output_preferred_read_format` / atomic test).
  - Never stay on `DRM_FORMAT_XRGB8888` while `HDR_OUTPUT_METADATA` is `ST2084_PQ` (8-bit PQ produces severe shadow banding).

### 3.2 `wlr_output_image_description` Construction & Safety Invariants

1. **Mandatory Safe Primaries (`atomic.c:250` invariant):**
   - `convert_primaries_to_colorspace()` in `subprojects/wlroots/backend/drm/atomic.c` calls `abort()` on any value other than `0` (Default) or `WLR_COLOR_NAMED_PRIMARIES_BT2020`.
   - Compositor code **MUST ONLY** ever commit `primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020` when enabling HDR10 (`transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ`), and `image_description = NULL` when disabling HDR back to SDR (`8-bit`, `Colorspace = 0`, `HDR_OUTPUT_METADATA = 0`).
2. **Mastering Luminance / InfoFrame Fields (`struct hdr_output_metadata`):**
   - When HDR10 is active:
     - `primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020`
     - `transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ`
     - `mastering_display_primaries`: BT.2020 coordinates (`R=(0.708,0.292)`, `G=(0.170,0.797)`, `B=(0.131,0.046)`, `W=(0.3127,0.3290)`) — noting `atomic.c` leaves InfoFrame primaries populated directly from `mastering_display_primaries`.
     - `mastering_luminance`: `{ .min = caps.min_luma_nits > 0 ? caps.min_luma_nits : 0.0004, .max = effective_peak_nits }` (where `effective_peak_nits` uses user `hdr_peak_nits` override if non-zero, else `caps.max_cll_nits > 0 ? caps.max_cll_nits : 400.0`).
     - `max_cll = round(effective_peak_nits)`, `max_fall = caps.max_fall_nits > 0 ? round(caps.max_fall_nits) : round(effective_peak_nits)`.
3. **Commit Through `wlr_scene_output_build_state()` (`ROADMAP §3.7.1` rule 2):**
   - Enabling HDR **MUST NOT** call `wlr_output_commit_state()` on an empty state without a scene-built buffer/transform commit: `wlr_scene_output_build_state()` sets up the `wlr_scene` Vulkan color transform encoding SDR `1.0` → `sdr_white_nits / 10000` PQ into the 10-bit framebuffer so the first HDR scanout frame does not flash unencoded `gamma 2.2` values at full panel brightness.
   - Force full-output damage (`wlr_damage_ring_add_whole` / `wlr_kfx_scene_output_damage_box`) whenever switching between SDR ↔ HDR (`image_description` transition) so every pixel on screen re-encodes under the new EOTF cleanly.

### 3.3 Atomic Test-Before-Commit Cascade Integration (`src/comp/comp_output.c:638`)

Extend `commit_output_cascade()` cleanly preserving the existing cascade contract:

1. **Step 1 (Full Requested State):** Requested mode + scale + transform + requested VRR + **requested HDR10 (`XRGB2101010` + `BT2020/PQ` via `wlr_scene_output_build_state`)**.
2. **Step 2 (Step down VRR):** Retry HDR10 + `vrr = off` (record `o->vrr_unsupported = true`).
3. **Step 3 (Step down HDR → SDR 8-bit):** If 10-bit PQ atomic test fails (e.g., display link bandwidth cannot carry `4K@120Hz 10-bit` or driver rejects `HDR_OUTPUT_METADATA`), fall back cleanly to `image_description = NULL` + default `8-bit` format at the requested mode/refresh, recording `o->hdr_unsupported = true` (or `o->hdr_fallback_sdr = true`) and logging why.
4. **Step 4–6 (Existing cascade):** Lower refresh rate at same `WxH` → preferred mode → scale/transform only.

---

## 4. Mixed SDR / HDR Colour Pipeline, `kfx` Effects & Colour Filters

```text
  [Untagged SDR client]  ── sRGB EOTF + SDR primaries ─┐
  [PQ / scRGB HDR client] ─ Client TF + gamut matrix ──┼──> Linear RGBA16F Blend Image
  [kfx effects / wallpaper / chrome] ──────────────────┘    (1.0 = SDR white, output primaries, unclamped)
                                                                       │
                                              Subpass 0 (`output.frag`) ▼
                                     (1) Linear Colour Filter / Night Light 3×3 Matrix (unclamped, >1.0 preserved!)
                                     (2) Output matrix (`sdr_white_nits / 10000` for HDR, `1.0` for SDR) + HDR gamut
                                     (3) X6 BT.2390 EETF PQ Tone Mapping (`hdr_peak_nits`, enabled when tonemap=true)
                                     (4) Inverse EOTF (PQ ST2084 for HDR10, Gamma 2.2/sRGB for SDR)
                                     (5) X6 Blue-Noise Dither (±0.5 LSB at 10-bit or 8-bit code values)
                                                                       ▼
                                                       DRM_FORMAT_XRGB2101010 (HDR10)
```

### 4.1 Critical Fixes & Guarantees Required in `output.frag` / `wlr_scene`

1. **SDR White Level Consistency (`comp.toml` `sdr_white_nits`):**
   - `wlr_kfx_output_set_sdr_white_nits()` (`0 = 203.0 cd/m²` ITU-R BT.2408 default, or user `80..1000`) scales linear SDR `1.0` relative to `10,000 cd/m²` PQ.
   - All `kajo-comp` rects, borders, shadows, overview labels, and `kfx` glass tints stay calibrated at SDR `1.0` (`203 cd/m²` default), never blinding white on an OLED in HDR mode.
   - `wlr_kfx_output_get_hdr_headroom(wo)` automatically returns `max(1.0, effective_peak_nits / effective_sdr_white_nits)` (`~3.04` on Host A `616.9 / 203` vs `1.0` in SDR), letting `fx_*` shaders adapt without per-app scaling hacks.
2. **Night Light & Linear Colour Filters Must Not Clamp `[0, 1]` (`spec_comp_effects.md` invariant):**
   - When `color_filter` or `night_light_temperature` is active on an HDR output, **never** apply a `[0, 1]`-clamped 3D LUT in `output.frag` (which would crush every HDR highlight `> 1.0` down to SDR white!).
   - Verify/ensure in the `wlroots` fork that `color_transform` generated by `comp_fx` colour filters + night light folds into an **unclamped linear 3×3 / 4×4 matrix** (`wlr_color_transform_init_linear_matrix` or `wlr_kfx` pre-tonemap matrix step) applied **before** `X6` BT.2390 PQ tone mapping so `> 1.0` HDR highlights warm cleanly with night light intact.
3. **10-Bit Dither Amplitude (`X6`):**
   - In `output.frag`, quantize dither amplitude to `±0.5 / 1023.0` when the output render format is 10-bit (`XRGB2101010`), vs `±0.5 / 255.0` when 8-bit (`XRGB8888`), avoiding unnecessary noise injection on 10-bit panels.
4. **HDR Headroom Exposure to Wayland Clients:**
   - Ensure `output->image_description` reflects actual active `mastering_luminance.max` / `max_cll` so `wlr_color_manager_v1` reports accurate display reference & max luminances (`max_luminance`, `min_luminance`, `reference_luminance = sdr_white_nits`) to clients such as `mpv` (`vo=dmabuf-wayland` / `gpu-next`), Chromium/CEF (`kajo-web`), and HDR games.

---

## 5. Wayland Protocol Negotiation, `hdr = "auto"` Policy & Capture

### 5.1 Upgrading `init_color_management()` (`src/comp/comp_protocols.c:520`)

Expand `wlr_color_manager_v1_options.features`:
- `.parametric = true` (already enabled)
- `.set_primaries = true` (allows clients specifying custom gamut coordinates)
- `.set_tf_power = true` (allows gamma-power parametric TFs from Chromium/games)
- `.set_luminances = true` (allows HDR content to declare target min/max/reference luminance for accurate BT.2390 tone mapping)
- `.set_mastering_display_primaries = true` (already enabled)
- `.extended_target_volume = true` (allows wide-gamut / HDR highlights above reference white when `hdr` output is active)
- `.windows_scrgb = true` (critical for Wine/Proton/DXVK scRGB linear float buffers!)

Add **surface preferred image description updates** in `src/comp/`:
- Whenever the primary output switches between SDR ↔ HDR (or `hdr_peak_nits` / `sdr_white_nits` changes), update `wlr_color_manager_v1_set_surface_preferred_image_description()` for mapped surfaces on that output so video players (`mpv`, `kajo-player`) and games dynamically retarget output EOTF without needing restart.

### 5.2 Output HDR Policy Modes (`off` | `on` | `auto`)

Each output supports three policy modes:
- **`"off"` (Default for incompatible/unverified panels, safe baseline):** Output remains 8-bit SDR; HDR clients are tone-mapped cleanly into SDR `8-bit` by `X6` BT.2390 (no wasted 10-bit power draw, zero panel mode-switch flicker).
- **`"on"`:** Keeps the HDR-capable output continuously in 10-bit `BT2020 / ST2084_PQ` mode while enabled (note: panel power/VRR tradeoffs depend on display; ideal for external HDR monitors/TVs).
- **`"auto"` (Recommended for OLED laptops like Host A):**
  - Idles in 8-bit SDR on desktop/terminals/IDEs.
  - Scans visible surfaces on each frame commit (already cheap — `comp_window` tracks fullscreen/active views; check `wlr_surface_get_image_description_v1_data(surf)` for `tf_named == ST2084_PQ` or `EXT_LINEAR` or `max_luminance > reference_luminance`).
  - When **at least one visible mapped HDR surface** appears on an `"auto"`-configured HDR-capable output → commits HDR10 immediately on the next frame.
  - When the last HDR surface hides/closes/switches workspace → waits a **`2000 ms` hysteresis timer** (avoiding rapid KMS flicker while `Alt+Tab`bing or scrubbing fullscreen toggles) before stepping back down to 8-bit SDR.

### 5.3 Direct Scanout Safety (`wlr_scene_output_commit`)

In fullscreen HDR video/games, `wlroots` attempts direct KMS scanout if buffer format, modifier, and surface transform match the primary plane:
- Only allow direct scanout bypass when the surface's committed `image_description` matches the output's active `image_description` **AND** no compositor `color_filter` / `night_light` / `sdr_white_nits != 203` / `kfx` overlay is active; otherwise keep compositor scene composition (`requires two_pass / Vulkan`) so colour transforms are never skipped silently.

### 5.4 Screenshots & Screencopy (`comp_screenshot.c`)

- Capturing a raw `10-bit PQ` buffer directly into a standard 8-bit PNG (`~/Pictures/Screenshots`) produces washed-out grey/dark images!
- When taking a compositor screenshot or feeding SDR `wlr-screencopy-unstable-v1` viewers while an HDR10 output is active:
  - Read from an offscreen linear `RGBA16F` render pass (or apply inverse-PQ → BT.2390 tone-map → sRGB 8-bit conversion pass in `comp_screenshot.c`) so PNG screenshots and clipboard captures render with faithful sRGB contrast and saturation.

---

## 6. Configuration (`comp.toml`), IPC & `kajo-settings` Integration

### 6.1 `comp.toml` (`[[output]]` rules + `[fx]` globals)

- Already present in `[fx]` (`src/comp/core/comp_schema_keys.h:185-187`):
  - `sdr_white_nits` (`0` = `203 cd/m²`, or `80..1000`)
  - `tonemap` (`bool`, default `true`)
  - `tonemap_peak_nits` (`0` = panel EDID max / auto, or `100..10000`)
- Add to per-output `[[output]]` blocks (`CompOutputRule` + `schema_comp.c`):
  - `hdr = "off" | "on" | "auto"` (default `"off"`)
  - `hdr_peak_nits = 0` (`0` = use EDID `max_cll` / default `400`, or explicit override `100..10000`)

### 6.2 `$KAJO_COMP_SOCKET` JSON IPC

- **`get_outputs` reply fields added per output:**
  - `hdr_supported` (`bool`), `hdr_active` (`bool`), `hdr_policy` (`"off" | "on" | "auto"`), `hdr_reason` (`""` | `"no_edid_support"` | `"no_vulkan_color_transform"` | `"kms_rejected"`), `hdr_max_nits` (`double`), `hdr_sdr_white_nits` (`double`), `hdr_headroom` (`double`).
- **Action:** `set_output_hdr` (`{ "output": "eDP-1", "hdr": "auto" }` / `"on"` / `"off"`), validated through `commit_output_cascade()` and persisted only when triggered via `kajo-settings`.

### 6.3 `kajo-settings` Displays & Compositor Pages (`AGENTS.md §3.4`, `§8.1`)

- In `src/settings/pages/page_displays.c` (matching [`docs/design_system.md`](design_system.md) §4.3):
  - **HDR Policy Control:** Segmented group `[ OFF ] [ AUTO ] [ ON ]` with real live `hdr_supported` gating (`gtk_widget_set_sensitive(..., caps.supported)` and explicit status subtitle `"Supported — 617 cd/m² OLED HDR10"` vs `"Unsupported — Display lacks BT.2020/PQ"` or `"Unsupported — Requires Vulkan renderer"` — never silent).
  - **SDR White Level & Peak Override:** Slider/spin controls backed by `comp.toml` `sdr_white_nits` (`80..500 cd/m²`, default `203`) and optional `hdr_peak_nits` calibration (`0 = EDID Auto`), gated by explicit **SAVE & APPLY** + **REVERT**.

---

## 7. Phased Implementation & Verification Plan

### Phase 1 — DisplayID 2.0 EDID Fallback & Capabilities (`subprojects/wlroots`)
- Implement `kfx_edid_scan_did2_cta_hdr()` in `subprojects/wlroots/backend/drm/util.c` + `wlr_kfx_output_get_hdr_caps()` in `wlr/kfx/scene.h`.
- Fix `atomic.c:238` `min_display_mastering_luminance` scaling (`* 10000.0`, clamped `[0, 65535]`) and verify CTA-861-H `CV == 0` unset handling.
- **Gate:** Capture Host A's real 256-byte EDID (`/sys/class/drm/card0-eDP-1/edid`) as a unit-test fixture (alongside synthetic `CV == 0`, malformed, truncated, and bad-checksum EDIDs) and assert exact `BT2020`, `ST2084_PQ`, `max_cll ≈ 616.884`, `max_fall = 400.0`, `min_luma ≈ 0.000379` extraction cleanly under ASan/UBSan.

### Phase 2 — Atomic HDR10 Scanout, Supernova Fix, `auto` Hysteresis & Colour Filter Safety
- Fix `scene_entry_render()` vs `scene_output_combine_color_transforms()` `sdr_white_nits` coupling in `subprojects/wlroots/types/scene/wlr_scene.c` so `sdr_white_nits` scales SDR surfaces only and leaves HDR PQ luminance invariant.
- Extend `commit_output_cascade()` (`comp_output.c`) with 10-bit PQ commit + SDR fallback step-down, add `[[output]]` `hdr` & `hdr_peak_nits` schema keys, add `auto` HDR surface detection + `2000ms` hysteresis timer, and verify unclamped night-light / colour-filter matrix folding in `output.frag`.
- **Gate (Explicit Nits & Supernova Readback Assertions):** Headless Vulkan test (`WLR_BACKENDS=headless WLR_RENDERER=vulkan`) committing `BT2020 + ST2084_PQ` on a 10-bit virtual output (`XRGB2101010`), reading back rendered pixels and asserting:
  - SDR white `1.0` at `sdr_white_nits = 203` decodes to `PQ(203 / 10000) ≈ 0.58069` → code **`594 ± 1 / 1023`** (immediately fails on unscaled `1023/1023` supernova!).
  - HDR PQ `600 cd/m²` input at `sdr_white_nits = 100`, `203`, and `400` all read back **`PQ(600 / 10000) ≈ 0.7009` → `717 ± 1 / 1023`** (proving `sdr_white_nits` never multiplies HDR content).
  - BT.2390 highlight rolloff above `hdr_peak_nits` and linear night-light warmth preservation above `1.0`.

### Phase 3 — Wayland Color Manager Features & Screenshot Tone Mapping
- Enable full `wp-color-management-v1` features (`extended_target_volume`, `windows_scrgb`, `set_luminances`, `set_primaries`) in `comp_protocols.c`, preferred image description updates on mode change, and sRGB tone-mapped capture in `comp_screenshot.c`.
- **Gate:** Mock Wayland client test negotiating `ST2084_PQ` and `EXT_LINEAR` (`windows_scrgb`) surfaces + screencopy/screenshot readback assertions + `scripts/asan-test.sh`.

### Phase 4 — Settings UI, Gallery & Live Host A OLED Sign-off
- Wire IPC + `kajo-settings` Displays HDR controls (with `docs/ui-gallery.html` & `docs/design_system.md` updates).
- **Gate:** `scripts/ui-shot.sh` visual inspection of `kajo-settings --page displays` (unsupported vs active HDR states) + live Host A bare-metal check (`mpv --vo=dmabuf-wayland` / HDR YouTube in `kajo-web --ozone-platform=wayland`, checking DRM `HDR_OUTPUT_METADATA` blob via `drm_info`, `auto` 2s hysteresis, Night Light over HDR highlights, and SDR PNG screenshot fidelity).
