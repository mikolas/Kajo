#!/usr/bin/env bash
# kajo-comp headless smoke test (meson test 'comp-headless').
# Runs the compositor on the wlroots headless backend with the pixman renderer
# (no GPU needed) and checks: required globals, client churn, exit-status
# propagation, missing command, and SIGTERM shutdown.
# Usage: comp_headless_smoke.sh KAJO_COMP TEST_CLIENT
set -u
KC="$1"
CL="$2"
fail=0
export WLR_BACKENDS=headless WLR_RENDERER="${WLR_RENDERER:-pixman}" WLR_LIBINPUT_NO_DEVICES=1
unset WAYLAND_DISPLAY DISPLAY WAYLAND_SOCKET KAJO_COMP_SOCKET NIRI_SOCKET
# Private runtime dir so the test never touches the user's live sockets
RT="$(mktemp -d)"
export XDG_RUNTIME_DIR="$RT"
trap 'rm -rf "$RT"' EXIT

check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "ok - $1"; else echo "not ok - $1 (want $2, got $3)"; fail=1; fi
}

timeout 30 "$KC" -q --exit-with-command -- "$CL" globals \
    wl_compositor wl_subcompositor wl_shm wl_seat wl_output wl_data_device_manager \
    zwp_primary_selection_device_manager_v1 wp_viewporter wp_single_pixel_buffer_manager_v1 \
    wp_fractional_scale_manager_v1 wp_presentation xdg_wm_base zxdg_decoration_manager_v1 \
    zwlr_layer_shell_v1 ext_session_lock_manager_v1 ext_idle_notifier_v1 \
    zwp_idle_inhibit_manager_v1 zwlr_output_power_manager_v1 \
    zwp_input_method_manager_v2 zwp_text_input_manager_v3 zwp_virtual_keyboard_manager_v1 \
    zwp_pointer_constraints_v1 zwp_relative_pointer_manager_v1 zwp_pointer_gestures_v1 \
    zxdg_exporter_v2 zxdg_importer_v2 xdg_wm_dialog_v1 xdg_activation_v1 \
    zwp_keyboard_shortcuts_inhibit_manager_v1 zwp_tablet_manager_v2 \
    zwlr_screencopy_manager_v1 zwlr_export_dmabuf_manager_v1 \
    ext_image_copy_capture_manager_v1 ext_output_image_capture_source_manager_v1 \
    ext_foreign_toplevel_image_capture_source_manager_v1 \
    zwlr_output_manager_v1 zxdg_output_manager_v1 \
    zwlr_foreign_toplevel_manager_v1 ext_foreign_toplevel_list_v1 \
    wp_alpha_modifier_v1 wp_tearing_control_manager_v1
check "required globals advertised" 0 $?

timeout 30 "$KC" -q --exit-with-command -- "$CL" windows 6
check "xdg_toplevel map/configure/decoration/popup/fullscreen lifecycle" 0 $?

timeout 30 "$KC" -q --exit-with-command -- "$CL" shell
check "zwlr_layer_shell_v1 exclusive zone + ext_session_lock_v1 lifecycle" 0 $?

# Phase 2d + Phase 3: IPC (`kajo-comp msg`), hot-reload, in-compositor switcher & screenshot UI
CFG_DIR="$RT/kajo"
SHOT_DIR="$RT/shots"
mkdir -p "$CFG_DIR/conf.d" "$SHOT_DIR"
cat > "$CFG_DIR/comp.toml" <<EOF
[layout]
gaps_inner = 10

