#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

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

constexpr int16_t fixed(double value) noexcept
{
  const double scaled = value * kUnit;
  return static_cast<int16_t>(scaled + (scaled < 0 ? -.5 : .5));
}

struct ChromaDelta
{
  int16_t y, u, v;
};

struct Remap
{
  int16_t luma_scale = kUnit;
  int luma_bias = 0;
  ChromaDelta blue{}, red{};
  uint8_t y_min = 0, y_max = 255, chroma_min = 0, chroma_max = 255;
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
  result.luma_scale = fixed(b.y_span / a.y_span);
  result.luma_bias = b.y_min * kUnit - a.y_min * result.luma_scale;
  result.y_min = static_cast<uint8_t>(b.y_min);
  result.y_max = static_cast<uint8_t>(b.y_min + b.y_span);
  result.chroma_min = result.y_min;
  result.chroma_max = b.y_min == 0 ? 255 : 240;
  result.blue = {
      fixed(out_y_from_u * b.y_span / a.chroma_span),
      fixed((b_from_u - out_y_from_u) * out_u_factor / a.chroma_span),
      fixed(-out_y_from_u * out_v_factor / a.chroma_span)};
  result.red = {
      fixed(out_y_from_v * b.y_span / a.chroma_span),
      fixed(-out_y_from_v * out_u_factor / a.chroma_span),
      fixed((r_from_v - out_y_from_v) * out_v_factor / a.chroma_span)};
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

#if defined(__aarch64__) && defined(__ARM_NEON)
inline uint8x8_t pack_vector(int32x4_t lo, int32x4_t hi, uint8_t low,
                             uint8_t high) noexcept
{
  const auto narrowed = vcombine_u16(vqrshrun_n_s32(lo, kShift), vqrshrun_n_s32(hi, kShift));
  return vmin_u8(vmax_u8(vqmovn_u16(narrowed), vdup_n_u8(low)), vdup_n_u8(high));
}

inline void remap_block(uint8_t* top, uint8_t* bottom, uint8_t* vu,
                        const Remap& map) noexcept
{
  const auto chroma = vld2_u8(vu);
  const auto center = vdupq_n_s16(128);
  const auto v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(chroma.val[0])), center);
  const auto u = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(chroma.val[1])), center);
  const auto mix_lo = [&](int16_t u_factor, int16_t v_factor, int32_t bias) {
    return vmlal_n_s16(vmlal_n_s16(vdupq_n_s32(bias), vget_low_s16(u), u_factor),
                      vget_low_s16(v), v_factor);
  };
  const auto mix_hi = [&](int16_t u_factor, int16_t v_factor, int32_t bias) {
    return vmlal_high_n_s16(vmlal_high_n_s16(vdupq_n_s32(bias), u, u_factor), v, v_factor);
  };
  const auto dy_lo = mix_lo(map.blue.y, map.red.y, map.luma_bias);
  const auto dy_hi = mix_hi(map.blue.y, map.red.y, map.luma_bias);
  const auto luma = [&](uint8x8_t samples) {
    const auto in = vreinterpretq_s16_u16(vmovl_u8(samples));
    return pack_vector(vmlal_n_s16(dy_lo, vget_low_s16(in), map.luma_scale),
                       vmlal_high_n_s16(dy_hi, in, map.luma_scale), map.y_min, map.y_max);
  };
  for (auto* row : {top, bottom})
  {
    const auto input = vld2_u8(row);
    const uint8x8x2_t output{{luma(input.val[0]), luma(input.val[1])}};
    vst2_u8(row, output);
  }
  const auto remap_chroma = [&](int16_t u_factor, int16_t v_factor) {
    return pack_vector(mix_lo(u_factor, v_factor, 128 * kUnit),
                       mix_hi(u_factor, v_factor, 128 * kUnit),
                       map.chroma_min, map.chroma_max);
  };
  const uint8x8x2_t output{{remap_chroma(map.blue.v, map.red.v),
                             remap_chroma(map.blue.u, map.red.u)}};
  vst2_u8(vu, output);
}
#endif

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
    int x = 0;
#if defined(__aarch64__) && defined(__ARM_NEON)
    for (; x <= width - 16; x += 16)
      remap_block(top + x, bottom + x, vu + x, map);
#endif
    for (; x < width; x += 2)
    {
      const int v = vu[x] - 128;
      const int u = vu[x + 1] - 128;
      const int dy = map.luma_bias + map.red.y * v + map.blue.y * u;
      top[x] = pack(map.luma_scale * top[x] + dy, map.y_min, map.y_max);
      top[x + 1] = pack(map.luma_scale * top[x + 1] + dy, map.y_min, map.y_max);
      bottom[x] = pack(map.luma_scale * bottom[x] + dy, map.y_min, map.y_max);
      bottom[x + 1] = pack(map.luma_scale * bottom[x + 1] + dy, map.y_min, map.y_max);
      vu[x] = pack(128 * kUnit + map.red.v * v + map.blue.v * u, map.chroma_min, map.chroma_max);
      vu[x + 1] = pack(128 * kUnit + map.red.u * v + map.blue.u * u, map.chroma_min, map.chroma_max);
    }
    vu += width;
  }
}

} // namespace cr::camhook
