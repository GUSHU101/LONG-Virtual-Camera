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
  Succeeded,
};

struct HookCounters
{
  std::atomic<std::uint64_t> callback_seen{0};
  std::atomic<std::uint64_t> replacement_attempted{0};
  std::atomic<std::uint64_t> replacement_succeeded{0};
  std::atomic<std::uint64_t> pass_through{0};
  std::atomic<std::uint64_t> original_queue_failed{0};
};

// The original queue operation is the authority for the callback result. The
// replacement side effect runs first, but may never suppress, duplicate or
// rewrite the original status_t value.
template <class ReplaceFn, class OriginalFn, class... Args>
std::int32_t dispatch_queue(HookCounters& counters, ReplaceFn&& replace,
                            OriginalFn original, Args&&... args) noexcept
{
  counters.callback_seen.fetch_add(1, std::memory_order_relaxed);
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
  if (outcome == ReplacementOutcome::Succeeded)
    counters.replacement_succeeded.fetch_add(1, std::memory_order_relaxed);
  else
    counters.pass_through.fetch_add(1, std::memory_order_relaxed);

  // A successfully installed hook always has an original target. Keep a
  // deterministic fail-closed value for defensive unit testing/corruption.
  if (!original)
  {
    counters.original_queue_failed.fetch_add(1, std::memory_order_relaxed);
    return -19; // -ENODEV without depending on platform errno headers.
  }

  const std::int32_t status = original(std::forward<Args>(args)...);
  if (status != 0)
    counters.original_queue_failed.fetch_add(1, std::memory_order_relaxed);
  return status;
}

} // namespace cr::camhook
