#include "media/FrameLayout.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

int main()
{
  cr::media::PlaneSpan window;
  assert(cr::media::valid_codec_window(4096, 16, 1024, window));
  assert(window.offset == 16 && window.bytes == 1024);
  assert(!cr::media::valid_codec_window(4096, -1, 10, window));
  assert(!cr::media::valid_codec_window(4096, 4000, 100, window));

  cr::media::Yuv420Layout layout;
  const std::size_t nv12_bytes = 640u * 480u * 3u / 2u;
  assert(cr::media::semiplanar_420(nv12_bytes, 640, 480, 640, 480, 640,
                                  layout));
  assert(!cr::media::semiplanar_420(nv12_bytes - 1, 640, 480, 640, 480,
                                   640, layout));
  assert(!cr::media::semiplanar_420(nv12_bytes, 641, 480, 641, 480, 641,
                                   layout));
  assert(!cr::media::semiplanar_420(nv12_bytes, 640, 480, 640, 481, 640,
                                   layout));

  const std::size_t i420_bytes = 640u * 480u + 2u * 320u * 240u;
  assert(cr::media::planar_420(i420_bytes, 640, 480, 640, 480, 320,
                              layout));
  assert(layout.planar);
  assert(!cr::media::planar_420(i420_bytes - 1, 640, 480, 640, 480, 320,
                               layout));
  assert(!cr::media::planar_420(i420_bytes, 640, 480, 640, 481, 320,
                               layout));

  std::size_t out = 0;
  assert(!cr::media::checked_mul(std::numeric_limits<std::size_t>::max(), 2,
                                out));
  assert(!cr::media::checked_add(std::numeric_limits<std::size_t>::max(), 1,
                                out));
  return 0;
}
