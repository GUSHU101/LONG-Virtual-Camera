#include "TrackFreshness.h"

#include <cassert>

int main()
{
  using namespace std::chrono_literals;
  using Freshness = cr::feed::TrackFreshness;
  const Freshness::Clock::time_point start{};
  Freshness video, audio;
  assert(!audio.expired(0, start, 2s));
  assert(!audio.expired(0, start + 5s, 2s)); // no PCM has been published
  assert(!video.expired(1, start + 5s, 2s));
  assert(!audio.expired(1, start + 5s, 2s));
  assert(!video.expired(61, start + 7s, 2s));
  assert(audio.expired(1, start + 7s, 2s)); // video must not keep audio ready
  assert(!audio.expired(1, start + 10s, 2s)); // revoke once per stall
  assert(!audio.expired(2, start + 10s, 2s));
  assert(audio.expired(2, start + 12s, 2s));
}
