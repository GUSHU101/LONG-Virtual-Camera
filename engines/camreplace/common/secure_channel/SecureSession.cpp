#include "SecureSession.h"
#include "LocalTicketAuth.h"

extern "C"
{
#include "vendor/monocypher/monocypher.h"
}

#include <cstring>
#include <new>

#if defined(_MSC_VER)
#define CR_NOINLINE __declspec(noinline)
#elif defined(__clang__) || defined(__GNUC__)
#define CR_NOINLINE __attribute__((noinline))
#else
#define CR_NOINLINE
#endif

namespace cr::secure_channel::v2
{
namespace
{

constexpr char kMasterDomain[] = "CRSCv2 master";
constexpr char kHostKeyDomain[] = "CRSCv2 host->phone key";
constexpr char kPhoneKeyDomain[] = "CRSCv2 phone->host key";
constexpr char kHostNonceDomain[] = "CRSCv2 host->phone nonce";
constexpr char kPhoneNonceDomain[] = "CRSCv2 phone->host nonce";
constexpr char kFinishedDomain[] = "CRSCv2 finished";
constexpr std::uint64_t kTicketClockSkewMs = 5ull * 60ull * 1000ull;
constexpr std::uint64_t kMaxLocalTicketLifetimeMs = 60ull * 60ull * 1000ull;

bool equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t size) noexcept
{
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < size; ++i)
    diff |= a[i] ^ b[i];
  return diff == 0;
}
void sequence_le(std::uint64_t sequence, std::array<std::uint8_t, 8>& out) noexcept
{
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<std::uint8_t>(sequence >> (8 * i));
}
bool derive(std::uint8_t* output, std::size_t output_size, const char* domain, std::size_t domain_size, const std::array<std::uint8_t, 64>& master, const std::array<std::uint8_t, kTicketUnsignedBytes>& ticket) noexcept
{
  // Every derivation input is small and fixed-size. Keeping it on the stack
  // preserves the Status/noexcept contract even under memory pressure.
  std::array<std::uint8_t, 192> message{};
  if (domain_size + ticket.size() > message.size())
  {
    crypto_wipe(output, output_size);
    return false;
  }
  std::memcpy(message.data(), domain, domain_size);
  std::memcpy(message.data() + domain_size, ticket.data(), ticket.size());
  crypto_blake2b_keyed(output, output_size, master.data(), master.size(),
                       message.data(), domain_size + ticket.size());
  crypto_wipe(message.data(), message.size());
  return true;
}

CR_NOINLINE Status validate_local_ticket(const TicketView& view, StreamKind expected_stream, std::uint64_t now_ms) noexcept
{
  if (view.ticket.stream != expected_stream)
    return Status::failure(Error::InvalidField);
  if (view.ticket.expires_at_ms - view.ticket.issued_at_ms > kMaxLocalTicketLifetimeMs)
    return Status::failure(Error::TicketLifetimeExceeded);
  if (now_ms < view.ticket.issued_at_ms && view.ticket.issued_at_ms - now_ms > kTicketClockSkewMs)
  {
    return Status::failure(Error::TicketNotYetValid);
  }
  if (now_ms > view.ticket.expires_at_ms && now_ms - view.ticket.expires_at_ms > kTicketClockSkewMs)
  {
    return Status::failure(Error::ExpiredTicket);
  }
  return {};
}

CR_NOINLINE Status verify_peer_key_binding_marker(const std::array<std::uint8_t, kKeyBytes>& local_public, const std::array<std::uint8_t, kKeyBytes>& expected_local) noexcept
{
  if (!equal(local_public.data(), expected_local.data(), local_public.size()))
  {
    return Status::failure(Error::AuthenticationFailed);
  }
  return {};
}

CR_NOINLINE Status verify_shared_secret_marker(const std::array<std::uint8_t, kKeyBytes>& shared) noexcept
{
  std::array<std::uint8_t, kKeyBytes> zero{};
  if (crypto_verify32(shared.data(), zero.data()) == 0)
  {
    return Status::failure(Error::CryptoFailure);
  }
  return {};
}

