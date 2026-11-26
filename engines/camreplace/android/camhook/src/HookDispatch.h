#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace cr::camhook
{

enum class TargetAbiKind : std::uint8_t
{
  QueueWithSurfaceIds,
  QueueLegacy,
};

struct TargetAbiDescriptor
{
  const char* symbol;
  TargetAbiKind kind;
};

enum class ReplacementOutcome : std::uint8_t
{
  PassThrough,
  AttemptedFailed,
  YuvWritten,
  RecordingYuvWritten,
  JpegWritten,
};

struct HookCounters
{
  std::atomic<std::uint64_t> callback_seen{0};
  std::atomic<std::uint64_t> replacement_attempted{0};
  std::atomic<std::uint64_t> yuv_queued{0};
  std::atomic<std::uint64_t> recording_queued{0};
  std::atomic<std::uint64_t> jpeg_queued{0};
  std::atomic<std::uint64_t> pass_through{0};
  std::atomic<std::uint64_t> original_queue_failed{0};
};

// The original queue operation is the authority for the callback result. The
// replacement side effect runs first, but may never suppress, duplicate or
// rewrite the original status_t value.
template <class ReplaceFn, class OriginalFn, class... Args>
std::int32_t dispatch_queue(HookCounters& counters, bool enabled, ReplaceFn&& replace,
                            OriginalFn original, Args&&... args) noexcept
{
  if (!enabled)
    return original ? original(std::forward<Args>(args)...) : -19;
  counters.callback_seen.fetch_add(1, std::memory_order_relaxed);
  // A missing original target cannot submit a buffer. Avoid modifying it.
  if (!original)
  {
    counters.original_queue_failed.fetch_add(1, std::memory_order_relaxed);
    return -19; // -ENODEV
  }
  ReplacementOutcome outcome = ReplacementOutcome::AttemptedFailed;
  try
  {
    outcome = std::forward<ReplaceFn>(replace)();
  }
  catch (...)
  {
    // Camera service callbacks must never unwind across the platform ABI.
    // The original queue call still runs and owns the returned status.
  }
  if (outcome != ReplacementOutcome::PassThrough)
    counters.replacement_attempted.fetch_add(1, std::memory_order_relaxed);
  if (outcome == ReplacementOutcome::PassThrough ||
      outcome == ReplacementOutcome::AttemptedFailed)
    counters.pass_through.fetch_add(1, std::memory_order_relaxed);

  const std::int32_t status = original(std::forward<Args>(args)...);
  if (status != 0)
    counters.original_queue_failed.fetch_add(1, std::memory_order_relaxed);
  else if (outcome == ReplacementOutcome::YuvWritten ||
           outcome == ReplacementOutcome::RecordingYuvWritten)
  {
    counters.yuv_queued.fetch_add(1, std::memory_order_relaxed);
    if (outcome == ReplacementOutcome::RecordingYuvWritten)
      counters.recording_queued.fetch_add(1, std::memory_order_relaxed);
  }
  else if (outcome == ReplacementOutcome::JpegWritten)
    counters.jpeg_queued.fetch_add(1, std::memory_order_relaxed);
  return status;
}

} // namespace cr::camhook
