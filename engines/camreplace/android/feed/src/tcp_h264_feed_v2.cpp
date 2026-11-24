/*
 * Назначение файла:
 *   Экспериментальный threaded TCP H.264 feed. Wire protocol совпадает с
 *   tcp_h264_feed.cpp (CRH2), но network/decode/publish разнесены по
 *   очередям и потокам для снижения back-pressure на socket read.
 *
 * Hook point и ABI assumptions:
 *   C symbol cr_feed_run_tcp_h264 совпадает с production file, поэтому
 *   одновременно компилируется только один variant через CR_FEED_USE_THREADED.
 *   Shared memory /data/cr/feed остаётся v1 и использует тот же cr_feed_header.
 *
 * Ограничения Pixel/Android:
 *   Tensor Pixel не должен зависеть от Qualcomm decoder names. Primary decoder
 *   selection — AMediaCodec_createDecoderByType("video/avc"), vendor names
 *   используются только как fallback. v1 shm cap остаётся 1920x1080 NV21.
 *
 * Подтверждено:
 *   Этот файл diagnostic/experimental, но не должен расходиться по ABI и
 *   decoder policy с production tcp_h264_feed.cpp.
 *
 * Diagnostic-only:
 *   Threaded pacing и queue drops являются экспериментальной диагностикой, а
 *   не новым production acceptance criterion.
 */

#include "../include/cr_feed.h"
#include "h264_decode_pipe.h" // ShmOut, NV12/I420 → NV21 helpers
#include "feed_queues.h"
#include "interruptible_socket.h"
#include "shm.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

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
#include <exception>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#include "util/Obf.h"

#define TAG "cr_feed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern std::atomic<bool> g_stop;

namespace
{

constexpr uint32_t kMagic = 0x32484243u; // 'CRH2' LE

bool nv21_shape_supported(int w, int h)
{
  if (w < 16 || w > (int) CR_FEED_MAX_WIDTH ||
      h < 16 || h > (int) CR_FEED_MAX_HEIGHT ||
      (w & 1) != 0 || (h & 1) != 0)
  {
    LOGE(OBF("v2: bogus dims %dx%d").c_str(), w, h);
    return false;
  }
  const uint64_t bytes = (uint64_t) w * (uint64_t) h * 3ull / 2ull;
  if (bytes > CR_FEED_MAX_NV21_BYTES)
  {
    LOGE(OBF("v2: %dx%d NV21 needs %llu bytes, pixel cap is %u bytes").c_str(), w, h, (unsigned long long) bytes, CR_FEED_MAX_NV21_BYTES);
    return false;
  }
  return true;
}

// Queue caps tuned for 60 fps @ 1080p. Packet queue ~ 6 frames worth so
// a brief decoder hiccup doesn't drop frames; frame queue ~ 4 NV21
// images so the publish thread always has something to send and the
// decoder thread can sprint a bit ahead.
constexpr size_t kPacketQueueCap = 12;
constexpr size_t kFrameQueueCap = 4;

bool recv_all(int fd, void* buf, size_t n)
{
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n > 0 && !g_stop.load(std::memory_order_relaxed))
  {
    ssize_t r = recv(fd, p, n, 0);
    if (r == 0)
      return false;
    if (r < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      return false;
    }
    p += r;
    n -= (size_t) r;
  }
  return n == 0;
}

// Network thread: pulls "CRH2"-framed packets off the socket, pushes
// NalPackets into pkt_q. Returns when socket dies or g_stop fires.
void network_loop(int client, cr_feed::BoundedQueue<cr_feed::NalPacket>& pkt_q, std::atomic<uint64_t>& pkt_dropped)
{
  while (!g_stop.load(std::memory_order_relaxed))
  {
    struct
    {
      uint32_t len;
      uint32_t flags;
      int64_t pts_us;
    } ph;
    if (!recv_all(client, &ph, sizeof(ph)))
      break;
    if (ph.len == 0 || ph.len > 16u * 1024u * 1024u)
    {
      LOGE(OBF("v2/net: bogus packet len %u").c_str(), ph.len);
      break;
    }
    cr_feed::NalPacket pkt;
    pkt.data.resize(ph.len);
    pkt.pts_us = ph.pts_us;
    pkt.is_key = (ph.flags & 1) != 0;
    if (!recv_all(client, pkt.data.data(), ph.len))
      break;
    if (!pkt_q.push(std::move(pkt)))
    {
      pkt_dropped.fetch_add(1, std::memory_order_relaxed);
    }
  }
  pkt_q.close();
}

