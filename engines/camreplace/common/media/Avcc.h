#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cr::media
{

struct H264Dimensions
{
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  explicit operator bool() const noexcept { return width != 0 && height != 0; }
};

struct H264SpsInfo
{
  H264Dimensions dimensions;
  std::optional<bool> full_range;
  std::uint8_t matrix_coefficients = 2; // Unspecified in H.264 VUI.
};

namespace detail
{

// RBSP/SPS geometry reader adapted from ReCam's MIT-licensed H.264 parser.
// It reads the fields needed to discover dimensions and VUI video signal type,
// with caps added before every attacker-controlled loop and multiplication.
class H264BitReader
{
public:
  explicit H264BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  bool bad() const noexcept { return bad_; }

  std::uint32_t bits(unsigned count)
  {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value = (value << 1u) | bit();
    return value;
  }

  std::uint32_t bit()
  {
    if (byte_ >= bytes_.size())
    {
      bad_ = true;
      return 0;
    }
    if (bit_ == 0 && byte_ >= 2 && bytes_[byte_] == 0x03 &&
        bytes_[byte_ - 1] == 0 && bytes_[byte_ - 2] == 0)
    {
      ++byte_;
      if (byte_ >= bytes_.size())
      {
        bad_ = true;
        return 0;
      }
    }
    const std::uint32_t value = (bytes_[byte_] >> (7u - bit_)) & 1u;
    if (++bit_ == 8)
    {
      bit_ = 0;
      ++byte_;
    }
    return value;
  }

  std::uint32_t ue()
  {
    unsigned zeroes = 0;
    while (bit() == 0)
    {
      if (bad_ || ++zeroes > 31)
      {
        bad_ = true;
        return 0;
      }
    }
    return zeroes == 0 ? 0 : ((1u << zeroes) - 1u + bits(zeroes));
  }

  std::int32_t se()
  {
    const std::uint32_t value = ue();
    return (value & 1u) ? static_cast<std::int32_t>((value + 1u) / 2u)
                        : -static_cast<std::int32_t>(value / 2u);
  }

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t byte_ = 0;
  unsigned bit_ = 0;
  bool bad_ = false;
};

inline bool h264_high_profile(std::uint32_t profile)
{
  switch (profile)
  {
  case 100: case 110: case 122: case 244: case 44: case 83: case 86:
  case 118: case 128: case 138: case 139: case 134: case 135:
    return true;
  default:
    return false;
  }
}

inline void skip_h264_scaling_list(H264BitReader& reader, unsigned size)
{
  std::int32_t last = 8;
  std::int32_t next = 8;
  for (unsigned i = 0; i < size && !reader.bad(); ++i)
  {
    if (next != 0) next = (last + reader.se() % 256 + 256) % 256;
    if (next != 0) last = next;
  }
}

} // namespace detail

inline H264SpsInfo parse_h264_sps(
    std::span<const std::uint8_t> nalu)
{
  H264SpsInfo result{};
  if (nalu.size() < 4 || (nalu[0] & 0x1fu) != 7u) return result;
  detail::H264BitReader reader(nalu.subspan(1));
  const std::uint32_t profile = reader.bits(8);
  reader.bits(8); // constraint flags and reserved bits
  reader.bits(8); // level_idc
  reader.ue();    // seq_parameter_set_id

  std::uint32_t chroma_format = 1;
  bool separate_colour_plane = false;
  if (detail::h264_high_profile(profile))
  {
    chroma_format = reader.ue();
    if (chroma_format > 3) return result;
    if (chroma_format == 3) separate_colour_plane = reader.bit() != 0;
    reader.ue(); // bit_depth_luma_minus8
    reader.ue(); // bit_depth_chroma_minus8
    reader.bit();
    if (reader.bit())
    {
      const unsigned lists = chroma_format == 3 ? 12u : 8u;
      for (unsigned i = 0; i < lists && !reader.bad(); ++i)
        if (reader.bit()) detail::skip_h264_scaling_list(reader, i < 6 ? 16 : 64);
    }
  }

  reader.ue();
  const std::uint32_t poc_type = reader.ue();
  if (poc_type == 0)
  {
    reader.ue();
  }
  else if (poc_type == 1)
  {
    reader.bit();
    reader.se();
    reader.se();
    const std::uint32_t cycle = reader.ue();
    if (cycle > 256) return result;
    for (std::uint32_t i = 0; i < cycle && !reader.bad(); ++i) reader.se();
  }
  else if (poc_type > 2)
  {
    return result;
  }

  reader.ue();
  reader.bit();
  const std::uint32_t width_mbs_minus_one = reader.ue();
  const std::uint32_t height_units_minus_one = reader.ue();
  if (width_mbs_minus_one >= 1024 || height_units_minus_one >= 1024)
    return result;
  const std::uint64_t width_mbs = static_cast<std::uint64_t>(width_mbs_minus_one) + 1u;
  const std::uint64_t height_units = static_cast<std::uint64_t>(height_units_minus_one) + 1u;
  const std::uint32_t frame_mbs_only = reader.bit();
  if (!frame_mbs_only) reader.bit();
  reader.bit();

  std::uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
  if (reader.bit())
  {
    crop_left = reader.ue();
    crop_right = reader.ue();
    crop_top = reader.ue();
    crop_bottom = reader.ue();
  }
  if (reader.bad()) return result;

  std::uint32_t sub_width = 1;
  std::uint32_t sub_height = 1;
  if (!separate_colour_plane)
  {
    if (chroma_format == 1) { sub_width = 2; sub_height = 2; }
    else if (chroma_format == 2) { sub_width = 2; }
  }
  const std::uint64_t crop_unit_x = separate_colour_plane ? 1u : sub_width;
  const std::uint64_t crop_unit_y =
      (separate_colour_plane ? 1u : sub_height) * (2u - frame_mbs_only);
  const std::uint64_t coded_width = width_mbs * 16u;
  const std::uint64_t coded_height =
      (2u - frame_mbs_only) * height_units * 16u;
  const std::uint64_t horizontal_crop = crop_unit_x *
      (static_cast<std::uint64_t>(crop_left) + crop_right);
  const std::uint64_t vertical_crop = crop_unit_y *
      (static_cast<std::uint64_t>(crop_top) + crop_bottom);
  if (horizontal_crop >= coded_width || vertical_crop >= coded_height)
    return result;
  const std::uint64_t width = coded_width - horizontal_crop;
  const std::uint64_t height = coded_height - vertical_crop;
  if (width < 2 || height < 2 || width > 16384 || height > 16384)
    return result;
  result.dimensions = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};

  if (!reader.bit()) return result; // vui_parameters_present_flag
  if (reader.bit() && reader.bits(8) == 255u) // Extended_SAR
  {
    reader.bits(16); // sar_width
    reader.bits(16); // sar_height
  }
  if (reader.bit()) reader.bit(); // overscan_appropriate_flag
  if (!reader.bit()) return result; // video_signal_type_present_flag
  reader.bits(3); // video_format
  const bool full_range = reader.bit() != 0;
  std::uint8_t matrix = 2;
  if (reader.bit()) // colour_description_present_flag
  {
    reader.bits(8); // colour_primaries
    reader.bits(8); // transfer_characteristics
    matrix = static_cast<std::uint8_t>(reader.bits(8));
  }
  if (!reader.bad())
  {
    result.full_range = full_range;
    result.matrix_coefficients = matrix;
  }
  return result;
}

