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
      counters,
      [] { return cr::camhook::ReplacementOutcome::Succeeded; },
      original_ok, 0);
  assert(ok == 0 && calls == 1);

  calls = 0;
  const auto bad_value = cr::camhook::dispatch_queue(
      counters,
      [] { return cr::camhook::ReplacementOutcome::PassThrough; },
      original_ok, -22);
  assert(bad_value == -22 && calls == 1);

  calls = 0;
  const auto no_memory = cr::camhook::dispatch_queue(
      counters,
      [] { return cr::camhook::ReplacementOutcome::AttemptedFailed; },
      original_ok, -12);
  assert(no_memory == -12 && calls == 1);

  assert(counters.callback_seen.load() == 3);
  assert(counters.replacement_attempted.load() == 2);
  assert(counters.replacement_succeeded.load() == 1);
  assert(counters.pass_through.load() == 2);
  assert(counters.original_queue_failed.load() == 2);

  using Original = std::int32_t (*)(int);
  const auto missing = cr::camhook::dispatch_queue(
      counters,
      [] { return cr::camhook::ReplacementOutcome::PassThrough; },
      static_cast<Original>(nullptr), 0);
  assert(missing == -19);
  return 0;
}
