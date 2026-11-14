#!/usr/bin/env bash
# Headless Vulkan end-to-end FX verification test (meson test 'comp-fx-shadow').
# Fails hard if Vulkan or KFX shaders fail to initialize.
# Verifies M2 milestones: SH1 (floating/popup/layer_top shadows + dim),
# SH2 (analytic Gaussian erf match, interior discard on transparent windows,
# opaque occlusion), SH3 (focus strength transition), SH4 (tiled/fullscreen
# no-shadow), and V4 (effects.enabled = false).
set -eu

KC="$1"
CL="$2"

export WLR_BACKENDS=headless
export WLR_RENDERER=vulkan
export WLR_LIBINPUT_NO_DEVICES=1
export KAJO_REQUIRE_VULKAN=1
export KAJO_COMP_NO_XWAYLAND=1
export KAJO_DEFAULTS_DIR="$(cd "$(dirname "$0")/../../data/defaults" && pwd)"
unset WAYLAND_DISPLAY DISPLAY WAYLAND_SOCKET KAJO_COMP_SOCKET NIRI_SOCKET KAJO_CONFIG_DIR

RT="$(mktemp -d)"
export XDG_RUNTIME_DIR="$RT"
trap 'rm -rf "$RT"' EXIT

if [ -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ]; then
    export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
else
    unset VK_INSTANCE_LAYERS
fi

# Hard Vulkan startup check (no fallback to pixman).
if ! timeout 10 "$KC" -q --exit-with-command -- true >"$RT/vk-probe.log" 2>&1; then
    echo "FAIL: Headless Vulkan startup failed (KAJO_REQUIRE_VULKAN=1):" >&2
    cat "$RT/vk-probe.log" >&2
    exit 1
fi
if grep -qi "pixman" "$RT/vk-probe.log" 2>/dev/null; then
    echo "FAIL: Unexpected pixman renderer in Vulkan FX test!" >&2
    cat "$RT/vk-probe.log" >&2
    exit 1
fi

cat > "$RT/png_helper.py" <<'PY'
import zlib, struct, math

