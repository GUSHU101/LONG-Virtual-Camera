#include "source/HostStack.h"
#include "device/DeviceStack.h" // for kVideoTcpPort / kRtmpListenPort

#include <algorithm>
#include <memory>

namespace cr::source
{

namespace
{
bool same_source_config(const DesktopSourceConfig& a, const DesktopSourceConfig& b)
{
  return a.kind == b.kind && a.value == b.value && a.fps == b.fps &&
         a.max_height == b.max_height;
}
} // namespace

HostStack& HostStack::instance()
{
  static HostStack s;
  return s;
}

bool HostStack::ensure_running(const std::string& serial, const cr::device::DeployCallback& cb, bool preview_decode)
{
  std::shared_ptr<ObsRtmpSource> old_obs;
  std::shared_ptr<DesktopSource> old_desktop;
  std::shared_ptr<ObsRtmpSource> existing_obs;
  std::shared_ptr<DesktopSource> existing_desktop;
  DesktopSourceConfig config;
  bool restart_existing_source = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    config = source_config_;
    if (obs_ && obs_->running() && session_serial_ == serial)
    {
      existing_obs = obs_;
      existing_desktop = desktop_source_;
      restart_existing_source = config.kind != DesktopSourceKind::Obs &&
                                (!existing_desktop || !existing_desktop->running());
    }
    else
    {
      if (obs_ && obs_->running() && session_serial_ != serial &&
          (camera_on_ || sound_on_ || photo_on_))
      {
        if (cb)
          cb(cr::device::DeployStatus::err(
              "host session belongs to another device",
              "active=" + session_serial_ + "; requested=" + serial +
                  "; stop the active modes before switching"));
        return false;
      }
      old_obs = std::move(obs_);
      old_desktop = std::move(desktop_source_);
      session_serial_.clear();
    }
  }

  // Process and socket shutdown may block. Keep it outside mu_ so the render
  // thread can continue polling status and painting progress.
  if (old_desktop)
    old_desktop->stop();
  if (old_obs)
    old_obs->stop();

  if (existing_obs)
  {
    if (preview_decode)
      existing_obs->set_preview_decode(true);
    if (!restart_existing_source)
      return true;

    auto desktop = existing_desktop ? existing_desktop : std::make_shared<DesktopSource>();
    if (!desktop->start(config, cr::device::kRtmpListenPort, "cr"))
    {
      if (cb)
        cb(cr::device::DeployStatus::err("source: ffmpeg restart failed",
                                         desktop->status().detail));
      return false;
    }
    bool accepted = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (obs_ == existing_obs && session_serial_ == serial)
      {
        desktop_source_ = desktop;
        accepted = true;
      }
    }
    if (!accepted)
    {
      desktop->stop();
      return false;
    }
    return true;
  }

  auto src = std::make_shared<ObsRtmpSource>();
  ObsRtmpSource::Config oc{};
  oc.adb_port = cr::device::kVideoTcpPort;
  oc.rtmp_port = cr::device::kRtmpListenPort;
  oc.stream_key = "cr";
  oc.video_forward = false; // armed below by camera/photo flips
  oc.preview_decode = preview_decode;
  if (!src->start(serial, oc, cb))
  {
    if (cb)
      cb(cr::device::DeployStatus::err("obs: start failed"));
    return false;
  }
  std::shared_ptr<DesktopSource> desktop;
  if (config.kind != DesktopSourceKind::Obs)
  {
    desktop = std::make_shared<DesktopSource>();
    if (!desktop->start(config, oc.rtmp_port, oc.stream_key))
    {
      if (cb)
        cb(cr::device::DeployStatus::err("source: ffmpeg start failed",
                                         desktop->status().detail));
      src->stop();
      return false;
    }
  }
  bool accepted = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!obs_ && same_source_config(source_config_, config))
    {
      obs_ = src;
      desktop_source_ = desktop;
      session_serial_ = serial;
      accepted = true;
    }
  }
  if (!accepted)
  {
    if (desktop)
      desktop->stop();
    src->stop();
  }
  return accepted;
}

bool HostStack::configure_source(DesktopSourceConfig config)
{
  std::lock_guard<std::mutex> lk(mu_);
  if (obs_ && obs_->running())
    return false;
  config.fps = std::clamp(config.fps, 1, 60);
  config.max_height = std::clamp(config.max_height, 240, 2160);
  source_config_ = std::move(config);
  return true;
}

