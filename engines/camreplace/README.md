# LONG Virtual Camera native core

This directory is the CamReplace-derived native core of LONG Virtual Camera.
ReCam-derived input adapters, ownership rules, validation and lifecycle behavior
are integrated into this same Windows/Android runtime. Internal `CameraReplace`
and `cr_*` target/protocol names remain only where renaming would break build or
wire compatibility.

The product builds one `LONG-Virtual-Camera.exe`. Its source adapters (OBS,
test pattern, file/URL and DirectShow camera) converge on one loopback RTMP
ingest, authenticated H.264 device lane, preview and Android frame ring.
Camera, still-photo and explicitly enabled microphone replacement are
composable feature leases, not separate engines.

## Build

Run `build.bat` at the repository root. Required inputs are Windows 10/11 x64,
CMake 3.24+, Ninja, Android Platform Tools, NDK 29.0.14033849 and an x86_64
LLVM-MinGW toolchain. Release configuration fails when any embedded adb/runtime/
Android/UI resource is absent or empty.

Generated/file/URL/DirectShow source adapters require `ffmpeg.exe` at runtime,
resolved first from `LONGVC_FFMPEG` and then from `PATH`. OBS direct push uses
the same ingest without launching the adapter.

Output:

```text
build\host-dev-dev\bin\LONG-Virtual-Camera.exe
```

The optional raw NV21, direct RTMP and threaded feed sources are retained for
upstream compatibility/regression builds. The supported product UI exposes one
H.264 video pipeline.

See the repository [README](../../README.md), [architecture](../../docs/ARCHITECTURE.md)
and [upstream audit](../../docs/UPSTREAM-CODE-AUDIT.md) for behavior, provenance,
tests and validation limits.

## Responsible use and license

Use only on devices you own or are authorized to test. Do not use virtual media
to bypass identity, financial, access-control or other trust decisions.

This combined work is GPL-3.0-only. ReCam's MIT notice and all third-party
notices remain preserved in the repository license/notice files.
