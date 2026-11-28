#pragma once

#include <algorithm>
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

  Clock::time_point deadline(Clock::duration timeout) const noexcept
  {
    return active_ ? last_progress_ + timeout : Clock::time_point::max();
  }

  bool expired(uint64_t publications, Clock::time_point now, Clock::duration timeout) noexcept
  {
    if (publications != observed_)
    {
      observed_ = publications;
      last_progress_ = now;
      active_ = publications != 0;
    }
    if (!active_ || now < deadline(timeout))
      return false;
    active_ = false;
    return true;
  }

private:
  uint64_t observed_ = 0;
  Clock::time_point last_progress_{};
  bool active_ = false;
};

class RtmpPlaybackFreshness
{
public:
  using Clock = TrackFreshness::Clock;

  struct Progress
  {
    bool primary_started;
    bool video_stalled;
    bool audio_stalled;
    bool reconnect;
  };

  explicit RtmpPlaybackFreshness(bool video_required) noexcept
      : video_required_(video_required) {}

  Progress observe(uint64_t frames, uint64_t pcm_blocks, Clock::time_point now) noexcept
  {
    // An optional microphone must not mask a stalled camera. Audio-only
    // playback uses PCM as its primary output.
    const auto primary = video_required_ ? frames : pcm_blocks;
    return {primary != 0,
            video_.expired(frames, now, kTrackTimeout),
            audio_.expired(pcm_blocks, now, kTrackTimeout),
            primary_.expired(primary, now, kReconnectTimeout)};
  }

  Clock::time_point deadline() const noexcept
  {
    return std::min({video_.deadline(kTrackTimeout), audio_.deadline(kTrackTimeout),
                     primary_.deadline(kReconnectTimeout)});
  }

private:
  static constexpr auto kTrackTimeout = std::chrono::seconds(2);
  static constexpr auto kReconnectTimeout = std::chrono::seconds(6);
  TrackFreshness video_, audio_, primary_;
  bool video_required_;
};

} // namespace cr::feed
