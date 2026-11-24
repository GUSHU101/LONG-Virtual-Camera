/*
 * Назначение файла:
 *   DeviceStack задаёт host-side lifecycle для Android artifacts: install,
 *   uninstall, load/unload hooks и per-mode arming video/audio/photo.
 *
 * Hook point и ABI assumptions:
 *   файл не содержит shell-команд напрямую и не меняет Android ABI. Все
 *   операции с cameraserver/audioserver и cr_feed_proc проходят через
 *   PhoneOps, а exported init symbols cr_camhook_init/cr_audhook_init,
 *   feed URL strings и shared memory v2 остаются совместимыми.
 *
 * Ограничения Pixel/Android:
 *   Pixel 6-10 / Android 12-16 respawn-ят cameraserver/audioserver
 *   асинхронно, поэтому fixed sleeps здесь запрещены как success criteria.
 *   Готовность определяется через process/log/readiness checks в PhoneOps.
 *
 * Подтверждено:
 *   Camera has one tcp_h264 transport; optional audio uses the independent
 *   tcp_pcm transport.
 *
 * Diagnostic-only:
 *   Успешный arm_* подтверждает только готовую инфраструктуру и mapped hook.
 *   Фактическая подмена кадра/PCM подтверждается runtime counters phone-side.
 */

#include "device/DeviceStack.h"

#include <chrono>
#include <thread>

namespace cr::device
{

std::string feed_url_for(FeedScheme scheme)
{
  switch (scheme)
  {
  case FeedScheme::H264:
    return std::string("tcp_h264:listen:") + std::to_string(kVideoTcpPort);
  case FeedScheme::Pcm:
    return std::string("tcp_pcm:listen:") + std::to_string(kAudioTcpPort);
  }
  return "";
}

DeviceStack& DeviceStack::instance()
{
  static DeviceStack s;
  return s;
}

void DeviceStack::reset_session()
{
  std::lock_guard<std::mutex> lk(state_mu_);
  prepared_serials_.clear();
  camera_hook_serials_.clear();
  audio_hook_serials_.clear();
  touched_serials_.clear();
}

std::vector<std::string> DeviceStack::session_serials()
{
  std::lock_guard<std::mutex> lk(state_mu_);
  return {touched_serials_.begin(), touched_serials_.end()};
}

void DeviceStack::remember_serial_(const std::string& serial)
{
  if (serial.empty())
    return;
  std::lock_guard<std::mutex> lk(state_mu_);
  touched_serials_.insert(serial);
}

bool DeviceStack::prepare_artifacts_(const std::string& serial, PhoneOps& ops, const DeployCallback& cb)
{
  // Caller holds bring_up_mu_ for the complete prepare + inject/arm
  // transaction. Releasing it between deployment and use allowed an uninstall
  // on another worker to wipe the files in that gap.
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    if (prepared_serials_.contains(serial))
    {
      if (cb)
        cb(DeployStatus::ok("artifacts already pushed", "serial=" + serial + "; skipping wipe+push"));
      return true;
    }
  }
  // Deployment is deliberately non-disruptive. Revoke abandoned media
  // leases and replace helper files, but defer a service restart until the
  // matching camera/microphone feature is explicitly requested.
  ops.kill_all_feed_procs(cb);
  ops.zero_shm_magic(ShmKind::Feed, cb);
  ops.zero_shm_magic(ShmKind::Audio, cb);
  ops.unforward(kVideoTcpPort, cb);
  ops.unforward(kAudioTcpPort, cb);
  ops.unforward(kRtmpListenPort, cb);
  if (!ops.push_artifacts(cb))
    return false;
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    prepared_serials_.insert(serial);
  }
  return true;
}

