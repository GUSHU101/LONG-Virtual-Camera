#!/system/bin/sh
# LONG Virtual Camera root controller.
#
# The LSPosed module is scoped only to "system". Actual global camera/audio
# replacement remains one native service-level pipeline: cameraserver for
# video/photo and, only after explicit opt-in, audioserver for the microphone.

PATH=/system/bin:/system/xbin:/sbin:/vendor/bin:/data/adb/magisk:$PATH
export PATH

DATA=/data/cr
BASE=$DATA/payload
CONTROL=$DATA/control
MEDIA=$DATA/media
LEGACY_MEDIA=/data/local/tmp/longvc-media
LOCK=$BASE/.longvc-control.lock
INJECTOR=$BASE/cr_injector
FEEDER=$BASE/cr_feed_proc
LEGACY_FEEDER=/data/local/tmp/cr_feed_proc
HOOK=$BASE/libcr_hooks.so
CAM_READY=$DATA/camhook.ready
AUD_READY=$DATA/audhook.ready
MIC_MODE=$DATA/mic_mode
VIDEO_LOG=$BASE/cr_feed.log
AUDIO_LOG=$BASE/cr_feed_audio.log
OBS_LOG=$BASE/cr_feed_obs.log
LAST_CAM=$BASE/cr_last_cam_pkgs
SOURCE_CONFIG=$CONTROL/source.conf
PROC_ROOT=/proc

say() {
  printf '%s\n' "$*"
}

fail() {
  say "result=error"
  say "message=$*"
  exit 1
}

require_root() {
  [ "$(id -u 2>/dev/null)" = "0" ] || fail "root_required"
}

require_payload() {
  [ -x "$INJECTOR" ] || fail "missing_cr_injector"
  [ -x "$FEEDER" ] || fail "missing_cr_feed_proc"
  [ -r "$HOOK" ] || fail "missing_libcr_hooks"
}

acquire_lock() {
  lock_i=0
  while ! mkdir "$LOCK" 2>/dev/null; do
    lock_i=$((lock_i + 1))
    # A hard-killed controller or a reboot can leave the mkdir lock behind.
    # Wait briefly for a just-created owner to publish its PID, then reclaim
    # only a lock whose owner is absent or is no longer this controller.
    if [ "$lock_i" -ge 10 ]; then
      lock_owner=$(cat "$LOCK/pid" 2>/dev/null)
      lock_active=0
      case "$lock_owner" in
        ''|*[!0-9]*) ;;
        *)
          if [ -r "/proc/$lock_owner/cmdline" ]; then
            lock_cmd=$(tr '\000' ' ' < "/proc/$lock_owner/cmdline" 2>/dev/null)
            case "$lock_cmd" in
              *longvc-control.sh*) lock_active=1 ;;
            esac
          fi
          ;;
      esac
      if [ "$lock_active" = "0" ]; then
        stale_lock="$BASE/.longvc-control.stale.$$"
        rm -rf "$stale_lock" 2>/dev/null
        if mv "$LOCK" "$stale_lock" 2>/dev/null; then
          rm -rf "$stale_lock" 2>/dev/null
          continue
        fi
      fi
    fi
    [ "$lock_i" -lt 50 ] || fail "controller_busy"
    sleep 0.1
  done
  trap 'rm -rf "$LOCK" 2>/dev/null' EXIT
  trap 'exit 1' HUP INT TERM
  printf '%s\n' "$$" > "$LOCK/pid" || fail "controller_lock_owner_write_failed"
}

apply_data_context() {
  if command -v chcon >/dev/null 2>&1; then
    chcon u:object_r:system_data_file:s0 "$@" 2>/dev/null && return 0
    [ "$(getenforce 2>/dev/null)" != "Enforcing" ] && return 0
    return 1
  fi
  [ "$(getenforce 2>/dev/null)" != "Enforcing" ]
}

prepare_data() {
  mkdir -p "$DATA" "$CONTROL" "$MEDIA" || return 1
  chmod 0711 "$DATA" || return 1
  chmod 0700 "$CONTROL" || return 1
  chmod 0700 "$MEDIA" || return 1
  touch "$DATA/feed" "$DATA/audio" "$CAM_READY" "$AUD_READY" || return 1
  (chown cameraserver:camera "$DATA/feed" "$CAM_READY" 2>/dev/null ||
    chown 1047:1006 "$DATA/feed" "$CAM_READY") || return 1
  (chown audioserver:audio "$DATA/audio" "$AUD_READY" 2>/dev/null ||
    chown 1041:1005 "$DATA/audio" "$AUD_READY") || return 1
  chmod 0600 "$DATA/feed" "$DATA/audio" "$CAM_READY" "$AUD_READY" || return 1
  apply_data_context "$DATA" "$CONTROL" "$MEDIA" "$DATA/feed" "$DATA/audio" \
    "$CAM_READY" "$AUD_READY" || return 1
}

process_start_time() (
  case "$1" in ''|*[!0-9]*) return 1 ;; esac
  process_stat=$(cat "$PROC_ROOT/$1/stat" 2>/dev/null) || return 1
  # Strip pid and comm, which is parenthesized and may contain spaces.
  process_fields=${process_stat##*) }
  set -f
  set -- $process_fields
  [ "$#" -ge 20 ] || return 1
  case "$1" in Z|X|x) return 1 ;; esac
  case "${20}" in ''|*[!0-9]*) return 1 ;; esac
  say "${20}"
)

