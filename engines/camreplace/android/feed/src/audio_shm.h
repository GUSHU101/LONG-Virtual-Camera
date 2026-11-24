/*
 * Назначение файла:
 *   ABI shared memory /data/cr/audio для sound replacement. tcp_pcm feed
 *   пишет S16LE PCM в кольцевой buffer, audhook внутри audioserver читает
 *   ring и подменяет результат StreamInHal или tinyalsa read.
 *
 * ABI/совместимость:
 *   cr_audio_header обязан оставаться 64 байта. Формат v1 хранит один
 *   producer ring на 2 MiB; per-consumer позиция чтения живёт только в
 *   audhook process memory и не меняет wire/shm layout.
 *
 * Pixel/Android ограничения:
 *   AIDL/HIDL AudioRecord paths покрываются audioserver hook. Sound Trigger,
 *   AOC/hotword и Now Playing могут обходить обычный Audio HAL read path и
 *   не являются гарантированной зоной покрытия этого shm ABI.
 */

#pragma once

// Shared-memory layout for /data/cr/audio.
// Continuous PCM ring buffer. The host pump writes a stream of S16LE samples
// (any rate / channel count) into the ring; the audhook inside audioserver
// consumes from the ring at the cadence the audio HAL polls it. No slots,
// no fixed frame size — just a circular byte buffer with a monotonic
// write pointer so the consumer can track its own position and detect
// underruns.

#include <stdatomic.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif

// Magic: 'CRAU' little-endian.
#define CR_AUDIO_MAGIC 0x55415243u

#define CR_AUDIO_VERSION 1u

#ifndef CR_CHANNEL_STATE_NONE
#define CR_CHANNEL_STATE_NONE 0u
#define CR_CHANNEL_STATE_READY 1u
#endif

// Reserved on-disk size for /data/cr/audio: header + 2 MiB of ring. At
// 48 kHz mono S16LE that's ~10 s of audio buffer, plenty to absorb any
// network jitter on the adb tunnel without wrapping. The ring is kept the
// same regardless of actual sample rate / channels.
#define CR_AUDIO_RING_BYTES (2u * 1024u * 1024u)

  typedef struct cr_audio_header
  {
    uint32_t magic;            // CR_AUDIO_MAGIC
    uint32_t version;          // CR_AUDIO_VERSION
    uint32_t sample_rate;      // e.g. 48000
    uint32_t channels;         // 1 or 2
    uint32_t bytes_per_sample; // always 2 (S16LE)
    uint32_t ring_bytes;       // == CR_AUDIO_RING_BYTES
    atomic_ullong write_pos;   // monotonic byte count produced
    atomic_uint generation;    // bumped when sample_rate / channels change
    // Secure-channel state; READY is published only by an authenticated
    // SCT2 session. Hook gating is fail-closed, not a root boundary.
    atomic_uint channel_state;
    // reserved[0] is a writer seqlock: odd while ring bytes are changing,
    // even after write_pos and the complete PCM chunk are published.
    uint32_t reserved[5]; // pad to 64 bytes
  } cr_audio_header;

  _Static_assert(sizeof(cr_audio_header) == 64, "cr_audio_header must stay 64 bytes");

  // Generation zero is reserved by the hook as "nothing observed yet".
  // Writers hold an exclusive file lease, so a load followed by a store is
  // sufficient and, unlike fetch_add, cannot wrap a live generation to zero.
  static inline uint32_t cr_audio_next_generation(cr_audio_header* h)
  {
    const uint32_t current =
        atomic_load_explicit(&h->generation, memory_order_relaxed);
    return current == UINT32_MAX ? 1u : current + 1u;
  }

  static inline uint32_t cr_audio_begin_write(cr_audio_header* h)
  {
    const uint32_t current =
        __atomic_load_n(&h->reserved[0], __ATOMIC_RELAXED);
    const uint32_t token = current | 1u;
    __atomic_store_n(&h->reserved[0], token, __ATOMIC_RELEASE);
    return token;
  }

  static inline void cr_audio_end_write(cr_audio_header* h, uint32_t token)
  {
    __atomic_store_n(&h->reserved[0], token + 1u, __ATOMIC_RELEASE);
  }

  static inline uint32_t cr_audio_write_sequence(const cr_audio_header* h)
  {
    return __atomic_load_n(&h->reserved[0], __ATOMIC_ACQUIRE);
  }

  static inline int cr_audio_write_sequence_stable(const cr_audio_header* h,
                                                   uint32_t observed)
  {
    return (observed & 1u) == 0u &&
           cr_audio_write_sequence(h) == observed;
  }

#define CR_AUDIO_PATH "/data/cr/audio"

#ifdef __cplusplus
}
#endif
