#!/bin/sh
# Appended to the generated forwarding script; all variables are validated by setup.
# These exclusively owned policies supplement iptables on the nftables backend.
# Inputs: ACTION, ZONE, HOST_IP, TCP_PORTS, UDP_PORTS and WG_PORT.
# Run before installing iptables rules because the firewalld reload can clear them.
if [ -n "$ZONE" ]; then
    command -v firewall-cmd >/dev/null 2>&1
    firewall-cmd --state >/dev/null
    if [ "$ACTION" = up ]; then
        mkdir -p /etc/firewalld/zones /etc/firewalld/policies
        for file in /etc/firewalld/zones/tunnel-wg0.xml \
                    /etc/firewalld/policies/tunnel-wg0-in.xml \
                    /etc/firewalld/policies/tunnel-wg0-out.xml \
                    /etc/firewalld/policies/tunnel-wg0-udp.xml; do
            if [ -e "$file" ] || [ -L "$file" ]; then
                [ ! -L "$file" ] && grep -q 'managed by the tunnel setup' "$file" || exit 1
            fi
        done
        cat > /etc/firewalld/zones/tunnel-wg0.xml <<EOF
<?xml version="1.0" encoding="utf-8"?>
<!-- managed by the tunnel setup -->
<zone><interface name="wg0"/></zone>
EOF
        # Allow only the forwarded ports from the public zone to the host.
        {
            printf '%s\n' '<?xml version="1.0" encoding="utf-8"?>' '<!-- managed by the tunnel setup -->' '<policy target="CONTINUE" priority="-100">'
            printf '<ingress-zone name="%s"/><egress-zone name="tunnel-wg0"/>\n' "$ZONE"
            for proto in tcp udp; do
                if [ "$proto" = tcp ]; then
                    ports=$TCP_PORTS
                else
                    ports=$UDP_PORTS
                fi
                for port in $ports; do
                    port=$(printf '%s' "$port" | tr ':' '-')
                    printf '<rule family="ipv4"><destination address="%s"/><port port="%s" protocol="%s"/><accept/></rule>\n' "$HOST_IP" "$port" "$proto"
                done
            done
            printf '%s\n' '</policy>'
        } > /etc/firewalld/policies/tunnel-wg0-in.xml
        # Allow the host's outbound traffic through the public interface.
        cat > /etc/firewalld/policies/tunnel-wg0-out.xml <<EOF
<?xml version="1.0" encoding="utf-8"?>
<!-- managed by the tunnel setup -->
<policy target="CONTINUE" priority="-100">
<ingress-zone name="tunnel-wg0"/><egress-zone name="$ZONE"/>
<rule family="ipv4"><source address="$HOST_IP"/><accept/></rule>
</policy>
EOF
        # WireGuard itself terminates on the VPS, in firewalld's HOST zone.
        cat > /etc/firewalld/policies/tunnel-wg0-udp.xml <<EOF
<?xml version="1.0" encoding="utf-8"?>
<!-- managed by the tunnel setup -->
<policy target="CONTINUE" priority="-100">
<ingress-zone name="$ZONE"/><egress-zone name="HOST"/>
<rule family="ipv4"><port port="$WG_PORT" protocol="udp"/><accept/></rule>
</policy>
EOF
        firewall-cmd --check-config
    else
        for file in /etc/firewalld/policies/tunnel-wg0-in.xml \
                    /etc/firewalld/policies/tunnel-wg0-out.xml \
                    /etc/firewalld/policies/tunnel-wg0-udp.xml \
                    /etc/firewalld/zones/tunnel-wg0.xml; do
            if [ -f "$file" ]; then
                grep -q 'managed by the tunnel setup' "$file" || exit 1
                rm -f "$file"
            fi
        done
    fi
    firewall-cmd --reload
fi
