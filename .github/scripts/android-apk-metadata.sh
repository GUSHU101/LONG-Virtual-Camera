#!/usr/bin/env bash
set -euo pipefail

version=$1
channel=$2
if [[ ! "$version" =~ ^v(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})([.-][0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$ ||
      ${#version} -gt 64 ]]; then
  echo '::error::版本号格式为 vMAJOR.MINOR.PATCH，可带 -rc.1 等后缀。' >&2
  exit 1
fi
major=$((10#${BASH_REMATCH[1]}))
minor=$((10#${BASH_REMATCH[2]}))
patch=$((10#${BASH_REMATCH[3]}))
code=$((major * 1000000 + minor * 1000 + patch))
if (( code < 1 || code > 2100000000 )); then
  echo "::error::Android versionCode 超出范围 $code" >&2
  exit 1
fi

case "$channel" in
  development) suffix=development; variant=debug ;;
  production) suffix=release; variant=release ;;
  *) echo '::error::未知 APK 渠道' >&2; exit 1 ;;
esac
artifact="LONG-Virtual-Camera-$version-LSPosed-arm64-$suffix"
commit=$(git rev-parse HEAD)
printf 'version=%s\nversion_code=%s\nvariant=%s\nartifact=%s\ncommit=%s\n' \
  "$version" "$code" "$variant" "$artifact" "$commit" >> "$GITHUB_OUTPUT"