CR_NOINLINE Status verify_finished_marker(const std::vector<std::uint8_t>& plaintext, const std::array<std::uint8_t, 32>& expected) noexcept
{
  if (plaintext.size() != expected.size() || crypto_verify32(plaintext.data(), expected.data()) != 0)
  {
    return Status::failure(Error::FinishedMismatch);
  }
  return {};
}

void wipe_and_clear(std::vector<std::uint8_t>& bytes) noexcept
{
  if (!bytes.empty())
    crypto_wipe(bytes.data(), bytes.size());
  bytes.clear();
}
} // namespace

SecureSession::~SecureSession()
{
  wipe();
}

Status SecureSession::initialize(Role role, Bytes local_private, Bytes ticket_bytes, StreamKind expected_stream, std::uint64_t now_ms) noexcept
{
  wipe();
  if ((role != Role::Host && role != Role::Phone) ||
      !local_private.valid() || local_private.size != kKeyBytes)
  {
    return Status::failure(Error::InvalidField);
  }
  TicketView view;
  if (Status status = parse_ticket(ticket_bytes, view); !status)
    return status;

  if (Status status = validate_local_ticket(view, expected_stream, now_ms); !status)
  {
    return status;
  }

  std::array<std::uint8_t, kKeyBytes> local_public{};
  crypto_x25519_public_key(local_public.data(), local_private.data);
  const auto& expected_local = role == Role::Host ? view.ticket.host_ephemeral_public : view.ticket.phone_ephemeral_public;
  const auto& peer_public = role == Role::Host ? view.ticket.phone_ephemeral_public : view.ticket.host_ephemeral_public;
  if (Status status = verify_peer_key_binding_marker(local_public, expected_local); !status)
  {
    crypto_wipe(local_public.data(), local_public.size());
    return status;
  }
  std::array<std::uint8_t, kKeyBytes> shared{};
  crypto_x25519(shared.data(), local_private.data, peer_public.data());
  if (Status status = verify_shared_secret_marker(shared); !status)
  {
    crypto_wipe(local_public.data(), local_public.size());
    crypto_wipe(shared.data(), shared.size());
    return status;
  }
  if (Status status = verify_local_ticket(view, bytes_of(shared)); !status)
  {
    crypto_wipe(local_public.data(), local_public.size());
    crypto_wipe(shared.data(), shared.size());
    return status;
  }
  role_ = role;
  if (Status status = derive_material(view.unsigned_bytes, bytes_of(shared)); !status)
  {
    crypto_wipe(local_public.data(), local_public.size());
    crypto_wipe(shared.data(), shared.size());
    wipe();
    return status;
  }
  session_id_ = view.ticket.session_id;
  initialized_ = true;
  crypto_wipe(local_public.data(), local_public.size());
  crypto_wipe(shared.data(), shared.size());
  return {};
}

Status SecureSession::derive_material(Bytes ticket_unsigned, Bytes shared) noexcept
{
  if (!ticket_unsigned.valid() || !shared.valid() ||
      ticket_unsigned.size != ticket_unsigned_.size() || shared.size != kKeyBytes)
    return Status::failure(Error::InvalidField);
  std::memcpy(ticket_unsigned_.data(), ticket_unsigned.data, ticket_unsigned.size);
  std::array<std::uint8_t, sizeof(kMasterDomain) + kTicketUnsignedBytes + kKeyBytes> input{};
  std::size_t pos = 0;
  std::memcpy(input.data() + pos, kMasterDomain, sizeof(kMasterDomain));
  pos += sizeof(kMasterDomain);
  std::memcpy(input.data() + pos, ticket_unsigned.data, ticket_unsigned.size);
  pos += ticket_unsigned.size;
  std::memcpy(input.data() + pos, shared.data, shared.size);
  pos += shared.size;
  crypto_blake2b(master_.data(), master_.size(), input.data(), pos);
  crypto_wipe(input.data(), input.size());
  if (!derive(send_key_.data(), send_key_.size(), role_ == Role::Host ? kHostKeyDomain : kPhoneKeyDomain, role_ == Role::Host ? sizeof(kHostKeyDomain) : sizeof(kPhoneKeyDomain), master_, ticket_unsigned_) ||
      !derive(receive_key_.data(), receive_key_.size(), role_ == Role::Host ? kPhoneKeyDomain : kHostKeyDomain, role_ == Role::Host ? sizeof(kPhoneKeyDomain) : sizeof(kHostKeyDomain), master_, ticket_unsigned_) ||
      !derive(send_nonce_base_.data(), send_nonce_base_.size(), role_ == Role::Host ? kHostNonceDomain : kPhoneNonceDomain, role_ == Role::Host ? sizeof(kHostNonceDomain) : sizeof(kPhoneNonceDomain), master_, ticket_unsigned_) ||
      !derive(receive_nonce_base_.data(), receive_nonce_base_.size(), role_ == Role::Host ? kPhoneNonceDomain : kHostNonceDomain, role_ == Role::Host ? sizeof(kPhoneNonceDomain) : sizeof(kHostNonceDomain), master_, ticket_unsigned_))
  {
    wipe();
    return Status::failure(Error::CryptoFailure);
  }
  return {};
}

