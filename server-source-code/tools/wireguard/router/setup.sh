#!/bin/sh
# tunnel router setup - runs on the VPS. POSIX sh, started by the host's setup script over ssh.
#
# makes this machine a router for a host behind NAT: installs WireGuard, creates the wg0 tunnel and
# forwards a set of ports from the public address through the tunnel to the host. application connections
# end on the host; this router does not terminate their tls. safe to run again: keys are
# kept, the rules and the peer entry are replaced. Upload cleanup belongs to the caller.
#
# usage (as root, or the script re-runs itself with sudo):
#   sh setup.sh --mode ports|full --host-pubkey KEY --wg-port 51820
#               --subnet 10.66.0.0/24 --router-ip 10.66.0.1 --host-ip 10.66.0.2
#               --tcp-ports 1111,8090 --udp-ports 3478,20000:20099
#
# the last lines of the output are read by the host script:
#   WG_PUBLIC_KEY=<key>  WG_PORT=<port>  WG_INTERFACE=<iface>  SETUP_OK=1

set -eu
umask 077
. "$(dirname "$0")/validate.sh"

if [ "$(id -u)" != "0" ]; then
    exec sudo -p '[tunnel] sudo password:
' sh "$0" "$@"
fi

# These are VPS-side defaults. The host setup script overrides them by passing
# command-line arguments, so you normally do not need to edit them here.

# "ports" forwards only the selected ports through the tunnel to the chat host.
# "full" also routes all of the host's IPv4 Internet traffic through the VPS.
MODE="ports"

# The chat host's WireGuard public key. The host setup script generates a key
# pair if needed and supplies the public key here. The private key stays on the
# host; it is not sent to the VPS. Empty means it has not been supplied yet.
HOST_PUBKEY=""

# The UDP port on which the VPS listens for WireGuard connections.
# 51820 is the conventional WireGuard port, not a requirement; it can be changed.
WG_PORT="51820"

# The VPS's SSH port, used to protect administrative access from port forwarding.
# 22 is the conventional SSH port. If SSH uses another port, supply that value.
# The script refuses to forward this TCP port to the chat host.
SSH_PORT="22"

# The private network used inside the WireGuard tunnel. 10.66.0.0/24 is an
# arbitrary private range; choose a range that does not overlap your LAN, other
# VPNs, or VPS networks. /24 contains 256 addresses, including the network and
# broadcast addresses; this setup uses two addresses for the tunnel endpoints.
# Chat users do not each receive a tunnel IP, so this does not limit server slots.
SUBNET="10.66.0.0/24"

# The VPS's address inside the tunnel, not its public Internet address.
# .1 is simply the chosen router endpoint within the private subnet.
ROUTER_IP="10.66.0.1"

# The chat server host's address inside the tunnel.
# .2 is the other endpoint. This setup expects the router at .1 and the host at .2.
HOST_IP="10.66.0.2"

# TCP ports to forward from the VPS's public address to the chat host.
# Empty here because the host setup script supplies the selected ports.
# Example: "1111,8090,443". Commas separate ports; a colon specifies a range.
TCP_PORTS=""

# UDP ports to forward from the VPS's public address to the chat host.
# Empty here because the host setup script supplies the selected ports.
# Example: "3478,20000:20099" forwards port 3478 and ports 20000 through 20099.
UDP_PORTS=""

while [ $# -gt 0 ]; do
    [ "$#" -ge 2 ] || { echo "missing value for $1" >&2; exit 2; }
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --host-pubkey) HOST_PUBKEY="$2"; shift 2 ;;
        --ssh-port) SSH_PORT="$2"; shift 2 ;;
        --wg-port) WG_PORT="$2"; shift 2 ;;
        --subnet) SUBNET="$2"; shift 2 ;;
        --router-ip) ROUTER_IP="$2"; shift 2 ;;
        --host-ip) HOST_IP="$2"; shift 2 ;;
        --tcp-ports) TCP_PORTS="$2"; shift 2 ;;
        --udp-ports) UDP_PORTS="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

