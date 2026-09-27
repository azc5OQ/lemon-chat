#!/bin/sh
# lemon-chat tools/wireguard/setup.sh - run on the LINUX machine that hosts the lemon-chat server (a pc, a pi, ...).
#
# turns a VPS into a router for this machine: asks a few questions, sets WireGuard up on the VPS over ssh
# (router/setup.sh), brings the tunnel up here with wg-quick, forwards the chat ports from the router's public
# address through the tunnel, and switches the server's wireguard mode on in server_settings.json.
# see README.md in this directory. run again to change answers; the saved answers are the defaults.
#
# needs: sudo, ssh, wireguard-tools (installed here when missing), python3 for the settings files.

set -eu
. "$(dirname "$0")/common.sh"
command -v python3 >/dev/null 2>&1 || {
    fail "python3 is required"
    exit 1
}
load_answers

if ! command -v ssh >/dev/null 2>&1; then
    fail "ssh is not installed"
    exit 1
fi
if ! command -v sudo >/dev/null 2>&1 && [ "$(id -u)" != "0" ]; then
    fail "sudo is needed to write /etc/wireguard"
    exit 1
fi
if [ ! -f "$WG_SERVER_SETTINGS" ]; then
    fail "buildresult/server_settings.json is missing; complete server setup first"
    exit 1
fi

python3 -c 'import json,sys; data=json.load(open(sys.argv[1])); assert isinstance(data,dict), "settings must be an object"' "$WG_SERVER_SETTINGS"

echo
printf '\033[32mlemon-chat: expose this machine'"'"'s server through a VPS (the router) over WireGuard\033[0m\n'
echo "every answer shows its default in brackets; Enter keeps it. answers are saved to tools/wireguard/answers.json (never a password)."
echo

# Collect connection details and ports before changing either machine.
ROUTER_IP="$(ask "router (VPS) public IPv4 address" "$ROUTER_IP")"
ROUTER_SSH_PORT="$(ask "router SSH port" "$ROUTER_SSH_PORT")"
ROUTER_SSH_USER="$(ask "router SSH user (root, or a user that may sudo)" "$ROUTER_SSH_USER")"

if [ -n "$SSH_KEY" ]; then
    auth_default=1
else
    auth_default=2
fi
auth="$(ask "SSH authentication: 1 = private key file, 2 = password typed at each ssh prompt" "$auth_default")"
if [ "$auth" = "1" ]; then
    key_default="$SSH_KEY"
    [ -z "$key_default" ] && [ -f "$HOME/.ssh/id_ed25519" ] && key_default="$HOME/.ssh/id_ed25519"
    SSH_KEY="$(ask "Path to the private key file" "$key_default")"
    [ -f "$SSH_KEY" ] || { fail "key file not found: $SSH_KEY"; exit 1; }
else
    SSH_KEY=""
fi

echo
echo "tunnel mode:"
echo "  1 = ports only: only the chat ports go through the tunnel. the server sees every visitor as the router's"
echo "      tunnel address, so country flags, IP bans and the same-IP rule do not work."
echo "  2 = full tunnel: ALL of this machine's IPv4 internet traffic leaves through the router. visitors keep their"
echo "      real addresses. the router carries this machine's traffic too (its bandwidth plan matters)."
if [ "$MODE" = "full" ]; then
    mode_default=2
else
    mode_default=1
fi
mode_choice="$(ask "Tunnel mode" "$mode_default")"
if [ "$mode_choice" = "2" ]; then
    MODE="full"
else
    MODE="ports"
fi

# the ports come from the server's current settings first: a port changed there after an earlier
# setup must show up here, the saved answer only fills in when the settings file is missing
if [ -f "$WG_SERVER_SETTINGS" ]; then
    WS_PORT="$(settings_get websocket_port "${WS_PORT:-1111}")"
    if [ "$(settings_get serve_client_http false)" = "True" ]; then
        HTTP_PORT="$(settings_get http_port 8090)"
    else
        HTTP_PORT=0
    fi
    UDP_RANGE="$(settings_get webrtc_port_range_begin 20000)-$(settings_get webrtc_port_range_end 20099)"
