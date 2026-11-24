#pragma once

// FeedController — orchestrates the on-phone camera-replace stack and the
// PC-side unified desktop source.
// The stack has two halves:
//   Software (device-side, persistent):
//     1. Push cr_feed_proc, libcr_hooks.so and cr_injector to
//        /data/local/tmp.
//     2. Load the camera half of libcr_hooks.so into cameraserver. It remains
//        pass-through until a camera/photo lease starts the H.264 feeder.
//   Replace camera (PC-side, per-session):
//     Open a local RTMP listener on 127.0.0.1:1935. OBS or a managed
//     DesktopSource publishes H.264 to that endpoint and we pass each NAL
//     unit straight through to cr_feed_proc on the phone.
// UI entry points enqueue work on one process-owned executor; progress lines
// flow back via DeployCallback. Shutdown cancels work that never started and
// joins the in-flight transaction before host/device teardown.

#include "device/DeployStatus.h"
#include "source/DesktopSource.h"
#include "source/ObsRtmpSource.h"

#include <string>

namespace cr::source
{

class FeedController
{
public:
  // --- Unified desktop input ------------------------------------------
  // OBS, test pattern, file/URL and DirectShow camera all feed the same
  // RTMP/preview/device pipeline. The source is locked for an active session.
  static bool set_desktop_source(DesktopSourceConfig config);
  static DesktopSourceConfig desktop_source();
  static DesktopSourceStatus desktop_source_status();

  // --- "Software" lifecycle --------------------------------------------
  // Push binaries + .so + relabel. Doesn't run anything. Safe to repeat.
  static void install_software(std::string serial, cr::device::DeployCallback cb);

  // Inverse of install_software — kills any running cr_feed_proc, drops only
  // hooks actually found in cameraserver/audioserver, restores displaced
  // camera clients and wipes every artifact we pushed. Idempotent.
  static void delete_software(std::string serial, cr::device::DeployCallback cb);

  // Push (idempotently) + inject the camera hook into cameraserver. No
  // feeder starts until a camera/photo feature is explicitly armed and the
  // microphone/audioserver remain untouched.
  static void start_software(std::string serial, cr::device::DeployCallback cb);

  // Tear down both the device stack and any PC-side OBS source.
  static void stop_software(std::string serial, cr::device::DeployCallback cb);

  // --- "Replace camera" lifecycle --------------------------------------
  // Bring up the device stack (if not already) and start the local OBS
  // RTMP listener. Once OBS connects, frames flow through to the phone.
  static void start_replace_camera(std::string serial, cr::device::DeployCallback cb);

  // Mirror of stop_software — kept as its own name for UI symmetry.
  static void stop_replace_camera(std::string serial, cr::device::DeployCallback cb);

  // --- "Replace photo (still capture)" lifecycle -----------------------
  // Source is the same OBS RTMP stream that camera replacement uses:
  // cr_feed_proc decodes the H.264 frames into /data/cr/feed shm, and
  // the camhook in photo-mode catches BLOB still-capture buffers and
  // fills them with a freshly-encoded JPEG of the latest NV21 slot.
  // Pipeline:
  //   1. Bring up the RTMP listener (idempotent — reuses what
  //      Start replace camera already started, if any).
  //   2. Touch /data/cr/photo_mode (sentinel file the hook checks at
  //      init) so the next inject enables BLOB substitution.
  //   3. (Re-)inject libcr_camhook.so so it picks up photo_mode.
  // Coexists with camera+sound replacement — preview YUV and saved
  // JPEG both get OBS content from the same shm.
  static void start_replace_photo(std::string serial, cr::device::DeployCallback cb);

  static void stop_replace_photo(std::string serial, cr::device::DeployCallback cb);

  // --- "Replace sound" lifecycle ---------------------------------------
  // Forwards on the existing OBS RTMP listener and additionally:
  //   * adb forward tcp:8902
  //   * launches a second cr_feed_proc instance with `tcp_pcm:listen:8902`
  //     that fills /data/cr/audio
  //   * injects libcr_audhook.so into audioserver
  //   * arms the AudioPump so AAC frames from the OBS stream are
  //     decoded → S16LE PCM → tcp_pcm tunnel
  // Independent of camera replacement: each can be on without the other,
  // sharing the same OBS RTMP listener.
  static void start_replace_sound(std::string serial, cr::device::DeployCallback cb);

  // Tear down the audio side: stop the audio pump, kill the audio
  // cr_feed_proc instance, leave camera path untouched.
  static void stop_replace_sound(std::string serial, cr::device::DeployCallback cb);

  // UI snapshot of the active RTMP ingest (empty if not running). Used by
  // the status panel for both external OBS and managed desktop sources.
  static ObsRtmpSource::Status obs_status();

  // Synchronous teardown: stops the active ObsRtmpSource and joins
  // its worker threads, then wipes phone-side artifacts only on devices that
  // this process actually touched. No callbacks are emitted to UI.
  // Called from App::run() during app shutdown so adb subprocesses
  // don't outlive the main loop and no Android payload remains behind.
  static void shutdown_all();

  // Fail-closed cleanup for transport failure and crash
  // recovery. Synchronous and idempotent.
  static void cleanup_known_devices(const char* reason);
};

} // namespace cr::source
