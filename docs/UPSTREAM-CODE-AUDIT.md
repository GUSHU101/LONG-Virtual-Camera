# Upstream code audit and fusion map

This is the historical audit of the two pinned donors and the first fusion.
The current release build contains only the Android APK and its phone-native
payload. The original Windows host and the standalone ReCam tree are
available in Git history. See [current architecture](ARCHITECTURE.md).

Both upstreams replace frames inside `cameraserver`. ReCam uses the
`Camera3OutputStream::returnBufferCheckedLocked` path. CamReplace uses
`Camera3OutputStream::queueBufferToConsumer` as its primary path. The APK
uses the latter with exact reviewed signatures and carries over ReCam's
geometry and frame-lifetime checks. Its LSPosed entry runs in the separate
`system_server` process.

## Reproducible baselines

This review used clean clones whose HEAD values were verified before comparison:

| Project | Repository | Audited commit | License |
|---|---|---|---|
| ReCam | `aimardcr/ReCam` | `44f99681079c04e3b3e6d3a74fef96f03b480348` | MIT |
| CamReplace | `pIat0n/CamReplace` | `8456a5d8041f4c81a26b5c19fa0d6409f57db048` | GPL-3.0-only |

The audit inventory covers every upstream-owned implementation, header, build
script, workflow, manifest, init/policy and resource declaration. Under that
definition ReCam contains 68 files (7,941 lines including its policy rule) and
CamReplace contains 88 files (19,810 lines including manifest/RC declarations):
156 first-party files in total.

Generated media assets, prose/license text and complete third-party source trees
are not misleadingly counted as first-party application code. Vendored
AndKittyInjector, XZ Embedded, ShadowHook, Monocypher, stb and ImGui were reviewed
at their build, ABI, privilege and license boundaries; they were not claimed as
new line-by-line cryptographic or codec audits.

The review had three passes:

1. inventory every first-party file and map entry points/build inclusion;
2. trace source → transport → decoder/ring → hook and every reverse lifecycle;
3. line-focused audit of trust boundaries: private ABI, ptrace, shared memory,
   buffer arithmetic, sockets/parsers, audio privacy, child processes and release
   embedding.

## ReCam: complete module findings

| Upstream area | Files/role reviewed | Finding and fusion result |
|---|---|---|
| `android/src/common` | control ABI, frame geometry, resident mapping, shared frame and ring access | Its reader/writer ownership and geometry discipline became the invariant for the unified three-slot ring; the incompatible wire layout itself was not imposed on CamReplace. |
| `android/src/feed` | FLV/H.264 parsing, MediaCodec decoder, scaling and feeder main | Decoder and source behavior informed fail-closed frame publication. The second feeder runtime was not kept as a product engine. |
| `android/src/payload` | ARM64/x86 inline hook, trampoline allocator, metadata import, frame hook/source, AHB/GBM/C2D/QCOM layouts, UBWC and blit | Prologue/target validation and unsupported-layout pass-through were adopted as policy. QCOM-private fast paths were not blindly copied into the generic CamReplace hook. |
| `android/src/cli` | install/start/stop/control command sequencing | Reverse-order cleanup and explicit lifecycle states informed the unified controller. |
| `android/jni`, policy, patch | NDK targets, ABI flags, scoped SELinux rule and injector patch | Kept as a donor regression build and provenance source; no global permissive SELinux path was accepted. |
| `desktop/recam_ui` | PySide UI, device control/status, FFmpeg streamer and logging | Test pattern, file/URL, camera discovery, URL redaction and non-blocking UI ideas were ported into native `DesktopSource`; the Python UI is no longer a fallback product runtime. |
| `tools` and workflow | packaging/release scripts and Android build | Used for provenance/regression checks, not as a second release pipeline. |

High-value ReCam properties retained are: pin-before-read, exclusive writer
claims, fail-closed geometry, hook-target validation, owned subprocesses,
redacted source display and explicit reverse lifecycle. Rejected as a direct
merge were its distinct shared-memory/control protocol and platform-specific
private buffer paths that could not be proven compatible with the selected
CamReplace Camera3 interception point.

## CamReplace: complete module findings

