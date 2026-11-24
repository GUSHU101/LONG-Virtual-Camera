# Fusion research record

This record describes the historical Windows-first fusion. The current
APK-only runtime is documented in [Architecture](ARCHITECTURE.md).

## Baselines and design choice

- ReCam: `44f99681079c04e3b3e6d3a74fef96f03b480348`.
- CamReplace: `8456a5d8041f4c81a26b5c19fa0d6409f57db048`.

The detailed inventory and review method are in
[UPSTREAM-CODE-AUDIT.md](UPSTREAM-CODE-AUDIT.md). The result is not a wrapper
that launches either repository. CamReplace's Windows/OBS/device pipeline is the
native product skeleton; ReCam's source adapters, frame ownership model, geometry
validation, fail-closed hook policy and lifecycle ideas were brought into that
same process. The old stable/enhanced selector and `--engine` option were removed.

## Integrated decisions

| Area | Upstream evidence | Fused implementation |
|---|---|---|
| Desktop input | ReCam supports generated/file/camera/RTMP sources | Native `DesktopSource` adapters publish all inputs into one loopback RTMP session |
| Product transport | CamReplace has H.264, raw and experimental receivers | One release H.264 lane; raw/direct/threaded retained only as diagnostic build sources |
| Frame ownership | ReCam has per-slot reader/writer state; CamReplace reset states on reconnect | Three-slot reader pins, writer claims, OS writer lease, reader-preserving reconnect and release-last publication |
| Camera hook | ReCam validates inline-hook targets; CamReplace hooks Camera3 output | Exact reviewed symbols only, `status_t` ABI, original exactly once, unknown ABI pass-through |
| Codec output | CamReplace decoded using buffer capacity and guessed flexible YUV | Use codec `offset/size`; publish only proven planar/semiplanar layouts |
| Still photo | CamReplace encodes the current slot | Slot and generation remain pinned across encode; cache key includes generation/sequence/geometry |
| Microphone consent | CamReplace supplied PCM replacement | Explicit default-OFF lease; no audioserver injection from camera/photo starts |
| Microphone privacy | Partial virtual delivery could retain HAL bytes | Active hook clears the whole buffer; failure is silence |
| Resampling | Per-callback fixed-point phase restarted | Per-consumer exact rational phase with one-hour drift regression |
| RTMP/AVCC | Minimal parser assumed 4-byte NAL lengths | Pointer-width sockets, bounded buffering/AMF, extended timestamps and config-driven 1/2/4-byte lengths |
| Async operations | UI operations used detached threads | Owned serial executor, joined shutdown, session-scoped cleanup |
| UI feedback | Each sound used an unowned detached worker | One bounded sound queue with explicit stop/join |
| Device state | A global prepared flag could cross phones | Per-serial preparation plus explicit active/touched serial registry |
| Device deployment | Service refresh could happen before validation, cameraserver could restart twice and crash recovery lost ownership | Validate first; one refresh/injection transaction, reconcile cleanup with live service maps and restore displaced camera clients |
| Security | CamReplace secure channel lacked a meaningful local ticket proof | Ephemeral X25519-derived proof, expiry, stream binding, replay and allocation failure handling |
| Privilege | Broad SELinux/runtime permissions were possible | Enforcing mode retained; scoped live rules and service-specific ownership |
| Host privilege | Windows host requested administrator elevation | Run as the invoking user; keep root explicit and device-side only |
| Child processes | ADB inherited broad handles and retained unbounded output | Explicit inherited-handle list, bounded capture/lines, exception-isolated callbacks and cancellable reader |
| Tool discovery | Default Windows executable lookup could include current directory | Accept explicit, bundled or absolute PATH FFmpeg only |
| Packaging | Missing resources became zero-byte placeholders and replacement was non-transactional | Release configure failure, SHA-256 manifest, private atomic host extraction, and staged/verified/rollback-capable phone installation from both desktop and APK |
| LSPosed/root control | Neither donor provided one modern system-scope companion shared with the fused host | API 102 module + UI, fixed `system` scope, one embedded controller, native cameraserver global path, default-OFF audioserver path and authenticated readiness reporting |
| JPEG/BLOB | BLOB capacity was calculated as a 2-D image | Enforce height 1 and width-as-bytes before JPEG/trailer writes; release failed mapper locks cleanly |

## One session, composable features

Camera, photo and microphone are not alternative modes. Camera and photo both
hold the same video pipeline; microphone holds an independent audio pipeline.
The controller reference behavior is:

```text
first feature ON  -> create source/session
camera or photo   -> acquire/reuse video feeder + forward
microphone ON     -> separately arm audioserver + PCM feeder
one feature OFF   -> release only its lease
last feature OFF  -> stop source/session
```

Rollback checks the leases that existed before a start. A failed photo start,
for example, cannot disarm a camera session that was already active.

## Rejected approaches

- two user-selectable engines or a silent fallback runtime;
- symbol prefix matching or arbitrary private-method signature guesses;
- treating an arbitrary null ptrace fault as a valid remote return;
- grayscale synthesis for an unknown YUV layout;
- resetting all slot reader counts on producer reconnect;
- detached deployment workers;
- wiping every authorized USB device on exit;
- distributable builds with empty embedded binaries;
- camera startup implicitly enabling microphone replacement.

## Remaining limits

Android camera/audio service internals remain private and firmware-specific.
The exact symbol list cannot promise support for a future vendor build; failure
is intentionally pass-through. The supported complete host is Windows x64 and
the Android payload is ARM64. The local ticket is not persistent device
identity. CI has no physical phone, so vendor gralloc layouts, repeated service
restart behavior and actual Audio HAL cadence require the device matrix described
in [ARCHITECTURE.md](ARCHITECTURE.md).