// Decoder thread: pulls packets, feeds AMediaCodec, drains output buffers
// and emits NV21 frames into frame_q.
// We can't reuse H264DecodePipe directly because its drain() writes
// straight to shm; here drain() needs to deposit into frame_q so the
// publisher can pace. So this is a slim local copy of the decode loop
// — same colour-format detection, same NV12→NV21 swap, output goes to
// frame_q instead of ShmOut.
struct DecoderState
{
  int w = 0, h = 0;
  AMediaCodec* dec = nullptr;
  std::string codec_name;
  int32_t color_format = 21, y_stride = 0, slice_height = 0, uv_stride = 0;
  uint64_t in_count = 0, out_count = 0;
  uint64_t output_layout_warns = 0;
  std::vector<uint8_t> nv21_scratch;
};

bool start_decoder(DecoderState& d, int w, int h)
{
  d.w = w;
  d.h = h;
  d.dec = AMediaCodec_createDecoderByType(OBF("video/avc").c_str());
  if (d.dec)
    d.codec_name = OBF("video/avc/byType").c_str();
  auto try_named_decoder = [&](const char* name) -> bool
  {
    if (d.dec)
      return true;
    d.dec = AMediaCodec_createCodecByName(name);
    if (!d.dec)
      return false;
    d.codec_name = name;
    return true;
  };
  try_named_decoder(OBF("c2.qti.avc.decoder").c_str());
  try_named_decoder(OBF("OMX.qcom.video.decoder.avc").c_str());
  if (!d.dec)
    return false;

  // Sprint A: max-input-size only. Other low-latency knobs are
  // silently broken on this device's vendor decoder — see the long
  // comment in tcp_h264_feed.cpp for the rationale.
  AMediaFormat* fmt = AMediaFormat_new();
  AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, OBF("video/avc").c_str());
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w);
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, h);
  AMediaFormat_setInt32(fmt, OBF("max-input-size").c_str(), 1 << 20);
  media_status_t cs = AMediaCodec_configure(d.dec, fmt, nullptr, nullptr, 0);
  AMediaFormat_delete(fmt);
  if (cs != AMEDIA_OK)
  {
    LOGE(OBF("v2/dec: configure failed status=%d").c_str(), (int) cs);
    AMediaCodec_delete(d.dec);
    d.dec = nullptr;
    return false;
  }
  media_status_t ss = AMediaCodec_start(d.dec);
  if (ss != AMEDIA_OK)
  {
    LOGE(OBF("v2/dec: start failed status=%d").c_str(), (int) ss);
    AMediaCodec_delete(d.dec);
    d.dec = nullptr;
    return false;
  }
  LOGI(OBF("v2/dec: started %dx%d (%s)").c_str(), w, h, d.codec_name.c_str());
  d.nv21_scratch.assign((size_t) w * h * 3 / 2, 0);
  return true;
}

void stop_decoder(DecoderState& d)
{
  if (d.dec)
  {
    AMediaCodec_stop(d.dec);
    AMediaCodec_delete(d.dec);
    d.dec = nullptr;
  }
}

