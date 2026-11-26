#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace cr::camhook
{

struct Raw10Color
{
  // Signed Q12 sensor-from-linear-sRGB matrices, one for each green row.
  static constexpr int32_t scale = 4096;
  using Matrix = std::array<int32_t, 9>;
  Matrix even{{scale, 0, 0, 0, scale, 0, 0, 0, scale}};
  Matrix odd = even;

  bool operator==(const Raw10Color&) const = default;
};

inline bool invert_sensor_color(const std::array<float, 9>& ccm,
                                const std::array<float, 4>& gains, Raw10Color& result)
{
  for (float gain : gains)
    if (!std::isfinite(gain) || gain < 0.5f || gain > 8.f)
      return false;
  for (float coefficient : ccm)
    if (!std::isfinite(coefficient) || std::abs(coefficient) > 4.f)
      return false;

  const auto& [a, b, c, d, e, f, g, h, i] = ccm;
  const std::array<float, 9> adjugate{{
      e*i - f*h, c*h - b*i, b*f - c*e,
      f*g - d*i, a*i - c*g, c*d - a*f,
      d*h - e*g, b*g - a*h, a*e - b*d}};
  const float determinant = a*adjugate[0] + b*adjugate[3] + c*adjugate[6];
  if (std::abs(determinant) < .05f)
    return false;

  Raw10Color checked;
  for (size_t index = 0; index < adjugate.size(); ++index)
  {
    constexpr std::array<size_t, 3> even_gain{{0, 1, 3}}, odd_gain{{0, 2, 3}};
    const float linear = adjugate[index] / determinant;
    const float even = linear / gains[even_gain[index / 3]];
    const float odd = linear / gains[odd_gain[index / 3]];
    if (std::abs(even) > 4.f || std::abs(odd) > 4.f)
      return false;
    checked.even[index] = std::lround(even * Raw10Color::scale);
    checked.odd[index] = std::lround(odd * Raw10Color::scale);
  }
  result = checked;
  return true;
}

} // namespace cr::camhook
