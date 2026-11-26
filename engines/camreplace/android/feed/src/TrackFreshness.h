#pragma once

#include <chrono>
#include <cstdint>

namespace cr::feed
{

// One notification per stall. Count successful shared-buffer publications
// so transport traffic and packets discarded by a decoder cannot keep a
// silent or frozen track marked ready.
class TrackFreshness
{
public:
  using Clock = std::chrono::steady_clock;

  bool expired(uint64_t publications, Clock::time_point now, Clock::duration timeout) noexcept
  {
    if (publications != observed_)
    {
      observed_ = publications;
      last_progress_ = now;
      active_ = publications != 0;
    }
    if (!active_ || now - last_progress_ < timeout)
      return false;
    active_ = false;
    return true;
  }

private:
  uint64_t observed_ = 0;
  Clock::time_point last_progress_{};
  bool active_ = false;
};

} // namespace cr::feed
