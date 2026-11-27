#include "media/Avcc.h"

#include <cassert>
#include <cstdint>
#include <vector>

using cr::media::AvcDecoderConfig;

static std::vector<std::uint8_t> config(std::uint8_t length_size_minus_one)
{
  return {1, 100, 0, 31, static_cast<std::uint8_t>(0xfcu | length_size_minus_one),
          0xe1, 0, 3, 0x67, 0x64, 0x1f, 1, 0, 2, 0x68, 0xee};
}

#include "media/RtmpAddress.h"

int main()
{
  const std::vector<std::uint8_t> sps_1280x720{
      0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50, 0x05,
      0xbb, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00,
      0x00, 0x03, 0x00, 0x20, 0xf1, 0x83, 0x19, 0x60};
  const auto dimensions = cr::media::parse_h264_sps_dimensions(sps_1280x720);
  assert(dimensions.width == 1280 && dimensions.height == 720);
  const std::vector<std::uint8_t> truncated_sps{0x67, 0x64, 0x00, 0x1f};
  assert(!cr::media::parse_h264_sps_dimensions(truncated_sps));
  const auto untagged = cr::media::parse_h264_sps(sps_1280x720);
  assert(!untagged.full_range.has_value() && untagged.matrix_coefficients == 2);
  const std::vector<std::uint8_t> bt709_full_sps{
      0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50, 0x05, 0xbb,
      0x01, 0x6e, 0x04, 0x04, 0x02, 0x80, 0x00, 0x00, 0x03, 0x00,
      0x80, 0x00, 0x00, 0x1e, 0x07, 0x8c, 0x18, 0xcb};
  const auto tagged = cr::media::parse_h264_sps(bt709_full_sps);
  assert(tagged.dimensions.width == 1280 && tagged.dimensions.height == 720);
  assert(tagged.full_range == true && tagged.matrix_coefficients == 1);
  const auto truncated_vui = cr::media::parse_h264_sps(
      std::span<const std::uint8_t>(bt709_full_sps).first(13));
  assert(truncated_vui.dimensions.width == 1280);
  assert(!truncated_vui.full_range.has_value() && truncated_vui.matrix_coefficients == 2);

  auto tagged_avcc = config(3);
  tagged_avcc[7] = static_cast<uint8_t>(bt709_full_sps.size());
  tagged_avcc.erase(tagged_avcc.begin() + 8, tagged_avcc.begin() + 11);
  tagged_avcc.insert(tagged_avcc.begin() + 8, bt709_full_sps.begin(), bt709_full_sps.end());
  AvcDecoderConfig tagged_config;
  assert(cr::media::parse_avc_decoder_config(tagged_avcc, tagged_config));
  assert(tagged_config.sps.dimensions.width == 1280 &&
         tagged_config.sps.dimensions.height == 720);
  assert(tagged_config.sps.full_range == true &&
         tagged_config.sps.matrix_coefficients == 1);

  for (const std::uint8_t minus_one : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{3}})
  {
    AvcDecoderConfig parsed{};
    const auto bytes = config(minus_one);
    assert(cr::media::parse_avc_decoder_config(bytes, parsed));
    assert(parsed.nalu_length_size == minus_one + 1);
    assert(parsed.parameter_sets_annexb.size() == 13);
  }

  AvcDecoderConfig unchanged{};
  unchanged.nalu_length_size = 4;
  unchanged.parameter_sets_annexb = {9};
  auto reserved = config(2);
  assert(!cr::media::parse_avc_decoder_config(reserved, unchanged));
  assert(unchanged.nalu_length_size == 4 && unchanged.parameter_sets_annexb == std::vector<std::uint8_t>{9});
  auto truncated = config(3);
  truncated.pop_back();
  assert(!cr::media::parse_avc_decoder_config(truncated, unchanged));

  std::vector<std::uint8_t> out;
  const std::vector<std::uint8_t> one{2, 0x65, 0xaa};
  const std::vector<std::uint8_t> two{0, 2, 0x41, 0xbb};
  const std::vector<std::uint8_t> four{0, 0, 0, 2, 0x41, 0xcc};
  assert(cr::media::avcc_access_unit_to_annexb(one, 1, out));
  assert(out == (std::vector<std::uint8_t>{0, 0, 0, 1, 0x65, 0xaa}));
  assert(cr::media::avcc_access_unit_to_annexb(two, 2, out));
  assert(cr::media::avcc_access_unit_to_annexb(four, 4, out));

  const auto saved = out;
  const std::vector<std::uint8_t> malformed{0, 0, 0, 9, 1};
  const std::vector<std::uint8_t> zero{0};
  assert(!cr::media::avcc_access_unit_to_annexb(malformed, 4, out));
  assert(out == saved); // malformed input is transactional, never partial.
  assert(!cr::media::avcc_access_unit_to_annexb({}, 4, out));
  assert(!cr::media::avcc_access_unit_to_annexb(zero, 1, out));
  const std::vector<std::uint8_t> prefix{0, 0, 0, 1, 0x67};
  assert(cr::media::avcc_access_unit_to_annexb(four, 4, out, prefix));
  assert(out == (std::vector<std::uint8_t>{0, 0, 0, 1, 0x67, 0, 0, 0, 1, 0x41, 0xcc}));
  const auto prefixed = out;
  assert(!cr::media::avcc_access_unit_to_annexb(malformed, 4, out, prefix));
  assert(out == prefixed);
  cr::media::RtmpAddress address;
  assert(cr::media::parse_rtmp_address("rtmp://relay.local/longvc", address));
  assert(address.host == "relay.local" && address.port == "1935" && address.app.empty());
  assert(address.tc_url == "rtmp://relay.local" && address.stream == "longvc");
  assert(cr::media::parse_rtmp_address(
      "rtmp://[2001:db8::1]:31935/live/studio?user=example&pass=example", address));
  assert(address.host == "2001:db8::1" && address.port == "31935" && address.app == "live");
  assert(address.tc_url == "rtmp://[2001:db8::1]:31935/live");
  assert(address.stream == "studio?user=example&pass=example");
  for (auto url : {"rtmp://localhost:", "rtmp://localhost:/live", "rtmp://localhost:0/live",
                   "rtmp://localhost:65536/live", "rtmp://@localhost/live", "rtmp://localhost/",
                   "rtmps://localhost/live", "rtmp://localhost/live/#fragment"})
    assert(!cr::media::parse_rtmp_address(url, address));
  return 0;
}
