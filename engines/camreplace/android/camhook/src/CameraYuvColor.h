#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace cr::camhook
{

enum class YuvEncoding : uint8_t
{
  Bt601Limited, Bt709Limited, Bt601Full, Bt709Full
};

constexpr YuvEncoding sdr_yuv_encoding(bool bt709, bool full) noexcept
{
  return static_cast<YuvEncoding>((bt709 ? 1u : 0u) | (full ? 2u : 0u));
}

// Only correct streams with a reviewed SDR matrix, transfer and range.
// The bit layout is Android ADataSpace, available before the NDK constants.
constexpr std::optional<YuvEncoding> camera_sdr_yuv_encoding(uint32_t dataspace) noexcept
{
  const unsigned standard = (dataspace >> 16) & 63u;
  const unsigned transfer = (dataspace >> 22) & 31u;
  const unsigned range = (dataspace >> 27) & 7u;
  if (transfer != 3u || (range != 1u && range != 2u) ||
      (standard != 1u && standard != 2u && standard != 4u))
    return {};
  return sdr_yuv_encoding(standard == 1u, range == 1u);
}

namespace yuv_color_detail
{

struct Coefficients
{
  double r, b, y_span, chroma_span;
  int y_min;
};

constexpr Coefficients coefficients(unsigned encoding) noexcept
{
  const bool bt709 = (encoding & 1u) != 0;
  const bool full = (encoding & 2u) != 0;
  return {bt709 ? .2126 : .299, bt709 ? .0722 : .114,
          full ? 255. : 219., full ? 255. : 224., full ? 0 : 16};
}

inline constexpr int kShift = 12;
inline constexpr int kUnit = 1 << kShift;

constexpr int fixed(double value) noexcept
{
  const double scaled = value * kUnit;
  return static_cast<int>(scaled + (scaled < 0 ? -.5 : .5));
}

struct ChromaDelta
{
  int y, u, v;
};

struct Remap
{
  std::array<int, 256> luma{};
  std::array<ChromaDelta, 256> blue{}, red{};
  int y_min = 0, y_max = 255, chroma_min = 0, chroma_max = 255;
};

constexpr Remap make_remap(unsigned input, unsigned output) noexcept
{
  const auto a = coefficients(input);
  const auto b = coefficients(output);
  const double b_from_u = 2. * (1. - a.b);
  const double r_from_v = 2. * (1. - a.r);
  const double green_factor = (1. - b.r - b.b) / (1. - a.r - a.b);
  const double out_y_from_u = (b.b - a.b * green_factor) * b_from_u;
  const double out_y_from_v = (b.r - a.r * green_factor) * r_from_v;
  const double out_u_factor = b.chroma_span / (2. * (1. - b.b));
  const double out_v_factor = b.chroma_span / (2. * (1. - b.r));
  Remap result;
  result.y_min = b.y_min;
  result.y_max = b.y_min + static_cast<int>(b.y_span);
  result.chroma_min = b.y_min;
  result.chroma_max = b.y_min == 0 ? 255 : 240;
  for (int i = 0; i < 256; ++i)
  {
    const double chroma = (i - 128) / a.chroma_span;
    result.luma[i] = fixed(b.y_min + (i - a.y_min) * b.y_span / a.y_span);
    result.blue[i] = {
        fixed(chroma * out_y_from_u * b.y_span),
        fixed(chroma * (b_from_u - out_y_from_u) * out_u_factor),
        fixed(-chroma * out_y_from_u * out_v_factor)};
    result.red[i] = {
        fixed(chroma * out_y_from_v * b.y_span),
        fixed(-chroma * out_y_from_v * out_u_factor),
        fixed(chroma * (r_from_v - out_y_from_v) * out_v_factor)};
  }
  return result;
}

inline constexpr auto kRemaps = [] {
  std::array<Remap, 16> maps{};
  for (unsigned input = 0; input < 4; ++input)
    for (unsigned output = 0; output < 4; ++output)
      if (input != output)
        maps[input * 4 + output] = make_remap(input, output);
  return maps;
}();

inline uint8_t pack(int value, int low, int high) noexcept
{
  return static_cast<uint8_t>(std::clamp((value + kUnit / 2) >> kShift, low, high));
}

} // namespace yuv_color_detail

// Transform a tightly packed NV21 frame to the consumer's SDR encoding.
// Each VU pair supplies the same luma correction for its four Y samples.
inline void remap_nv21_color(uint8_t* image, int width, int height, YuvEncoding input,
                             YuvEncoding output) noexcept
{
  using namespace yuv_color_detail;
  if (input == output)
    return;
  const auto& map = kRemaps[static_cast<unsigned>(input) * 4 + static_cast<unsigned>(output)];
  auto* vu = image + static_cast<size_t>(width) * height;
  for (int y = 0; y < height; y += 2)
  {
    auto* top = image + static_cast<size_t>(y) * width;
    auto* bottom = top + width;
    for (int x = 0; x < width; x += 2)
    {
      const auto v = map.red[vu[x]];
      const auto u = map.blue[vu[x + 1]];
      const int dy = v.y + u.y;
      top[x] = pack(map.luma[top[x]] + dy, map.y_min, map.y_max);
      top[x + 1] = pack(map.luma[top[x + 1]] + dy, map.y_min, map.y_max);
      bottom[x] = pack(map.luma[bottom[x]] + dy, map.y_min, map.y_max);
      bottom[x + 1] = pack(map.luma[bottom[x + 1]] + dy, map.y_min, map.y_max);
      vu[x] = pack(128 * kUnit + v.v + u.v, map.chroma_min, map.chroma_max);
      vu[x + 1] = pack(128 * kUnit + v.u + u.u, map.chroma_min, map.chroma_max);
    }
    vu += width;
  }
}

} // namespace cr::camhook
