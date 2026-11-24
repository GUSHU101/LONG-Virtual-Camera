#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

namespace cr::media
{

struct AvcDecoderConfig
{
  std::uint8_t nalu_length_size = 0;
  std::vector<std::uint8_t> parameter_sets_annexb;
};

inline bool append_annexb_nalu(std::span<const std::uint8_t> nalu,
                               std::vector<std::uint8_t>& out)
{
  if (nalu.empty())
    return false;
  static constexpr std::uint8_t start_code[]{0, 0, 0, 1};
  out.insert(out.end(), std::begin(start_code), std::end(start_code));
  out.insert(out.end(), nalu.begin(), nalu.end());
  return true;
}

inline bool parse_avc_decoder_config(std::span<const std::uint8_t> data,
                                     AvcDecoderConfig& out)
{
  if (data.size() < 7 || data[0] != 1)
    return false;
  const std::uint8_t length_size = (data[4] & 0x03u) + 1u;
  if (length_size == 3)
    return false; // lengthSizeMinusOne == 2 is reserved by ISO/IEC 14496-15.

  AvcDecoderConfig parsed{};
  parsed.nalu_length_size = length_size;
  std::size_t cursor = 5;
  const std::uint8_t sps_count = data[cursor++] & 0x1fu;
  if (sps_count == 0)
    return false;

  auto read_sets = [&](std::uint8_t count) -> bool
  {
    for (std::uint8_t i = 0; i < count; ++i)
    {
      if (cursor > data.size() || data.size() - cursor < 2)
        return false;
      const std::size_t size = (static_cast<std::size_t>(data[cursor]) << 8u) |
                               data[cursor + 1];
      cursor += 2;
      if (size == 0 || cursor > data.size() || size > data.size() - cursor)
        return false;
      if (!append_annexb_nalu(data.subspan(cursor, size), parsed.parameter_sets_annexb))
        return false;
      cursor += size;
    }
    return true;
  };

  if (!read_sets(sps_count) || cursor >= data.size())
    return false;
  const std::uint8_t pps_count = data[cursor++];
  if (pps_count == 0 || !read_sets(pps_count))
    return false;

  out = std::move(parsed);
  return true;
}

inline bool avcc_access_unit_to_annexb(std::span<const std::uint8_t> data,
                                       std::uint8_t length_size,
                                       std::vector<std::uint8_t>& out)
{
  if (length_size != 1 && length_size != 2 && length_size != 4)
    return false;

  std::vector<std::uint8_t> converted;
  converted.reserve(data.size() + 16);
  std::size_t cursor = 0;
  while (cursor < data.size())
  {
    if (data.size() - cursor < length_size)
      return false;
    std::uint32_t nalu_size = 0;
    for (std::uint8_t i = 0; i < length_size; ++i)
      nalu_size = (nalu_size << 8u) | data[cursor + i];
    cursor += length_size;
    if (nalu_size == 0 || nalu_size > data.size() - cursor)
      return false;
    if (!append_annexb_nalu(data.subspan(cursor, nalu_size), converted))
      return false;
    cursor += nalu_size;
  }
  if (converted.empty())
    return false;
  out = std::move(converted);
  return true;
}

} // namespace cr::media
