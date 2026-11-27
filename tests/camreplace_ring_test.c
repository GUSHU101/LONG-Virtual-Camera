#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../engines/camreplace/android/feed/include/cr_feed.h"
#include "../engines/camreplace/android/feed/src/shm.h"
#include "../engines/camreplace/android/feed/src/audio_shm.h"

typedef struct publication_stress
{
  cr_feed_header header;
  atomic_ullong payload_sequence[CR_FEED_NUM_SLOTS];
  atomic_bool stop;
  atomic_bool failed;
} publication_stress;

static void* publish_frames(void* opaque)
{
  publication_stress* stress = (publication_stress*) opaque;
  uint32_t current = 0;
  for (uint64_t iteration = 0; iteration < 200000 &&
                               !atomic_load(&stress->failed);
       ++iteration)
  {
    uint32_t slot = 0;
    while (!cr_feed_claim_write(&stress->header, current, &slot))
    {
      if (atomic_load(&stress->failed))
        goto done;
      sched_yield();
    }
    uint64_t expected = atomic_load(&stress->header.frame_counter) + 1ull;
    if (expected == 0ull)
      expected = 1ull;
    atomic_store_explicit(&stress->payload_sequence[slot], expected,
                          memory_order_relaxed);
    assert(cr_feed_publish(&stress->header, slot) == expected);
    current = slot;
  }
done:
  atomic_store(&stress->stop, true);
  return NULL;
}

static void* verify_publications(void* opaque)
{
  publication_stress* stress = (publication_stress*) opaque;
  while (!atomic_load(&stress->stop) && !atomic_load(&stress->failed))
  {
    const uint64_t sequence = atomic_load_explicit(
        &stress->header.frame_counter, memory_order_acquire);
    const uint32_t slot = atomic_load_explicit(
        &stress->header.write_index, memory_order_acquire) % CR_FEED_NUM_SLOTS;
    if (sequence == 0 || !cr_feed_slot_try_read(&stress->header, slot))
      continue;

    const bool stable =
        atomic_load_explicit(&stress->header.frame_counter,
                             memory_order_acquire) == sequence &&
        atomic_load_explicit(&stress->header.write_index,
                             memory_order_acquire) % CR_FEED_NUM_SLOTS == slot;
    const uint64_t payload = atomic_load_explicit(
        &stress->payload_sequence[slot], memory_order_relaxed);
    if (stable && payload != sequence)
      atomic_store(&stress->failed, true);
    assert(cr_feed_slot_release_read(&stress->header, slot));
  }
  return NULL;
}

