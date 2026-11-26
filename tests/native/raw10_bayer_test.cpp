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
}