bool DeviceStack::cleanup_device_(const std::string& serial, PhoneOps& ops, const DeployCallback& cb)
{
  bool camera_touched = false;
  bool audio_touched = false;
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    camera_touched = camera_hook_serials_.contains(serial);
    audio_touched = audio_hook_serials_.contains(serial);
  }

  // The in-memory owner sets disappear after a host crash. Reconcile them
  // with the service maps so Delete/exit cleanup can still remove an older
  // mapped build without needlessly disturbing an untouched service.
  camera_touched = camera_touched || ops.hook_loaded(HostProc::CameraServer);
  audio_touched = audio_touched || ops.hook_loaded(HostProc::AudioServer);

  // Drop only hooks this process actually prepared. Merely installing or
  // using the virtual camera must never restart the phone's audio service;
  // a mapped hook recovered from a previous crashed session also counts.
  ops.kill_all_feed_procs(cb);
  ops.zero_shm_magic(ShmKind::Feed, cb);
  ops.zero_shm_magic(ShmKind::Audio, cb);
  bool hook_cleanup_ok = true;
  if (camera_touched)
    hook_cleanup_ok = ops.close_camera_clients(/*remember_for_relaunch=*/true, cb) &&
                      hook_cleanup_ok;
  if (camera_touched && audio_touched)
    hook_cleanup_ok = ops.restart_services({"cameraserver", "audioserver"}, cb) &&
                      hook_cleanup_ok;
  else if (camera_touched)
    hook_cleanup_ok = ops.restart_services({"cameraserver"}, cb) && hook_cleanup_ok;
  else if (audio_touched)
    hook_cleanup_ok = ops.restart_services({"audioserver"}, cb) && hook_cleanup_ok;

  if (camera_touched)
    hook_cleanup_ok = ops.wait_host_proc_alive(HostProc::CameraServer, 3000, cb) &&
                      hook_cleanup_ok;
  if (audio_touched)
    hook_cleanup_ok = ops.wait_host_proc_alive(HostProc::AudioServer, 3000, cb) &&
                      hook_cleanup_ok;
  if (camera_touched)
    ops.relaunch_remembered_camera_clients(cb);

  const bool wiped = ops.wipe_artifacts(cb);
  ops.unforward(kVideoTcpPort, cb);
  ops.unforward(kAudioTcpPort, cb);
  ops.unforward(kRtmpListenPort, cb);

  {
    std::lock_guard<std::mutex> sl(state_mu_);
    prepared_serials_.erase(serial);
    camera_hook_serials_.erase(serial);
    audio_hook_serials_.erase(serial);
  }
  return wiped && hook_cleanup_ok;
}

// === Software lifecycle ===

bool DeviceStack::install(const std::string& serial, const DeployCallback& cb)
{
  remember_serial_(serial);
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  return prepare_artifacts_(serial, ops, cb);
}

bool DeviceStack::uninstall(const std::string& serial, const DeployCallback& cb)
{
  remember_serial_(serial);
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  return cleanup_device_(serial, ops, cb);
}

bool DeviceStack::cleanup(const std::string& serial, const DeployCallback& cb)
{
  remember_serial_(serial);
  return uninstall(serial, cb);
}

bool DeviceStack::load_hooks(const std::string& serial, const DeployCallback& cb)
{
  remember_serial_(serial);
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  if (!prepare_artifacts_(serial, ops, cb))
    return false;

  bool refresh_service = false;
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    refresh_service = !camera_hook_serials_.contains(serial);
  }

  // Preload only the camera hook. The phone microphone is an explicit opt-in:
  // audioserver must not be injected until arm_audio() is reached from the
  // dedicated "Enable virtual microphone" action.
  const bool injected = ops.inject_lib(
      HostProc::CameraServer, "libcr_hooks.so", "cr_camhook_init",
      feed_url_for(FeedScheme::H264), refresh_service, cb);
  if (injected)
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    camera_hook_serials_.insert(serial);
  }
  return injected;
}

bool DeviceStack::unload_hooks(const std::string& serial, bool relaunch_clients, const DeployCallback& cb)
{
  remember_serial_(serial);
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  bool camera_touched = false;
  bool audio_touched = false;
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    camera_touched = camera_hook_serials_.contains(serial);
    audio_touched = audio_hook_serials_.contains(serial);
  }
  camera_touched = camera_touched || ops.hook_loaded(HostProc::CameraServer);
  audio_touched = audio_touched || ops.hook_loaded(HostProc::AudioServer);
  ops.kill_all_feed_procs(cb);
  ops.zero_shm_magic(ShmKind::Feed, cb);
  ops.zero_shm_magic(ShmKind::Audio, cb);
  if (camera_touched)
    ops.close_camera_clients(relaunch_clients, cb);
  bool hooks_unloaded = true;
  if (camera_touched && audio_touched)
    hooks_unloaded = ops.restart_services({"cameraserver", "audioserver"}, cb);
  else if (camera_touched)
    hooks_unloaded = ops.restart_services({"cameraserver"}, cb);
  else if (audio_touched)
    hooks_unloaded = ops.restart_services({"audioserver"}, cb);
  if (camera_touched)
    hooks_unloaded = ops.wait_host_proc_alive(HostProc::CameraServer, 3000, cb) &&
                     hooks_unloaded;
  if (audio_touched)
    hooks_unloaded = ops.wait_host_proc_alive(HostProc::AudioServer, 3000, cb) &&
                     hooks_unloaded;
  if (relaunch_clients && camera_touched)
  {
    ops.relaunch_remembered_camera_clients(cb);
  }
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    camera_hook_serials_.erase(serial);
    audio_hook_serials_.erase(serial);
  }
  return hooks_unloaded;
}

