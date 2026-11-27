/*
 * Назначение файла:
 *   Общий H.264 -> NV21 -> /data/cr/feed decode pipeline для phone-side feed
 *   variants. Транспортный слой отдаёт Annex-B NAL units, а этот wrapper
 *   выбирает Android decoder, конвертирует output format и публикует NV21 в
 *   shared memory.
 *
 * Hook point и ABI assumptions:
 *   /data/cr/feed uses cr_feed_header followed by FHD and UHD NV21 slots.
 *   This header-only helper is shared by the feed process's input paths.
 *   Decoder выбирается через AMediaCodec_createDecoderByType("video/avc")
 *   как primary path, vendor names остаются только fallback.
 *
 * Ограничения Pixel/Android:
 *   Codec availability and maximum decode size depend on the device.
 *   Source frames within the UHD pixel budget are published at full size.
 *
 * Подтверждено:
 *   Нормальный путь использует Android media codec selection по MIME type.
 *   Qualcomm names diagnostic/fallback-only для старых vendor builds.
 *
 * Diagnostic-only:
 *   codec_name() нужен только для логов и не является контрактом выбора codec.
 */

#pragma once

// Shared H.264 → NV21 → /data/cr/feed pipeline.
// Both the TCP-frame transport (tcp_h264_feed.cpp) and the legacy opt-in
// RTMP playback client (rtmp_h264_feed.cpp) feed the same decoder/SHM
// path: identical AMediaCodec setup, identical NV12→NV21 swap, identical
// triple-buffered shm publishing.
// This header exposes a tiny C++ API that wraps that pipeline so the
// transport-specific layer only has to:
//   1. Construct an H264DecodePipe with width/height (from the wire header
//      or a probed SPS/PPS).
//   2. Push() each Annex-B NAL unit (with PTS in microseconds, plus
//      a key-frame flag).
//   3. Call Stop() on shutdown.
// Both transports already shared 90% of their per-packet logic; pulling it
// here avoids divergence (e.g. someone fixing a colour-space bug in only
// one path).

#include "../include/cr_feed.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <android/log.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "shm.h"
#include "nv21_blit.h"
#include "media_codec_runtime.h"
#include "media/AdaptiveGeometry.h"
#include "media/FrameLayout.h"
#include "util/Obf.h"

#define CR_FEED_LOGI(...) __android_log_print(ANDROID_LOG_INFO, OBF("cr_feed").c_str(), __VA_ARGS__)
#define CR_FEED_LOGW(...) __android_log_print(ANDROID_LOG_WARN, OBF("cr_feed").c_str(), __VA_ARGS__)
#define CR_FEED_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, OBF("cr_feed").c_str(), __VA_ARGS__)

namespace cr_feed
{

inline bool fit_source_geometry(int source_width, int source_height,
                                int& output_width, int& output_height)
{
  return cr::media::fit_even_frame(
      source_width, source_height, static_cast<int>(CR_FEED_MAX_WIDTH),
      static_cast<int>(CR_FEED_MAX_HEIGHT), CR_FEED_MAX_PIXELS, output_width,
      output_height) &&
      output_width >= 16 && output_height >= 16;
}

// Writer used by the OBS decoder and local media playback.
struct ShmOut
{
  int fd = -1;
  void* map = nullptr;
  size_t mapped = 0;
  int w = 0;
  int h = 0;
  size_t slot_bytes = 0;
  uint32_t cur_wi = 0;

  ~ShmOut() { shutdown(); }
  ShmOut() = default;
  ShmOut(const ShmOut&) = delete;
  ShmOut& operator=(const ShmOut&) = delete;

