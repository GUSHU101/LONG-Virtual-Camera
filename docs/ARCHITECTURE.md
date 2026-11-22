# Architecture and safety contracts

## Product boundary

`long_virtual_camera.py` selects one of two isolated engines. They share product
policy and tests but never inject both camera payloads into the same service at
the same time.

```text
                         LONG Virtual Camera
                                  │
                    ┌─────────────┴─────────────┐
                    │                           │
          stable engine                  enhanced engine
       ReCam-derived / cross-OS       CamReplace-derived / Windows
          video camera              video + optional mic + JPEG
                    │                           │
                    └──────── Root Android ─────┘
```

The desktop process, ADB transport, root device tools and selected injected
payload are one authorized control plane. Camera/audio applications are
untrusted consumers. Network listeners intentionally bind only desktop/device
loopback.

## Camera buffer contract

1. Unsupported status, geometry, format, layout or private ABI passes through.
2. The original HAL fence FD remains owned by Camera3. A duplicate is handed to
   AHB, or the GBM fallback waits for it before CPU access.
3. Native window buffers use the official AHB conversion function when present;
   private pointer reinterpretation is not a supported path.
4. The selected source slot remains reader-pinned for the complete pixel copy or
   JPEG encode.
5. Linear CPU writes unlock synchronously. An asynchronous optimization is valid
   only if its output fence is correctly merged into the fence forwarded to
   Camera3 on every exit path.
6. The original Camera3 method is called only after replacement writes complete.

## Shared frame ownership

Both ring implementations enforce the same invariant:

- a reader atomically pins a published slot and revalidates publication state;
- a writer may claim only a slot with no readers and marks it exclusively owned;
- pixels and metadata are written before release publication;
- if no slot is available, the producer skips a publication and keeps the last
  complete frame instead of overwriting an active reader.

The exact layouts remain engine-specific for protocol compatibility. Tests in
`tests/ring_access_test.cpp` and `tests/camreplace_ring_test.c` exercise their
ownership models.

## Virtual microphone contract

The microphone starts OFF and is independent of camera/photo state.

- OFF before first enable: no audio feeder, no `audioserver` injection, phone
  microphone passes through normally.
- ON: after channel readiness, the hook clears the full real-HAL result buffer
  before format probing or virtual delivery. Unknown format, underrun and short
  delivery therefore produce silence rather than real microphone bytes.
- Disable: stop audio transport and clear shared magic. Any resident hook sees
  the channel as inactive and immediately calls through without substitution.
- Service restart/reboot fully removes the resident hook mapping.

Audio callbacks must not allocate unbounded memory or wait on the network. New
format conversion needs tests for mono/stereo, sample-rate change, non-frame
tails, underrun and concurrent disable.

## Lifecycle and privilege contract

- Startup is transactional; failure rolls back every completed stage.
- Shutdown attempts every stage even after an earlier error.
- Runtime paths are not world writable. Camera and audio artifacts have separate
  service owners.
- SELinux remains enforcing. Only scoped, verified rules may be added.
- Only target services create readiness evidence; the controller cannot forge it.
- Hook-visible mappings/resources cannot be unmapped until callbacks are drained
  or the owning system service exits.

Root access is still inherently high risk. Use a test device and keep a recovery
path; reboot if a service cannot be restored deterministically.

## Private ABI policy

Android camera-service internals are not an NDK API. A hook candidate must have a
known signature family, supported 64-bit architecture and valid loaded-module
address. The stable engine additionally compares live prologue bytes with the
ELF resolution source and validates relocation. Unknown cases fail closed.

Supporting a new Android release requires adding evidence for its mangling and
structure layout plus a device regression result; a similar symbol prefix alone
is never sufficient.