def read_png(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos = 8
    w = h = 0
    idat = bytearray()
    while pos < len(data):
        ln, ctype = struct.unpack('>I4s', data[pos:pos+8])
        chunk = data[pos+8:pos+8+ln]
        pos += 12 + ln
        if ctype == b'IHDR':
            w, h = struct.unpack('>II', chunk[:8])
        elif ctype == b'IDAT':
            idat.extend(chunk)
    raw = zlib.decompress(bytes(idat))
    stride = w * 4
    rows = []
    prev = bytearray(stride)
    for y in range(h):
        rs = y * (stride + 1)
        filt = raw[rs]
        cur = bytearray(raw[rs+1:rs+1+stride])
        if filt == 1:
            for i in range(4, stride): cur[i] = (cur[i] + cur[i-4]) & 0xff
        elif filt == 2:
            for i in range(stride): cur[i] = (cur[i] + prev[i]) & 0xff
        elif filt == 3:
            for i in range(stride):
                a = cur[i-4] if i >= 4 else 0
                cur[i] = (cur[i] + ((a + prev[i]) >> 1)) & 0xff
        elif filt == 4:
            for i in range(stride):
                a = cur[i-4] if i >= 4 else 0
                b = prev[i]
                c = prev[i-4] if i >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 0xff
        prev = cur
        rows.append(cur)
    return w, h, rows

def px(rows, x, y):
    r = rows[y]
    i = x * 4
    return r[i], r[i+1], r[i+2], r[i+3]

def srgb_to_lin(u):
    v = u / 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4

def cov(x, y, x0, y0, x1, y1, s):
    k = 1.0 / (s * math.sqrt(2.0))
    return 0.25 * (math.erf((x - x0) * k) - math.erf((x - x1) * k)) * (math.erf((y - y0) * k) - math.erf((y - y1) * k))
PY

take_shot() {
    local kc="$1"
    local shot_dir="$2"
    local out_name="$3"
    rm -f "$shot_dir"/s-*.png
    "$kc" msg action screenshot-screen | grep -q '"ok"'
    for _ in $(seq 80); do
        local f
        f=$(find "$shot_dir" -maxdepth 1 -name 's-*.png' | head -n 1 || true)
        if [ -n "$f" ] && [ -s "$f" ]; then
            mv "$f" "$shot_dir/$out_name"
            return 0
        fi
        sleep 0.05
    done
    echo "Timed out waiting for screenshot $out_name" >&2
    return 1
}

wait_file() {
    for _ in $(seq 80); do
        [ -f "$1" ] && return 0
        sleep 0.05
    done
    echo "Timed out waiting for ready file $1" >&2
    return 1
}

run_case() {
    local case_name="$1"
    local script_body="$2"
    local cfg_dir="$RT/$case_name/cfg"
    local shot_dir="$RT/$case_name/shots"
    local log_file="$RT/$case_name/comp.log"
    mkdir -p "$cfg_dir" "$shot_dir"

    set +e
    timeout 40 env KAJO_CONFIG_DIR="$cfg_dir" "$KC" --exit-with-command -- bash -c "$script_body" \
        >"$log_file" 2>&1
    local rc=$?
    set -e
    if [ $rc -ne 0 ]; then
        echo "FAIL [$case_name]: exited $rc" >&2
        cat "$log_file" >&2
        exit 1
    fi
    if grep -qE "VUID-|Validation Error" "$log_file" 2>/dev/null; then
        echo "FAIL [$case_name]: Vulkan validation error found:" >&2
        grep -E "VUID-|Validation Error" "$log_file" >&2
        exit 1
    fi
    echo "ok - $case_name"
}

export -f take_shot wait_file

# ── Case 1: SH2 analytic Gaussian erf + transparent interior discard ──
mkdir -p "$RT/sh2/cfg" "$RT/sh2/shots"
cat > "$RT/sh2/cfg/comp.toml" <<EOF
[layout]
gaps_inner = 0
gaps_outer = 0
border_px = 0

[effects]
enabled = true
dither = false

[effects.shadow]
enabled = true
color = "#000000"
unfocused_scale = 0.25
duration_ms = 0

[[effects.shadow.layers]]
sigma = 16.0
offset_x = 0.0
offset_y = 0.0
opacity = 0.6

[screenshots]
path = "$RT/sh2/shots/s-%Y%m%d-%H%M%S.png"
copy_to_clipboard = false
show_notification = false
include_window_border = false
EOF
cat > "$RT/sh2/cfg/rules.toml" <<'EOF'
[[rule]]
app_id = ['^org\.kajo\.FxTrans$']
floating = true
anchor_corner = "top-left"
anchor_x = 400
anchor_y = 200
EOF
run_case "sh2" "
    set -eu
    '$CL' fx-layer background kajo-bg 0 0 0 0 ffffffff '$RT/sh2-bg' &
    BG_PID=\$!
    wait_file '$RT/sh2-bg'

    '$CL' fx-win org.kajo.FxTrans 200 160 00000000 '$RT/sh2-win' &
    WIN_PID=\$!
    wait_file '$RT/sh2-win'
    sleep 0.05

    take_shot '$KC' '$RT/sh2/shots' 'sh2.png'
    PYTHONPATH='$RT' python3 - '$RT/sh2/shots/sh2.png' <<'PY'
import sys, png_helper as ph

w, h, rows = ph.read_png(sys.argv[1])
assert (w, h) == (1280, 720), (w, h)
assert ph.px(rows, 20, 20)[:3] == (255, 255, 255), ph.px(rows, 20, 20)
# Interior of 100% transparent window [400..600]x[200..360] MUST remain pure white (discarded).
assert ph.px(rows, 500, 280)[:3] == (255, 255, 255), f'interior bleed: {ph.px(rows, 500, 280)}'

prev_r = -1
for d in (2, 6, 12, 20, 32):
    r, g, b, _ = ph.px(rows, 500, 200 - d)
    assert abs(r - g) <= 2 and abs(r - b) <= 2, (r, g, b)
    meas_lin = ph.srgb_to_lin(r)
    exp_cov = ph.cov(500.5, 200.0 - d + 0.5, 400.0, 200.0, 600.0, 360.0, 16.0)
    exp_lin = 1.0 - 0.6 * exp_cov
    assert abs(meas_lin - exp_lin) < 0.025, f'd={d}: meas={meas_lin:.4f} exp={exp_lin:.4f} rgb=({r},{g},{b})'
    assert r > prev_r, f'non-monotonic decay at d={d}: {r} <= {prev_r}'
    prev_r = r
PY
    rm -f '$RT/sh2-win' '$RT/sh2-bg'
    wait \$WIN_PID \$BG_PID || true
"

# ── Case 2: SH1 popup/layer shadows + SH2 opaque occlusion + [effects.dim] ──
mkdir -p "$RT/sh1_occlusion_dim/cfg" "$RT/sh1_occlusion_dim/shots"
cat > "$RT/sh1_occlusion_dim/cfg/comp.toml" <<EOF
[layout]
gaps_inner = 0
gaps_outer = 0
border_px = 0

[effects]
enabled = true
dither = false

[effects.shadow]
enabled = true
color = "#000000"
unfocused_scale = 0.2
duration_ms = 0

[[effects.shadow.layers]]
sigma = 14.0
offset_x = 0.0
offset_y = 0.0
opacity = 0.6

[effects.dim]
enabled = true
color = "#000000"
opacity = 0.4
duration_ms = 0

[screenshots]
path = "$RT/sh1_occlusion_dim/shots/s-%Y%m%d-%H%M%S.png"
copy_to_clipboard = false
show_notification = false
include_window_border = false
EOF
cat > "$RT/sh1_occlusion_dim/cfg/rules.toml" <<'EOF'
[[rule]]
app_id = ['^org\.kajo\.FxA$']
floating = true
anchor_corner = "top-left"
anchor_x = 400
anchor_y = 200

[[rule]]
app_id = ['^org\.kajo\.FxB$']
floating = true
anchor_corner = "top-left"
anchor_x = 580
anchor_y = 280
EOF
run_case "sh1_occlusion_dim" "
    set -eu
    '$CL' fx-layer background kajo-bg 0 0 0 0 ffffffff '$RT/sh1-bg' &
    BG_PID=\$!
    wait_file '$RT/sh1-bg'

    '$CL' fx-layer top kajo-pill 160 60 40 40 107070ff '$RT/sh1-pill' >/dev/null 2>&1 &
    PILL_PID=\$!
    wait_file '$RT/sh1-pill'

    '$CL' fx-win org.kajo.FxA 300 200 d02020ff '$RT/sh1-a' &
    A_PID=\$!
    wait_file '$RT/sh1-a'

    # Popup placed at offset (220, 10) relative to FxB (580,280) -> [800..920]x[290..350]
    '$CL' fx-win org.kajo.FxB 200 160 2040d0ff '$RT/sh1-b' 220 10 120 60 >/dev/null 2>&1 &
    B_PID=\$!
    wait_file '$RT/sh1-b'
    sleep 0.05

    take_shot '$KC' '$RT/sh1_occlusion_dim/shots' 'sh1.png'
    PYTHONPATH='$RT' python3 - '$RT/sh1_occlusion_dim/shots/sh1.png' <<'PY'
import sys, png_helper as ph

w, h, rows = ph.read_png(sys.argv[1])
# 1. Non-exclusive layer surface kajo-pill [40..200]x[40..100] has pure interior and dark halo at bottom edge
pr, pg, pb, _ = ph.px(rows, 120, 70)
assert (pr, pg, pb) == (0x10, 0x70, 0x70), f'layer interior wrong: {(pr, pg, pb)}'
hr, hg, hb, _ = ph.px(rows, 120, 105)
assert hr < 235 and hg < 235 and hb < 235, f'layer surface did not cast shadow: {(hr, hg, hb)}'

# 2. FxB (focused, [580..780]x[280..440]) overlaps FxA [400..700]x[200..400] at (650, 340): pure FxB blue
br, bg, bb, _ = ph.px(rows, 650, 340)
assert abs(br - 0x20) <= 2 and abs(bg - 0x40) <= 2 and abs(bb - 0xd0) <= 2, f'opaque overlap wrong: {(br, bg, bb)}'

# 3. Uncovered FxA (unfocused, dim=0.4) at (450, 250) is darkened relative to 0xd0 (~208 -> ~167 sRGB)
ar, ag, ab, _ = ph.px(rows, 450, 250)
assert ar < 185 and ar > 135, f'expected dimmed FxA red (~167), got {(ar, ag, ab)}'

# 4. xdg_popup [800..920]x[290..350] casts dark shadow onto white bg outside bottom edge at (860, 355)
qr, qg, qb, _ = ph.px(rows, 860, 355)
assert qr < 235 and qg < 235 and qb < 235, f'popup did not cast shadow: {(qr, qg, qb)}'
PY
    # Leave PILL_PID (layer surface) and B_PID (window + popup) alive across compositor
    # shutdown so server_destructor tears down live FX surfaces during wl_display_destroy_clients.
    rm -f '$RT/sh1-a' '$RT/sh1-bg'
    wait \$A_PID \$BG_PID || true
"

# ── Case 3: SH3 focus strength transition ──
mkdir -p "$RT/sh3_focus/cfg" "$RT/sh3_focus/shots"
cat > "$RT/sh3_focus/cfg/comp.toml" <<EOF
[layout]
gaps_inner = 0
gaps_outer = 0
border_px = 0

[effects]
enabled = true
dither = false

[effects.shadow]
enabled = true
color = "#000000"
unfocused_scale = 0.15
duration_ms = 120

[[effects.shadow.layers]]
sigma = 14.0
offset_x = 0.0
offset_y = 0.0
opacity = 0.7

[screenshots]
path = "$RT/sh3_focus/shots/s-%Y%m%d-%H%M%S.png"
copy_to_clipboard = false
show_notification = false
include_window_border = false
EOF
cat > "$RT/sh3_focus/cfg/rules.toml" <<'EOF'
[[rule]]
app_id = ['^org\.kajo\.FxA$']
floating = true
anchor_corner = "top-left"
anchor_x = 200
anchor_y = 200

[[rule]]
app_id = ['^org\.kajo\.FxB$']
floating = true
anchor_corner = "top-left"
anchor_x = 700
anchor_y = 200
EOF
run_case "sh3_focus" "
    set -eu
    '$CL' fx-layer background kajo-bg 0 0 0 0 ffffffff '$RT/sh3-bg' &
    BG_PID=\$!
    wait_file '$RT/sh3-bg'

    '$CL' fx-win org.kajo.FxA 200 200 202080ff '$RT/sh3-a' &
    A_PID=\$!
    wait_file '$RT/sh3-a'

    '$CL' fx-win org.kajo.FxB 200 200 802020ff '$RT/sh3-b' &
    B_PID=\$!
    wait_file '$RT/sh3-b'
    sleep 0.25

    # FxB focused, FxA unfocused -> capture FxA left halo (x=196, y=300)
    take_shot '$KC' '$RT/sh3_focus/shots' 'unfocused.png'
    '$KC' msg action focus-app app_id=org.kajo.FxA | grep -q '\"ok\"'
    sleep 0.30
    take_shot '$KC' '$RT/sh3_focus/shots' 'focused.png'

    PYTHONPATH='$RT' python3 - '$RT/sh3_focus/shots/unfocused.png' '$RT/sh3_focus/shots/focused.png' <<'PY'
import sys, png_helper as ph

_, _, u_rows = ph.read_png(sys.argv[1])
_, _, f_rows = ph.read_png(sys.argv[2])
ur = ph.px(u_rows, 196, 300)[0]
fr = ph.px(f_rows, 196, 300)[0]
# Focused halo must be significantly darker than unfocused halo (0.7 vs 0.105 linear opacity),
# matching exact Gaussian erf attenuation in linear space.
c = ph.cov(196.5, 300.5, 200.0, 200.0, 400.0, 400.0, 14.0)
exp_u = 1.0 - (0.7 * 0.15) * c
exp_f = 1.0 - 0.7 * c
meas_u = ph.srgb_to_lin(ur)
meas_f = ph.srgb_to_lin(fr)
assert abs(meas_u - exp_u) < 0.03, f'unfocused lin mismatch: meas={meas_u:.4f} exp={exp_u:.4f} r={ur}'
assert abs(meas_f - exp_f) < 0.03, f'focused lin mismatch: meas={meas_f:.4f} exp={exp_f:.4f} r={fr}'
assert ur > fr + 25, f'focus transition too small: unfocused_r={ur}, focused_r={fr}'
PY
    rm -f '$RT/sh3-b' '$RT/sh3-a' '$RT/sh3-bg'
    wait \$B_PID \$A_PID \$BG_PID || true
"

# ── Case 4: SH4 tiled default no-shadow + fullscreen no-shadow + V4 effects.enabled=false ──
mkdir -p "$RT/sh4_tiled_fullscreen/cfg" "$RT/sh4_tiled_fullscreen/shots"
cat > "$RT/sh4_tiled_fullscreen/cfg/comp.toml" <<EOF
[layout]
gap_px = 24
border_px = 0
default_column_width = 0.5

[animations]
enabled = false

[effects]
enabled = true

[screenshots]
path = "$RT/sh4_tiled_fullscreen/shots/s-%Y%m%d-%H%M%S.png"
copy_to_clipboard = false
show_notification = false
include_window_border = false
EOF
cat > "$RT/sh4_tiled_fullscreen/cfg/rules.toml" <<'EOF'
# No floating/shadow rule for FxTiled -> tiled by default, no shadow.
EOF
run_case "sh4_tiled_fullscreen" "
    set -eu
    '$CL' fx-layer background kajo-bg 0 0 0 0 ffffffff '$RT/sh4-bg' &
    BG_PID=\$!
    wait_file '$RT/sh4-bg'

    '$CL' fx-win org.kajo.FxTiled 400 400 205020ff '$RT/sh4-a' &
    A_PID=\$!
    wait_file '$RT/sh4-a'

    '$CL' fx-win org.kajo.FxTiled 400 400 502050ff '$RT/sh4-b' &
    B_PID=\$!
    wait_file '$RT/sh4-b'
    sleep 0.1

    take_shot '$KC' '$RT/sh4_tiled_fullscreen/shots' 'tiled.png'
    '$KC' msg action fullscreen-window | grep -q '\"ok\"'
    sleep 0.1
    take_shot '$KC' '$RT/sh4_tiled_fullscreen/shots' 'fullscreen.png'

    PYTHONPATH='$RT' python3 - '$RT/sh4_tiled_fullscreen/shots/tiled.png' '$RT/sh4_tiled_fullscreen/shots/fullscreen.png' <<'PY'
import sys, png_helper as ph

_, _, t_rows = ph.read_png(sys.argv[1])
# Sample outer gap (12, 360) and inner gap [628..652] between col 1 (w=604 at x=24) and col 2 (at x=652):
# must stay pure white (255,255,255), zero tiled shadow!
assert ph.px(t_rows, 12, 360)[:3] == (255, 255, 255), ph.px(t_rows, 12, 360)
gap_whites = sum(1 for x in range(620, 660) if ph.px(t_rows, x, 360)[:3] == (255, 255, 255))
assert gap_whites >= 20, f'shadow leaked into tiled gap (pure white pixels={gap_whites})'

_, _, fs_rows = ph.read_png(sys.argv[2])
# Fullscreen window FxB (0x50, 0x20, 0x50) fills whole 1280x720 output with no shadow or white gaps
assert ph.px(fs_rows, 5, 5)[:3] == (0x50, 0x20, 0x50), ph.px(fs_rows, 5, 5)
assert ph.px(fs_rows, 1270, 710)[:3] == (0x50, 0x20, 0x50), ph.px(fs_rows, 1270, 710)
PY
    rm -f '$RT/sh4-b' '$RT/sh4-a' '$RT/sh4-bg'
    wait \$B_PID \$A_PID \$BG_PID || true
"

# ── Case 5: V4 effects.enabled = false ──
mkdir -p "$RT/v4_disabled/cfg" "$RT/v4_disabled/shots"
cat > "$RT/v4_disabled/cfg/comp.toml" <<EOF
[layout]
gaps_inner = 0
gaps_outer = 0
border_px = 0

[effects]
enabled = false

[screenshots]
path = "$RT/v4_disabled/shots/s-%Y%m%d-%H%M%S.png"
copy_to_clipboard = false
show_notification = false
include_window_border = false
EOF
cat > "$RT/v4_disabled/cfg/rules.toml" <<'EOF'
[[rule]]
app_id = ['^org\.kajo\.FxTrans$']
floating = true
anchor_corner = "top-left"
anchor_x = 400
anchor_y = 200
EOF
run_case "v4_disabled" "
    set -eu
    '$CL' fx-layer background kajo-bg 0 0 0 0 ffffffff '$RT/v4-bg' &
    BG_PID=\$!
    wait_file '$RT/v4-bg'

    '$CL' fx-win org.kajo.FxTrans 200 160 202020ff '$RT/v4-win' &
    WIN_PID=\$!
    wait_file '$RT/v4-win'
    sleep 0.05

    take_shot '$KC' '$RT/v4_disabled/shots' 'v4.png'
    PYTHONPATH='$RT' python3 - '$RT/v4_disabled/shots/v4.png' <<'PY'
import sys, png_helper as ph

_, _, rows = ph.read_png(sys.argv[1])
assert ph.px(rows, 500, 195)[:3] == (255, 255, 255), f'shadow drawn when effects.enabled=false: {ph.px(rows, 500, 195)}'
assert ph.px(rows, 395, 280)[:3] == (255, 255, 255), f'shadow drawn when effects.enabled=false: {ph.px(rows, 395, 280)}'
PY
    rm -f '$RT/v4-win' '$RT/v4-bg'
    wait \$WIN_PID \$BG_PID || true
"

# ── Case 6: GB1 & GB2 glass blur over high-contrast step edge + edge-hugging panel ──
mkdir -p "$RT/gb1_gb2_glass/cfg" "$RT/gb1_gb2_glass/shots"
cat > "$RT/gb1_gb2_glass/cfg/comp.toml" <<EOF
[layout]
gaps_inner = 0
gaps_outer = 0
border_px = 0

[effects]
enabled = true
dither = false

[effects.glass]
enabled = true
passes = 3
offset = 2.0
saturation = 1.15
brightness = 0.90
tint = "#141414"
tint_alpha = 0.35

[effects.shadow]
enabled = false

[screenshots]
path = "$RT/gb1_gb2_glass/shots/s-%Y%m%d-%H%M%S.png"
copy_to_clipboard = false
show_notification = false
include_window_border = false
EOF
cat > "$RT/gb1_gb2_glass/cfg/rules.toml" <<'EOF'
[[rule]]
app_id = ['^org\.kajo\.FxStep$']
floating = true
anchor_corner = "top-left"
anchor_x = 400
anchor_y = 200
shadow = false
blur = false

[[rule]]
app_id = ['^org\.kajo\.FxGlass$']
floating = true
anchor_corner = "top-left"
anchor_x = 500
anchor_y = 220
shadow = false
blur = true

[[layer_rule]]
namespace = ['^kajo-edge-panel$']
blur = true
xray = false
shadow = false
EOF
run_case "gb1_gb2_glass" "
    set -eu
    '$CL' fx-layer background kajo-bg 0 0 0 0 ffffffff '$RT/gb2-bg' &
    BG_PID=\$!
    wait_file '$RT/gb2-bg'

    # Edge-hugging translucent panel at (0, 0) size 1280x32
    '$CL' fx-layer top kajo-edge-panel 1280 32 0 0 00000000 '$RT/gb2-panel' >/dev/null 2>&1 &
    PANEL_PID=\$!
    wait_file '$RT/gb2-panel'

    # Opaque red step [400..600]x[200..400]
    '$CL' fx-win org.kajo.FxStep 200 200 ff0000ff '$RT/gb2-step' &
    STEP_PID=\$!
    wait_file '$RT/gb2-step'

    # Transparent glass surface [500..700]x[220..380] straddling x=600 red/white boundary
    '$CL' fx-win org.kajo.FxGlass 200 160 00000000 '$RT/gb2-glass' &
    GLASS_PID=\$!
    wait_file '$RT/gb2-glass'
    sleep 0.05

    take_shot '$KC' '$RT/gb1_gb2_glass/shots' 'gb2.png'
    PYTHONPATH='$RT' python3 - '$RT/gb1_gb2_glass/shots/gb2.png' <<'PY'
import sys, png_helper as ph

w, h, rows = ph.read_png(sys.argv[1])
# 1. GB2: Pixels outside glass box [500..700]x[220..380] are 100% untouched!
assert ph.px(rows, 550, 210)[:3] == (255, 0, 0), f'red outside glass modified: {ph.px(rows, 550, 210)}'
assert ph.px(rows, 650, 210)[:3] == (255, 255, 255), f'white outside glass modified: {ph.px(rows, 650, 210)}'
assert ph.px(rows, 705, 300)[:3] == (255, 255, 255), f'right of glass modified: {ph.px(rows, 705, 300)}'

# 2. GB2: Reference match deep inside glass over white (x=670, y=300) and over red (x=530, y=300)
tint_l = ph.srgb_to_lin(0x14)
exp_white_l = (1.0 - 0.35) * (1.0 * 0.90) + 0.35 * tint_l
wr, wg, wb, _ = ph.px(rows, 670, 300)
assert abs(ph.srgb_to_lin(wr) - exp_white_l) < 0.03, f'white glass mismatch: {ph.srgb_to_lin(wr):.4f} vs {exp_white_l:.4f}'

# Over red (1, 0, 0): after brightness 0.9 -> (0.9, 0, 0), luma = 0.2126*0.9 = 0.19134,
# sat 1.15 -> r = 0.19134 + (0.9 - 0.19134)*1.15 = 1.0063, g=b clamped to 0.0
exp_red_r_l = (1.0 - 0.35) * (0.19134 + (0.90 - 0.19134) * 1.15) + 0.35 * tint_l
exp_red_gb_l = 0.35 * tint_l
rr, rg, rb, _ = ph.px(rows, 530, 300)
assert abs(ph.srgb_to_lin(rr) - exp_red_r_l) < 0.035, f'red glass R mismatch: {ph.srgb_to_lin(rr):.4f} vs {exp_red_r_l:.4f}'
assert abs(ph.srgb_to_lin(rg) - exp_red_gb_l) < 0.02, f'red glass G mismatch: {ph.srgb_to_lin(rg):.4f} vs {exp_red_gb_l:.4f}'

# 3. GB2: Smooth blur transition across step edge at x=600 (green channel increases monotonically)
g_vals = [ph.px(rows, x, 300)[1] for x in (584, 592, 600, 608, 616)]
assert g_vals[0] < g_vals[2] < g_vals[4], f'expected smooth blur across x=600, got G={g_vals}'
assert 30 < g_vals[2] < 190, f'step edge at x=600 not blurred: G={g_vals[2]}'

# 4. GB1: Edge-hugging panel at (0, 0) has no dark border bleed at y=0..1 vs y=16
e0 = ph.px(rows, 100, 0)[:3]
e1 = ph.px(rows, 100, 1)[:3]
em = ph.px(rows, 100, 16)[:3]
assert abs(e0[0] - em[0]) <= 3 and abs(e1[0] - em[0]) <= 3, f'edge bleed at top border: y0={e0} y1={e1} mid={em}'

# 5. GB6 (dither=false): Uniform glass region [650..658]x[260..268] has 1 exact code value (no dither)
u_vals = {ph.px(rows, 650 + dx, 260 + dy)[0] for dx in range(8) for dy in range(8)}
assert len(u_vals) == 1, f'expected uniform code value when dither=false, got {u_vals}'
PY
    rm -f '$RT/gb2-glass' '$RT/gb2-step' '$RT/gb2-panel' '$RT/gb2-bg'
    wait \$GLASS_PID \$STEP_PID \$PANEL_PID \$BG_PID || true
"

# ── Case 7: GB8 (opaque occlusion under/over glass) & GB9 (xray background-only panel) & GB6 (dither) ──
mkdir -p "$RT/gb6_gb8_gb9/cfg" "$RT/gb6_gb8_gb9/shots"
cat > "$RT/gb6_gb8_gb9/cfg/comp.toml" <<EOF
[layout]
gaps_inner = 0
gaps_outer = 0
border_px = 0

[effects]
enabled = true
dither = true

[effects.glass]
enabled = true
passes = 3
offset = 2.0
saturation = 1.15
brightness = 0.90
tint = "#141414"
tint_alpha = 0.35

[effects.shadow]
enabled = false

[screenshots]
path = "$RT/gb6_gb8_gb9/shots/s-%Y%m%d-%H%M%S.png"
copy_to_clipboard = false
show_notification = false
include_window_border = false
EOF
cat > "$RT/gb6_gb8_gb9/cfg/rules.toml" <<'EOF'
[[rule]]
app_id = ['^org\.kajo\.FxUnderPanel$']
floating = true
anchor_corner = "top-left"
anchor_x = 100
anchor_y = 0
shadow = false
blur = false

[[rule]]
app_id = ['^org\.kajo\.FxGlassOcc$']
floating = true
anchor_corner = "top-left"
anchor_x = 500
anchor_y = 200
shadow = false
blur = true

[[rule]]
app_id = ['^org\.kajo\.FxTopOcc$']
floating = true
anchor_corner = "top-left"
anchor_x = 600
anchor_y = 200
shadow = false
blur = false

[[layer_rule]]
namespace = ['^kajo-xray-panel$']
blur = true
xray = true
shadow = false

[[layer_rule]]
namespace = ['^kajo-autohide-panel$']
blur = true
EOF
run_case "gb6_gb8_gb9" "
    set -eu
    '$CL' fx-layer background kajo-bg 0 0 0 0 ffffffff '$RT/gb9-bg' &
    BG_PID=\$!
    wait_file '$RT/gb9-bg'

    # Bright red floating window at (100, 0) directly underneath the xray panel
    '$CL' fx-win org.kajo.FxUnderPanel 300 120 ff0000ff '$RT/gb9-under' &
    UNDER_PID=\$!
    wait_file '$RT/gb9-under'

    # Top-layer xray=true panel at (0, 0) size 1280x36: must blur ONLY background (white), ignoring FxUnderPanel!
    '$CL' fx-layer top kajo-xray-panel 600 36 0 0 00000000 '$RT/gb9-panel' >/dev/null 2>&1 &
    PANEL_PID=\$!
    wait_file '$RT/gb9-panel'

    # Retracted autohide top-layer panel at (700, -34) size 400x36 (2px edge strip): blur & shadow must be suppressed!
    '$CL' fx-layer top kajo-autohide-panel 400 36 -34 700 00000000 '$RT/gb9-ah' >/dev/null 2>&1 &
    AH_PID=\$!
    wait_file '$RT/gb9-ah'

    # GB8: Glass window [500..700]x[200..400] with an opaque white window FxTopOcc [600..800]x[200..400] above its right half
    '$CL' fx-win org.kajo.FxGlassOcc 200 200 00000000 '$RT/gb9-gocc' &
    GOCC_PID=\$!
    wait_file '$RT/gb9-gocc'

    '$CL' fx-win org.kajo.FxTopOcc 200 200 ffffffff '$RT/gb9-tocc' &
    TOCC_PID=\$!
    wait_file '$RT/gb9-tocc'
    sleep 0.05

    take_shot '$KC' '$RT/gb6_gb8_gb9/shots' 'gb9.png'
    PYTHONPATH='$RT' python3 - '$RT/gb6_gb8_gb9/shots/gb9.png' <<'PY'
import sys, png_helper as ph

_, _, rows = ph.read_png(sys.argv[1])
# 1. GB9: xray=true panel at (200, 18) over red FxUnderPanel blurs white wallpaper (neutral R==G==B), NOT red window!
pr, pg, pb, _ = ph.px(rows, 200, 18)
assert abs(pr - pg) <= 2 and abs(pg - pb) <= 2 and pr > 180, f'xray panel bled red window: {(pr, pg, pb)}'

# 2. GB8: Uncovered left half of FxGlassOcc right next to FxTopOcc edge at (595, 300) has no dark halo
or_, og, ob, _ = ph.px(rows, 595, 300)
mr, mg, mb, _ = ph.px(rows, 530, 300)
assert abs(or_ - mr) <= 3 and abs(og - mg) <= 3, f'dark halo next to opaque occluder: edge={(or_, og, ob)} mid={(mr, mg, mb)}'

# 3. GB6: With dither=true, an 8x8 tile in uniform glass region [520..528]x[260..268] has blue-noise dither variance (>1 distinct code value)
vals = {ph.px(rows, 520 + dx, 260 + dy)[0] for dx in range(8) for dy in range(8)}
assert len(vals) >= 2 and max(vals) - min(vals) <= 2, f'expected 1-LSB dither in 8x8 block, got {vals}'

# 4. Retracted autohide panel at [700..1100]x[-34..2]: neither glass tint at (900, 1) nor drop shadow at (900, 10)
ar1, ag1, ab1, _ = ph.px(rows, 900, 1)
ar2, ag2, ab2, _ = ph.px(rows, 900, 10)
assert (ar1, ag1, ab1) == (255, 255, 255), f'retracted autohide strip drew glass tint: {(ar1, ag1, ab1)}'
assert (ar2, ag2, ab2) == (255, 255, 255), f'retracted autohide strip cast shadow: {(ar2, ag2, ab2)}'
PY
    rm -f '$RT/gb9-tocc' '$RT/gb9-gocc' '$RT/gb9-ah' '$RT/gb9-panel' '$RT/gb9-under' '$RT/gb9-bg'
    wait \$TOCC_PID \$GOCC_PID \$AH_PID \$PANEL_PID \$UNDER_PID \$BG_PID || true
"

echo "All comp-fx-shadow headless Vulkan pixel tests passed."
