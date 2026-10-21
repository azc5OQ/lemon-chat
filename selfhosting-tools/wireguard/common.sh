#!/bin/sh
# lemon-chat selfhosting-tools/wireguard/common.sh - shared by setup.sh, status.sh and teardown.sh on a LINUX host (sourced, not run).
# paths, the saved answers, the ssh helpers and the server_settings.json access.

WG_HERE="$(cd "$(dirname "$0")" && pwd)"
WG_REPO="$(cd "$WG_HERE/../.." && pwd)"   # The project root is two directories above selfhosting-tools/wireguard.
WG_ANSWERS="$WG_HERE/answers.json"
WG_KEYS_DIR="$WG_HERE/keys"
WG_SSH_DIR="$WG_HERE/ssh"
WG_TUNNEL="lemon-wg0"
WG_CONF="/etc/wireguard/$WG_TUNNEL.conf"
WG_SERVER_SETTINGS="$WG_REPO/buildresult/server_settings.json"

# Console output and interactive questions.
step() { printf '\033[36m[lemon-wg] %s\033[0m\n' "$*"; }
warn() { printf '\033[33m[lemon-wg] %s\033[0m\n' "$*"; }
fail() { printf '\033[31m[lemon-wg] %s\033[0m\n' "$*"; }

ask() {
    # ask "prompt" "default" -> prints the answer; Enter takes the default, an empty default repeats
    prompt="$1"
    default="$2"
    answer=""
    while true; do
        if [ -n "$default" ]; then
            printf '%s [%s]: ' "$prompt" "$default" >&2
            IFS= read -r answer || exit 1
            [ -z "$answer" ] && answer="$default"
        else
            printf '%s: ' "$prompt" >&2
            IFS= read -r answer || exit 1
        fi
        answer="$(echo "$answer" | sed 's/^ *//; s/ *$//')"
        [ -n "$answer" ] && {
            echo "$answer"
            return
        }
    done
}

ask_yes_no() {
    # ask_yes_no "prompt" yes|no -> exit status 0 for yes
    prompt="$1"
    default="$2"
    hint=""
    answer=""
    if [ "$default" = "yes" ]; then
        hint="(Y/n)"
    else
        hint="(y/N)"
    fi
    while true; do
        printf '%s %s: ' "$prompt" "$hint" >&2
        IFS= read -r answer || return 1
        answer="$(echo "$answer" | tr '[:upper:]' '[:lower:]')"
        case "$answer" in
            "") [ "$default" = "yes" ] && return 0 || return 1 ;;
            y|yes) return 0 ;;
            n|no) return 1 ;;
        esac
    done
}

# Saved answers and server settings. Python handles JSON, not shell evaluation.
have_python() { command -v python3 >/dev/null 2>&1; }

answer_get() {
    # answer_get name default
    if have_python && [ -f "$WG_ANSWERS" ]; then
        python3 - "$WG_ANSWERS" "$1" "$2" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as f:
        value = json.load(f).get(sys.argv[2], "")
    print(value if str(value) != "" else sys.argv[3])
except Exception:
    print(sys.argv[3])
PY
    else
        echo "$2"
    fi
}

answers_save() {
    python3 "$WG_HERE/json_file.py" answers "$WG_ANSWERS" "$@"
}

settings_get() {
    # settings_get name default -> a value from server_settings.json
    if have_python && [ -f "$WG_SERVER_SETTINGS" ]; then
        python3 - "$WG_SERVER_SETTINGS" "$1" "$2" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as f:
        value = json.load(f).get(sys.argv[2], "")
    print(value if str(value) != "" else sys.argv[3])
except Exception:
    print(sys.argv[3])
PY
    else
        echo "$2"
    fi
}

settings_set() {
    [ -f "$WG_SERVER_SETTINGS" ] || {
        fail "server_settings.json is missing"
        return 1
    }
    python3 "$WG_HERE/json_file.py" settings "$WG_SERVER_SETTINGS" "$@"
}

