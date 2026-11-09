# Specification: `kajo-mod` — Retro Amiga, Tracker & Chiptune Music Player (`docs/spec_kajo_mod.md`)

**Status:** Active Specification — tracking branch `feat/kajo-mod`  
**Target Binary:** `kajo-mod` (`org.kajo.Mod`, `src/mod/`)  
**Dependencies:** `libuade` (`uade >= 3.0`), `libsidplayfp` (`>= 3.0`, with `libresidfp` & `libstilview`), `libgme`, `libopenmpt`, `libxmp`, `libarchive`, `libpulse-simple` / `libpulse`, `gtk4`, `ShellConfig` (`src/config.c`)

---

## 1. Objectives & Motivation

Generic media players (`mpv`, VLC, GStreamer) treat module and chiptune files as flat audio streams, discarding the structural and hardware controls that make dedicated players like **XMPlay** and **DeaDBeeF** essential for retro computer music:

1. **Amiga Primary Playback (`libuade`):** Over 200 Amiga game and demoscene formats (`TFMX`, Jochen Hippel, David Whittaker, Future Composer, SidMon, SoundMon, Delta Music, Richard Joseph, AHX/HVL, The Player `P61`, `cust.*`, plus Amiga ProTracker/NoiseTracker/SoundTracker `.mod`, `MED`/`OctaMED`, and `Oktalyzer`) are played through real 68000 Eagleplayer + Paula emulation via `libuade`.
2. **Reference-Grade Commodore 64 SID Emulation (`libsidplayfp` + `reSIDfp`):** Cycle-exact 6510 + CIA/VIC-II + transistor-level **`reSIDfp`** analog filter modeling for `MOS 6581` (non-linear warm filter curve) and `MOS 8580` (with optional DigiBoost), supporting `PSID`/`RSID` (`.sid`), `.mus`, 2SID/3SID stereo/tri-chip tunes, per-voice muting, and HVSC `Songlengths.md5` / `STIL` commentary.
3. **8/16-Bit Console Chiptunes (`libgme`) & Non-Amiga PC Trackers (`libopenmpt` + `libxmp`):** Dedicated support for `SPC`, `NSF`/`NSFE`, `VGM`/`VGZ`, `AY`, `SAP`, `GBS`, `HES`, `KSS`, followed by `libopenmpt` and `libxmp` as fallbacks for non-Amiga/PC trackers (`.xm`, `.s3m`, `.it`, `.mptm`, multichannel PC `.mod`).
4. **First-Class Multi-Tune / Subsong Navigation:** A single Amiga custom module, C64 `.sid`, or `.nsf`/`.sap` file often contains 5–40 subsongs (title theme, level themes, boss music, jingles, game over). Users need dedicated subsong stepping (`[ ◀ SUB ]  SUB 03 / 18  [ SUB ▶ ]`) and per-subsong loop/end handling.
5. **Authentic Hardware & Resampling DSP Controls:**
   - **Stereo Separation (`0%..100%`):** Hardware Amiga Paula hard-pans channels `1,4` 100% Left and `2,3` 100% Right, which is fatiguing on headphones without a live crossfeed/separation slider (`0%` Mono .. `50%` Headphone default .. `100%` Hard Amiga L/R).
   - **Chip & Filter Controls:** Live switching between **Amiga 500 BLEP**, **Amiga 1200 BLEP**, **Nearest**, **Linear**, and **Sinc**; toggling the **Amiga 3.3 kHz LED Low-Pass Filter** (`AUTO` / `ON` / `OFF`); and switching C64 SID model (**`AUTO`** / **`MOS 6581`** / **`MOS 8580`**).
6. **Live Scope/Spectrum, Pattern Roll, Channel Mute/Solo & Sample/STIL Inspector:**
   - Dual-trace stereo oscilloscope + 16-band FFT spectrum analyzer + per-channel VU meters.
   - XMPlay-style **Sample / Instrument / Song Message / C64 STIL** readout in `Fira Code` monospace—where tracker composers and HVSC curators stored release notes, greetings, and ASCII art.
   - Real-time scrolling tracker pattern view (`Order / Pattern / Row / Speed / BPM`) for tracker fallback formats and per-voice **Mute / Solo** toggles (`libsidplayfp`, `libgme`, `libopenmpt`, `libxmp`).
7. **Amiga Prefix Naming & Multi-File `.lha` / `.zip` Archives:**
   - Amiga files frequently use **prefix extensions** (`mod.nebula`, `mdat.turrican2` + `smpl.turrican2`, `ahx.crystal`, `hip.wings`, `dw.platou`, `fc14.intro`).
   - Two-file formats (`TFMX` `mdat.*` + `smpl.*`, Richard Joseph `rjp.*` + `smp.*`, Jason Page `jpn.*` + `smp.*`) require companion sample files in the same directory, including when loaded directly from `.lha` / `.lzh` / `.zip` archives.

---

## 2. Architecture & Module Layout

