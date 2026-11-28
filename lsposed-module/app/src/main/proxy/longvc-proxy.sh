#!/system/bin/sh
set -eu
export PATH=/system/bin:/system/xbin
export LC_ALL=C
umask 077
cd /data/cr/proxy
BIN=/data/cr/proxy/sing-box
STATE=core.pid
IFACE=lvcproxy0
TABLE=39761
RULE_START=8850
RULE_END=8860
CONFIG=active.json

fail() { printf 'proxy=%s\n' "$1"; exit 1; }

start_ticks() {
    sed 's/^.*) //' "/proc/$1/stat" 2>/dev/null | awk '{print $20}'
}

running() {
    [ -f "$STATE" ] || return 1
    IFS=' ' read -r pid ticks < "$STATE" || return 1
    case "$pid" in ''|*[!0-9]*|0|1) return 1 ;; esac
    [ -n "$ticks" ] && [ "$(start_ticks "$pid")" = "$ticks" ] &&
        [ "$(readlink "/proc/$pid/exe" 2>/dev/null)" = "$BIN" ]
}

read_table() {
    if routes=$(ip "$1" route show table "$TABLE" 2>&1); then
        printf '%s' "$routes"
    else
        case "$routes" in
            *'FIB table does not exist'*) return 0 ;;
            *) return 1 ;;
        esac
    fi
}

read_network() {
    rules4=$(ip -4 rule show 2>/dev/null) && rules6=$(ip -6 rule show 2>/dev/null) &&
        routes4=$(read_table -4) && routes6=$(read_table -6) && links=$(ip -o link show 2>/dev/null)
}

has_rule() {
    printf '%s\n' "$1" | awk -v first="$RULE_START" -v last="$RULE_END" -v table="$TABLE" -v conflict="$2" '
        $1+0 >= first && $1+0 <= last { reserved=1 }
        { for (i=1;i<NF;i++) if ($i=="lookup" && $(i+1)==table) lookup=1 }
        END { exit !(lookup || (conflict && reserved)) }
    '
}

has_routes() {
    has_link || [ -n "$routes4$routes6" ] ||
        has_rule "$rules4" 1 || has_rule "$rules6" 1
}

has_link() {
    printf '%s\n' "$links" | awk -F ': ' -v iface="$IFACE" '
        $2 == iface || index($2, iface "@") == 1 { found=1 }
        END { exit !found }
    '
}

points_to_tun() {
    printf '%s\n' "$1" | awk -v iface="$IFACE" '
        { for (i=1;i<NF;i++) if ($i=="dev" && $(i+1)==iface) found=1 }
        END { exit !found }
    '
}

system_ready() {
    running && read_network && has_link &&
        points_to_tun "$routes4" && points_to_tun "$routes6" &&
        has_rule "$rules4" 0 && has_rule "$rules6" 0 && running
}

stop_core() {
    if running; then
        kill -TERM "$pid" 2>/dev/null || true
        count=0
        while running && [ "$count" -lt 20 ]; do
            sleep 0.25
            count=$((count + 1))
        done
        running && return 1
    fi
    rm -f -- "$STATE" "$CONFIG"
    read_network && ! has_routes
}

[ "$(id -u)" = 0 ] || fail no_root
exec 9>session.lock
# Explicit duplication lets Android mksh pass the lock descriptor to Toybox.
timeout 6 flock -x 8 8>&9 || fail busy
case "${1:-}" in
    status)
        if running; then
            if system_ready; then echo proxy=active; else echo proxy=unverified; fi
        else
            read_network || fail route_unavailable
            if has_routes; then echo proxy=residue; else echo proxy=stopped; fi
        fi
        ;;
    start)
        if running; then fail already_running; fi
        [ -c /dev/tun ] && command -v ip >/dev/null || fail unsupported
        read_network || fail route_unavailable
        has_routes && fail route_busy
        trap 'rm -f -- "$CONFIG"' EXIT
        cat > "$CONFIG"
        "$BIN" check -c "$CONFIG" >/dev/null 2>&1 || fail invalid_configuration
        # The core owns its TUN and rules. Keep the controller lock in this shell.
        nohup "$BIN" run -c "$CONFIG" </dev/null 9>&- >/dev/null 2>&1 &
        child=$!
        count=0
        while [ "$count" -lt 24 ]; do
            if [ "$(readlink "/proc/$child/exe" 2>/dev/null)" = "$BIN" ]; then
                ticks=$(start_ticks "$child")
                [ -n "$ticks" ] || break
                if ! printf '%s %s\n' "$child" "$ticks" > "$STATE"; then
                    [ "$(start_ticks "$child")" != "$ticks" ] || kill -TERM "$child" 2>/dev/null || true
                    fail recovery_unverified
                fi
                break
            fi
            kill -0 "$child" 2>/dev/null || break
            sleep 0.25
            count=$((count + 1))
        done
        while running && [ "$count" -lt 32 ]; do
            if system_ready; then echo proxy=active; exit 0; fi
            sleep 0.25
            count=$((count + 1))
        done
        stop_core || fail recovery_unverified
        fail start_failed
        ;;
    stop)
        stop_core || fail recovery_unverified
        echo proxy=stopped
        ;;
    test)
        IFS= read -r url || fail invalid_configuration
        case "$url" in http://*|https://*) ;; *) fail invalid_configuration ;; esac
        probe=$(mktemp probe.XXXXXX) || fail io
        trap 'rm -f -- "$probe"' EXIT
        cat > "$probe"
        # The probe has no TUN inbound. It explicitly selects the given upstream.
        if timeout 12 "$BIN" tools fetch -c "$probe" -o upstream -- "$url" >/dev/null 2>&1; then
            echo proxy=response
        else
            result=$?
            if [ "$result" -eq 124 ] || [ "$result" -eq 143 ]; then fail timeout; fi
            fail connection_failed
        fi
        ;;
    *) fail invalid_action ;;
esac
