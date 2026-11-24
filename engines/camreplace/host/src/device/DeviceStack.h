/*
 * Назначение файла:
 *   DeviceStack — публичный host-side фасад lifecycle для установки,
 *   загрузки hooks и запуска video/audio/photo режимов на rooted Pixel.
 *
 * Hook point и ABI assumptions:
 *   класс не раскрывает phone-side structs и не меняет shared memory v2.
 *   Он только выбирает feed scheme, порты и exported init functions, которые
 *   PhoneOps передаёт cr_injector. Public C++ API оставлен совместимым.
 *
 * Ограничения Pixel/Android:
 *   Video uses one tcp_h264 lane. Audio tcp_pcm is independent, and readiness
 *   is verified through process/log evidence rather than fixed sleeps.
 *   процесс и log listener, а не через fixed sleep.
 *
 * Подтверждено:
 *   Camera replacement идёт через cameraserver/camhook, audio replacement —
 *   через audioserver/audhook. Photo mode — sentinel-файл /data/cr/photo_mode.
 *
 * Diagnostic-only:
 *   load_hooks без feed process оставляет hooks в pass-through; успешный API
 *   вызов не означает, что кадры или PCM уже заменяются.
 */

#pragma once

// DeviceStack — the phone-side half of the camera-replace machinery.
// Owns:
//   * per-device prepared serial set (idempotent push without cross-device leaks)
//   * the bring-up mutex that serialises concurrent start_replace_*
//   * the port constants the cr_feed_proc tunnels listen on
// Delegates ALL shell composition to `PhoneOps`. DeviceStack itself is
// just sequencing: "for arm_video — forward the port, launch the H.264
// feeder, inject camhook". Each step
// is a single PhoneOps call.
// Why split this out of FeedController: the old monolith mixed
// phone-side orchestration with host-side state (ObsRtmpSource lifecycle,
// transport selection). After the split FeedController is ~150 lines of
// pure intent ("the user pressed Start replace camera, do these two
// things and update mode flags") and DeviceStack/HostStack each own
// their own world.

#include "device/PhoneOps.h"
#include "device/DeployStatus.h"

#include <mutex>
#include <string>
#include <vector>
#include <unordered_set>

namespace cr::device
{

// adb-forward ports for the host↔phone tunnels.
constexpr int kVideoTcpPort = 8901;   // → /data/cr/feed
constexpr int kAudioTcpPort = 8902;   // → /data/cr/audio
constexpr int kRtmpListenPort = 1935; // PC-side RtmpServer (OBS dest)

// Build the feed URL string for a given scheme. cr_feed_proc parses
// these in feed_main.cpp.
std::string feed_url_for(FeedScheme scheme);

class DeviceStack
{
public:
  static DeviceStack& instance();

  // Drop every per-device prepared-session flag. Call from
  // shutdown_all / uninstall so the next push runs the clean step.
  void reset_session();

  // Devices actually touched by this process. Shutdown cleanup is deliberately
  // scoped to this set; merely connecting/authorizing a phone must never make
  // it a cleanup target.
  std::vector<std::string> session_serials();

  // === Software lifecycle ===
  // Pure file copy. No service restarts, no hooks loaded.
  bool install(const std::string& serial, const DeployCallback& cb);

  // Drop hooks (via service restart) + rm artifacts + unforward all
  // adb-forward rules we ever installed.
  bool uninstall(const std::string& serial, const DeployCallback& cb);

  // Best-effort destructive cleanup. Same phone-side wipe as uninstall,
  // Exposed for app shutdown and crash cleanup.
  // recovery before the next push.
  bool cleanup(const std::string& serial, const DeployCallback& cb);

  // Preload the camera hook only. Audio is intentionally untouched until the
  // user explicitly enables the virtual microphone through arm_audio().
  // No cr_feed_proc is launched; the hook remains pass-through until armed.
  bool load_hooks(const std::string& serial, const DeployCallback& cb);

  // Restart only services whose hooks this process prepared. `relaunch_clients`
  // re-monkey-launches camera apps closed while dropping the camera hook.
  bool unload_hooks(const std::string& serial, bool relaunch_clients, const DeployCallback& cb);

  // === Per-mode arming (for replace_*) ===
  // Forward port + launch cr_feed_proc + ensure camhook is loaded.
  // Idempotent on hook load. H.264 is the product's only video transport.
  bool arm_video(const std::string& serial, const DeployCallback& cb);

  // Kill cr_feed_proc(video) + zero feed shm magic. Hook stays loaded
  // and falls through to real sensor. Audio side untouched.
  void disarm_video(const std::string& serial, const DeployCallback& cb);

  // Forward port + launch cr_feed_proc(tcp_pcm) + ensure audhook is
  // loaded. Idempotent.
  bool arm_audio(const std::string& serial, const DeployCallback& cb);

  // Kill cr_feed_proc(audio) + zero audio shm magic. Audhook stays
  // loaded and falls through to real mic. Video side untouched.
  void disarm_audio(const std::string& serial, const DeployCallback& cb);

  // Touch / remove the photo_mode sentinel under /data/cr.
  bool enable_photo_mode(const std::string& serial, const DeployCallback& cb);
  bool disable_photo_mode(const std::string& serial, const DeployCallback& cb);

private:
  DeviceStack() = default;

  // Push artifacts if not pushed yet this session.
  bool prepare_artifacts_(const std::string& serial, PhoneOps& ops, const DeployCallback& cb);
  bool cleanup_device_(const std::string& serial, PhoneOps& ops, const DeployCallback& cb);
  void remember_serial_(const std::string& serial);

  // Serialises concurrent start_replace_* threads through arm_video /
  // arm_audio so push_artifacts and inject can't interleave.
  std::mutex bring_up_mu_;

  std::mutex state_mu_;
  // Preparation is per device. A single process can switch between connected
  // phones; treating this as one global boolean skipped deployment on the
  // second serial and then operated on missing/stale artifacts.
  std::unordered_set<std::string> prepared_serials_;
  // A service enters these sets only after this process explicitly prepares
  // it for injection. Camera-only use therefore never restarts audioserver.
  std::unordered_set<std::string> camera_hook_serials_;
  std::unordered_set<std::string> audio_hook_serials_;
  std::unordered_set<std::string> touched_serials_;
};

} // namespace cr::device