All source files live in `src/mod/` and adhere to the `< 1500` lines per file limit (`AGENTS.md` §5.1):

| File | Responsibility |
| :--- | :--- |
| `src/mod/main.c` | `GtkApplication` (`org.kajo.Mod`) entry point, `kajo_app_init_theme()`, CLI file/directory/archive argument handling, single-instance URI forwarding (`G_APPLICATION_HANDLES_OPEN`). |
| `src/mod/mod_decoder.h` / `mod_decoder.c` | Unified `KajoModDecoder` abstraction routing in strict priority order: **1. `libuade`** $\rightarrow$ **2. `libsidplayfp` (`reSIDfp`)** $\rightarrow$ **3. `libgme`** $\rightarrow$ **4. `libopenmpt` / `libxmp`**. |
| `src/mod/mod_sid_bridge.h` / `mod_sid_bridge.cpp` | Thin `extern "C"` bridge (~200 LOC) wrapping C++ `libsidplayfp`, `ReSIDfpBuilder` (6581/8580 transistor filter), `SidTune`, `SidDatabase` (`Songlengths.md5`), and `libstilview`. |
| `src/mod/mod_archive.h` / `mod_archive.c` | Amiga prefix/suffix format classifier and `libarchive`-backed `.lha`/`.zip`/`.7z` bundle unpacker for single-file and multi-file (`mdat.*` + `smpl.*`) songs. |
| `src/mod/mod_audio.h` / `mod_audio.c` | Background audio render thread (`48000 Hz` stereo float32/int16 via `libpulse-simple`), command queue (seek, subsong, mute, DSP parameters), and lock-free UI telemetry/scope ringbuffer. |
| `src/mod/mod_window.h` / `mod_window.c` | Main Aero + Metro deck window (`KajoModWindow`), LCD telemetry readout, transport + subsong bar, Paula/SID DSP strip, and playlist/queue management. |
| `src/mod/mod_vis.h` / `mod_vis.c` | Custom `GtkDrawingArea` Cairo renderers for the **Stereo Oscilloscope + 16-Band FFT + Channel VU** deck and **Live Pattern Roll** (with channel Mute/Solo header bar). |
| `tests/test_mod_decoder.c` | Unit tests covering Amiga prefix/suffix detection, `PSID`/`RSID` vs Amiga `sid.*` disambiguation, multi-file companion pairing (`mdat.` $\leftrightarrow$ `smpl.`), `.lha`/`.zip` archive bundle extraction, and decoding across `libuade`, `libsidplayfp`, `libgme`, and `libopenmpt`. |

---

## 3. Five-Backend Decoder Router (`src/mod/mod_decoder.c`)

### 3.1 Decoder Backend Selection Priority

When opening a local file path, `kajo_mod_decoder_open()` routes in the following strict priority order:

| Priority | Backend | Formats Handled | Routing & Handshake Rule |
| :---: | :--- | :--- | :--- |
| **1 (Primary)** | **`libuade`** | **All Amiga modules & custom 68k formats:** Amiga `.mod` (`PTK-Prowiz` + 100+ packed ProTracker formats like `P61`, `P60`, `PP20`, `NP3`, `PRU2`, `UNIC`), `MED` / `OctaMED` (`MMD0–2`), `Oktalyzer`, `DIGI-Booster`, `AHX`/`THX`, `TFMX` (`mdat.*`+`smpl.*`), `Jochen Hippel`, `David Whittaker`, `Future Composer`, `SidMon 1/2` (`sid1.*`/`sid2.*`), `SoundMon`, `CustomMade` (`cust.*`), `Richard Joseph`, `Sonic Arranger`, `.rmc`, and 170+ Eagleplayer formats in `/usr/share/uade/eagleplayer.conf` | Skip only if the file has a C64 `"PSID"`/`"RSID"` 4-byte magic header (since `eagleplayer.conf` also lists `sid` for Amiga SIDMon 1.0) or an unambiguous console magic (`SNES-SPC700`, `NESM`, `Vgm `). Call `uade_is_our_file(path, state)` $\rightarrow$ `uade_play(path, -1, state)` and perform one probe `uade_read()` to verify the 68000 Eagleplayer accepted the module. |
| **2 (C64 SID)** | **`libsidplayfp` (`reSIDfp`)** | **Commodore 64 SID tunes:** `PSID` v1–v4, `RSID` (`.sid`), and Compute's Gazette `.mus` / `.str` (1SID, 2SID stereo, 3SID) | Verified via `"PSID"` / `"RSID"` header magic or `SidTune::getStatus()`. Emulated via `ReSIDfpBuilder` at `48000 Hz` stereo (`RESAMPLE_INTERPOLATE`). |
| **3 (Chiptune)** | **`libgme`** | **8/16-bit console & home computer chiptunes:** `.spc` (SNES), `.nsf`/`.nsfe` (NES), `.vgm`/`.vgz`/`.gym` (Mega Drive / SMS / Arcade FM), `.ay` (ZX Spectrum / CPC), `.sap` (Atari XL/XE POKEY), `.gbs` (Game Boy), `.hes` (PC Engine), `.kss` (MSX) | Verified via `gme_identify_file()`. |
| **4 (Fallback 1)** | **`libopenmpt`** | **Non-Amiga / PC tracker formats:** `.xm`, `.s3m`, `.it`, `.mptm`, `.stm`, `.mtm`, `.669`, `.ult`, `.far`, `.mdl`, `.ams`, `.dbm`, `.psm`, `.umx`, `.j2b`, `.mo3`, plus 8–32 channel PC `.mod` files rejected by UADE's `PTK-Prowiz` | Used only when `libuade`, `libsidplayfp`, and `libgme` do not claim the file and `openmpt_probe_file_header()` succeeds. |
| **5 (Fallback 2)** | **`libxmp`** | **Extended tracker fallback:** `OctaMED MMD3` / Soundstudio or Atari/Acorn tracker variants rejected by both `libuade` and `libopenmpt` | Used only when `xmp_test_module()` succeeds after all higher-priority engines decline. |

