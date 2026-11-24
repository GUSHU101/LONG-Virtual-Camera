#pragma once

// ReCam-derived desktop source adapter for the unified native host.
//
// OBS remains a first-class external publisher, but it is no longer the only
// input. This adapter owns one ffmpeg child and publishes a test pattern,
// looping file/URL, or DirectShow camera into the same loopback RTMP ingest
// used by camera/photo/audio replacement. It uses CreateProcess directly (no
// shell), a kill-on-close Job object, bounded shutdown and redacted logging.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cr::source
{

enum class DesktopSourceKind : std::uint8_t
{
  Obs = 0,
  TestPattern = 1,
  FileOrUrl = 2,
  Camera = 3,
};

struct DesktopSourceConfig
{
  DesktopSourceKind kind{DesktopSourceKind::Obs};
  std::string value;
  int fps{30};
  int max_height{1080};
};

struct DesktopSourceStatus
{
  DesktopSourceKind kind{DesktopSourceKind::Obs};
  bool running{false};
  std::uint32_t exit_code{0};
  std::string detail;
};

class DesktopSource
{
public:
  DesktopSource() = default;
  ~DesktopSource();
  DesktopSource(const DesktopSource&) = delete;
  DesktopSource& operator=(const DesktopSource&) = delete;

  bool start(const DesktopSourceConfig& config, int rtmp_port,
             const std::string& stream_key);
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] DesktopSourceStatus status() const;
  [[nodiscard]] static std::vector<std::string> list_cameras() noexcept;

private:
  void drain_(void* pipe_handle, void* process_handle) noexcept;

  mutable std::mutex mu_;
  DesktopSourceStatus status_{};
  void* process_{nullptr};
  void* job_{nullptr};
  void* stderr_read_{nullptr};
  std::thread drain_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
};

} // namespace cr::source
