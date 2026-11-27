/*
 * Назначение файла:
 *   TCP H.264 receiver для compressed transport. Host присылает CRH2 header
 *   и Annex-B H.264 chunks, этот процесс декодирует через AMediaCodec,
 *   конвертирует output в NV21 и публикует кадры в /data/cr/feed.
 *
 * ABI/совместимость:
 *   The shared ring keeps separate FHD and UHD slot regions.
 *   Input above the ring's pixel budget is rejected before starting the decoder.
 *
 * Pixel/Android ограничения:
 *   Нельзя считать устройство Qualcomm. Основной decoder selection идёт через
 *   AMediaCodec_createDecoderByType("video/avc"), vendor names используются
 *   только как fallback для старых Qualcomm test devices.
 */

// TCP H.264 feed mode.
// cr_feed_run_tcp_h264(port) binds 127.0.0.1:port and speaks the "CRH2"
// protocol that the PC's TcpFeedClient::send_h264_* emits:
//   header (16 bytes, LE):
//     uint32 magic  = 'CRH2'
//     uint32 width
//     uint32 height
//     uint32 fps
//   per packet (16-byte prefix + payload):
//     uint32 length
//     uint32 flags       (bit 0 = keyframe)
//     int64  pts_us
//     byte[length]       Annex-B H.264 NAL unit(s)
// We feed packets into AMediaCodec "video/avc" (CPU-output path), convert
// the decoded NV12 slices to NV21 and publish into /data/cr/feed exactly
// like the raw-NV21 path — the hook doesn't care where the NV21 came from.
// The reason this mode exists: USB 2.0 caps raw-NV21 throughput at roughly
// 20 fps @ 720p. Compressed H.264 is 10-20× smaller on the wire, so 60 fps
// @ 720p is trivially reachable over the same transport.

#include "../include/cr_feed.h"
#include "shm.h"
#include "secure_channel_reader.h"
#include "interruptible_socket.h"
#include "media/FrameLayout.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>
#include "util/Obf.h"
#define TAG "cr_feed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern std::atomic<bool> g_stop; // shared with feed_main.cpp

namespace
{

constexpr uint32_t kMagic = 0x32484243u; // 'CRH2' LE
constexpr int64_t kMaxLiveLagMs = 180;
constexpr int64_t kKeyInputWaitUs = 5 * 1000;

bool nv21_shape_supported(int w, int h, const char* tag)
{
  if (w < 16 || w > (int) CR_FEED_MAX_WIDTH || h < 16 || h > (int) CR_FEED_MAX_HEIGHT ||
      (w & 1) != 0 || (h & 1) != 0)
  {
    LOGE(OBF("%s: bogus dims %dx%d").c_str(), tag, w, h);
    return false;
  }
  const uint64_t bytes = (uint64_t) w * (uint64_t) h * 3ull / 2ull;
  if (bytes > CR_FEED_MAX_NV21_BYTES)
  {
    LOGE(OBF("%s: %dx%d NV21 needs %llu bytes, shared slot cap is %u bytes").c_str(), tag, w, h, (unsigned long long) bytes, CR_FEED_MAX_NV21_BYTES);
    return false;
  }
  return true;
}

// OpenMAX / MediaCodec colour-format constants — same set we use in
// video_feed.cpp's software path.
constexpr int32_t kFmtYUV420Planar = 19;     // I420
constexpr int32_t kFmtYUV420SemiPlanar = 21; // NV12
constexpr int32_t kFmtYUV420PackedSemiPlanar = 0x7fa30c04;

// /data/cr/feed writer. Identical contract to the raw NV21 path: a stable-
// size reserved file, grown-only, that the hook's mmap can safely hold.
struct ShmOut
{
  int fd = -1;
  void* map = nullptr;
  size_t mapped = 0;
  int w = 0;
  int h = 0;
  size_t slot_bytes = 0;
  uint32_t cur_wi = 0; // last index we published into
  bool ready_logged = false;

