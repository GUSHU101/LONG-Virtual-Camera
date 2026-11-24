#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace cr::media
{

// Exact rational clock for a streaming PCM resampler. Keeping the remainder
// between callbacks prevents cumulative sample loss when rates differ.
struct PcmResamplePlan
{
  std::uint64_t source_frames_required = 0;
  std::uint64_t source_frames_consumed = 0;
  std::uint64_t phase_after = 0;
};

class PcmResampleClock
{
public:
  void reset() noexcept
  {
    source_rate_ = 0;
    destination_rate_ = 0;
    phase_ = 0;
  }

  [[nodiscard]] std::uint64_t phase() const noexcept { return phase_; }

  [[nodiscard]] bool plan(std::size_t destination_frames,
                          std::uint32_t source_rate,
                          std::uint32_t destination_rate,
                          PcmResamplePlan& out) noexcept
  {
    out = {};
    if (destination_frames == 0 || source_rate == 0 || destination_rate == 0)
      return false;

    const bool format_changed = source_rate_ != source_rate || destination_rate_ != destination_rate;
    const std::uint64_t base_phase = format_changed ? 0u : phase_;

    const auto frames = static_cast<std::uint64_t>(destination_frames);
    if (frames > (std::numeric_limits<std::uint64_t>::max() - base_phase) / source_rate)
      return false;

    const std::uint64_t end = base_phase + frames * source_rate;
    const std::uint64_t last = base_phase + (frames - 1u) * source_rate;
    const std::uint64_t last_index = last / destination_rate;
    if (last_index > std::numeric_limits<std::uint64_t>::max() - 2u)
      return false;

    // Linear interpolation reads both source[index] and source[index + 1].
    out.source_frames_required = last_index + 2u;
    out.source_frames_consumed = end / destination_rate;
    out.phase_after = end % destination_rate;
    if (format_changed)
    {
      source_rate_ = source_rate;
      destination_rate_ = destination_rate;
      phase_ = 0;
    }
    return true;
  }

  void commit(const PcmResamplePlan& plan) noexcept { phase_ = plan.phase_after; }

private:
  std::uint32_t source_rate_ = 0;
  std::uint32_t destination_rate_ = 0;
  std::uint64_t phase_ = 0;
};

} // namespace cr::media