validate_tunnel || { echo "invalid mode, subnet, ports, or SSH/WireGuard port collision" >&2; exit 2; }
[ "${#HOST_PUBKEY}" -eq 44 ] || { echo "invalid public key" >&2; exit 2; }
case "$HOST_PUBKEY" in *[!a-zA-Z0-9+/=]*) echo "invalid public key" >&2; exit 2 ;; esac
BASE=${SUBNET%.*}
[ "$ROUTER_IP" = "$BASE.1" ] && [ "$HOST_IP" = "$BASE.2" ] || { echo "tunnel addresses do not match subnet" >&2; exit 2; }

WG_DIR="/etc/wireguard"
FORWARD_SCRIPT="$WG_DIR/wg0-forward.sh"
MARKER="managed by the tunnel setup"
TCP_LIST="$(echo "$TCP_PORTS" | tr ',' ' ')"
UDP_LIST="$(echo "$UDP_PORTS" | tr ',' ' ')"

log() { echo "[tunnel] $*"; }
finish() { exit "$1"; }

# Refuse ownership collisions before changing packages, services or the firewall.
for owned in "$WG_DIR/wg0.conf" "$FORWARD_SCRIPT" /etc/sysctl.d/99-wg0-forward.conf; do
    if [ -e "$owned" ] || [ -L "$owned" ]; then
        [ ! -L "$owned" ] && grep -q "^# $MARKER" "$owned" || {
            log "refusing to replace unowned $owned"
            exit 1
        }
    fi
done
if ip link show wg0 >/dev/null 2>&1 && [ ! -f "$WG_DIR/wg0.conf" ]; then
    log "wg0 already exists without a managed configuration"
    exit 1
fi
mkdir -p "$WG_DIR"
chmod 700 "$WG_DIR"
# Serialize setup/teardown and stage replacements before touching the live tunnel.
mkdir "$WG_DIR/.tunnel-lock" 2>/dev/null || {
    log "another setup/teardown is running (or a stale .tunnel-lock needs inspection)"
    exit 1
}
STAGE=$(mktemp -d "$WG_DIR/.tunnel.XXXXXXXXXX") || {
    rmdir "$WG_DIR/.tunnel-lock"
    exit 1
}
WAS_UP=no
wg show wg0 >/dev/null 2>&1 && WAS_UP=yes
CHANGED=no
WAS_ENABLED=no
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    if systemctl is-enabled --quiet wg-quick@wg0; then
        WAS_ENABLED=yes
    fi
fi

# On failure, stop partial new rules, restore saved files, then restart the old tunnel.
# CHANGED stays "no" during validation and staging, so early failures only remove staging.
cleanup() {
    result=$?
    trap - 0
    if [ "$result" -ne 0 ] && [ "$CHANGED" = yes ]; then
        set +e
        log "setup failed; restoring the previous tunnel configuration"
        if wg show wg0 >/dev/null 2>&1; then
            wg-quick down wg0
        fi
        [ ! -f "$FORWARD_SCRIPT" ] || sh "$FORWARD_SCRIPT" down
        for old in wg0.conf wg0-forward.sh wg0-ip-forward.before; do
            if [ -f "$STAGE/$old" ]; then
                cp -p "$STAGE/$old" "$WG_DIR/$old"
            else
                rm -f "$WG_DIR/$old"
            fi
        done
        if [ -f "$STAGE/sysctl.conf" ]; then
            cp -p "$STAGE/sysctl.conf" /etc/sysctl.d/99-wg0-forward.conf
        else
            rm -f /etc/sysctl.d/99-wg0-forward.conf
        fi
        sysctl -q -w "net.ipv4.ip_forward=$FORWARD_BEFORE"
        if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
            if [ "$WAS_ENABLED" = no ]; then
                systemctl disable wg-quick@wg0
            fi
            if [ "$WAS_UP" = yes ]; then
                systemctl start wg-quick@wg0 || log "ROLLBACK FAILED: inspect wg0 before retrying"
            fi
        elif command -v rc-service >/dev/null 2>&1 && [ -f /etc/init.d/wg-quick.wg0 ]; then
            if [ "$WAS_UP" = yes ]; then
                rc-service wg-quick.wg0 start || log "ROLLBACK FAILED: inspect wg0 before retrying"
            fi
            if [ ! -f "$STAGE/wg0.conf" ]; then
                rc-update del wg-quick.wg0 default
                rm -f /etc/init.d/wg-quick.wg0
            fi
        elif [ "$WAS_UP" = yes ]; then
            wg-quick up wg0 || log "ROLLBACK FAILED: inspect wg0 before retrying"
        fi
    fi
    rm -f "$STAGE/wg0.conf" "$STAGE/wg0-forward.sh" \
        "$STAGE/new.conf" "$STAGE/new-forward.sh" \
        "$STAGE/sysctl.conf" "$STAGE/wg0-ip-forward.before"
    rmdir "$STAGE" "$WG_DIR/.tunnel-lock"
    exit "$result"
}
trap cleanup 0
trap 'exit 1' HUP INT TERM
FORWARD_BEFORE=$(sysctl -n net.ipv4.ip_forward)
case "$FORWARD_BEFORE" in 0|1) ;; *) exit 1 ;; esac
for old in wg0.conf wg0-forward.sh wg0-ip-forward.before; do
    [ ! -f "$WG_DIR/$old" ] || cp -p "$WG_DIR/$old" "$STAGE/$old"