  ~ShmOut() { shutdown(OBF("scope exit").c_str()); }
  ShmOut() = default;
  ShmOut(const ShmOut&) = delete;
  ShmOut& operator=(const ShmOut&) = delete;

  bool init(int ww, int hh)
  {
    mkdir(OBF("/data/cr").c_str(), 0711);
    slot_bytes = (size_t) ww * hh * 3 / 2;
    if (slot_bytes > CR_FEED_MAX_NV21_BYTES)
    {
      LOGE(OBF("h264 shm init rejected %dx%d: %zu > %u").c_str(), ww, hh, slot_bytes, CR_FEED_MAX_NV21_BYTES);
      return false;
    }
    // Reserve enough for N slots up to the maximum NV21 size we ever
    // accept. Grow-only — never shrink, so an existing camhook mmap
    // stays valid even if the previous run reserved more.
    const size_t reserve = CR_FEED_MAP_RESERVE;

    fd = open(OBF("/data/cr/feed").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
      return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
      LOGE(OBF("h264 shm writer lease is already owned: %s").c_str(), strerror(errno));
      close(fd);
      fd = -1;
      return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0)
    {
      close(fd);
      fd = -1;
      return false;
    }
    size_t map_size = reserve;
    if ((size_t) st.st_size < reserve)
    {
      if (ftruncate(fd, (off_t) reserve) != 0)
      {
        close(fd);
        fd = -1;
        return false;
      }
    }
    else
    {
      // File already bigger from a previous build — keep that mapping
      // size so existing hook mmaps don't get truncated underneath them.
      map_size = (size_t) st.st_size;
    }
    map = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
    {
      map = nullptr;
      close(fd);
      fd = -1;
      return false;
    }
    mapped = map_size;
    w = ww;
    h = hh;

    auto* hd = (cr_feed_header*) map;
    const bool ownership_compatible = cr_feed_ownership_layout_valid(hd);
    const uint32_t prev_gen =
        ownership_compatible ? atomic_load(&hd->generation) : 0u;
    const uint32_t prev_wi =
        ownership_compatible ? atomic_load(&hd->write_index) : 0u;
    __atomic_store_n(&hd->magic, 0u, __ATOMIC_RELEASE);

    if (ownership_compatible)
      cr_feed_recover_dead_writer(hd);
    else
      cr_feed_reset_slot_states(hd);

    __atomic_store_n(&hd->version, CR_FEED_SHM_VERSION, __ATOMIC_RELAXED);
    __atomic_store_n(&hd->width, (uint32_t) ww, __ATOMIC_RELAXED);
    __atomic_store_n(&hd->height, (uint32_t) hh, __ATOMIC_RELAXED);
    __atomic_store_n(&hd->stride, (uint32_t) ww, __ATOMIC_RELAXED);
    __atomic_store_n(&hd->slot_size, (uint32_t) slot_bytes, __ATOMIC_RELAXED);
    __atomic_store_n(&hd->num_slots, CR_FEED_NUM_SLOTS, __ATOMIC_RELAXED);
    __atomic_store_n(&hd->format, 0u, __ATOMIC_RELAXED); // NV21
    atomic_store(&hd->write_index, prev_wi % CR_FEED_NUM_SLOTS);
    atomic_store(&hd->generation, cr_feed_next_generation(prev_gen));
    atomic_store(&hd->frame_counter, 0ull);
    atomic_store(&hd->channel_state, CR_CHANNEL_STATE_NONE);
    cur_wi = prev_wi % CR_FEED_NUM_SLOTS;

    __atomic_store_n(&hd->magic, CR_FEED_MAGIC, __ATOMIC_RELEASE);
    return true;
  }