# Local tunnel ownership and service management.
as_root() {
    if [ "$(id -u)" = 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

local_tunnel_owned() {
    if as_root test -e "$WG_CONF"; then
        # Accept configurations created before the tools directory was renamed.
        as_root grep -Eq '^# written by lemon-chat (selfhosting-tools|tools)/wireguard/setup\.sh( |$)' "$WG_CONF" || {
            fail "refusing to replace an unrelated $WG_CONF"
            return 1
        }
    elif as_root wg show "$WG_TUNNEL" >/dev/null 2>&1; then
        fail "tunnel exists without a managed configuration"
        return 1
    fi
}

local_tunnel_stop() {
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        if systemctl is-active --quiet "wg-quick@$WG_TUNNEL"; then
            as_root systemctl stop "wg-quick@$WG_TUNNEL" || return 1
        fi
        if systemctl is-enabled --quiet "wg-quick@$WG_TUNNEL"; then
            as_root systemctl disable "wg-quick@$WG_TUNNEL" || return 1
        fi
    elif command -v rc-service >/dev/null 2>&1 && [ -f "/etc/init.d/wg-quick.$WG_TUNNEL" ]; then
        if as_root rc-service "wg-quick.$WG_TUNNEL" status >/dev/null 2>&1; then
            as_root rc-service "wg-quick.$WG_TUNNEL" stop || return 1
        fi
        as_root rc-update del "wg-quick.$WG_TUNNEL" default || return 1
    fi
    if as_root wg show "$WG_TUNNEL" >/dev/null 2>&1; then
        as_root wg-quick down "$WG_TUNNEL" || return 1
    fi
}

local_tunnel_start() {
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        as_root systemctl enable --now "wg-quick@$WG_TUNNEL"
    elif command -v rc-update >/dev/null 2>&1 && [ -f /etc/init.d/wg-quick ]; then
        as_root ln -sf /etc/init.d/wg-quick "/etc/init.d/wg-quick.$WG_TUNNEL" &&
            as_root rc-update add "wg-quick.$WG_TUNNEL" default &&
            as_root rc-service "wg-quick.$WG_TUNNEL" start
    else
        warn "No supported service manager; start this tunnel again after reboot."
        as_root wg-quick up "$WG_TUNNEL"
    fi
}

# SSH commands and temporary script uploads.
shell_quote() {
    # Close the single-quoted argument around each literal apostrophe, then reopen it.
    printf "'"
    printf '%s' "$1" | sed "s/'/'\"'\"'/g"
    printf "'"
}

remote_ssh() (
    # Positional parameters preserve argument boundaries without Bash arrays.
    valid_ipv4 "$ROUTER_IP" && valid_port "$ROUTER_SSH_PORT" && valid_name "$ROUTER_SSH_USER" || exit 2
    set -- -p "$ROUTER_SSH_PORT" \
        -o StrictHostKeyChecking=accept-new \
        -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 \
        -o ServerAliveCountMax=3 "$@"
    if [ -n "$SSH_KEY" ]; then
        set -- -i "$SSH_KEY" -o IdentitiesOnly=yes "$@"
    fi
    ssh "$@"
)
remote_run() { remote_ssh "$ROUTER_SSH_USER@$ROUTER_IP" "$1"; }

remote_script() (
    # A subshell keeps upload variables and traps local to this invocation.
    name=$1
    shift
    case "$name" in setup.sh|status.sh|teardown.sh|check.sh) ;; *) exit 2 ;; esac
    stage=$(remote_run 'umask 077; mktemp -d /tmp/tunnel.XXXXXXXXXX') || exit 1
    case "$stage" in /tmp/tunnel.*) ;; *) exit 1 ;; esac
    case "${stage#/tmp/tunnel.}" in ''|*[!a-zA-Z0-9]*) exit 1 ;; esac
    # Remove only this invocation's uploads, including after a failed remote command.
    cleanup() {
        remote_run "rm -f '$stage/$name' '$stage/validate.sh' '$stage/firewalld.sh'; rmdir '$stage'" >/dev/null 2>&1 || :
    }
    trap cleanup 0
    trap 'exit 1' HUP INT TERM
    for file in "$name" validate.sh firewalld.sh; do
        # Read/normalize first, so a failed read cannot be masked by the ssh pipeline.
        payload=$(sed 's/\r$//' "$WG_HERE/router/$file") || exit 1
        printf '%s\n' "$payload" | remote_run "umask 077; cat > '$stage/$file'" || exit 1
    done
    command="sh '$stage/$name'"
    for argument in "$@"; do
        command="$command $(shell_quote "$argument")"
    done
    if [ "$ROUTER_SSH_USER" = root ] || [ "$name" = check.sh ]; then
        remote_run "$command"
    else
        # A remote terminal lets sudo ask its own password; never save or pipe the password.
        status_file=$(mktemp) || exit 1
        # tee displays the prompt while the caller captures output. Save SSH's
        # own exit code because a POSIX pipeline otherwise reports tee's status.
        (
            remote_ssh -tt "$ROUTER_SSH_USER@$ROUTER_IP" "$command"
            printf '%s' "$?" > "$status_file"
        ) | tee /dev/stderr
        result=$(cat "$status_file")
        rm -f "$status_file"
        exit "$result"
    fi
    result=$?
    exit "$result"
)

. "$WG_HERE/router/validate.sh"

# Defaults reused by setup, status and teardown.
load_answers() {
    ROUTER_IP="$(answer_get router_ip "")"
    ROUTER_SSH_PORT="$(answer_get router_ssh_port 22)"
    ROUTER_SSH_USER="$(answer_get router_ssh_user root)"
    SSH_KEY="$(answer_get ssh_key "")"
    MODE="$(answer_get mode ports)"
    WS_PORT="$(answer_get ws_port "")"
    HTTP_PORT="$(answer_get http_port 0)"
    UDP_RANGE="$(answer_get udp_range "")"
    IS_TLS_FORWARDED="$(answer_get is_tls_forwarded no)"
    WSS_PORT_SAVED="$(answer_get wss_port 0)"
    HTTPS_PORT_SAVED="$(answer_get https_port 0)"
    WG_PORT="$(answer_get wg_port 51820)"
    SUBNET="$(answer_get subnet 10.66.0.0/24)"
    TCP_PORTS_SAVED="$(answer_get tcp_ports "")"
    UDP_PORTS_SAVED="$(answer_get udp_ports "")"
}

handshake_age() {
    # seconds since the tunnel's last handshake, -1 when none
    now=""
    stamp=""
    now="$(date +%s)"
    stamp="$(as_root wg show "$WG_TUNNEL" latest-handshakes 2>/dev/null | awk '{print $2}' | sort -n | tail -n 1)"
    if [ -z "$stamp" ] || [ "$stamp" = "0" ]; then
        echo -1
    else
        echo $((now - stamp))
    fi
}
