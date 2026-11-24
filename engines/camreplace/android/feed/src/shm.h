/*
 * Назначение файла:
 *   ABI shared memory /data/cr/feed для video/photo pipeline. cr_feed_proc
 *   пишет NV21 кадры в triple-buffered slots, camhook читает последний
 *   опубликованный slot внутри cameraserver и подменяет YUV/BLOB buffers.
 *
 * ABI/совместимость:
 *   cr_feed_header обязан оставаться 64 байта. Wire/shm формат v2 использует
 *   бывший reserve для per-slot reader/writer ownership и оставляет
 *   фиксированный per-slot reserve под 1920x1080 или 1080x1920 NV21.
 *
 * Pixel/Android ограничения:
 *   Pixel 6-10 могут отдавать preview/photo pipelines выше 1080p, но текущий
 *   v2 контракт не меняет размер slot на лету. Динамический slot sizing
 *   потребует нового shm version и отдельной миграции.
 */

#pragma once

// Shared-memory layout for /data/cr/feed.
// One header, followed by N equally-sized NV21 slots. Writers advance
// write_index after filling a slot; readers pick the latest committed slot
// via the index (typical single-producer / multiple-reader ring).
// Kept tiny and POD — readers in other processes (cameraserver hook,
// debugger) map the same struct without any C++ runtime.

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif

// Magic: 'CRFD' little-endian.
#define CR_FEED_MAGIC 0x44465243u

  typedef struct cr_feed_header
  {
    uint32_t magic;              // CR_FEED_MAGIC
    uint32_t version;            // CR_FEED_SHM_VERSION
    uint32_t width;              // pixels
    uint32_t height;             // pixels
    uint32_t stride;             // == width for NV21
    uint32_t slot_size;          // bytes per slot = width*height*3/2
    uint32_t num_slots;          // >= 1
    uint32_t format;             // 0 = NV21
    atomic_uint write_index;     // last slot a writer fully committed
    atomic_uint generation;      // bumped on any feed-shape change
    atomic_ullong frame_counter; // monotonic, ++'d per commit
    // Set READY only after SCT2 verification, Finished exchange and
    // authenticated input. Hooks gate replacement on it as a fail-closed
    // availability signal; shared memory is not a root security boundary.
    atomic_uint channel_state;
    // Zero = free, CR_FEED_SLOT_WRITER = producer owns it, otherwise the
    // value is the number of camera callbacks currently reading this slot.
    atomic_uint slot_state[3]; // keeps total size = 64 bytes
  } cr_feed_header;

  _Static_assert(sizeof(cr_feed_header) == 64, "header must stay 64 bytes");
  _Static_assert(offsetof(cr_feed_header, channel_state) == 48,
                 "host pass-through command depends on channel_state offset");

#ifndef CR_CHANNEL_STATE_NONE
#define CR_CHANNEL_STATE_NONE 0u
#define CR_CHANNEL_STATE_READY 1u
#endif

// Default path — /data/cr exists, u:object_r:system_data_file:s0 after the
// one-time setup we'll do in sprint 4. For sprint 3 the writer creates it.
#define CR_FEED_PATH "/data/cr/feed"

// Number of NV21 slots in the ring. Triple-buffered so the writer can
// memcpy a fresh frame into slot[(wi+1)%N] without ever racing the
// camera-server hook that's reading slot[wi]. Tearing was visible with
// the previous N=1 layout — half top old / half bottom new — even at
// ~3 MB/s sustained writes.
#define CR_FEED_NUM_SLOTS 3u

#define CR_FEED_SLOT_WRITER 0x80000000u

static inline int cr_feed_slot_try_read(cr_feed_header* h, uint32_t slot)
{
  if (!h || slot >= CR_FEED_NUM_SLOTS)
    return 0;
  unsigned value = atomic_load_explicit(&h->slot_state[slot], memory_order_acquire);
  for (unsigned attempt = 0; attempt < 8 && value < CR_FEED_SLOT_WRITER - 1u; ++attempt)
  {
    if (atomic_compare_exchange_weak_explicit(&h->slot_state[slot], &value, value + 1u,
                                              memory_order_acquire, memory_order_relaxed))
      return 1;
  }
  return 0;
}

static inline int cr_feed_slot_release_read(cr_feed_header* h, uint32_t slot)
{
  if (!h || slot >= CR_FEED_NUM_SLOTS)
    return 0;
  unsigned value = atomic_load_explicit(&h->slot_state[slot], memory_order_acquire);
  while (value > 0u && value < CR_FEED_SLOT_WRITER)
  {
    if (atomic_compare_exchange_weak_explicit(&h->slot_state[slot], &value,
                                              value - 1u, memory_order_release,
                                              memory_order_relaxed))
      return 1;
  }
  return 0;
}