| Upstream area | Files/role reviewed | Finding and fusion result |
|---|---|---|
| `host/src/source` | RTMP server, OBS fan-out, audio pump, feed and host controllers | Retained as the main native skeleton, then changed to one source adapter layer, bounded parsing, lease-based feature composition and an owned operation executor. |
| `host/src/transport` | TCP framing and secure session client | Retained with finite connect/handshake/I/O bounds, reconnect behavior and authenticated local ticket proof. |
| `host/src/device` | ADB process control, scanner, phone operations, log tail and device stack | Retained with command deadlines, inherited-handle allowlisting, bounded output, callback isolation, per-serial preparation, session ownership, exactly-once service refresh, scoped cleanup, transactional rollback and no SELinux-disable path. |
| `host/src/render` | D3D11 and Media Foundation H.264 preview | Retained with output length/stride/geometry validation, exception containment and bounded shutdown. |
| `host/src/resources` and CMake | embedded ADB/payload extraction and Windows manifest | Retained; release builds reject missing/empty blobs, extraction uses a user-private non-reparse directory plus exclusive staging/flush/atomic replace/read-back verification, and CI emits size/SHA-256 provenance. |
| `host/src/ui` and app | ImGui window, feature controls, status/logging and shutdown | Retained under one product identity. Camera/photo/microphone are composable leases; mic is explicitly OFF by default and has health states. |
| `android/feed` | H.264/raw/threaded/direct receivers, PCM ring, MediaCodec and shared frame ABI | H.264 is the sole release lane. Ring reset, codec-capacity and unknown-YUV bugs were fixed. Other sources remain diagnostic build inputs rather than deleted history or user engines. |
| `android/camhook` | Camera3 output hook, AHB/GBM mapping, NV21 scaler and JPEG | Retained with exact reviewed symbols, correct `status_t` return, original-call preservation, snapshot revalidation and encode-long pinning. |
| `android/audhook` | HAL read hooks, format probe, ring consumer and resampler | Retained as explicit opt-in; active failures are silence, and per-consumer rational phase prevents long-run drift. |
| `android/injector` | ptrace remote call and live policy work | Retained after removing ambiguous null-fault success and constraining readiness/privilege behavior. |
| `common/secure_channel` | protocol, session state, crypto vendor and status | Retained with real ephemeral-ticket proof, replay/size checks and allocation-safe `noexcept` paths. |
| root scripts/toolchains | Android/Windows orchestration | Unified under the root `build.bat`; pinned CI builds both sides before packaging. |

## Defects found during cross-reading

The following were concrete code defects, not cosmetic differences:

- Camera queue proxies discarded Android `status_t` by using a `void`
  interpretation. The proxy now returns the original status unchanged and a
  native test covers OK, BAD_VALUE and NO_MEMORY.
- Producer reconnect reset every slot state, erasing live cameraserver readers.
  Reconnect now preserves readers and clears only an exact abandoned writer.
- A newly written slot was released before publication metadata was complete.
  Publication is now index, sequence, then writer-token release; a concurrent
  pthread stress test proves stable snapshots pair payload and sequence.
- MediaCodec output used capacity rather than `AMediaCodecBufferInfo::size` and
  guessed unknown flexible YUV as grayscale. Bounds now use offset/size and
  unknown layouts publish nothing.
- The audio resampler restarted its fractional position on every HAL callback.
  Phase is now persistent per consumer; an uneven-block one-hour test has exact
  44.1→48 kHz totals.
- Android branches exposing `read(void*, uint32_t, uint32_t*)` were routed
  through a `size_t` proxy. The `j/Pj` and `m/Pm` manglings now use distinct
  width-correct proxies so a 64-bit read cannot cross the caller's object.
- Audio format probing assumed every HAL callback had the same byte count and
  publication could expose READY before the cursor. It now measures cumulative
  bytes over variable-size timing windows and publishes the write cursor before
  READY.
- Hook callbacks could observe a partially populated process-local shared-map or
  runtime mapper table. Mapping publication is now release/acquire guarded and
  AHB/GBM operations hold a shared lock across lock/blit/unlock.
- A Windows `SOCKET` was narrowed to `int`; it is pointer-width end-to-end.
- AVCC media parsing assumed four-byte NAL lengths. It now obeys the decoder
  configuration's legal 1/2/4-byte value and rejects malformed input atomically.
- RTMP message/chunk-stream/AMF allocation and nesting were insufficiently
  bounded; explicit budgets and extended-timestamp continuation handling were
  added.
- Deployment operations used detached threads, so teardown could race callbacks
  and ADB. A joined process-owned executor replaces them.
- Exit cleanup targeted every authorized device instead of only session devices.
  A touched-serial registry now defines the cleanup scope.
- Photo and camera were described as coexisting internally but blocked by UI,
  and rollback could disarm an existing video user. Both now share a video lease.