> [!NOTE]
> **Sample & Instrument Readout While `libuade` Is Playing:**
> Because `libuade` executes raw 68000 Eagleplayer code in `uadecore`, it does not expose sample tables through `uade_get_song_info()` directly. On track load, when `libuade` is the active backend, `kajo_mod_decoder_open()` also calls `uade_song_info(buf, sizeof(buf), path, UADE_MODULE_INFO)` and (for tracker files like `.mod` / `MED`) performs a one-shot metadata-only read of the sample/instrument names so the **`📋 SAMPLES & INFO`** tab is fully populated while audio renders through `libuade`'s 68000 + Paula emulator.

> [!IMPORTANT]
> **Preventing Companion Sample Files from Appearing as Playable Tracks:**
> Two-file Amiga formats store sample data in companion files (`smpl.*`, `smp.*`, `ins.*`). `kajo_mod_is_companion_sample_file(filename)` identifies sample-only companion files so directory scans and archive imports add only the playable song header (`mdat.*`, `rjp.*`, `jpn.*`, `sng.*`) to the playlist.

---

### 3.2 Unified Decoder Snapshot & Control Contract (`mod_decoder.h`)

```c
typedef enum {
    KAJO_MOD_BACKEND_NONE = 0,
    KAJO_MOD_BACKEND_UADE,        /* Priority 1: 68000 Eagleplayer + Paula emulator */
    KAJO_MOD_BACKEND_SIDPLAYFP,   /* Priority 2: C64 SID (libsidplayfp + reSIDfp 6581/8580) */
    KAJO_MOD_BACKEND_GME,         /* Priority 3: Console chiptunes (SPC, NSF, VGM, AY, SAP) */
    KAJO_MOD_BACKEND_OPENMPT,     /* Priority 4: Non-Amiga / PC multichannel trackers */
    KAJO_MOD_BACKEND_XMP,         /* Priority 5: Extended tracker fallback */
} KajoModBackendType;

typedef enum {
    KAJO_MOD_SID_AUTO = 0,        /* Honor PSID/RSID header chip model (6581 vs 8580) */
    KAJO_MOD_SID_6581,            /* Force MOS 6581 (non-linear analog filter curve) */
    KAJO_MOD_SID_8580,            /* Force MOS 8580 (clean C64C filter + DigiBoost) */
} KajoModSidModel;

typedef enum {
    KAJO_MOD_INTERP_A500 = 0,     /* Amiga 500 Paula BLEP + fixed filter */
    KAJO_MOD_INTERP_A1200,        /* Amiga 1200 Paula BLEP */
    KAJO_MOD_INTERP_NEAREST,      /* Zero-order hold (no interpolation) */
    KAJO_MOD_INTERP_LINEAR,       /* Linear interpolation */
    KAJO_MOD_INTERP_SINC,         /* Cubic spline / 8-tap windowed sinc */
} KajoModInterpMode;

typedef enum {
    KAJO_MOD_LED_AUTO = 0,        /* Honor module E00/E01 / eagleplayer LED state */
    KAJO_MOD_LED_ON,              /* Force 3.3 kHz Amiga LED low-pass filter ON */
    KAJO_MOD_LED_OFF,             /* Force LED filter OFF */
} KajoModLedMode;

typedef enum {
    KAJO_MOD_LOOP_ADVANCE = 0,    /* Play once (or fade on loop) -> next subsong/track */
    KAJO_MOD_LOOP_SUBSONG,        /* Loop current subsong indefinitely */
    KAJO_MOD_LOOP_FADEOUT,        /* Loop 1x then 3s fadeout -> next subsong/track */
} KajoModLoopMode;

#define KAJO_MOD_MAX_CHANNELS 64

typedef struct {
    char     title[256];
    char     artist[256];
    char     copyright[256];      /* C64 SID / GME release year & publisher */
    char     format_name[128];    /* e.g. "ProTracker MOD", "C64 PSID v2 (6581 PAL)", "TFMX-Pro" */
    char     tracker_name[128];   /* e.g. "Eagleplayer: PTK-Prowiz", "reSIDfp", "FastTracker II" */
    KajoModBackendType backend;

    int      subsong_min;
    int      subsong_max;
    int      subsong_cur;
    int      subsong_count;       /* (subsong_max - subsong_min + 1) */

    int      num_channels;
    int      num_orders;
    int      num_patterns;
    int      num_instruments;
    int      num_samples;
    double   duration_sec;        /* 0.0 if unknown (custom 68k/SID loop without MD5 DB) */
    gboolean has_pattern_view;    /* TRUE for OPENMPT and XMP */
    gboolean can_mute_channels;   /* TRUE for SIDPLAYFP, GME, OPENMPT, and XMP */
    gboolean can_change_tempo;    /* TRUE for GME, OPENMPT, and XMP */
} KajoModTrackMeta;

typedef struct {
    int      subsong;
    int      order;
    int      pattern;
    int      row;
    int      num_rows;
    int      speed;
    int      bpm;
    double   position_sec;
    gboolean led_filter_active;
    float    channel_vu[KAJO_MOD_MAX_CHANNELS];
    gboolean channel_muted[KAJO_MOD_MAX_CHANNELS];
} KajoModFrameState;
```

