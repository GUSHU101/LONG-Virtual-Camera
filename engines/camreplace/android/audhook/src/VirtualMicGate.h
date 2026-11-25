#pragma once

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/stat.h>

namespace cr::audhook
{

// The control marker belongs to the user-selected mode, not the feeder.
// A missing marker means OFF. An unexpected access error while the hook
// is resident must not expose the microphone.
inline bool prepare_virtual_mic_read(const char* marker, void* buffer,
                                     std::size_t bytes) noexcept
{
  struct stat mode{};
  if (::lstat(marker, &mode) != 0 && errno == ENOENT)
    return false;
  std::memset(buffer, 0, bytes);
  return true;
}

} // namespace cr::audhook
