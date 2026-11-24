#include "security/Sha256.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{

std::string hex_digest(std::string_view input)
{
  std::array<std::uint8_t, 32> digest{};
  cr::sha256::Hash hash;
  hash.init();
  // Exercise incremental updates alongside the known SHA-256 vectors.
  const std::size_t split = input.size() / 2;
  hash.update(input.data(), split);
  hash.update(input.data() + split, input.size() - split);
  hash.final(digest.data());

  constexpr char kHex[] = "0123456789abcdef";
  std::string encoded(64, '0');
  for (std::size_t i = 0; i < digest.size(); ++i)
  {
    encoded[i * 2] = kHex[digest[i] >> 4];
    encoded[i * 2 + 1] = kHex[digest[i] & 0x0f];
  }
  return encoded;
}

} // namespace

int main()
{
  if (hex_digest("") !=
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
    return 1;
  if (hex_digest("abc") !=
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    return 2;
  if (hex_digest("The quick brown fox jumps over the lazy dog") !=
      "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592")
    return 3;
  return 0;
}
