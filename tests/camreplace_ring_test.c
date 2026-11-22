#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../engines/camreplace/android/feed/include/cr_feed.h"
#include "../engines/camreplace/android/feed/src/shm.h"

int main(void)
{
  cr_feed_header header;
  memset(&header, 0, sizeof(header));
  header.magic = CR_FEED_MAGIC;
  header.version = CR_FEED_SHM_VERSION;
  header.num_slots = CR_FEED_NUM_SLOTS;
  assert(sizeof(header) == 64);
  assert(CR_FEED_MAX_WIDTH >= 1920u);
  assert(CR_FEED_MAX_HEIGHT >= 1920u);
  assert((uint64_t) 1080u * 1920u * 3u / 2u <= CR_FEED_MAX_NV21_BYTES);

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
  return 0;
}