feeder_start_time() (
  feeder_started=$(process_start_time "$1") || return 1
  feeder_exe=$(readlink "$PROC_ROOT/$1/exe" 2>/dev/null) || return 1
  case "$feeder_exe" in
    "$FEEDER"|"$FEEDER (deleted)"|"$LEGACY_FEEDER"|"$LEGACY_FEEDER (deleted)") ;;
    *) return 1 ;;
  esac
  feeder_cmd=$(tr '\000' ' ' 2>/dev/null < "$PROC_ROOT/$1/cmdline") || return 1
  case "$feeder_cmd" in *"$2"*) ;; *) return 1 ;; esac
  [ "$(process_start_time "$1")" = "$feeder_started" ] || return 1
  say "$feeder_started"
)

find_feed_pid() (
  for feed_pid in $(pidof cr_feed_proc 2>/dev/null); do
    if feeder_start_time "$feed_pid" "$1" >/dev/null; then
      say "$feed_pid"
      return 0
    fi
  done
  return 1
)

wait_feeders_exit() (
  stop_i=0
  while [ "$stop_i" -lt 20 ]; do
    stop_alive=0
    for stop_identity in "$@"; do
      stop_pid=${stop_identity%%:*}
      if [ "$(process_start_time "$stop_pid")" = "${stop_identity#*:}" ]; then
        stop_alive=1
        break
      fi
    done
    [ "$stop_alive" = 0 ] && return 0
    stop_i=$((stop_i + 1))
    sleep 0.1
  done
  return 1
)

stop_feeder() (
  stop_tag=$1
  set --
  for stop_pid in $(pidof cr_feed_proc 2>/dev/null); do
    stop_started=$(feeder_start_time "$stop_pid" "$stop_tag") || continue
    set -- "$@" "$stop_pid:$stop_started"
    # One process exiting concurrently must not skip the other owned feeders.
    kill -TERM "$stop_pid" 2>/dev/null || true
  done
  wait_feeders_exit "$@" && return 0
  for stop_identity in "$@"; do
    stop_pid=${stop_identity%%:*}
    if [ "$(feeder_start_time "$stop_pid" "$stop_tag")" = "${stop_identity#*:}" ]; then
      kill -KILL "$stop_pid" 2>/dev/null || true
    fi
  done
  wait_feeders_exit "$@"
)

