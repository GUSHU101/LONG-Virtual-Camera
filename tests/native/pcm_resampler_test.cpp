#include "media/PcmResampler.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

using cr::media::PcmResampleClock;
using cr::media::PcmResamplePlan;

int main()
{
  PcmResampleClock clock;
  PcmResamplePlan plan{};

  assert(!clock.plan(0, 44100, 48000, plan));
  assert(!clock.plan(480, 0, 48000, plan));

  assert(clock.plan(480, 44100, 48000, plan));
  assert(plan.source_frames_required == 442);
  assert(plan.source_frames_consumed == 441);
  assert(plan.phase_after == 0);
  clock.commit(plan);

  // One hour with uneven callback sizes must have zero long-term drift.
  clock.reset();
  constexpr std::uint64_t destination_total = 48000ull * 60ull * 60ull;
  constexpr std::uint64_t expected_source = 44100ull * 60ull * 60ull;
  constexpr std::array<std::size_t, 7> chunks{127, 480, 511, 960, 193, 1024, 73};
  std::uint64_t produced = 0;
  std::uint64_t consumed = 0;
  std::size_t index = 0;
  while (produced < destination_total)
  {
    std::size_t chunk = chunks[index++ % chunks.size()];
    if (chunk > destination_total - produced)
      chunk = static_cast<std::size_t>(destination_total - produced);
    assert(clock.plan(chunk, 44100, 48000, plan));
    assert(plan.source_frames_required >= plan.source_frames_consumed);
    consumed += plan.source_frames_consumed;
    produced += chunk;
    clock.commit(plan);
  }
  assert(consumed == expected_source);
  assert(clock.phase() == 0);

  // A format change resets the fractional phase.
  assert(clock.plan(1, 32000, 48000, plan));
  assert(plan.source_frames_consumed == 0);
  assert(plan.phase_after == 32000);

  clock.commit(plan);
  const auto saved_phase = clock.phase();
  assert(!clock.plan(std::numeric_limits<std::size_t>::max(), 192000, 8000, plan));
  assert(clock.phase() == saved_phase);
  return 0;
}
