#include "LocalTicketAuth.h"

extern "C"
{
#include "vendor/monocypher/monocypher.h"
}

#include <array>
#include <cstring>
#include <new>
#include <vector>

namespace cr::secure_channel::v2
{
namespace
{

constexpr char kTicketMacDomain[] = "LONGVC local ticket proof v1";
constexpr std::size_t kDomainBytes = sizeof(kTicketMacDomain) - 1;
using MacInput = std::array<std::uint8_t, kDomainBytes + kTicketUnsignedBytes>;

Status compute_mac(std::array<std::uint8_t, 64>& output,
                   Bytes unsigned_ticket,
                   Bytes shared_secret) noexcept
{
  if (!unsigned_ticket.valid() || unsigned_ticket.size != kTicketUnsignedBytes ||
      !shared_secret.valid() || shared_secret.size != kKeyBytes)
    return Status::failure(Error::InvalidField);

  MacInput input{};
  std::memcpy(input.data(), kTicketMacDomain, kDomainBytes);
  std::memcpy(input.data() + kDomainBytes, unsigned_ticket.data, unsigned_ticket.size);
  crypto_blake2b_keyed(output.data(), output.size(),
                       shared_secret.data, shared_secret.size,
                       input.data(), input.size());
  crypto_wipe(input.data(), input.size());
  return {};
}

} // namespace

Status sign_local_ticket(Ticket& ticket, Bytes shared_secret) noexcept
{
  try
  {
    ticket.signature.fill(0);
    std::vector<std::uint8_t> encoded;
    if (Status status = encode_ticket(ticket, encoded); !status)
      return status;
    return compute_mac(ticket.signature,
                       {encoded.data(), kTicketUnsignedBytes},
                       shared_secret);
  }
  catch (const std::bad_alloc&)
  {
    ticket.signature.fill(0);
    return Status::failure(Error::ResourceExhausted);
  }
}

Status verify_local_ticket(const TicketView& ticket, Bytes shared_secret) noexcept
{
  std::array<std::uint8_t, 64> expected{};
  const Status status = compute_mac(expected, ticket.unsigned_bytes, shared_secret);
  if (!status)
    return status;
  const bool valid = crypto_verify64(expected.data(), ticket.signature.data) == 0;
  crypto_wipe(expected.data(), expected.size());
  return valid ? Status{} : Status::failure(Error::AuthenticationFailed);
}

} // namespace cr::secure_channel::v2