// A replacement producer may inherit a shared file after its predecessor
// died while owning a writer token. It may reclaim only that exact token;
// low values are live reader pins in cameraserver and must survive reconnect.
static inline void cr_feed_recover_dead_writer(cr_feed_header* h)
{
  if (!h)
    return;
  for (uint32_t slot = 0; slot < CR_FEED_NUM_SLOTS; ++slot)
  {
    unsigned expected = CR_FEED_SLOT_WRITER;
    (void) atomic_compare_exchange_strong_explicit(
        &h->slot_state[slot], &expected, 0u, memory_order_acq_rel,
        memory_order_relaxed);
  }
}

// Ownership counters remain meaningful even after the previous producer
// revoked `magic` during pass-through. A new producer must therefore not
// clear reader pins merely because the channel is currently inactive: an
// already-running camera callback may still be finishing a copy/JPEG encode.
// Only an unknown ABI layout is safe to initialize from zero.
static inline int cr_feed_ownership_layout_valid(const cr_feed_header* h)
{
  return h &&
         __atomic_load_n(&h->version, __ATOMIC_ACQUIRE) ==
             CR_FEED_SHM_VERSION &&
         __atomic_load_n(&h->num_slots, __ATOMIC_ACQUIRE) ==
             CR_FEED_NUM_SLOTS;
}

static inline void cr_feed_reset_slot_states(cr_feed_header* h)
{
  if (!h)
    return;
  for (uint32_t slot = 0; slot < CR_FEED_NUM_SLOTS; ++slot)
    atomic_store_explicit(&h->slot_state[slot], 0u, memory_order_release);
}

static inline int cr_feed_claim_write(cr_feed_header* h, uint32_t current, uint32_t* claimed)
{
  if (!h || !claimed)
    return 0;
  unsigned n = __atomic_load_n(&h->num_slots, __ATOMIC_ACQUIRE);
  if (n == 0 || n > CR_FEED_NUM_SLOTS)
    return 0;
  current %= n;
  for (unsigned step = 1; step <= n; ++step)
  {
    const unsigned slot = (current + step) % n;
    if (n > 1 && slot == current)
      continue;
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(&h->slot_state[slot], &expected,
                                                CR_FEED_SLOT_WRITER,
                                                memory_order_acquire, memory_order_relaxed))
    {
      *claimed = slot;
      return 1;
    }
  }
  return 0;
}

static inline void cr_feed_abort_write(cr_feed_header* h, uint32_t slot)
{
  if (h && slot < CR_FEED_NUM_SLOTS)
    atomic_store_explicit(&h->slot_state[slot], 0u, memory_order_release);
}

static inline uint32_t cr_feed_next_generation(uint32_t previous)
{
  const uint32_t next = previous + 1u;
  return next == 0u ? 1u : next;
}

static inline uint64_t cr_feed_publish(cr_feed_header* h, uint32_t slot)
{
  // Keep the writer token until every publication field is final. Index must
  // precede sequence: a reader that acquires the new sequence then also sees
  // its index. A reader that saw the old sequence plus the new index either
  // fails to pin the writer-owned slot or acquires its final release and then
  // rejects the changed sequence during the post-pin recheck.
  uint64_t sequence = atomic_load_explicit(&h->frame_counter, memory_order_relaxed) + 1ull;
  if (sequence == 0ull)
    sequence = 1ull;
  atomic_store_explicit(&h->write_index, slot, memory_order_release);
  atomic_store_explicit(&h->frame_counter, sequence, memory_order_release);
  atomic_store_explicit(&h->slot_state[slot], 0u, memory_order_release);
  return sequence;
}

#define CR_FEED_MAX_WIDTH 1920u
#define CR_FEED_MAX_HEIGHT 1920u
#define CR_FEED_MAX_PIXELS (1920u * 1080u)

// Reserved per-slot size. Sized for 1920×1080 NV21 (~3 MB) which is the
// substituted-camera output we actually use; the file ftruncates up to
// `sizeof(header) + CR_FEED_NUM_SLOTS * CR_FEED_MAX_NV21_BYTES` (~9 MB
// total). Older builds used a single 18 MB slot; the bigger reserve is
// fine on every Android we target but most of it sat unused.
#define CR_FEED_MAX_NV21_BYTES ((uint32_t) (CR_FEED_MAX_PIXELS * 3u / 2u))

#ifdef __cplusplus
}
#endif
