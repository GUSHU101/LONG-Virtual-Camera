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

int main()
{
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
  return 0;
}
