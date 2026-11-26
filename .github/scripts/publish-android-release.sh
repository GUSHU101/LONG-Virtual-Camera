#!/usr/bin/env bash
set -euo pipefail

repo="repos/$GITHUB_REPOSITORY"
tag="refs/tags/$RELEASE_VERSION"
marker="<!-- longvc-release-run:$GITHUB_RUN_ID:$BUILD_COMMIT -->"

error() {
  echo "::error::$*" >&2
  exit 1
}

check_tag() {
  local ref object kind oid depth
  ref=$(gh api "$repo/git/matching-refs/tags/$RELEASE_VERSION" |
        jq -c --arg tag "$tag" '.[] | select(.ref == $tag) | .object')
  if [[ -z "$ref" ]]; then
    [[ "$GITHUB_REF_TYPE" != tag ]] || error "Tag $RELEASE_VERSION no longer exists"
    return
  fi
  object=$ref
  for (( depth=0; depth<16; depth++ )); do
    kind=$(jq -er '.type' <<< "$object")
    oid=$(jq -er '.sha' <<< "$object")
    case "$kind" in
      commit)
        [[ "$oid" == "$BUILD_COMMIT" ]] ||
          error "Tag $RELEASE_VERSION points to $oid. The APK build targets $BUILD_COMMIT."
        return
        ;;
      tag) object=$(gh api "$repo/git/tags/$oid" --jq '.object') ;;
      *) error "Tag $RELEASE_VERSION does not point to a commit" ;;
    esac
  done
  error "Could not resolve annotated tag $RELEASE_VERSION"
}

find_release() {
  gh api --paginate "$repo/releases?per_page=100" |
    jq -c --arg version "$RELEASE_VERSION" '.[] | select(.tag_name == $version)'
}

check_draft() {
  jq -e --arg target "$BUILD_COMMIT" --arg tag "$RELEASE_VERSION" --arg marker "$marker" \
    '.draft and .target_commitish == $target and .tag_name == $tag and
     (.body // "" | contains($marker))' >/dev/null <<< "$1" ||
    error "Release $RELEASE_VERSION already exists or belongs to a different run. Published releases are permanent."
}

[[ "$GITHUB_REF_TYPE" != tag || "$GITHUB_REF_NAME" == "$RELEASE_VERSION" ]] ||
  error "Requested version must match the tag that triggered the workflow"
check_tag
release=$(find_release)
if [[ -n "$release" ]]; then
  [[ "$release" != *$'\n'* ]] || error "Multiple releases use the tag $RELEASE_VERSION"
  check_draft "$release"
fi

if [[ "$1" == preflight ]]; then
  exit 0
fi
[[ "$1" == publish ]] || error "Unknown release operation"

apk="$ARTIFACT_NAME.apk"
checksum="$apk.sha256"
shopt -s nullglob dotglob
files=(dist/*)
[[ ${#files[@]} -eq 2 && -f "dist/$apk" && -f "dist/$checksum" ]] ||
  error "Expected exactly $apk and $checksum in the release artifact"
[[ "$APK_SHA256" =~ ^[a-f0-9]{64}$ ]] || error "Build did not return a valid APK SHA-256"
(cd dist && cmp "$checksum" <(printf '%s *%s\n' "$APK_SHA256" "$apk") &&
  sha256sum --check "$checksum") || error "Downloaded APK or checksum differs from the verified build"
checksum_hash=$(sha256sum "dist/$checksum")
checksum_hash=${checksum_hash%% *}

if [[ -z "$release" ]]; then
  prerelease=false
  [[ "$RELEASE_VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || prerelease=true
  release=$(gh api --method POST "$repo/releases" \
    -f tag_name="$RELEASE_VERSION" -f target_commitish="$BUILD_COMMIT" \
    -f name="LONG Virtual Camera $RELEASE_VERSION" -f body="$marker" \
    -F draft=true -F generate_release_notes=true -F prerelease="$prerelease")
  check_draft "$release"
fi
release_id=$(jq -er '.id' <<< "$release")

upload_asset() {
  local name=$1 digest=$2 existing
  existing=$(jq -c --arg name "$name" '.assets[] | select(.name == $name)' <<< "$release")
  if [[ -n "$existing" ]]; then
    jq -e --arg digest "sha256:$digest" \
      '.state == "uploaded" and .digest == $digest' >/dev/null <<< "$existing" ||
      error "Draft asset $name differs from the verified build. Inspect this run's draft before retrying."
    return
  fi
  # Use the verified draft ID so uploads cannot target a different release.
  printf 'Authorization: Bearer %s\n' "$GH_TOKEN" |
    curl --fail-with-body --silent --show-error --max-time 90 \
    -H @- -H 'Content-Type: application/octet-stream' \
    --data-binary "@dist/$name" \
    "https://uploads.github.com/$repo/releases/$release_id/assets?name=$name" |
      jq -e --arg digest "sha256:$digest" '.state == "uploaded" and .digest == $digest' >/dev/null ||
      error "GitHub did not accept $name with the expected SHA-256"
}

upload_asset "$apk" "$APK_SHA256"
upload_asset "$checksum" "$checksum_hash"

release=$(gh api "$repo/releases/$release_id")
check_draft "$release"
jq -e --arg apk "$apk" --arg apk_digest "sha256:$APK_SHA256" \
  --arg checksum "$checksum" --arg checksum_digest "sha256:$checksum_hash" \
  '(.assets | map({name, digest, state}) | sort_by(.name)) ==
   [{name: $apk, digest: $apk_digest, state: "uploaded"},
    {name: $checksum, digest: $checksum_digest, state: "uploaded"}]' \
  >/dev/null <<< "$release" || error "Draft assets do not match the verified build"
check_tag

url=$(gh api --method PATCH "$repo/releases/$release_id" -F draft=false --jq '.html_url')
printf '## Published signed APK\n\n[%s](%s)\n\nCommit: \x60%s\x60\n' \
  "$RELEASE_VERSION" "$url" "$BUILD_COMMIT" >> "$GITHUB_STEP_SUMMARY"