### 3.3 Backend-Specific Parameter Mapping

| Parameter | `libuade` (Prio 1) | `libsidplayfp` (Prio 2) | `libgme` (Prio 3) | `libopenmpt` (Prio 4) | `libxmp` (Prio 5) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Stereo Separation (`sep_pct`: `0..100`)** | `uade_effect_enable(st, UADE_EFFECT_PAN)` + `uade_effect_pan_set_amount(st, 1.0f - (sep_pct / 100.0f))` | Stereo crossfeed on 2SID/3SID output buffer (`sep_pct / 100.0f`) | `gme_set_stereo_depth(emu, sep_pct / 100.0)` | `OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT` = `sep_pct * 2` | `xmp_set_player(ctx, XMP_PLAYER_MIX, sep_pct)` |
| **Interpolation / Chip Filter** | `uade_config_set_option`: `UC_FILTER_TYPE` (`"a500"`/`"a1200"`), `UC_RESAMPLER` (`"default"`/`"none"`/`"sinc"`) | `SidConfig::samplingMethod`: `RESAMPLE_INTERPOLATE` vs `INTERPOLATE`; `ReSIDfpBuilder::filter(true/false)` | `gme_enable_accuracy(emu, mode != KAJO_MOD_INTERP_NEAREST)` | `A500`/`A1200`: `emulate_amiga_type = "a500"/"a1200"`<br>`NEAREST`/`LINEAR`/`SINC`: `1`/`2`/`8` | `A500`: `XMP_FLAGS_A500`<br>`NEAREST`/`LINEAR`/`SINC`: `NEAREST`/`LINEAR`/`SPLINE` |
| **LED Filter / SID Model** | `uade_set_filter_state(st, led == KAJO_MOD_LED_ON ? 1 : 0)` | `SidConfig::defaultSidModel` (`MOS6581` / `MOS8580`) + `forceSidModel` (`SID_AUTO` / `SID_6581` / `SID_8580`) + `digiBoost` | Equalizer treble cut (`gme_set_equalizer`) when `ON` | `OFF`: `"unfiltered"` resampler type; `AUTO`/`ON`: `"a500"`/`"a1200"` | `xmp_set_player(ctx, XMP_PLAYER_DSP, led ? XMP_DSP_LOWPASS : 0)` |
| **Subsong Selection** | `uade_seek(UADE_SEEK_SUBSONG_RELATIVE, 0.0, subsong, st)` | `SidTune::selectSong(subsong)` + `sidplayfp::load(&tune)` | `gme_start_track(emu, idx)` | `openmpt_module_select_subsong(mod, idx)` | `xmp_set_position(ctx, seq_data[idx].entry_point)` |
| **Channel / Voice Mute** | N/A (`can_mute_channels = FALSE`) | `sidplayfp::mute(chip, voice, mute)` (3 voices per SID chip) | `gme_mute_voice(emu, ch, mute)` | `interactive.set_channel_mute_status(ext, ch, mute)` | `xmp_channel_mute(ctx, ch, mute)` |

> [!WARNING]
> **`libuade` Thread & Process Discipline (`uade.h:15-22`):**
> 1. Call `signal(SIGPIPE, SIG_IGN)` at startup in `main.c` so an unexpected `uadecore` termination never kills `kajo-mod`.
> 2. Never access a `struct uade_state *` from more than one thread simultaneously; all `uade_*` calls after load execute exclusively on the audio worker thread via the command queue.
> 3. Always call `uade_cleanup_state(state)` on track unload or window destruction so the child `uadecore` process is reaped cleanly without zombies.

