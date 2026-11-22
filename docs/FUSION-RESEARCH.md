# Fusion research record

## Scope and provenance

The fusion started from full source snapshots of:

- ReCam `44f99681079c04e3b3e6d3a74fef96f03b480348`;
- CamReplace `8456a5d8041f4c81a26b5c19fa0d6409f57db048`.

Repository-owned C/C++, Python, build scripts, workflow files and public
documentation were read. Vendored dependencies were inspected at their build,
API and privilege boundaries rather than independently re-auditing every
upstream implementation. Both projects are now present as maintained source,
not merely used as design references.

## Why the product retains two engines

The implementations hook different private Android surfaces and have different
host assumptions. Combining their payloads into one process would increase ABI
and rollback risk without providing a user benefit. The new product therefore
uses one launcher, one policy set and one release identity while retaining two
selectable engines:

- the ReCam-derived stable engine provides a conservative cross-platform video
  path, strong symbol/prologue validation and multi-geometry frame lanes;
- the CamReplace-derived enhanced engine provides the Windows OBS experience,
  optional virtual microphone and JPEG still replacement.

This is deliberate component-level fusion: fixes and invariants are shared,
while incompatible hook mechanisms remain isolated behind an engine boundary.

## Cross-project findings and implemented repairs

| Area | Finding | Fusion repair |
|---|---|---|
| Camera fences | CPU access could start before HAL completion; ReCam discarded an unlock fence | Incoming fences are consumed by AHB/GBM locks; linear unlock is synchronous |
| Native buffer conversion | CamReplace cast a private window buffer pointer to AHB | Resolve and use `ANativeWindowBuffer_getHardwareBuffer`; unsupported paths pass through |
| Frame ownership | CamReplace triple buffer could overwrite an active reader | Protocol v2 adds atomic reader pins and an exclusive writer token |
| JPEG source | Encoding read an unpinned changing slot | The source remains pinned for the complete encode/cache operation |
| Audio privacy | Short reads/format probing could leave bytes from the real mic | Once virtual mic is active, clear the full client buffer before any delivery; underrun is silence |
| Audio consent | Audio pass-through contract was unclear | Explicit independent OFF/ON switch; OFF is default and does not arm/inject audio |
| SELinux | CamReplace invoked global `setenforce 0` | Keep enforcing; calculate/apply scoped allow rules and verify them |
| Runtime files | `/data/cr` files were world writable | Per-service ownership, directory `0711`, channels/markers `0600`, photo input `0400` |
| Readiness | Host could manufacture a successful ready marker | Only the injected target service may write its pre-created marker |
| Injection return | A null fault address could be mistaken for a successful remote call | Only the designated PC return trap is accepted |
| Private ABI | Prefix matches could select an unknown C++ signature | Unknown signature families fail closed; stable engine also checks live prologue bytes |
| Hook shutdown | Mappings could be freed while an in-flight trampoline still referenced them | Disable substitution but retain the tiny mapping until the owning service exits |
| Lanczos math | Q15 unity `32768` overflowed `int16_t` | Store weights in `int32_t` |
| Dimensions | Enhanced feed mishandled portrait limits | Independently bound width/height and require even NV21 geometry |
| Desktop cleanup | Window close did less cleanup than Stop | Shared fail-safe shutdown tries streamer, feeder, payload and ADB forwarding |
| Exposure/logging | RTMP could bind all interfaces and logs contained secrets | Loopback-only listeners and credential/header/token redaction |
| Local ticket | Development tickets used an all-zero proof and had no maximum accepted duration | Authenticate the canonical ticket with the ephemeral X25519 shared secret; enforce a one-hour lifetime plus clock/expiry checks |
| Async lifecycle | Detached Start/Stop workers could interleave device, forwarding and OBS state | Serialize complete lifecycle transactions and make every failed start roll back phone and host stages |
| Multi-device | One global deploy flag and no host-session owner leaked state across serials | Track prepared artifacts per serial, bind the host pipeline to one owning serial and lock device switching while active |
| UI callbacks | Old worker callbacks could overwrite the state of a newer user action | Atomic mode flags plus per-mode operation generations reject stale completions |

## Virtual microphone state machine

```text
OFF (default)
  phone microphone is untouched
  no PCM feeder
  no audioserver injection
        │ explicit Enable
        ▼
ARMING ── failure ──► OFF
        │ ready marker + PCM channel READY
        ▼
ON
  OBS PCM replaces the complete output buffer
  missing/short/unknown-format input => silence
        │ explicit Disable
        ▼
OFF
  stop feeder + clear shared magic
  loaded hook falls through to the phone microphone
```

Camera and photo modes never transition the microphone state.

## Remaining engineering limits

- Both engines depend on private Android camera-service behavior; compatibility
  is firmware/SoC-specific and unsupported layouts pass through.
- Enhanced mode is presently arm64 Android plus a Windows x64 host.
- The enhanced local secure-channel ticket is authenticated with a keyed proof
  derived from both ephemeral X25519 keys, replacing the historical all-zero
  signature. It protects handshake integrity and key possession over USB/loopback,
  but it is deliberately not persistent remote identity authentication.
- Retaining an enhanced hook mapping until service exit is an intentional safety
  tradeoff. A service restart/reboot is the complete unload mechanism.
- Physical-device regression is still required for each Android build family,
  gralloc layout, camera output format and audio HAL format.

## Release gates

A release is acceptable only when Python tests, both ring model tests, the
authenticated-channel handshake/replay/tamper tests, both NDK builds and source
safety guards pass. Device validation should cover start/stop, service death,
USB disconnect, device switching, repeated rapid Start/Stop, video underrun,
audio underrun, audio opt-in/out, portrait/landscape, preview/record/JPEG, and
recovery after reboot.
