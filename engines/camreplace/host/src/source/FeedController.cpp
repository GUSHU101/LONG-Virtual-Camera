#include "source/FeedController.h"
#include "source/HostStack.h"
#include "device/DeviceStack.h"
#include "util/Log.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
namespace cr::source
{

namespace
{

// Every mode is one transaction spanning a process-wide OBS source, adb
// forwards and state in one phone. Serialising those transactions prevents a
// fast Start/Stop/device-switch sequence from interleaving half of two modes.
std::mutex g_lifecycle_mu;

// Deployment operations are owned by the process and executed in order. This
// replaces detached threads, which could still call UI callbacks or adb after
// App::run() had already torn down their dependencies.
class OperationExecutor
{
public:
  bool submit(std::function<void()> job)
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!accepting_)
      return false;
    if (!worker_.joinable())
    {
      try
      {
        worker_ = std::thread([this] { run(); });
      }
      catch (...)
      {
        return false;
      }
    }
    jobs_.push_back(std::move(job));
    cv_.notify_one();
    return true;
  }

  void stop_and_join()
  {
    std::thread worker;
    {
      std::lock_guard<std::mutex> lock(mu_);
      accepting_ = false;
      stopping_ = true;
      jobs_.clear();
      worker.swap(worker_);
    }
    cv_.notify_all();
    if (worker.joinable())
      worker.join();
  }

  ~OperationExecutor() { stop_and_join(); }

private:
  void run()
  {
    for (;;)
    {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (stopping_ && jobs_.empty())
          return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  std::thread worker_;
  bool accepting_ = true;
  bool stopping_ = false;
};

OperationExecutor& operation_executor()
{
  static OperationExecutor executor;
  return executor;
}

std::string owning_serial(HostStack& host, const std::string& requested,
                          const cr::device::DeployCallback& cb)
{
  const std::string owner = host.session_serial();
  if (owner.empty() || owner == requested)
    return requested;
  if (cb)
    cb(cr::device::DeployStatus::info(
        "use active session device",
        "active=" + owner + "; selected=" + requested));
  return owner;
}

void report_worker_failure(const std::string& action, const char* phase, const std::string& detail, const cr::device::DeployCallback& cb)
{
  std::string step = action;
  step.append(phase);

  std::string line = step;
  if (!detail.empty())
  {
    line.append(" - ");
    line.append(detail);
  }
  cr::log::error("deploy", line);

  if (cb)
  {
    cb(cr::device::DeployStatus::err(std::move(step), detail));
  }
}

template <class Fn> void launch_worker(std::string action, cr::device::DeployCallback fail_cb, Fn&& fn)
{
  using FnT = std::decay_t<Fn>;
  struct WorkerJob
  {
    std::string action;
    cr::device::DeployCallback fail_cb;
    FnT fn;

    void operator()()
    {
      try
      {
        fn();
      }
      catch (const std::exception& e)
      {
        report_worker_failure(action, ": operation failed", e.what(), fail_cb);
      }
      catch (...)
      {
        report_worker_failure(action, ": operation failed", "unknown exception", fail_cb);
      }
    }
  };

  auto job = std::make_shared<WorkerJob>(WorkerJob{
      std::move(action),
      std::move(fail_cb),
      std::forward<Fn>(fn),
  });
  auto run = [job]() mutable { (*job)(); };

  try
  {
    if (!operation_executor().submit(run))
      report_worker_failure(job->action, ": operation rejected", "controller is shutting down or no worker thread is available", job->fail_cb);
  }
  catch (const std::exception& e)
  {
    report_worker_failure(job->action, ": operation rejected", e.what(), job->fail_cb);
  }
  catch (...)
  {
    report_worker_failure(job->action, ": operation rejected", "unknown executor error", job->fail_cb);
  }
}

void cleanup_log_status(cr::device::DeployStatus s)
{
  std::string line = s.step;
  if (!s.detail.empty())
  {
    std::string detail = s.detail;
    for (char& c : detail)
      if (c == '\n' || c == '\r')
        c = ' ';
    line.append(" - ");
    line.append(detail);
  }
  using K = cr::device::DeployStatus::Kind;
  if (s.kind == K::Err)
    cr::log::warn("cleanup", line);
  else if (s.kind == K::Ok)
    cr::log::ok("cleanup", line);
  else
    cr::log::info("cleanup", line);
}

} // namespace

bool FeedController::set_desktop_source(DesktopSourceConfig config)
{
  return HostStack::instance().configure_source(std::move(config));
}

DesktopSourceConfig FeedController::desktop_source()
{
  return HostStack::instance().source_config();
}

DesktopSourceStatus FeedController::desktop_source_status()
{
  return HostStack::instance().source_status();
}

// Software lifecycle (Install / Delete / Start / Stop)

void FeedController::install_software(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("install software", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
                  if (!cr::device::DeviceStack::instance().install(serial, cb))
                    return;
                  if (cb)
                    cb(cr::device::DeployStatus::ok("install: software pushed", "press Start software to load hooks"));
                });
}