---

## 4. Archive Unpacking & Multi-File Amiga Support (`src/mod/mod_archive.c`)

Amiga music collections (Modland, ExoticA, HVSC, AMP) distribute modules inside `.lha` / `.lzh` or `.zip` archives, where a single song may consist of multiple files (`mdat.turrican2` + `smpl.turrican2`).

1. **Archive Bundle Extraction:**
   - When the user opens a `.lha`, `.lzh`, `.lzx`, `.zip`, `.7z`, or `.tar.*` file in `kajo-mod`:
   - `kajo_mod_archive_extract_bundle()` extracts regular files (enforcing a 64 MB per-archive cap and path traversal sanitization identical to `kajo-files`) into `$XDG_RUNTIME_DIR/kajo-mod/bundle-<pid>-<seq>/`.
   - It scans the extracted directory for playable module files, filtering out `README` / `.txt` / `.diz` / `.nfo` text files (while capturing any `.txt`/`.diz`/`.nfo` content to display in the **Message** inspector tab!) and filtering out companion sample files (`smpl.*`, `smp.*`, `ins.*`).
   - Because all files from the archive sit together in the temporary bundle directory, when `libuade` opens `mdat.turrican2`, `uadecore`'s AmigaOS loader automatically finds `smpl.turrican2` in the same directory.
2. **Lifecycle Cleanup:**
   - Extracted bundle directories are tracked and removed when the playlist is cleared or when `kajo-mod` exits.

---

## 5. Audio Engine & Map-Gated Telemetry (`src/mod/mod_audio.c`)

1. **Audio Render Thread:**
   - Uses `pa_simple_new()` (`48000 Hz`, `PA_SAMPLE_FLOAT32NE`, `2` channels, `tlength = 2048` bytes for snappy mute/seek/DSP response).
   - Renders `512` stereo frames per iteration (~10.67 ms per tick).
   - Drains an atomic/mutex-guarded command queue (`CMD_SEEK`, `CMD_SET_SUBSONG`, `CMD_SET_MUTE`, `CMD_SET_DSP`, `CMD_SET_TEMPO`, `CMD_PAUSE`, `CMD_STOP`) at the top of every 10 ms frame tick so UI controls never block on decoder locks.
2. **Loop & Subsong End Handling:**
   - For `OPENMPT` and `XMP`: tracks loop count; in `KAJO_MOD_LOOP_ADVANCE`, stops at song end; in `KAJO_MOD_LOOP_FADEOUT`, triggers a 3.0-second linear gain ramp after 1 completed loop, then emits an `end-of-subsong` event to the UI main loop (`g_idle_add`).
   - For `UADE`: configures `UC_SUBSONG_TIMEOUT_VALUE` (default `180`s) and `UC_SILENCE_TIMEOUT_VALUE` (`8`s) and monitors `uade_read_notification()` for `UADE_NOTIFICATION_SONG_END`.
   - When a subsong finishes in `ADVANCE` or `FADEOUT` mode:
     - If `subsong_cur < subsong_max`, automatically advances to `subsong_cur + 1`.
     - Otherwise, advances to the next track in the playlist queue.
3. **Map-Gated UI Scope & Telemetry Tap (`AGENTS.md` §4.D):**
   - The audio thread writes the latest 512 stereo frames + `KajoModFrameState` into a lock-free ringbuffer only when `g_atomic_int_get(&audio->ui_mapped)` is `TRUE`.
   - The GTK4 60 fps tick callback (`gtk_widget_add_tick_callback`) is connected on window `map` and removed on `unmap`, ensuring `0%` UI rendering overhead when `kajo-mod` is minimized or on another workspace.

---

## 6. Visual Design & User Interface (`src/mod/mod_window.c`)

`kajo-mod` follows the **Aero + Metro** design system (`docs/design_system.md`):
- Undecorated window (`gtk_window_set_decorated(win, FALSE)`) with CSS classes `shell-settings-window` and `shell-mod-window`, default size `860 × 620`.
- `0px` corner radius across all cards, buttons, level bars, and tables.

### 6.1 Niri Column Proportions & Controlled Reflow Architecture

