#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cr::media
{

// Geometry policy shared by phone-local MediaCodec playback and OBS ingest.
// It follows ReCam's fit_frame contract, with an additional pixel budget so
// landscape, portrait and square sources receive the same memory bound.
// The source is never enlarged and the returned 4:2:0 geometry is even.
inline bool fit_even_frame(std::int32_t source_width,
                           std::int32_t source_height,
                           std::int32_t max_width,
                           std::int32_t max_height,
                           std::uint64_t max_pixels,
                           std::int32_t& output_width,
                           std::int32_t& output_height) noexcept
{
  output_width = 0;
  output_height = 0;
  if (source_width < 2 || source_height < 2 || source_width > 8192 ||
      source_height > 8192 || (source_width & 1) || (source_height & 1) ||
      max_width < 2 || max_height < 2 || max_pixels < 4 ||
      static_cast<std::uint64_t>(source_width) * source_height >
          32ull * 1024ull * 1024ull)
    return false;

  long double scale = 1.0L;
  scale = std::min(scale, static_cast<long double>(max_width) / source_width);
  scale = std::min(scale, static_cast<long double>(max_height) / source_height);
  const auto source_pixels =
      static_cast<long double>(source_width) * source_height;
  if (source_pixels > static_cast<long double>(max_pixels))
    scale = std::min(scale, std::sqrt(
        static_cast<long double>(max_pixels) / source_pixels));

  output_width =
      std::min(source_width, static_cast<std::int32_t>(source_width * scale)) &
      ~1;
  output_height =
      std::min(source_height,
               static_cast<std::int32_t>(source_height * scale)) &
      ~1;

  // Floating-point rounding at an exact boundary must not escape the hard
  // budget. Shrink by one 4:2:0 sample pair while preserving the source ratio.
  while (output_width >= 2 && output_height >= 2 &&
         static_cast<std::uint64_t>(output_width) * output_height > max_pixels)
  {
    if (static_cast<std::int64_t>(output_width) * source_height >=
        static_cast<std::int64_t>(output_height) * source_width)
      output_width -= 2;
    else
      output_height -= 2;
  }
  return output_width >= 2 && output_height >= 2 &&
         output_width <= max_width && output_height <= max_height &&
         static_cast<std::uint64_t>(output_width) * output_height <= max_pixels;
}

inline bool fit_oriented_even_frame(std::int32_t source_width,
                                    std::int32_t source_height,
                                    std::int32_t rotation,
                                    std::int32_t max_width,
                                    std::int32_t max_height,
                                    std::uint64_t max_pixels,
                                    std::int32_t& output_width,
                                    std::int32_t& output_height) noexcept
{
  if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
  {
    output_width = output_height = 0;
    return false;
  }
  if (rotation == 90 || rotation == 270)
    std::swap(source_width, source_height);
  return fit_even_frame(source_width, source_height, max_width, max_height,
                        max_pixels, output_width, output_height);
}

} // namespace cr::media
