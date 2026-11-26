// Phone-side RTMP playback from the configured desktop media server.
// The existing RTMP chunk/FLV parser feeds the shared H.264 and optional AAC decoders.
#include "h264_decode_pipe.h"
#include "aac_decode_pipe.h"
#include "interruptible_socket.h"
#include "RtmpChunks.h"
#include "media/Avcc.h"
#include "media/RtmpAddress.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <new>
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

constexpr unsigned kMaxAmfDepth = 16u;
constexpr size_t kMaxStartupBytes = 1024u * 1024u;
constexpr auto kStartupTimeout = std::chrono::seconds(12);
constexpr auto kLiveIoTimeout = std::chrono::seconds(3);
constexpr auto kVideoFrameTimeout = std::chrono::seconds(2);

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

  bool read_string_property(std::string_view key, std::string& value)
  {
    if (peek_type() == 0x03)
      ++p;
    else if (peek_type() == 0x08 && has(5))
      p += 5;
    else
      return false;
    while (has(2))
    {
      const size_t length = (size_t{p[0]} << 8) | p[1];
      p += 2;
      if (length == 0 || !has(length))
        return false;
      const std::string_view name(reinterpret_cast<const char*>(p), length);
      p += length;
      if (name == key)
        return read_string(value);
      if (!skip_value())
        return false;
    }
    return false;
  }

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

bool send_message(cr::feed::DeadlineSocket& connection, uint8_t type, uint32_t stream,
                  const uint8_t* bytes, size_t count)
{
  std::vector<uint8_t> message;
  build_rtmp_message(message, type == 0x14 ? 3 : 2, 0, type, stream, bytes, count, 128);
  return connection.send(message.data(), message.size());
}

bool send_command(cr::feed::DeadlineSocket& connection, uint32_t stream,
                  const std::vector<uint8_t>& command)
{
  return send_message(connection, 0x14, stream, command.data(), command.size());
}