Status SecureSession::nonce_for(const std::array<std::uint8_t, kNonceBytes>& base, std::uint64_t sequence, std::array<std::uint8_t, kNonceBytes>& nonce) const noexcept
{
  std::array<std::uint8_t, 8> seq{};
  sequence_le(sequence, seq);
  crypto_blake2b_keyed(nonce.data(), nonce.size(), base.data(), base.size(), seq.data(), seq.size());
  crypto_wipe(seq.data(), seq.size());
  return {};
}

Status SecureSession::seal(RecordType type, Bytes plaintext, std::vector<std::uint8_t>& record) noexcept try
{
  if (!initialized_ || !plaintext.valid() || plaintext.size > kMaxCiphertextBytes ||
      (type == RecordType::Application && !established()))
  {
    record.clear();
    return Status::failure(Error::InvalidState);
  }
  if (send_sequence_ == UINT64_MAX)
  {
    record.clear();
    return Status::failure(Error::SequenceTooOld);
  }
  RecordHeader header{type, 0, session_id_, send_sequence_, static_cast<std::uint32_t>(plaintext.size)};
  std::vector<std::uint8_t> encoded;
  if (Status status = encode_record_header(header, encoded); !status)
  {
    record.clear();
    return status;
  }
  encoded.resize(kRecordHeaderBytes + plaintext.size + kTagBytes);
  std::array<std::uint8_t, kNonceBytes> nonce{};
  if (Status status = nonce_for(send_nonce_base_, header.sequence, nonce); !status)
  {
    record.clear();
    return status;
  }
  auto* ciphertext = encoded.data() + kRecordHeaderBytes;
  crypto_aead_lock(ciphertext, ciphertext + plaintext.size, send_key_.data(), nonce.data(),
                   encoded.data(), kRecordHeaderBytes, plaintext.data, plaintext.size);
  crypto_wipe(nonce.data(), nonce.size());
  record.swap(encoded);
  ++send_sequence_;
  return {};
}
catch (const std::bad_alloc&)
{
  record.clear();
  return Status::failure(Error::ResourceExhausted);
}