feeder_owns_listener() (
  case "$1" in ''|*[!0-9]*) return 1 ;; esac
  listener_pid=$1
  listener_port=$2
  listener_dir=$PROC_ROOT/$listener_pid
  listener_started=$(process_start_time "$listener_pid") || return 1
  set --
  for listener_table in "$listener_dir/net/tcp" "$listener_dir/net/tcp6"; do
    [ ! -r "$listener_table" ] || set -- "$@" "$listener_table"
  done
  [ "$#" -gt 0 ] || return 1
  listener_inodes=$(awk -v port="$listener_port" '
    $4 == "0A" && $10 ~ /^[1-9][0-9]*$/ {
      split($2, address, ":")
      if (toupper(address[2]) == port) print $10
    }' "$@" 2>/dev/null) || return 1
  [ -n "$listener_inodes" ] || return 1
  for listener_fd in "$listener_dir"/fd/*; do
    listener_target=$(readlink "$listener_fd" 2>/dev/null) || continue
    for listener_inode in $listener_inodes; do
      if [ "$listener_target" = "socket:[$listener_inode]" ] &&
         [ "$(process_start_time "$listener_pid")" = "$listener_started" ]; then
        return 0
      fi
    done
  done
  return 1
)

channel_has_header() {
  [ "$(read_u32_hex "$1" 0)" = "$2" ] &&
    [ "$(read_u32_hex "$1" "$3")" = "00000001" ]
}

read_u32_hex() {
  read_path=$1
  read_offset=$2
  [ -r "$read_path" ] || return 1
  od -An -t x4 -j "$read_offset" -N 4 "$read_path" 2>/dev/null |
    tr -d ' \r\n' | tr 'A-F' 'a-f'
}

revoke_u32() {
  revoke_path=$1
  revoke_offset=$2
  [ -e "$revoke_path" ] || return 0
  dd if=/dev/zero of="$revoke_path" bs=1 seek="$revoke_offset" \
    count=4 conv=notrunc 2>/dev/null || return 1
  [ "$(read_u32_hex "$revoke_path" "$revoke_offset" 2>/dev/null)" = "00000000" ]
}

wait_feeder() {
  wait_tag=$1
  wait_kind=$2
  wait_i=0
  while [ "$wait_i" -lt 100 ]; do
    if wait_pid=$(find_feed_pid "$wait_tag"); then
      case "$wait_kind" in
        video)
          channel_has_header "$DATA/feed" 44465243 48 && return 0
          ;;
        audio)
          channel_has_header "$DATA/audio" 55415243 36 && return 0
          ;;
        obs)
          feeder_owns_listener "$wait_pid" 078F && return 0
          ;;
      esac
    fi
    wait_i=$((wait_i + 1))
    sleep 0.1
  done
  return 1
}

start_feeder() {
  start_url=$1
  start_role=$2
  start_ready=$3
  start_log=$4
  start_tag="--role $start_role"
  if find_feed_pid "$start_tag" >/dev/null 2>&1; then
    if wait_feeder "$start_tag" "$start_ready"; then
      return 0
    fi
    # A live process without its listener can never become READY. Replace it
    # instead of making every later arm attempt fail on the same stale PID.
    stop_feeder "$start_tag" || return 1
  fi
  # A hard-killed writer can leave a READY header behind. Publish readiness
  # only after the new writer has committed data to the ring.
  case "$start_ready" in
    video) revoke_u32 "$DATA/feed" 48 || return 1 ;;
    audio) revoke_u32 "$DATA/audio" 36 || return 1 ;;
  esac
  rm -f "$start_log"
  if command -v nohup >/dev/null 2>&1; then
    LD_LIBRARY_PATH="$BASE" nohup "$FEEDER" --feed "$start_url" --role "$start_role" \
      >"$start_log" 2>&1 </dev/null &
  else
    (LD_LIBRARY_PATH="$BASE" "$FEEDER" --feed "$start_url" --role "$start_role" \
      >"$start_log" 2>&1 </dev/null) &
  fi
  wait_feeder "$start_tag" "$start_ready"
}

hook_pid() {
  hook_proc=$1
  hook_ready=$2
  for hook_candidate in $(pidof "$hook_proc" 2>/dev/null); do
    [ -r "/proc/$hook_candidate/maps" ] || continue
    grep -q '/libcr_hooks\.so' "/proc/$hook_candidate/maps" 2>/dev/null || continue
    [ -r "$hook_ready" ] || continue
    hook_marker=$(tr -dc '0-9' < "$hook_ready" 2>/dev/null)
    [ "$hook_marker" = "$hook_candidate" ] || continue
    say "$hook_candidate"
    return 0
  done
  return 1
}

mapped_hook_pid() {
  mapped_proc=$1
  for mapped_candidate in $(pidof "$mapped_proc" 2>/dev/null); do
    [ -r "/proc/$mapped_candidate/maps" ] || continue
    if grep -q '/libcr_hooks\.so' "/proc/$mapped_candidate/maps" 2>/dev/null; then
      say "$mapped_candidate"
      return 0
    fi
  done
  return 1
}

wait_service() {
  wait_proc=$1
  wait_old_pid=${2:-}
  service_i=0
  while [ "$service_i" -lt 100 ]; do
    for service_pid in $(pidof "$wait_proc" 2>/dev/null); do
      [ "$service_pid" != "$wait_old_pid" ] || continue
      [ -r "/proc/$service_pid/cmdline" ] || continue
      case "$wait_proc" in
        cameraserver)
          service check media.camera 2>/dev/null | grep -q ': found$' || continue
          ;;
        audioserver)
          service check media.audio_flinger 2>/dev/null | grep -q ': found$' || continue
          ;;
      esac
      return 0
    done
    service_i=$((service_i + 1))
    sleep 0.1
  done
  return 1
}

camera_packages() {
  dumpsys media.camera 2>/dev/null |
    awk '
      /^Active Camera Clients:/ {
        active = $0 !~ /\[\]/
        next
      }
      active && /^[[:space:]]*\]|^Allowed user IDs:|^==/ { exit }
      active && /Client Package Name:/ {
        sub(/^.*Client Package Name:[[:space:]]*/, "")
        if (match($0, /^[A-Za-z0-9._]+/)) print substr($0, RSTART, RLENGTH)
      }' |
    sort -u | head -n 3
}

remember_and_close_camera() {
  camera_packages > "$LAST_CAM"
  if [ -s "$LAST_CAM" ]; then
    while IFS= read -r close_pkg; do
      [ -n "$close_pkg" ] || continue
      am force-stop "$close_pkg" >/dev/null 2>&1 || true
    done < "$LAST_CAM"
    sleep 0.3
  fi
}

restore_camera() {
  sleep 0.6
  if [ -s "$LAST_CAM" ]; then
    while IFS= read -r restore_pkg; do
      [ -n "$restore_pkg" ] || continue
      monkey -p "$restore_pkg" -c android.intent.category.LAUNCHER 1 \
        >/dev/null 2>&1 || true
    done < "$LAST_CAM"
  fi
  rm -f "$LAST_CAM"
}

prepare_camera_ready() {
  : > "$CAM_READY" || return 1
  (chown cameraserver:camera "$CAM_READY" 2>/dev/null ||
    chown 1047:1006 "$CAM_READY") || return 1
  chmod 0600 "$CAM_READY" || return 1
  apply_data_context "$CAM_READY"
}

prepare_audio_ready() {
  : > "$AUD_READY" || return 1
  (chown audioserver:audio "$AUD_READY" 2>/dev/null ||
    chown 1041:1005 "$AUD_READY") || return 1
  chmod 0600 "$AUD_READY" || return 1
  apply_data_context "$AUD_READY"
}