void drain_decoder_into_q(DecoderState& d, cr_feed::BoundedQueue<cr_feed::Nv21Frame>& frame_q, std::atomic<uint64_t>& frame_dropped)
{
  constexpr int32_t kFmtYUV420Planar = 19;
  constexpr int32_t kFmtYUV420SemiPlanar = 21;
  constexpr int32_t kFmtYUV420PackedSemiPlanar = 0x7fa30c04;

  for (;;)
  {
    AMediaCodecBufferInfo info;
    ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(d.dec, &info, 0);
    if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
      break;
    if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
    {
      AMediaFormat* of = AMediaCodec_getOutputFormat(d.dec);
      AMediaFormat_getInt32(of, OBF("color-format").c_str(), &d.color_format);
      AMediaFormat_getInt32(of, OBF("stride").c_str(), &d.y_stride);
      AMediaFormat_getInt32(of, OBF("slice-height").c_str(), &d.slice_height);
      d.uv_stride = d.y_stride;
      LOGI(OBF("v2/dec out fmt: color=0x%x stride=%d slice=%d").c_str(), d.color_format, d.y_stride, d.slice_height);
      AMediaFormat_delete(of);
      continue;
    }
    if (out_idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
      continue;
    if (out_idx < 0)
      break;

    if (info.size > 0)
    {
      size_t cap = 0;
      uint8_t* out_buf = AMediaCodec_getOutputBuffer(d.dec, (size_t) out_idx, &cap);
      const int sy = d.y_stride ? d.y_stride : d.w;
      const int suv = d.uv_stride ? d.uv_stride : sy;
      const int slice = d.slice_height > 0 ? d.slice_height : d.h;
      cr::media::PlaneSpan window;
      const bool window_ok = out_buf && cr::media::valid_codec_window(
                                              cap, info.offset, info.size, window);
      const uint8_t* base = window_ok ? out_buf + window.offset : nullptr;
      const size_t available = window_ok ? window.bytes : 0;
      bool converted = false;

      if (!base || sy < d.w || suv < d.w || slice < d.h || sy <= 0 ||
          suv <= 0)
      {
        const uint64_t n = ++d.output_layout_warns;
        if (n <= 3 || (n % 120) == 0)
          LOGW(OBF("v2/dec: output layout rejected color=0x%x off=%d size=%d cap=%zu stride=%d uv=%d slice=%d dims=%dx%d").c_str(),
               d.color_format, (int) info.offset, (int) info.size, cap, sy,
               suv, slice, d.w, d.h);
      }
      else if (d.color_format == kFmtYUV420SemiPlanar ||
               d.color_format == kFmtYUV420PackedSemiPlanar)
      {
        cr::media::Yuv420Layout layout;
        if (cr::media::semiplanar_420(
                available, (uint32_t) d.w, (uint32_t) d.h, (uint32_t) sy,
                (uint32_t) slice, (uint32_t) suv, layout))
        {
          cr_feed::nv12_to_nv21(base, base + layout.u_or_uv.offset, d.w, d.h,
                                sy, suv, d.nv21_scratch.data());
          converted = true;
        }
      }
      else if (d.color_format == kFmtYUV420Planar)
      {
        const int chroma_stride = sy > 1 ? sy / 2 : d.w / 2;
        cr::media::Yuv420Layout layout;
        if (chroma_stride > 0 && cr::media::planar_420(
                                     available, (uint32_t) d.w,
                                     (uint32_t) d.h, (uint32_t) sy,
                                     (uint32_t) slice,
                                     (uint32_t) chroma_stride, layout))
        {
          cr_feed::i420_to_nv21(base, base + layout.u_or_uv.offset,
                                base + layout.v.offset, d.w, d.h, sy,
                                chroma_stride, chroma_stride,
                                d.nv21_scratch.data());
          converted = true;
        }
      }
      else
      {
        const uint64_t n = ++d.output_layout_warns;
        if (n <= 3 || (n % 120) == 0)
          LOGW(OBF("v2/dec: unsupported output format color=0x%x off=%d size=%d cap=%zu").c_str(),
               d.color_format, (int) info.offset, (int) info.size, cap);
      }

      if (converted)
      {
        cr_feed::Nv21Frame frame;
        frame.data = d.nv21_scratch; // copy, decoder reuses scratch
        frame.width = d.w;
        frame.height = d.h;
        frame.pts_us = (int64_t) info.presentationTimeUs;
        if (!frame_q.push(std::move(frame)))
          frame_dropped.fetch_add(1, std::memory_order_relaxed);
        ++d.out_count;
        if (d.out_count <= 3 || (d.out_count % 120) == 0)
          LOGI(OBF("v2/dec: frame #%llu (in=%llu)").c_str(), (unsigned long long) d.out_count, (unsigned long long) d.in_count);
      }
    }
    AMediaCodec_releaseOutputBuffer(d.dec, (size_t) out_idx, false);
  }
}

void decoder_loop(int w, int h, cr_feed::BoundedQueue<cr_feed::NalPacket>& pkt_q, cr_feed::BoundedQueue<cr_feed::Nv21Frame>& frame_q, std::atomic<uint64_t>& frame_dropped)
{
  DecoderState d;
  if (!start_decoder(d, w, h))
  {
    LOGE(OBF("v2/dec: start failed for %dx%d").c_str(), w, h);
    frame_q.close();
    return;
  }
  bool need_keyframe = false;

  while (!g_stop.load(std::memory_order_relaxed))
  {
    cr_feed::NalPacket pkt;
    if (!pkt_q.pop(pkt))
      break;
    if (need_keyframe && !pkt.is_key)
      continue;

    ssize_t in_idx = AMediaCodec_dequeueInputBuffer(d.dec, 20 * 1000);
    if (in_idx >= 0)
    {
      size_t cap = 0;
      uint8_t* in_buf = AMediaCodec_getInputBuffer(d.dec, (size_t) in_idx, &cap);
      if (in_buf && cap >= pkt.data.size())
      {
        memcpy(in_buf, pkt.data.data(), pkt.data.size());
        const media_status_t queued = AMediaCodec_queueInputBuffer(
            d.dec, (size_t) in_idx, 0, pkt.data.size(),
            pkt.pts_us < 0 ? 0u : (uint64_t) pkt.pts_us, 0);
        if (queued == AMEDIA_OK)
        {
          ++d.in_count;
          if (pkt.is_key)
            need_keyframe = false;
        }
        else
        {
          LOGW(OBF("v2/dec: queueInput failed status=%d").c_str(),
               (int) queued);
          AMediaCodec_flush(d.dec);
          need_keyframe = true;
        }
      }
      else
      {
        LOGW(OBF("v2/dec: input buf too small: cap=%zu need=%zu").c_str(), cap, pkt.data.size());
        AMediaCodec_queueInputBuffer(d.dec, (size_t) in_idx, 0, 0,
                                     pkt.pts_us < 0 ? 0u : (uint64_t) pkt.pts_us,
                                     0);
        AMediaCodec_flush(d.dec);
        need_keyframe = true;
      }
    }
    else
    {
      // Losing an encoded access unit invalidates inter-frame references.
      AMediaCodec_flush(d.dec);
      need_keyframe = true;
    }
    drain_decoder_into_q(d, frame_q, frame_dropped);
  }
  drain_decoder_into_q(d, frame_q, frame_dropped);
  stop_decoder(d);
  frame_q.close();
}

// Publish thread: pops Nv21Frames, paces by PTS, writes to /data/cr/feed.
// PTS pacing is conservative: anchor on first frame's PTS, target a
// constant per-frame wall-clock delta. Skip pacing if we're more than
// 100 ms behind so we never accumulate a long tail.
void publish_loop(cr_feed::BoundedQueue<cr_feed::Nv21Frame>& frame_q, cr_feed::ShmOut& shm)
{
  int64_t pts_anchor_us = -1;
  int64_t wall_anchor_ns = 0;

  auto now_ns = []() -> int64_t
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1'000'000'000ll + ts.tv_nsec;
  };

  while (!g_stop.load(std::memory_order_relaxed))
  {
    cr_feed::Nv21Frame frame;
    if (!frame_q.pop(frame))
      break;

    if (pts_anchor_us < 0)
    {
      pts_anchor_us = frame.pts_us;
      wall_anchor_ns = now_ns();
    }
    else
    {
      const uint64_t pts_delta_us =
          static_cast<uint64_t>(frame.pts_us) -
          static_cast<uint64_t>(pts_anchor_us);
      if (frame.pts_us < pts_anchor_us ||
          pts_delta_us >
              static_cast<uint64_t>(std::numeric_limits<int64_t>::max() /
                                    1000ll))
      {
        pts_anchor_us = frame.pts_us;
        wall_anchor_ns = now_ns();
        shm.publish_nv21(frame.data.data());
        continue;
      }
      const int64_t pts_delta_ns = static_cast<int64_t>(pts_delta_us) * 1000ll;
      if (wall_anchor_ns > std::numeric_limits<int64_t>::max() - pts_delta_ns)
      {
        pts_anchor_us = frame.pts_us;
        wall_anchor_ns = now_ns();
        shm.publish_nv21(frame.data.data());
        continue;
      }
      int64_t target_ns = wall_anchor_ns + pts_delta_ns;
      int64_t delta_ns = target_ns - now_ns();
      if (delta_ns > 100'000'000ll)
      {
        // > 100 ms — re-anchor (we got too far ahead, e.g. after
        // a decoder burst).
        pts_anchor_us = frame.pts_us;
        wall_anchor_ns = now_ns();
      }
      else if (delta_ns > 0)
      {
        struct timespec rq
        {
          delta_ns / 1'000'000'000ll, delta_ns % 1'000'000'000ll
        };
        nanosleep(&rq, nullptr);
      }
      // delta_ns < 0 → already late; publish immediately.
    }
    shm.publish_nv21(frame.data.data());
  }
}

