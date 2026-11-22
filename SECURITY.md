# Security policy

## Supported branch

Security fixes are applied to `main`. Reports should identify the commit, Android
build fingerprint, device/SoC, root solution and the smallest reproducible input.

## Reporting

Please use GitHub's private vulnerability reporting for this repository. Do not
attach unredacted desktop, `logcat` or payload logs to a public issue; logs can
contain device serials, application identifiers and local paths.

## Security invariants

- The project never intentionally disables SELinux enforcement.
- Runtime files are created with restrictive permissions.
- TCP listeners bind loopback only.
- Unsupported camera layouts and unknown private ABIs fail closed or pass through.
- The Stop and window-close paths both attempt to restore the real camera.
- A missing virtual source produces an explicit test pattern rather than silently
  claiming that replacement is active.
- The virtual microphone is OFF by default and is armed only by an explicit user
  action. While active, underrun/format failure produces silence, never bytes
  from the phone microphone.

This software injects into a privileged Android service and therefore cannot be
made low-risk. Use a test device, keep a recovery path available, and reboot if a
failed cleanup leaves camera services in an uncertain state.