ensure_camera_hook() {
  hook_pid cameraserver "$CAM_READY" >/dev/null 2>&1 && return 0

  remember_and_close_camera
  camera_previous=$(pidof -s cameraserver 2>/dev/null)
  if [ -n "$camera_previous" ]; then
    kill -TERM "$camera_previous" 2>/dev/null || { restore_camera; return 1; }
  fi
  if ! wait_service cameraserver "$camera_previous" || ! prepare_camera_ready; then
    restore_camera
    return 1
  fi

  LD_LIBRARY_PATH="$BASE" "$INJECTOR" \
    --target cameraserver --lib "$HOOK" --init cr_camhook_init \
    --feed tcp_h264:listen:8901
  inject_rc=$?
  if [ "$inject_rc" -eq 0 ]; then
    hook_pid cameraserver "$CAM_READY" >/dev/null 2>&1
    inject_rc=$?
  fi
  restore_camera
  return "$inject_rc"
}

ensure_audio_hook() {
  hook_pid audioserver "$AUD_READY" >/dev/null 2>&1 && return 0

  if pidof audioserver >/dev/null 2>&1; then
    killall -q audioserver >/dev/null 2>&1 ||
      killall audioserver >/dev/null 2>&1 || return 1
  fi
  wait_service audioserver || return 1
  prepare_audio_ready || return 1
  LD_LIBRARY_PATH="$BASE" "$INJECTOR" \
    --target audioserver --lib "$HOOK" --init cr_audhook_init \
    --feed tcp_pcm:listen:8902 || return 1
  hook_pid audioserver "$AUD_READY" >/dev/null 2>&1
}

config_value() {
  config_key=$1
  config_file=${2:-$SOURCE_CONFIG}
  [ -r "$config_file" ] || return 1
  [ "$(sed -n 's/^schema=//p' "$config_file" 2>/dev/null | head -n 1)" = 2 ] ||
    return 1
  sed -n "s/^${config_key}=//p" "$config_file" 2>/dev/null | head -n 1
}

valid_sha256() {
  case "$1" in ''|*[!0-9a-f]*) return 1 ;; esac
  [ "${#1}" = 64 ]
}

file_sha256() (
  if command -v sha256sum >/dev/null 2>&1; then
    digest_line=$(sha256sum "$@") || return 1
  else
    digest_line=$(toybox sha256sum "$@") || return 1
  fi
  digest=${digest_line%% *}
  valid_sha256 "$digest" && say "$digest"
)

managed_media_path() (
  media_root=${3:-$MEDIA}
  case "$2" in
    "$media_root/$1-"*.media) ;;
    *) return 1 ;;
  esac
  media_digest=${2#"$media_root/$1-"}
  valid_sha256 "${media_digest%.media}"
)

commit_source() (
  [ "$#" = 3 ] || fail "invalid_source_commit"
  source_name=$1
  case "$source_name" in .source-config-* ) ;; *) fail "invalid_source_commit" ;; esac
  source_suffix=${source_name#.source-config-}
  case "$source_suffix" in ''|*[!0-9a-f-]*) fail "invalid_source_commit" ;; esac
  valid_sha256 "$2" && valid_sha256 "$3" || fail "invalid_source_commit"
  source_stage=$CONTROL/$source_name

  for lease in camera photo mic; do
    [ ! -e "$CONTROL/$lease.lease" ] || fail "source_runtime_active"
  done
  [ -f "$source_stage" ] && [ ! -L "$source_stage" ] ||
    fail "source_stage_missing"
  [ "$(file_sha256 "$source_stage")" = "$3" ] || fail "source_stage_checksum"
  if [ -e "$SOURCE_CONFIG" ]; then
    [ -f "$SOURCE_CONFIG" ] && [ ! -L "$SOURCE_CONFIG" ] || fail "source_config_invalid"
    source_current=$(file_sha256 "$SOURCE_CONFIG") || fail "source_config_unreadable"
  else
    source_current=$(printf 'schema=2\n' | file_sha256) || fail "source_config_unreadable"
  fi
  [ "$source_current" = "$2" ] || fail "source_config_conflict"

  next_visual=$(config_value visual_path "$source_stage") || fail "source_stage_invalid"
  next_audio=$(config_value audio_path "$source_stage") || fail "source_stage_invalid"
  old_visual=$(config_value visual_path 2>/dev/null || true)
  old_audio=$(config_value audio_path 2>/dev/null || true)
  for media_type in visual audio; do
    case "$media_type" in
      visual) next_path=$next_visual; old_path=$old_visual ;;
      audio) next_path=$next_audio; old_path=$old_audio ;;
    esac
    [ -n "$next_path" ] && [ "$next_path" != "$old_path" ] || continue
    managed_media_path "$media_type" "$next_path" &&
      [ -f "$next_path" ] && [ ! -L "$next_path" ] || fail "source_media_missing"
  done
  chown 0:0 "$source_stage" && chmod 0600 "$source_stage" ||
    fail "source_stage_permissions"
  mv -f "$source_stage" "$SOURCE_CONFIG" || fail "source_config_commit_failed"

  # Keep cleanup in the same transaction so concurrent readers never commit a
  # config which references an earlier, already removed media file.
  if [ "$old_visual" != "$next_visual" ] && managed_media_path visual "$old_visual"; then
    rm -f "$old_visual" 2>/dev/null || true
  fi
  if [ "$old_audio" != "$next_audio" ] && managed_media_path audio "$old_audio"; then
    rm -f "$old_audio" 2>/dev/null || true
  fi
)

current_visual_mode() {
  selected_mode=$(config_value visual_mode 2>/dev/null)
  case "$selected_mode" in
    local|obs) say "$selected_mode" ;;
    *) say local ;;
  esac
}

