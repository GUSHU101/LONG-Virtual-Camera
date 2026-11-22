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
 *   Комбинации tcp_h264/tcp_nv21 для camera и tcp_pcm для audio запускаются
 *   раздельно, video schemes делят один TCP port и перед переключением
 *   должны дождаться остановки старого cr_feed_proc.
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
  case FeedScheme::Nv21:
    return std::string("tcp_nv21:listen:") + std::to_string(kVideoTcpPort);
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
  if (!cleanup_device_(serial, ops, cb))
    return false;
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
  // Drop hooks first; otherwise unlinked libraries can remain mapped
  // inside cameraserver/audioserver until the next service restart.
  ops.kill_all_feed_procs(cb);
  ops.zero_shm_magic(ShmKind::Feed, cb);
  ops.zero_shm_magic(ShmKind::Audio, cb);
  ops.close_camera_clients(/*remember_for_relaunch=*/false, cb);

  const std::string cam = "cameraserver";
  const std::string aud = "audioserver";
  ops.restart_services({cam.c_str(), aud.c_str()}, cb);

  const bool wiped = ops.wipe_artifacts(cb);
  ops.unforward(kVideoTcpPort, cb);
  ops.unforward(kAudioTcpPort, cb);
  ops.unforward(kRtmpListenPort, cb);

  {
    std::lock_guard<std::mutex> sl(state_mu_);
    prepared_serials_.erase(serial);
  }
  return wiped;
}

// === Software lifecycle ===

bool DeviceStack::install(const std::string& serial, const DeployCallback& cb)
{
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  return prepare_artifacts_(serial, ops, cb);
}

bool DeviceStack::uninstall(const std::string& serial, const DeployCallback& cb)
{
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  return cleanup_device_(serial, ops, cb);
}

bool DeviceStack::cleanup(const std::string& serial, const DeployCallback& cb)
{
  return uninstall(serial, cb);
}

bool DeviceStack::load_hooks(const std::string& serial, FeedScheme video_scheme, const DeployCallback& cb)
{
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  if (!prepare_artifacts_(serial, ops, cb))
    return false;

  // Preload only the camera hook. The phone microphone is an explicit opt-in:
  // audioserver must not be injected until arm_audio() is reached from the
  // dedicated "Enable virtual microphone" action.
  return ops.inject_lib(HostProc::CameraServer, "libcr_hooks.so", "cr_camhook_init", feed_url_for(video_scheme), cb);
}

bool DeviceStack::unload_hooks(const std::string& serial, bool relaunch_clients, const DeployCallback& cb)
{
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  ops.kill_all_feed_procs(cb);
  ops.zero_shm_magic(ShmKind::Feed, cb);
  ops.zero_shm_magic(ShmKind::Audio, cb);
  ops.close_camera_clients(relaunch_clients, cb);

  const std::string cam = "cameraserver";
  const std::string aud = "audioserver";
  ops.restart_services({cam.c_str(), aud.c_str()}, cb);
  if (relaunch_clients)
  {
    ops.relaunch_remembered_camera_clients(cb);
  }
  return true;
}

// === Per-mode arming ===

bool DeviceStack::arm_video(const std::string& serial, FeedScheme scheme, const DeployCallback& cb)
{
  if (scheme == FeedScheme::Pcm)
    return false; // wrong domain

  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  if (!prepare_artifacts_(serial, ops, cb))
    return false;

  const FeedScheme other = (scheme == FeedScheme::H264) ? FeedScheme::Nv21 : FeedScheme::H264;
  const std::string url = feed_url_for(scheme);
  const auto rollback_video = [&]()
  {
    ops.disable_video_replace_passthrough(cb);
    ops.kill_feed_proc(FeedScheme::H264, cb);
    ops.kill_feed_proc(FeedScheme::Nv21, cb);
    ops.zero_shm_magic(ShmKind::Feed, cb);
    ops.unforward(kVideoTcpPort, cb);
  };

  if (!ops.forward(kVideoTcpPort, cb))
    return false;

  if (ops.feed_proc_running(other))
  {
    ops.kill_feed_proc(other, cb);
    if (!ops.wait_feed_stopped(other, 1500, cb))
    {
      rollback_video();
      return false;
    }
  }

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

  if (!ops.inject_lib(HostProc::CameraServer, "libcr_hooks.so", "cr_camhook_init", url, cb))
  {
    rollback_video();
    return false;
  }
  return true;
}

void DeviceStack::disarm_video(const std::string& serial, const DeployCallback& cb)
{
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
  // Kill BOTH schemes — across a Stop/Start with a transport switch
  // we'd otherwise leave the previous-scheme cr_feed_proc orphaned.
  ops.kill_feed_proc(FeedScheme::H264, cb);
  ops.kill_feed_proc(FeedScheme::Nv21, cb);
  ops.zero_shm_magic(ShmKind::Feed, cb);
  ops.unforward(kVideoTcpPort, cb);
}

bool DeviceStack::arm_audio(const std::string& serial, const DeployCallback& cb)
{
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

  if (!ops.inject_lib(HostProc::AudioServer, "libcr_hooks.so", "cr_audhook_init", feed_url_for(FeedScheme::Pcm), cb))
  {
    ops.kill_feed_proc(FeedScheme::Pcm, cb);
    ops.zero_shm_magic(ShmKind::Audio, cb);
    ops.unforward(kAudioTcpPort, cb);
    return false;
  }
  return true;
}

void DeviceStack::disarm_audio(const std::string& serial, const DeployCallback& cb)
{
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  ops.kill_feed_proc(FeedScheme::Pcm, cb);
  ops.zero_shm_magic(ShmKind::Audio, cb);
  ops.unforward(kAudioTcpPort, cb);
}

bool DeviceStack::enable_photo_mode(const std::string& serial, const DeployCallback& cb)
{
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  return ops.enable_photo_mode(cb);
}

bool DeviceStack::disable_photo_mode(const std::string& serial, const DeployCallback& cb)
{
  PhoneOps ops(serial);
  std::lock_guard<std::mutex> lk(bring_up_mu_);
  return ops.disable_photo_mode(cb);
}

} // namespace cr::device
