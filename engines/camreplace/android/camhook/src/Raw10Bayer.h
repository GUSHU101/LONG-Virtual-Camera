#pragma once

#include "nv21_blit.h"
#include "Raw10Color.h"
#include "YuvColor.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace cr::camhook
{

// Standard MIPI RAW10, four high bytes followed by four two-bit tails.
inline void pack_raw10(uint8_t* out, unsigned a, unsigned b, unsigned c, unsigned d)
{
  out[0] = a >> 2;
  out[1] = b >> 2;
  out[2] = c >> 2;
  out[3] = d >> 2;
  out[4] = (a & 3) | ((b & 3) << 2) | ((c & 3) << 4) | ((d & 3) << 6);
}

// Construct a synthetic BGGR mosaic for the Pixel 5 sensor profile.
// Decode SDR NV21 into linear RGB and convert it to the sensor's color
// space before packing the BGGR samples at code levels 64..1023.
inline bool nv21_to_bggr10(std::span<const uint8_t> nv21, int sw, int sh,
                           std::span<uint8_t> raw, int dw, int dh,
                           const Raw10Color& color = {}, bool bt709 = false,
                           bool full_range = false)
{
  cr_nv21_crop_rect crop;
  if (sw > 8192 || sh > 8192 || dw > 4656 || dh > 3496 ||
      (dw & 3) != 0 || !cr_nv21_center_crop(sw, sh, dw, dh, &crop))
    return false;
  const size_t luma = static_cast<size_t>(sw) * sh;
  const size_t row_bytes = static_cast<size_t>(dw / 4) * 5;
  if (nv21.size() < luma + luma / 2 || raw.size() < row_bytes * dh)
    return false;

  const auto source_coordinate = [](int position, int source, int dest) {
    return ((2 * position + 1) * source) / (2 * dest);
  };
  std::vector<int> columns(dw);
  for (int x = 0; x < dw; ++x)
    columns[x] = crop.x + source_coordinate(x, crop.width, dw);

  static const auto linear = [] {
    std::array<int32_t, 256> table{};
    for (size_t index = 0; index < table.size(); ++index)
    {
      const float x = static_cast<float>(index) / 255.f;
      const float value = x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
      table[index] = std::lround(value * 959.f);
    }
    return table;
  }();
  const auto yuv = cr_yuv_coefficients_for(bt709, full_range);
  const auto sample = [&](int y, int u, int v, const Raw10Color::Matrix& matrix, int row) {
    const int scaled_y = yuv.y_scale * (y - yuv.y_offset);
    const auto decode = [&](int offset) {
      return linear[std::clamp((scaled_y + offset + 128) >> 8, 0, 255)];
    };
    const auto* coefficients = matrix.data() + 3 * row;
    const int sensor = coefficients[0] * decode(yuv.red_v * v) +
                       coefficients[1] * decode(-yuv.green_u * u - yuv.green_v * v) +
                       coefficients[2] * decode(yuv.blue_u * u);
    return 64u + static_cast<unsigned>(std::clamp((sensor + Raw10Color::scale / 2) >> 12, 0, 959));
  };
  // Sample luma at each sensor pixel center. Repeating an entire source
  // Bayer cell introduces false fine stripes when its luma values differ.
  std::array<int, 2> previous_row{-1, -1};
  for (int y = 0; y < dh; ++y)
  {
    const int phase = y & 1;
    const int sy = crop.y + source_coordinate(y, crop.height, dh);
    uint8_t* output = raw.data() + static_cast<size_t>(y) * row_bytes;
    if (sy == previous_row[phase])
    {
      memcpy(output, output - 2 * row_bytes, row_bytes);
      continue;
    }
    previous_row[phase] = sy;
    const uint8_t* luma_row = nv21.data() + static_cast<size_t>(sy) * sw;
    const uint8_t* vu = nv21.data() + luma + static_cast<size_t>(sy / 2) * sw;
    const auto& matrix = phase ? color.odd : color.even;
    std::array<int, 2> previous_column{-1, -1};
    std::array<unsigned, 2> previous_sample{};
    const auto pixel = [&](int x, int column_phase) {
      const int sx = columns[x];
      if (sx != previous_column[column_phase])
      {
        const int cx = sx & ~1;
        previous_sample[column_phase] =
            sample(luma_row[sx], vu[cx + 1] - 128, vu[cx] - 128,
                   matrix, 2 - phase - column_phase);
        previous_column[column_phase] = sx;
      }
      return previous_sample[column_phase];
    };
    for (int x = 0; x < dw; x += 4)
    {
      const auto a = pixel(x, 0);
      const auto b = pixel(x + 1, 1);
      const auto c = pixel(x + 2, 0);
      const auto d = pixel(x + 3, 1);
      pack_raw10(output + (x / 4) * 5, a, b, c, d);
    }
  }
  return true;
}

} // namespace cr::camhook