In Kajo's Niri configuration (`30-layout-animations.kdl`, `00-kajo.kdl`, `50-window-rules.kdl`), windows are tiled in scrolling columns with `default-column-width { proportion 0.5; }`, `preset-column-widths { proportion 0.33333; proportion 0.5; proportion 0.66667; }`, `gaps 6`, and `clip-to-geometry true`:
- On the reference `2880×1800 @ 1.75x` display (`1646×1028` logical px), Niri column widths are **`~540px` (`1/3`)**, **`~814px` (`1/2` default)**, **`~1088px` (`2/3`)**, and **`~1634px` (`1.0` full)**.
- Because `clip-to-geometry true` clips any GTK4 widget row whose natural/minimum width exceeds the active column width, and because `flex-wrap` on control bars causes vertical height jumps when cycling `Mod+R`, `kajo-mod` enforces two strict layout invariants:
  1. **Zero-Jump, Zero-Clip Top Decks (`Zones 1–5`, `vexpand: FALSE`, `min-width <= 480px`):**
     - **Zone 1 (`36px`):** Window Header Bar (`.settings-header-bar`, `🎹 KAJO MOD`, active engine badge, `🗗 FLOAT / TILE`, `✕`).
     - **Zone 2 (`66px`):** LCD Telemetry Readout Deck (`.mod-telemetry-deck` with `PANGO_ELLIPSIZE_END` on title/subtitle + 6-column homogeneous `.mod-counter-strip` grid `SUB │ VOICES │ TIMING │ CLOCK │ OUT │ TIME`).
     - **Zone 3 (`58px`):** 2-Row Deterministic Transport & Subsong Deck (`.mod-transport-deck` — Row 1: full-throw seek bar + `[ ◀ SUB ] SUB 01 / 09 [ SUB ▶ ]`; Row 2: transport buttons + `ADVANCE/LOOP/FADE` + volume).
     - **Zone 4 (`52px`):** 2-Row Deterministic Hardware & DSP Matrix (`.mod-dsp-strip` — Row 1: `FILTER`, `LED`, `SID` segmented selectors; Row 2: `1fr 1fr` equal grid for `STEREO SEP` and `TEMPO`).
     - **Zone 5 (`118px`, bounded height):** 4-column homogeneous Voice `[M]/[S]` Header Strip (`.mod-channel-header`) + `1fr 1fr` Dual-Trace Oscilloscope & 16-Band FFT Spectrum Deck (`.mod-scope-deck`).
  2. **Controlled `< 760px` Reflow in the Expanding `GtkPaned` Region (`Zone 6`, `vexpand: TRUE`):**
     - Note: A tracker pattern roll is intentionally omitted because `libuade` (68000 machine code), `libsidplayfp` (6510 + SID), and `libgme` do not expose tracker pattern tables, and multi-channel pattern grids clip horizontally in narrow Niri columns.
     - **Pane 6A — Upper Expanding Deck (`📋 MODULE, HARDWARE & ARCHIVE INSPECTOR`):**
       - **Wide Column ($\ge 760\text{px}$ — Niri `1/2` `0.5` and `2/3` `0.667` columns):** Displays the scrollable **Samples / Instruments / Hardware Voices Table** (`Fira Code`, preserving 22–32 char Amiga ASCII liner notes) and the scrollable **HVSC STIL / Archive `.NFO` / Song Message** well **side-by-side in a `1.2fr 1fr` split**.
       - **Narrow Column ($< 760\text{px}$ — Niri `1/3` `0.333` column at `~540px`):** Automatically switches to a **1-column full-width view** with a `[ SAMPLES / VOICES │ STIL / .NFO / MSG ]` `.segmented-group` in the pane header so 32-character ASCII sample tables are never clipped.
     - **Pane 6B — Lower Expanding Deck (`📂 PLAYLIST & ARCHIVE QUEUE`):**
       - Always-docked `GtkScrolledWindow` queue table where the track title column (`hexpand: TRUE`, `PANGO_ELLIPSIZE_END`) smoothly absorbs column width changes.
  3. **Zone 7 (`24px`, `vexpand: FALSE`):** Bottom Status Bar (`.mod-status-bar`).

