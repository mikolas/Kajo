# Spec: `kajo-comp` Optical Glass & Light-Scattering Materials (`docs/spec_comp_glass_styles.md`)

**Status:** Implemented & Verified (branch `feat/comp-fx-glass-styles`; 33/33 Meson suites including GS1–GS9 pure-core & headless Vulkan pixel tests).  
**Builds on:** [`docs/spec_comp_effects.md`](spec_comp_effects.md) (M1–M5, `wlr_kfx_*` X1–X6) and [`docs/design_system.md`](design_system.md) (`0px` Aero + Metro design system).  
**Fork impact:** **Zero.** All materials run on the existing `kajo-0.20.2-fx9` fork (`X1` custom fragment shaders with up to 2 texture inputs, `X2` below-node backdrop render, `X3` pooled targets + single-CB target chains, `X5` damage helpers, `X6` blue-noise output dither & linear FP16 blend image).

---

## 1. Goals and Non-Goals

### Goals
- **G1 (Light-transport materials, not just uniform blur):** Extend `kajo-comp`'s glass slot (`FX_SLOT_GLASS`) from a single uniform Dual-Kawase blur into a family of 7 selectable optical glass materials: `frosted`, `liquid`, `halation`, `fluted`, `acrylic`, `aero`, and `dichroic`.
- **G2 (Native `0px` Aero + Metro geometry):** Every material is designed specifically for Kajo's strict `0px` sharp rectangular geometry (chamfered/beveled edges, architectural flutes, micro-etched surfaces, multi-scale halation) rather than rounded pill blobs.
- **G3 (Linear FP16 & HDR-correct):** All refraction, Fresnel speculars, chromatic dispersion, thin-film interference, and halation scattering execute in linear light before `X6` output encoding (sRGB / PQ) and blue-noise dithering.
- **G4 (Zero regression on `frosted`):** `style = "frosted"` remains the default and preserves the exact current Dual-Kawase shader path, target count, and pixel output.
- **G5 (Zero idle cost):** All materials remain strictly damage-gated (`X5`) and `xray`-cacheable (`bg_serial`). No time-driven animations or pointer-tracking wakes are enabled by default (`< 0.05%` idle CPU budget).
- **G6 (Per-surface composition):** Global default in `comp.toml` `[effects.glass]`, with optional per-surface overrides in `rules.toml` (`[[rule]]` and `[[layer_rule]]`, e.g. `fluted` or `halation` on the panel, `liquid` on popovers/launcher).

### Non-Goals
- Rounded corners or pill-shaped SDF masks (excluded by the `0px` design system).
- Dynamic cursor-tracking specular lights in v1 (would wake frames on every pointer motion; could be an opt-in follow-up).
- Runtime user-supplied `.frag` shaders (still deferred per `spec_comp_effects.md` §13 D1).
- Changes to `subprojects/wlroots` (`wlr_kfx_*` ABI stays frozen at `kajo-0.20.2-fx9`).
- GLES2 or pixman fallbacks (glass remains Vulkan-only; other renderers stay unchanged).

---

## 2. Material Catalogue (All 7 Styles)

Every style executes in `glass.frag` (or a dedicated 2-input variant `glass_dual.frag` for styles that bind both `u_tex0` and `u_tex1`) and shares the common post-processing tail: linear `brightness`, Rec.709 `saturation`, linear `tint_lin` mix, and premultiplied `opacity`.