- Release configuration silently embedded zero-byte dependencies. Missing/empty
  resources now fail configuration unless a developer explicitly requests a
  non-distributable placeholder build.
- Camera BLOB buffers were treated like ordinary two-dimensional images. Android's
  BLOB contract requires height 1 and defines width as byte capacity; both are now
  checked before placing the JPEG payload and trailer, preventing a capacity
  overestimate and trailer overwrite. A successful GBM lock with a null mapping is
  also unlocked before the mapper fallback is tried.
- ADB children inherited every inheritable host handle, capture grew without a
  limit and callback exceptions could terminate worker threads. The launcher now
  passes an explicit handle list, drains into an 8 MiB bounded transcript, caps
  streaming lines, contains callbacks and cancels a blocked reader during stop.
- Resource replacement deleted the last known-good file before writing its
  successor and used predictable temporary names. Extraction now rejects reparse
  directories, applies a current-user/System/Administrators DACL, creates a unique
  staging file exclusively, flushes it, atomically replaces and verifies bytes.
- Windows startup requested administrator elevation although the host only needs
  ordinary user access to ADB and local media. The manifest is now `asInvoker`;
  Android root remains an explicit `adb shell su` device-side requirement.
- FFmpeg lookup could include the current directory through default Windows search
  semantics. Only an explicit `LONGVC_FFMPEG`, the bundled adjacent binary or an
  absolute PATH component is now accepted.
- UI sound playback created detached threads per action. A single bounded,
  process-owned player queue now stops and joins during window teardown.
- Hook deployment mutated services before validating feed/log/symbol parameters,
  and camera setup could restart cameraserver twice. Inputs are now validated
  first; the first owned lease performs exactly one service refresh, waits for the
  service, injects, restores camera clients on every failure path and records
  ownership only after injection succeeds. Camera/photo never touch audioserver;
  only explicit microphone opt-in may refresh it.
- A host crash erased the in-memory hook owner set, so a later Delete could wipe
  files while an older library remained mapped. Cleanup now reconciles ownership
  with the actual camera/audio service maps, restarts only services where the hook
  is present, waits for respawn and relaunches camera clients it displaced.
- The desktop and Android companion initially had equivalent but separate root
  command paths. Both now embed one fixed-action controller; desktop low-level
  commands are recovery-only. Camera/photo leases preserve their shared feeder
  and ADB forward independently, while microphone remains a separate opt-in.
- Root action locking could become permanently wedged after `SIGKILL` or reboot.
  Lock recovery now validates the recorded PID and `/proc/<pid>/cmdline`, then
  renames a confirmed stale directory before deleting it so it cannot erase a
  newly acquired lock.
- A live feeder whose TCP listener had failed was repeatedly reused, and lease
  creation failures could leave a hook/feeder half active. Stale feeders are now
  replaced; every camera/photo arm rolls back its new video state, while every
  microphone arm failure also unloads the audio hook.
- Windows deployment deleted committed phone binaries before a replacement was
  complete. It now hashes extracted resources, stages them under a private path,
  verifies device-side SHA-256/size, commits with backups, re-verifies final
  names under an active rollback trap, and only then discards the old set.
- Activity recreation could create a second Root executor while installation was
  still running. A process-owned serial queue now spans Activity instances, and
  stale UI callbacks are discarded after destruction.

## What “one tool” means

There is one supported executable, one selected serial, one source session, one
RTMP parser, one preview, one authenticated H.264 path and one lifecycle owner.
The UI's camera/photo/microphone switches control capabilities inside that
session. They are no more separate engines than video and audio tracks in a media
player.

ReCam-derived source files remain in the repository for donor regression,
license provenance and comparison. Their presence does not cause the launcher
to select them. Likewise, optional CamReplace receiver sources remain buildable
for regression purposes without being exposed as release modes.

## Verification evidence and honest boundary

Automated gates cover Python behavior/guards, ReCam and CamReplace ring models,
secure-channel handshake/tamper/replay, hook dispatch ABI, MediaCodec frame
layout, AVCC parsing, long-duration resampling, Android native cross-compilation,
Windows native compilation and embedded-resource presence.

Those gates cannot emulate a vendor Camera3 implementation, gralloc mapper,
MediaCodec color layout or Audio HAL. A release candidate still needs physical
ARM64 devices across supported Android builds and must exercise preview, record,
JPEG, portrait/landscape, mic enable/disable, publisher pause, USB loss, service
restart and at least 100 repeated start/stop cycles. Until that matrix is run,
the correct claim is “code-audited and build-verified,” not “universally stable.”