else
    [ -z "$WS_PORT" ] && WS_PORT=1111
    [ -z "$HTTP_PORT" ] && HTTP_PORT=0
    [ -z "$UDP_RANGE" ] && UDP_RANGE="20000-20099"
fi
WS_PORT="$(ask "lemon-chat websocket port to forward" "$WS_PORT")"
HTTP_PORT="$(ask "http port of the served client page to forward (0 = the page is not served)" "$HTTP_PORT")"
while true; do
    UDP_RANGE="$(ask "UDP port range for voice and video, forwarded to this machine (BEGIN-END)" "$UDP_RANGE")"
    RANGE_BEGIN="${UDP_RANGE%-*}"
    RANGE_END="${UDP_RANGE#*-}"
    if valid_port "$RANGE_BEGIN" && valid_port "$RANGE_END" && [ "$RANGE_BEGIN" -ge 1024 ] && [ "$RANGE_END" -ge "$RANGE_BEGIN" ]; then
        break
    fi
    warn "enter two ports from 1024 to 65535 as BEGIN-END"
done

# tls ends on this machine, in the server's own stunnel; the router forwards the encrypted connections
# unchanged. the question is asked while stunnel is still off too, because the certificate can only be
# obtained once tcp 80 is forwarded
HOST_STUNNEL="$(settings_get use_stunnel false)"
HOST_WSS=0
HOST_HTTPS=0
if [ "$HOST_STUNNEL" = "True" ] || [ "$IS_TLS_FORWARDED" = "yes" ]; then
    tls_default=yes
else
    tls_default=no
fi
echo
echo "tls (wss and https) is handled on this machine by the server's own stunnel. the router forwards the"
echo "encrypted connections unchanged and holds no certificate. answer yes before the certificate exists:"
echo "its validation arrives on tcp 80, which is forwarded with the tls ports."
if ask_yes_no "Forward the tls ports too (wss, https and tcp 80)?" "$tls_default"; then
    IS_TLS_FORWARDED=yes
    wss_default="$(settings_get wss_port "$WSS_PORT_SAVED")"
    [ "$wss_default" = "0" ] && wss_default=1112
    HOST_WSS="$(ask "wss port of the server's stunnel to forward" "$wss_default")"
    https_default="$HTTPS_PORT_SAVED"
    if [ -f "$WG_SERVER_SETTINGS" ] && [ "$(settings_get serve_https false)" = "True" ]; then
        https_default="$(settings_get https_port 443)"
    fi
    HOST_HTTPS="$(ask "https port of the served client page to forward (0 = the page is not served over https)" "$https_default")"
else
    IS_TLS_FORWARDED=no
fi

