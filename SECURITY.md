# Security policy

## Supported branch

Security fixes are applied to `main`. Reports should identify the commit, Android
build fingerprint, device/SoC, root solution and the smallest reproducible input.

## Reporting

Please use GitHub's private vulnerability reporting for this repository. Do not
attach unredacted `logcat`, payload logs or OBS setup screens to a public issue.
Those can contain device identifiers, local paths or streaming credentials.

## Security invariants

- The project never intentionally disables SELinux enforcement.
- Runtime files are created with restrictive permissions.
- OBS binds loopback by default. LAN listening is opt-in and uses
  unencrypted RTMP with a generated publisher key.
- Unsupported camera layouts and unknown private ABIs fail closed or pass through.
- Emergency Stop revokes media and checks recovery. Leaving the control panel
  open or closing it does not control the native service lifetime.
- Missing video passes through to the original camera.
- The virtual microphone is OFF by default and is armed only by an explicit user
  action. While active, underrun/format failure produces silence, never bytes
  from the phone microphone.

This software injects into a privileged Android service and therefore cannot be
made low-risk. Use a test device, keep a recovery path available, and reboot if a
failed cleanup leaves camera services in an uncertain state.
