#include "Raw10Bayer.h"

#include <array>
#include <cassert>

int main()
{
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
}