bool play_session(int socket, const cr::media::RtmpAddress& address,
                  bool enable_video, bool enable_audio)
{
  int one = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  const auto startup_deadline = cr::feed::DeadlineSocket::Clock::now() + kStartupTimeout;
  cr::feed::DeadlineSocket connection(socket, g_stop, std::chrono::seconds(10), startup_deadline);
  std::array<uint8_t, 1537> c0c1{};
  c0c1[0] = 3;
  std::random_device random;
  for (size_t n = 9; n < c0c1.size(); ++n)
    c0c1[n] = static_cast<uint8_t>(random());

  std::array<uint8_t, 3073> response{};
  if (!connection.send(c0c1.data(), c0c1.size()) ||
      !connection.receive(response.data(), response.size()) || response[0] != 3 ||
      !connection.send(response.data() + 1, 1536))
    return false;

  std::vector<uint8_t> connect;
  amf_put_string(connect, "connect");
  amf_put_number(connect, 1);
  connect.push_back(0x03);
  amf_put_obj_key(connect, "app");
  amf_put_string(connect, address.app);
  amf_put_obj_key(connect, "tcUrl");
  amf_put_string(connect, address.tc_url);
  amf_put_obj_key(connect, "flashVer");
  amf_put_string(connect, "LONG Virtual Camera");
  amf_put_obj_key(connect, "objectEncoding");
  amf_put_number(connect, 0);
  amf_put_obj_end(connect);
  if (!send_command(connection, 0, connect))
    return false;

  cr::feed::RtmpChunkReader reader;
  uint32_t ack_window = 0;
  uint32_t advertised_peer_window = 0;
  uint64_t last_ack = 0;
  auto acknowledge = [&]()
  {
    const uint64_t received = connection.received();
    if (ack_window == 0 || received - last_ack < ack_window)
      return true;
    uint8_t ack[4];
    put_be32(ack, static_cast<uint32_t>(received));
    if (!send_message(connection, 0x03, 0, ack, sizeof(ack)))
      return false;
    last_ack = received;
    return true;
  };
  auto read_with_ack = [&](uint8_t* data, size_t size)
  {
    while (size != 0)
    {
      if (!acknowledge())
        return false;
      size_t take = size;
      if (ack_window != 0)
        take = static_cast<size_t>(std::min<uint64_t>(
            size, ack_window - (connection.received() - last_ack)));
      if (!connection.receive(data, take))
        return false;
      data += take;
      size -= take;
    }
    return acknowledge();
  };

  std::vector<uint8_t> sps_pps_annexb;
  std::vector<uint8_t> annexb;
  uint8_t avcc_nalu_length_size = 0;
  int stream_w = 0, stream_h = 0;
  int metadata_w = 0, metadata_h = 0;
  bool geometry_from_sps = false;
  int stream_fps_hint = 30;
  bool pipe_started = false;
  int pipe_w = 0, pipe_h = 0;
  bool seen_video_keyframe = false;
  cr_feed::H264DecodePipe pipe;

  // Audio decoding begins after an AAC AudioSpecificConfig.
  cr_feed::AacDecodePipe audio_pipe;
  bool audio_pipe_started = false;
  bool connection_accepted = false;
  uint32_t play_stream_id = 0;
  bool receiving_media = false;
  uint64_t last_decoded = 0;
  auto last_frame = std::chrono::steady_clock::time_point::min();
  bool video_ready = false;

  auto begin_media = [&]()
  {
    if (receiving_media)
      return;
    receiving_media = true;
    connection.finish_startup();
    connection.set_io_timeout(kLiveIoTimeout);
    LOGI("rtmp: desktop stream is delivering supported media");
  };

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

  while (!g_stop.load(std::memory_order_relaxed))
  {
    cr::feed::RtmpMessage message;
    const auto result = reader.read(read_with_ack, message);
    if (result == cr::feed::RtmpChunkResult::Closed)
      break;
    if (result == cr::feed::RtmpChunkResult::Invalid)
    {
      LOGW("rtmp: invalid chunk headers or receive budget exceeded");
      return false;
    }
    if (!receiving_media && (connection.received() > kMaxStartupBytes ||
                            std::chrono::steady_clock::now() > startup_deadline))
    {
      LOGW("rtmp: play startup deadline exceeded");
      return false;
    }
    if (result == cr::feed::RtmpChunkResult::Incomplete)
      continue;

    {
      const uint8_t type = message.type;
      auto body = message.body;
      switch (type)
      {
      case 0x01: // Set Chunk Size
        if (body.size() != 4 || !reader.set_chunk_size(be32(body.data())))
          goto done;
        break;

      case 0x02: // Abort one partially assembled message.
        if (body.size() == 4)
          reader.abort(be32(body.data()));
        break;

      case 0x04: // User Control
        if (body.size() == 6 && body[0] == 0)
        {
          if (body[1] == 1 && play_stream_id != 0 && be32(body.data() + 2) == play_stream_id)
            goto done; // Stream EOF
          if (body[1] == 6)
          {
            body[1] = 7;
            if (!send_message(connection, 0x04, 0, body.data(), body.size()))
              goto done;
          }
        }
        break;

      case 0x05: // Window Acknowledgement Size
        if (body.size() == 4 && be32(body.data()) != 0)
          ack_window = be32(body.data());
        break;

      case 0x06: // Set Peer Bandwidth
        if (body.size() == 5 && body[4] <= 2)
        {
          const uint32_t window = be32(body.data());
          if (window != 0 && window != advertised_peer_window)
          {
            if (!send_message(connection, 0x05, 0, body.data(), 4))
              goto done;
            advertised_peer_window = window;
          }
        }
        break;

      case 0x11:
      case 0x14:
      {
        const size_t prefix = type == 0x11 ? 1 : 0;
        if (body.size() <= prefix)
          break;
        Amf0Reader r{body.data() + prefix, body.data() + body.size()};
        std::string name;
        double transaction = 0;
        if (!r.read_string(name) || !r.read_double(transaction))
          break;
        if (name == "_error")
        {
          LOGW("rtmp: server rejected the playback request");
          goto done;
        }
        if (name == "onStatus" && play_stream_id != 0 &&
            message.stream_id == play_stream_id)
        {
          std::string code;
          if (r.skip_value() && r.read_string_property("code", code) &&
              (code == "NetStream.Play.StreamNotFound" || code == "NetStream.Play.Failed" ||
               code == "NetStream.Play.Stop" || code == "NetStream.Play.UnpublishNotify"))
          {
            LOGI("rtmp: requested stream ended or is unavailable");
            goto done;
          }
        }
        if (name == "_result" && transaction == 1 && !connection_accepted)
        {
          std::vector<uint8_t> create;
          amf_put_string(create, "createStream");
          amf_put_number(create, 2);
          amf_put_null(create);
          if (!send_command(connection, 0, create))
            goto done;
          connection_accepted = true;
        }
        else if (name == "_result" && transaction == 2 && connection_accepted && !play_stream_id)
        {
          double id = 0;
          if (!r.skip_value() || !r.read_double(id) ||
              !std::isfinite(id) || id < 1 || id > UINT32_MAX || std::floor(id) != id)
            goto done;
          play_stream_id = static_cast<uint32_t>(id);
          std::vector<uint8_t> play;
          amf_put_string(play, "play");
          amf_put_number(play, 0);
          amf_put_null(play);
          amf_put_string(play, address.stream);
          amf_put_number(play, -1000);
          if (!send_command(connection, play_stream_id, play))
            goto done;
        }
        break;
      }

      case 0x12:
      case 0x0f:
      { // AMF data (onMetaData)
        if (play_stream_id == 0 || message.stream_id != play_stream_id || body.empty())
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
                  metadata_w = static_cast<int>(d);
                else if (key == OBF("height").c_str() && d >= 16.0 && d <= 8192.0)
                  metadata_h = static_cast<int>(d);
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
          if (enable_video && !geometry_from_sps && avcc_nalu_length_size != 0 &&
              metadata_w > 0 && metadata_h > 0)
          {
            stream_w = metadata_w;
            stream_h = metadata_h;
            LOGI(OBF("rtmp meta: %dx%d @%d fps").c_str(), stream_w, stream_h, stream_fps_hint);
            try_start_pipe();
          }
        }
        break;
      }

      case 0x08:
      { // audio (FLV)
        if (play_stream_id == 0 || message.stream_id != play_stream_id || !enable_audio)
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
            begin_media();
          }
        }
        else if (aac_pkt_type == 1 && audio_pipe_started)
        {
          int64_t pts_us = (int64_t) message.timestamp * 1000;
          audio_pipe.push(payload, pay_len, pts_us);
        }
        break;
      }

      case 0x09:
      { // video (H.264 / AVC)
        if (play_stream_id == 0 || message.stream_id != play_stream_id || !enable_video)
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
        int64_t dts_ms = (int64_t) message.timestamp;
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
          begin_media();
          sps_pps_annexb = std::move(config.parameter_sets_annexb);
          avcc_nalu_length_size = config.nalu_length_size;
          seen_video_keyframe = false;
          // ReCam's SPS-derived geometry removes the fragile dependency on
          // onMetaData ordering. SPS is authoritative when it is parseable;
          // metadata remains a compatibility fallback for unusual profiles.
          geometry_from_sps = config.width != 0 && config.height != 0;
          stream_w = geometry_from_sps ? static_cast<int>(config.width) : metadata_w;
          stream_h = geometry_from_sps ? static_cast<int>(config.height) : metadata_h;
          if (stream_w > 0 && stream_h > 0)
            try_start_pipe();
        }
        else if (avc_packet_type == 1 && avcc_nalu_length_size != 0)
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
          const bool is_key = (frame_type == 1);
          if (!is_key && !seen_video_keyframe)
            break;
          const auto prefix = is_key ? std::span<const uint8_t>(sps_pps_annexb)
                                     : std::span<const uint8_t>();
          if (!cr::media::avcc_access_unit_to_annexb(
                  {payload, pay_len}, avcc_nalu_length_size, annexb, prefix))
          {
            LOGW(OBF("rtmp: malformed AVCC access unit").c_str());
            break;
          }
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
    // Audio or keepalives can outlive the video. Publish unavailable once
    // until a complete decoded frame has advanced the shared ring again.
    const auto frame_count = pipe.output_count();
    if (frame_count != last_decoded)
    {
      last_decoded = frame_count;
      last_frame = std::chrono::steady_clock::now();
      video_ready = frame_count != 0;
    }
    else if (video_ready && std::chrono::steady_clock::now() - last_frame >= kVideoFrameTimeout)
    {
      pipe.revoke_frame();
      video_ready = false;
      LOGW("rtmp: no decoded video in two seconds, waiting for a fresh frame");
    }
  }