current_audio_mode() {
  selected_mode=$(config_value audio_mode 2>/dev/null)
  case "$selected_mode" in
    local|obs) say "$selected_mode" ;;
    *) say local ;;
  esac
}

current_source_mode() {
  visual_mode=$(current_visual_mode)
  audio_mode=$(current_audio_mode)
  if [ "$visual_mode" = "$audio_mode" ]; then
    say "$visual_mode"
  else
    say hybrid
  fi
}

current_obs_bind() {
  selected_bind=$(config_value obs_bind 2>/dev/null)
  case "$selected_bind" in
    lan) say lan ;;
    *) say loopback ;;
  esac
}

valid_obs_token() {
  token_value=$(config_value obs_token 2>/dev/null)
  case "$token_value" in
    ''|*[!A-Za-z0-9_-]*) return 1 ;;
  esac
  token_length=${#token_value}
  [ "$token_length" -ge 24 ] && [ "$token_length" -le 96 ]
}

ensure_obs_feeder() {
  valid_obs_token || return 1
  obs_tracks=$1
  case "$obs_tracks" in video|audio|av) ;; *) return 1 ;; esac
  obs_bind=$(current_obs_bind)
  obs_role="obs-$obs_tracks"
  if find_feed_pid "--role $obs_role" >/dev/null 2>&1 &&
     wait_feeder "--role $obs_role" obs; then
    return 0
  fi
  stop_feeder "--role obs" || return 1
  start_feeder "rtmp:listen:1935:config:$obs_bind:$obs_tracks" \
    "$obs_role" obs "$OBS_LOG"
}

local_visual_url() {
  visual_source=$(config_value visual_path 2>/dev/null)
  managed_media_path visual "$visual_source" ||
    managed_media_path visual "$visual_source" "$LEGACY_MEDIA" || return 1
  [ -f "$visual_source" ] && [ ! -L "$visual_source" ] && [ -r "$visual_source" ] || return 1
  visual_kind=$(config_value visual_kind 2>/dev/null)
  case "$visual_kind" in
    video) say "file_video:$visual_source" ;;
    image) say "file_nv21:$visual_source" ;;
    *) return 1 ;;
  esac
}

local_audio_url() {
  audio_source=$(config_value audio_path 2>/dev/null)
  managed_media_path audio "$audio_source" ||
    managed_media_path audio "$audio_source" "$LEGACY_MEDIA" || return 1
  [ -f "$audio_source" ] && [ ! -L "$audio_source" ] && [ -r "$audio_source" ] || return 1
  say "file_audio:$audio_source"
}

ensure_video() {
  prepare_data || return 1
  if [ "$(current_visual_mode)" = obs ]; then
    obs_tracks=video
    if [ -e "$CONTROL/mic.lease" ] && [ "$(current_audio_mode)" = obs ]; then
      obs_tracks=av
    fi
    ensure_obs_feeder "$obs_tracks" || return 1
  else
    video_url=$(local_visual_url) || return 1
    start_feeder "$video_url" video video "$VIDEO_LOG" || return 1
  fi
  if ! ensure_camera_hook; then
    if [ "$(current_visual_mode)" = obs ]; then
      if [ ! -e "$CONTROL/mic.lease" ] || [ "$(current_audio_mode)" != obs ]; then
        stop_feeder "--role obs"
      fi
    else
      stop_feeder "--role video"
    fi
    revoke_u32 "$DATA/feed" 0 || true
    return 1
  fi
}

ensure_audio() {
  prepare_data || return 1
  if [ "$(current_audio_mode)" = obs ]; then
    obs_tracks=audio
    if { [ -e "$CONTROL/camera.lease" ] || [ -e "$CONTROL/photo.lease" ]; } &&
       [ "$(current_visual_mode)" = obs ]; then
      obs_tracks=av
    fi
    ensure_obs_feeder "$obs_tracks" || return 1
  else
    audio_url=$(local_audio_url) || return 1
    start_feeder "$audio_url" audio audio "$AUDIO_LOG" || return 1
  fi
  if ! publish_mic_mode || ! ensure_audio_hook; then
    if [ "$(current_audio_mode)" = obs ]; then
      [ "$(current_visual_mode)" = obs ] &&
        { [ -e "$CONTROL/camera.lease" ] || [ -e "$CONTROL/photo.lease" ]; } ||
        stop_feeder "--role obs"
    else
      stop_feeder "--role audio"
    fi
    revoke_u32 "$DATA/audio" 0 || true
    return 1
  fi
}

stop_video_passthrough() {
  stop_rc=0
  if [ "$(current_visual_mode)" = obs ]; then
    if [ ! -e "$CONTROL/mic.lease" ] || [ "$(current_audio_mode)" != obs ]; then
      stop_feeder "--role obs" || stop_rc=1
    else
      ensure_obs_feeder audio || stop_rc=1
    fi
    revoke_u32 "$DATA/feed" 0 || stop_rc=1
  else
    if [ -f "$DATA/feed" ]; then
      revoke_u32 "$DATA/feed" 48 || stop_rc=1
      sleep 0.35
    fi
    stop_feeder "--role video" || stop_rc=1
    revoke_u32 "$DATA/feed" 0 || stop_rc=1
  fi
  # Migration cleanup for payloads launched by the former desktop transport.
  stop_feeder tcp_h264 || stop_rc=1
  return "$stop_rc"
}