| Style (`comp.toml`) | Inputs | Extra Pooled Target | Visual Signature | Best Paired With |
| :--- | :---: | :---: | :--- | :--- |
| **`frosted`** *(default)* | 1 (`blurred`) | 0 | Classic uniform Dual-Kawase frosted blur. | Baseline / low-power |
| **`liquid`** | 2 (`sharp`, `blurred`) | 1 (`go->aux_target`) | Thick chamfered-bevel optical glass: Snell UV refraction along a rectangular edge bevel, RGB chromatic dispersion, Fresnel rim highlight, and internal absorption. Center smoothly blends into soft blur while the bevel bends crisp light. | Popovers, launcher, OSD, floating dialogs |
| **`halation`** | 2 (`tight`, `wide`) | 1 (`go->aux_target`) | Multi-scale Mie-like light scattering: blends a tight blur (1–2 passes) with a deep wide-pyramid glow (4–5 passes) weighted by backdrop luminance. Bright elements bleed a luminous halo while dark areas stay deep OLED charcoal (`#111111`). | Panel, notifications, dark-room OLED |
| **`fluted`** | 1 (`blurred` or `1×` light blur) | 0 | Architectural reeded glass: periodic cylindrical lenslets (`sin` normal profile) refract the backdrop into vertical (or horizontal) light ribbons with fine specular ridges and subtle chromatic edge split. | Panel, launcher, OSK |
| **`acrylic`** | 1 (`blurred`) | 0 | Etched / sandblasted micro-facet frost: surface-locked high-frequency micro-normal perturbation + subtle anisotropic specular grain glints. Breaks up smooth Gaussian banding into tactile frosted glass. | All surfaces, high-DPI OLED |
| **`aero`** | 1 (`blurred`) | 0 | Evolved Vista/7 Aero crystal: diagonal specular light sheen bands across the surface + crisp 45° chamfer highlight along the `0px` top/left edges, modulated by underlying backdrop luminance. | Panel, popovers, dialogs |
| **`dichroic`** | 1 (`blurred`) | 0 | Thin-film spectral interference: computes an optical path-difference phase from edge incidence angle + backdrop luminance to tint bevels and specular highlights with subtle iridescent pearl/prism bands in linear light. | Launcher, OSD, accent showcases |

---

## 3. Optical & Mathematical Models

All coordinates below are in **surface-local logical pixels** $\mathbf{p}_{\text{surf}} \in [0, W] \times [0, H]$ (computed from `uv` and `surface_size_logical`, with directional vectors pre-rotated for `WL_OUTPUT_TRANSFORM_*` in `comp_fx_glass_pack()` so rotated outputs stay physically upright).

### 3.1 Common Bevel Normal Field (`liquid`, `aero`, `dichroic`)
For a `0px`-corner rectangle of logical size $(W, H)$ and bevel width $b = \text{bevel\_px} \in [2, 64]$:

1. Signed distances to the four edges:
   $$d_L = x,\quad d_R = W - x,\quad d_T = y,\quad d_B = H - y$$
2. Distance to the nearest edge:
   $$d_{\text{edge}} = \min(\min(d_L, d_R), \min(d_T, d_B))$$
3. Smooth bevel profile $t = \text{clamp}(1 - d_{\text{edge}} / b, 0, 1)$, shaped by a cubic lens curve:
   $$h'(t) = 3t^2 - 2t^3 \quad (\text{or smooth hemispherical slope } \frac{t}{\sqrt{1 - 0.85 t^2}})$$
4. Outward 2D normal $\mathbf{n}_{xy}$ blends smoothly across corners using soft-min edge weights $w_i = \text{clamp}(1 - d_i / b, 0, 1)^2$:
   $$\mathbf{n}_{xy} = \text{normalize\_safe}\Big((w_R - w_L,\; w_B - w_T)\Big) \cdot h'(t)$$
   $$n_z = \sqrt{\max(0.05,\; 1 - \|\mathbf{n}_{xy}\|^2)}$$

This produces a clean 45° mitered bevel at all four `0px` corners with zero discontinuities and zero rounded-corner artifacts.

### 3.2 `liquid` (Refractive Bevel + Dispersion + Fresnel)
- **Inputs:** `u_tex0` = lightly blurred or `1×` sharp backdrop; `u_tex1` = full Kawase blurred backdrop.
- **Refraction displacement (in logical px):**
  $$\Delta \mathbf{p} = -\mathbf{n}_{xy} \cdot \text{refraction} \cdot b$$
  where `refraction` $\in [0.0, 2.0]$ (default `0.45`).
