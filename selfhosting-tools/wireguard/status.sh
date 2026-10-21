#!/bin/sh
# lemon-chat selfhosting-tools/wireguard/status.sh - run on the Linux machine that hosts the server.
# shows the tunnel on this machine, then the router side (router/status.sh over ssh) and a tcp check of the
# websocket port through the router. read-only.

set -u
. "$(dirname "$0")/common.sh"
load_answers

if [ -z "$ROUTER_IP" ]; then
    fail "no saved answers (selfhosting-tools/wireguard/answers.json); run setup.sh first"
    exit 1
fi

# a port changed in server_settings.json after the setup is not forwarded until the setup runs again
if [ -f "$WG_SERVER_SETTINGS" ]; then
    CURRENT_WS="$(settings_get websocket_port "")"
    CURRENT_HTTP="$(settings_get http_port "")"
    CURRENT_RANGE="$(settings_get webrtc_port_range_begin "")-$(settings_get webrtc_port_range_end "")"
    [ "$CURRENT_WS" != "$WS_PORT" ] && warn "websocket port is $CURRENT_WS in the settings, forwarding was set up for $WS_PORT - run setup.sh again"
    [ "$(settings_get is_client_http_enabled false)" = "True" ] && [ "$CURRENT_HTTP" != "$HTTP_PORT" ] && warn "http port is $CURRENT_HTTP in the settings, forwarding was set up for $HTTP_PORT - run setup.sh again"
    [ "$CURRENT_RANGE" != "$UDP_RANGE" ] && warn "udp range is $CURRENT_RANGE in the settings, forwarding was set up for $UDP_RANGE - run setup.sh again"
    [ "$(settings_get is_wireguard_mode_active false)" != "True" ] && warn "is_wireguard_mode_active is off in the settings; voice and video will not work through the router"
fi

printf '\033[36m== tunnel %s on this machine ==\033[0m\n' "$WG_TUNNEL"
if command -v wg >/dev/null 2>&1; then
    as_root wg show "$WG_TUNNEL" 2>/dev/null || echo "tunnel is not up"
    AGE="$(handshake_age)"
    if [ "$AGE" -ge 0 ]; then
        echo "last handshake: $AGE s ago"
    else
        echo "no handshake"
    fi
else
    warn "wireguard-tools is not installed here"
fi

echo
printf '\033[36m== router %s ==\033[0m\n' "$ROUTER_IP"
remote_script status.sh

echo
printf '\033[36m== reachability ==\033[0m\n'
HOST_TUNNEL_IP="$(echo "$SUBNET" | awk -F'[./]' '{print $1"."$2"."$3".2"}')"
if remote_script check.sh "$HOST_TUNNEL_IP" "$WS_PORT" | grep -q TUNNEL_TCP_OK; then
    echo "through the tunnel, router -> tcp $HOST_TUNNEL_IP:$WS_PORT (websocket): answers"
else
    echo "through the tunnel, router -> tcp $HOST_TUNNEL_IP:$WS_PORT (websocket): no answer (server not running, or the tunnel is down)"
fi
if [ "$MODE" = "ports" ]; then
    if command -v nc >/dev/null 2>&1; then
        if nc -z -w 3 "$ROUTER_IP" "$WS_PORT" 2>/dev/null; then
            echo "from outside, tcp $ROUTER_IP:$WS_PORT (websocket): answers"
        else
            echo "from outside, tcp $ROUTER_IP:$WS_PORT (websocket): no answer (forwarding down, or the provider's firewall)"
        fi
        if [ "$HTTP_PORT" != "0" ]; then
            if nc -z -w 3 "$ROUTER_IP" "$HTTP_PORT" 2>/dev/null; then
                echo "from outside, tcp $ROUTER_IP:$HTTP_PORT (http): answers"
            else
                echo "from outside, tcp $ROUTER_IP:$HTTP_PORT (http): no answer"
            fi
        fi
    else
        echo "nc is not installed, the outside checks were skipped"
    fi
else
    echo "full tunnel mode: the public address cannot be checked from this machine (its own packets travel through the tunnel); test from another network"
fi
echo "udp ports cannot be probed from here; a voice call between two clients is the test for 3478 and $UDP_RANGE"
