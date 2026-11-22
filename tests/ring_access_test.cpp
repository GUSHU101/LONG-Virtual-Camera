#include <cassert>
#include <cstdint>
#include <memory>

#include "../android/src/common/ring_access.hpp"

using namespace recam;

static void pinned_slots_are_never_overwritten()
{
    ShmLane lane{};

    {
        RingPublication first(lane);
        assert(first.seq == 1);
        first.publish(10);
    }
    auto reader1 = std::make_unique<RingFrame>(lane);
    assert(reader1->seq == 1);

    {
        RingPublication second(lane);
        assert(second.seq == 2);
        assert(second.slot != reader1->slot);
        second.publish(20);
    }
    auto reader2 = std::make_unique<RingFrame>(lane);
    assert(reader2->seq == 2);

    {
        RingPublication third(lane);
        assert(third.seq == 3);
        assert(third.slot != reader1->slot);
        assert(third.slot != reader2->slot);
        third.publish(30);
    }
    auto reader3 = std::make_unique<RingFrame>(lane);
    assert(reader3->seq == 3);

    RingPublication blocked(lane);
    assert(blocked.seq == 0);
}

static void aborted_publication_releases_its_slot()
{
    ShmLane lane{};
    uint32_t chosen = 0;
    {
        RingPublication aborted(lane);
        assert(aborted.seq != 0);
        chosen = aborted.slot;
        assert(lane.slots[chosen] == kWriter);
    }
    assert(lane.slots[chosen] == 0);
}

static void recovery_only_clears_dead_writer_tokens()
{
    ShmHeader header{};
    header.lane[0].slots[0] = kWriter;
    header.lane[0].slotSeq[0] = 99;
    header.lane[0].slots[1] = 2;  // two live readers must survive recovery
    header.lane[1].access = kWriter + 1;
    header.lane[1].published = 7;
    header.lane[1].width = 640;

    recover_dead_producer(header);

    assert(header.lane[0].slots[0] == 0);
    assert(header.lane[0].slotSeq[0] == 0);
    assert(header.lane[0].slots[1] == 2);
    assert(header.lane[1].access == 0);
    assert(header.lane[1].published == 0);
    assert(header.lane[1].width == 0);
}

int main()
{
    pinned_slots_are_never_overwritten();
    aborted_publication_releases_its_slot();
    recovery_only_clears_dead_writer_tokens();
    return 0;
}