  // Claim a slot with no active camera readers. If all non-current slots
  // are pinned, retain the last complete frame instead of tearing it.
  void publish_nv21(const uint8_t* src)
  {
    if (!map)
      return;
    auto* hd = (cr_feed_header*) map;
    uint32_t next = 0;
    if (!cr_feed_claim_write(hd, cur_wi, &next))
      return;
    memcpy(static_cast<uint8_t*>(map) + cr_feed_slot_offset(slot_bytes, next), src, slot_bytes);
    cr_feed_publish(hd, next);
    atomic_store_explicit(&hd->channel_state, CR_CHANNEL_STATE_READY,
                          memory_order_release);
    if (!ready_logged)
    {
      LOGI(OBF("h264 shm READY transition on first publish (%dx%d slot=%zu)").c_str(), w, h, slot_bytes);
      ready_logged = true;
    }
    cur_wi = next;
  }

  void shutdown(const char* reason)
  {
    if (map)
    {
      if (mapped >= sizeof(cr_feed_header))
      {
        auto* hd = (cr_feed_header*) map;
        const uint32_t prev = atomic_load(&hd->channel_state);
        atomic_store(&hd->channel_state, CR_CHANNEL_STATE_NONE);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        LOGI(OBF("h264 shm channel_state %u -> NONE teardown=%s").c_str(), prev, reason ? reason : OBF("unknown").c_str());
      }
      munmap(map, mapped);
      map = nullptr;
      mapped = 0;
    }
    if (fd >= 0)
    {
      close(fd);
      fd = -1;
    }
  }
};

// NV12 → NV21: swap every chroma pair. Source + dest are the same shape
// (codec doesn't resample for us — its output is width×height).
void nv12_to_nv21_planes(const uint8_t* y_plane, const uint8_t* uv_plane, int ww, int hh, int src_y_stride, int src_uv_stride, uint8_t* dst)
{
  for (int y = 0; y < hh; ++y)
  {
    memcpy(dst + (size_t) y * ww, y_plane + (size_t) y * src_y_stride, (size_t) ww);
  }
  uint8_t* dst_vu = dst + (size_t) ww * hh;
  const int uv_rows = hh / 2;
  for (int y = 0; y < uv_rows; ++y)
  {
    const uint8_t* s = uv_plane + (size_t) y * src_uv_stride;
    uint8_t* d = dst_vu + (size_t) y * ww;
    for (int x = 0; x < ww; x += 2)
    {
      d[x + 0] = s[x + 1]; // V ← was U in NV12 pair
      d[x + 1] = s[x + 0]; // U
    }
  }
}

void i420_to_nv21_planes(const uint8_t* y_plane, const uint8_t* u_plane, const uint8_t* v_plane, int ww, int hh, int src_y_stride, int src_u_stride, int src_v_stride, uint8_t* dst)
{
  for (int y = 0; y < hh; ++y)
  {
    memcpy(dst + (size_t) y * ww, y_plane + (size_t) y * src_y_stride, (size_t) ww);
  }
  uint8_t* dst_vu = dst + (size_t) ww * hh;
  for (int y = 0; y < hh / 2; ++y)
  {
    const uint8_t* u = u_plane + (size_t) y * src_u_stride;
    const uint8_t* v = v_plane + (size_t) y * src_v_stride;
    uint8_t* out = dst_vu + (size_t) y * ww;
    for (int x = 0; x < ww / 2; ++x)
    {
      out[x * 2 + 0] = v[x];
      out[x * 2 + 1] = u[x];
    }
  }
}

// Decoder session — handles one connected client end-to-end.
int serve_client(int client)
{
  // Reader thread already set TCP_NODELAY on the PC end. Mirror here so
  // replies (none, but future-proof) don't coalesce.
  int nodelay = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  cr::feed::SecureChannelReader channel;
  if (!channel.handshake(client, cr::secure_channel::v2::StreamKind::Video))
  {
    LOGW(OBF("h264: secure-channel handshake rejected").c_str());
    return -1;
  }

  // --- Header ------------------------------------------------------------
  uint32_t hdr[4] = {};
  if (!channel.read_exact(hdr, sizeof(hdr)))
  {
    LOGW(OBF("h264: short header").c_str());
    return -1;
  }
  if (hdr[0] != kMagic)
  {
    LOGE(OBF("h264: bad magic 0x%x").c_str(), hdr[0]);
    return -1;
  }
  const int w = (int) hdr[1];
  const int h = (int) hdr[2];
  const int fps = (int) hdr[3];
  if (!nv21_shape_supported(w, h, OBF("h264").c_str()))
    return -1;
  LOGI(OBF("h264 header: %dx%d %dfps").c_str(), w, h, fps);

  // --- Decoder ----------------------------------------------------------
  // byType first keeps Tensor/Exynos/Qualcomm selection vendor-neutral.
  AMediaCodec* dec = nullptr;
  std::string chosen;
  dec = AMediaCodec_createDecoderByType(OBF("video/avc").c_str());
  if (dec)
  {
    chosen = OBF("video/avc/byType");
  }

  auto try_named_decoder = [&](const char* name) -> bool
  {
    if (dec)
      return true;
    dec = AMediaCodec_createCodecByName(name);
    if (!dec)
      return false;
    chosen = name;
    return true;
  };
  try_named_decoder(OBF("c2.qti.avc.decoder").c_str());
  try_named_decoder(OBF("OMX.qcom.video.decoder.avc").c_str());
  if (!dec)
  {
    LOGE(OBF("h264: no decoder available").c_str());
    return -1;
  }
  LOGI(OBF("h264: decoder = %s for %dx%d").c_str(), chosen.c_str(), w, h);

  // --- Sprint A: max-input-size only ---
  // We tested low-latency=1 / operating-rate=240 / priority=0 on this
  // device (Snapdragon 665, OMX.qcom.video.decoder.avc, Android 12) —
  // configure() and start() both returned AMEDIA_OK but every later
  // dequeue spammed "sf error code: -38" (INVALID_OPERATION) and no
  // frames came out. The vendor decoder accepts the keys at configure
  // time and silently breaks downstream. Until we have a runtime probe
  // that catches that and rolls back, only max-input-size stays —
  // it's been in MediaFormat since API 16 and is universally safe.
  AMediaFormat* fmt = AMediaFormat_new();
  AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, OBF("video/avc").c_str());
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w);
  AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, h);
  AMediaFormat_setInt32(fmt, OBF("max-input-size").c_str(), 1 << 20);
  media_status_t cs = AMediaCodec_configure(dec, fmt, nullptr, nullptr, 0);
  AMediaFormat_delete(fmt);
  if (cs != AMEDIA_OK)
  {
    LOGE(OBF("h264: configure failed status=%d").c_str(), (int) cs);
    AMediaCodec_delete(dec);
    return -1;
  }
  media_status_t ss = AMediaCodec_start(dec);
  if (ss != AMEDIA_OK)
  {
    LOGE(OBF("h264: start failed status=%d").c_str(), (int) ss);
    AMediaCodec_delete(dec);
    return -1;
  }
  LOGI(OBF("h264: started %dx%d (%s)").c_str(), w, h, chosen.c_str());

  // --- SHM writer (inits with the advertised size) ----------------------
  ShmOut shm;
  if (!shm.init(w, h))
  {
    AMediaCodec_stop(dec);
    AMediaCodec_delete(dec);
    return -1;
  }

  std::vector<uint8_t> packet;
  std::vector<uint8_t> nv21((size_t) w * h * 3 / 2);

  int32_t color_format = kFmtYUV420SemiPlanar;
  int32_t y_stride = w;
  int32_t slice_height = h;
  int32_t uv_stride = w;

  uint64_t in_count = 0, out_count = 0;

  // Freeze diagnostics: track wall-clock time of the previous packet
  // arrival and previous decoded-frame publish. If either gap exceeds
  // 500 ms we log it once — that pinpoints whether the freeze is on the
  // wire (network/USB) or inside the codec.
  auto now_ms = []() -> int64_t
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1'000'000;
  };
  int64_t last_pkt_ms = now_ms();
  int64_t last_pub_ms = last_pkt_ms;
  int64_t pts_anchor_us = -1;
  int64_t wall_anchor_ms = 0;
  uint64_t dropped_late = 0;
  uint64_t dropped_busy = 0;
  uint64_t dropped_until_key = 0;
  uint64_t output_layout_warns = 0;
  bool need_keyframe = false;

  auto enter_need_keyframe = [&](const char* reason, bool flush_decoder)
  {
    if (!need_keyframe && flush_decoder)
    {
      const media_status_t fs = AMediaCodec_flush(dec);
      LOGW(OBF("h264 decoder desync: need_keyframe=1 reason=%s flush=%d in=%llu out=%llu late=%llu busy=%llu").c_str(),
           reason ? reason : OBF("unknown").c_str(),
           (int) fs,
           (unsigned long long) in_count,
           (unsigned long long) out_count,
           (unsigned long long) dropped_late,
           (unsigned long long) dropped_busy);
    }
    need_keyframe = true;
  };

  while (!g_stop.load(std::memory_order_relaxed))
  {
    // --- 1. Receive one packet header ---------------------------------
    struct
    {
      uint32_t len;
      uint32_t flags;
      int64_t pts_us;
    } ph;
    if (!channel.read_exact(&ph, sizeof(ph)))
      break;

    const int64_t arrived = now_ms();
    if (in_count > 0 && (arrived - last_pkt_ms) > 500)
    {
      LOGW(OBF("h264 STALL on wire: %lldms since last packet (likely OBS / USB / adb-forward)").c_str(), (long long) (arrived - last_pkt_ms));
    }
    last_pkt_ms = arrived;

    if (ph.len == 0 || ph.len > 16u * 1024u * 1024u)
    {
      LOGE(OBF("h264: bogus packet len %u").c_str(), ph.len);
      break;
    }
    packet.resize(ph.len);
    if (!channel.read_exact(packet.data(), ph.len))
      break;
    const bool is_keyframe = (ph.flags & 1u) != 0;
    bool skip_input = false;

    if (pts_anchor_us < 0)
    {
      pts_anchor_us = ph.pts_us;
      wall_anchor_ms = arrived;
    }
    else
    {
      const int64_t stream_elapsed_ms = (ph.pts_us - pts_anchor_us) / 1000;
      const int64_t wall_elapsed_ms = arrived - wall_anchor_ms;
      const int64_t live_lag_ms = wall_elapsed_ms - stream_elapsed_ms;
      if (live_lag_ms > kMaxLiveLagMs)
      {
        if (is_keyframe)
        {
          LOGW(OBF("h264 lag=%lldms but keyframe is kept (need_keyframe=%d in=%llu out=%llu)").c_str(),
               (long long) live_lag_ms,
               need_keyframe ? 1 : 0,
               (unsigned long long) in_count,
               (unsigned long long) out_count);
          // The keyframe is our recovery boundary. Without re-anchoring here,
          // every following inter frame is still measured against the stale
          // pre-stall clock and is dropped forever, despite a valid IDR.
          pts_anchor_us = ph.pts_us;
          wall_anchor_ms = arrived;
        }
        else
        {
          ++dropped_late;
          if (dropped_late <= 3 || (dropped_late % 120) == 0)
          {
            LOGW(OBF("h264 low-latency drop non-key: lag=%lldms late=%llu need_keyframe=%d pts=%lld in=%llu out=%llu").c_str(),
                 (long long) live_lag_ms,
                 (unsigned long long) dropped_late,
                 need_keyframe ? 1 : 0,
                 (long long) ph.pts_us,
                 (unsigned long long) in_count,
                 (unsigned long long) out_count);
          }
          enter_need_keyframe(OBF("late-drop").c_str(), true);
          skip_input = true;
        }
      }
    }

    if (need_keyframe && !is_keyframe)
    {
      ++dropped_until_key;
      if (dropped_until_key <= 3 || (dropped_until_key % 120) == 0)
      {
        LOGW(OBF("h264 waiting for IDR: drop non-key wait=%llu late=%llu busy=%llu pts=%lld in=%llu out=%llu").c_str(),
             (unsigned long long) dropped_until_key,
             (unsigned long long) dropped_late,
             (unsigned long long) dropped_busy,
             (long long) ph.pts_us,
             (unsigned long long) in_count,
             (unsigned long long) out_count);
      }
      skip_input = true;
    }

    // --- 2. Feed the packet into the decoder --------------------------
    ssize_t in_idx = skip_input ? -1 : AMediaCodec_dequeueInputBuffer(dec, is_keyframe ? kKeyInputWaitUs : 0);
    if (!skip_input && in_idx >= 0)
    {
      size_t cap = 0;
      uint8_t* in_buf = AMediaCodec_getInputBuffer(dec, (size_t) in_idx, &cap);
      if (in_buf && cap >= packet.size())
      {
        memcpy(in_buf, packet.data(), packet.size());
        uint32_t cflags = 0;
        // We never set BUFFER_FLAG_CODEC_CONFIG explicitly because
        // the encoder inlines SPS/PPS at the head of every keyframe
        // packet on our wire. MediaCodec autodetects that fine.
        const media_status_t queued = AMediaCodec_queueInputBuffer(
            dec, (size_t) in_idx, 0, packet.size(), (uint64_t) ph.pts_us,
            cflags);
        if (queued == AMEDIA_OK)
        {
          ++in_count;
          if (is_keyframe)
          {
            if (need_keyframe)
            {
              LOGI(OBF("h264 recovered on IDR pts=%lld in=%llu out=%llu late=%llu busy=%llu wait=%llu").c_str(),
                   (long long) ph.pts_us,
                   (unsigned long long) in_count,
                   (unsigned long long) out_count,
                   (unsigned long long) dropped_late,
                   (unsigned long long) dropped_busy,
                   (unsigned long long) dropped_until_key);
            }
            need_keyframe = false;
          }
        }
        else
        {
          ++dropped_busy;
          LOGW(OBF("h264: queueInput failed status=%d key=%d bytes=%zu").c_str(),
               (int) queued, is_keyframe ? 1 : 0, packet.size());
          enter_need_keyframe(OBF("queue-input-failed").c_str(), true);
        }
      }
      else
      {
        ++dropped_busy;
        LOGW(OBF("h264: input buf too small key=%d cap=%zu need=%zu pts=%lld need_keyframe=%d").c_str(),
             is_keyframe ? 1 : 0,
             cap,
             packet.size(),
             (long long) ph.pts_us,
             need_keyframe ? 1 : 0);
        // dequeueInputBuffer transferred one codec slot to us. Queue an empty
        // buffer to return ownership before flushing/resetting for the next
        // IDR; otherwise repeated oversize packets can exhaust every slot.
        const media_status_t returned = AMediaCodec_queueInputBuffer(
            dec, (size_t) in_idx, 0, 0, (uint64_t) ph.pts_us, 0);
        if (returned != AMEDIA_OK)
          LOGW(OBF("h264: could not return undersized input slot status=%d").c_str(),
               (int) returned);
        enter_need_keyframe(OBF("input-buffer-too-small").c_str(), true);
      }
    }
    else if (!skip_input)
    {
      ++dropped_busy;
      if (is_keyframe || dropped_busy <= 3 || (dropped_busy % 120) == 0)
      {
        LOGW(OBF("h264 low-latency drop: no input buffer key=%d busy=%llu pts=%lld need_keyframe=%d in=%llu out=%llu").c_str(),
             is_keyframe ? 1 : 0,
             (unsigned long long) dropped_busy,
             (long long) ph.pts_us,
             need_keyframe ? 1 : 0,
             (unsigned long long) in_count,
             (unsigned long long) out_count);
      }
      enter_need_keyframe(is_keyframe ? OBF("keyframe-input-busy").c_str() : OBF("input-busy").c_str(), true);
    }

    // --- 3. Drain whatever output is ready ----------------------------
    for (;;)
    {
      AMediaCodecBufferInfo info;
      ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(dec, &info, 0);
      if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        break;

      if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
      {
        AMediaFormat* of = AMediaCodec_getOutputFormat(dec);
        AMediaFormat_getInt32(of, OBF("color-format").c_str(), &color_format);
        AMediaFormat_getInt32(of, OBF("stride").c_str(), &y_stride);
        AMediaFormat_getInt32(of, OBF("slice-height").c_str(), &slice_height);
        uv_stride = y_stride;
        LOGI(OBF("h264 out fmt: color=0x%x stride=%d slice=%d").c_str(), color_format, y_stride, slice_height);
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
        uint8_t* out_buf = AMediaCodec_getOutputBuffer(dec, (size_t) out_idx, &cap);
        const int sy = y_stride ? y_stride : w;
        const int suv = uv_stride ? uv_stride : sy;
        const int slice = slice_height > 0 ? slice_height : h;
        cr::media::PlaneSpan window;
        const bool window_ok = out_buf && cr::media::valid_codec_window(
                                                cap, info.offset, info.size, window);
        const uint8_t* base = window_ok ? out_buf + window.offset : nullptr;
        const size_t available = window_ok ? window.bytes : 0;
        bool converted = false;

        if (!base || sy < w || suv < w || slice < h || sy <= 0 || suv <= 0)
        {
          const uint64_t lw = ++output_layout_warns;
          if (lw <= 3 || (lw % 120) == 0)
          {
            LOGW(OBF("h264 output layout rejected: color=0x%x off=%d size=%d cap=%zu stride=%d uv=%d slice=%d dims=%dx%d").c_str(),
                 color_format,
                 (int) info.offset,
                 (int) info.size,
                 cap,
                 sy,
                 suv,
                 slice,
                 w,
                 h);
          }
        }
        else if (color_format == kFmtYUV420SemiPlanar ||
                 color_format == kFmtYUV420PackedSemiPlanar)
        {
          cr::media::Yuv420Layout layout;
          if (cr::media::semiplanar_420(
                  available, (uint32_t) w, (uint32_t) h, (uint32_t) sy,
                  (uint32_t) slice, (uint32_t) suv, layout))
          {
            nv12_to_nv21_planes(base, base + layout.u_or_uv.offset, w, h,
                                sy, suv, nv21.data());
            converted = true;
          }
          else
          {
            const uint64_t lw = ++output_layout_warns;
            if (lw <= 3 || (lw % 120) == 0)
              LOGW(OBF("h264 semiplanar layout rejected: valid=%zu off=%d size=%d cap=%zu stride=%d uv=%d slice=%d").c_str(),
                   available, (int) info.offset, (int) info.size, cap, sy, suv,
                   slice);
          }
        }
        else if (color_format == kFmtYUV420Planar)
        {
          const int chroma_stride = sy > 1 ? sy / 2 : w / 2;
          cr::media::Yuv420Layout layout;
          if (chroma_stride > 0 && cr::media::planar_420(
                                       available, (uint32_t) w, (uint32_t) h,
                                       (uint32_t) sy, (uint32_t) slice,
                                       (uint32_t) chroma_stride, layout))
          {
            i420_to_nv21_planes(base, base + layout.u_or_uv.offset,
                                base + layout.v.offset, w, h, sy,
                                chroma_stride, chroma_stride, nv21.data());
            converted = true;
          }
          else
          {
            const uint64_t lw = ++output_layout_warns;
            if (lw <= 3 || (lw % 120) == 0)
              LOGW(OBF("h264 planar layout rejected: valid=%zu off=%d size=%d cap=%zu stride=%d chroma=%d slice=%d").c_str(),
                   available, (int) info.offset, (int) info.size, cap, sy,
                   chroma_stride, slice);
          }
        }
        else
        {
          // Flexible YUV needs explicit plane metadata; proprietary/tiled
          // formats need a verified adapter. Guessing UV offsets or publishing
          // a grey frame would turn an unsupported layout into corrupted
          // replacement, so keep the real camera frame instead.
          const uint64_t lw = ++output_layout_warns;
          if (lw <= 3 || (lw % 120) == 0)
          {
            LOGW(OBF("h264 unsupported output format: color=0x%x off=%d size=%d cap=%zu stride=%d uv=%d slice=%d").c_str(),
                 color_format, (int) info.offset, (int) info.size, cap, sy, suv,
                 slice);
          }
        }

        if (converted)
        {
          const int64_t pub = now_ms();
          if (out_count > 0 && (pub - last_pub_ms) > 500)
          {
            LOGW(OBF("h264 STALL in decoder: %lldms since last decoded frame (need_keyframe=%d late=%llu busy=%llu in=%llu out=%llu pts=%lld key=%d)").c_str(),
                 (long long) (pub - last_pub_ms), need_keyframe ? 1 : 0,
                 (unsigned long long) dropped_late,
                 (unsigned long long) dropped_busy,
                 (unsigned long long) in_count,
                 (unsigned long long) out_count,
                 (long long) info.presentationTimeUs,
                 (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) ? 1 : 0);
          }
          last_pub_ms = pub;

          shm.publish_nv21(nv21.data());
          ++out_count;
          if (out_count <= 3 || (out_count % 120) == 0)
          {
            LOGI(OBF("h264: decoded frame #%llu (in=%llu fmt=0x%x off=%d size=%d stride=%d uv=%d slice=%d)").c_str(),
                 (unsigned long long) out_count,
                 (unsigned long long) in_count, color_format,
                 (int) info.offset, (int) info.size, sy, suv, slice);
          }
        }
      }
      AMediaCodec_releaseOutputBuffer(dec, (size_t) out_idx, false);
    }
  }

  LOGI(OBF("h264: client gone (in=%llu out=%llu need_keyframe=%d late=%llu busy=%llu wait=%llu)").c_str(),
       (unsigned long long) in_count,
       (unsigned long long) out_count,
       need_keyframe ? 1 : 0,
       (unsigned long long) dropped_late,
       (unsigned long long) dropped_busy,
       (unsigned long long) dropped_until_key);
  AMediaCodec_stop(dec);
  AMediaCodec_delete(dec);
  shm.shutdown(OBF("client gone").c_str());
  return 0;
}

} // namespace

