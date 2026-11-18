#!/bin/sh
# Bounded TCP-connect check. Upload cleanup belongs to the caller.
set -eu
. "$(dirname "$0")/validate.sh"

[ "$#" = 2 ] && valid_ipv4 "$1" && valid_port "$2" || {
    echo 'usage: check.sh IPv4 PORT' >&2
    exit 2
}
command -v curl >/dev/null 2>&1 || {
    echo 'curl is required for the tunnel check' >&2
    exit 1
}

# num_connects proves a connection completed even for a silent/non-HTTP listener.
# Disable proxies and curlrc: the check must reach the tunnel host directly.
connected=$(curl -q --noproxy '*' -s -o /dev/null \
    --connect-timeout 3 --max-time 4 -w '%{num_connects}' "http://$1:$2/" 2>/dev/null) || :
case "$connected" in ''|*[!0-9]*) exit 1 ;; esac
if [ "$connected" -gt 0 ]; then
    echo TUNNEL_TCP_OK
else
    echo 'no TCP connection through tunnel' >&2
    exit 1
fi