int main(void)
{
  cr_audio_header audio;
  memset(&audio, 0, sizeof(audio));
  assert(sizeof(audio) == 64);
  assert(cr_audio_next_generation(&audio) == 1u);
  atomic_store(&audio.generation, UINT32_MAX);
  assert(cr_audio_next_generation(&audio) == 1u);
  const uint32_t audio_write = cr_audio_begin_write(&audio);
  assert((audio_write & 1u) != 0u);
  assert(!cr_audio_write_sequence_stable(&audio, audio_write));
  cr_audio_end_write(&audio, audio_write);
  const uint32_t audio_stable = cr_audio_write_sequence(&audio);
  assert((audio_stable & 1u) == 0u);
  assert(cr_audio_write_sequence_stable(&audio, audio_stable));
  const uint32_t next_audio_write = cr_audio_begin_write(&audio);
  assert(!cr_audio_write_sequence_stable(&audio, audio_stable));
  cr_audio_end_write(&audio, next_audio_write);

  cr_feed_header header;
  memset(&header, 0, sizeof(header));
  header.magic = CR_FEED_MAGIC;
  header.version = CR_FEED_SHM_VERSION;
  header.num_slots = CR_FEED_NUM_SLOTS;
  assert(sizeof(header) == 64);
  assert(CR_FEED_MAX_WIDTH >= 3840u);
  assert(CR_FEED_MAX_HEIGHT >= 3840u);
  assert((uint64_t) 3840u * 2160u * 3u / 2u == CR_FEED_MAX_NV21_BYTES);
  assert(cr_feed_slot_offset(CR_FEED_FHD_SLOT_BYTES, 0) == sizeof(header));
  const size_t fhd_end = cr_feed_slot_offset(CR_FEED_FHD_SLOT_BYTES, 2) +
                         CR_FEED_FHD_SLOT_BYTES;
  assert(cr_feed_slot_offset(CR_FEED_FHD_SLOT_BYTES + 1, 0) == fhd_end);
  assert(cr_feed_slot_offset(CR_FEED_MAX_NV21_BYTES, 2) + CR_FEED_MAX_NV21_BYTES ==
         CR_FEED_MAP_RESERVE);
  header.version = CR_FEED_SHM_VERSION_FHD;
  assert(cr_feed_ownership_layout_valid(&header));
  header.version = CR_FEED_SHM_VERSION;

  uint32_t slot1 = 0;
  assert(cr_feed_claim_write(&header, 0, &slot1));
  assert(slot1 == 1);
  assert(!cr_feed_slot_try_read(&header, slot1));
  cr_feed_publish(&header, slot1);
  assert(cr_feed_slot_try_read(&header, slot1));

  uint32_t slot2 = 0;
  assert(cr_feed_claim_write(&header, slot1, &slot2));
  assert(slot2 == 2);
  cr_feed_publish(&header, slot2);
  assert(cr_feed_slot_try_read(&header, slot2));

  uint32_t slot0 = 99;
  assert(cr_feed_claim_write(&header, slot2, &slot0));
  assert(slot0 == 0);
  cr_feed_publish(&header, slot0);
  assert(cr_feed_slot_try_read(&header, slot0));

  uint32_t blocked = 99;
  assert(!cr_feed_claim_write(&header, slot0, &blocked));

  cr_feed_slot_release_read(&header, slot1);
  assert(cr_feed_claim_write(&header, slot0, &blocked));
  assert(blocked == slot1);
  cr_feed_abort_write(&header, blocked);
  cr_feed_slot_release_read(&header, slot2);
  cr_feed_slot_release_read(&header, slot0);

  // Reconnect recovery may clear only an abandoned writer token. A reader
  // pinned by cameraserver must remain visible to the next producer.
  memset(&header.slot_state, 0, sizeof(header.slot_state));
  atomic_store(&header.slot_state[0], 1u);
  atomic_store(&header.slot_state[1], CR_FEED_SLOT_WRITER);
  atomic_store(&header.slot_state[2], 2u);
  cr_feed_recover_dead_writer(&header);
  assert(atomic_load(&header.slot_state[0]) == 1u);
  assert(atomic_load(&header.slot_state[1]) == 0u);
  assert(atomic_load(&header.slot_state[2]) == 2u);

  uint32_t reconnect_slot = 99;
  assert(cr_feed_claim_write(&header, 2, &reconnect_slot));
  assert(reconnect_slot == 1u);
  cr_feed_abort_write(&header, reconnect_slot);
  assert(cr_feed_slot_release_read(&header, 0));
  assert(!cr_feed_slot_release_read(&header, 0)); // no unsigned underflow
  assert(atomic_load(&header.slot_state[0]) == 0u);
  assert(cr_feed_slot_release_read(&header, 2));
  assert(cr_feed_slot_release_read(&header, 2));

  // Four simultaneous callbacks may pin the same published frame. A writer
  // cannot claim it until every guard has left.
  memset(&header.slot_state, 0, sizeof(header.slot_state));
  for (int i = 0; i < 4; ++i)
    assert(cr_feed_slot_try_read(&header, 1));
  assert(atomic_load(&header.slot_state[1]) == 4u);
  for (int i = 0; i < 4; ++i)
    assert(cr_feed_slot_release_read(&header, 1));
  assert(atomic_load(&header.slot_state[1]) == 0u);

  assert(cr_feed_next_generation(UINT32_MAX) == 1u);
  atomic_store(&header.frame_counter, UINT64_MAX);
  atomic_store(&header.slot_state[1], CR_FEED_SLOT_WRITER);
  assert(cr_feed_publish(&header, 1) == 1u);
  assert(atomic_load(&header.frame_counter) == 1u);

  // Deterministic 100k-operation stress: readers are never decremented below
  // zero and a claimed writer never overlaps a reader count.
  memset(&header.slot_state, 0, sizeof(header.slot_state));
  uint32_t rng = 0x13579bdu;
  uint32_t held[CR_FEED_NUM_SLOTS] = {0};
  uint32_t current = 0;
  for (int iteration = 0; iteration < 100000; ++iteration)
  {
    rng = rng * 1664525u + 1013904223u;
    const uint32_t slot = (rng >> 8) % CR_FEED_NUM_SLOTS;
    if ((rng & 3u) == 0u)
    {
      if (cr_feed_slot_try_read(&header, slot))
        ++held[slot];
    }
    else if ((rng & 3u) == 1u && held[slot] != 0u)
    {
      assert(cr_feed_slot_release_read(&header, slot));
      --held[slot];
    }
    else
    {
      uint32_t claim = 99;
      if (cr_feed_claim_write(&header, current, &claim))
      {
        assert(held[claim] == 0u);
        cr_feed_publish(&header, claim);
        current = claim;
      }
    }
    for (uint32_t i = 0; i < CR_FEED_NUM_SLOTS; ++i)
      assert(atomic_load(&header.slot_state[i]) == held[i]);
  }
  for (uint32_t slot = 0; slot < CR_FEED_NUM_SLOTS; ++slot)
    while (held[slot] != 0u)
    {
      assert(cr_feed_slot_release_read(&header, slot));
      --held[slot];
    }

  // Concurrent publication proof: any snapshot that survives the same
  // post-pin validation used by camhook must pair the slot payload with its
  // published frame sequence. This catches cross-atomic order regressions.
  publication_stress stress;
  memset(&stress, 0, sizeof(stress));
  stress.header.num_slots = CR_FEED_NUM_SLOTS;
  pthread_t writer;
  pthread_t readers[4];
  assert(pthread_create(&writer, NULL, publish_frames, &stress) == 0);
  for (size_t i = 0; i < 4; ++i)
    assert(pthread_create(&readers[i], NULL, verify_publications, &stress) == 0);
  assert(pthread_join(writer, NULL) == 0);
  for (size_t i = 0; i < 4; ++i)
    assert(pthread_join(readers[i], NULL) == 0);
  assert(!atomic_load(&stress.failed));
  return 0;
}