// Per-connection orchestrator. Mirrors serve_client() in the legacy file
// but spins up the three threads instead of running a serial loop.
int serve_client(int client)
{
  int nodelay = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  uint32_t hdr[4] = {};
  if (!recv_all(client, hdr, sizeof(hdr)))
  {
    LOGW(OBF("v2: short header").c_str());
    return -1;
  }
  if (hdr[0] != kMagic)
  {
    LOGE(OBF("v2: bad magic 0x%x").c_str(), hdr[0]);
    return -1;
  }
  const int w = (int) hdr[1];
  const int h = (int) hdr[2];
  const int fps = (int) hdr[3];
  if (!nv21_shape_supported(w, h))
    return -1;
  LOGI(OBF("v2 header: %dx%d %dfps").c_str(), w, h, fps);

  cr_feed::ShmOut shm;
  if (!shm.init(w, h))
  {
    LOGE(OBF("v2: shm init failed").c_str());
    return -1;
  }

  cr_feed::BoundedQueue<cr_feed::NalPacket> pkt_q(kPacketQueueCap, cr_feed::BoundedQueue<cr_feed::NalPacket>::kDropOldestNonKey);
  cr_feed::BoundedQueue<cr_feed::Nv21Frame> frame_q(kFrameQueueCap, cr_feed::BoundedQueue<cr_feed::Nv21Frame>::kDropOldest);

  std::atomic<uint64_t> pkt_dropped{0}, frame_dropped{0};
  std::atomic<bool> worker_failed{false};

  auto fail_worker = [&](const char* worker, const char* detail)
  {
    worker_failed.store(true, std::memory_order_release);
    LOGE(OBF("v2/%s: worker failed: %s").c_str(), worker,
         detail ? detail : OBF("unknown").c_str());
    pkt_q.close();
    frame_q.close();
    shutdown(client, SHUT_RDWR);
  };
  std::thread t_dec([&]
                    {
                      try
                      {
                        decoder_loop(w, h, pkt_q, frame_q, frame_dropped);
                      }
                      catch (const std::exception& e)
                      {
                        fail_worker(OBF("decoder").c_str(), e.what());
                      }
                      catch (...)
                      {
                        fail_worker(OBF("decoder").c_str(), OBF("non-standard exception").c_str());
                      }
                    });
  std::thread t_pub([&]
                    {
                      try
                      {
                        publish_loop(frame_q, shm);
                      }
                      catch (const std::exception& e)
                      {
                        fail_worker(OBF("publisher").c_str(), e.what());
                      }
                      catch (...)
                      {
                        fail_worker(OBF("publisher").c_str(), OBF("non-standard exception").c_str());
                      }
                    });

  // Network reads on this thread (we already own its lifetime here).
  network_loop(client, pkt_q, pkt_dropped);

  pkt_q.close();
  if (t_dec.joinable())
    t_dec.join();
  frame_q.close();
  if (t_pub.joinable())
    t_pub.join();

  LOGI(OBF("v2: client gone (drops pkt=%llu frame=%llu)").c_str(), (unsigned long long) pkt_dropped.load(), (unsigned long long) frame_dropped.load());
  shm.shutdown();
  return worker_failed.load(std::memory_order_acquire) ? -1 : 0;
}

} // namespace

extern "C" int cr_feed_run_tcp_h264(int port);

int cr_feed_run_tcp_h264(int port)
{
  int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (srv < 0)
  {
    LOGE(OBF("v2: socket: %s").c_str(), strerror(errno));
    return -1;
  }
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t) port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(srv, (sockaddr*) &addr, sizeof(addr)) != 0)
  {
    LOGE(OBF("v2: bind :%d: %s").c_str(), port, strerror(errno));
    close(srv);
    return -1;
  }
  if (listen(srv, 1) != 0)
  {
    LOGE(OBF("v2: listen: %s").c_str(), strerror(errno));
    close(srv);
    return -1;
  }
  LOGI(OBF("v2 tcp_h264 feed listening on 127.0.0.1:%d").c_str(), port);

  while (!g_stop.load(std::memory_order_relaxed))
  {
    int client = cr::feed::accept_interruptible(srv, g_stop);
    if (client < 0)
    {
      if (g_stop.load(std::memory_order_relaxed))
        break;
      if (errno == EINTR)
        continue;
      LOGW(OBF("v2: accept: %s").c_str(), strerror(errno));
      break;
    }
    LOGI(OBF("v2: client connected").c_str());
    serve_client(client);
    close(client);
  }

  close(srv);
  return 0;
}