done
[ ! -f /etc/sysctl.d/99-wg0-forward.conf ] || cp -p /etc/sysctl.d/99-wg0-forward.conf "$STAGE/sysctl.conf"

# ---- 1. packages ----

install_packages() {
    if command -v apt-get >/dev/null 2>&1; then
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq wireguard-tools iptables curl >/dev/null
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y -q wireguard-tools iptables curl >/dev/null
    elif command -v yum >/dev/null 2>&1; then
        yum install -y -q epel-release >/dev/null 2>&1 || true
        yum install -y -q wireguard-tools iptables curl >/dev/null
    elif command -v apk >/dev/null 2>&1; then
        apk add -q wireguard-tools iptables curl iproute2
    elif command -v pacman >/dev/null 2>&1; then
        pacman -S --noconfirm --needed wireguard-tools iptables curl >/dev/null
    else
        log "no known package manager (apt, dnf, yum, apk, pacman); install wireguard-tools, iptables and curl by hand and run again"
        finish 1
    fi
}

if ! command -v wg >/dev/null 2>&1 ||
   ! command -v wg-quick >/dev/null 2>&1 ||
   ! command -v iptables >/dev/null 2>&1 ||
   ! command -v curl >/dev/null 2>&1 ||
   ! command -v ip >/dev/null 2>&1; then
    log "installing wireguard-tools, iptables and curl"
    install_packages
fi

for tool in wg wg-quick iptables curl ip sysctl; do
    command -v "$tool" >/dev/null 2>&1 || { log "$tool is missing"; exit 1; }
done

# ---- 2. the public interface and ip forwarding ----

IFACE="$(ip route get 1.1.1.1 2>/dev/null | awk '{ for (i = 1; i <= NF; i++) if ($i == "dev") print $(i + 1) }' | head -n 1)"
if [ -z "$IFACE" ]; then
    # busybox ip has no "route get": read the default route instead
    IFACE="$(ip route 2>/dev/null | awk '$1 == "default" { for (i = 1; i <= NF; i++) if ($i == "dev") print $(i + 1) }' | head -n 1)"
fi
if [ -z "$IFACE" ] && command -v route >/dev/null 2>&1; then
    IFACE="$(route -n 2>/dev/null | awk '$1 == "0.0.0.0" { print $NF }' | head -n 1)"
fi
if [ -z "$IFACE" ]; then
    log "could not find the interface with the default route"
    finish 1
fi
valid_name "$IFACE" || { log "invalid interface name"; exit 1; }
log "public interface: $IFACE"

# ---- 3. keys ----

mkdir -p "$WG_DIR"
chmod 700 "$WG_DIR"
if [ ! -f "$WG_DIR/wg0.key" ]; then
    umask 077
    wg genkey > "$WG_DIR/wg0.key.new"
    mv "$WG_DIR/wg0.key.new" "$WG_DIR/wg0.key"
    log "generated the key pair"
