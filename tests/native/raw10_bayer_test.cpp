#include "Raw10Bayer.h"
#include "CaptureZoom.h"
#include "CameraYuvColor.h"

#include <array>
#include <cassert>

int main()
{
  using cr::camhook::YuvEncoding;
  constexpr auto jfif = cr::camhook::camera_sdr_yuv_encoding(0x08c20000);
  constexpr auto limited709 = cr::camhook::camera_sdr_yuv_encoding(0x10c10000);
  static_assert(jfif == YuvEncoding::Bt601Full);
  static_assert(limited709 == YuvEncoding::Bt709Limited);
  static_assert(!cr::camhook::camera_sdr_yuv_encoding(0));
  static_assert(!cr::camhook::camera_sdr_yuv_encoding(0x11c60000));
  std::array<uint8_t, 12> sdr{16, 235, 63, 63, 16, 235, 63, 63, 128, 128, 240, 102};
  const auto original_sdr = sdr;
  cr::camhook::remap_nv21_color(sdr.data(), 4, 2, *limited709, *jfif);
  assert(sdr[0] == 0 && sdr[1] == 255 && sdr[4] == 0 && sdr[5] == 255);
  assert(sdr[8] == 128 && sdr[9] == 128);
  assert(std::abs(sdr[2] - 76) <= 1 && std::abs(sdr[10] - 255) <= 1 &&
         std::abs(sdr[11] - 85) <= 1);
  cr::camhook::remap_nv21_color(sdr.data(), 4, 2, *jfif, *limited709);
  for (size_t i = 0; i < sdr.size(); ++i)
    assert(std::abs(sdr[i] - original_sdr[i]) <= 1);

  // A width with both full ARM blocks and a tail must match independently
  // transforming each 2x2 cell through the scalar path.
  constexpr int width = 34;
  std::array<uint8_t, width * 3> varied{};
  for (size_t i = 0; i < varied.size(); ++i)
    varied[i] = static_cast<uint8_t>((i * 79 + i / 3) & 255);
  for (unsigned input = 0; input < 4; ++input)
    for (unsigned output = 0; output < 4; ++output)
    {
      const auto src = static_cast<YuvEncoding>(input);
      const auto dst = static_cast<YuvEncoding>(output);
      auto whole = varied;
      cr::camhook::remap_nv21_color(whole.data(), width, 2, src, dst);
      for (int x = 0; x < width; x += 2)
      {
        const std::array<int, 6> offsets{x, x + 1, width + x, width + x + 1,
                                          2 * width + x, 2 * width + x + 1};
        std::array<uint8_t, 6> cell{};
        for (size_t i = 0; i < cell.size(); ++i)
          cell[i] = varied[offsets[i]];
        cr::camhook::remap_nv21_color(cell.data(), 2, 2, src, dst);
        for (size_t i = 0; i < cell.size(); ++i)
          assert(whole[offsets[i]] == cell[i]);
      }
    }

  assert(cr::camhook::capture_zoom_q10(1.f, 2.57f, {469, 353, 2342, 1758}) == 1434);
  assert(cr::camhook::capture_zoom_q10(1.f, 2.57f, {0, 2, 3280, 2460}) == CR_NV21_ZOOM_ONE);
  assert(cr::camhook::capture_zoom_q10(1.f, 2.57f, {0, 309, 3280, 1846}) == CR_NV21_ZOOM_ONE);
  assert(cr::camhook::capture_zoom_q10(1.f, 0.f, {469, 353, 2342, 1758}) == CR_NV21_ZOOM_ONE);
  assert(cr::camhook::capture_zoom_q10(.6f, 2.22f, {469, 353, 2342, 1758}) == CR_NV21_ZOOM_ONE);
  assert(cr::camhook::capture_zoom_q10(1.4f, 2.57f, {469, 353, 2342, 1758}) == 1434);

  std::array<uint8_t, 5> group{};
  cr::camhook::pack_raw10(group.data(), 0x3ff, 0x200, 0x001, 0x002);
  assert((group == std::array<uint8_t, 5>{255, 128, 0, 0, 0x93}));

  // A neutral black/white source maps to the calibrated black and white
  // levels. Each pair of rows keeps the same 2x2 Bayer phase.
  const std::array<uint8_t, 12> neutral{
      16, 16, 235, 235, 16, 16, 235, 235, 128, 128, 128, 128};
  std::array<uint8_t, 10> raw{};
  assert(cr::camhook::nv21_to_bggr10(neutral, 4, 2, raw, 4, 2));
  assert((raw == std::array<uint8_t, 10>{16, 16, 255, 255, 0xf0,
                                        16, 16, 255, 255, 0xf0}));
  std::array<uint8_t, 40> upscaled_raw{};
  assert(cr::camhook::nv21_to_bggr10(neutral, 4, 2, upscaled_raw, 8, 4));
  for (size_t row = 0; row < 4; ++row)
  {
    for (size_t byte = 0; byte < 4; ++byte)
      assert(upscaled_raw[row * 10 + byte] == 16 &&
             upscaled_raw[row * 10 + 5 + byte] == 255);
    assert(upscaled_raw[row * 10 + 4] == 0 && upscaled_raw[row * 10 + 9] == 0xff);
  }

  // A luma edge inside a single source cell must stay a single edge after
  // upscaling. Copying the cell would alternate bright and dark samples.
  const std::array<uint8_t, 12> fine_edge{
      16, 235, 235, 235, 16, 235, 235, 235, 128, 128, 128, 128};
  assert(cr::camhook::nv21_to_bggr10(fine_edge, 4, 2, upscaled_raw, 8, 4));
  for (size_t row = 0; row < 4; ++row)
  {
    const auto* pixels = upscaled_raw.data() + row * 10;
    assert(pixels[0] == 16 && pixels[1] == 16);
    assert(pixels[2] == 255 && pixels[3] == 255 && pixels[4] == 0xf0);
  }

  auto red = neutral;
  red.fill(81);
  red[8] = red[10] = 240;
  red[9] = red[11] = 90;
  assert(cr::camhook::nv21_to_bggr10(red, 4, 2, raw, 4, 2));
  assert(raw[0] == raw[1] && raw[1] == raw[5] && raw[5] < raw[6]);
  const auto before = raw;
  assert(!cr::camhook::nv21_to_bggr10(red, 4, 2, raw, 4, 4));
  assert(!cr::camhook::nv21_to_bggr10(red, 4, 2, raw, 6, 2));
  assert(raw == before);

  // Reuse only rows whose luma and interleaved chroma both match. The 2x
  // upscale also exercises repeated rows of the same Bayer phase.
  std::array<uint8_t, 24> first{
      16, 40, 60, 80, 20, 44, 64, 84, 24, 48, 68, 88, 28, 52, 72, 92,
      128, 128, 128, 128, 160, 120, 152, 110};
  auto next = first;
  next[2] = 220;
  next[21] = 200;
  std::array<uint8_t, 80> previous_raw{}, expected{}, reused{};
  assert(cr::camhook::nv21_to_bggr10(first, 4, 4, previous_raw, 8, 8));
  assert(cr::camhook::nv21_to_bggr10(next, 4, 4, expected, 8, 8));
  assert(cr::camhook::nv21_to_bggr10(next, 4, 4, reused, 8, 8, {}, false, false,
                                    {first, previous_raw}));
  assert(reused == expected && reused != previous_raw);

  const std::array<float, 9> identity{{1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f}};
  cr::camhook::Raw10Color calibrated;
  assert(cr::camhook::invert_sensor_color(identity, {2.f, 1.f, 1.f, 1.5f}, calibrated));
  auto gray = neutral;
  gray.fill(128);
  assert(cr::camhook::nv21_to_bggr10(gray, 4, 2, raw, 4, 2, calibrated, true));
  const auto unpack = [&raw](size_t start, size_t pixel) {
    return static_cast<unsigned>(raw[start + pixel]) * 4 +
           ((raw[start + 4] >> (pixel * 2)) & 3u);
  };
  const auto blue = unpack(0, 0) - 64, green = unpack(0, 1) - 64, red_value = unpack(5, 1) - 64;
  assert(std::abs(blue * 1.5 - green) <= 1.5);
  assert(std::abs(red_value * 2.0 - green) <= 1.0);
  const auto saved = calibrated;
  auto nearby = calibrated;
  nearby.even[2] += 40;
  nearby.even[1] -= 40;
  nearby.odd[6] += 128;
  assert(cr::camhook::raw10_color_compatible(calibrated, nearby));
  nearby.even[0] += 49;
  assert(!cr::camhook::raw10_color_compatible(calibrated, nearby));
  nearby.even = calibrated.even;
  ++nearby.odd[6];
  assert(!cr::camhook::raw10_color_compatible(calibrated, nearby));
  const auto wide_sensor = cr::camhook::pixel5_wide_raw_color({.5f, 1.f, .75f});
  assert(wide_sensor && wide_sensor->even == wide_sensor->odd);
  const auto row_sum = [](const cr::camhook::Raw10Color::Matrix& matrix, size_t row) {
    return matrix[3 * row] + matrix[3 * row + 1] + matrix[3 * row + 2];
  };
  assert(std::abs(row_sum(wide_sensor->even, 0) - 2174) < 6);
  assert(std::abs(row_sum(wide_sensor->even, 1) - 4155) < 6);
  assert(std::abs(row_sum(wide_sensor->even, 2) - 2837) < 6);
  assert(!cr::camhook::pixel5_wide_raw_color({1.f, 0.f, .5f}));
  assert(!cr::camhook::pixel5_wide_raw_color({2.f, .2f, .5f}));
  const std::array<float, 9> singular{};
  assert(!cr::camhook::invert_sensor_color(singular, {1.f, 1.f, 1.f, 1.f}, calibrated));
  assert(!cr::camhook::invert_sensor_color(identity, {0.f, 1.f, 1.f, 1.f}, calibrated));
  assert(calibrated == saved);
  assert(cr::camhook::nv21_to_bggr10(red, 4, 2, raw, 4, 2, {}, true));
  const auto red_709 = raw;
  assert(cr::camhook::nv21_to_bggr10(red, 4, 2, raw, 4, 2, {}, false));
  assert(raw != red_709);
  assert(cr::camhook::nv21_to_bggr10(gray, 4, 2, raw, 4, 2, {}, true, true));
  assert(unpack(0, 0) < green + 64);

  cr::camhook::Raw10StreamCalibration history;
  const int wide = 0, other = 1;
  constexpr int64_t observed = 20'000'000'000LL;
  history.remember(&wide, 0, observed, calibrated);
  history.remember(&wide, 0, observed - 1, {});
  assert(history.recall(&wide, 0, observed + 7'000'000'000LL) == calibrated);
  assert(!history.recall(&wide, 0, observed + 12'000'000'001LL));
  assert(!history.recall(&wide, 0, observed - 1));
  assert(!history.recall(&wide, 1, observed));
  assert(!history.recall(&other, 0, observed));
  history.remember(&other, 0, observed, calibrated);
  history.forget(&wide);
  assert(!history.recall(&wide, 0, observed + 1));
  assert(history.recall(&other, 0, observed + 1) == calibrated);
}
