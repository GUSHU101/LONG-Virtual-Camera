#include "TrackFreshness.h"

#include <cassert>

static void verify_playback_freshness()
{
  using namespace std::chrono_literals;
  using Playback = cr::feed::RtmpPlaybackFreshness;
  const Playback::Clock::time_point start{};
  Playback audiovisual(true);
  auto progress = audiovisual.observe(0, 1, start);
  assert(!progress.primary_started);
  progress = audiovisual.observe(1, 10, start + 1s);
  assert(progress.primary_started && !progress.reconnect);
  progress = audiovisual.observe(1, 40, start + 3s);
  assert(progress.video_stalled && !progress.audio_stalled && !progress.reconnect);
  progress = audiovisual.observe(1, 90, start + 6s);
  assert(!progress.video_stalled && !progress.reconnect);
  assert(audiovisual.deadline() == start + 7s);
  progress = audiovisual.observe(1, 100, start + 7s);
  assert(progress.reconnect); // Continuing PCM must not keep frozen video connected.

  Playback recovered(true);
  recovered.observe(1, 1, start);
  progress = recovered.observe(1, 1, start + 2s);
  assert(progress.video_stalled && progress.audio_stalled);
  progress = recovered.observe(2, 1, start + 5s);
  assert(progress.primary_started && !progress.reconnect);
  progress = recovered.observe(2, 1, start + 6s);
  assert(!progress.reconnect);

  Playback audio_only(false);
  progress = audio_only.observe(0, 1, start);
  assert(progress.primary_started);
  progress = audio_only.observe(0, 1, start + 2s);
  assert(progress.audio_stalled && !progress.video_stalled && !progress.reconnect);
  progress = audio_only.observe(0, 1, start + 6s);
  assert(progress.reconnect);
}

int main()
{
  using namespace std::chrono_literals;
  using Freshness = cr::feed::TrackFreshness;
  const Freshness::Clock::time_point start{};
  Freshness video, audio;
  assert(video.deadline(2s) == Freshness::Clock::time_point::max());
  assert(!audio.expired(0, start, 2s));
  assert(!audio.expired(0, start + 5s, 2s)); // no PCM has been published
  assert(!video.expired(1, start + 5s, 2s));
  assert(video.deadline(2s) == start + 7s);
  assert(!audio.expired(1, start + 5s, 2s));
  assert(!video.expired(61, start + 7s, 2s));
  assert(audio.expired(1, start + 7s, 2s)); // video must not keep audio ready
  assert(audio.deadline(2s) == Freshness::Clock::time_point::max());
  assert(!audio.expired(1, start + 10s, 2s)); // revoke once per stall
  assert(!audio.expired(2, start + 10s, 2s));
  assert(audio.expired(2, start + 12s, 2s));
  verify_playback_freshness();
}
