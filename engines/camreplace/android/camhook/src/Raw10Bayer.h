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

  std::vector<int> columns(dw / 2);
  for (int x = 0; x < dw / 2; ++x)
    columns[x] = crop.x + (x * crop.width / dw) * 2;

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
  int previous_row = -1;
  for (int y = 0; y < dh; y += 2)
  {
    const int sy = crop.y + (y * crop.height / dh & ~1);
    uint8_t* even = raw.data() + static_cast<size_t>(y) * row_bytes;
    uint8_t* odd = even + row_bytes;
    if (sy == previous_row)
    {
      memcpy(even, even - 2 * row_bytes, 2 * row_bytes);
      continue;
    }
    previous_row = sy;
    const uint8_t* top = nv21.data() + static_cast<size_t>(sy) * sw;
    const uint8_t* bottom = top + sw;
    const uint8_t* vu = nv21.data() + luma + static_cast<size_t>(sy / 2) * sw;
    if (dw * 2 < crop.width * 3)
    {
      // There are few or no repeated columns in this geometry.
      for (int group = 0; group < dw / 4; ++group)
      {
        const int left = columns[group * 2], right = columns[group * 2 + 1];
        const int u0 = vu[left + 1] - 128, v0 = vu[left] - 128;
        const int u1 = vu[right + 1] - 128, v1 = vu[right] - 128;
        pack_raw10(even + group * 5,
                   sample(top[left], u0, v0, color.even, 2), sample(top[left + 1], u0, v0, color.even, 1),
                   sample(top[right], u1, v1, color.even, 2), sample(top[right + 1], u1, v1, color.even, 1));
        pack_raw10(odd + group * 5,
                   sample(bottom[left], u0, v0, color.odd, 1), sample(bottom[left + 1], u0, v0, color.odd, 0),
                   sample(bottom[right], u1, v1, color.odd, 1), sample(bottom[right + 1], u1, v1, color.odd, 0));
      }
      continue;
    }

    // A substantial upscale repeats source 2x2 blocks across adjacent
    // Bayer cells. Keep both calibrated green values in the cached cell.
    int previous_column = -1;
    std::array<unsigned, 4> previous_cell{};
    const auto cell = [&](int column) {
      if (column != previous_column)
      {
        const int v = vu[column] - 128, u = vu[column + 1] - 128;
        previous_cell = {
            sample(top[column], u, v, color.even, 2),
            sample(top[column + 1], u, v, color.even, 1),
            sample(bottom[column], u, v, color.odd, 1),
            sample(bottom[column + 1], u, v, color.odd, 0)};
        previous_column = column;
      }
      return previous_cell;
    };
    for (int group = 0; group < dw / 4; ++group)
    {
      const auto left = cell(columns[group * 2]);
      const auto right = cell(columns[group * 2 + 1]);
      pack_raw10(even + group * 5, left[0], left[1], right[0], right[1]);
      pack_raw10(odd + group * 5, left[2], left[3], right[2], right[3]);
    }
  }
  return true;
}

} // namespace cr::camhook