fi
ROUTER_PRIVATE_KEY="$(cat "$WG_DIR/wg0.key")"
ROUTER_PUBLIC_KEY="$(echo "$ROUTER_PRIVATE_KEY" | wg pubkey)"

# Build native firewalld policies, scoped to the actual public zone and wg0.
# Keep ordinary iptables/UFW permissions untouched: only tagged rules are installed.
ZONE=""
if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    firewall-cmd --get-policies >/dev/null || { log "firewalld policy support is required"; exit 1; }
    binding=$(firewall-cmd --get-zone-of-interface=wg0 || :)
    case "$binding" in
        ''|'no zone'|tunnel-wg0) ;;
        *)
            log "wg0 already belongs to another firewall zone"
            exit 1
            ;;
    esac
    ZONE=$(firewall-cmd --get-zone-of-interface="$IFACE" || :)
    [ "$ZONE" != "no zone" ] && [ -n "$ZONE" ] || ZONE=$(firewall-cmd --get-default-zone)
    valid_name "$ZONE" || exit 1
    for file in /etc/firewalld/zones/tunnel-wg0.xml \
                /etc/firewalld/policies/tunnel-wg0-in.xml \
                /etc/firewalld/policies/tunnel-wg0-out.xml \
                /etc/firewalld/policies/tunnel-wg0-udp.xml; do
        [ ! -e "$file" ] || grep -q 'managed by the tunnel setup' "$file" || {
            log "refusing to replace $file"
            exit 1
        }
    done
fi

# ---- 4. the forwarding script (PostUp / PostDown of wg0) ----

cat > "$STAGE/new-forward.sh" <<EOF
#!/bin/sh
set -eu
# $MARKER - do not edit, run the setup again instead
# \$1 is "up" or "down". every rule is checked before it is added, so a repeat run adds nothing twice.
ACTION="\$1"
IFACE="$IFACE"
HOST_IP="$HOST_IP"
MODE="$MODE"
TCP_PORTS="$TCP_LIST"
UDP_PORTS="$UDP_LIST"
WG_PORT="$WG_PORT"
ZONE="$ZONE"

EOF
cat "$(dirname "$0")/firewalld.sh" >> "$STAGE/new-forward.sh"
cat >> "$STAGE/new-forward.sh" <<'RULES'
rule() {
    # rule <table or -> <chain> <args...>: insert on up (only when absent), delete on down.
    table="$1"
    chain="$2"
    shift 2
    if [ "$table" = "-" ]; then
        table=filter
    fi
    iptables -w 10 -t "$table" -S "$chain" >/dev/null
    if [ "$ACTION" = "up" ]; then
        iptables -w 10 -t "$table" -C "$chain" "$@" -m comment --comment tunnel-wg0 2>/dev/null ||
            iptables -w 10 -t "$table" -I "$chain" 1 "$@" -m comment --comment tunnel-wg0
    else
        if iptables -w 10 -t "$table" -C "$chain" "$@" -m comment --comment tunnel-wg0 2>/dev/null; then
            iptables -w 10 -t "$table" -D "$chain" "$@" -m comment --comment tunnel-wg0
        fi
    fi
}

# The WireGuard listener is local; selected application ports are forwarded to the host.
rule - INPUT -i "$IFACE" -p udp --dport "$WG_PORT" -j ACCEPT
for p in $TCP_PORTS; do
    rule nat PREROUTING -i "$IFACE" -p tcp --dport "$p" -j DNAT --to-destination "$HOST_IP"
    rule - FORWARD -i "$IFACE" -o wg0 -p tcp -d "$HOST_IP" --dport "$p" -j ACCEPT
done
for p in $UDP_PORTS; do
    rule nat PREROUTING -i "$IFACE" -p udp --dport "$p" -j DNAT --to-destination "$HOST_IP"
    rule - FORWARD -i "$IFACE" -o wg0 -p udp -d "$HOST_IP" --dport "$p" -j ACCEPT
