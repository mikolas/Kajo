#!/usr/bin/env bash
# Render a Kajo GTK app headlessly and save a screenshot, without touching your
# real session. Uses a nested `cage` compositor on wlroots' headless backend
# (own Wayland socket, 1280x720 output) and `grim` to capture it.
#
#   scripts/ui-shot.sh [-o out.png] [-d seconds] [-k] <binary> [args...]
#   scripts/ui-shot.sh -o /tmp/ide.png builddir/kajo-settings --page ide
#
# Isolation (do not remove these; they exist because live-session side effects
# have happened before):
#   * private XDG_RUNTIME_DIR, so no connection to your compositor/shell sockets
#   * NIRI_SOCKET / WAYLAND_DISPLAY / DISPLAY unset: Niri actions (float toggle,
#     config reload) and `kajo --lock` style IPC can never reach the real Niri
#   * XDG_CONFIG_HOME / XDG_DATA_HOME point at throwaway COPIES of
#     ~/.config/kajo and ~/.local/share/kajo; the repo's data/defaults/style.css
#     is copied over the CSS so the shot shows the working tree (use -k to keep
#     the copied user CSS instead)
#   * the D-Bus SESSION and SYSTEM buses are the real ones (NetworkManager,
#     BlueZ, UPower, PipeWire are read as usual). Do not use dbus-run-session
#     here: it D-Bus-activates your installed services inside the private bus and
#     leaves FUSE mounts and stray daemons behind. Nothing is clicked, so pages
#     only read; avoid apps that register session-bus names you depend on.
# Never point this at the `kajo` shell binary or at kajo-greeter without --test.
set -euo pipefail

out="/tmp/kajo-shot.png"
delay=3.5
keep_css=0
while getopts "o:d:k" opt; do
    case "$opt" in
        o) out="$OPTARG" ;;
        d) delay="$OPTARG" ;;
        k) keep_css=1 ;;
        *) echo "usage: $0 [-o out.png] [-d seconds] [-k] <binary> [args...]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || { echo "usage: $0 [-o out.png] [-d seconds] [-k] <binary> [args...]" >&2; exit 2; }

for tool in cage grim; do
    command -v "$tool" >/dev/null || { echo "missing dependency: $tool" >&2; exit 1; }
done

src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="$1"; shift
case "$bin" in /*) ;; *) bin="$PWD/$bin" ;; esac
[ -x "$bin" ] || { echo "not executable: $bin" >&2; exit 1; }
case "$(basename "$bin")" in
    kajo) echo "refusing to run the 'kajo' shell here: it owns session services" >&2; exit 1 ;;
esac

work="$(mktemp -d /tmp/kajo-shot.XXXXXX)"
mkdir -p "$work/runtime" "$work/config" "$work/data"
chmod 700 "$work/runtime"
[ -d "$HOME/.config/kajo" ] && cp -r "$HOME/.config/kajo" "$work/config/kajo" || mkdir -p "$work/config/kajo"
[ -d "$HOME/.local/share/kajo" ] && cp -r "$HOME/.local/share/kajo" "$work/data/kajo" || mkdir -p "$work/data/kajo/defaults"
if [ "$keep_css" -eq 0 ]; then
    mkdir -p "$work/data/kajo/defaults"
    cp "$src_dir/data/defaults/style.css" "$work/config/kajo/style.css"
    cp "$src_dir/data/defaults/style.css" "$work/data/kajo/defaults/style.css"
fi

cage_pid=""
cleanup() {
    [ -n "$cage_pid" ] && kill "$cage_pid" 2>/dev/null || true
    sleep 0.3
    pkill -P "$cage_pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

export XDG_RUNTIME_DIR="$work/runtime" XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data"
unset NIRI_SOCKET WAYLAND_DISPLAY DISPLAY
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
export GSK_RENDERER=cairo LIBGL_ALWAYS_SOFTWARE=1

cage -- "$bin" "$@" >"$work/app.log" 2>&1 &
cage_pid=$!

sock=""
for _ in $(seq 1 40); do
    sleep 0.25
    sock="$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1 || true)"
    [ -n "$sock" ] && break
done
[ -n "$sock" ] || { echo "cage did not start; log:" >&2; cat "$work/app.log" >&2; exit 1; }

sleep "$delay"
WAYLAND_DISPLAY="$sock" grim "$out"
echo "saved $out ($(stat -c %s "$out") bytes, 1280x720)"
grep -iE "critical|assertion|segfault|runtime error" "$work/app.log" | grep -v "Lost connection" \
    && echo "^^ GTK/GLib problems were logged while rendering" >&2 || true