  bool init(int ww, int hh)
  {
    mkdir(OBF("/data/cr").c_str(), 0711);
    if (ww < 16 || ww > (int) CR_FEED_MAX_WIDTH || hh < 16 ||
        hh > (int) CR_FEED_MAX_HEIGHT || (ww & 1) != 0 || (hh & 1) != 0)
      return false;
    slot_bytes = (size_t) ww * hh * 3 / 2;
    if (slot_bytes > CR_FEED_MAX_NV21_BYTES)
    {
      CR_FEED_LOGE(OBF("h264 pipe shm init rejected %dx%d: %zu > %u (%ux%u cap)").c_str(), ww, hh, slot_bytes, CR_FEED_MAX_NV21_BYTES, CR_FEED_MAX_WIDTH, CR_FEED_MAX_HEIGHT);
      return false;
    }
    const size_t reserve = CR_FEED_MAP_RESERVE;

    fd = open(OBF("/data/cr/feed").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
      return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
      CR_FEED_LOGE(OBF("pipe shm writer lease is already owned: %s").c_str(), strerror(errno));
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
    __atomic_store_n(&hd->format, 0u, __ATOMIC_RELAXED);
    atomic_store(&hd->write_index, prev_wi % CR_FEED_NUM_SLOTS);
    atomic_store(&hd->generation, cr_feed_next_generation(prev_gen));
    atomic_store(&hd->frame_counter, 0ull);
    atomic_store(&hd->channel_state, CR_CHANNEL_STATE_NONE);
    cur_wi = prev_wi % CR_FEED_NUM_SLOTS;

    __atomic_store_n(&hd->magic, CR_FEED_MAGIC, __ATOMIC_RELEASE);
    return true;
  }

  // The writer fills an owned slot synchronously. Readers only see the slot
  // after the complete image is committed.
  template<typename Write>
  bool write_nv21(uint32_t color_flags, const Write& write)
  {
    if (!map)
      return false;
    auto* hd = (cr_feed_header*) map;
    uint32_t next = 0;
    if (!cr_feed_claim_write(hd, cur_wi, &next))
      return false;
    write(static_cast<uint8_t*>(map) + cr_feed_slot_offset(slot_bytes, next));
    if (__atomic_load_n(&hd->format, __ATOMIC_RELAXED) != color_flags)
    {
      atomic_store_explicit(&hd->channel_state, CR_CHANNEL_STATE_NONE, memory_order_release);
      __atomic_store_n(&hd->format, color_flags, __ATOMIC_RELEASE);
      atomic_store(&hd->generation, cr_feed_next_generation(atomic_load(&hd->generation)));
    }
    cr_feed_publish(hd, next);
    atomic_store_explicit(&hd->channel_state, CR_CHANNEL_STATE_READY,
                          memory_order_release);
    cur_wi = next;
    return true;
  }

  bool publish_nv21(const uint8_t* src, uint32_t color_flags = 0)
  {
    return write_nv21(color_flags, [&](uint8_t* dest) {
      memcpy(dest, src, slot_bytes);
    });
  }

  void revoke_frame()
  {
    if (map && mapped >= sizeof(cr_feed_header))
    {
      auto* hd = static_cast<cr_feed_header*>(map);
      atomic_store_explicit(&hd->channel_state, CR_CHANNEL_STATE_NONE,
                            memory_order_release);
    }
  }

  void shutdown()
  {
    if (map)
    {
      revoke_frame();
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

// Colour-format converters (NV12 → NV21 / I420 → NV21). Same code that was
// inline in tcp_h264_feed.cpp.
inline void copy_decoder_luma(const uint8_t* src, int width, int height,
                              int stride, uint8_t* dst)
{
  if (stride == width)
  {
    memcpy(dst, src, static_cast<size_t>(width) * height);
    return;
  }
  for (int y = 0; y < height; ++y)
    memcpy(dst + static_cast<size_t>(y) * width,
           src + static_cast<size_t>(y) * stride, static_cast<size_t>(width));
}

inline void nv12_to_nv21(const uint8_t* src_y, const uint8_t* src_uv,
                         int ww, int hh, int src_y_stride,
                         int src_uv_stride, uint8_t* dst)
{
  copy_decoder_luma(src_y, ww, hh, src_y_stride, dst);
  uint8_t* dst_vu = dst + (size_t) ww * hh;
  const int uv_rows = hh / 2;
  for (int y = 0; y < uv_rows; ++y)
  {
    const uint8_t* s = src_uv + (size_t) y * src_uv_stride;
    uint8_t* d = dst_vu + (size_t) y * ww;
    int x = 0;
#if defined(__aarch64__) && defined(__ARM_NEON)
    for (; x <= ww - 16; x += 16)
      vst1q_u8(d + x, vrev16q_u8(vld1q_u8(s + x)));
#endif
    for (; x < ww; x += 2)
    {
      d[x + 0] = s[x + 1];
      d[x + 1] = s[x + 0];
    }
  }
}

inline void i420_to_nv21(const uint8_t* src_y, const uint8_t* src_u,
                         const uint8_t* src_v, int ww, int hh,
                         int src_y_stride, int src_u_stride,
                         int src_v_stride, uint8_t* dst)
{
  copy_decoder_luma(src_y, ww, hh, src_y_stride, dst);
  uint8_t* dst_vu = dst + (size_t) ww * hh;
  for (int y = 0; y < hh / 2; ++y)
  {
    const uint8_t* u = src_u + (size_t) y * src_u_stride;
    const uint8_t* v = src_v + (size_t) y * src_v_stride;
    uint8_t* out = dst_vu + (size_t) y * ww;
    int x = 0;
#if defined(__aarch64__) && defined(__ARM_NEON)
    for (; x <= ww / 2 - 16; x += 16)
    {
      const uint8x16x2_t vu{vld1q_u8(v + x), vld1q_u8(u + x)};
      vst2q_u8(out + x * 2, vu);
    }
#endif
    for (; x < ww / 2; ++x)
    {
      out[x * 2 + 0] = v[x];
      out[x * 2 + 1] = u[x];
    }
  }
}

// Pipeline: AMediaCodec H.264 decoder + ShmOut.
// This is the SAME setup tcp_h264_feed::serve_client built inline. After
// construction, callers push() Annex-B NAL units; the pipe internally
// drains the decoder output, converts to NV21, and publishes into shm.
class H264DecodePipe
{
public:
  H264DecodePipe() = default;
  ~H264DecodePipe() { stop(); }
  H264DecodePipe(const H264DecodePipe&) = delete;
  H264DecodePipe& operator=(const H264DecodePipe&) = delete;

  bool start(int ww, int hh, SdrColorAspects source_color = {})
  {
    (void) ensure_media_codec_binder_pool();
    w = ww;
    h = hh;
    coded_color = source_color;
    color_flags = coded_color.flags(w >= 1280 || h >= 720);
    if (!fit_source_geometry(w, h, output_w, output_h))
    {
      CR_FEED_LOGE(OBF("h264 pipe: rejected source geometry %dx%d").c_str(),
                   w, h);
      return false;
    }
    dec = AMediaCodec_createDecoderByType(OBF("video/avc").c_str());
    if (dec)
      chosen_name = OBF("video/avc/byType").c_str();
    auto try_named_decoder = [&](const char* name) -> bool
    {
      if (dec)
        return true;
      dec = AMediaCodec_createCodecByName(name);
      if (!dec)
        return false;
      chosen_name = name;
      return true;
    };
    try_named_decoder(OBF("c2.qti.avc.decoder").c_str());
    try_named_decoder(OBF("OMX.qcom.video.decoder.avc").c_str());
    if (!dec)
      return false;

    // Sprint A: pre-allocate the input buffer so the first big
    // keyframe doesn't trigger a mid-stream realloc (visible 100ms
    // stall in the preview). This single key is universally safe:
    // it's been in MediaFormat since API 16, and a vendor that
    // ignores it just wastes a small amount of allocation.
    // Three other knobs (low-latency, operating-rate, priority) were
    // tried earlier in this sprint but on Snapdragon 665 / OMX.qcom
    // (Android 12) they cause AMediaCodec_start to lie — it returns
    // AMEDIA_OK but every subsequent dequeueOutputBuffer fails with
    // sf error -38 (INVALID_OPERATION) at ~100/sec, and no frames
    // ever come out. Until we have a runtime probe to detect that
    // and roll back, those knobs stay off here. They CAN be re-
    // enabled per-device by editing this block — Pixel 6+ and late
    // Snapdragon 8-gen Codec2 do honour them.
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, OBF("video/avc").c_str());
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, h);
    const int64_t suggested_input =
        std::clamp<int64_t>(static_cast<int64_t>(w) * h / 2,
                            1 << 20, 8 << 20);
    AMediaFormat_setInt32(fmt, OBF("max-input-size").c_str(),
                          static_cast<int32_t>(suggested_input));
    media_status_t cs = AMediaCodec_configure(dec, fmt, nullptr, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (cs != AMEDIA_OK)
    {
      CR_FEED_LOGE(OBF("h264 pipe: configure failed status=%d").c_str(), (int) cs);
      AMediaCodec_delete(dec);
      dec = nullptr;
      return false;
    }
    media_status_t ss = AMediaCodec_start(dec);
    if (ss != AMEDIA_OK)
    {
      CR_FEED_LOGE(OBF("h264 pipe: start failed status=%d").c_str(), (int) ss);
      AMediaCodec_delete(dec);
      dec = nullptr;
      return false;
    }
    CR_FEED_LOGI(OBF("h264 pipe: started source=%dx%d shared=%dx%d (%s)").c_str(),
                 w, h, output_w, output_h, chosen_name.c_str());

    if (!shm.init(output_w, output_h))
    {
      AMediaCodec_stop(dec);
      AMediaCodec_delete(dec);
      dec = nullptr;
      return false;
    }
    if (output_w != w || output_h != h)
      nv21.resize(static_cast<size_t>(w) * h * 3 / 2);
    return true;
  }

  void stop()
  {
    if (dec)
    {
      AMediaCodec_stop(dec);
      AMediaCodec_delete(dec);
      dec = nullptr;
    }
    shm.shutdown();
    nv21.clear();
    output_w = 0;
    output_h = 0;
  }

  const char* codec_name() const noexcept
  {
    return chosen_name.c_str();
  }

  // Push one Annex-B access unit with a microsecond PTS. The caller
  // prepends SPS/PPS to key frames so the codec can resynchronize.
  bool push(const uint8_t* nal, size_t len, int64_t pts_us)
  {
    if (!dec || !nal || !len)
      return false;

    using Clock = std::chrono::steady_clock;
    using Microseconds = std::chrono::microseconds;
    const auto input_deadline = Clock::now() + std::chrono::milliseconds(45);
    ssize_t in_idx = AMediaCodec_dequeueInputBuffer(dec, 0);
    while (in_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
    {
      // Releasing one decoded output can unlock an input slot. Recheck
      // immediately, including when the copy consumed the wait budget.
      drain(1);
      in_idx = AMediaCodec_dequeueInputBuffer(dec, 0);
      if (in_idx != AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        break;
      const auto remaining = std::chrono::duration_cast<Microseconds>(
          input_deadline - Clock::now());
      if (remaining <= Microseconds::zero())
        break;
      in_idx = AMediaCodec_dequeueInputBuffer(
          dec, std::min(remaining, Microseconds(3000)).count());
    }
    if (in_idx >= 0)
    {
      size_t cap = 0;
      uint8_t* in_buf = AMediaCodec_getInputBuffer(dec, (size_t) in_idx, &cap);
      if (in_buf && cap >= len)
      {
        memcpy(in_buf, nal, len);
        const media_status_t queued = AMediaCodec_queueInputBuffer(
            dec, (size_t) in_idx, 0, len,
            pts_us < 0 ? 0u : (uint64_t) pts_us, 0);
        if (queued != AMEDIA_OK)
        {
          CR_FEED_LOGW(OBF("h264: queueInput failed status=%d").c_str(),
                       (int) queued);
          AMediaCodec_flush(dec);
          return false;
        }
        ++in_count;
      }
      else
      {
        CR_FEED_LOGW(OBF("h264: input buf too small: cap=%zu need=%zu").c_str(), cap, len);
        // A dequeued input slot must always be returned to MediaCodec.
        AMediaCodec_queueInputBuffer(dec, (size_t) in_idx, 0, 0,
                                     pts_us < 0 ? 0u : (uint64_t) pts_us, 0);
        AMediaCodec_flush(dec);
        return false;
      }
    }
    else
    {
      const uint64_t missed = ++input_deadline_misses;
      if (missed <= 3 || missed % 120 == 0)
        CR_FEED_LOGW("h264: input unavailable after bounded retry (status=%zd, total=%llu)",
                     in_idx, static_cast<unsigned long long>(missed));
      return false;
    }
    drain();
    return true;
  }

  // Drain decoder output without queueing new input. Useful for callers
  // that want to flush after a burst.
  void drain(size_t buffer_budget = SIZE_MAX)
  {
    if (!dec)
      return;
    while (buffer_budget != 0)
    {
      AMediaCodecBufferInfo info;
      ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(dec, &info, 0);
      if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        break;
      if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
      {
        AMediaFormat* of = AMediaCodec_getOutputFormat(dec);
        if (!of)
          break;
        AMediaFormat_getInt32(of, OBF("color-format").c_str(), &color_format);
        AMediaFormat_getInt32(of, OBF("stride").c_str(), &y_stride);
        AMediaFormat_getInt32(of, OBF("slice-height").c_str(), &slice_height);
        uv_stride = y_stride;
        SdrColorAspects color;
        color.update(of);
        // An unknown vendor standard is not evidence for BT.601.
        color_flags = color.with_fallback(coded_color).flags(w >= 1280 || h >= 720);
        CR_FEED_LOGI("h264 color standard=%d range=%d coded_standard=%d coded_range=%d shared_flags=%u",
                     color.standard, color.range, coded_color.standard, coded_color.range, color_flags);
        CR_FEED_LOGI(OBF("h264 out fmt: color=0x%x stride=%d slice=%d").c_str(), color_format, y_stride, slice_height);
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

        constexpr int32_t kFmtYUV420Planar = 19;
        constexpr int32_t kFmtYUV420SemiPlanar = 21;
        constexpr int32_t kFmtYUV420PackedSemiPlanar = 0x7fa30c04;
        cr::media::PlaneSpan window;
        const bool window_ok = out_buf && cr::media::valid_codec_window(
                                                cap, info.offset, info.size, window);
        const uint8_t* base = window_ok ? out_buf + window.offset : nullptr;
        const size_t available = window_ok ? window.bytes : 0;
        bool published = false;

        if (!base || sy < w || suv < w || slice < h || sy <= 0 || suv <= 0)
        {
          const uint64_t n = ++output_layout_warns;
          if (n <= 3 || (n % 120) == 0)
            CR_FEED_LOGW(OBF("h264 pipe output layout rejected color=0x%x off=%d size=%d cap=%zu stride=%d uv=%d slice=%d dims=%dx%d").c_str(),
                         color_format, (int) info.offset, (int) info.size, cap,
                         sy, suv, slice, w, h);
        }
        else if (color_format == kFmtYUV420SemiPlanar ||
                 color_format == kFmtYUV420PackedSemiPlanar)
        {
          cr::media::Yuv420Layout layout;
          if (cr::media::semiplanar_420(
                  available, (uint32_t) w, (uint32_t) h, (uint32_t) sy,
                  (uint32_t) slice, (uint32_t) suv, layout))
          {
            published = publish_decoded([&](uint8_t* dest) {
              nv12_to_nv21(base, base + layout.u_or_uv.offset, w, h, sy, suv, dest);
            });
          }
          else
          {
            const uint64_t n = ++output_layout_warns;
            if (n <= 3 || (n % 120) == 0)
              CR_FEED_LOGW(OBF("h264 pipe semiplanar layout rejected available=%zu stride=%d uv=%d slice=%d").c_str(),
                           available, sy, suv, slice);
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
            published = publish_decoded([&](uint8_t* dest) {
              i420_to_nv21(base, base + layout.u_or_uv.offset, base + layout.v.offset,
                           w, h, sy, chroma_stride, chroma_stride, dest);
            });
          }
          else
          {
            const uint64_t n = ++output_layout_warns;
            if (n <= 3 || (n % 120) == 0)
              CR_FEED_LOGW(OBF("h264 pipe planar layout rejected available=%zu stride=%d chroma=%d slice=%d").c_str(),
                           available, sy, chroma_stride, slice);
          }
        }
        else
        {
          const uint64_t n = ++output_layout_warns;
          if (n <= 3 || (n % 120) == 0)
            CR_FEED_LOGW(OBF("h264 pipe unsupported output format color=0x%x off=%d size=%d cap=%zu").c_str(),
                         color_format, (int) info.offset, (int) info.size, cap);
        }
        if (published)
        {
          ++published_count;
          if (published_count <= 3 || (published_count % 120) == 0)
            CR_FEED_LOGI("h264: published frame #%llu (in=%llu slot_busy=%llu input_unavailable=%llu)",
                         static_cast<unsigned long long>(published_count),
                         static_cast<unsigned long long>(in_count),
                         static_cast<unsigned long long>(slot_busy_count),
                         static_cast<unsigned long long>(input_deadline_misses));
        }
      }
      AMediaCodec_releaseOutputBuffer(dec, (size_t) out_idx, false);
      --buffer_budget;
    }
  }

  uint64_t input_count() const noexcept
  {
    return in_count;
  }
  uint64_t frames_published() const noexcept
  {
    return published_count;
  }
  void revoke_frame() { shm.revoke_frame(); }

private:
  template<typename Convert>
  bool publish_decoded(const Convert& convert)
  {
    const bool published = shm.write_nv21(color_flags, [&](uint8_t* dest) {
      if (nv21.empty())
      {
        convert(dest);
        return;
      }
      convert(nv21.data());
      uint8_t* dest_vu = dest + static_cast<size_t>(output_w) * output_h;
      cr_nv21_blit(nv21.data(), w, h, dest, dest_vu,
                   output_w, output_h, output_w, output_w, 0);
    });
    if (!published)
      ++slot_busy_count;
    return published;
  }

  AMediaCodec* dec = nullptr;
  std::string chosen_name;

  int w = 0, h = 0;
  int output_w = 0, output_h = 0;
  int32_t color_format = 21; // NV12 default
  int32_t y_stride = 0;
  int32_t slice_height = 0;
  int32_t uv_stride = 0;

  ShmOut shm;
  std::vector<uint8_t> nv21;
  uint64_t in_count = 0;
  uint64_t input_deadline_misses = 0;
  uint64_t slot_busy_count = 0;
  uint64_t published_count = 0;
  SdrColorAspects coded_color;
  uint32_t color_flags = 0;
  uint64_t output_layout_warns = 0;
};

} // namespace cr_feed
