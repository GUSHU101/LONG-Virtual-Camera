#include "RtmpChunks.h"

#include <cassert>
#include <cstring>
#include <initializer_list>

using cr::feed::RtmpChunkReader;
using cr::feed::RtmpChunkResult;
using cr::feed::RtmpMessage;

static RtmpChunkResult read(RtmpChunkReader& reader, RtmpMessage& message,
                            std::initializer_list<std::uint8_t> input)
{
  auto cursor = input.begin();
  const auto result = reader.read([&](std::uint8_t* dst, std::size_t count) {
    if (count > static_cast<std::size_t>(input.end() - cursor))
      return false;
    std::memcpy(dst, cursor, count);
    cursor += count;
    return true;
  }, message);
  if (result == RtmpChunkResult::Message || result == RtmpChunkResult::Incomplete)
    assert(cursor == input.end());
  return result;
}

int main()
{
  RtmpMessage message;
  {
    RtmpChunkReader reader;
    assert(!reader.set_chunk_size(0) && !reader.set_chunk_size(1048577));
    assert(reader.set_chunk_size(4));
    // Video is interrupted by a control message on a different chunk stream.
    assert(read(reader, message, {4, 0,0,5, 0,0,6, 9, 1,0,0,0, 1,2,3,4})
           == RtmpChunkResult::Incomplete);
    assert(read(reader, message, {2, 0,0,0, 0,0,4, 2, 0,0,0,0, 0,0,0,4})
           == RtmpChunkResult::Message);
    assert(message.type == 2 && message.stream_id == 0 && message.body[3] == 4);
    reader.abort(4);
    assert(read(reader, message, {4, 0,0,9, 0,0,2, 9, 1,0,0,0, 7,8})
           == RtmpChunkResult::Message);
    assert(message.timestamp == 9 && message.stream_id == 1 &&
           message.body.size() == 2 && message.body[0] == 7);
  }
  {
    RtmpChunkReader reader;
    assert(read(reader, message, {4, 0,0,30, 0,0,1, 9, 1,0,0,0, 1})
           == RtmpChunkResult::Message);
    assert(read(reader, message, {0xc4, 2}) == RtmpChunkResult::Message);
    assert(message.timestamp == 60);
    assert(read(reader, message, {0x44, 0,0,10, 0,0,2, 9, 3,4})
           == RtmpChunkResult::Message);
    assert(message.timestamp == 70);
    assert(read(reader, message, {0x84, 0,0,5, 5,6}) == RtmpChunkResult::Message);
    assert(message.timestamp == 75);
    assert(read(reader, message, {0xc4, 7,8}) == RtmpChunkResult::Message);
    assert(message.timestamp == 80);
  }
  {
    RtmpChunkReader reader;
    assert(reader.set_chunk_size(2));
    // Extended timestamp on a continuation of a type-0 header.
    assert(read(reader, message, {4, 255,255,255, 0,0,3, 9, 1,0,0,0,
                                  1,0,0,0, 1,2}) == RtmpChunkResult::Incomplete);
    assert(read(reader, message, {0xc4, 1,0,0,0, 3}) == RtmpChunkResult::Message);
    assert(message.timestamp == 0x01000000 && message.body[2] == 3);
    assert(read(reader, message, {0x45}) == RtmpChunkResult::Invalid);
  }
  {
    RtmpChunkReader reader;
    assert(reader.set_chunk_size(1));
    constexpr std::uint8_t channel_ids[] = {4, 5, 6};
    for (std::uint8_t id : channel_ids)
      assert(read(reader, message, {id, 0,0,0, 128,0,0, 9, 1,0,0,0, 0})
             == RtmpChunkResult::Incomplete);
    assert(read(reader, message, {7, 0,0,0, 0,0,1, 9, 1,0,0,0, 0})
           == RtmpChunkResult::Invalid);
  }
}
