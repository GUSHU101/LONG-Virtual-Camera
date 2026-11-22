#pragma once

#include "V2Protocol.h"

namespace cr::secure_channel::v2
{

// The development channel has no remote identity service. Instead, both
// endpoints prove possession of their ephemeral X25519 keys by authenticating
// the canonical ticket with the derived shared secret. This protects the
// complete handshake transcript from tampering and rejects the historical
// all-zero placeholder signature.
// Signing serializes the canonical ticket and can allocate. Keep it throwable
// so an allocation failure is handled by the host worker instead of invoking
// std::terminate through an incorrect noexcept contract.
[[nodiscard]] Status sign_local_ticket(Ticket& ticket, Bytes shared_secret);
[[nodiscard]] Status verify_local_ticket(const TicketView& ticket, Bytes shared_secret) noexcept;

} // namespace cr::secure_channel::v2
