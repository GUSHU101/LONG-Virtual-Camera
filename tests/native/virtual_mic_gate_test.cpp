#include "VirtualMicGate.h"

#include <array>
#include <cassert>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

int main()
{
#if defined(__ANDROID__)
  char directory[] = "/data/local/tmp/longvc-mic-XXXXXX";
#else
  char directory[] = "/tmp/longvc-mic-XXXXXX";
#endif
  assert(::mkdtemp(directory));
  const std::string marker = std::string(directory) + "/mode";
  const int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0400);
  assert(fd >= 0);
  assert(::close(fd) == 0);
  std::array<unsigned char, 8> audio{};
  audio.fill(0x5a);
  assert(cr::audhook::prepare_virtual_mic_read(marker.c_str(), audio.data(), audio.size()));
  assert(audio.front() == 0 && audio.back() == 0);
  audio.fill(0x5a);
  const std::string invalid_path = marker + "/unexpected";
  assert(cr::audhook::prepare_virtual_mic_read(invalid_path.c_str(), audio.data(), audio.size()));
  assert(audio.front() == 0 && audio.back() == 0);
  assert(::unlink(marker.c_str()) == 0);
  audio.fill(0x5a);
  assert(!cr::audhook::prepare_virtual_mic_read(marker.c_str(), audio.data(), audio.size()));
  assert(audio.front() == 0x5a && audio.back() == 0x5a);
  assert(::rmdir(directory) == 0);
}
