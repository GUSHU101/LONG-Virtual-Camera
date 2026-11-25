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
  v2::TicketView view;
  assert(v2::parse_ticket({ticket.data(), ticket.size()}, view));
  std::array<std::uint8_t, 32> shared{};
  crypto_x25519(shared.data(), host_private.data(), view.ticket.phone_ephemeral_public.data());
  assert(v2::verify_local_ticket(view, v2::bytes_of(shared)));
  view.signature = {view.signature.data, 1};
  assert(v2::verify_local_ticket(view, v2::bytes_of(shared)).code == sc::Error::InvalidField);
  view.signature = {nullptr, 64};
  assert(v2::verify_local_ticket(view, v2::bytes_of(shared)).code == sc::Error::InvalidField);
  crypto_wipe(shared.data(), shared.size());

  view.ticket.stream = static_cast<v2::StreamKind>(0xff);
  std::vector<std::uint8_t> invalid_ticket;
  assert(v2::encode_ticket(view.ticket, invalid_ticket).code == sc::Error::InvalidField);
  v2::detail::Reader empty_reader({});
  v2::Bytes empty_span;
  assert(empty_reader.bytes(0, empty_span));
  assert(empty_span.data == nullptr && empty_span.size == 0);

  v2::SecureSession host;
  v2::SecureSession phone;
  assert(phone.initialize(static_cast<v2::Role>(0xff), v2::bytes_of(phone_private),
                          {ticket.data(), ticket.size()}, v2::StreamKind::Video,
                          1'030'000).code == sc::Error::InvalidField);
  assert(host.initialize(v2::Role::Host, {host_private.data(), host_private.size()},
                         {ticket.data(), ticket.size()}, v2::StreamKind::Video, 1'030'000));
  assert(phone.initialize(v2::Role::Phone, {phone_private.data(), phone_private.size()},
                          {ticket.data(), ticket.size()}, v2::StreamKind::Video, 1'030'000));

  std::vector<std::uint8_t> plaintext{0xff};
  auto type = v2::RecordType::Application;
  std::vector<std::uint8_t> wrong_finished;
  const std::array<std::uint8_t, 32> wrong_proof{};
  assert(host.seal(v2::RecordType::Finished, v2::bytes_of(wrong_proof), wrong_finished));
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    assert(phone.open({wrong_finished.data(), wrong_finished.size()}, plaintext,
                      type).code == sc::Error::FinishedMismatch);
    assert(plaintext.empty() && type == v2::RecordType::Application);
  }

  std::vector<std::uint8_t> host_finished;
  assert(host.make_finished(host_finished));
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
  auto damaged_record = record;
  damaged_record.back() ^= 1;
  assert(phone.open({damaged_record.data(), damaged_record.size()}, plaintext,
                    type).code == sc::Error::AuthenticationFailed);
  assert(plaintext.empty() && type == v2::RecordType::Finished);
  assert(phone.open({record.data(), record.size()}, plaintext, type));
  assert(type == v2::RecordType::Application);
  assert(std::equal(plaintext.begin(), plaintext.end(), message.begin(), message.end()));
  const auto replay = phone.open({record.data(), record.size()}, plaintext, type);
  assert(!replay && replay.code == sc::Error::ReplayDetected);
  assert(plaintext.empty() && type == v2::RecordType::Application);
  plaintext = {0xff};
  assert(phone.open({}, plaintext, type).code == sc::Error::Truncated);
  assert(plaintext.empty() && type == v2::RecordType::Application);

  const auto invalid_record = host.seal(static_cast<v2::RecordType>(0xff), {}, record);
  assert(invalid_record.code == sc::Error::InvalidField && record.empty());
  assert(host.seal(v2::RecordType::Application, {}, record));
  v2::RecordView parsed;
  assert(v2::parse_record({record.data(), record.size()}, parsed));
  assert(parsed.header.sequence == 3);
  assert(phone.open({record.data(), record.size()}, plaintext, type));
  assert(plaintext.empty());

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
