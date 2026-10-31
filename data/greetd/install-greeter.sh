#!/usr/bin/env bash
# install-greeter.sh — Installs kajo-greeter, kajo-greeter-session, niri-greeter.kdl,
# and system theme/icon assets for the greetd display manager.
#
# Usage:
#   ./data/greetd/install-greeter.sh              # Install files to /usr/local & /etc/greetd
#   ./data/greetd/install-greeter.sh --enable     # Install files AND activate in /etc/greetd/config.toml
#   ./data/greetd/install-greeter.sh --restore    # Restore /etc/greetd/config.toml.bak

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ACTION="install"
DEFAULT_CMD="niri-session"

while [ $# -gt 0 ]; do
    case "$1" in
        --enable|-e)
            ACTION="enable"
            shift
            ;;
        --restore|--uninstall|-r)
            ACTION="restore"
            shift
            ;;
        --cmd|-c)
            DEFAULT_CMD="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [--enable] [--cmd <session-cmd>] [--restore]"
            echo ""
            echo "Options:"
            echo "  --enable, -e       Also update /etc/greetd/config.toml (backs up to config.toml.bak)"
            echo "  --cmd, -c <CMD>    Default session command (default: niri-session)"
            echo "  --restore, -r      Restore /etc/greetd/config.toml from config.toml.bak"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

# Build as normal user first if builddir/kajo-greeter is missing
if [ "$ACTION" != "restore" ] && [ ! -x "$PROJECT_ROOT/builddir/kajo-greeter" ]; then
    echo "==> Building kajo-greeter in $PROJECT_ROOT/builddir..."
    meson compile -C "$PROJECT_ROOT/builddir" kajo-greeter
fi

# Escalate to root via sudo if not already root
if [ "$(id -u)" -ne 0 ]; then
    echo "==> Elevating privileges with sudo..."
    exec sudo "$0" "$@"
fi

if [ "$ACTION" = "restore" ]; then
    if [ -f /etc/greetd/config.toml.bak ]; then
        cp -a /etc/greetd/config.toml.bak /etc/greetd/config.toml
        echo "==> Restored /etc/greetd/config.toml from /etc/greetd/config.toml.bak"
    else
        echo "No backup found at /etc/greetd/config.toml.bak" >&2
        exit 1
    fi
    exit 0
fi

echo "==> Installing kajo-greeter binaries to /usr/local/bin..."
install -Dm755 "$PROJECT_ROOT/builddir/kajo-greeter" /usr/local/bin/kajo-greeter
install -Dm755 "$PROJECT_ROOT/data/greetd/kajo-greeter-session" /usr/local/bin/kajo-greeter-session

echo "==> Installing niri-greeter.kdl to /etc/greetd/ and /usr/local/share/kajo/greetd/..."
install -Dm644 "$PROJECT_ROOT/data/greetd/niri-greeter.kdl" /etc/greetd/niri-greeter.kdl
install -Dm644 "$PROJECT_ROOT/data/greetd/niri-greeter.kdl" /usr/local/share/kajo/greetd/niri-greeter.kdl

echo "==> Installing default CSS theme & icons to /usr/local/share/kajo/..."
install -Dm644 "$PROJECT_ROOT/data/defaults/style.css" /usr/local/share/kajo/defaults/style.css
install -Dm644 "$PROJECT_ROOT/data/defaults/config.json" /usr/local/share/kajo/defaults/config.json
install -Dm644 "$PROJECT_ROOT/data/defaults/icon-map.json" /usr/local/share/kajo/defaults/icon-map.json
install -dm755 /usr/local/share/kajo/icons
cp -r "$PROJECT_ROOT/data/icons/"* /usr/local/share/kajo/icons/

echo "==> Setting up /var/cache/kajo-greeter state directory..."
if id -u greeter >/dev/null 2>&1; then
    install -dm755 -o greeter -g greeter /var/cache/kajo-greeter
else
    install -dm755 /var/cache/kajo-greeter
fi

if [ "$ACTION" = "enable" ] && [ -f /etc/greetd/config.toml ]; then
    if [ ! -f /etc/greetd/config.toml.bak ]; then
        cp -a /etc/greetd/config.toml /etc/greetd/config.toml.bak
        echo "==> Backed up /etc/greetd/config.toml -> /etc/greetd/config.toml.bak"
    fi

    # Replace active command = "..." line under [default_session]
    sed -i -E "s|^([[:space:]]*command[[:space:]]*=.*)$|# \1\ncommand = \"kajo-greeter-session --cmd ${DEFAULT_CMD}\"|" /etc/greetd/config.toml
    echo "==> Activated kajo-greeter-session in /etc/greetd/config.toml"
else
    echo ""
    echo "==> Installation complete!"
    echo "To activate in /etc/greetd/config.toml, either run:"
    echo "  ./data/greetd/install-greeter.sh --enable"
    echo "or set in /etc/greetd/config.toml under [default_session]:"
    echo "  command = \"kajo-greeter-session --cmd ${DEFAULT_CMD}\""
fi