Status SecureSession::open(Bytes encoded, std::vector<std::uint8_t>& plaintext, RecordType& type) noexcept try
{
  const auto reject = [&plaintext](Status status)
  {
    wipe_and_clear(plaintext);
    return status;
  };
  if (!initialized_)
    return reject(Status::failure(Error::InvalidState));
  RecordView record;
  if (Status status = parse_record(encoded, record); !status)
    return reject(status);
  if (!equal(record.header.session_id.data(), session_id_.data(), session_id_.size()))
    return reject(Status::failure(Error::AuthenticationFailed));
  if (record.header.type == RecordType::Application && !established())
    return reject(Status::failure(Error::InvalidState));
  if (receive_initialized_ && record.header.sequence <= receive_highest_)
  {
    const std::uint64_t distance = receive_highest_ - record.header.sequence;
    if (distance >= 64)
      return reject(Status::failure(Error::SequenceTooOld));
    if ((receive_seen_ & (std::uint64_t{1} << distance)) != 0)
      return reject(Status::failure(Error::ReplayDetected));
  }
  std::vector<std::uint8_t> decoded(record.ciphertext.size);
  std::array<std::uint8_t, kNonceBytes> nonce{};
  if (Status status = nonce_for(receive_nonce_base_, record.header.sequence, nonce); !status)
    return reject(status);
  const int authenticated = crypto_aead_unlock(decoded.data(), record.tag.data,
      receive_key_.data(), nonce.data(), encoded.data, kRecordHeaderBytes,
      record.ciphertext.data, record.ciphertext.size);
  crypto_wipe(nonce.data(), nonce.size());
  if (authenticated != 0)
    return reject(Status::failure(Error::AuthenticationFailed));

  if (record.header.type == RecordType::Finished)
  {
    std::array<std::uint8_t, 32> expected{};
    const Status status = finished(expected);
    const Status verified = status ? verify_finished_marker(decoded, expected) : status;
    crypto_wipe(expected.data(), expected.size());
    if (!verified)
    {
      wipe_and_clear(decoded);
      return reject(verified);
    }
    received_finished_ = true;
  }
  if (!receive_initialized_)
  {
    receive_initialized_ = true;
    receive_highest_ = record.header.sequence;
    receive_seen_ = 1;
  }
  else if (record.header.sequence > receive_highest_)
  {
    const auto distance = record.header.sequence - receive_highest_;
    receive_seen_ = distance >= 64 ? 1 : (receive_seen_ << distance) | 1;
    receive_highest_ = record.header.sequence;
  }
  else
    receive_seen_ |= std::uint64_t{1} << (receive_highest_ - record.header.sequence);
  type = record.header.type;
  wipe_and_clear(plaintext);
  plaintext.swap(decoded);
  return {};
}
catch (const std::bad_alloc&)
{
  wipe_and_clear(plaintext);
  return Status::failure(Error::ResourceExhausted);
}

Status SecureSession::finished(std::array<std::uint8_t, 32>& output) const noexcept
{
  if (!initialized_)
    return Status::failure(Error::InvalidState);
  std::array<std::uint8_t, sizeof(kFinishedDomain) + kTicketUnsignedBytes> message{};
  std::memcpy(message.data(), kFinishedDomain, sizeof(kFinishedDomain));
  std::memcpy(message.data() + sizeof(kFinishedDomain), ticket_unsigned_.data(), ticket_unsigned_.size());
  crypto_blake2b_keyed(output.data(), output.size(), master_.data(), master_.size(), message.data(), message.size());
  crypto_wipe(message.data(), message.size());
  return {};
}

CR_NOINLINE Status SecureSession::make_finished(std::vector<std::uint8_t>& record) noexcept
{
  if (!initialized_ || sent_finished_)
  {
    record.clear();
    return Status::failure(Error::InvalidState);
  }
  std::array<std::uint8_t, 32> digest{};
  if (Status status = finished(digest); !status)
    return status;
  const Status status = seal(RecordType::Finished, bytes_of(digest), record);
  crypto_wipe(digest.data(), digest.size());
  if (status)
    sent_finished_ = true;
  return status;
}

void SecureSession::wipe() noexcept
{
  crypto_wipe(master_.data(), master_.size());
  crypto_wipe(send_key_.data(), send_key_.size());
  crypto_wipe(receive_key_.data(), receive_key_.size());
  crypto_wipe(send_nonce_base_.data(), send_nonce_base_.size());
  crypto_wipe(receive_nonce_base_.data(), receive_nonce_base_.size());
  crypto_wipe(ticket_unsigned_.data(), ticket_unsigned_.size());
  initialized_ = sent_finished_ = received_finished_ = receive_initialized_ = false;
  send_sequence_ = receive_highest_ = receive_seen_ = 0;
}

} // namespace cr::secure_channel::v2

#undef CR_NOINLINE