// === Per-mode arming ===

bool DeviceStack::arm_video(const std::string& serial, const DeployCallback& cb)
{
  remember_serial_(serial);
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  if (!prepare_artifacts_(serial, ops, cb))
    return false;

  constexpr FeedScheme scheme = FeedScheme::H264;
  const std::string url = feed_url_for(FeedScheme::H264);
  const auto rollback_video = [&]()
  {
    ops.disable_video_replace_passthrough(cb);
    ops.kill_feed_proc(FeedScheme::H264, cb);
    ops.zero_shm_magic(ShmKind::Feed, cb);
    ops.unforward(kVideoTcpPort, cb);
  };

  if (!ops.forward(kVideoTcpPort, cb))
    return false;

  if (!ops.feed_proc_running(scheme))
  {
    if (!ops.launch_feed(url, "cr_feed.log", cb))
    {
      rollback_video();
      return false;
    }
  }
  else
  {
    if (cb)
      cb(DeployStatus::ok("cr_feed_proc(video) already running", "reuse"));
  }
  if (!ops.wait_feed_ready(scheme, "cr_feed.log", 4000, cb))
  {
    rollback_video();
    return false;
  }

  bool refresh_service = false;
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    refresh_service = !camera_hook_serials_.contains(serial);
  }
  if (!ops.inject_lib(HostProc::CameraServer, "libcr_hooks.so",
                      "cr_camhook_init", url, refresh_service, cb))
  {
    rollback_video();
    return false;
  }
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    camera_hook_serials_.insert(serial);
  }
  return true;
}

void DeviceStack::disarm_video(const std::string& serial, const DeployCallback& cb)
{
  remember_serial_(serial);
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  ops.disable_video_replace_passthrough(cb);
  if (cb)
    cb(DeployStatus::info("wait camera pass-through frame window"));
  // Not a readiness criterion: this is a grace window for already-open
  // camera clients to queue at least one real sensor frame before we stop
  // the feed process and invalidate the shm.
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  if (cb)
    cb(DeployStatus::ok("wait camera pass-through frame window", "350ms"));
  ops.kill_feed_proc(FeedScheme::H264, cb);
  ops.zero_shm_magic(ShmKind::Feed, cb);
  ops.unforward(kVideoTcpPort, cb);
}

bool DeviceStack::arm_audio(const std::string& serial, const DeployCallback& cb)
{
  remember_serial_(serial);
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  if (!prepare_artifacts_(serial, ops, cb))
    return false;

  if (!ops.forward(kAudioTcpPort, cb))
    return false;

  if (!ops.feed_proc_running(FeedScheme::Pcm))
  {
    if (!ops.launch_feed(feed_url_for(FeedScheme::Pcm), "cr_feed_audio.log", cb))
    {
      ops.unforward(kAudioTcpPort, cb);
      return false;
    }
  }
  else
  {
    if (cb)
      cb(DeployStatus::ok("cr_feed_proc(audio) already running", "reuse"));
  }
  if (!ops.wait_feed_ready(FeedScheme::Pcm, "cr_feed_audio.log", 4000, cb))
  {
    ops.kill_feed_proc(FeedScheme::Pcm, cb);
    ops.zero_shm_magic(ShmKind::Audio, cb);
    ops.unforward(kAudioTcpPort, cb);
    return false;
  }

  bool refresh_service = false;
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    refresh_service = !audio_hook_serials_.contains(serial);
  }
  if (!ops.inject_lib(HostProc::AudioServer, "libcr_hooks.so",
                      "cr_audhook_init", feed_url_for(FeedScheme::Pcm),
                      refresh_service, cb))
  {
    ops.kill_feed_proc(FeedScheme::Pcm, cb);
    ops.zero_shm_magic(ShmKind::Audio, cb);
    ops.unforward(kAudioTcpPort, cb);
    return false;
  }
  {
    std::lock_guard<std::mutex> sl(state_mu_);
    audio_hook_serials_.insert(serial);
  }
  return true;
}

void DeviceStack::disarm_audio(const std::string& serial, const DeployCallback& cb)
{
  remember_serial_(serial);
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  ops.kill_feed_proc(FeedScheme::Pcm, cb);
  ops.zero_shm_magic(ShmKind::Audio, cb);
  ops.unforward(kAudioTcpPort, cb);
}

bool DeviceStack::enable_photo_mode(const std::string& serial, const DeployCallback& cb)
{
  remember_serial_(serial);
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  return ops.enable_photo_mode(cb);
}

bool DeviceStack::disable_photo_mode(const std::string& serial, const DeployCallback& cb)
{
  remember_serial_(serial);
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  return ops.disable_photo_mode(cb);
}

} // namespace cr::device
