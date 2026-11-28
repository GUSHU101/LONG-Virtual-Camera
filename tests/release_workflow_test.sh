#!/usr/bin/env bash
set -euo pipefail

script="$PWD/.github/scripts/publish-android-release.sh"
metadata="$PWD/.github/scripts/android-apk-metadata.sh"
commit=$(git rev-parse HEAD)
temp=$(mktemp -d)
trap 'rm -rf -- "$temp"' EXIT
for channel in development production; do
  case "$channel" in
    development) variant=debug; suffix=development ;;
    production) variant=release; suffix=release ;;
  esac
  GITHUB_OUTPUT="$temp/metadata" bash "$metadata" v2.3.4-rc.1 "$channel"
  grep -Fxq 'version=v2.3.4-rc.1' "$temp/metadata"
  grep -Fxq 'version_code=2003004' "$temp/metadata"
  grep -Fxq "variant=$variant" "$temp/metadata"
  grep -Fxq "artifact=LONG-Virtual-Camera-v2.3.4-rc.1-LSPosed-arm64-$suffix" "$temp/metadata"
  grep -Fxq "commit=$commit" "$temp/metadata"
  : > "$temp/metadata"
done
expect_invalid_metadata() {
  if GITHUB_OUTPUT="$temp/metadata" bash "$metadata" "$1" "$2" > "$temp/metadata-error" 2>&1; then
    printf 'Unexpectedly accepted version %q in channel %q\n' "$1" "$2" >&2
    exit 1
  fi
  grep -Fq '::error::' "$temp/metadata-error"
}
for invalid in '' v2.01.4 v0.0.0 v2100.0.1 'v2.0.1 bad'; do
  expect_invalid_metadata "$invalid" development
done
expect_invalid_metadata v2.0.1 quality
[[ ! -s "$temp/metadata" ]]

cd "$temp"
mkdir bin dist
export PATH="$PWD/bin:$PATH"
export GITHUB_REPOSITORY=example/longvc GITHUB_RUN_ID=123 GITHUB_REF_TYPE=branch
export GITHUB_REF_NAME=main GH_TOKEN=fixture GITHUB_STEP_SUMMARY="$PWD/summary"
export RELEASE_VERSION=v2.0.22 BUILD_COMMIT=1111111111111111111111111111111111111111
export ARTIFACT_NAME="LONG-Virtual-Camera-$RELEASE_VERSION-LSPosed-arm64-release"
export TEST_DIR="$PWD"

apk="$ARTIFACT_NAME.apk"
printf 'verified APK bytes\n' > "dist/$apk"
(cd dist && sha256sum --binary "$apk" > "$apk.sha256")
read -r APK_SHA256 _ < "dist/$apk.sha256"
export APK_SHA256

cat > bin/gh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
cd "$TEST_DIR"
case "$*" in
  'api repos/example/longvc/git/matching-refs/'*)
    cat refs.json ;;
  'api repos/example/longvc/git/tags/'*)
    jq -nc --arg sha "$BUILD_COMMIT" '{type:"commit",sha:$sha}' ;;
  'api --paginate repos/example/longvc/releases?per_page=100')
    if [[ -f release.json ]]; then jq -s . release.json; else echo '[]'; fi ;;
  'api --method POST repos/example/longvc/releases '*)
    jq -nc --arg version "$RELEASE_VERSION" --arg commit "$BUILD_COMMIT" \
      --arg marker "<!-- longvc-release-run:$GITHUB_RUN_ID:$BUILD_COMMIT -->" \
      '{id:7,tag_name:$version,target_commitish:$commit,body:$marker,draft:true,assets:[]}' |
      tee release.json ;;
  'api repos/example/longvc/releases/7')
    cat release.json ;;
  'api --method PATCH repos/example/longvc/releases/7 -F draft=false --jq .html_url')
    jq '.draft = false' release.json > updated.json
    mv updated.json release.json
    echo 'https://example.invalid/release' ;;
  *) echo "Unexpected gh invocation: $*" >&2; exit 2 ;;
esac
EOF

cat > bin/curl <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
cat >/dev/null # Authorization header from stdin.
url=${!#}
[[ "$url" == *'/releases/7/assets?name='* ]]
name=${url##*=}
if [[ "$name" == *.sha256 && -f fail-checksum ]]; then
  exit 22
fi
digest=$(sha256sum "dist/$name")
digest=${digest%% *}
if [[ -f corrupt-digest ]]; then digest=invalid; fi
asset=$(jq -nc --arg name "$name" --arg digest "sha256:$digest" \
  '{name:$name,digest:$digest,state:"uploaded"}')
jq --argjson asset "$asset" '.assets += [$asset]' release.json > updated.json
mv updated.json release.json
printf '%s\n' "$asset"
EOF
chmod +x bin/gh bin/curl

expect_failure() {
  local step=$1 message=$2
  if bash "$script" "$step" > failure.log 2>&1; then
    echo "Expected failure: $message" >&2
    exit 1
  fi
  if ! grep -Fq "$message" failure.log; then
    cat failure.log >&2
    exit 1
  fi
  echo "Rejected as expected: $message"
}

printf '[]\n' > refs.json
bash "$script" preflight

printf '[{"ref":"refs/tags/%s","object":{"type":"commit","sha":"wrong"}}]\n' \
  "$RELEASE_VERSION" > refs.json
expect_failure preflight "points to wrong"
printf '[{"ref":"refs/tags/%s","object":{"type":"tag","sha":"annotated"}}]\n' \
  "$RELEASE_VERSION" > refs.json
bash "$script" preflight

touch dist/extra
expect_failure publish "Expected exactly"
rm dist/extra
cp "dist/$apk" good.apk
printf 'tampered\n' >> "dist/$apk"
expect_failure publish "differs from the verified build"
mv good.apk "dist/$apk"
[[ ! -f release.json ]]

touch fail-checksum
expect_failure publish "GitHub did not accept $apk.sha256"
jq -e '.draft and (.assets | length == 1)' release.json
rm fail-checksum
bash "$script" publish
jq -e '(.draft | not) and (.assets | length == 2)' release.json
expect_failure preflight "already exists"
rm release.json

touch corrupt-digest
expect_failure publish "GitHub did not accept $apk with the expected SHA-256"
jq -e '.draft' release.json
