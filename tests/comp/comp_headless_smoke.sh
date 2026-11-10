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
    wp_fractional_scale_manager_v1 wp_presentation
check "required globals advertised" 0 $?

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