stop_audio_passthrough() {
  stop_rc=0
  if [ "$(current_audio_mode)" = obs ]; then
    if { [ ! -e "$CONTROL/camera.lease" ] &&
         [ ! -e "$CONTROL/photo.lease" ]; } ||
       [ "$(current_visual_mode)" != obs ]; then
      stop_feeder "--role obs" || stop_rc=1
    else
      ensure_obs_feeder video || stop_rc=1
    fi
    revoke_u32 "$DATA/audio" 0 || stop_rc=1
  else
    stop_feeder "--role audio" || stop_rc=1
    revoke_u32 "$DATA/audio" 0 || stop_rc=1
  fi
  stop_feeder tcp_pcm || stop_rc=1
  return "$stop_rc"
}

reconcile_stale_leases() {
  # /data survives a reboot while injected mappings and feeder processes do
  # not. Never interpret a stale marker as an instruction to auto-enable,
  # especially for the opt-in microphone path.
  video_live=0
  if find_feed_pid "--role video" >/dev/null 2>&1 ||
     find_feed_pid "--role obs-video" >/dev/null 2>&1 ||
     find_feed_pid "--role obs-av" >/dev/null 2>&1 ||
     mapped_hook_pid cameraserver >/dev/null 2>&1; then
    video_live=1
  fi
  if [ "$video_live" = "0" ]; then
    rm -f "$CONTROL/camera.lease" "$CONTROL/photo.lease" \
      "$DATA/photo_mode" "$DATA/video_mode"
  fi

  audio_live=0
  if find_feed_pid "--role audio" >/dev/null 2>&1 ||
     find_feed_pid "--role obs-audio" >/dev/null 2>&1 ||
     find_feed_pid "--role obs-av" >/dev/null 2>&1 ||
     mapped_hook_pid audioserver >/dev/null 2>&1; then
    audio_live=1
  fi
  if [ "$audio_live" = "0" ]; then
    rm -f "$CONTROL/mic.lease" "$MIC_MODE"
  fi
}

unload_camera_hook() {
  if ! old_camera_pid=$(mapped_hook_pid cameraserver); then
    rm -f "$CAM_READY" && wait_service cameraserver
    return $?
  fi
  remember_and_close_camera
  rm -f "$CAM_READY"
  if ! killall -q cameraserver >/dev/null 2>&1 &&
     ! killall cameraserver >/dev/null 2>&1; then
    restore_camera
    return 1
  fi
  if ! wait_service cameraserver "$old_camera_pid" ||
     mapped_hook_pid cameraserver >/dev/null 2>&1; then
    restore_camera
    return 1
  fi
  restore_camera
  return 0
}

unload_audio_hook() {
  if ! old_audio_pid=$(mapped_hook_pid audioserver); then
    rm -f "$AUD_READY" && wait_service audioserver
    return $?
  fi
  rm -f "$AUD_READY"
  if ! killall -q audioserver >/dev/null 2>&1 &&
     ! killall audioserver >/dev/null 2>&1; then
    return 1
  fi
  wait_service audioserver "$old_audio_pid" || return 1
  ! mapped_hook_pid audioserver >/dev/null 2>&1
}

install_camera_flag() {
  flag_path=$1
  printf '1\n' > "$flag_path" || return 1
  (chown cameraserver:camera "$flag_path" 2>/dev/null ||
    chown 1047:1006 "$flag_path") || return 1
  chmod 0400 "$flag_path" || return 1
  apply_data_context "$flag_path"
}

arm_camera() {
  if ! (: > "$CONTROL/camera.lease" && chmod 0600 "$CONTROL/camera.lease"); then
    rm -f "$CONTROL/camera.lease"
    return 1
  fi
  if ! ensure_video; then
    rm -f "$CONTROL/camera.lease"
    if [ ! -e "$CONTROL/photo.lease" ]; then
      stop_video_passthrough
      unload_camera_hook
    fi
    return 1
  fi
  if ! install_camera_flag "$DATA/video_mode"; then
    disarm_camera
    return 1
  fi
}

disarm_camera() {
  rm -f "$CONTROL/camera.lease" "$DATA/video_mode" || return 1
  if [ ! -e "$CONTROL/photo.lease" ]; then
    disarm_rc=0
    stop_video_passthrough || disarm_rc=1
    unload_camera_hook || disarm_rc=1
    return "$disarm_rc"
  fi
}

arm_photo() {
  if ! (: > "$CONTROL/photo.lease" && chmod 0600 "$CONTROL/photo.lease"); then
    rm -f "$CONTROL/photo.lease"
    return 1
  fi
  if ! ensure_video; then
    rm -f "$CONTROL/photo.lease" "$DATA/photo_mode"
    if [ ! -e "$CONTROL/camera.lease" ]; then
      stop_video_passthrough
      unload_camera_hook
    fi
    return 1
  fi
  if ! install_camera_flag "$DATA/photo_mode"; then
    disarm_photo
    return 1
  fi
}

