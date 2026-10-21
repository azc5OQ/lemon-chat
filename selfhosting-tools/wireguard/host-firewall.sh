#!/bin/sh
# written by lemon-chat selfhosting-tools/wireguard/setup.sh
# Linux host firewall hook: up [TCP_PORTS UDP_PORTS] or down.
# Setup saves the port lists; later wg-quick hooks reuse them after a reboot.
set -eu

DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
if [ -f "$DIR/router/validate.sh" ]; then
    . "$DIR/router/validate.sh"
else
    . "$DIR/lemon-wg0-validate.sh"
fi

STATE=/etc/wireguard/lemon-wg0-ports
ZONE=/etc/firewalld/zones/lemon-wg0.xml
ACTION=$1
case "$ACTION" in up|down) ;; *) exit 2 ;; esac

# Remove the previous ports before replacing the saved list.
if [ "$ACTION" = up ] && [ "$#" = 3 ]; then
    valid_ports "$2" && valid_ports "$3" || exit 2
    if [ -f "$STATE" ]; then
        sh "$0" down
    fi
    umask 077
    printf '%s\n%s\n' "$2" "$3" > "$STATE.new"
    mv "$STATE.new" "$STATE"
fi
[ -f "$STATE" ] || exit 0
TCP_PORTS=$(sed -n '1p' "$STATE")
UDP_PORTS=$(sed -n '2p' "$STATE")
valid_ports "$TCP_PORTS" && valid_ports "$UDP_PORTS" || exit 2

# Use a dedicated zone on firewalld systems; otherwise use tagged INPUT rules.
if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    if [ -e "$ZONE" ]; then
        grep -Eq '^<!-- written by lemon-chat (selfhosting-tools|tools)/wireguard/setup\.sh -->$' "$ZONE" || exit 1
    fi
    if [ "$ACTION" = up ]; then
        current=$(firewall-cmd --get-zone-of-interface=lemon-wg0 || :)
        case "$current" in
            ''|'no zone'|lemon-wg0) ;;
            *)
                echo 'tunnel already belongs to another firewall zone' >&2
                exit 1
                ;;
        esac
        mkdir -p /etc/firewalld/zones
        {
            printf '%s\n' '<?xml version="1.0" encoding="utf-8"?>' '<!-- written by lemon-chat selfhosting-tools/wireguard/setup.sh -->' '<zone><interface name="lemon-wg0"/>'
            for proto in tcp udp; do
                if [ "$proto" = tcp ]; then
                    ports=$TCP_PORTS
                else
                    ports=$UDP_PORTS
                fi
                for port in $(printf '%s' "$ports" | tr ',' ' '); do
                    port=$(printf '%s' "$port" | tr ':' '-')
                    printf '<port port="%s" protocol="%s"/>\n' "$port" "$proto"
                done
            done
            printf '%s\n' '</zone>'
        } > "$ZONE"
        firewall-cmd --check-config
    else
        rm -f "$ZONE"
    fi
    firewall-cmd --reload
else
    for proto in tcp udp; do
        if [ "$proto" = tcp ]; then
            ports=$TCP_PORTS
        else
            ports=$UDP_PORTS
        fi
        for port in $(printf '%s' "$ports" | tr ',' ' '); do
            set -- -i lemon-wg0 -p "$proto" --dport "$port" -m comment --comment lemon-wg0 -j ACCEPT
            if [ "$ACTION" = up ]; then
                iptables -w 10 -C INPUT "$@" 2>/dev/null || iptables -w 10 -I INPUT 1 "$@"
            elif iptables -w 10 -C INPUT "$@" 2>/dev/null; then
                iptables -w 10 -D INPUT "$@"
            fi
        done
    done
fi
