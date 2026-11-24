#pragma once

// Authenticated TCP streaming client for the one H.264 video lane and the
// explicit opt-in PCM audio lane, both carried through adb loopback forwards.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "secure_channel/SecureSession.h"

namespace cr::transport
{

class TcpFeedClient
{
public:
  // Magic constants matching the Android receivers; exposed so tests can
  // validate headers without peeking into the private cpp.
  //   kMagicH264  — H.264 Annex-B stream (tcp_h264_feed.cpp)
  static constexpr uint32_t kMagicH264 = 0x32484243u; // 'CRH2' LE
  static constexpr uint32_t kMagicPcm = 0x55415243u;  // 'CRAU' LE — PCM stream

  // Connect to 127.0.0.1:port. Assumes `adb forward tcp:port tcp:port`
  // has already been set up by the caller. Returns true on success.
  bool open(int port, cr::secure_channel::v2::StreamKind stream_kind);

  // --- H.264 Annex-B protocol ------------------------------------------
  // Stream header (magic CRH2, width, height, fps). Once per session.
  bool send_h264_header(int width, int height, int fps);
  // One encoded chunk:  [u32 len][u32 flags][i64 pts_us][len bytes]
  //   flags bit 0 → keyframe
  bool send_h264_chunk(const void* data, std::size_t bytes, int64_t pts_us, bool is_keyframe);

  // --- PCM audio protocol ----------------------------------------------
  // Header (once per session): magic CRAU, sample_rate, channels, bps.
  // Then a continuous stream of S16LE samples (no per-frame framing) —
  // the phone receiver memcpy's bytes straight into a ring buffer.
  bool send_pcm_header(uint32_t sample_rate, uint16_t channels, uint16_t bytes_per_sample);
  bool send_pcm(const void* data, std::size_t bytes);

  void close();
  bool is_open() const noexcept;

  ~TcpFeedClient();

private:
  bool send_application_(const void* data, std::size_t bytes);
  bool flush_pcm_blocks_();
  bool send_all_(const void* data, std::size_t bytes) noexcept;

  // SOCKET is an unsigned pointer-sized handle on Windows; we store it as
  // uintptr_t to avoid dragging <winsock2.h> into the public header.
  std::uintptr_t sock_ = ~std::uintptr_t{0};
  cr::secure_channel::v2::SecureSession secure_{};
  std::vector<std::uint8_t> pcm_pending_{};
  std::size_t pcm_offset_{0};
};

} // namespace cr::transport