```text
┌────────────────────────────────────────────────────────────────────────────────────────┐
│ 🎹 KAJO MOD  [UADE 68K • PAULA BLEP]                        [ 🗗 FLOAT / TILE ]  [ ✕ ] │ (Zone 1: Fixed 36px)
├────────────────────────────────────────────────────────────────────────────────────────┤
│ ┌─ LCD TELEMETRY DECK (#141414, 1px solid #2b2b2b, 3px left @metro-accent bar) ──────┐ │
│ │ TURRICAN II — THE FINAL FIGHT (TITLE THEME)     [UADE 3.05] [TFMX-PRO] [LED: ON]   │ │
│ │ SUB 01/09 │ VOICES 04 PAULA │ TIMING 50Hz PAL │ CLOCK 68k VBI │ 48.0kHz │ 01:14    │ │ (Zone 2: Fixed 66px)
│ └────────────────────────────────────────────────────────────────────────────────────┘ │
│ ┌─ 2-ROW DETERMINISTIC TRANSPORT & SUBSONG DECK (Min-Width ~440px, Zero Overflow) ───┐ │
│ │ 01:14 ━━━━━━━━●━━━━━━━━━━━━━━━━━━━━━━━━━━━ 03:20       [ ◀ SUB ] SUB 01 / 09 [SUB ▶]│ │ (Zone 3: Fixed 58px)
│ │ [⏮] [▶ PLAY] [⏸] [⏹] [⏭]   LOOP: [ADVANCE│LOOP│FADE]                 🔊 ━━━━●━ 85% │ │
│ └────────────────────────────────────────────────────────────────────────────────────┘ │
│ ┌─ 2-ROW DETERMINISTIC HARDWARE & DSP MATRIX (Zero Flex-Wrap Height Jumping) ────────┐ │
│ │ FILTER:[A500│A1200│NEAR│LIN│SINC]   LED:[AUTO│ON│OFF]   SID:[AUTO│6581│8580]       │ │ (Zone 4: Fixed 52px)
│ │ STEREO SEP: ━━━━●━━━ 65% (AMIGA)    │    TEMPO: ━━━━●━━━ 100% [1:1]                │ │
│ └────────────────────────────────────────────────────────────────────────────────────┘ │
│ ┌─ BOUNDED SCOPE & 16-BAND FFT DECK (vexpand: FALSE, 118px fixed height) ────────────┐ │
│ │ [PAULA 1L ▬▬ [M][S]] [PAULA 2R ▬▬ [M][S]] [PAULA 3R ▬▬ [M][S]] [PAULA 4L ▬▬ [M][S]]│ │ (Zone 5: Fixed 118px)
│ │ ┌─ STEREO OSCILLOSCOPE (88px) ───────────┐ ┌─ 16-BAND LOG FFT SPECTRUM (88px) ───┐ │ │
│ │ │  ~~~╱╲~~~╱╲~~~ (L @metro-accent)       │ │  ▄▄ ██ ▆▆ ▃▃ ▅▅ ▇▇ ▄▄ ▂▂ ...        │ │ │
│ │ └────────────────────────────────────────┘ └─────────────────────────────────────┘ │ │
│ └────────────────────────────────────────────────────────────────────────────────────┘ │
│ ┌─ PANE 6A: MODULE, HARDWARE & ARCHIVE INSPECTOR (2-Col >=760px / 1-Col <760px) ─────┐ │ (Zone 6A: Expands)
│ │ 📋 MODULE, HARDWARE & ARCHIVE INSPECTOR                   [2-COL SPLIT VIEW ≥760px]│ │
│ │ ┌─ SAMPLES / INSTRUMENTS (31 SLOTS) ─────┐ ┌─ ARCHIVE .NFO / HVSC STIL / MESSAGE ┐ │ │
│ │ │ 01 │ --- TURRICAN II ---    │ 14280    │ │ [UADE EAGLEPLAYER TELEMETRY]        │ │ │
│ │ │ 02 │ BY CHRIS HUELSBECK     │ 08420    │ │ Player: TFMX-Pro (Chris Hülsbeck)   │ │ │
│ │ │ 03 │ =======================│ 04112    │ │ Companion: mdat + smpl.turrican2    │ │ │
│ │ │ .. │ (Fills vertical space) │ .....    │ │ (In 1/3 col <760px, switches 1-col) │ │ │
│ │ └────────────────────────────────────────┘ └─────────────────────────────────────┘ │ │
│ └────────────────────────────────────────────────────────────────────────────────────┘ │
│ ═══════════════════════════ [ ⋯ GtkPaned Drag Handle ⋯ ] ═════════════════════════════ │
│ ┌─ PANE 6B: DOCKED PLAYLIST & ARCHIVE QUEUE (vexpand: TRUE) ─────────────────────────┐ │ (Zone 6B: Expands)
│ │ 📂 PLAYLIST & ARCHIVE QUEUE (8 TRACKS)       [+ ADD] [+ DIR] [− REM] [CLR] [🔍...] │ │
│ │ ▶01 │ Turrican II — The Final Fight (mdat.turrican2)  │ UADE 68K │ 9 SUB │ 03:20   │ │
│ │  02 │ Rob Hubbard — Commando High Score (.sid)        │ RESIDFP  │ 3 SUB │ 04:12   │ │
│ │  03 │ David Wise — Stickerbush Symphony (.spc)        │ LIBGME   │ 1 SUB │ 02:45   │ │
│ │  04 │ Purple Motion — Second Reality (2nd_pm.s3m)     │ OPENMPT  │ 1 SUB │ 04:38   │ │
│ └────────────────────────────────────────────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────────────────────────────────────────┤
│ ● PIPEWIRE 48.0kHz / FLOAT32LE │ PRIO 1: UADE 3.05        SPACE:PLAY ALT+◀/▶:SUB M/S   │ (Zone 7: Fixed 24px)
└────────────────────────────────────────────────────────────────────────────────────────┘
```

### 6.2 Keyboard Shortcuts