WG_PORT="$(ask "WireGuard port on the router (UDP)" "$WG_PORT")"
SUBNET="$(ask "Tunnel subnet (the router is .1, this machine .2)" "$SUBNET")"
valid_subnet "$SUBNET" || {
    fail "use a canonical private IPv4 subnet containing .1 and .2 (for example 10.66.0.0/24)"
    exit 1
}
SUBNET_BASE=${SUBNET%.*}
SUBNET_PREFIX=${SUBNET#*/}
ROUTER_TUNNEL_IP="$SUBNET_BASE.1"
HOST_TUNNEL_IP="$SUBNET_BASE.2"

TCP_PORTS="$WS_PORT"
[ "$HTTP_PORT" != "0" ] && TCP_PORTS="$TCP_PORTS,$HTTP_PORT"

if [ "$IS_TLS_FORWARDED" = "yes" ]; then
    TCP_PORTS="$TCP_PORTS,$HOST_WSS"
    [ "$HOST_HTTPS" != "0" ] && TCP_PORTS="$TCP_PORTS,$HOST_HTTPS"
    # the certificate is validated over tcp 80 (certbot)
    TCP_PORTS="$TCP_PORTS,80"
fi
# a port named twice (an http port of 80, for example) is forwarded once
TCP_PORTS="$(echo "$TCP_PORTS" | tr ',' '\n' | awk '!seen[$0]++' | paste -sd, -)"
UDP_PORTS="3478,$RANGE_BEGIN:$RANGE_END"
SSH_PORT=$ROUTER_SSH_PORT
valid_ipv4 "$ROUTER_IP" && valid_name "$ROUTER_SSH_USER" && validate_tunnel || {
    fail "invalid address/ports, or forwarded ports overlap SSH/WireGuard"
    exit 1
}

echo
echo "summary:"
echo "  router ............. $ROUTER_SSH_USER@$ROUTER_IP port $ROUTER_SSH_PORT $([ -n "$SSH_KEY" ] && echo '(key)' || echo '(password)')"
echo "  mode ............ $MODE"
echo "  tcp forwarded ... $TCP_PORTS"
echo "  udp forwarded ... $UDP_PORTS"
echo "  tls ............. $([ "$IS_TLS_FORWARDED" = "yes" ] && echo "ends on this machine (stunnel); wss $HOST_WSS, https $HOST_HTTPS and tcp 80 are forwarded" || echo "not forwarded")"
echo "  tunnel .......... $ROUTER_TUNNEL_IP <-> $HOST_TUNNEL_IP, udp $WG_PORT"
ask_yes_no "Continue?" yes || exit 0

# ---- 1. wireguard on this machine ----

if ! command -v wg >/dev/null 2>&1 || ! command -v wg-quick >/dev/null 2>&1 || ! command -v iptables >/dev/null 2>&1; then
    step "installing wireguard-tools"
    if command -v apt-get >/dev/null 2>&1; then
        as_root apt-get install -y wireguard-tools iptables
    elif command -v dnf >/dev/null 2>&1; then
        as_root dnf install -y wireguard-tools iptables
    elif command -v pacman >/dev/null 2>&1; then
        as_root pacman -S --noconfirm --needed wireguard-tools iptables
    elif command -v apk >/dev/null 2>&1; then
        as_root apk add wireguard-tools iptables
    else
        fail "install wireguard-tools by hand and run again"
        exit 1
    fi
fi

local_tunnel_owned

# ---- 2. this machine's key pair ----

mkdir -p "$WG_KEYS_DIR"
chmod 700 "$WG_KEYS_DIR"
if [ ! -f "$WG_KEYS_DIR/host.key" ]; then
    step "generating this machine's WireGuard key pair"
    (
        umask 077
        wg genkey > "$WG_KEYS_DIR/host.key.new" &&
            mv "$WG_KEYS_DIR/host.key.new" "$WG_KEYS_DIR/host.key"
    )
fi
HOST_PRIVATE_KEY="$(cat "$WG_KEYS_DIR/host.key")"
HOST_PUBLIC_KEY="$(wg pubkey < "$WG_KEYS_DIR/host.key")"
printf '%s\n' "$HOST_PUBLIC_KEY" > "$WG_KEYS_DIR/host.pub"

# ---- 3. ssh access ----

if [ -z "$SSH_KEY" ]; then
    echo
    if ask_yes_no "Create a key for this setup and install it on the router, so the password is asked only once?" yes; then
        mkdir -p "$WG_SSH_DIR"
        chmod 700 "$WG_SSH_DIR"
        [ -f "$WG_SSH_DIR/lemon_router_key" ] || ssh-keygen -q -t ed25519 -N "" -C "tunnel-host" -f "$WG_SSH_DIR/lemon_router_key"
        step "installing the key on the router (the router password is asked now)"
        if ssh -p "$ROUTER_SSH_PORT" -o StrictHostKeyChecking=accept-new "$ROUTER_SSH_USER@$ROUTER_IP" "umask 077; mkdir -p ~/.ssh; cat >> ~/.ssh/authorized_keys" < "$WG_SSH_DIR/lemon_router_key.pub"; then
            SSH_KEY="$WG_SSH_DIR/lemon_router_key"

        else
            warn "the key could not be installed; continuing with password prompts"
        fi
    fi
fi

step "checking ssh access to the router"
if ! remote_run "echo SSH_OK" | grep -q SSH_OK; then
    fail "ssh to $ROUTER_SSH_USER@$ROUTER_IP port $ROUTER_SSH_PORT failed"
    exit 1
fi

# ---- 4. the router side ----

step "setting the router up (packages, keys, tunnel, port forwarding)"
OUTPUT=$(remote_script setup.sh \
    --mode "$MODE" \
    --host-pubkey "$HOST_PUBLIC_KEY" \
    --wg-port "$WG_PORT" \
    --ssh-port "$ROUTER_SSH_PORT" \
    --subnet "$SUBNET" \
    --router-ip "$ROUTER_TUNNEL_IP" \
    --host-ip "$HOST_TUNNEL_IP" \
    --tcp-ports "$TCP_PORTS" \
    --udp-ports "$UDP_PORTS") || {
    fail "router setup failed"
    exit 1
}
printf '%s\n' "$OUTPUT"

OUTPUT=$(printf '%s\n' "$OUTPUT" | tr -d '\r')
ROUTER_PUBLIC_KEY="$(echo "$OUTPUT" | sed -n 's/^WG_PUBLIC_KEY=//p' | tail -n 1)"
if ! echo "$OUTPUT" | grep -q "^SETUP_OK=1" || [ -z "$ROUTER_PUBLIC_KEY" ]; then
    fail "the router setup did not finish; read the output above"
    exit 1
fi

answers_save \
    "router_ip=$ROUTER_IP" \
    "router_ssh_port=$ROUTER_SSH_PORT" \
    "router_ssh_user=$ROUTER_SSH_USER" \
    "ssh_key=$SSH_KEY" \
    "mode=$MODE" \
    "ws_port=$WS_PORT" \
    "http_port=$HTTP_PORT" \
    "udp_range=$UDP_RANGE" \
    "is_tls_forwarded=$IS_TLS_FORWARDED" \
    "wss_port=$HOST_WSS" \
    "https_port=$HOST_HTTPS" \
    "wg_port=$WG_PORT" \
    "subnet=$SUBNET" \
    "tcp_ports=$TCP_PORTS" \
    "udp_ports=$UDP_PORTS"

# ---- 5. the tunnel on this machine ----

# full tunnel: wg-quick routes everything through the router and keeps the lan reachable through its own
# policy routing. ports only: just the tunnel subnet
ALLOWED_IPS="$SUBNET"
[ "$MODE" = "full" ] && ALLOWED_IPS="0.0.0.0/0"

as_root mkdir -p /etc/wireguard
as_root chmod 700 /etc/wireguard
local_tunnel_owned
local_tunnel_stop
as_root sh "$WG_HERE/host-firewall.sh" up "$TCP_PORTS" "$UDP_PORTS"
as_root cp "$WG_HERE/host-firewall.sh" /etc/wireguard/lemon-wg0-firewall.sh
as_root cp "$WG_HERE/router/validate.sh" /etc/wireguard/lemon-wg0-validate.sh
as_root chmod 600 /etc/wireguard/lemon-wg0-firewall.sh /etc/wireguard/lemon-wg0-validate.sh
as_root sh -c 'umask 077; cat > "$1.new"' sh "$WG_CONF" <<EOF
# written by lemon-chat tools/wireguard/setup.sh - the tunnel to the VPS that acts as the router
[Interface]
PrivateKey = $HOST_PRIVATE_KEY
Address = $HOST_TUNNEL_IP/$SUBNET_PREFIX
PostUp = sh /etc/wireguard/lemon-wg0-firewall.sh up
PostDown = sh /etc/wireguard/lemon-wg0-firewall.sh down

[Peer]
PublicKey = $ROUTER_PUBLIC_KEY
Endpoint = $ROUTER_IP:$WG_PORT
AllowedIPs = $ALLOWED_IPS
PersistentKeepalive = 25
EOF
as_root mv "$WG_CONF.new" "$WG_CONF"

step "bringing the tunnel $WG_TUNNEL up"
local_tunnel_start

step "waiting for the handshake"
AGE=-1
attempt=0
while [ "$attempt" -lt 30 ]; do
    attempt=$((attempt + 1))
    sleep 1
    AGE="$(handshake_age)"
    [ "$AGE" -ge 0 ] && break
done
if [ "$AGE" -lt 0 ]; then
    warn "no handshake within 30 s. check that udp $WG_PORT is open in the VPS provider's firewall, then run status.sh"
else
    step "tunnel is up (handshake $AGE s ago)"
fi

# ---- 6. the server's settings ----

set -- "is_wireguard_mode_active=true" \
    "webrtc_public_address=\"$ROUTER_IP\"" \
    "webrtc_port_range_begin=$RANGE_BEGIN" \
    "webrtc_port_range_end=$RANGE_END"
if [ "$MODE" = "ports" ]; then
    set -- "$@" "is_same_ip_address_allowed=true"
fi
settings_set "$@"
step "server_settings.json updated: wireguard mode on, public address $ROUTER_IP, udp $RANGE_BEGIN-$RANGE_END. restart the chat server"

# ---- 7. checks ----

# through the tunnel first: proves the tunnel passes traffic and the server listens (the server must run)
if remote_script check.sh "$HOST_TUNNEL_IP" "$WS_PORT" | grep -q TUNNEL_TCP_OK; then
    step "the router reaches the chat server through the tunnel (tcp $HOST_TUNNEL_IP:$WS_PORT)"
else
    warn "the router does not reach tcp $HOST_TUNNEL_IP:$WS_PORT through the tunnel yet. start (or restart) the chat server, then run status.sh"
fi

# from outside: only possible from here in ports-only mode. in full tunnel mode this machine's own
# packets to the router address travel through the tunnel and never meet the forwarding rules
if [ "$MODE" = "ports" ]; then
    if command -v nc >/dev/null 2>&1; then
        if nc -z -w 3 "$ROUTER_IP" "$WS_PORT" 2>/dev/null; then
            step "tcp $ROUTER_IP:$WS_PORT answers from outside (the forwarding works)"
        else
            warn "tcp $ROUTER_IP:$WS_PORT does not answer from outside; check the VPS provider's firewall, then run status.sh"
        fi
    fi
else
    echo "  in full tunnel mode the public address cannot be checked from this machine; a connection from another network is the test"
fi

echo
printf '\033[32mdone.\033[0m\n'
echo "  friends connect to:  $ROUTER_IP  port $WS_PORT"
[ "$HTTP_PORT" != "0" ] && echo "  browser:             http://$ROUTER_IP:$HTTP_PORT/client.html"
if [ "$IS_TLS_FORWARDED" = "yes" ]; then
    echo "  tls:                 ends on this machine; the certificate and its key are not on the router"
    STUNNEL_DOMAIN="$(settings_get stunnel_domain "")"
    if [ -n "$STUNNEL_DOMAIN" ]; then
        echo "  domain:              the A record of $STUNNEL_DOMAIN must point at $ROUTER_IP"
        [ "$HOST_HTTPS" != "0" ] && echo "  browser, https:      https://$STUNNEL_DOMAIN:$HOST_HTTPS/client.html"
    else
        echo "  domain:              point its A record at $ROUTER_IP, then obtain the certificate on this machine (README.md)"
    fi
fi
echo "  voice and video:     udp 3478 and $RANGE_BEGIN-$RANGE_END are forwarded; the server announces $ROUTER_IP"
echo "  status:              ./status.sh      remove everything: ./teardown.sh"

exit 0
