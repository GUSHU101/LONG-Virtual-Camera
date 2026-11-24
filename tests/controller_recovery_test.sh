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