inline H264Dimensions parse_h264_sps_dimensions(
    std::span<const std::uint8_t> nalu)
{
  return parse_h264_sps(nalu).dimensions;
}

struct AvcDecoderConfig
{
  std::uint8_t nalu_length_size = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
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

  auto read_sets = [&](std::uint8_t count, bool sps) -> bool
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
      if (sps && parsed.width == 0)
      {
        const H264Dimensions dimensions =
            parse_h264_sps_dimensions(data.subspan(cursor, size));
        if (dimensions)
        {
          parsed.width = dimensions.width;
          parsed.height = dimensions.height;
        }
      }
      if (!append_annexb_nalu(data.subspan(cursor, size), parsed.parameter_sets_annexb))
        return false;
      cursor += size;
    }
    return true;
  };

  if (!read_sets(sps_count, true) || cursor >= data.size())
    return false;
  const std::uint8_t pps_count = data[cursor++];
  if (pps_count == 0 || !read_sets(pps_count, false))
    return false;

  out = std::move(parsed);
  return true;
}

inline bool avcc_access_unit_to_annexb(std::span<const std::uint8_t> data,
                                       std::uint8_t length_size,
                                       std::vector<std::uint8_t>& out,
                                       std::span<const std::uint8_t> prefix)
{
  if (data.empty() || (length_size != 1 && length_size != 2 && length_size != 4))
    return false;

  const auto next_nalu = [&](std::size_t& cursor) -> std::span<const std::uint8_t>
  {
    if (data.size() - cursor < length_size)
      return {};
    std::uint32_t nalu_size = 0;
    for (std::uint8_t i = 0; i < length_size; ++i)
      nalu_size = (nalu_size << 8u) | data[cursor + i];
    cursor += length_size;
    if (nalu_size == 0 || nalu_size > data.size() - cursor)
      return {};
    const auto nalu = data.subspan(cursor, nalu_size);
    cursor += nalu_size;
    return nalu;
  };

  // Validate before touching out, so a truncated unit preserves prior output.
  // The receive and prefix storage must remain distinct from the destination.
  std::size_t size = prefix.size();
  for (std::size_t cursor = 0; cursor < data.size();)
  {
    const auto nalu = next_nalu(cursor);
    if (nalu.empty() || size > out.max_size() - 4 ||
        nalu.size() > out.max_size() - size - 4)
      return false;
    size += 4 + nalu.size();
  }
  out.reserve(size);
  out.clear();
  if (!prefix.empty())
    out.insert(out.end(), prefix.begin(), prefix.end());
  for (std::size_t cursor = 0; cursor < data.size();)
    append_annexb_nalu(next_nalu(cursor), out);
  return true;
}

inline bool avcc_access_unit_to_annexb(std::span<const std::uint8_t> data,
                                       std::uint8_t length_size,
                                       std::vector<std::uint8_t>& out)
{
  std::vector<std::uint8_t> converted;
  if (!avcc_access_unit_to_annexb(data, length_size, converted, {}))
    return false;
  out.swap(converted);
  return true;
}

} // namespace cr::media