disarm_photo() {
  rm -f "$CONTROL/photo.lease" "$DATA/photo_mode" || return 1
  if [ ! -e "$CONTROL/camera.lease" ]; then
    disarm_rc=0
    stop_video_passthrough || disarm_rc=1
    unload_camera_hook || disarm_rc=1
    return "$disarm_rc"
  fi
}

publish_mic_mode() {
  mic_stage=$DATA/.mic-mode.$$
  if (umask 077; set -C; printf '1\n' > "$mic_stage") &&
     chown 0:0 "$mic_stage" && chmod 0400 "$mic_stage" &&
     apply_data_context "$mic_stage" &&
     mv -f "$mic_stage" "$MIC_MODE"; then
    return 0
  fi
  rm -f "$mic_stage"
  return 1
}

arm_mic() {
  if ! (: > "$CONTROL/mic.lease" && chmod 0600 "$CONTROL/mic.lease"); then
    rm -f "$CONTROL/mic.lease" "$MIC_MODE"
    return 1
  fi
  if ! ensure_audio; then
    rm -f "$CONTROL/mic.lease" "$MIC_MODE"
    stop_audio_passthrough
    unload_audio_hook
    return 1
  fi
}

disarm_mic() {
  disarm_rc=0
  rm -f "$CONTROL/mic.lease" "$MIC_MODE" || disarm_rc=1
  stop_audio_passthrough || disarm_rc=1
  unload_audio_hook || disarm_rc=1
  return "$disarm_rc"
}

stop_all() {
  stop_all_rc=0
  rm -f "$CONTROL/camera.lease" "$CONTROL/photo.lease" \
    "$CONTROL/mic.lease" "$MIC_MODE" "$DATA/photo_mode" "$DATA/video_mode" || stop_all_rc=1
  # A partial transaction can leave a feeder from the previous source.
  # Recovery must cover every owned feeder regardless of the saved plan.
  revoke_u32 "$DATA/feed" 48 || stop_all_rc=1
  revoke_u32 "$DATA/audio" 36 || stop_all_rc=1
  stop_feeder "" || stop_all_rc=1
  revoke_u32 "$DATA/feed" 0 || stop_all_rc=1
  revoke_u32 "$DATA/audio" 0 || stop_all_rc=1
  unload_camera_hook || stop_all_rc=1
  unload_audio_hook || stop_all_rc=1
  return "$stop_all_rc"
}

camera_output_stats() {
  case "$1" in ''|*[!0-9]*) return ;; esac
  # Only export structured counters from the currently mapped cameraserver.
  # Older payloads and devices without log access simply omit these fields.
  logcat --pid="$1" -b main -d -v raw -s cr_camhook:I 2>/dev/null |
    awk '(/^camera_output v1 yuv=[0-9]+ jpeg=[0-9]+ raw10=[0-9]+ unsupported=[0-9]+$/ ||
          /^camera_output v2 yuv=[0-9]+ jpeg=[0-9]+ raw10=[0-9]+ unsupported=[0-9]+ queued_yuv=[0-9]+ queued_jpeg=[0-9]+ queue_error=[0-9]+$/) {
      split($3, yuv, "="); split($4, jpeg, "=")
      split($5, raw, "="); split($6, unsupported, "=")
      found = 1
      queued = ($2 == "v2")
      if (queued) {
        split($7, queued_yuv, "="); split($8, queued_jpeg, "=")
        split($9, queue_error, "=")
      }
    }
    /^camera_preview v1 seen=[0-9]+ staged=[0-9]+ queue_error=[0-9]+$/ {
      split($3, preview_seen, "=")
      split($4, preview_staged, "=")
      split($5, preview_errors, "=")
    }
    END {
      if (found) {
        print "camera_written_yuv=" yuv[2]
        print "camera_written_jpeg=" jpeg[2]
        print "camera_raw_ten=" raw[2]
        print "camera_unsupported=" unsupported[2]
        if (queued) {
          print "camera_queued_yuv=" queued_yuv[2]
          print "camera_queued_jpeg=" queued_jpeg[2]
          print "camera_queue_errors=" queue_error[2]
        }
      }
      if (preview_seen[2] != "") {
        print "camera_preview_seen=" preview_seen[2]
        print "camera_preview_staged=" preview_staged[2]
        print "camera_preview_errors=" preview_errors[2]
      }
    }'
}

