#!/bin/sh
# Remove only this setup's configuration and tagged rules. Keys are kept.
set -eu

if [ "$(id -u)" != 0 ]; then
    exec sudo -p '[tunnel] sudo password:
' sh "$0" "$@"
fi

WG_DIR=/etc/wireguard
MARKER='managed by the tunnel setup'
[ "$#" = 0 ] || {
    echo 'teardown reads installed state; no port arguments are accepted' >&2
    exit 2
}

# Check every owned file before stopping services or removing anything.
for file in "$WG_DIR/wg0.conf" "$WG_DIR/wg0-forward.sh" /etc/sysctl.d/99-wg0-forward.conf; do
    if [ -e "$file" ] || [ -L "$file" ]; then
        [ ! -L "$file" ] && grep -q "^# $MARKER" "$file" || {
            echo "refusing to remove unowned $file" >&2
            exit 1
        }
    fi
done
if [ ! -f "$WG_DIR/wg0.conf" ] && [ ! -f "$WG_DIR/wg0-forward.sh" ]; then
    echo TEARDOWN_OK=1
    exit 0
fi
mkdir "$WG_DIR/.tunnel-lock" 2>/dev/null || {
    echo 'another operation is running, or its lock needs inspection' >&2
    exit 1
}
trap 'rmdir "$WG_DIR/.tunnel-lock"' 0
trap 'exit 1' HUP INT TERM

# Stop the tunnel while its PostDown hook and saved port lists still exist.
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    if systemctl is-active --quiet wg-quick@wg0; then
        systemctl stop wg-quick@wg0
    fi
    if systemctl is-enabled --quiet wg-quick@wg0; then
        systemctl disable wg-quick@wg0
    fi
elif command -v rc-service >/dev/null 2>&1 && [ -f /etc/init.d/wg-quick.wg0 ]; then
    if rc-service wg-quick.wg0 status >/dev/null 2>&1; then
        rc-service wg-quick.wg0 stop
    fi
    rc-update del wg-quick.wg0 default
    rm -f /etc/init.d/wg-quick.wg0
fi
if wg show wg0 >/dev/null 2>&1; then
    wg-quick down wg0
fi
[ ! -f "$WG_DIR/wg0-forward.sh" ] || sh "$WG_DIR/wg0-forward.sh" down
rm -f "$WG_DIR/wg0.conf" "$WG_DIR/wg0-forward.sh"

# Restore the forwarding setting recorded by the first successful installation.
if [ -f /etc/sysctl.d/99-wg0-forward.conf ]; then
    grep -q "^# $MARKER" /etc/sysctl.d/99-wg0-forward.conf || exit 1
    rm -f /etc/sysctl.d/99-wg0-forward.conf
fi
if [ -f "$WG_DIR/wg0-ip-forward.before" ]; then
    before=$(cat "$WG_DIR/wg0-ip-forward.before")
    case "$before" in
        0|1) sysctl -q -w "net.ipv4.ip_forward=$before" ;;
        *) exit 1 ;;
    esac
    rm -f "$WG_DIR/wg0-ip-forward.before"
fi
echo TEARDOWN_OK=1
