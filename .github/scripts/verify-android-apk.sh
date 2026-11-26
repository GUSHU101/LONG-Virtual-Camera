#!/usr/bin/env bash
set -euo pipefail

tools=$1
apk=$2
channel=$3
version=$4
version_code=$5

error() {
  echo "::error::$*" >&2
  exit 1
}

package=com.gushu.longvc
case "$channel" in
  production) debuggable=false ;;
  development|quality)
    package+=.debug
    version+=-debug
    debuggable=true
    ;;
  *) error "Unknown APK channel" ;;
esac

badging=$("$tools/aapt2" dump badging "$apk")
expected="package: name='$package' versionCode='$version_code' versionName='$version'"
[[ "$badging" == "$expected "* ]] || error "APK package or version does not match $expected"
if grep -Fxq 'application-debuggable' <<< "$badging"; then
  [[ "$debuggable" == true ]] || error "Production APK is debuggable"
else
  [[ "$debuggable" == false ]] || error "Development APK is missing its debug flag"
fi

signing_report=$("$tools/apksigner" verify --verbose --print-certs "$apk")
grep -Eq '^Verified using v[23](\.[0-9]+)? scheme .*: true$' <<< "$signing_report" ||
  error "APK needs a verified v2 or v3 signature"
grep -Fxq 'Number of signers: 1' <<< "$signing_report" ||
  error "Expected one APK signer"
signer=$(awk '/^(Signer #[0-9]+|V[0-9.]+ Signer( #[0-9]+)?):? certificate SHA-256 digest:/ { print $NF }' \
  <<< "$signing_report" | sort -u)
[[ "$signer" =~ ^[a-f0-9]{64}$ ]] || error "Expected a single APK signing certificate"
development_cert=$(<.github/development-cert.sha256)
if [[ "$channel" == development ]]; then
  [[ "$signer" == "$development_cert" ]] ||
    error "Development APK certificate differs from the pinned development certificate"
elif [[ "$channel" == production ]]; then
  [[ "$signer" != "$development_cert" ]] || error "Production APK uses the development certificate"
  if [[ -n "${PRODUCTION_CERT_SHA256:-}" ]]; then
    [[ "$signer" == "${PRODUCTION_CERT_SHA256,,}" ]] ||
      error "Production APK certificate differs from LONGVC_ANDROID_CERT_SHA256"
  fi
fi

python3 tests/verify_apk.py "$apk"
printf 'APK verified for %s %s (%s). Signer SHA-256 %s\n' "$package" "$version" "$version_code" "$signer"
