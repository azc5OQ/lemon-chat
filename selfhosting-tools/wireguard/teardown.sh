#!/bin/sh
# lemon-chat selfhosting-tools/wireguard/teardown.sh - run on the Linux machine that hosts the server.
# removes the tunnel here, undoes the router side (router/teardown.sh over ssh) and switches the server's
# wireguard mode off in server_settings.json. the saved answers and the key pairs stay.

set -eu
. "$(dirname "$0")/common.sh"
load_answers

if [ -z "$ROUTER_IP" ]; then
    fail "no saved answers (selfhosting-tools/wireguard/answers.json); nothing to remove"
    exit 1
fi

echo "this removes the WireGuard tunnel on this machine and on $ROUTER_IP, and the port forwarding."
ask_yes_no "Continue?" no || exit 0

step "removing the tunnel $WG_TUNNEL"
local_tunnel_owned
local_tunnel_stop
if [ "$(as_root readlink "/etc/init.d/wg-quick.$WG_TUNNEL" 2>/dev/null || :)" = /etc/init.d/wg-quick ]; then
    as_root rm -f "/etc/init.d/wg-quick.$WG_TUNNEL"
fi
as_root sh "$WG_HERE/host-firewall.sh" down
as_root rm -f "$WG_CONF" /etc/wireguard/lemon-wg0-firewall.sh /etc/wireguard/lemon-wg0-validate.sh /etc/wireguard/lemon-wg0-ports

step "undoing the router side"
OUTPUT=$(remote_script teardown.sh) || {
    fail "router teardown failed; saved answers were retained for retry"
    exit 1
}
printf '%s\n' "$OUTPUT"
printf '%s\n' "$OUTPUT" | grep -q '^TEARDOWN_OK=1' || exit 1

settings_set "is_wireguard_mode_active=false"
step "server_settings.json: wireguard mode off. restart the chat server"

printf '\033[32mdone. selfhosting-tools/wireguard/answers.json, selfhosting-tools/wireguard/keys and selfhosting-tools/wireguard/ssh were kept; delete them by hand if wanted.\033[0m\n'
