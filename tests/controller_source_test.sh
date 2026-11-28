#!/usr/bin/env bash
set -euo pipefail

source <(sed '/^require_root$/,$d' lsposed-module/app/src/main/payload/longvc-control.sh)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
CONTROL=$fixture/control
DATA=$fixture
MEDIA=$fixture/media
MIC_MODE=$DATA/mic_mode
SOURCE_CONFIG=$CONTROL/source.conf
mkdir -p "$CONTROL" "$MEDIA"
chown() { :; } # CI has no root. The real function still checks chmod and rename.
feeder=idle
feeder_runtime_state() { printf '%s\n' "$feeder"; }

old_file=$MEDIA/visual-$(printf '%064d' 1).media
next_file=$MEDIA/visual-$(printf '%064d' 2).media
missing_file=$MEDIA/visual-$(printf '%064d' 3).media
touch "$old_file" "$next_file"

stage_name=.source-config-79
stage=$CONTROL/$stage_name
prepare() { printf 'schema=2\nvisual_path=%s\naudio_path=\n' "$1" > "$stage"; }
commit() { (commit_source "$stage_name" "$1" "$(file_sha256 "$stage")"); }

initial=$(printf 'schema=2\n' | file_sha256)
prepare "$old_file"
commit "$initial"
[[ $(config_value visual_path) == "$old_file" && -f "$old_file" && ! -e "$stage" ]]
revision=$(file_sha256 "$SOURCE_CONFIG")
prepare "$next_file"
[[ $(commit "$initial" || true) == *source_config_conflict* ]]
[[ $(file_sha256 "$SOURCE_CONFIG") == "$revision" && -f "$old_file" && -f "$stage" ]]

touch "$CONTROL/camera.lease"
[[ $(commit "$revision" || true) == *source_runtime_active* ]]
rm "$CONTROL/camera.lease"
[[ $(config_value visual_path) == "$old_file" ]]
touch "$DATA/video_mode"
[[ $(commit "$revision" || true) == *source_runtime_active* ]]
rm "$DATA/video_mode"
for feeder in active unknown; do
  [[ $(commit "$revision" || true) == *source_runtime_active* ]]
  [[ $(config_value visual_path) == "$old_file" && -f "$old_file" && -f "$stage" ]]
done
feeder=idle

prepare "$missing_file"
[[ $(commit "$revision" || true) == *source_media_missing* ]]
[[ $(config_value visual_path) == "$old_file" && -f "$old_file" ]]

prepare "$next_file"
[[ $( (commit_source "$stage_name" "$revision" "$initial") || true) == *source_stage_checksum* ]]
commit "$revision"
[[ $(config_value visual_path) == "$next_file" && -f "$next_file" && ! -e "$old_file" ]]
printf '%s\n' 'Source commit rejected conflicts, active leases, missing media and a bad checksum'