void FeedController::delete_software(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("delete software", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
                  auto& host = HostStack::instance();
                  const std::string target = owning_serial(host, serial, cb);
                  host.shutdown();
                  if (!cr::device::DeviceStack::instance().uninstall(target, cb))
                    return;
                  if (cb)
                    cb(cr::device::DeployStatus::ok("delete: software removed"));
                });
}

void FeedController::start_software(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("start software", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
                  if (!cr::device::DeviceStack::instance().load_hooks(serial, cb))
                    return;
                  if (cb)
                    cb(cr::device::DeployStatus::ok("software running", "camera hook loaded; microphone remains untouched until explicitly enabled"));
                });
}

void FeedController::stop_software(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("stop software", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
                  auto& host = HostStack::instance();
                  const std::string target = owning_serial(host, serial, cb);
                  host.shutdown();
                  cr::device::DeviceStack::instance().unload_hooks(target,
                                                                   /*relaunch_clients=*/true, cb);
                  if (cb)
                    cb(cr::device::DeployStatus::ok("software stopped", "hooks unloaded; artifacts kept (use Delete to wipe)"));
                });
}

// Replace camera

void FeedController::start_replace_camera(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("start replace camera", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
                  auto& dev = cr::device::DeviceStack::instance();
                  auto& host = HostStack::instance();
                  const bool video_already_leased = host.photo_active();

                  if (!host.ensure_running(serial, cb, true))
                    return;
                  if (!video_already_leased && !dev.arm_video(serial, cb))
                  {
                    host.teardown_if_idle();
                    return;
                  }
                  if (!host.set_camera_active(true))
                  {
                    if (cb)
                      cb(cr::device::DeployStatus::err("camera: enable forward failed"));
                    if (!video_already_leased)
                      cr::device::DeviceStack::instance().disarm_video(serial, cb);
                    host.teardown_if_idle();
                    return;
                  }
                  if (cb)
                    cb(cr::device::DeployStatus::ok("OBS RTMP ready", "rtmp://127.0.0.1:1935/live/cr"));
                });
}

void FeedController::stop_replace_camera(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("stop replace camera", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
                  auto& host = HostStack::instance();
                  const std::string target = owning_serial(host, serial, cb);
                  const bool also_photo = host.photo_active();

                  // Photo mode also needs the camhook live + shm filled, so only
                  // disarm the phone-side video pipeline if photo isn't holding it.
                  if (!also_photo)
                  {
                    cr::device::DeviceStack::instance().disarm_video(target, cb);
                  }
                  host.set_camera_active(false);
                  host.teardown_if_idle();
                  if (cb)
                    cb(cr::device::DeployStatus::ok("camera: stopped"));
                });
}

// Replace photo

void FeedController::start_replace_photo(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("start replace photo", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
                  auto& dev = cr::device::DeviceStack::instance();
                  auto& host = HostStack::instance();
                  const bool video_already_leased = host.camera_active();

                  if (!host.ensure_running(serial, cb, true))
                    return;
                  if (!video_already_leased && !dev.arm_video(serial, cb))
                  {
                    host.teardown_if_idle();
                    return;
                  }
                  if (!host.set_photo_active(true))
                  {
                    if (cb)
                      cb(cr::device::DeployStatus::err("photo: enable forward failed"));
                    if (!video_already_leased)
                      cr::device::DeviceStack::instance().disarm_video(serial, cb);
                    host.teardown_if_idle();
                    return;
                  }
                  if (!dev.enable_photo_mode(serial, cb))
                  {
                    // Photo mode is a transaction spanning host forwarding,
                    // video shm and the device sentinel. Undo every completed
                    // stage so an error never leaves camera replacement armed.
                    host.set_photo_active(false);
                    dev.disable_photo_mode(serial, cb);
                    if (!video_already_leased)
                      dev.disarm_video(serial, cb);
                    host.teardown_if_idle();
                    return;
                  }

                  if (cb)
                    cb(cr::device::DeployStatus::ok("Photo replace ready", "Take a photo in any camera app - JPEG will be encoded from the live OBS stream."));
                });
}

