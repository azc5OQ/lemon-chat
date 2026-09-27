#!/bin/sh
# tunnel router status - runs on the VPS. POSIX sh, started by the host's status script over ssh.
# prints the tunnel state, the forwarding rules and the firewall state, read-only.
# Upload cleanup is performed by the caller.

if [ "$(id -u)" != "0" ]; then
    exec sudo -p '[tunnel] sudo password:
' sh "$0" "$@"
fi

SELF="$0"

echo "== wg0 =="
if wg show wg0 >/dev/null 2>&1; then
    wg show wg0
    NOW="$(date +%s)"
    wg show wg0 latest-handshakes | while read -r key stamp; do
        if [ "$stamp" = "0" ]; then
            echo "peer $key: no handshake yet"
        else
            echo "peer $key: last handshake $((NOW - stamp)) s ago"
        fi
    done
else
    echo "wg0 is not up"
fi

echo
echo "== ip forwarding =="
sysctl -n net.ipv4.ip_forward

echo
echo "== forwarding rules (nat PREROUTING, FORWARD, nat POSTROUTING) =="
iptables -t nat -S PREROUTING 2>/dev/null | grep -- "-j DNAT" || echo "no DNAT rules"
iptables -S FORWARD 2>/dev/null | grep -- "wg0" || echo "no FORWARD rules for wg0"
iptables -t nat -S POSTROUTING 2>/dev/null | grep -- "MASQUERADE" || echo "no MASQUERADE rules"

echo
echo "== firewall =="
if command -v ufw >/dev/null 2>&1; then
    ufw status 2>/dev/null | head -n 30
elif command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    firewall-cmd --list-ports
    echo "masquerade: $(firewall-cmd --query-masquerade)"
else
    echo "no ufw or firewalld found; the provider's firewall (if any) is outside this script"
fi

echo
echo "== listening udp sockets (the tunnel port is expected here; forwarded ports have no listener on this machine) =="
ss -lnu 2>/dev/null | head -n 20


