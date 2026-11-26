#include "HookDispatch.h"

#include <cassert>
#include <cstdint>

namespace
{
int calls = 0;

std::int32_t original_ok(int value)
{
  ++calls;
  return value;
}
} // namespace

int main()
{
  cr::camhook::HookCounters counters;

  calls = 0;
  const auto ok = cr::camhook::dispatch_queue(
      counters, true,
      [] { return cr::camhook::ReplacementOutcome::YuvWritten; },
      original_ok, 0);
  assert(ok == 0 && calls == 1);

  calls = 0;
  const auto bad_value = cr::camhook::dispatch_queue(
      counters, true,
      [] { return cr::camhook::ReplacementOutcome::PassThrough; },
      original_ok, -22);
  assert(bad_value == -22 && calls == 1);

  calls = 0;
  const auto no_memory = cr::camhook::dispatch_queue(
      counters, true,
      [] { return cr::camhook::ReplacementOutcome::AttemptedFailed; },
      original_ok, -12);
  assert(no_memory == -12 && calls == 1);

  assert(counters.callback_seen.load() == 3);
  assert(counters.replacement_attempted.load() == 2);
  assert(counters.yuv_queued.load() == 1);
  assert(counters.pass_through.load() == 2);
  assert(counters.original_queue_failed.load() == 2);

  const auto refused = cr::camhook::dispatch_queue(
      counters, true,
      [] { return cr::camhook::ReplacementOutcome::YuvWritten; },
      original_ok, -32);
  assert(refused == -32);
  assert(counters.yuv_queued.load() == 1);
  assert(counters.original_queue_failed.load() == 3);

  const auto jpeg = cr::camhook::dispatch_queue(
      counters, true,
      [] { return cr::camhook::ReplacementOutcome::JpegWritten; },
      original_ok, 0);
  assert(jpeg == 0 && counters.jpeg_queued.load() == 1);

  const auto record = [] { return cr::camhook::ReplacementOutcome::RecordingYuvWritten; };
  assert(cr::camhook::dispatch_queue(counters, true, record, original_ok, 0) == 0);
  assert(cr::camhook::dispatch_queue(counters, true, record, original_ok, -32) == -32);
  assert(counters.yuv_queued.load() == 2 && counters.recording_queued.load() == 1);

  const auto raw = [] { return cr::camhook::ReplacementOutcome::Raw10Written; };
  assert(cr::camhook::dispatch_queue(counters, true, raw, original_ok, 0) == 0);
  assert(cr::camhook::dispatch_queue(counters, true, raw, original_ok, -32) == -32);
  assert(counters.raw10_queued.load() == 1 && counters.jpeg_queued.load() == 1);

  using Original = std::int32_t (*)(int);
  const auto missing = cr::camhook::dispatch_queue(
      counters, true,
      [] {
        assert(false && "a buffer must not be written without a queue target");
        return cr::camhook::ReplacementOutcome::JpegWritten;
      },
      static_cast<Original>(nullptr), 0);
  assert(missing == -19);
  const auto recorded_callbacks = counters.callback_seen.load();
  calls = 0;
  const auto paused = cr::camhook::dispatch_queue(
      counters, false,
      [] {
        assert(false && "a paused camera must not touch source or output buffers");
        return cr::camhook::ReplacementOutcome::YuvWritten;
      },
      original_ok, -21);
  assert(paused == -21 && calls == 1);
  assert(counters.callback_seen.load() == recorded_callbacks);
  return 0;
}