print_status() {
  reconcile_stale_leases
  [ -x "$INJECTOR" ] && [ -x "$FEEDER" ] && [ -r "$HOOK" ] && installed=1 || installed=0
  visual_mode=$(current_visual_mode)
  audio_mode=$(current_audio_mode)
  source_mode=$(current_source_mode)
  obs_bind=$(current_obs_bind)
  obs_configured=0
  if { [ "$visual_mode" = obs ] || [ "$audio_mode" = obs ]; } &&
     valid_obs_token; then
    obs_configured=1
  fi
  if obs_pid=$(find_feed_pid "--role obs") && feeder_owns_listener "$obs_pid" 078F; then
    obs_listener=1
  else
    obs_listener=0
  fi

  visual_kind=$(config_value visual_kind 2>/dev/null)
  case "$visual_kind" in video|image) ;; *) visual_kind=none ;; esac
  if [ "$visual_mode" = obs ]; then
    visual_source_ready=$obs_configured
    if [ "$obs_listener" = "1" ] &&
       { find_feed_pid "--role obs-video" >/dev/null 2>&1 ||
         find_feed_pid "--role obs-av" >/dev/null 2>&1; }; then
      video_feed=1
    else
      video_feed=0
    fi
  else
    local_visual_url >/dev/null 2>&1 && visual_source_ready=1 || visual_source_ready=0
    find_feed_pid "--role video" >/dev/null 2>&1 && video_feed=1 || video_feed=0
  fi

  if [ "$audio_mode" = obs ]; then
    audio_source_ready=$obs_configured
    if [ "$obs_listener" = "1" ] &&
       { find_feed_pid "--role obs-audio" >/dev/null 2>&1 ||
         find_feed_pid "--role obs-av" >/dev/null 2>&1; }; then
      audio_feed=1
    else
      audio_feed=0
    fi
  else
    local_audio_url >/dev/null 2>&1 && audio_source_ready=1 || audio_source_ready=0
    find_feed_pid "--role audio" >/dev/null 2>&1 && audio_feed=1 || audio_feed=0
  fi
  [ "$video_feed" = "1" ] && channel_has_header "$DATA/feed" 44465243 48 && video_channel=1 || video_channel=0
  [ "$audio_feed" = "1" ] && channel_has_header "$DATA/audio" 55415243 36 && audio_channel=1 || audio_channel=0
  camera_hook=$(hook_pid cameraserver "$CAM_READY" 2>/dev/null || true)
  audio_hook=$(hook_pid audioserver "$AUD_READY" 2>/dev/null || true)
  [ -n "$camera_hook" ] && camera_hook_ready=1 || camera_hook_ready=0
  [ -n "$audio_hook" ] && audio_hook_ready=1 || audio_hook_ready=0
  [ -e "$CONTROL/camera.lease" ] && camera_enabled=1 || camera_enabled=0
  [ -e "$CONTROL/photo.lease" ] && [ -e "$DATA/photo_mode" ] && photo_enabled=1 || photo_enabled=0
  [ -e "$CONTROL/mic.lease" ] && [ -f "$MIC_MODE" ] && audio_enabled=1 || audio_enabled=0

  say "schema=1"
  say "root=1"
  say "abi=$(getprop ro.product.cpu.abi 2>/dev/null)"
  say "installed=$installed"
  say "source_mode=$source_mode"
  say "visual_mode=$visual_mode"
  say "audio_mode=$audio_mode"
  say "visual_kind=${visual_kind:-none}"
  say "visual_source_ready=$visual_source_ready"
  say "audio_source_ready=$audio_source_ready"
  say "obs_configured=$obs_configured"
  say "obs_listener_ready=$obs_listener"
  say "obs_bind=$obs_bind"
  say "camera_enabled=$camera_enabled"
  say "photo_enabled=$photo_enabled"
  say "mic_enabled=$audio_enabled"
  say "video_feeder_ready=$video_feed"
  say "audio_feeder_ready=$audio_feed"
  say "video_channel_ready=$video_channel"
  say "audio_channel_ready=$audio_channel"
  say "camera_hook_ready=$camera_hook_ready"
  say "audio_hook_ready=$audio_hook_ready"
  say "camera_hook_pid=$camera_hook"
  say "audio_hook_pid=$audio_hook"
  camera_output_stats "$camera_hook"
  say "selinux=$(getenforce 2>/dev/null)"
  say "result=ok"
}

require_root
action=${1:-status}

case "$action" in
  status)
    acquire_lock
    print_status
    exit 0
    ;;
  arm-camera|arm-camera-fresh|arm-photo|arm-mic|arm-mic-fresh)
    require_payload
    acquire_lock
    reconcile_stale_leases
    ;;
  disarm-camera|disarm-photo|disarm-mic|stop-all)
    # Recovery must remain available when a native payload is missing or
    # corrupt. These actions need only the control script itself.
    acquire_lock
    reconcile_stale_leases
    ;;
  commit-source)
    acquire_lock
    shift
    commit_source "$@" || exit 1
    ;;
  uninstall)
    acquire_lock
    ;;
  *)
    fail "unknown_action"
    ;;
esac

case "$action" in
  arm-camera) arm_camera || fail "camera_arm_failed" ;;
  arm-camera-fresh)
    rm -f "$CAM_READY"
    arm_camera || fail "camera_fresh_arm_failed"
    ;;
  disarm-camera) disarm_camera || fail "camera_disarm_failed" ;;
  arm-photo) arm_photo || fail "photo_arm_failed" ;;
  disarm-photo) disarm_photo || fail "photo_disarm_failed" ;;
  arm-mic) arm_mic || fail "microphone_arm_failed" ;;
  arm-mic-fresh)
    rm -f "$AUD_READY"
    arm_mic || fail "microphone_fresh_arm_failed"
    ;;
  disarm-mic) disarm_mic || fail "microphone_disarm_failed" ;;
  stop-all) stop_all || fail "stop_all_failed" ;;
  uninstall)
    stop_all || fail "uninstall_stop_failed"
    rm -f "$INJECTOR" "$FEEDER" "$HOOK" "$BASE/longvc-control.sh" \
      "$BASE/longvc-payload.sha256" "$VIDEO_LOG" "$AUDIO_LOG" "$OBS_LOG"
    rm -rf "$MEDIA" "$CONTROL"
    ;;
esac

say "result=ok"
say "action=$action"
exit 0
