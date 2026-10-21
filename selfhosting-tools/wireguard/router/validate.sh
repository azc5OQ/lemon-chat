#!/bin/sh
# Shared validation; no side effects when sourced. Values written into shell configs must pass here.

valid_port() {
    case "$1" in ''|*[!0-9]*|0*|??????*) return 1 ;; esac
    [ "$1" -ge 1 ] && [ "$1" -le 65535 ]
}

valid_ipv4() {
    printf '%s\n' "$1" | awk -F. '
        NF != 4 { exit 1 }
        {
            for (i = 1; i <= 4; i++) {
                if ($i !~ /^[0-9]+$/ || length($i) > 3 || $i > 255 ||
                    (length($i) > 1 && substr($i, 1, 1) == "0")) exit 1
            }
        }
    '
}

valid_name() {
    case "$1" in ''|*[!a-zA-Z0-9_.-]*|-*) return 1 ;; esac
}

valid_subnet() {
    # The UI assigns .1 and .2. Require a canonical RFC1918 network containing both.
    printf '%s\n' "$1" | awk -F'[./]' '
        NF != 5 { exit 1 }
        {
            for (i = 1; i <= 5; i++) {
                if ($i !~ /^[0-9]+$/ || length($i) > 3 ||
                    (length($i) > 1 && substr($i, 1, 1) == "0")) exit 1
            }
            for (i = 1; i <= 4; i++) {
                if ($i > 255) exit 1
            }
            if ($5 < 8 || $5 > 30 || $4 != 0) exit 1
            if (!($1 == 10 ||
                  ($1 == 172 && $2 >= 16 && $2 <= 31 && $5 >= 12) ||
                  ($1 == 192 && $2 == 168 && $5 >= 16))) exit 1

            # Reject host bits in the network address.
            n = (($1 * 256 + $2) * 256 + $3) * 256 + $4
            if (n % (2 ^ (32 - $5)) != 0) exit 1
        }
    '
}

# Subshells keep comma splitting and temporary variables out of the caller.
valid_ports() (
    case "$1" in ''|*[!0-9,:]*|,*|*,|*,,*) exit 1 ;; esac
    IFS=,
    set -- $1
    for vp in "$@"; do
        case "$vp" in
            *:*)
                va=${vp%:*}
                vb=${vp#*:}
                valid_port "$va" && valid_port "$vb" && [ "$va" -le "$vb" ] || exit 1
                ;;
            *) valid_port "$vp" || exit 1 ;;
        esac
    done
)

port_in_list() (
    needle=$1
    IFS=,
    set -- $2
    for vp in "$@"; do
        va=${vp%:*}
        vb=${vp#*:}
        if [ "$needle" -ge "$va" ] && [ "$needle" -le "$vb" ]; then
            exit 0
        fi
    done
    exit 1
)

validate_tunnel() {
    case "$MODE" in ports|full) ;; *) return 1 ;; esac
    valid_port "$WG_PORT" && valid_port "$SSH_PORT" && valid_subnet "$SUBNET" &&
        valid_ports "$TCP_PORTS" && valid_ports "$UDP_PORTS" || return 1
    # Forwarding these ports would steal SSH access or the WireGuard listener.
    port_in_list "$SSH_PORT" "$TCP_PORTS" && return 1
    port_in_list "$WG_PORT" "$UDP_PORTS" && return 1
    return 0
}
