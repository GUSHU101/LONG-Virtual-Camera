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
  assert(!cr::media::valid_codec_window(4096, 0, INT64_C(4294967296), window));
  assert(!cr::media::valid_codec_window(4096, INT64_C(4294967296), 0, window));

  cr::media::Yuv420Crop crop;
  assert(cr::media::padded_yuv420_crop(672, 512, 16, 8, 655, 487,
                                       672u * 512u * 3u / 2u, crop));
  assert(crop.width == 640 && crop.height == 480);
  assert(crop.storage_bytes == 672u * 512u * 3u / 2u);
  assert(!cr::media::padded_yuv420_crop(672, 512, INT32_MIN, 0,
                                        INT32_MAX, 479, 4096, crop));
  assert(!cr::media::padded_yuv420_crop(672, 512, 0, 0, 672, 479, 4096, crop));
  assert(!cr::media::padded_yuv420_crop(640, 480, 1, 0, 638, 479, 4096, crop));
  assert(!cr::media::padded_yuv420_crop(INT32_MAX - 1, INT32_MAX - 1,
                                        0, 0, 639, 479, 96u << 20, crop));
  assert(crop.storage_bytes == 0);

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