DesktopSourceConfig HostStack::source_config() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return source_config_;
}

DesktopSourceStatus HostStack::source_status() const
{
  std::shared_ptr<DesktopSource> source;
  DesktopSourceKind kind;
  {
    std::lock_guard<std::mutex> lk(mu_);
    source = desktop_source_;
    kind = source_config_.kind;
  }
  if (source)
    return source->status();
  return {kind, kind == DesktopSourceKind::Obs, 0,
          kind == DesktopSourceKind::Obs ? "external OBS publisher" : "source not running"};
}

bool HostStack::set_camera_active(bool on)
{
  // Never hold mu_ across set_video_forward — it can block for seconds
  // on connect retries, and the UI polls status() on the same mutex
  // every frame (would freeze all clicks, including Start replace *).
  std::shared_ptr<ObsRtmpSource> obs;
  bool want_forward = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on)
    {
      camera_on_ = false;
      if (!obs_)
        return true;
      obs = obs_;
      want_forward = video_should_be_on_locked();
    }
    else
    {
      if (!obs_)
        return false;
      obs = obs_;
      want_forward = true; // camera (or photo) enable always needs forward
    }
  }

  if (!obs->set_video_forward(want_forward))
  {
    // Leave camera_on_ false on failed enable so teardown_if_idle
    // can still drop the OBS source and Stop UI stays coherent.
    return false;
  }

  if (on)
  {
    std::lock_guard<std::mutex> lk(mu_);
    camera_on_ = true;
  }
  return true;
}

bool HostStack::set_photo_active(bool on)
{
  std::shared_ptr<ObsRtmpSource> obs;
  bool want_forward = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on)
    {
      photo_on_ = false;
      if (!obs_)
        return true;
      obs = obs_;
      want_forward = video_should_be_on_locked();
    }
    else
    {
      if (!obs_)
        return false;
      obs = obs_;
      want_forward = true; // camera (or photo) enable always needs forward
    }
  }

  if (!obs->set_video_forward(want_forward))
  {
    return false;
  }

  if (on)
  {
    std::lock_guard<std::mutex> lk(mu_);
    photo_on_ = true;
  }
  return true;
}

bool HostStack::start_sound(const AudioPump::Config& cfg, const cr::device::DeployCallback& cb)
{
  std::shared_ptr<ObsRtmpSource> obs;
  {
    std::lock_guard<std::mutex> lk(mu_);
    obs = obs_;
  }
  if (!obs || !obs->start_audio(cfg, cb))
    return false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (obs_ != obs)
    {
      obs->stop_audio();
      return false;
    }
    sound_on_ = true;
  }
  return true;
}

void HostStack::stop_sound()
{
  std::shared_ptr<ObsRtmpSource> obs;
  {
    std::lock_guard<std::mutex> lk(mu_);
    sound_on_ = false;
    obs = obs_;
  }
  if (obs)
    obs->stop_audio();
}

bool HostStack::camera_active() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return camera_on_;
}
bool HostStack::photo_active() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return photo_on_;
}
bool HostStack::sound_active() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return sound_on_;
}
bool HostStack::any_active() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return camera_on_ || sound_on_ || photo_on_;
}

std::string HostStack::session_serial() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return session_serial_;
}

void HostStack::teardown_if_idle()
{
  std::shared_ptr<ObsRtmpSource> obs;
  std::shared_ptr<DesktopSource> desktop;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (camera_on_ || sound_on_ || photo_on_)
      return;
    obs = std::move(obs_);
    desktop = std::move(desktop_source_);
    session_serial_.clear();
  }
  // Process/socket shutdown can take seconds. Never hold the state mutex
  // while joining them; the render loop polls status() every frame.
  if (desktop)
    desktop->stop();
  if (obs)
    obs->stop();
}

void HostStack::shutdown()
{
  std::shared_ptr<ObsRtmpSource> obs;
  std::shared_ptr<DesktopSource> desktop;
  {
    std::lock_guard<std::mutex> lk(mu_);
    obs = std::move(obs_);
    desktop = std::move(desktop_source_);
    session_serial_.clear();
    camera_on_ = sound_on_ = photo_on_ = false;
  }
  if (desktop)
    desktop->stop();
  if (obs)
    obs->stop();
}

ObsRtmpSource::Status HostStack::status() const
{
  std::lock_guard<std::mutex> lk(mu_);
  if (obs_)
    return obs_->status();
  return {};
}

} // namespace cr::source
