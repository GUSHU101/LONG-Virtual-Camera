#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

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

inline bool raw10_color_compatible(const Raw10Color& rendered, const Raw10Color& requested)
{
  // Bound the error of each row for any linear RGB sample. Minor quantization
  // noise can reuse a frame, while a meaningful white balance change cannot.
  constexpr int32_t kMaxRowError = Raw10Color::scale / 32;
  const auto within_budget = [](const Raw10Color::Matrix& a, const Raw10Color::Matrix& b) {
    for (size_t row = 0; row < a.size(); row += 3)
    {
      const int64_t error = std::abs(static_cast<int64_t>(a[row]) - b[row]) +
                            std::abs(static_cast<int64_t>(a[row + 1]) - b[row + 1]) +
                            std::abs(static_cast<int64_t>(a[row + 2]) - b[row + 2]);
      if (error > kMaxRowError)
        return false;
    }
    return true;
  };
  return within_budget(rendered.even, requested.even) &&
         within_budget(rendered.odd, requested.odd);
}

// Pixel 5 rear wide's reviewed DNG forward matrices are identical at both
// reference illuminants. Invert that sensor-to-XYZ(D50) transform against
// the sRGB primaries, and normalize rows to the DNG neutral white.
inline std::optional<Raw10Color> pixel5_wide_raw_color(const std::array<float, 3>& neutral)
{
  for (float coordinate : neutral)
    if (!std::isfinite(coordinate) || coordinate < .125f || coordinate > 2.f)
      return {};

  constexpr std::array<float, 9> reference_from_srgb{{
      .982715f, .008293f, .008992f,
      .008448f, .997393f, -.005841f,
      -.003258f, .005237f, .998021f}};
  // Residual Pixel 5 rear-wide HDR+ response, fitted in linear sRGB using
  // neutral and warm color patches and blended 75% towards its inverse.
  // The nonnegative rows keep saturated input colors in the valid domain.
  constexpr std::array<float, 9> hdr_input_compensation{{
      .83404f, .05076f, .17796f,
      .08416f, .89352f, .03616f,
      .08180f, .05572f, .78589f}};
  Raw10Color result;
  for (size_t row = 0; row < neutral.size(); ++row)
  {
    const float sensor_neutral = neutral[row] / neutral[1];
    if (sensor_neutral < .125f || sensor_neutral > 2.f)
      return {};
    for (size_t col = 0; col < 3; ++col)
    {
      const size_t index = row * 3 + col;
      const float coefficient = reference_from_srgb[row * 3] * hdr_input_compensation[col] +
                                reference_from_srgb[row * 3 + 1] * hdr_input_compensation[3 + col] +
                                reference_from_srgb[row * 3 + 2] * hdr_input_compensation[6 + col];
      result.even[index] =
          std::lround(coefficient * sensor_neutral * Raw10Color::scale);
    }
  }
  result.odd = result.even;
  return result;
}

class Raw10StreamCalibration
{
public:
  void forget(const void* stream)
  {
    for (auto& entry : entries_)
      if (entry.stream == stream)
        entry = {};
  }

  void remember(const void* stream, size_t profile, int64_t timestamp, const Raw10Color& color)
  {
    if (!stream || timestamp <= 0)
      return;
    auto slot = find(stream, profile);
    if (slot == entries_.end())
      slot = std::min_element(entries_.begin(), entries_.end(),
          [](const Entry& a, const Entry& b) { return a.timestamp < b.timestamp; });
    else if (slot->timestamp > timestamp)
      return;
    *slot = {stream, profile, timestamp, color};
  }

  std::optional<Raw10Color> recall(const void* stream, size_t profile, int64_t timestamp)
  {
    if (!stream || timestamp <= 0)
      return {};
    const auto slot = find(stream, profile);
    constexpr int64_t kMaxCalibrationAgeNs = 12'000'000'000LL;
    if (slot == entries_.end() || timestamp < slot->timestamp ||
        timestamp - slot->timestamp > kMaxCalibrationAgeNs)
      return {};
    return slot->color;
  }

private:
  struct Entry
  {
    const void* stream = nullptr;
    size_t profile = 0;
    int64_t timestamp = 0;
    Raw10Color color;
  };
  std::array<Entry, 8> entries_{};

  std::array<Entry, 8>::iterator find(const void* stream, size_t profile)
  {
    return std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
      return entry.stream == stream && entry.profile == profile;
    });
  }
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
