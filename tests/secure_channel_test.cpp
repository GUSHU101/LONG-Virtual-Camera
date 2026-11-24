#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "../engines/camreplace/common/secure_channel/LocalTicketAuth.h"
#include "../engines/camreplace/common/secure_channel/SecureSession.h"

extern "C"
{
#include "../engines/camreplace/common/secure_channel/vendor/monocypher/monocypher.h"
}

namespace sc = cr::secure_channel;
namespace v2 = cr::secure_channel::v2;

static std::vector<std::uint8_t> make_ticket(
    const std::array<std::uint8_t, 32>& host_private,
    const std::array<std::uint8_t, 32>& phone_private)
{
  v2::Ticket ticket{};
  ticket.stream = v2::StreamKind::Video;
  ticket.issued_at_ms = 1'000'000;
  ticket.expires_at_ms = 1'060'000;
  for (std::size_t i = 0; i < ticket.session_id.size(); ++i)
    ticket.session_id[i] = static_cast<std::uint8_t>(0x30 + i);
  for (std::size_t i = 0; i < ticket.phone_nonce.size(); ++i)
    ticket.phone_nonce[i] = static_cast<std::uint8_t>(0x70 + i);
  crypto_x25519_public_key(ticket.host_ephemeral_public.data(), host_private.data());
  crypto_x25519_public_key(ticket.phone_ephemeral_public.data(), phone_private.data());

  std::array<std::uint8_t, 32> shared{};
  crypto_x25519(shared.data(), host_private.data(), ticket.phone_ephemeral_public.data());
  assert(v2::sign_local_ticket(ticket, {shared.data(), shared.size()}));
  assert(std::any_of(ticket.signature.begin(), ticket.signature.end(), [](std::uint8_t b) { return b != 0; }));
  crypto_wipe(shared.data(), shared.size());

  std::vector<std::uint8_t> encoded;
  assert(v2::encode_ticket(ticket, encoded));
  return encoded;
}

int main()
{
  std::array<std::uint8_t, 32> host_private{};
  std::array<std::uint8_t, 32> phone_private{};
  for (std::size_t i = 0; i < host_private.size(); ++i)
  {
    host_private[i] = static_cast<std::uint8_t>(i + 1);
    phone_private[i] = static_cast<std::uint8_t>(0xa0 + i);
  }

  const auto ticket = make_ticket(host_private, phone_private);
  v2::SecureSession host;
  v2::SecureSession phone;
  assert(host.initialize(v2::Role::Host, {host_private.data(), host_private.size()},
                         {ticket.data(), ticket.size()}, v2::StreamKind::Video, 1'030'000));
  assert(phone.initialize(v2::Role::Phone, {phone_private.data(), phone_private.size()},
                          {ticket.data(), ticket.size()}, v2::StreamKind::Video, 1'030'000));

  std::vector<std::uint8_t> host_finished;
  assert(host.make_finished(host_finished));
  std::vector<std::uint8_t> plaintext;
  v2::RecordType type{};
  assert(phone.open({host_finished.data(), host_finished.size()}, plaintext, type));
  assert(type == v2::RecordType::Finished);

  std::vector<std::uint8_t> phone_finished;
  assert(phone.make_finished(phone_finished));
  assert(host.open({phone_finished.data(), phone_finished.size()}, plaintext, type));
  assert(host.established() && phone.established());

  const std::array<std::uint8_t, 6> message{'L', 'O', 'N', 'G', 'V', 'C'};
  std::vector<std::uint8_t> record;
  assert(host.seal(v2::RecordType::Application,
                   {message.data(), message.size()}, record));
  assert(phone.open({record.data(), record.size()}, plaintext, type));
  assert(type == v2::RecordType::Application);
  assert(std::equal(plaintext.begin(), plaintext.end(), message.begin(), message.end()));
  const auto replay = phone.open({record.data(), record.size()}, plaintext, type);
  assert(!replay && replay.code == sc::Error::ReplayDetected);

  auto tampered_unsigned = ticket;
  tampered_unsigned[24] ^= 0x40; // canonical session id is covered by the proof
  v2::SecureSession rejected_unsigned;
  const auto unsigned_status = rejected_unsigned.initialize(
      v2::Role::Host, {host_private.data(), host_private.size()},
      {tampered_unsigned.data(), tampered_unsigned.size()}, v2::StreamKind::Video, 1'030'000);
  assert(!unsigned_status && unsigned_status.code == sc::Error::AuthenticationFailed);

  auto tampered_proof = ticket;
  tampered_proof.back() ^= 0x01;
  v2::SecureSession rejected_proof;
  const auto proof_status = rejected_proof.initialize(
      v2::Role::Phone, {phone_private.data(), phone_private.size()},
      {tampered_proof.data(), tampered_proof.size()}, v2::StreamKind::Video, 1'030'000);
  assert(!proof_status && proof_status.code == sc::Error::AuthenticationFailed);

  auto zero_proof = ticket;
  std::fill(zero_proof.end() - 64, zero_proof.end(), std::uint8_t{0});
  v2::SecureSession rejected_zero;
  const auto zero_status = rejected_zero.initialize(
      v2::Role::Host, {host_private.data(), host_private.size()},
      {zero_proof.data(), zero_proof.size()}, v2::StreamKind::Video, 1'030'000);
  assert(!zero_status && zero_status.code == sc::Error::AuthenticationFailed);
  return 0;
}