- **Chromatic dispersion (RGB split along $\mathbf{n}_{xy}$):**
  With `dispersion` $\in [0.0, 1.0]$ (default `0.25`):
  $$\Delta \mathbf{p}_R = \Delta \mathbf{p} \cdot (1 - 0.18 \cdot \text{dispersion}),\quad \Delta \mathbf{p}_G = \Delta \mathbf{p},\quad \Delta \mathbf{p}_B = \Delta \mathbf{p} \cdot (1 + 0.18 \cdot \text{dispersion})$$
  Each channel samples `u_tex0` (crisp refraction) and `u_tex1` (frosted interior) clamped to `valid_bounds`, blended by the bevel factor $k_{\text{rim}} = \text{smoothstep}(0.0, 0.85, t)$:
  $$C_c = \text{mix}\big(\text{sample}(u_{\text{tex1}}, \mathbf{p} + \Delta\mathbf{p}_c)_c,\; \text{sample}(u_{\text{tex0}}, \mathbf{p} + \Delta\mathbf{p}_c)_c,\; 0.75 \cdot k_{\text{rim}}\big)$$
- **Fresnel specular & internal absorption (linear FP16):**
  - Directional top-left key light $\mathbf{L} = \text{normalize}(-0.35, -0.80, 0.48)$ (pre-rotated for output transform).
  - Schlick Fresnel $F = (1 - n_z)^3$.
  - Rim specular: $S = \text{specular} \cdot \big(0.65 \cdot \max(\mathbf{n} \cdot \mathbf{L}, 0)^6 + 0.35 \cdot F\big) \cdot k_{\text{rim}}$.
  - Subtle internal dark rim on the bottom-right shadow bevel: $A = 1 - 0.18 \cdot \text{specular} \cdot \max(-\mathbf{n} \cdot \mathbf{L}, 0) \cdot k_{\text{rim}}$.
  - Result before tint: $\mathbf{C} = \mathbf{C} \cdot A + S \cdot (0.95, 0.98, 1.0)$.

### 3.3 `halation` (Multi-Scale Light Scattering)
- **Inputs:** `u_tex0` = tight blur (1–2 Kawase passes); `u_tex1` = wide blur (4–5 Kawase passes).
- **Scattering model:**
  - Sample $\mathbf{C}_{\text{tight}} = \text{sample}(u_{\text{tex0}}, \mathbf{p})$, $\mathbf{C}_{\text{wide}} = \text{sample}(u_{\text{tex1}}, \mathbf{p})$.
  - Luminance of wide scatter $Y_{\text{wide}} = \text{dot}(\mathbf{C}_{\text{wide}}, \text{Rec709})$.
  - Smooth threshold knee so dark regions stay pitch-dark while bright highlights bloom softly:
    $$g = \text{smoothstep}(0.08, 0.75, Y_{\text{wide}}) \cdot \text{scatter}$$
  - Output before tint:
    $$\mathbf{C} = \text{mix}(\mathbf{C}_{\text{tight}}, \mathbf{C}_{\text{wide}}, 0.35 \cdot \text{scatter}) + \mathbf{C}_{\text{wide}} \cdot (0.45 \cdot g)$$
  - Optional subtle inner-edge light trap: $+ 0.08 \cdot \text{specular} \cdot (1 - \text{smoothstep}(0, 2.0, d_{\text{edge}})) \cdot (0.5 + \mathbf{C}_{\text{wide}})$.

