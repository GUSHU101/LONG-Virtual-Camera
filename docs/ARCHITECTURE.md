# Android runtime architecture

The product is the ARM64 Android APK in `lsposed-module` and its native payload in `engines/camreplace/android`. The static LSPosed scope contains `system`. The APK runs a verified Root controller to manage one instance of the native pipeline on the phone.

Local video and images publish NV21 through the same triple-buffered file used by OBS H.264. Local audio and OBS AAC publish S16LE into the same PCM ring. Video and photo share a feed and have separate enable flags. Microphone replacement uses an explicit lease. While the microphone is off, the original device microphone handles capture.

The shared video buffer has a fixed pixel budget. ReCam-derived SPS geometry and aspect-preserving resizing adapt incoming frames before publication. The camera path center-crops to the requested aspect ratio with chroma alignment, then scales or copies for the destination stride. Reader pins protect the slot until the copy or encode completes. A stale video frame eventually passes through to the device camera.

Media files are written under content-addressed names, verified by length and SHA-256, and referenced from a single atomically renamed source manifest. Video and audio selections have separate fields. OBS authenticates with a generated publisher key and listens on loopback by default. The controller gives the feeder the private configuration location implicitly and keeps the key out of ordinary status output.

Emergency recovery runs separately from ordinary app operations. The controller attempts all cleanup steps, verifies feeder exit and service recovery, and reports error if any step fails. Root process cleanup and the device-side wait remain bounded. The platform and root manager determine whether subordinate processes can be fully cleaned up after a forced interruption.

Quality CI runs Python contract checks, controller fault injection, native geometry and ring tests, APK lint, signing checks and packaged payload verification. Official release builds require signing secrets. Testing Android vendor buffer access, concurrent camera clients, microphone restoration, stream reconnect and long-running stability requires a device.

See the [global code audit](GLOBAL-CODE-AUDIT.md) for confirmed defects, fixes and remaining verification coverage.
