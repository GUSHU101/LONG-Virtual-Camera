#!/usr/bin/env bash
set -euo pipefail

# Load the real function bodies, without running the Android entry point.
source <(sed '/^require_root$/,$d' lsposed-module/app/src/main/payload/longvc-control.sh)
fixture=$(mktemp -d)
trap 'rmdir "$fixture"' EXIT
DATA=$fixture
CONTROL=$fixture
MIC_MODE=$fixture/mic_mode

verify_steps() {
  local fail_step=$1 expected_rc=$2 rc=0
  steps=""
  touch "$MIC_MODE"
  revoke_u32() { steps+=" revoke"; [[ $fail_step != revoke ]]; }
  stop_feeder() { steps+=" feeder"; [[ $fail_step != feeder ]]; }
  unload_camera_hook() { steps+=" camera"; [[ $fail_step != camera ]]; }
  unload_audio_hook() { steps+=" audio"; [[ $fail_step != audio ]]; }
  stop_all || rc=$?
  [[ $steps == " revoke revoke feeder revoke revoke camera audio" ]]
  [[ $rc == "$expected_rc" ]]
  [[ ! -e "$MIC_MODE" ]]
}

verify_steps none 0
verify_steps revoke 1
verify_steps feeder 1
verify_steps camera 1
verify_steps audio 1
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
    'camera_preview v1 seen=91 staged=90 queue_error=-1'
}
[[ $(camera_output_stats 42) == $'camera_preview_seen=90\ncamera_preview_staged=88\ncamera_preview_errors=1' ]]

dumpsys() { printf '%s\n' "$camera_dump"; }
camera_history=$'== Camera service events log ==\nDISCONNECT device 0 client for package com.example.previous\nClient Package Name: com.example.closed'
camera_dump=$'Active Camera Clients:\n[]\nAllowed user IDs: 0\n'"$camera_history"
[[ -z $(camera_packages) ]]
camera_dump=$'Active Camera Clients:\n[\n(Camera ID: 0, Client Package Name: com.example.active, Conflicting Client Devices: {})\n]\n'"$camera_history"
[[ $(camera_packages) == com.example.active ]]

(
  BASE=$fixture
  INJECTOR=true
  HOOK=$fixture/libcr_hooks.so
  CAM_READY=$fixture/camhook.ready
  pidof() { return 1; }
  wait_service() { return 0; }
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
  activation=off reopened_pid=42
  ensure_camera_hook || exit 1
  activation=off reopened_pid=43
  if ensure_camera_hook; then exit 1; fi
  activation=off reopened_pid=
  if ensure_camera_hook; then exit 1; fi
)

ensure_video() { steps+=" video"; }
install_camera_flag() { steps+=" flag"; [[ $flag_ready == 1 ]]; }
disarm_photo() { steps+=" rollback"; rm -f "$CONTROL/photo.lease"; }
steps=""
flag_ready=0
if arm_photo; then exit 1; fi
[[ $steps == ' video flag rollback' && ! -e "$CONTROL/photo.lease" ]]