### 3.4 `fluted` (Architectural Reeded Glass)
- **Inputs:** `u_tex0` (1-input; best with `passes = 1..2` so ribs stay legible).
- **Cylindrical lenslet phase:**
  - Coordinate along flute axis $u = \text{dot}(\mathbf{p}_{\text{surf}}, \mathbf{a}_{\text{flute}})$ where $\mathbf{a}_{\text{flute}}$ is `(1, 0)` (vertical ribs) or `(0, 1)` (horizontal ribs), pre-rotated for output transform.
  - Normalized phase $\phi = \text{fract}(u / \text{flute\_px}) \cdot 2 - 1 \in [-1, 1]$ (`flute_px` $\in [4.0, 64.0]$, default `14.0`).
  - Cylindrical slope $n_u = \phi \cdot \sqrt{1 - 0.5 \phi^2}$, $n_z = \sqrt{\max(0.1, 1 - n_u^2)}$.
  - UV refraction along $\mathbf{a}_{\text{flute}}$:
    $$\Delta \mathbf{p} = -\mathbf{a}_{\text{flute}} \cdot n_u \cdot \text{refraction} \cdot \text{flute\_px} \cdot 0.65$$
  - RGB dispersion along $\mathbf{a}_{\text{flute}}$ (same 3-tap pattern as `liquid` when `dispersion > 0`).
  - Specular ridge along each cylinder crest + subtle valley darkening:
    $$S = \text{specular} \cdot \Big(0.28 \cdot \max(0, 1 - |\phi + 0.35| \cdot 2.4)^3 - 0.10 \cdot \max(0, 1 - |\phi - 0.75| \cdot 3.0)^2\Big)$$