void FeedController::stop_replace_photo(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("stop replace photo", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
                  auto& host = HostStack::instance();
                  const std::string target = owning_serial(host, serial, cb);
                  const bool also_camera = host.camera_active();

                  auto& dev = cr::device::DeviceStack::instance();
                  dev.disable_photo_mode(target, cb);
                  if (!also_camera)
                  {
                    dev.disarm_video(target, cb);
                  }
                  host.set_photo_active(false);
                  host.teardown_if_idle();
                  if (cb)
                    cb(cr::device::DeployStatus::ok("photo: stopped"));
                });
}

// Replace sound

void FeedController::start_replace_sound(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("start replace sound", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
                  auto& dev = cr::device::DeviceStack::instance();
                  auto& host = HostStack::instance();

                  if (!host.ensure_running(serial, cb, false))
                  {
                    if (cb)
                      cb(cr::device::DeployStatus::err("audio: rtmp pipeline bring-up failed"));
                    return;
                  }
                  if (!dev.arm_audio(serial, cb))
                  {
                    host.teardown_if_idle();
                    return;
                  }

                  AudioPump::Config ac{};
                  ac.adb_port = cr::device::kAudioTcpPort;
                  // Keep AAC decoder-native PCM on the host. audhook adapts to the
                  // consumer format, so forcing 48 kHz/stereo here just adds an
                  // avoidable resample before the phone may resample again.
                  ac.target_sr = 0;
                  ac.target_ch = 0;
                  if (!host.start_sound(ac, cb))
                  {
                    // arm_audio has already made the phone-side channel READY.
                    // Roll it back before reporting failure so a failed start
                    // can never leave the real microphone silently replaced by
                    // an empty virtual stream.
                    dev.disarm_audio(serial, cb);
                    host.teardown_if_idle();
                    if (cb)
                      cb(cr::device::DeployStatus::err("audio: pump start failed"));
                    return;
                  }
                  if (cb)
                    cb(cr::device::DeployStatus::ok("OBS audio replace ready"));
                });
}

void FeedController::stop_replace_sound(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("stop replace sound", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
                  auto& host = HostStack::instance();
                  const std::string target = owning_serial(host, serial, cb);
                  // Invalidate the shared channel first. The next audioserver
                  // callback immediately falls through to the phone microphone.
                  cr::device::DeviceStack::instance().disarm_audio(target, cb);
                  host.stop_sound();
                  host.teardown_if_idle();
                  if (cb)
                    cb(cr::device::DeployStatus::ok("sound: stopped"));
                });
}

// Status / shutdown

ObsRtmpSource::Status FeedController::obs_status()
{
  return HostStack::instance().status();
}

void FeedController::shutdown_all()
{
  // Stop accepting new commands, discard commands that never started, and
  // wait for the one in-flight transaction before touching shared resources.
  operation_executor().stop_and_join();
  cleanup_known_devices("app shutdown");
}

void FeedController::cleanup_known_devices(const char* reason)
{
  std::lock_guard<std::mutex> lifecycle(g_lifecycle_mu);
  // Synchronous - joins ObsRtmpSource worker threads so adb subprocesses
  // don't outlive App::run(), then wipes phone-side state while adb is
  // still alive.
  HostStack::instance().shutdown();

  auto& devices = cr::device::DeviceStack::instance();
  const auto serials = devices.session_serials();
  if (serials.empty())
  {
    cr::log::info("cleanup", std::string(reason ? reason : "cleanup") + ": no devices touched by this session");
    devices.reset_session();
    return;
  }

  for (const auto& serial : serials)
  {
    cr::log::info("cleanup", std::string(reason ? reason : "cleanup") + ": wiping " + serial);
    if (!devices.cleanup(serial, cleanup_log_status))
    {
      cr::log::warn("cleanup", "device cleanup reported failure for " + serial);
    }
  }
  devices.reset_session();
}

} // namespace cr::source
