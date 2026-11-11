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
unset WAYLAND_DISPLAY DISPLAY WAYLAND_SOCKET
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
    zwlr_screencopy_manager_v1 ext_image_copy_capture_manager_v1 \
    ext_output_image_capture_source_manager_v1 \
    ext_foreign_toplevel_image_capture_source_manager_v1 \
    zwlr_foreign_toplevel_manager_v1 ext_foreign_toplevel_list_v1 \
    wp_alpha_modifier_v1
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
include_border = true
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