done
rule - FORWARD -i wg0 -o "$IFACE" -s "$HOST_IP" -j ACCEPT
rule - FORWARD -i "$IFACE" -o wg0 -d "$HOST_IP" -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
# Clamp TCP MSS to the route's MTU so forwarded connections fit through the tunnel.
rule mangle FORWARD -o wg0 -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
rule mangle FORWARD -i wg0 -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu

if [ "$MODE" = "ports" ]; then
    # replies must come back through the tunnel: the host sees every visitor as this machine's tunnel address
    rule nat POSTROUTING -o wg0 -d "$HOST_IP" -j MASQUERADE
else
    # full tunnel: the host's own internet traffic leaves through this machine; visitors keep their addresses
    rule nat POSTROUTING -s "$HOST_IP" -o "$IFACE" -j MASQUERADE
fi
RULES
chmod 700 "$STAGE/new-forward.sh"

# ---- 5. wg0.conf ----

PREFIX="${SUBNET#*/}"
umask 077
cat > "$STAGE/new.conf" <<EOF
# $MARKER
[Interface]
Address = $ROUTER_IP/$PREFIX
ListenPort = $WG_PORT
PrivateKey = $ROUTER_PRIVATE_KEY
PostUp = $FORWARD_SCRIPT up
PostDown = $FORWARD_SCRIPT down

[Peer]
# the tunnel client
PublicKey = $HOST_PUBKEY
AllowedIPs = $HOST_IP/32
EOF
# only the key and wg0.conf are private
umask 022

# ---- 6. Replace the live configuration ----

# Remove the OLD rules while the OLD config and PostDown still exist.
CHANGED=yes
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    if systemctl is-active --quiet wg-quick@wg0; then
        systemctl stop wg-quick@wg0
    fi
elif command -v rc-service >/dev/null 2>&1 && [ -f /etc/init.d/wg-quick.wg0 ]; then
    if rc-service wg-quick.wg0 status >/dev/null 2>&1; then
        rc-service wg-quick.wg0 stop
    fi
fi
if wg show wg0 >/dev/null 2>&1; then
    wg-quick down wg0
fi
if [ -f "$FORWARD_SCRIPT" ]; then
    sh "$FORWARD_SCRIPT" down
fi
mv "$STAGE/new-forward.sh" "$FORWARD_SCRIPT"
mv "$STAGE/new.conf" "$WG_DIR/wg0.conf"
[ -f "$WG_DIR/wg0-ip-forward.before" ] || sysctl -n net.ipv4.ip_forward > "$WG_DIR/wg0-ip-forward.before"
mkdir -p /etc/sysctl.d
printf '# %s\nnet.ipv4.ip_forward = 1\n' "$MARKER" > /etc/sysctl.d/99-wg0-forward.conf
sysctl -q -w net.ipv4.ip_forward=1

# ---- 7. bring the tunnel up (restart when it already runs, so the new rules and peer apply) ----

if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    systemctl enable wg-quick@wg0 >/dev/null 2>&1
    if systemctl is-active --quiet wg-quick@wg0; then
        systemctl restart wg-quick@wg0
    else
        systemctl start wg-quick@wg0
    fi
elif command -v rc-update >/dev/null 2>&1 && [ -f /etc/init.d/wg-quick ]; then
    # openrc (alpine): the wg-quick service takes its interface from the service name
    ln -sf /etc/init.d/wg-quick /etc/init.d/wg-quick.wg0
    rc-update add wg-quick.wg0 default >/dev/null 2>&1
    rc-service wg-quick.wg0 start
else
    log "no supported service manager: tunnel will not survive a reboot"
    wg-quick up wg0
fi

if ! wg show wg0 >/dev/null 2>&1; then
    log "wg0 did not come up; run: journalctl -u wg-quick@wg0"
    finish 1
fi

# ---- 8. report ----

log "tunnel wg0 is up on udp $WG_PORT, mode: $MODE"
log "forwarded to $HOST_IP: tcp [$TCP_LIST] udp [$UDP_LIST]"
echo "WG_PUBLIC_KEY=$ROUTER_PUBLIC_KEY"
echo "WG_PORT=$WG_PORT"
echo "WG_INTERFACE=$IFACE"
echo "SETUP_OK=1"
finish 0
