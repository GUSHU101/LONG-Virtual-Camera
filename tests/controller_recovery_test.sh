#!/usr/bin/env bash
set -euo pipefail

# Load the real function bodies, without running the Android entry point.
source <(sed '/^require_root$/,$d' lsposed-module/app/src/main/payload/longvc-control.sh)
fixture=$(mktemp -d)
trap 'rmdir "$fixture"' EXIT
DATA=$fixture
CONTROL=$fixture
MIC_MODE=$fixture/mic_mode

(
  CAM_READY=$fixture/camhook.ready
  AUD_READY=$fixture/audhook.ready
  reported_pid=42 map_result=1 service_result=0
  pidof() {
    [[ ( $1 == cameraserver || $1 == audioserver ) && -n "$reported_pid" ]] || return 1
    printf '%s\n' "$reported_pid"
  }
  grep() {
    case "${3:-}" in /proc/*/maps) return "$map_result" ;; esac
    command grep "$@"
  }
  service() {
    [[ $1 == check && ( $2 == media.camera || $2 == media.audio_flinger ) &&
       $service_result == 0 ]] || return 1
    printf 'Service %s: found\n' "$2"
  }
  [[ $(camera_runtime_state) == stock ]]
  map_result=0
  [[ $(camera_runtime_state) == hook_mapped ]]
  map_result=2
  [[ $(camera_runtime_state) == unknown ]]
  map_result=1 service_result=1
  [[ $(camera_runtime_state) == unknown ]]
  service_result=0 reported_pid=
  [[ $(camera_runtime_state) == unknown ]]

  reported_pid=42 map_result=2
  wait_service() { return 0; }
  touch "$CAM_READY"
  if unload_camera_hook; then exit 1; fi
  [[ ! -e "$CAM_READY" ]]
  touch "$AUD_READY"
  if unload_audio_hook; then exit 1; fi
  [[ ! -e "$AUD_READY" && $(audio_runtime_state) == unknown ]]
  map_result=1
  unload_camera_hook
  unload_audio_hook
)

verify_steps() {
  local fail_step=$1 expected_rc=$2 rc=0
  steps=""
  touch "$MIC_MODE"
  revoke_u32() { steps+=" revoke"; [[ $fail_step != revoke ]]; }
  stop_feeder() { steps+=" feeder"; [[ $fail_step != feeder ]]; }
  unload_camera_hook() { steps+=" camera"; [[ $fail_step != camera ]]; }
  unload_audio_hook() { steps+=" audio"; [[ $fail_step != audio ]]; }
  feeder_runtime_state() {
    case "$fail_step" in
      residual) printf 'active\n' ;;
      scan) printf 'unknown\n' ;;
      *) printf 'idle\n' ;;
    esac
  }
  stop_all || rc=$?
  [[ $steps == " feeder revoke revoke revoke revoke camera audio" ]]
  [[ $rc == "$expected_rc" ]]
  [[ ! -e "$MIC_MODE" ]]
}

verify_steps none 0
verify_steps revoke 1
verify_steps feeder 1
verify_steps camera 1
verify_steps audio 1
verify_steps residual 1
verify_steps scan 1
printf '%s\n' 'Recovery propagated every simulated failure and ran all cleanup steps'

logcat() {
  [[ $1 == --pid=42 ]] || return 1
  printf '%s\n' 'camera_output v1 yuv=29 jpeg=0 raw10=30 unsupported=30' \
    'camera_output v1 yuv=59 jpeg=0 raw10=60 unsupported=61' \
    'camera_output v1 yuv=999 jpeg=-1 raw10=999 unsupported=999'
}
[[ $(camera_output_stats 42) == $'camera_written_yuv=59\ncamera_written_jpeg=0\ncamera_raw_ten=60\ncamera_unsupported=61' ]]
[[ -z $(camera_output_stats 43) && -z $(camera_output_stats invalid) ]]
logcat() {
  printf '%s\n' 'camera_output v1 yuv=59 jpeg=0 raw10=60 unsupported=61' \
    'camera_output v2 yuv=62 jpeg=1 raw10=63 unsupported=64 queued_yuv=60 queued_jpeg=1 queue_error=2'
}
[[ $(camera_output_stats 42) == $'camera_written_yuv=62\ncamera_written_jpeg=1\ncamera_raw_ten=63\ncamera_unsupported=64\ncamera_queued_yuv=60\ncamera_queued_jpeg=1\ncamera_queue_errors=2' ]]
logcat() {
  printf '%s\n' 'camera_preview v1 seen=60 staged=58 queue_error=1' \
    'camera_preview v1 seen=90 staged=88 queue_error=1' \
    'camera_preview v1 seen=91 staged=90 queue_error=-1' \
    'camera_recording v1 queued=42' \
    'camera_recording v1 queued=-1'
}
[[ $(camera_output_stats 42) == $'camera_preview_seen=90\ncamera_preview_staged=88\ncamera_preview_errors=1\ncamera_recording_queued=42' ]]
logcat() {
  printf '%s\n' 'camera_raw10 v1 written=90 queued=89 lock_error=0' \
    'camera_raw10 v1 written=91 queued=-1 lock_error=0'
}
[[ $(camera_output_stats 42) == $'camera_written_raw_ten=90\ncamera_queued_raw_ten=89\ncamera_raw_ten_lock_errors=0' ]]

dumpsys() { printf '%s\n' "$camera_dump"; }
camera_history=$'== Camera service events log ==\nDISCONNECT device 0 client for package com.example.previous\nClient Package Name: com.example.closed'
camera_dump=$'Active Camera Clients:\n[]\nAllowed user IDs: 0\n'"$camera_history"
[[ -z $(camera_packages) ]]
camera_dump=$'Active Camera Clients:\n[\n(Camera ID: 0, Client Package Name: com.example.active, Conflicting Client Devices: {})\n]\n'"$camera_history"
[[ $(camera_packages) == com.example.active ]]

(
  stops=0
  stop_video_passthrough() { stops=$((stops + 1)); }
  unload_camera_hook() { exit 7; }
  touch "$CONTROL/camera.lease" "$DATA/video_mode" "$CONTROL/photo.lease" "$DATA/photo_mode"
  disarm_camera
  [[ $stops == 0 && ! -e "$DATA/video_mode" && -e "$DATA/photo_mode" ]]
  disarm_photo
  [[ $stops == 1 && ! -e "$DATA/photo_mode" ]]
  touch "$CONTROL/camera.lease" "$DATA/video_mode"
  disarm_camera
  [[ $stops == 2 && ! -e "$DATA/video_mode" ]]
)

(
  BASE=$fixture
  INJECTOR=true
  HOOK=$fixture/libcr_hooks.so
  CAM_READY=$fixture/camhook.ready
  wait_service() { return 0; }
  mapped_hook_pid() {
    [ -n "$mapped_pid" ] || return 1
    printf '%s\n' "$mapped_pid"
  }
  kill() { killed_pid=$2; }
  sleep() { :; }
  prepare_camera_ready() { return 0; }
  remember_and_close_camera() { activation=loading; }
  restore_camera() { activation=reopened; }
  hook_pid() {
    case "$activation" in
      loading) printf '%s\n' 42 ;;
      reopened) [ -n "$reopened_pid" ] || return 1; printf '%s\n' "$reopened_pid" ;;
      *) return 1 ;;
    esac
  }
  activation=off reopened_pid=42 mapped_pid= killed_pid=
  ensure_camera_hook || exit 1
  [[ -z $killed_pid ]]
  activation=off reopened_pid=43 mapped_pid=40
  if ensure_camera_hook; then exit 1; fi
  [[ $killed_pid == 40 ]]
  activation=off reopened_pid= mapped_pid=
  if ensure_camera_hook; then exit 1; fi
)

(
  ensure_video() { steps+=" video"; }
  install_camera_flag() { steps+=" flag"; return 1; }
  stop_video_passthrough() { steps+=" feeder"; [[ $fail_step != feeder ]]; }
  unload_camera_hook() { steps+=" hook"; [[ $fail_step != hook ]]; }
  for fail_step in none feeder hook; do
    steps=""
    if arm_photo > "$fixture/arm_failure"; then exit 1; fi
    [[ $steps == ' video flag feeder hook' && ! -e "$CONTROL/photo.lease" ]]
    if [[ $fail_step == none ]]; then
      [[ ! -s "$fixture/arm_failure" ]]
    else
      [[ $(cat "$fixture/arm_failure") == camera_recovery=unverified ]]
    fi
  done
  rm -f "$fixture/arm_failure"
)

(
  prepare_data() { return 0; }
  current_visual_mode() { printf 'local\n'; }
  local_visual_url() { printf 'test\n'; }
  start_feeder() { return 0; }
  ensure_camera_hook() { return 1; }
  stop_feeder() { exit 8; }
  revoke_u32() { exit 9; }
  unload_camera_hook() { exit 10; }
  touch "$CONTROL/camera.lease" "$DATA/video_mode"
  if arm_photo || arm_camera; then exit 1; fi
  [[ -f "$CONTROL/camera.lease" && -f "$DATA/video_mode" && ! -e "$CONTROL/photo.lease" ]]
  rm -f "$CONTROL/camera.lease" "$DATA/video_mode"
)
touch "$CONTROL/photo.lease" "$DATA/video_mode"
reconcile_mode_pair "$CONTROL/photo.lease" "$DATA/photo_mode"
reconcile_mode_pair "$CONTROL/camera.lease" "$DATA/video_mode"
[[ ! -e "$CONTROL/photo.lease" && ! -e "$DATA/video_mode" ]]

(
  ensure_video() { [[ ! -e "$DATA/video_mode" ]]; }
  install_camera_flag() { : > "$1"; }
  ensure_camera_hook() { [[ -f "$DATA/video_mode" ]]; }
  arm_camera
  [[ -f "$CONTROL/camera.lease" && -f "$DATA/video_mode" ]]
  rm -f "$CONTROL/camera.lease" "$DATA/video_mode"
)

if command -v flock >/dev/null 2>&1; then
  BASE=$fixture
  LOCK=$fixture/controller
  (acquire_lock; printf 'first\n' >> "$fixture/order"; sleep 0.3; printf 'release\n' >> "$fixture/order") &
  first=$!
  for ((i=0; i<100; i++)); do
    [[ -s "$fixture/order" ]] && break
    sleep 0.01
  done
  [[ -s "$fixture/order" ]]
  (acquire_lock; printf 'second\n' >> "$fixture/order") &
  second=$!
  wait "$first" "$second"
  [[ $(cat "$fixture/order") == $'first\nrelease\nsecond' ]]
  rm -f "$fixture/order" "$LOCK.flock"
fi
