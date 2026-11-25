// RTMP H.264 feed mode (sprint B).
// Why this exists:
//   The legacy path was OBS → PC RtmpServer → PC parses chunks → PC packs
//   each NAL into a 16-byte-prefixed TCP frame → cr_feed_proc on phone
//   reads frames → MediaCodec.
//   The PC-side parser was an extra place latency (and bugs) could land.
//   This file moves the RTMP listener onto the phone itself. OBS can publish
//   over an authorized LAN or through a standard adb forward; no LONG VC
//   desktop companion is required. A per-configuration stream key is mandatory
//   before any media message is accepted.
// Wire compatibility:
//   We accept the same simple RTMP 1.0 handshake + chunk-stream + AMF0
//   command set the host-side server already supported:
//     connect / releaseStream / FCPublish / createStream / publish
//   And the same FLV-tag types:
//     0x09 video  (AVC sequence header + coded frames)
//     0x12 data   (onMetaData / @setDataFrame for resolution/fps hint)
//   Audio tags (AAC) are decoded into the same opt-in microphone shared
//   memory used by the local-media source.
// Code lineage:
//   This is a near-line-for-line port of host/src/source/RtmpServer.cpp.
//   Original PC source is preserved verbatim under
//   .backup_20260426_ABCE/ if a side-by-side diff is ever needed.

#include "../include/cr_feed.h"
#include "h264_decode_pipe.h"
#include "aac_decode_pipe.h"
#include "interruptible_socket.h"
#include "media/Avcc.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include "util/Obf.h"

#define TAG "cr_feed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern std::atomic<bool> g_stop;

