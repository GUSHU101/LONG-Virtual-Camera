#!/usr/bin/env bash
set -euo pipefail

# Load the real function bodies, without running the Android entry point.
source <(sed '/^require_root$/,$d' lsposed-module/app/src/main/payload/longvc-control.sh)
fixture=$(mktemp -d)
trap 'rmdir "$fixture"' EXIT
DATA=$fixture
CONTROL=$fixture

verify_steps() {
  local fail_step=$1 expected_rc=$2 rc=0
  steps=""
  revoke_u32() { steps+=" revoke"; [[ $fail_step != revoke ]]; }
  stop_feeder() { steps+=" feeder"; [[ $fail_step != feeder ]]; }
  unload_camera_hook() { steps+=" camera"; [[ $fail_step != camera ]]; }
  unload_audio_hook() { steps+=" audio"; [[ $fail_step != audio ]]; }
  stop_all || rc=$?
  [[ $steps == " revoke revoke feeder revoke revoke camera audio" ]]
  [[ $rc == "$expected_rc" ]]
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

dumpsys() { printf '%s\n' "$camera_dump"; }
camera_history=$'== Camera service events log ==\nDISCONNECT device 0 client for package com.example.previous\nClient Package Name: com.example.closed'
camera_dump=$'Active Camera Clients:\n[]\nAllowed user IDs: 0\n'"$camera_history"
[[ -z $(camera_packages) ]]
camera_dump=$'Active Camera Clients:\n[\n(Camera ID: 0, Client Package Name: com.example.active, Conflicting Client Devices: {})\n]\n'"$camera_history"
[[ $(camera_packages) == com.example.active ]]

ensure_video() { steps+=" video"; }
install_camera_flag() { steps+=" flag"; [[ $flag_ready == 1 ]]; }
disarm_photo() { steps+=" rollback"; rm -f "$CONTROL/photo.lease"; }
steps=""
flag_ready=0
if arm_photo; then exit 1; fi
[[ $steps == ' video flag rollback' && ! -e "$CONTROL/photo.lease" ]]
