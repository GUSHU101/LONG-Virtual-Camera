#!/usr/bin/env bash
set -euo pipefail

root=$(mktemp -d)
trap 'rm -rf -- "$root"' EXIT
mkdir "$root/bin"
launcher=lsposed-module/app/src/main/proxy/longvc-proxy.sh
sed -e "s|/data/cr/proxy|$root|g" \
    -e "s|^export PATH=.*|export PATH=\"$root/bin:\$PATH\"|" "$launcher" > "$root/control.sh"

cat > "$root/bin/id" <<'SH'
#!/bin/sh
echo 0
SH
cat > "$root/bin/ip" <<'SH'
#!/bin/sh
case "$*" in
    '-4 rule show')
        [ "${NETWORK:-}" != unavailable ] || exit 13
        [ "${NETWORK:-}" != rule ] || echo '8854: from all lookup main'
        ;;
    '-6 rule show') echo '0: from all lookup local' ;;
    '-4 route show table 39761')
        [ "${NETWORK:-}" != route ] || echo 'default dev another-tun'
        ;;
    '-6 route show table 39761')
        echo 'Error: ipv6: FIB table does not exist.' >&2
        exit 2
        ;;
    '-o link show')
        [ "${NETWORK:-}" != link ] || echo '7: lvcproxy0: <UP> mtu 1400'
        ;;
    *) exit 2 ;;
esac
SH
chmod +x "$root/bin/id" "$root/bin/ip"
bash -n "$launcher"
control() { bash "$root/control.sh" "$@"; }

[[ $(control status) == proxy=stopped ]]
[[ $(control stop) == proxy=stopped ]]
[[ $(NETWORK=unavailable control status || true) == proxy=route_unavailable ]]
for state in rule route link; do
    [[ $(NETWORK=$state control status) == proxy=residue ]]
    [[ $(NETWORK=$state control stop || true) == proxy=recovery_unverified ]]
done

exec 9>"$root/session.lock"
flock -x 8 8>&9
[[ $(control status 9>&- || true) == proxy=busy ]]
exec 9>&-
[[ $(control status) == proxy=stopped ]]
echo 'Proxy status, route conflicts, lost netlink access, and lock contention passed'