namespace
{

constexpr size_t kMaxRtmpMessageBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxBufferedRtmpBytes = 24u * 1024u * 1024u;
constexpr size_t kMaxChunkStreams = 64u;
constexpr uint32_t kMaxInboundChunkBytes = 1024u * 1024u;
constexpr unsigned kMaxAmfDepth = 16u;
constexpr size_t kMaxPreAuthBytes = 1024u * 1024u;
constexpr auto kPublishAuthDeadline = std::chrono::seconds(12);

bool constant_time_equal(std::string_view left, std::string_view right)
{
  if (left.size() != right.size())
    return false;
  unsigned difference = 0;
  for (size_t i = 0; i < left.size(); ++i)
    difference |= static_cast<unsigned char>(left[i]) ^
                  static_cast<unsigned char>(right[i]);
  return difference == 0;
}

inline uint32_t be24(const uint8_t* p)
{
  return (p[0] << 16) | (p[1] << 8) | p[2];
}
inline uint32_t be32(const uint8_t* p)
{
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}
inline void put_be24(uint8_t* p, uint32_t v)
{
  p[0] = (v >> 16) & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = v & 0xff;
}
inline void put_be32(uint8_t* p, uint32_t v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

struct Amf0Reader
{
  const uint8_t* p;
  const uint8_t* end;

  bool eof() const noexcept
  {
    return p >= end;
  }
  bool has(size_t n) const noexcept
  {
    return p <= end && static_cast<size_t>(end - p) >= n;
  }
  uint8_t peek_type()
  {
    return eof() ? 0xff : *p;
  }

  bool read_double(double& out)
  {
    if (!has(9) || *p != 0x00)
      return false;
    ++p;
    uint8_t be[8];
    memcpy(be, p, 8);
    p += 8;
    uint8_t le[8];
    for (int i = 0; i < 8; ++i)
      le[i] = be[7 - i];
    memcpy(&out, le, 8);
    return true;
  }
  bool read_string(std::string& out)
  {
    if (!has(3) || *p != 0x02)
      return false;
    ++p;
    uint16_t n = ((uint16_t) p[0] << 8) | p[1];
    p += 2;
    if (!has(n))
      return false;
    out.assign((const char*) p, n);
    p += n;
    return true;
  }
  bool skip_value(unsigned depth = 0);

  bool skip_object_body(unsigned depth)
  {
    if (depth > kMaxAmfDepth)
      return false;
    while (!eof())
    {
      if (has(3) && p[0] == 0 && p[1] == 0 && p[2] == 0x09)
      {
        p += 3;
        return true;
      }
      if (!has(2))
        return false;
      uint16_t klen = ((uint16_t) p[0] << 8) | p[1];
      p += 2;
      if (!has(klen))
        return false;
      p += klen;
      if (!skip_value(depth))
        return false;
    }
    return false;
  }
};

bool Amf0Reader::skip_value(unsigned depth)
{
  if (eof() || depth > kMaxAmfDepth)
    return false;
  uint8_t t = *p++;
  switch (t)
  {
  case 0x00:
    if (!has(8))
      return false;
    p += 8;
    return true;
  case 0x01:
    if (!has(1))
      return false;
    p += 1;
    return true;
  case 0x02:
  {
    if (!has(2))
      return false;
    uint16_t n = ((uint16_t) p[0] << 8) | p[1];
    p += 2;
    if (!has(n))
      return false;
    p += n;
    return true;
  }
  case 0x03:
    return skip_object_body(depth + 1);
  case 0x05:
    return true;
  case 0x06:
    return true;
  case 0x08:
  {
    if (!has(4))
      return false;
    p += 4;
    return skip_object_body(depth + 1);
  }
  case 0x0a:
  {
    if (!has(4))
      return false;
    uint32_t n = be32(p);
    p += 4;
    if (n > static_cast<uint32_t>(end - p))
      return false;
    for (uint32_t i = 0; i < n; ++i)
      if (!skip_value(depth + 1))
        return false;
    return true;
  }
  default:
    return false;
  }
}

void amf_put_string(std::vector<uint8_t>& out, const std::string& s)
{
  out.push_back(0x02);
  out.push_back((s.size() >> 8) & 0xff);
  out.push_back(s.size() & 0xff);
  out.insert(out.end(), s.begin(), s.end());
}
void amf_put_number(std::vector<uint8_t>& out, double v)
{
  out.push_back(0x00);
  uint8_t le[8];
  memcpy(le, &v, 8);
  for (int i = 7; i >= 0; --i)
    out.push_back(le[i]);
}
void amf_put_null(std::vector<uint8_t>& out)
{
  out.push_back(0x05);
}
void amf_put_obj_key(std::vector<uint8_t>& out, const std::string& k)
{
  out.push_back((k.size() >> 8) & 0xff);
  out.push_back(k.size() & 0xff);
  out.insert(out.end(), k.begin(), k.end());
}
void amf_put_obj_end(std::vector<uint8_t>& out)
{
  out.push_back(0x00);
  out.push_back(0x00);
  out.push_back(0x09);
}

void build_rtmp_message(std::vector<uint8_t>& out, uint8_t cs_id, uint32_t timestamp, uint8_t msg_type, uint32_t msg_stream_id, const uint8_t* body, size_t body_len, uint32_t chunk_size)
{
  out.push_back((0 << 6) | (cs_id & 0x3f));
  uint8_t hdr[11];
  put_be24(hdr + 0, timestamp);
  put_be24(hdr + 3, (uint32_t) body_len);
  hdr[6] = msg_type;
  hdr[7] = msg_stream_id & 0xff;
  hdr[8] = (msg_stream_id >> 8) & 0xff;
  hdr[9] = (msg_stream_id >> 16) & 0xff;
  hdr[10] = (msg_stream_id >> 24) & 0xff;
  out.insert(out.end(), hdr, hdr + 11);

  size_t pos = 0;
  bool first = true;
  while (pos < body_len)
  {
    if (!first)
      out.push_back((3 << 6) | (cs_id & 0x3f));
    size_t take = (body_len - pos < (size_t) chunk_size) ? (body_len - pos) : (size_t) chunk_size;
    out.insert(out.end(), body + pos, body + pos + take);
    pos += take;
    first = false;
  }
}

// Per-connection RTMP session: handshake → chunks → FLV → push into pipe.
bool serve_client(int client, const std::string& expected_stream_key,
                  bool enable_video, bool enable_audio)
{
  int no_delay = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
  const auto auth_deadline =
      cr::feed::DeadlineSocket::Clock::now() + kPublishAuthDeadline;
  cr::feed::DeadlineSocket connection(client, g_stop, std::chrono::seconds(10),
                                      auth_deadline);

  // ---- Handshake (simple, no digest) -----------------------------------
  uint8_t c0c1[1 + 1536];
  if (!connection.receive(c0c1, sizeof(c0c1)))
    return false;
  if (c0c1[0] != 0x03)
  {
    LOGW(OBF("rtmp: unexpected C0 version=0x%02x").c_str(), (unsigned) c0c1[0]);
    return false;
  }
  std::vector<uint8_t> s0s1s2;
  s0s1s2.reserve(1 + 1536 + 1536);
  s0s1s2.push_back(0x03);
  uint8_t s1[1536] = {0};
  std::random_device rd;
  for (int i = 8; i < 1536; ++i)
    s1[i] = (uint8_t) rd();
  s0s1s2.insert(s0s1s2.end(), s1, s1 + 1536);
  s0s1s2.insert(s0s1s2.end(), c0c1 + 1, c0c1 + 1 + 1536);
  if (!connection.send(s0s1s2.data(), s0s1s2.size()))
    return false;

  uint8_t c2[1536];
  if (!connection.receive(c2, sizeof(c2)))
    return false;
  LOGI(OBF("rtmp: handshake done").c_str());

  // ---- Chunk stream loop -----------------------------------------------
  uint32_t in_chunk_size = 128;
  uint32_t out_chunk_size = 4096;

  struct MsgHdr
  {
    uint32_t timestamp = 0;
    uint32_t timestamp_delta = 0;
    uint32_t length = 0;
    uint8_t type = 0;
    uint32_t stream_id = 0;
    bool extended_timestamp = false;
    bool valid = false;
  };
  std::map<uint32_t, MsgHdr> last_hdrs;
  std::map<uint32_t, std::vector<uint8_t>> partial;

  std::vector<uint8_t> sps_pps_annexb;
  uint8_t avcc_nalu_length_size = 0;
  int stream_w = 0, stream_h = 0;
  int stream_fps_hint = 30;
  bool pipe_started = false;
  int pipe_w = 0, pipe_h = 0;
  bool seen_video_keyframe = false;
  cr_feed::H264DecodePipe pipe;

  // AAC audio sibling. Lazily configured when the first FLV audio
  // tag arrives with AACPacketType=0 (AudioSpecificConfig). Until
  // then it stays inert and ignores raw AAC frames. This is what
  // lets a "Start replace sound" session work with no pre-existing
  // PC AudioPump in direct-RTMP mode.
  cr_feed::AacDecodePipe audio_pipe;
  bool audio_pipe_started = false;
  bool publisher_authorized = false;
  size_t pre_auth_bytes = 0;

  auto try_start_pipe = [&]()
  {
    int fitted_width = 0;
    int fitted_height = 0;
    if (!cr_feed::fit_source_geometry(stream_w, stream_h, fitted_width,
                                      fitted_height))
      return false;
    if (pipe_started && pipe_w == stream_w && pipe_h == stream_h)
      return true;
    if (pipe_started)
    {
      LOGI(OBF("rtmp: stream geometry changed %dx%d -> %dx%d; restarting decoder").c_str(),
           pipe_w, pipe_h, stream_w, stream_h);
      pipe.stop();
      pipe_started = false;
      seen_video_keyframe = false;
    }
    if (!pipe.start(stream_w, stream_h))
    {
      LOGE(OBF("rtmp: H264DecodePipe.start(%dx%d) failed").c_str(), stream_w, stream_h);
      return false;
    }
    LOGI(OBF("rtmp: pipe started source=%dx%d shared=%dx%d (decoder=%s)").c_str(),
         stream_w, stream_h, fitted_width, fitted_height, pipe.codec_name());
    pipe_started = true;
    pipe_w = stream_w;
    pipe_h = stream_h;
    return true;
  };

  // Initial protocol-control bursts — Window Ack / Peer BW / Set Chunk
  // Size. Same as PC server.
  {
    uint8_t body[4];
    put_be32(body, 5'000'000);
    std::vector<uint8_t> msg;
    build_rtmp_message(msg, 2, 0, 0x05, 0, body, 4, out_chunk_size);
    if (!connection.send(msg.data(), msg.size()))
      return false;
  }
  {
    uint8_t body[5];
    put_be32(body, 5'000'000);
    body[4] = 2;
    std::vector<uint8_t> msg;
    build_rtmp_message(msg, 2, 0, 0x06, 0, body, 5, out_chunk_size);
    if (!connection.send(msg.data(), msg.size()))
      return false;
  }
  {
    uint8_t body[4];
    put_be32(body, out_chunk_size);
    std::vector<uint8_t> msg;
    build_rtmp_message(msg, 2, 0, 0x01, 0, body, 4, out_chunk_size);
    if (!connection.send(msg.data(), msg.size()))
      return false;
  }

  while (!g_stop.load(std::memory_order_relaxed))
  {
    // --- Basic header -------------------------------------------------
    uint8_t bh0;
    if (!connection.receive(&bh0, 1))
      break;
    uint32_t fmt = bh0 >> 6;
    uint32_t cs_id = bh0 & 0x3f;
    if (cs_id == 0)
    {
      uint8_t ext;
      if (!connection.receive(&ext, 1))
        break;
      cs_id = 64u + ext;
    }
    else if (cs_id == 1)
    {
      uint8_t ext[2];
      if (!connection.receive(ext, 2))
        break;
      cs_id = 64u + (uint32_t) ext[0] + ((uint32_t) ext[1] << 8);
    }

    // --- Message header ----------------------------------------------
    auto header_it = last_hdrs.find(cs_id);
    if (header_it == last_hdrs.end())
    {
      if (fmt != 0 || last_hdrs.size() >= kMaxChunkStreams)
      {
        LOGW(OBF("rtmp: invalid or excessive chunk stream").c_str());
        return false;
      }
      header_it = last_hdrs.emplace(cs_id, MsgHdr{}).first;
      partial.emplace(cs_id, std::vector<uint8_t>{});
    }
    MsgHdr& last = header_it->second;
    auto& buf = partial.at(cs_id);
    const bool first_chunk = buf.empty();
    if (!first_chunk && fmt != 3)
    {
      LOGW(OBF("rtmp: new message header before prior message completed").c_str());
      return false;
    }

    if (fmt == 0)
    {
      uint8_t h[11];
      if (!connection.receive(h, 11))
        break;
      const uint32_t raw_timestamp = be24(h);
      last.timestamp = raw_timestamp;
      last.length = be24(h + 3);
      last.type = h[6];
      last.stream_id = (uint32_t) h[7] | ((uint32_t) h[8] << 8) |
                       ((uint32_t) h[9] << 16) |
                       ((uint32_t) h[10] << 24);
      last.timestamp_delta = 0;
      last.extended_timestamp = raw_timestamp == 0xffffffu;
      if (last.extended_timestamp)
      {
        uint8_t ext[4];
        if (!connection.receive(ext, 4))
          break;
        last.timestamp = be32(ext);
      }
      last.valid = true;
    }
    else if (fmt == 1)
    {
      if (!last.valid)
        return false;
      uint8_t h[7];
      if (!connection.receive(h, 7))
        break;
      const uint32_t raw_delta = be24(h);
      uint32_t delta = raw_delta;
      last.length = be24(h + 3);
      last.type = h[6];
      last.extended_timestamp = raw_delta == 0xffffffu;
      if (last.extended_timestamp)
      {
        uint8_t ext[4];
        if (!connection.receive(ext, 4))
          break;
        delta = be32(ext);
      }
      last.timestamp_delta = delta;
      last.timestamp += delta;
    }
    else if (fmt == 2)
    {
      if (!last.valid)
        return false;
      uint8_t h[3];
      if (!connection.receive(h, 3))
        break;
      const uint32_t raw_delta = be24(h);
      uint32_t delta = raw_delta;
      last.extended_timestamp = raw_delta == 0xffffffu;
      if (last.extended_timestamp)
      {
        uint8_t ext[4];
        if (!connection.receive(ext, 4))
          break;
        delta = be32(ext);
      }
      last.timestamp_delta = delta;
      last.timestamp += delta;
    }
    else
    {
      if (!last.valid)
        return false;
      if (last.extended_timestamp)
      {
        uint8_t ignored[4];
        if (!connection.receive(ignored, 4))
          break;
      }
      if (first_chunk)
        last.timestamp += last.timestamp_delta;
    }

    if (last.length > kMaxRtmpMessageBytes || buf.size() > last.length)
    {
      LOGW(OBF("rtmp: message exceeds parser budget").c_str());
      return false;
    }

    // --- Chunk payload ------------------------------------------------
    const uint32_t remaining =
        last.length - static_cast<uint32_t>(buf.size());
    const uint32_t take = remaining < in_chunk_size ? remaining : in_chunk_size;
    size_t buffered = 0;
    for (const auto& entry : partial)
    {
      if (entry.second.size() > kMaxBufferedRtmpBytes - buffered)
        return false;
      buffered += entry.second.size();
    }
    if (take > kMaxBufferedRtmpBytes - buffered)
      return false;
    size_t old_size = buf.size();
    buf.resize(old_size + take);
    if (take > 0)
    {
      if (!connection.receive(buf.data() + old_size, take))
        goto done;
    }
    if (!publisher_authorized)
    {
      pre_auth_bytes += take;
      if (pre_auth_bytes > kMaxPreAuthBytes ||
          std::chrono::steady_clock::now() > auth_deadline)
      {
        LOGW(OBF("rtmp: publisher authentication deadline exceeded").c_str());
        return false;
      }
    }

    if (buf.size() < last.length)
      continue;

    // --- Full message — dispatch -------------------------------------
    {
      const uint8_t type = last.type;
      std::vector<uint8_t> body = std::move(buf);
      buf.clear();

      switch (type)
      {
      case 0x01:
      { // Set Chunk Size
        if (body.size() >= 4)
        {
          uint32_t ns = be32(body.data()) & 0x7fffffff;
          if (ns > 0 && ns <= kMaxInboundChunkBytes)
            in_chunk_size = ns;
        }
        break;
      }
      case 0x03:
      case 0x04:
      case 0x05:
      case 0x06:
        break;

      case 0x11:
      case 0x14:
      { // AMF command
        const uint8_t* p0 = body.data();
        const uint8_t* pe = body.data() + body.size();
        if (type == 0x11 && p0 < pe)
          ++p0;
        Amf0Reader r{p0, pe};
        std::string cmd;
        double tid = 0;
        if (!r.read_string(cmd) || !r.read_double(tid) || !std::isfinite(tid))
          break;

        if (cmd == OBF("connect").c_str())
        {
          std::vector<uint8_t> resp;
          amf_put_string(resp, OBF("_result").c_str());
          amf_put_number(resp, tid);
          resp.push_back(0x03);
          amf_put_obj_key(resp, OBF("fmsVer").c_str());
          amf_put_string(resp, OBF("FMS/3,5,3,888").c_str());
          amf_put_obj_key(resp, OBF("capabilities").c_str());
          amf_put_number(resp, 31.0);
          amf_put_obj_key(resp, OBF("mode").c_str());
          amf_put_number(resp, 1.0);
          amf_put_obj_end(resp);
          resp.push_back(0x03);
          amf_put_obj_key(resp, OBF("level").c_str());
          amf_put_string(resp, OBF("status").c_str());
          amf_put_obj_key(resp, OBF("code").c_str());
          amf_put_string(resp, OBF("NetConnection.Connect.Success").c_str());
          amf_put_obj_key(resp, OBF("description").c_str());
          amf_put_string(resp, OBF("Connection succeeded.").c_str());
          amf_put_obj_key(resp, OBF("objectEncoding").c_str());
          amf_put_number(resp, 0.0);
          amf_put_obj_end(resp);
          std::vector<uint8_t> msg;
          build_rtmp_message(msg, 3, 0, 0x14, 0, resp.data(), resp.size(), out_chunk_size);
          if (!connection.send(msg.data(), msg.size()))
            goto done;
        }
        else if (cmd == OBF("releaseStream").c_str() || cmd == OBF("FCPublish").c_str() || cmd == OBF("FCUnpublish").c_str() || cmd == OBF("deleteStream").c_str())
        {
          std::vector<uint8_t> resp;
          amf_put_string(resp, OBF("_result").c_str());
          amf_put_number(resp, tid);
          amf_put_null(resp);
          amf_put_null(resp);
          std::vector<uint8_t> msg;
          build_rtmp_message(msg, 3, 0, 0x14, 0, resp.data(), resp.size(), out_chunk_size);
          if (!connection.send(msg.data(), msg.size()))
            goto done;
        }
        else if (cmd == OBF("createStream").c_str())
        {
          std::vector<uint8_t> resp;
          amf_put_string(resp, OBF("_result").c_str());
          amf_put_number(resp, tid);
          amf_put_null(resp);
          amf_put_number(resp, 1.0);
          std::vector<uint8_t> msg;
          build_rtmp_message(msg, 3, 0, 0x14, 0, resp.data(), resp.size(), out_chunk_size);
          if (!connection.send(msg.data(), msg.size()))
            goto done;
        }
        else if (cmd == OBF("publish").c_str())
        {
          // publish(null, streamName, "live"). The server is reachable on
          // the LAN, so accepting an arbitrary stream name would let another
          // local-network peer replace camera frames. Authenticate before
          // acknowledging publish or consuming any media.
          std::string stream_name;
          if (!r.skip_value() || !r.read_string(stream_name) ||
              !constant_time_equal(stream_name, expected_stream_key))
          {
            LOGW(OBF("rtmp: rejected publisher with invalid stream key").c_str());
            return false;
          }
          publisher_authorized = true;
          connection.finish_startup();
          std::vector<uint8_t> resp;
          amf_put_string(resp, OBF("onStatus").c_str());
          amf_put_number(resp, 0.0);
          amf_put_null(resp);
          resp.push_back(0x03);
          amf_put_obj_key(resp, OBF("level").c_str());
          amf_put_string(resp, OBF("status").c_str());
          amf_put_obj_key(resp, OBF("code").c_str());
          amf_put_string(resp, OBF("NetStream.Publish.Start").c_str());
          amf_put_obj_key(resp, OBF("description").c_str());
          amf_put_string(resp, OBF("Start publishing.").c_str());
          amf_put_obj_end(resp);
          std::vector<uint8_t> msg;
          build_rtmp_message(msg, 4, 0, 0x14, 1, resp.data(), resp.size(), out_chunk_size);
          if (!connection.send(msg.data(), msg.size()))
            goto done;
        }
        break;
      }

      case 0x12:
      case 0x0f:
      { // AMF data (onMetaData)
        if (!publisher_authorized)
          break;
        const uint8_t* p0 = body.data();
        const uint8_t* pe = body.data() + body.size();
        if (type == 0x0f && p0 < pe)
          ++p0;
        Amf0Reader r{p0, pe};
        std::string s;
        if (r.read_string(s) && (s == OBF("@setDataFrame").c_str() || s == OBF("onMetaData").c_str()))
        {
          std::string inner;
          if (s == OBF("@setDataFrame").c_str())
            r.read_string(inner);
          while (!r.eof())
          {
            uint8_t t = r.peek_type();
            if (t == 0x08)
            {
              if (!r.has(5))
                break;
              r.p += 5;
            }
            else if (t == 0x03)
            {
              ++r.p;
            }
            else
              break;

            while (!r.eof())
            {
              if (r.has(3) && r.p[0] == 0 && r.p[1] == 0 && r.p[2] == 9)
              {
                r.p += 3;
                break;
              }
              if (!r.has(2))
                break;
              uint16_t klen = ((uint16_t) r.p[0] << 8) | r.p[1];
              r.p += 2;
              if (!r.has(klen))
                break;
              std::string key((const char*) r.p, klen);
              r.p += klen;

              if (r.peek_type() == 0x00)
              {
                double d;
                if (!r.read_double(d))
                  break;
                if (!std::isfinite(d))
                  continue;
                if (key == OBF("width").c_str() && d >= 16.0 && d <= 8192.0)
                  stream_w = static_cast<int>(d);
                else if (key == OBF("height").c_str() && d >= 16.0 && d <= 8192.0)
                  stream_h = static_cast<int>(d);
                else if ((key == OBF("framerate").c_str() || key == "fps") &&
                         d >= 1.0 && d <= 240.0)
                  stream_fps_hint = static_cast<int>(d + 0.5);
              }
              else
              {
                if (!r.skip_value())
                  break;
              }
            }
            break;
          }
          if (enable_video && stream_w > 0 && stream_h > 0)
          {
            LOGI(OBF("rtmp meta: %dx%d @%d fps").c_str(), stream_w, stream_h, stream_fps_hint);
            try_start_pipe();
          }
        }
        break;
      }

      case 0x08:
      { // audio (FLV)
        if (!publisher_authorized || !enable_audio)
          break;
        if (body.size() < 2)
          break;
        const uint8_t flv0 = body[0];
        const uint8_t sound_fmt = flv0 >> 4;
        if (sound_fmt != 10)
          break; // only AAC handled
        const uint8_t aac_pkt_type = body[1];
        const uint8_t* payload = body.data() + 2;
        size_t pay_len = body.size() - 2;
        if (aac_pkt_type == 0)
        { // AudioSpecificConfig
          if (audio_pipe.on_csd(payload, pay_len))
          {
            audio_pipe_started = true;
          }
        }
        else if (aac_pkt_type == 1 && audio_pipe_started)
        {
          int64_t pts_us = (int64_t) last.timestamp * 1000;
          audio_pipe.push(payload, pay_len, pts_us);
        }
        break;
      }

      case 0x09:
      { // video (H.264 / AVC)
        if (!publisher_authorized || !enable_video)
          break;
        if (body.size() < 5)
          break;
        uint8_t frame_type = (body[0] >> 4) & 0x0f;
        uint8_t codec_id = body[0] & 0x0f;
        if (codec_id != 7)
          break;

        uint8_t avc_packet_type = body[1];
        int32_t cts_ms = (int32_t) ((body[2] << 16) | (body[3] << 8) | body[4]);
        if (cts_ms & 0x800000)
          cts_ms |= ~0xffffff;
        int64_t dts_ms = (int64_t) last.timestamp;
        int64_t pts_us = (dts_ms + cts_ms) * 1000;

        const uint8_t* payload = body.data() + 5;
        size_t pay_len = body.size() - 5;

        if (avc_packet_type == 0)
        {
          cr::media::AvcDecoderConfig config{};
          if (!cr::media::parse_avc_decoder_config({payload, pay_len}, config))
          {
            LOGW(OBF("rtmp: invalid AVCDecoderConfigurationRecord").c_str());
            break;
          }
          sps_pps_annexb = std::move(config.parameter_sets_annexb);
          avcc_nalu_length_size = config.nalu_length_size;
          seen_video_keyframe = false;
          // ReCam's SPS-derived geometry removes the fragile dependency on
          // onMetaData ordering. SPS is authoritative when it is parseable;
          // metadata remains a compatibility fallback for unusual profiles.
          if (config.width != 0 && config.height != 0)
          {
            stream_w = static_cast<int>(config.width);
            stream_h = static_cast<int>(config.height);
          }
          if (stream_w > 0 && stream_h > 0)
            try_start_pipe();
          if (pipe_started && !sps_pps_annexb.empty())
          {
            pipe.push(sps_pps_annexb.data(), sps_pps_annexb.size(), pts_us);
          }
        }
        else if (avc_packet_type == 1)
        {
          if (!pipe_started)
          {
            if (stream_w <= 0 || stream_h <= 0)
            {
              LOGW(OBF("rtmp: waiting for bounded stream metadata before decoder start").c_str());
              break;
            }
            try_start_pipe();
            if (!pipe_started)
              break;
          }
          std::vector<uint8_t> annexb;
          const bool is_key = (frame_type == 1);
          if (!is_key && !seen_video_keyframe)
            break;
          if (is_key && !sps_pps_annexb.empty())
          {
            annexb.insert(annexb.end(), sps_pps_annexb.begin(), sps_pps_annexb.end());
          }
          if (avcc_nalu_length_size == 0)
            break;
          std::vector<uint8_t> converted;
          if (!cr::media::avcc_access_unit_to_annexb(
                  {payload, pay_len}, avcc_nalu_length_size, converted))
          {
            LOGW(OBF("rtmp: malformed AVCC access unit").c_str());
            break;
          }
          annexb.insert(annexb.end(), converted.begin(), converted.end());
          if (!annexb.empty())
          {
            if (!pipe.push(annexb.data(), annexb.size(), pts_us))
              seen_video_keyframe = false;
            else if (is_key)
              seen_video_keyframe = true;
          }
        }
        break;
      }

      default:
        break;
      }
    }
  }
done:
  LOGI(OBF("rtmp: client gone (decoded in=%llu out=%llu, audio=%s)").c_str(), (unsigned long long) pipe.input_count(), (unsigned long long) pipe.output_count(), audio_pipe_started ? "on" : OBF("off").c_str());
  pipe.stop();
  audio_pipe.stop();
  return true;
}

} // namespace

extern "C" int cr_feed_run_rtmp(int port, const char* stream_key,
                                int allow_lan, int enable_video,
                                int enable_audio);

int cr_feed_run_rtmp(int port, const char* stream_key, int allow_lan,
                     int enable_video, int enable_audio)
{
  if (!stream_key || strlen(stream_key) < 24 || strlen(stream_key) > 96 ||
      (!enable_video && !enable_audio))
  {
    LOGE(OBF("rtmp: invalid stream-key policy").c_str());
    return -1;
  }
  const std::string expected_stream_key(stream_key);
  int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (srv < 0)
  {
    LOGE(OBF("rtmp: socket: %s").c_str(), strerror(errno));
    return -1;
  }
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t) port);
  // Loopback/USB is the default. Listening on the LAN is possible only after
  // an explicit UI opt-in and still requires the random stream key.
  addr.sin_addr.s_addr = htonl(allow_lan ? INADDR_ANY : INADDR_LOOPBACK);

  if (bind(srv, (sockaddr*) &addr, sizeof(addr)) != 0)
  {
    LOGE(OBF("rtmp: bind :%d: %s").c_str(), port, strerror(errno));
    close(srv);
    return -1;
  }
  if (listen(srv, 1) != 0)
  {
    LOGE(OBF("rtmp: listen: %s").c_str(), strerror(errno));
    close(srv);
    return -1;
  }
  LOGI(OBF("rtmp listener on %s:%d (authenticated, video=%d audio=%d)").c_str(),
       allow_lan ? OBF("0.0.0.0").c_str() : OBF("127.0.0.1").c_str(),
       port, enable_video, enable_audio);

  while (!g_stop.load(std::memory_order_relaxed))
  {
    int client = cr::feed::accept_interruptible(srv, g_stop);
    if (client < 0)
    {
      if (g_stop.load(std::memory_order_relaxed))
        break;
      if (errno == EINTR)
        continue;
      LOGW(OBF("rtmp: accept: %s").c_str(), strerror(errno));
      break;
    }
    LOGI(OBF("rtmp: publisher connected").c_str());
    serve_client(client, expected_stream_key, enable_video != 0,
                 enable_audio != 0);
    close(client);
    LOGI(OBF("rtmp: publisher gone").c_str());
  }

  close(srv);
  return 0;
}