done:
  LOGI(OBF("rtmp: reader disconnected (decoded in=%llu out=%llu, audio=%s)").c_str(), (unsigned long long) pipe.input_count(), (unsigned long long) pipe.output_count(), audio_pipe_started ? "on" : OBF("off").c_str());
  pipe.stop();
  audio_pipe.stop();
  return true;
}

} // namespace

namespace
{

int connect_rtmp(const cr::media::RtmpAddress& address)
{
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* candidates = nullptr;
  if (getaddrinfo(address.host.c_str(), address.port.c_str(), &hints, &candidates) != 0)
    return -1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  int connected = -1;
  for (auto* candidate = candidates; candidate && !g_stop.load(std::memory_order_relaxed);
       candidate = candidate->ai_next)
  {
    const int fd = socket(candidate->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          candidate->ai_protocol);
    if (fd < 0)
      continue;
    const int result = connect(fd, candidate->ai_addr, candidate->ai_addrlen);
    if (result == 0)
    {
      connected = fd;
      break;
    }
    if (errno == EINPROGRESS)
    {
      while (!g_stop.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline)
      {
        pollfd watcher{fd, POLLOUT, 0};
        const int ready = poll(&watcher, 1, 200);
        if (ready == 0 || (ready < 0 && errno == EINTR))
          continue;
        int error = 0;
        socklen_t size = sizeof(error);
        if (ready > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0)
          connected = fd;
        break;
      }
    }
    if (connected == fd)
      break;
    close(fd);
    if (std::chrono::steady_clock::now() >= deadline)
      break;
  }
  freeaddrinfo(candidates);
  return connected;
}

} // namespace

extern "C" int cr_feed_run_rtmp_pull(const char* url, int video_enabled, int audio_enabled)
{
  cr::media::RtmpAddress address;
  if (!url || !cr::media::parse_rtmp_address(url, address) ||
      (!video_enabled && !audio_enabled))
  {
    LOGE("rtmp: invalid desktop playback address");
    return -1;
  }

  unsigned retry = 0;
  while (!g_stop.load(std::memory_order_relaxed))
  {
    const int socket = connect_rtmp(address);
    if (socket >= 0)
    {
      retry = 0;
      LOGI("rtmp: connected to desktop media server");
      try
      {
        play_session(socket, address, video_enabled != 0, audio_enabled != 0);
      }
      catch (const std::bad_alloc&)
      {
        LOGW("rtmp: session memory limit reached");
      }
      close(socket);
    }
    else if (retry++ % 8 == 0)
    {
      LOGW("rtmp: desktop media server unreachable, retrying");
    }
    for (int i = 0; i < 10 && !g_stop.load(std::memory_order_relaxed); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}
