#pragma once

#include "nv21_blit.h"

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
// Convert limited-range NV21 to RGB and approximate the inverse display
// transfer curve before mapping to the sensor's 64..1023 code range.
inline bool nv21_to_bggr10(std::span<const uint8_t> nv21, int sw, int sh,
                           std::span<uint8_t> raw, int dw, int dh)
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

  const auto sample = [](int y, int adjustment) {
    const int rgb = std::clamp((298 * (y - 16) + adjustment + 128) >> 8, 0, 255);
    return 64u + (static_cast<unsigned>(rgb * rgb) * 959u + 32512u) / 65025u;
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
    for (int group = 0; group < dw / 4; ++group)
    {
      const int left = columns[group * 2];
      const int right = columns[group * 2 + 1];
      const int u0 = vu[left + 1] - 128, v0 = vu[left] - 128;
      const int u1 = vu[right + 1] - 128, v1 = vu[right] - 128;
      const int g0 = -100 * u0 - 208 * v0, g1 = -100 * u1 - 208 * v1;
      pack_raw10(even + group * 5,
                 sample(top[left], 516 * u0), sample(top[left + 1], g0),
                 sample(top[right], 516 * u1), sample(top[right + 1], g1));
      pack_raw10(odd + group * 5,
                 sample(bottom[left], g0), sample(bottom[left + 1], 409 * v0),
                 sample(bottom[right], g1), sample(bottom[right + 1], 409 * v1));
    }
  }
  return true;
}

} // namespace cr::camhook
