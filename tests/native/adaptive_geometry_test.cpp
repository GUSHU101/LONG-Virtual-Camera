#include "media/AdaptiveGeometry.h"

#include <cassert>
#include <cstdint>

namespace
{
constexpr std::int32_t kMaxDimension = 1920;
constexpr std::uint64_t kMaxPixels = 1920ull * 1080ull;

void fit(std::int32_t source_width, std::int32_t source_height,
         std::int32_t expected_width, std::int32_t expected_height)
{
  std::int32_t width = 0;
  std::int32_t height = 0;
  assert(cr::media::fit_even_frame(source_width, source_height, kMaxDimension,
                                   kMaxDimension, kMaxPixels, width, height));
  assert(width == expected_width && height == expected_height);
  assert((width & 1) == 0 && (height & 1) == 0);
  assert(static_cast<std::uint64_t>(width) * height <= kMaxPixels);
}
} // namespace

int main()
{
  fit(1280, 720, 1280, 720);       // Never upscale a small source.
  fit(3840, 2160, 1920, 1080);     // 4K landscape -> shared-frame cap.
  fit(2160, 3840, 1080, 1920);     // Portrait receives the same pixel budget.
  fit(3840, 1600, 1920, 800);      // Ultrawide aspect is preserved.
  fit(2048, 2048, 1440, 1440);     // Square is pixel-bound, not stretched.

  std::int32_t width = 0;
  std::int32_t height = 0;
  assert(cr::media::fit_oriented_even_frame(
      1920, 1080, 90, kMaxDimension, kMaxDimension, kMaxPixels, width,
      height));
  assert(width == 1080 && height == 1920);
  assert(!cr::media::fit_even_frame(1919, 1080, kMaxDimension, kMaxDimension,
                                    kMaxPixels, width, height));
  assert(!cr::media::fit_oriented_even_frame(
      1920, 1080, 45, kMaxDimension, kMaxDimension, kMaxPixels, width,
      height));
  assert(!cr::media::fit_even_frame(8192, 8192, kMaxDimension, kMaxDimension,
                                    kMaxPixels, width, height));
  return 0;
}