| Shortcut | Action |
| :--- | :--- |
| `Space` | Play / Pause |
| `S` | Stop |
| `Left` / `Right` | Seek `-5s` / `+5s` |
| `Alt+Left` / `Alt+Right` or `[` / `]` | Previous / Next Subsong |
| `Ctrl+Left` / `Ctrl+Right` or `P` / `N` | Previous / Next Track in Playlist |
| `1` .. `8` | Toggle Mute on Voice/Channel `1..8` (`Shift+1..8` = Solo Voice `1..8`) |
| `0` | Unmute All Voices/Channels |
| `F` | Cycle Amiga LED Filter (`AUTO` $\rightarrow$ `ON` $\rightarrow$ `OFF`) |
| `I` | Cycle Resampler Mode (`A500` $\rightarrow$ `A1200` $\rightarrow$ `NEAR` $\rightarrow$ `LIN` $\rightarrow$ `SINC`) |
| `Tab` | Toggle `SAMPLES / VOICES` $\leftrightarrow$ `STIL / .NFO` when in Narrow `1/3` Column Mode (`< 760px`) |
| `Ctrl+O` | Open Module / Archive File Dialog |
| `Ctrl+Q` | Quit `kajo-mod` |

---

## 7. Suite Integration & Persistence

1. **`ShellConfig` (`src/config.h` / `src/config.c`):**
   - Persist preferences in `~/.config/kajo/config.json`:
     - `mod_stereo_separation`: `int` (`0..100`, default `50`)
     - `mod_interp_mode`: `string` (`"a500"`, `"a1200"`, `"nearest"`, `"linear"`, `"sinc"`, default `"a500"`)
     - `mod_led_filter`: `string` (`"auto"`, `"on"`, `"off"`, default `"auto"`)
     - `mod_sid_model`: `string` (`"auto"`, `"6581"`, `"8580"`, default `"auto"`)
     - `mod_loop_mode`: `string` (`"advance"`, `"subsong"`, `"fadeout"`, default `"fadeout"`)
     - `mod_vis_mode`: `string` (`"scope"`, `"pattern"`, `"info"`, `"playlist"`, default `"scope"`)
     - `mod_volume`: `int` (`0..100`, default `80`)
2. **MPRIS2 (`org.mpris.MediaPlayer2.kajo_mod`):**
   - Reuses Kajo's MPRIS2 D-Bus pattern so the top panel `mpris` widget and media keys control `kajo-mod` out of the box.
3. **Desktop Entry (`data/org.kajo.Mod.desktop`) & `docs/ui-gallery.html`:**
   - Registers MIME types (`audio/x-mod`, `audio/x-xm`, `audio/x-s3m`, `audio/x-it`, `audio/x-amiga-module`, `audio/prs.sid`, `audio/x-sid`, `audio/x-spc`, `audio/x-nsf`, `audio/x-vgm`) and installs `/usr/bin/kajo-mod`.
   - Adds the `kajo-mod` Telemetry Deck, Paula/SID DSP Strip, and Pattern Roll specimen to `docs/ui-gallery.html` and `docs/design_system.md`.

---

## 8. Verification & Test Plan

1. **Automated Unit Tests (`tests/test_mod_decoder.c`):**
   - Prefix & suffix classification (`mod.foo`, `mdat.turrican2`, `smpl.turrican2` companion exclusion, `.ahx`, `.sid` `PSID`/`RSID` vs Amiga `sid1.*`, `.xm`, `.spc`, `.vgm`).
   - Five-backend priority routing test:
     1. **Priority 1 (`KAJO_MOD_BACKEND_UADE`):** Programmatically generated 4-channel Amiga ProTracker `.mod` (`M.K.`) opens via `libuade` and synthesizes stereo PCM frames while populating sample metadata.
     2. **Priority 2 (`KAJO_MOD_BACKEND_SIDPLAYFP`):** Minimal valid `PSID` v2 C64 header + 6510 SID write loop opens via `libsidplayfp` (`reSIDfp`), verifies title/author/copyright metadata, 6581/8580 model switch, and synthesizes non-zero stereo PCM frames.
     3. **Priority 3 (`KAJO_MOD_BACKEND_GME`):** Minimal valid chiptune (`SPC` / `AY` / `VGM`) opens via `libgme`.
     4. **Priority 4 & 5 (`KAJO_MOD_BACKEND_OPENMPT` / `KAJO_MOD_BACKEND_XMP`):** Non-UADE PC tracker format (e.g. FastTracker II `.xm`) falls back to `libopenmpt` and verifies pattern cell formatting, channel muting, and PCM synthesis.
   - Archive bundle unpacker test: create a temporary `.zip` containing a paired `mdat.test` + `smpl.test` + `readme.txt`, extract via `kajo_mod_archive_extract_bundle()`, verify only `mdat.test` is listed as a playable track while `smpl.test` sits alongside it on disk and `readme.txt` is captured as bundle info text.
2. **Sanitizer & Visual Verification:**
   - `meson compile -C builddir && meson test -C builddir` (0 warnings, all suites pass).
   - `./scripts/asan-test.sh` (0 leaks/UAF across decoder lifecycle and archive extraction).
   - `./scripts/ui-shot.sh` headless visual render check against `docs/ui-gallery.html`.