extern "C" int cr_feed_run_tcp_h264(int port);

int cr_feed_run_tcp_h264(int port)
{
  int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (srv < 0)
  {
    LOGE(OBF("h264: socket: %s").c_str(), strerror(errno));
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
    LOGE(OBF("h264: bind :%d: %s").c_str(), port, strerror(errno));
    close(srv);
    return -1;
  }
  if (listen(srv, 1) != 0)
  {
    LOGE(OBF("h264: listen: %s").c_str(), strerror(errno));
    close(srv);
    return -1;
  }
  LOGI(OBF("tcp_h264 feed listening on 127.0.0.1:%d").c_str(), port);
  // Host wait_feed_ready may also grep the redirected stdout/stderr file.
  fprintf(stderr, "listening on 127.0.0.1:%d\n", port);
  fflush(stderr);

  while (!g_stop.load(std::memory_order_relaxed))
  {
    int client = cr::feed::accept_interruptible(srv, g_stop);
    if (client < 0)
    {
      if (g_stop.load(std::memory_order_relaxed))
        break;
      if (errno == EINTR)
        continue;
      LOGW(OBF("h264: accept: %s").c_str(), strerror(errno));
      break;
    }
    LOGI(OBF("h264: client connected").c_str());
    serve_client(client);
    close(client);
  }

  close(srv);
  return 0;
}