[screenshots]
path = "$SHOT_DIR/shot-%Y%m%d-%H%M%S.png"
copy_to_clipboard = true
show_notification = true
include_window_border = true
EOF
export XDG_CONFIG_HOME="$RT"
timeout 30 "$KC" -q --exit-with-command -- sh -c "
    set -e
    \"$KC\" msg get-outputs | grep -q '\"ok\"'
    \"$KC\" msg get-workspaces | grep -q '\"ok\"'
    \"$KC\" msg get-windows | grep -q '\"ok\"'
    \"$KC\" msg get-keyboard-layouts | grep -q '\"ok\"'
    \"$KC\" msg action switch-workspace idx=2 | grep -q '\"ok\"'
    \"$KC\" msg action switch-workspace idx=1 | grep -q '\"ok\"'
    \"$KC\" msg debug-mem | grep -q 'CompServer'
    printf '[layout]\ngaps_inner = 14\n' > '$CFG_DIR/conf.d/10-test.toml'
    \"$KC\" msg action reload-config | grep -q '\"ok\"'

    # Subscribe to screenshot events in background
    \"$KC\" msg subscribe screenshots > '$RT/sub.log' &
    SUB_PID=\$!
    sleep 0.05

    # Spawn 3 mapped windows and wait for ready file
    \"$CL\" hold 3 '$RT/ready' &
    HOLD_PID=\$!
    for _ in \$(seq 50); do [ -f '$RT/ready' ] && break; sleep 0.05; done
    [ -f '$RT/ready' ]

    # Test interactive Alt-Tab switcher: open -> cancel (preserves focus), open -> confirm (changes focus)
    BEFORE_WINS=\$(\"${KC}\" msg get-windows)
    echo \"\$BEFORE_WINS\" | grep -q '\"title\":\"Kajo Test Window 3\".*\"is_focused\":true'
    \"$KC\" msg action next-window hold=true | grep -q '\"ok\"'
    \"$KC\" msg action switcher-cancel | grep -q '\"ok\"'
    \"$KC\" msg get-windows | grep -q '\"title\":\"Kajo Test Window 3\".*\"is_focused\":true'
    \"$KC\" msg action next-window hold=true | grep -q '\"ok\"'
    \"$KC\" msg action switcher-confirm | grep -q '\"ok\"'
    \"$KC\" msg get-windows | grep -q '\"title\":\"Kajo Test Window 2\".*\"is_focused\":true'

    # Exercise layout & window IPC actions end-to-end
    \"$KC\" msg action set-column-width delta=+10% | grep -q '\"ok\"'
    \"$KC\" msg action set-window-height delta=-10% | grep -q '\"ok\"'
    \"$KC\" msg action toggle-column-tabbed | grep -q '\"ok\"'
    \"$KC\" msg action toggle-column-tabbed | grep -q '\"ok\"'
    \"$KC\" msg action consume-or-expel-window-left | grep -q '\"ok\"'
    \"$KC\" msg action toggle-window-floating | grep -q '\"ok\"'
    \"$KC\" msg get-windows | grep -q '\"title\":\"Kajo Test Window 2\"[^}]*\"is_floating\":true'
    \"$KC\" msg action toggle-window-floating | grep -q '\"ok\"'
    \"$KC\" msg action focus-app app_id=org.kajo.TestClient | grep -q '\"ok\"'

    # Test screenshot-screen, screenshot-window, and interactive screenshot (cancel + confirm)
    \"$KC\" msg action screenshot-screen | grep -q '\"ok\"'
    \"$KC\" msg action screenshot-window | grep -q '\"ok\"'
    \"$KC\" msg action screenshot | grep -q '\"ok\"'
    \"$KC\" msg action screenshot-cancel | grep -q '\"ok\"'
    \"$KC\" msg action screenshot | grep -q '\"ok\"'
    \"$KC\" msg action screenshot-confirm | grep -q '\"ok\"'

    # Wait for async PNG writer threads to emit 3 ScreenshotSaved events
    for _ in \$(seq 60); do
        COUNT=\$(grep -c 'ScreenshotSaved' '$RT/sub.log' 2>/dev/null || true)
        [ \"\$COUNT\" -ge 3 ] && break
        sleep 0.05
    done
    COUNT=\$(grep -c 'ScreenshotSaved' '$RT/sub.log' 2>/dev/null || true)
    [ \"\$COUNT\" -eq 3 ]

    # Verify all 3 saved PNG files have valid PNG magic bytes and non-zero size
    PNG_COUNT=0
    for f in '$SHOT_DIR'/*.png; do
        [ -s \"\$f\" ]
        MAGIC=\$(head -c 8 \"\$f\" | od -An -tx1 | tr -d ' \n')
        [ \"\$MAGIC\" = '89504e470d0a1a0a' ]
        PNG_COUNT=\$((PNG_COUNT + 1))
    done
    [ \"\$PNG_COUNT\" -eq 3 ]

    rm -f '$RT/ready'
    wait \"\$HOLD_PID\" || true
    kill \"\$SUB_PID\" 2>/dev/null || true
"
check "kajo-comp msg IPC, reload-config, Alt-Tab switcher, and screenshot UI" 0 $?

# Attack-surface & rogue-client resilience: dynamic rule promotion, UTF-8 title clamping,
# virtual-keyboard/IMv2 lifecycle, and 96-socket IPC flood + float-cast overflow immunity
cat > "$CFG_DIR/rules.toml" << 'EOF'
[[rule]]
app_id = ['firefox$']
title = ['^picture-in-picture$']
floating = true

[[rule]]
title = ['(X+)+Y']
floating = true
EOF

timeout 30 "$KC" -q --exit-with-command -- sh -c "
    set -e
    \"$CL\" rogue '$RT/rogue.ready' &
    ROGUE_PID=\$!
    for _ in \$(seq 50); do [ -f '$RT/rogue.ready' ] && break; sleep 0.05; done
    [ -f '$RT/rogue.ready' ]

    WINS=\$(\"${KC}\" msg get-windows)
    # 1. Verify firefox window dynamically promoted to floating on set_title('Picture-in-Picture')
    echo \"\$WINS\" | grep -q '\"title\":\"Picture-in-Picture\",\"app_id\":\"firefox\"[^}]*\"is_floating\":true'
    # 2. Verify 8192-byte title with 2-byte UTF-8 at byte 511..512 was truncated cleanly to 511 'X' chars
    python3 -c '
import json, sys
doc = json.loads(sys.argv[1])
oversized = [w for w in doc[\"ok\"][\"windows\"] if w[\"app_id\"] == \"org.kajo.Oversized\"]
assert len(oversized) == 1
assert len(oversized[0][\"title\"]) == 511
' \"\$WINS\"

    # 3. Flood IPC socket with 96 simultaneous connections + adversarial 1e300 numeric payloads
    python3 -c '
import os, socket
path = os.environ[\"KAJO_COMP_SOCKET\"]
socks = []
for _ in range(96):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.setblocking(False)
        s.connect(path)
        s.send(b\"{\\\"cmd\\\":\\\"action\\\",\\\"name\\\":\\\"focus-window\\\",\\\"id\\\":1e300}\\n\")
        s.send(b\"{\\\"cmd\\\":\\\"action\\\",\\\"name\\\":\\\"switch-workspace\\\",\\\"idx\\\":1e300}\\n\")
        socks.append(s)
    except Exception:
        pass
for s in socks:
    s.close()
'
    # Compositor must remain responsive immediately after connection flood
    \"$KC\" msg get-workspaces | grep -q '\"ok\"'

    rm -f '$RT/rogue.ready'
    wait \"\$ROGUE_PID\" || true
"
check "rogue client churn, dynamic rule promotion, UTF-8 title cap, vkbd/IMv2 & IPC flood" 0 $?
rm -f "$CFG_DIR/rules.toml"

# Phase 4: Single-output selection (OUT1-OUT4), KMS cascade, transform rotation & systemd notify/watchdog
NOTIFY_SOCK="$RT/notify.sock"
NOTIFY_LOG="$RT/notify.log"
python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind('$NOTIFY_SOCK')
s.settimeout(10.0)
with open('$NOTIFY_LOG', 'w') as f:
    while True:
        try:
            data = s.recv(1024).decode('utf-8', 'replace')
            f.write(data + '\n')
            f.flush()
            if 'STOPPING=1' in data:
                break
        except Exception:
            break
" &
NPID=$!
for _ in $(seq 30); do [ -S "$NOTIFY_SOCK" ] && break; sleep 0.02; done

rm -f "$CFG_DIR/outputs.toml"
WLR_HEADLESS_OUTPUTS=2 NOTIFY_SOCKET="$NOTIFY_SOCK" KAJO_WATCHDOG_MS=40 KAJO_COMP_NO_DBUS_ENV=1 \
    timeout 30 "$KC" -q --session --exit-with-command -- sh -c "
    set -e
    OUTS=\$(\"${KC}\" msg get-outputs)
    echo \"\$OUTS\" | grep -q '\"name\":\"HEADLESS-2\"[^}]*\"active\":true'
    echo \"\$OUTS\" | grep -q '\"name\":\"HEADLESS-1\"[^}]*\"active\":false'

    # Switch primary output via [[output]] priority (OUT2)
    printf '[[output]]\nmatch = \"HEADLESS-1\"\n' > '$CFG_DIR/outputs.toml'
    \"$KC\" msg action reload-config | grep -q '\"ok\"'
    OUTS2=\$(\"${KC}\" msg get-outputs)
    echo \"\$OUTS2\" | grep -q '\"name\":\"HEADLESS-2\"[^}]*\"active\":false'
    echo \"\$OUTS2\" | grep -q '\"name\":\"HEADLESS-1\"[^}]*\"active\":true'

    # Posture transform rotation (OUT4 + §3.15.4)
    \"$KC\" msg action set-output-transform transform=90 | grep -q '\"ok\"'
    \"$KC\" msg get-outputs | grep -q '\"name\":\"HEADLESS-1\"[^}]*\"transform\":\"90\"'
    \"$KC\" msg action set-output-transform transform=normal | grep -q '\"ok\"'
    \"$KC\" msg get-outputs | grep -q '\"name\":\"HEADLESS-1\"[^}]*\"transform\":\"normal\"'

    # Atomic test-before-commit cascade (set-output-config)
    \"$KC\" msg action set-output-config scale=1.5 vrr=on-demand | grep -q '\"ok\"'
    \"$KC\" msg get-outputs | grep -q '\"name\":\"HEADLESS-1\"[^}]*\"vrr\":\"on-demand\"'

    # Allow watchdog timer (40ms) to fire at least once
    sleep 0.12
"
P4_RC=$?
wait "$NPID" 2>/dev/null || true
grep -q 'READY=1' "$NOTIFY_LOG" && grep -q 'WATCHDOG=1' "$NOTIFY_LOG" && grep -q 'STOPPING=1' "$NOTIFY_LOG" || P4_RC=1
rm -f "$CFG_DIR/outputs.toml"
check "Phase 4 single-output priority, transform rotation, KMS cascade & systemd watchdog" 0 $P4_RC

# ── Phase 5: Greeter Mode, EX_SOFTWARE (70) Fallback Chain & kajo-session (§6.1–6.5) ──
GREETER_SOCK="$RT/greeter-dont-create.sock"
GREETER_CFG="$RT/comp-greeter.toml"
cat > "$GREETER_CFG" << 'EOF'
[input.keyboard]
xkb_layout = "us"

[[output]]
match = "HEADLESS-1"
scale = 2.0
EOF

WLR_HEADLESS_OUTPUTS=2 KAJO_COMP_SOCKET="$GREETER_SOCK" KAJO_GREETER_DPMS_MS=60 \
    timeout 30 "$KC" -q --greeter-mode --config "$GREETER_CFG" -- sh -c "
    set -e
    # IPC socket must NEVER be created in --greeter-mode
    [ ! -e '$GREETER_SOCK' ]
    # Allow 60ms DPMS blanking timer to fire cleanly
    sleep 0.10
"
check "Phase 5 --greeter-mode multi-output, no IPC socket & DPMS idle timer" 0 $?

# Missing XDG_RUNTIME_DIR or missing config or missing greeter binary -> exit 70 (EX_SOFTWARE)
env -u XDG_RUNTIME_DIR "$KC" -q --greeter-mode -- sh -c 'exit 0' 2>/dev/null
check "Phase 5 --greeter-mode without XDG_RUNTIME_DIR exits 70" 70 $?

"$KC" -q --greeter-mode --config "$RT/nonexistent-greeter.toml" -- sh -c 'exit 0' 2>/dev/null
check "Phase 5 --greeter-mode with invalid --config exits 70" 70 $?

"$KC" -q --greeter-mode -- /nonexistent/kajo-greeter-missing 2>/dev/null
check "Phase 5 --greeter-mode with missing greeter binary exits 70" 70 $?

# Verify kajo-greeter-session fallback chain (kajo-comp -> niri -> cage) and kajo-session CLI
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FAKE_BIN="$RT/fake-bin"
mkdir -p "$FAKE_BIN"
cat > "$FAKE_BIN/kajo-comp" << 'EOF'
#!/bin/sh
exit 70
EOF
cat > "$FAKE_BIN/niri" << 'EOF'
#!/bin/sh
exit 1
EOF
cat > "$FAKE_BIN/cage" << 'EOF'
#!/bin/sh
echo "cage-fallback:$*" > "$KAJO_FALLBACK_MARKER"
exit 0
EOF
chmod +x "$FAKE_BIN/kajo-comp" "$FAKE_BIN/niri" "$FAKE_BIN/cage"

KAJO_FALLBACK_MARKER="$RT/fallback.out" PATH="$FAKE_BIN:$PATH" \
    sh "$REPO_ROOT/data/greetd/kajo-greeter-session" --cmd kajo-session 2>/dev/null
FB_RC=$?
grep -q "cage-fallback:-s -- kajo-greeter" "$RT/fallback.out" || FB_RC=1
check "Phase 5 kajo-greeter-session 3-stage fallback chain" 0 $FB_RC

bash "$REPO_ROOT/data/session/kajo-session" --help >/dev/null
check "Phase 5 kajo-session --help" 0 $?

timeout 60 "$KC" -q --exit-with-command -- "$CL" churn 200
check "200 connect/disconnect cycles" 0 $?

timeout 30 "$KC" -q --exit-with-command -- sh -c 'exit 7'
check "command exit status is propagated" 7 $?

timeout 30 "$KC" -q --exit-with-command -- /nonexistent/kajo-comp-test-binary
check "missing command exits 127" 127 $?

# Clients run in their own session and outlive the compositor by design (spec §7.5.4),
# so the child must not hold the test's stdout open and is killed afterwards.
marker="$RT/child.pid"
timeout 30 "$KC" -q -- sh -c "echo \$\$ > '$marker'; exec sleep 60" >/dev/null 2>&1 &
pid=$!
for _ in $(seq 50); do [ -s "$marker" ] && break; sleep 0.1; done
kill -TERM "$pid"
wait "$pid"
check "SIGTERM shuts down with status 0" 0 $?
child="$(cat "$marker" 2>/dev/null)"
if [ -n "$child" ] && kill -0 "$child" 2>/dev/null; then
    echo "ok - client survives compositor shutdown (own session)"
    kill "$child"
else
    echo "not ok - client did not survive compositor shutdown"; fail=1
fi

exit $fail
