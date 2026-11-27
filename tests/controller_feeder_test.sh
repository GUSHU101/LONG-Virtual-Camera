#!/usr/bin/env bash
set -euo pipefail

source <(sed '/^require_root$/,$d' lsposed-module/app/src/main/payload/longvc-control.sh)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
PROC_ROOT=$fixture/proc
DATA=$fixture/data
CONTROL=$DATA/control
MIC_MODE=$DATA/mic_mode
SOURCE_CONFIG=$CONTROL/source.conf
mkdir -p "$CONTROL" "$PROC_ROOT"/{41,42,43,44}/fd "$PROC_ROOT"/42/net

write_stat() {
  # /proc/pid/stat fields 3 through 22, including a name with spaces and ')'.
  printf '%s (cr demo)) %s 1 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 %s\n' \
    "$1" "$2" "$3" > "$PROC_ROOT/$1/stat"
}

for pid in 41 42 43 44; do
  printf '%s\0--role\0obs-video\0' "$FEEDER" > "$PROC_ROOT/$pid/cmdline"
  printf '%s\n' "$FEEDER" > "$PROC_ROOT/$pid/exe"
  write_stat "$pid" S "$((pid + 100))"
done
write_stat 43 Z 143
printf '/unrelated/cr_feed_proc\n' > "$PROC_ROOT/44/exe"

fixture_pids='41 42 43 44'
pidof() { [[ $1 == cr_feed_proc ]] && printf '%s\n' "$fixture_pids"; }
readlink() { cat "$1"; }
sleep() { :; }
kill() {
  printf '%s %s\n' "$1" "$2" >> "$fixture/signals"
  case "$1:$2" in
    -TERM:41) write_stat 41 S 200; return 1 ;; # Original process has exited.
    -TERM:42) return 0 ;;                    # Simulate a stuck writer.
    -KILL:42) write_stat 42 Z 142 ;;
    *) return 1 ;;
  esac
}

[[ $(find_feed_pid '--role obs-video') == 41 ]]
[[ $(process_start_time 42) == 142 ]]
[[ -z $(process_start_time 43 || true) ]]
[[ -z $(feeder_start_time 44 '' || true) ]]
printf '%s\n' "$LEGACY_FEEDER (deleted)" > "$PROC_ROOT/44/exe"
[[ $(feeder_start_time 44 '--role obs-video') == 144 ]]
printf '/unrelated/cr_feed_proc\n' > "$PROC_ROOT/44/exe"
printf 'schema=2\nvisual_mode=obs\nobs_url=rtmp://relay.local/live/path-key?user=test&pass=placeholder\n' > "$SOURCE_CONFIG"
[[ $(current_obs_url) == 'rtmp://relay.local/live/path-key?user=test&pass=placeholder' ]]
status=$(print_status)
[[ "$status" == *$'\nobs_configured=1\n'* ]]
[[ "$status" != *obs_address* ]]
[[ "$status" != *relay.local* ]]
[[ "$status" != *path-key* ]]
[[ "$status" != *placeholder* ]]
[[ "$status" == *$'\nobs_pull_running=1\n'* ]]

stop_feeder '--role obs'
[[ $(cat "$fixture/signals") == $'-TERM 41\n-TERM 42\n-KILL 42' ]]

fixture_pids=''
dd if=/dev/zero of="$DATA/feed" bs=64 count=1 2>/dev/null
printf 'CRFD' | dd of="$DATA/feed" bs=1 conv=notrunc 2>/dev/null
printf '\001\000\000\000' | dd of="$DATA/feed" bs=1 seek=48 conv=notrunc 2>/dev/null
channel_has_header "$DATA/feed" 44465243 48
[[ $(print_status | awk -F= '$1=="video_channel_ready"{print $2}') == 0 ]]

nohup() { :; }
wait_feeder() { channel_has_header "$DATA/feed" 44465243 48; }
! start_feeder test-source video video "$fixture/feeder.log"
[[ $(read_u32_hex "$DATA/feed" 48) == 00000000 ]]
for i in {1..20}; do
  [[ -f "$fixture/feeder.log" ]] && break
  command sleep 0.05
done
[[ $(stat -c %a "$fixture/feeder.log") == 600 ]]
printf '%s\n' 'Feeder identity, outbound OBS status, and stale channel checks passed'