### 3.5 `acrylic` (Etched Micro-Facet Frost)
- **Inputs:** `u_tex0` (blurred backdrop).
- **Surface-locked micro-facet hash:**
  - Quantized surface pixel coordinate $\mathbf{q} = \lfloor \mathbf{p}_{\text{surf}} \cdot \text{scale} \rfloor$ so the etched texture moves rigidly with the window/popover instead of swimming.
  - Fast 2D integer-free PCG/sinless hash `hash22(q)` $\rightarrow \boldsymbol{\xi} \in [-1, 1]^2$ (avoids `sin()` precision artifacts across GPUs).
  - Micro-normal UV jitter: $\Delta \mathbf{p} = \boldsymbol{\xi} \cdot \text{grain} \cdot 3.5$ (in logical px), `grain` $\in [0.0, 1.0]$ (default `0.35`).
  - Micro-specular sparkle (subtle, luminance-modulated so it doesn't add noise over pure black):
    $$\text{glint} = (\xi_x \cdot 0.5 + 0.5 - 0.5) \cdot 0.045 \cdot \text{grain} \cdot (0.35 + \text{clamp}(Y, 0, 1))$$
  - Plus a crisp `1px` top-edge specular hairline (`specular`).

### 3.6 `aero` (Beveled Crystal + Diagonal Sheen)
- **Inputs:** `u_tex0` (blurred backdrop).
- **Diagonal glass sheen:**
  - Normalized diagonal coordinate $s = (0.65 x + 0.35 y) / \max(W, H)$.
  - Two soft anisotropic specular reflection bands (inspired by Windows 7 Aero Glass):
    $$B_1 = \exp(-((s - 0.28) / 0.09)^2),\quad B_2 = 0.55 \cdot \exp(-((s - 0.52) / 0.045)^2)$$
  - Modulated by backdrop luminance (`0.35 + 0.65 * clamp(Y, 0, 1)`) so the sheen catches light naturally over bright content and stays understated over dark OLED backgrounds.
- **45° chamfered top/left rim** using §3.1 (`bevel_px`), adding a sharp `specular` highlight along top/left edges and a subtle shadow on bottom/right edges.

### 3.7 `dichroic` (Thin-Film Spectral Interference)
- **Inputs:** `u_tex0` (blurred backdrop).
- **Thin-film phase shift:**
  - Uses bevel incidence angle $\theta = \arccos(n_z)$ from §3.1 plus surface gradient $(x/W, y/H)$ and backdrop luminance $Y$:
    $$\psi = 6.2831853 \cdot \big(0.65 \cdot (1 - n_z) + 0.18 \cdot (x/W + 0.5 y/H) + 0.12 \cdot \text{clamp}(Y, 0, 1)\big)$$
  - Physically inspired cosine spectral palette (linear RGB, normalized to unit mean so it shifts hue without clipping):
    $$\mathbf{I}(\psi) = 0.5 + 0.5 \cos(\psi + (0.0,\; 2.094,\; 4.189))$$
  - Applied primarily on the bevel Fresnel rim ($F = (1 - n_z)^2$) and softly across highlights, scaled by `specular` and `dispersion`:
    $$\mathbf{C} = \mathbf{C} + \mathbf{I}(\psi) \cdot \text{specular} \cdot \text{dispersion} \cdot \big(0.55 \cdot k_{\text{rim}} \cdot (0.3 + F) + 0.06 \cdot \text{clamp}(Y, 0, 1)\big)$$

---

## 4. Pipeline, Target Pooling & `X5` Sample Reach

### 4.1 Dual-Input Target Pipeline (`liquid`, `halation`)
Currently [fx_glass.c](file:///home/mikolas/src/Kajo/src/comp/fx/fx_glass.c) owns a single `struct wlr_kfx_target *target` on `CompFxGlassOutput` and overwrites `levels[0]` in-place during the `blur_up` chain.

To support 2-input styles (`liquid`, `halation`) with **zero extra GPU submissions** and **zero cost when unused**:

1. Add `struct wlr_kfx_target *aux_target;` and `struct wlr_box aux_src_box;` to `CompFxGlassOutput` (released in `glass_output_destroy()` and released immediately if the surface switches back to a 1-input style).
2. **`liquid` capture:**
   - Capture the backdrop at `downscale = 1` (or `2` if `passes == 0` is not set; configurable/automatic: `downscale = 1` for surfaces $\le 1920\times 1080$ logical, `2` for larger) into `go->aux_target` (sharp reference).
   - Downsample from `go->aux_target` into `levels[1..N]` and upsample back into `go->target` (blurred interior), all inside the same `wlr_kfx_renderer_run_target_chain()` call.
   - Composite binds `inputs = { go->aux_target, go->target, NULL, NULL }`.
3. **`halation` capture:**
   - Capture the backdrop at `GLASS_DOWNSCALE = 2` into `go->target`.
   - Run the full pyramid down to `max(passes, 4)` levels.
   - Save the tight upsample (`level 1 → go->target`, 1–2 passes) AND a wide upsample (`level 4 → go->aux_target`, or sample `levels[3]` directly with bilinear filtering!) — **even cheaper:** keeping `levels[min(chain_steps, 3)]` (a tiny `1/16`-res target, ~40 KB!) as `go->aux_target` requires **zero extra blur passes** and gives a huge, silky Mie scattering halo!
   - Composite binds `inputs = { go->target, go->aux_target, NULL, NULL }`.
4. **1-input styles (`frosted`, `fluted`, `acrylic`, `aero`, `dichroic`):**
   - `go->aux_target` is `NULL` (released if previously held).
   - Exact same single target and single-CB pyramid as today.
   - For `fluted` when `passes == 1` and `offset == 0.0`, skip the blur chain completely and sample the crisp `downscale = 1` backdrop directly.

### 4.2 Sample Reach (`X5`) & Output Damage
Update `comp_fx_glass_sample_reach(const CompFxGlassConfig *cfg, CompFxGlassStyle style)` in `src/comp/core/comp_fx_core.c`:

- Base blur reach: $r_{\text{blur}} = \lceil 2^{\text{passes}} \cdot \text{offset} \rceil$ (unchanged).
- Additional optical displacement reach $r_{\text{opt}}$:
  - `frosted`, `aero`, `dichroic`: `0`
  - `liquid`: $\lceil \text{refraction} \cdot \text{bevel\_px} \cdot (1 + 0.18 \cdot \text{dispersion}) \rceil$
  - `fluted`: $\lceil \text{refraction} \cdot \text{flute\_px} \cdot 0.65 \cdot (1 + 0.18 \cdot \text{dispersion}) \rceil$
  - `acrylic`: $\lceil 4 \cdot \text{grain} \rceil$
  - `halation`: $\max(r_{\text{blur}}, \lceil 2^{\max(\text{passes}, 4)} \cdot \text{offset} \rceil)$
- Total reach: $r_{\text{total}} = \min(256,\; r_{\text{blur}} + r_{\text{opt}})$.

This guarantees that moving content just outside the surface edge still invalidates refractive styles cleanly via `wlr_kfx_scene_output_damage_intersects(so, &sample_box)` with no stale border artifacts.

### 4.3 Shader Organization & UBO Layout (`std140`, $\le 256$ bytes)
Two compiled SPIR-V shaders (keeping `frosted` untouched and fast, or sharing one clean layout):
- `glass.frag` (`n_inputs = 1`): handles `frosted` (fast path `if (u.style == 0u) ...`), `fluted`, `acrylic`, `aero`, `dichroic`.
- `glass_dual.frag` (`n_inputs = 2`): handles `liquid` and `halation` (both share the exact same `CompFxGlassParams` UBO, plus `aux_crop_rect`, `aux_valid_bounds`, `aux_inv_tex_size`).

Expanded `CompFxGlassParams` (`std140`, **192 bytes** $\le 256$ byte limit):

```c
typedef struct {
    CompFxCommonParams common;            /*   0..32:  dst_origin, dst_size, opacity, scale, hdr_headroom, flags */
    float              crop_rect[4];      /*  32..48:  (x, y, w, h) in u_tex0 px */
    float              valid_bounds[4];   /*  48..64:  clamp (min_x, min_y, max_x, max_y) in u_tex0 px */
    float              tint_lin[4];       /*  64..80:  linear RGB + tint_alpha in .a */
    float              inv_tex_size[2];   /*  80..88:  1 / u_tex0 size */
    float              saturation;        /*  88..92 */
    float              brightness;        /*  92..96 */
    /* --- M6 Optical Glass extension (96..192) --- */
    float              aux_crop_rect[4];  /*  96..112: (x, y, w, h) in u_tex1 px (dual-input styles) */
    float              aux_valid_bounds[4];/* 112..128: clamp bounds in u_tex1 px */
    float              aux_inv_tex_size[2];/* 128..136: 1 / u_tex1 size */
    float              surface_size[2];   /* 136..144: full surface logical size (W, H) */
    float              light_dir[2];      /* 144..152: pre-rotated 2D key light / flute axis */
    float              surface_offset[2]; /* 152..160: clipped pass_dst - full_glass_buf in logical px */
    uint32_t           style;             /* 160..164: CompFxGlassStyle enum (0..6) */
    float              refraction;        /* 164..168: 0.0..2.0 */
    float              dispersion;        /* 168..172: 0.0..1.0 */
    float              bevel_px;          /* 172..176: 2.0..64.0 logical px */
    float              specular;          /* 176..180: 0.0..2.0 */
    float              grain;             /* 180..184: 0.0..1.0 (acrylic) or scatter (halation) */
    float              flute_px;          /* 184..188: 4.0..64.0 logical px */
    float              _pad0;             /* 188..192: 16-byte std140 tail alignment */
} CompFxGlassParams;
```

*Note on `surface_offset`*: When a glass surface is partially off-screen (e.g. during slide-in or autohide), `pass_dst` is clipped to the output while `full_glass_buf` is the unclipped surface rectangle. Passing `surface_offset` + `surface_size` ensures bevels, flutes, and etched grain stay anchored to the surface's true edges instead of jumping when clipped!

---

## 5. Configuration Schema (`comp.toml` & `rules.toml`)

### 5.1 `comp.toml` (`[effects.glass]`)
All new keys are optional with backward-compatible defaults (`style = "frosted"`):

```toml
[effects.glass]
enabled = true
style = "frosted"       # "frosted" | "liquid" | "halation" | "fluted" | "acrylic" | "aero" | "dichroic"
passes = 3              # 1..6 (0 allowed for crisp refraction in liquid/fluted)
offset = 2.0            # 0.0..16.0
saturation = 1.15       # 0.0..4.0
brightness = 0.90       # 0.0..4.0
tint = "#141414"
tint_alpha = 0.35       # 0.0..1.0
max_recompute_hz = 0    # 0..1000
# Optical material parameters (used by styles that read them):
refraction = 0.45       # 0.0..2.0  (liquid, fluted)
dispersion = 0.25       # 0.0..1.0  (liquid, fluted, dichroic)
bevel_px = 14.0         # 2.0..64.0 (liquid, aero, dichroic)
specular = 0.55         # 0.0..2.0  (liquid, halation, fluted, aero, dichroic)
grain = 0.35            # 0.0..1.0  (acrylic micro-facet roughness / halation scatter weight)
flute_px = 14.0         # 4.0..64.0 (fluted rib pitch in logical px)
flute_axis = "vertical" # "vertical" | "horizontal" (fluted)
```

### 5.2 `rules.toml` (`[[rule]]` and `[[layer_rule]]`)
Allow per-surface style overrides so the user can mix materials (for example, `halation` on the docked panel and `liquid` on floating popovers and the launcher):

```toml
[[layer_rule]]
namespace = ['^kajo$']
blur = true
xray = true
glass_style = "halation"

[[layer_rule]]
namespace = ['^kajo-launcher$', '^kajo-osd$']
blur = true
glass_style = "liquid"
```

- `has_glass_style` + `CompFxGlassStyle glass_style` added to `CompWindowRuleResult`, `CompLayerRuleResult`, and `CompFxSurfaceState`.
- When unset, the surface inherits `sys->cfg.glass.style`.
- Unknown style string in `comp.toml` or `rules.toml` emits a `CompDiagList` diagnostic and keeps the default (`frosted`).

---

## 6. `kajo-settings` UI (`src/settings/pages/page_niri.c`)

In the **Compositor & Effects → Glass & Surface Effects** card:
1. **Glass Style dropdown** (`create_string_dropdown`): `Frosted (Kawase)`, `Liquid Glass`, `Halation (Scatter)`, `Fluted (Reeded)`, `Acrylic (Etched)`, `Aero Sheen`, `Dichroic (Iridescent)`.
2. **Contextual optical sliders** (shown/sensitized based on the selected style so the UI stays uncluttered — no dead sliders for irrelevant styles):
   - `liquid`: Refraction, Chromatic Dispersion, Bevel Width (`px`), Specular Highlight.
   - `halation`: Scatter Intensity (`grain`), Specular Edge.
   - `fluted`: Rib Pitch (`flute_px`), Orientation (`Vertical` / `Horizontal` segmented group), Refraction, Dispersion, Specular.
   - `acrylic`: Etch Roughness (`grain`), Specular Edge.
   - `aero`: Bevel Width (`px`), Sheen Intensity (`specular`).
   - `dichroic`: Bevel Width (`px`), Iridescence (`dispersion`), Specular.
   - `frosted`: Existing Blur Strength, Brightness, Saturation, Tint Alpha only.
3. Persists to `~/.config/kajo/comp.toml` `[effects.glass]` via `kajo_config_comp.c` and triggers `ReloadConfig` over `$KAJO_COMP_SOCKET` on **SAVE & APPLY** (with live **REVERT** support).
4. Add a specimen to `docs/ui-gallery.html` and document the new glass styles in `docs/design_system.md`.

---

## 7. Verification & Acceptance Criteria

### 7.1 Pure-Core Unit Tests (`test-comp-fx-core`, `test-comp-config`)
- **GS1 (Config & Rules):** Round-trip parse of all 7 `style` strings, `flute_axis`, range clamping on `refraction`/`dispersion`/`bevel_px`/`specular`/`grain`/`flute_px`, diagnostic on unknown `style`, and `glass_style` in both `[[rule]]` and `[[layer_rule]]`.
- **GS2 (UBO Packing & Alignment):** `_Static_assert(sizeof(CompFxGlassParams) == 192)` and `<= 256`; verify `std140` offsets, pre-rotated `light_dir` across all 8 `WL_OUTPUT_TRANSFORM_*` values, and `surface_offset` clipping math.
- **GS3 (Sample Reach `X5`):** Verify `comp_fx_glass_sample_reach()` expands for `liquid` (refraction + dispersion), `fluted`, `acrylic`, and `halation`, and matches the legacy formula exactly for `frosted`.

### 7.2 Headless Vulkan Pixel Tests (`comp-fx-shadow` / `comp-fx-scene`)
- **GS4 (`frosted` parity):** Existing `comp-fx-shadow` assertions pass unchanged when `style = "frosted"`.
- **GS5 (`liquid` refraction + dispersion + Fresnel):** Render a high-contrast vertical black/white edge behind a `liquid` glass surface; assert:
  1. Pixels inside the bevel zone exhibit non-zero R-vs-B channel separation (chromatic dispersion) and shifted edge position (Snell refraction) relative to `frosted`.
  2. Top-left bevel edge is brighter than interior (Fresnel specular).
- **GS6 (`halation` multi-scale scatter):** Render a bright white patch on a black background behind a `halation` surface; assert wide-radius luminance halo beyond the tight-blur radius while far black regions remain near-zero.
- **GS7 (`fluted` periodicity):** Render a horizontal gradient behind a vertical `fluted` surface; assert periodic luminance modulation at period `flute_px`.
- **GS8 (`acrylic`, `aero`, `dichroic`):** Assert non-zero spatial variance on `acrylic` over a flat colour, diagonal luminance lift on `aero`, and non-neutral chroma shift on `dichroic` bevel pixels.
- **GS9 (Target lifecycle & leak check):** Switching styles at runtime (`frosted` $\leftrightarrow$ `liquid` $\leftrightarrow$ `halation` $\leftrightarrow$ `frosted`) releases `aux_target` cleanly with zero leaked `wlr_kfx_target`s and passes `scripts/asan-test.sh` + `scripts/audit.sh comp` + `-Dwlroots=system` stub build.

### 7.3 Visual & Live-Session Checks
- Render `kajo-settings --page niri` via `scripts/ui-shot.sh` and inspect layout/alignment.
- Live visual review on Host A OLED (`2880×1800` @ `1.75×` scale) across all 7 styles on panel (`xray`), launcher, and popovers, checking legibility of `Fira Code` text and `0px` borders.

---

## 8. Implementation Phases

1. **Phase 1 — Core schema, math & UBO (`src/comp/core/`):**
   - `CompFxGlassStyle` enum, `CompFxGlassConfig` fields, `comp_fx_glass_sample_reach()`, `CompFxGlassParams` (192 B), `comp_config.c`, `comp_rules.c`, `data/defaults/comp.toml`, and `test_comp_config.c` + `test_comp_fx_core.c`.
2. **Phase 2 — Shaders & dual-target runtime (`src/comp/fx/`):**
   - `glass.frag` (5 single-input styles: `frosted`, `fluted`, `acrylic`, `aero`, `dichroic`) + `glass_dual.frag` (2 dual-input styles: `liquid`, `halation`), `fx_glass.c` `aux_target` management & per-surface `glass_style` resolution, `fx_stub.c` parity.
3. **Phase 3 — Vulkan pixel tests (`tests/comp/`):**
   - Extend `comp_fx_shadow_test.sh` / `test_comp_fx_scene.c` with GS4–GS9.
4. **Phase 4 — `kajo-settings` UI & docs (`src/settings/`, `src/config/kajo_config_comp.c`, `docs/`):**
   - Add Glass Style dropdown + contextual optical sliders in `page_niri.c`, update `kajo_config_comp.c`, visually verify with `scripts/ui-shot.sh`, and update `docs/design_system.md` / `docs/ui-gallery.html`.
