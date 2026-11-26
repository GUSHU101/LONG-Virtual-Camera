#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace cr::feed
{

enum class RtmpChunkResult { Incomplete, Message, Closed, Invalid };

struct RtmpMessage
{
  std::uint32_t timestamp = 0;
  std::uint32_t stream_id = 0;
  std::uint8_t type = 0;
  std::span<std::uint8_t> body;
};

class RtmpChunkReader
{
public:
  RtmpChunkReader() = default;
  RtmpChunkReader(const RtmpChunkReader&) = delete;
  RtmpChunkReader& operator=(const RtmpChunkReader&) = delete;

  bool set_chunk_size(std::uint32_t size)
  {
    if (size == 0 || size > kMaxChunkBytes)
      return false;
    chunk_size_ = size;
    return true;
  }

  void abort(std::uint32_t chunk_stream_id)
  {
    auto it = channels_.find(chunk_stream_id);
    if (it != channels_.end() && &it->second != completed_)
      recycle(it->second);
  }

  // Message payload remains available until the next read().
  template <class Read>
  RtmpChunkResult read(Read&& receive, RtmpMessage& message)
  {
    message = {};
    if (completed_)
    {
      recycle(*completed_);
      completed_ = nullptr;
    }

    std::uint8_t basic = 0;
    if (!receive(&basic, 1))
      return RtmpChunkResult::Closed;
    const auto format = basic >> 6;
    std::uint32_t channel_id = basic & 0x3f;
    if (channel_id < 2)
    {
      std::uint8_t extension[2]{};
      if (!receive(extension, channel_id + 1))
        return RtmpChunkResult::Closed;
      channel_id = 64u + extension[0] + (std::uint32_t{extension[1]} << 8);
    }

    auto it = channels_.find(channel_id);
    if (it == channels_.end())
    {
      if (format != 0 || channels_.size() >= kMaxChannels)
        return RtmpChunkResult::Invalid;
      it = channels_.try_emplace(channel_id).first;
    }
    Channel& channel = it->second;
    const bool first = channel.body.empty();
    if (!first && format != 3)
      return RtmpChunkResult::Invalid;

    if (format != 3)
    {
      constexpr std::size_t header_sizes[] = {11, 7, 3};
      std::uint8_t header[11]{};
      if (!receive(header, header_sizes[format]))
        return RtmpChunkResult::Closed;
      if (format != 2)
      {
        channel.length = be24(header + 3);
        channel.type = header[6];
      }
      if (format == 0)
        channel.stream_id = std::uint32_t{header[7]} | (std::uint32_t{header[8]} << 8) |
                            (std::uint32_t{header[9]} << 16) | (std::uint32_t{header[10]} << 24);
      std::uint32_t time = be24(header);
      channel.extended = time == 0xffffffu;
      if (channel.extended && !read_extended(receive, time))
        return RtmpChunkResult::Closed;
      channel.delta = time;
      channel.timestamp = format == 0 ? time : channel.timestamp + time;
    }
    else
    {
      if (channel.extended)
      {
        std::uint32_t extension = 0;
        if (!read_extended(receive, extension))
          return RtmpChunkResult::Closed;
      }
      if (first)
        channel.timestamp += channel.delta;
    }

    if (channel.length > kMaxMessageBytes ||
        channel.body.size() > channel.length || !reserve(channel))
      return RtmpChunkResult::Invalid;
    const std::size_t prior = channel.body.size();
    const std::size_t take = std::min<std::size_t>(channel.length - prior, chunk_size_);
    channel.body.resize(prior + take);
    if (take != 0 && !receive(channel.body.data() + prior, take))
      return RtmpChunkResult::Closed;
    if (channel.body.size() != channel.length)
      return RtmpChunkResult::Incomplete;

    completed_ = &channel;
    message = {channel.timestamp, channel.stream_id, channel.type, channel.body};
    return RtmpChunkResult::Message;
  }

private:
  struct Channel
  {
    std::vector<std::uint8_t> body;
    std::uint32_t timestamp = 0;
    std::uint32_t delta = 0;
    std::uint32_t length = 0;
    std::uint32_t stream_id = 0;
    std::uint8_t type = 0;
    bool extended = false;
  };

  static constexpr std::size_t kMaxChannels = 64;
  static constexpr std::uint32_t kMaxChunkBytes = 1u << 20;
  static constexpr std::size_t kMaxMessageBytes = 8u << 20;
  static constexpr std::size_t kBufferBudget = 24u << 20;
  static constexpr std::size_t kRetainPerChannel = 256u << 10;

  static std::uint32_t be24(const std::uint8_t* data)
  {
    return (std::uint32_t{data[0]} << 16) | (std::uint32_t{data[1]} << 8) | data[2];
  }

  template <class Read>
  static bool read_extended(Read& receive, std::uint32_t& value)
  {
    std::uint8_t data[4]{};
    if (!receive(data, sizeof(data)))
      return false;
    value = (std::uint32_t{data[0]} << 24) | be24(data + 1);
    return true;
  }

  void release(Channel& channel)
  {
    allocated_ -= channel.body.capacity();
    std::vector<std::uint8_t>().swap(channel.body);
  }

  void recycle(Channel& channel)
  {
    channel.body.clear();
    if (channel.body.capacity() > kRetainPerChannel)
      release(channel);
  }

  bool reserve(Channel& channel)
  {
    const std::size_t old_capacity = channel.body.capacity();
    if (old_capacity >= channel.length)
      return true;
    const std::size_t extra = channel.length - old_capacity;
    if (extra > kBufferBudget - allocated_)
    {
      for (auto& entry : channels_)
      {
        Channel& idle = entry.second;
        if (&idle != &channel && idle.body.empty())
          release(idle);
      }
      if (extra > kBufferBudget - allocated_)
        return false;
    }
    channel.body.reserve(channel.length);
    allocated_ += channel.body.capacity() - old_capacity;
    return allocated_ <= kBufferBudget;
  }

  std::map<std::uint32_t, Channel> channels_;
  Channel* completed_ = nullptr;
  std::size_t allocated_ = 0;
  std::uint32_t chunk_size_ = 128;
};

} // namespace cr::feed
