# Architecture and safety contracts

## One product runtime

The supported product is one Windows executable and one device-owned session.
OBS, generated test video, file/URL playback and DirectShow capture are source
adapters, not engines. Camera, still-photo and microphone replacement are
independent leases on the same source and transport stack.

```text
DesktopSource adapters
        │
loopback RTMP ingest ── preview
        │ H.264                    optional AAC
        │                              │
authenticated TCP/ADB            AudioPump/PCM
        │                              │
Android MediaCodec → 3-slot ring   audio ring
        │                              │
Camera3OutputStream hook          audioserver hook
```

The release UI exposes one H.264 lane. Upstream raw/direct/threaded sources are
retained as diagnostic build configurations so upstream behavior remains
regression-testable; they are not selectable product modes.

## Camera ABI and buffer contract

1. Only exact reviewed mangled symbols are candidates. Unknown firmware passes
   through; prefix guessing is forbidden.
2. Proxies have the reviewed argument count and return Android `status_t`; the
   original method is called exactly once and its value is returned unchanged.
3. Replacement failure never suppresses the original queue operation.
4. A duplicated HAL fence is consumed before CPU access. Native window buffers
   use `ANativeWindowBuffer_getHardwareBuffer` when supported.
5. Geometry, format, codec `offset/size`, row span and mapped capacity are
   checked before access. Unknown YUV layouts do not use a grayscale fallback.
6. A source slot stays pinned for the full pixel copy or JPEG encode.

## Frame publication contract

The producer holds an exclusive OS writer lease for `/data/cr/feed`. On a
compatible reconnect it clears only an abandoned writer token and preserves
live cameraserver reader counts. A writer claims only a free non-current slot,
writes all bytes, publishes index then sequence while retaining the writer token,
and releases that token last. Readers pin, snapshot and revalidate generation,
index, sequence and geometry. If every alternate slot is pinned, the producer
drops the new frame instead of overwriting an active reader.

## Microphone contract

- Initial OFF is the default: there is no audio feeder or audio-hook injection,
  so the phone microphone follows the original HAL path untouched.
- ON is explicit. Once the authenticated channel is READY, the hook clears the
  complete HAL result before copying virtual PCM, so errors/underruns are
  silence rather than leaked real-microphone bytes.
- Every HAL consumer has its own cursor, probe and exact rational resampling
  phase. Fractional phase survives callback boundaries.
- Disable invalidates the phone channel before stopping the host pump. If the
  hook was loaded by an earlier enable, it may remain dormant until service or
  session cleanup, but it calls the original HAL result and substitutes no bytes.
- Health is observable as STARTING/READY/STALLED/UNSUPPORTED.

## Lifecycle and ownership

A process-owned single-worker executor serializes install/start/stop/delete.
Shutdown stops accepting jobs, cancels jobs that never started, joins the active
transaction, then tears down host resources. The active serial cannot be
silently replaced by the UI. Cleanup is restricted to serials this process
actually touched; merely authorizing another connected phone does not make it a
wipe target.

Camera and photo share a video lease, so stopping one cannot disarm the other.
Every partially completed start has an explicit reverse path. Blocking ADB,
handshake, socket and child-process operations have deadlines or forced teardown.

## Parser and transport budgets

RTMP binds only to `127.0.0.1`. Socket handles remain pointer-width on Windows.
The parser limits chunk streams, per-message bytes, aggregate buffered bytes,
inbound chunk size and AMF nesting. Extended timestamps are consumed on every
continuation. AVC configuration determines the 1/2/4-byte NAL length field;
malformed configuration/access units are rejected transactionally.

Each media lane performs an ephemeral X25519 exchange and ticket proof bound to
stream kind and expiry. Records are authenticated, ordered and size-limited.
This secures the local USB/loopback transport; it is not persistent device
identity.

## Release boundary

The build fails if adb, the MinGW runtime, Android payloads or UI assets are
missing/empty. CI publishes the executable with a manifest containing source
commit, pinned toolchains, payload sizes and SHA-256 hashes. A special
`CR_ALLOW_MISSING_RESOURCES=ON` option exists only for non-distributable UI
bootstrap builds.

Generated video, file/URL playback and DirectShow adapters launch an external
FFmpeg executable resolved from `LONGVC_FFMPEG` or `PATH`. OBS push mode enters
the same loopback RTMP ingest directly and does not require the adapter process.

## Validation boundary

Host/unit/ABI/parser/ring tests and cross-compilation are mandatory gates, but
cannot validate vendor Camera3, gralloc, MediaCodec or Audio HAL behavior.
Physical-device testing remains required for each supported build fingerprint,
orientation, format, preview/record/JPEG path, USB interruption and repeated
start/stop cycle.
