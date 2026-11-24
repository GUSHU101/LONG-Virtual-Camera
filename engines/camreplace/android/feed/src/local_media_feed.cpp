/*
 * Phone-local media playback for LONG Virtual Camera.
 *
 * The file-decoder shape follows the MediaExtractor/MediaCodec approach used
 * by ReCam (MIT, aimardcr, 2026), but publishes into CamReplace's hardened
 * video/audio shared-memory ABIs. This is the actual fusion point: local
 * files are decoded entirely on Android while cameraserver/audioserver keep
 * the single global replacement path.
 */

#include "../include/cr_feed.h"
#include "audio_shm.h"
#include "h264_decode_pipe.h"
#include "media_codec_runtime.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <string>
#include <vector>

#include "util/Obf.h"
#include "media/AdaptiveGeometry.h"

#define TAG "cr_feed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern std::atomic<bool> g_stop;

namespace
{

constexpr int32_t kColorYuv420Planar = 19;
constexpr int32_t kColorYuv420PackedPlanar = 20;
constexpr int32_t kColorYuv420SemiPlanar = 21;
constexpr int32_t kColorYuv420PackedSemiPlanar = 39;
constexpr uint32_t kImageMagic = 0x31564e4cu; // "LNV1" little-endian
constexpr uint64_t kMaxMediaBytes = 4ull * 1024ull * 1024ull * 1024ull;

uint64_t monotonic_us()
{
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000ull +
         static_cast<uint64_t>(now.tv_nsec) / 1000ull;
}

void sleep_interruptible_us(uint64_t duration)
{
  while (duration > 0 && !g_stop.load(std::memory_order_relaxed))
  {
    const uint64_t part = std::min<uint64_t>(duration, 20000);
    usleep(static_cast<useconds_t>(part));
    duration -= part;
  }
}

bool open_regular_media(const char* path, int& fd, off_t& length)
{
  fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
  {
    LOGE(OBF("local media: open %s failed: %s").c_str(), path, strerror(errno));
    return false;
  }
  struct stat st{};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      static_cast<uint64_t>(st.st_size) > kMaxMediaBytes)
  {
    LOGE(OBF("local media: rejected non-regular, empty, or oversized source").c_str());
    ::close(fd);
    fd = -1;
    return false;
  }
  length = st.st_size;
  return true;
}

struct Plane
{
  const uint8_t* data = nullptr;
  int32_t row_stride = 0;
  int32_t pixel_stride = 1;
};

struct VideoFrame
{
  Plane y;
  Plane u;
  Plane v;
  int32_t width = 0;
  int32_t height = 0;
  int64_t pts_us = 0;
};

class FileVideoDecoder
{
public:
  ~FileVideoDecoder() { close(); }
  FileVideoDecoder() = default;
  FileVideoDecoder(const FileVideoDecoder&) = delete;
  FileVideoDecoder& operator=(const FileVideoDecoder&) = delete;

  bool open(const char* path)
  {
    close();
    (void) cr_feed::ensure_media_codec_binder_pool();
    off_t length = 0;
    if (!open_regular_media(path, fd_, length))
      return false;

    extractor_ = AMediaExtractor_new();
    if (!extractor_ ||
        AMediaExtractor_setDataSourceFd(extractor_, fd_, 0, length) != AMEDIA_OK)
    {
      LOGE(OBF("local video: container is not readable by MediaExtractor").c_str());
      close();
      return false;
    }

    AMediaFormat* selected = nullptr;
    int track = -1;
    const char* mime = nullptr;
    const size_t tracks = AMediaExtractor_getTrackCount(extractor_);
    for (size_t i = 0; i < tracks; ++i)
    {
      AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor_, i);
      const char* candidate = nullptr;
      AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &candidate);
      if (track < 0 && candidate && strncmp(candidate, OBF("video/").c_str(), 6) == 0)
      {
        selected = format;
        mime = candidate;
        track = static_cast<int>(i);
      }
      else
      {
        AMediaFormat_delete(format);
      }
    }
    if (track < 0 || !selected || !mime)
    {
      LOGE(OBF("local video: no decodable video track").c_str());
      close();
      return false;
    }

    AMediaFormat_getInt32(selected, AMEDIAFORMAT_KEY_WIDTH, &coded_width_);
    AMediaFormat_getInt32(selected, AMEDIAFORMAT_KEY_HEIGHT, &coded_height_);
    int32_t fps = 0;
    if (AMediaFormat_getInt32(selected, AMEDIAFORMAT_KEY_FRAME_RATE, &fps) &&
        fps >= 1 && fps <= 240)
      fallback_interval_us_ = 1000000 / fps;
    AMediaFormat_getInt32(selected, OBF("rotation-degrees").c_str(), &rotation_);
    rotation_ = ((rotation_ % 360) + 360) % 360;
    if (rotation_ != 0 && rotation_ != 90 && rotation_ != 180 && rotation_ != 270)
      rotation_ = 0;
    if (coded_width_ < 2 || coded_height_ < 2 || coded_width_ > 16384 ||
        coded_height_ > 16384)
    {
      LOGE(OBF("local video: implausible geometry %dx%d").c_str(),
           coded_width_, coded_height_);
      AMediaFormat_delete(selected);
      close();
      return false;
    }

    AMediaExtractor_selectTrack(extractor_, track);
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_ ||
        AMediaCodec_configure(codec_, selected, nullptr, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(codec_) != AMEDIA_OK)
    {
      LOGE(OBF("local video: decoder configure/start failed for %s").c_str(), mime);
      AMediaFormat_delete(selected);
      close();
      return false;
    }
    AMediaFormat_delete(selected);
    LOGI(OBF("local video: opened %dx%d rotation=%d").c_str(),
         coded_width_, coded_height_, rotation_);
    return true;
  }

  void close()
  {
    frame_.clear();
    frame_ready_ = false;
    format_ready_ = false;
    if (codec_)
    {
      AMediaCodec_stop(codec_);
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
    }
    if (extractor_)
    {
      AMediaExtractor_delete(extractor_);
      extractor_ = nullptr;
    }
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool next(VideoFrame& out)
  {
    frame_ready_ = false;
    for (int attempt = 0; attempt < 96 && !g_stop.load(std::memory_order_relaxed);
         ++attempt)
    {
      const ssize_t input = input_eos_ ? -1 :
          AMediaCodec_dequeueInputBuffer(codec_, 2000);
      if (input >= 0)
      {
        size_t capacity = 0;
        uint8_t* bytes =
            AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(input), &capacity);
        if (!bytes)
          return false;
        const ssize_t count = AMediaExtractor_readSampleData(extractor_, bytes,
                                                              capacity);
        const int64_t pts = AMediaExtractor_getSampleTime(extractor_);
        input_eos_ = count < 0;
        const media_status_t queued = AMediaCodec_queueInputBuffer(
            codec_, static_cast<size_t>(input), 0,
            input_eos_ ? 0 : static_cast<size_t>(count),
            pts < 0 ? 0 : static_cast<uint64_t>(pts),
            input_eos_ ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
        if (queued != AMEDIA_OK)
        {
          LOGE(OBF("local video: queue input failed (%d)").c_str(), queued);
          return false;
        }
        if (!input_eos_)
          AMediaExtractor_advance(extractor_);
      }

      AMediaCodecBufferInfo info{};
      const ssize_t output = AMediaCodec_dequeueOutputBuffer(codec_, &info, 4000);
      if (output == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
      {
        update_output_format();
        continue;
      }
      if (output < 0)
        continue;

      if (!format_ready_)
        update_output_format();
      size_t capacity = 0;
      uint8_t* bytes =
          AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(output), &capacity);
      bool copied = false;
      if (bytes && info.offset >= 0 && info.size > 0 &&
          static_cast<size_t>(info.offset) <= capacity &&
          static_cast<size_t>(info.size) <=
              capacity - static_cast<size_t>(info.offset))
      {
        frame_.assign(bytes + info.offset, bytes + info.offset + info.size);
        frame_pts_us_ = info.presentationTimeUs;
        frame_ready_ = true;
        copied = describe(out);
      }
      AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(output), false);
      if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
        rewind_pending_ = true;
      if (copied)
      {
        out.pts_us = frame_pts_us_;
        return true;
      }
      if (rewind_pending_)
        break;
    }
    return false;
  }

  int rotation() const { return rotation_; }
  bool take_looped()
  {
    const bool value = looped_;
    looped_ = false;
    return value;
  }

  bool rewind_if_drained()
  {
    if (!rewind_pending_)
      return true;
    if (AMediaExtractor_seekTo(extractor_, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC) !=
            AMEDIA_OK ||
        AMediaCodec_flush(codec_) != AMEDIA_OK)
      return false;
    input_eos_ = false;
    rewind_pending_ = false;
    looped_ = true;
    return true;
  }

private:
  void update_output_format()
  {
    AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
    if (!format)
      return;
    int32_t width = coded_width_;
    int32_t height = coded_height_;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &stride_);
    // The key exists on old releases even though the named NDK constant was
    // only exported in API 28; use its stable wire name for minSdk 27 builds.
    AMediaFormat_getInt32(format, OBF("slice-height").c_str(), &slice_height_);
    const bool have_color =
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color_);
    if (stride_ <= 0)
      stride_ = width;
    if (slice_height_ <= 0)
      slice_height_ = height;

    crop_left_ = 0;
    crop_top_ = 0;
    crop_right_ = width - 1;
    crop_bottom_ = height - 1;
    int32_t left = 0, top = 0, right = width - 1, bottom = height - 1;
    if (AMediaFormat_getInt32(format, OBF("crop-left").c_str(), &left) &&
        AMediaFormat_getInt32(format, OBF("crop-top").c_str(), &top) &&
        AMediaFormat_getInt32(format, OBF("crop-right").c_str(), &right) &&
        AMediaFormat_getInt32(format, OBF("crop-bottom").c_str(), &bottom))
    {
      crop_left_ = left;
      crop_top_ = top;
      crop_right_ = right;
      crop_bottom_ = bottom;
    }
    const int32_t visible_width = crop_right_ - crop_left_ + 1;
    const int32_t visible_height = crop_bottom_ - crop_top_ + 1;
    format_ready_ =
        have_color && visible_width >= 2 && visible_height >= 2 &&
        (visible_width & 1) == 0 && (visible_height & 1) == 0 &&
        stride_ >= width && slice_height_ >= height && (stride_ & 1) == 0 &&
        (slice_height_ & 1) == 0 && crop_left_ >= 0 && crop_top_ >= 0 &&
        crop_right_ >= crop_left_ && crop_bottom_ >= crop_top_ &&
        crop_right_ < stride_ && crop_bottom_ < slice_height_ &&
        (crop_left_ & 1) == 0 && (crop_top_ & 1) == 0;
    LOGI(OBF("local video output: color=0x%x stride=%d slice=%d crop=%d,%d-%d,%d").c_str(),
         color_, stride_, slice_height_, crop_left_, crop_top_, crop_right_,
         crop_bottom_);
    AMediaFormat_delete(format);
  }

  bool describe(VideoFrame& out)
  {
    if (!frame_ready_ || !format_ready_)
      return false;
    const int32_t width = crop_right_ - crop_left_ + 1;
    const int32_t height = crop_bottom_ - crop_top_ + 1;
    const size_t y_bytes = static_cast<size_t>(stride_) * slice_height_;
    size_t required = 0;
    size_t u_offset = 0;
    size_t v_offset = 0;
    int32_t chroma_stride = 0;
    int32_t pixel_stride = 0;
    if (color_ == kColorYuv420Planar || color_ == kColorYuv420PackedPlanar)
    {
      chroma_stride = stride_ / 2;
      pixel_stride = 1;
      const size_t chroma_bytes =
          static_cast<size_t>(chroma_stride) * (slice_height_ / 2);
      required = y_bytes + chroma_bytes * 2;
      const size_t crop = static_cast<size_t>(crop_top_ / 2) * chroma_stride +
                          static_cast<size_t>(crop_left_ / 2);
      u_offset = y_bytes + crop;
      v_offset = y_bytes + chroma_bytes + crop;
    }
    else if (color_ == kColorYuv420SemiPlanar ||
             color_ == kColorYuv420PackedSemiPlanar)
    {
      chroma_stride = stride_;
      pixel_stride = 2;
      required = y_bytes + static_cast<size_t>(stride_) * (slice_height_ / 2);
      const size_t crop = static_cast<size_t>(crop_top_ / 2) * stride_ +
                          static_cast<size_t>(crop_left_);
      u_offset = y_bytes + crop;
      v_offset = u_offset + 1;
    }
    else
    {
      LOGE(OBF("local video: unsupported byte-buffer color format 0x%x").c_str(),
           color_);
      return false;
    }
    if (frame_.size() < required)
    {
      LOGE(OBF("local video: output buffer %zu is smaller than layout %zu").c_str(),
           frame_.size(), required);
      return false;
    }
    const uint8_t* base = frame_.data();
    out.width = width;
    out.height = height;
    out.y = {base + static_cast<size_t>(crop_top_) * stride_ + crop_left_,
             stride_, 1};
    out.u = {base + u_offset, chroma_stride, pixel_stride};
    out.v = {base + v_offset, chroma_stride, pixel_stride};
    return true;
  }

  int fd_ = -1;
  AMediaExtractor* extractor_ = nullptr;
  AMediaCodec* codec_ = nullptr;
  std::vector<uint8_t> frame_;
  int32_t coded_width_ = 0;
  int32_t coded_height_ = 0;
  int32_t stride_ = 0;
  int32_t slice_height_ = 0;
  int32_t color_ = 0;
  int32_t crop_left_ = 0;
  int32_t crop_top_ = 0;
  int32_t crop_right_ = 0;
  int32_t crop_bottom_ = 0;
  int32_t rotation_ = 0;
  int64_t fallback_interval_us_ = 33333;
  int64_t frame_pts_us_ = 0;
  bool frame_ready_ = false;
  bool format_ready_ = false;
  bool looped_ = false;
  bool input_eos_ = false;
  bool rewind_pending_ = false;
};

bool valid_plane(const Plane& plane, int32_t width)
{
  if (!plane.data || plane.row_stride <= 0 || plane.pixel_stride < 1 ||
      plane.pixel_stride > 4 || width < 1)
    return false;
  const int64_t needed =
      static_cast<int64_t>(width - 1) * plane.pixel_stride + 1;
  return needed <= plane.row_stride;
}

uint8_t bilinear(const Plane& plane, int32_t width, int32_t height,
                 uint32_t x16, uint32_t y16)
{
  const int32_t x0 = std::min<int32_t>(x16 >> 16, width - 1);
  const int32_t y0 = std::min<int32_t>(y16 >> 16, height - 1);
  const int32_t x1 = std::min<int32_t>(x0 + 1, width - 1);
  const int32_t y1 = std::min<int32_t>(y0 + 1, height - 1);
  const int32_t fx = x16 & 0xffffu;
  const int32_t fy = y16 & 0xffffu;
  const uint8_t* row0 = plane.data + static_cast<size_t>(y0) * plane.row_stride;
  const uint8_t* row1 = plane.data + static_cast<size_t>(y1) * plane.row_stride;
  const int32_t a = row0[x0 * plane.pixel_stride];
  const int32_t b = row0[x1 * plane.pixel_stride];
  const int32_t c = row1[x0 * plane.pixel_stride];
  const int32_t d = row1[x1 * plane.pixel_stride];
  const int32_t top = a + (((b - a) * fx) >> 16);
  const int32_t bottom = c + (((d - c) * fx) >> 16);
  return static_cast<uint8_t>(top + (((bottom - top) * fy) >> 16));
}

bool choose_output_geometry(int32_t source_width, int32_t source_height,
                            int rotation, int& output_width, int& output_height)
{
  return cr::media::fit_oriented_even_frame(
      source_width, source_height, rotation,
      static_cast<int32_t>(CR_FEED_MAX_WIDTH),
      static_cast<int32_t>(CR_FEED_MAX_HEIGHT), CR_FEED_MAX_PIXELS,
      output_width, output_height) &&
      output_width >= 16 && output_height >= 16;
}

bool frame_to_nv21(const VideoFrame& source, int rotation, int output_width,
                   int output_height, std::vector<uint8_t>& destination)
{
  if (source.width < 2 || source.height < 2 || (source.width & 1) ||
      (source.height & 1) || !valid_plane(source.y, source.width) ||
      !valid_plane(source.u, source.width / 2) ||
      !valid_plane(source.v, source.width / 2))
    return false;
  destination.assign(static_cast<size_t>(output_width) * output_height * 3 / 2,
                     0);

  auto sample_oriented = [&](const Plane& plane, int32_t source_w,
                             int32_t source_h, int32_t dest_x, int32_t dest_y,
                             int32_t dest_w, int32_t dest_h) -> uint8_t
  {
    const int32_t oriented_w =
        (rotation == 90 || rotation == 270) ? source_h : source_w;
    const int32_t oriented_h =
        (rotation == 90 || rotation == 270) ? source_w : source_h;

    int32_t crop_w = oriented_w;
    int32_t crop_h = oriented_h;
    if (static_cast<int64_t>(oriented_w) * dest_h >
        static_cast<int64_t>(dest_w) * oriented_h)
      crop_w = static_cast<int32_t>(
          static_cast<int64_t>(oriented_h) * dest_w / dest_h);
    else
      crop_h = static_cast<int32_t>(
          static_cast<int64_t>(oriented_w) * dest_h / dest_w);
    crop_w = std::max(1, crop_w);
    crop_h = std::max(1, crop_h);
    const int32_t crop_x = (oriented_w - crop_w) / 2;
    const int32_t crop_y = (oriented_h - crop_h) / 2;
    const uint32_t ox16 =
        static_cast<uint32_t>((static_cast<int64_t>(crop_x) << 16) +
                              (static_cast<int64_t>(dest_x) * (crop_w - 1)
                               << 16) /
                                  std::max(1, dest_w - 1));
    const uint32_t oy16 =
        static_cast<uint32_t>((static_cast<int64_t>(crop_y) << 16) +
                              (static_cast<int64_t>(dest_y) * (crop_h - 1)
                               << 16) /
                                  std::max(1, dest_h - 1));

    uint32_t sx16 = ox16;
    uint32_t sy16 = oy16;
    if (rotation == 90)
    {
      sx16 = oy16;
      sy16 = static_cast<uint32_t>((static_cast<int64_t>(source_h - 1) << 16) -
                                   ox16);
    }
    else if (rotation == 180)
    {
      sx16 = static_cast<uint32_t>((static_cast<int64_t>(source_w - 1) << 16) -
                                   ox16);
      sy16 = static_cast<uint32_t>((static_cast<int64_t>(source_h - 1) << 16) -
                                   oy16);
    }
    else if (rotation == 270)
    {
      sx16 = static_cast<uint32_t>((static_cast<int64_t>(source_w - 1) << 16) -
                                   oy16);
      sy16 = ox16;
    }
    return bilinear(plane, source_w, source_h, sx16, sy16);
  };

  uint8_t* y_plane = destination.data();
  uint8_t* vu_plane =
      y_plane + static_cast<size_t>(output_width) * output_height;
  for (int y = 0; y < output_height; ++y)
    for (int x = 0; x < output_width; ++x)
      y_plane[static_cast<size_t>(y) * output_width + x] =
          sample_oriented(source.y, source.width, source.height, x, y,
                          output_width, output_height);
  for (int y = 0; y < output_height / 2; ++y)
  {
    for (int x = 0; x < output_width / 2; ++x)
    {
      const size_t offset =
          static_cast<size_t>(y) * output_width + static_cast<size_t>(x) * 2;
      vu_plane[offset] =
          sample_oriented(source.v, source.width / 2, source.height / 2, x, y,
                          output_width / 2, output_height / 2);
      vu_plane[offset + 1] =
          sample_oriented(source.u, source.width / 2, source.height / 2, x, y,
                          output_width / 2, output_height / 2);
    }
  }
  return true;
}

class AudioShmWriter
{
public:
  ~AudioShmWriter() { close(); }
  AudioShmWriter() = default;
  AudioShmWriter(const AudioShmWriter&) = delete;
  AudioShmWriter& operator=(const AudioShmWriter&) = delete;

  bool open()
  {
    mkdir(OBF("/data/cr").c_str(), 0711);
    const size_t wanted = sizeof(cr_audio_header) + CR_AUDIO_RING_BYTES;
    fd_ = ::open(CR_AUDIO_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0 || flock(fd_, LOCK_EX | LOCK_NB) != 0)
    {
      LOGE(OBF("local audio: shared-memory writer already active").c_str());
      close();
      return false;
    }
    struct stat st{};
    if (fstat(fd_, &st) != 0 ||
        (static_cast<size_t>(st.st_size) < wanted &&
         ftruncate(fd_, static_cast<off_t>(wanted)) != 0))
    {
      close();
      return false;
    }
    void* mapped =
        mmap(nullptr, wanted, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED)
    {
      close();
      return false;
    }
    header_ = static_cast<cr_audio_header*>(mapped);
    ring_ = reinterpret_cast<uint8_t*>(mapped) + sizeof(cr_audio_header);
    mapped_ = wanted;
    revoke();
    return true;
  }

  bool configure(uint32_t sample_rate, uint32_t channels)
  {
    if (!header_ || sample_rate < 8000 || sample_rate > 192000 ||
        channels < 1 || channels > 2)
      return false;
    __atomic_store_n(&header_->magic, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&header_->version, CR_AUDIO_VERSION, __ATOMIC_RELAXED);
    __atomic_store_n(&header_->sample_rate, sample_rate, __ATOMIC_RELAXED);
    __atomic_store_n(&header_->channels, channels, __ATOMIC_RELAXED);
    __atomic_store_n(&header_->bytes_per_sample, 2u, __ATOMIC_RELAXED);
    __atomic_store_n(&header_->ring_bytes, CR_AUDIO_RING_BYTES,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&header_->reserved[0], 0u, __ATOMIC_RELAXED);
    atomic_store(&header_->write_pos, 0ull);
    atomic_store(&header_->generation, cr_audio_next_generation(header_));
    atomic_store(&header_->channel_state, CR_CHANNEL_STATE_NONE);
    __atomic_store_n(&header_->magic, CR_AUDIO_MAGIC, __ATOMIC_RELEASE);
    sample_rate_ = sample_rate;
    channels_ = channels;
    return true;
  }

  bool write(const uint8_t* bytes, size_t count)
  {
    if (!header_ || !ring_ || !bytes || count == 0 ||
        count > CR_AUDIO_RING_BYTES)
      return false;
    const uint64_t position = atomic_load(&header_->write_pos);
    const uint32_t offset =
        static_cast<uint32_t>(position % CR_AUDIO_RING_BYTES);
    const size_t first =
        std::min<size_t>(count, CR_AUDIO_RING_BYTES - offset);
    const uint32_t token = cr_audio_begin_write(header_);
    memcpy(ring_ + offset, bytes, first);
    if (first < count)
      memcpy(ring_, bytes + first, count - first);
    atomic_store_explicit(&header_->write_pos, position + count,
                          memory_order_release);
    atomic_store_explicit(&header_->channel_state, CR_CHANNEL_STATE_READY,
                          memory_order_release);
    cr_audio_end_write(header_, token);
    return true;
  }

  uint32_t sample_rate() const { return sample_rate_; }
  uint32_t channels() const { return channels_; }

private:
  void revoke()
  {
    if (!header_)
      return;
    atomic_store_explicit(&header_->channel_state, CR_CHANNEL_STATE_NONE,
                          memory_order_release);
    __atomic_store_n(&header_->magic, 0u, __ATOMIC_RELEASE);
  }

  void close()
  {
    revoke();
    if (header_)
    {
      munmap(header_, mapped_);
      header_ = nullptr;
      ring_ = nullptr;
      mapped_ = 0;
    }
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int fd_ = -1;
  cr_audio_header* header_ = nullptr;
  uint8_t* ring_ = nullptr;
  size_t mapped_ = 0;
  uint32_t sample_rate_ = 0;
  uint32_t channels_ = 0;
};

class FileAudioDecoder
{
public:
  ~FileAudioDecoder() { close(); }
  FileAudioDecoder() = default;
  FileAudioDecoder(const FileAudioDecoder&) = delete;
  FileAudioDecoder& operator=(const FileAudioDecoder&) = delete;

  bool open(const char* path)
  {
    close();
    (void) cr_feed::ensure_media_codec_binder_pool();
    off_t length = 0;
    if (!open_regular_media(path, fd_, length))
      return false;
    extractor_ = AMediaExtractor_new();
    if (!extractor_ ||
        AMediaExtractor_setDataSourceFd(extractor_, fd_, 0, length) != AMEDIA_OK)
    {
      close();
      return false;
    }

    AMediaFormat* selected = nullptr;
    const char* mime = nullptr;
    int track = -1;
    const size_t tracks = AMediaExtractor_getTrackCount(extractor_);
    for (size_t i = 0; i < tracks; ++i)
    {
      AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor_, i);
      const char* candidate = nullptr;
      AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &candidate);
      if (track < 0 && candidate && strncmp(candidate, OBF("audio/").c_str(), 6) == 0)
      {
        selected = format;
        mime = candidate;
        track = static_cast<int>(i);
      }
      else
      {
        AMediaFormat_delete(format);
      }
    }
    if (track < 0 || !selected || !mime)
    {
      LOGE(OBF("local audio: no decodable audio track").c_str());
      close();
      return false;
    }
    // Android's PCM_ENCODING_16BIT constant is 2. Decoders that ignore this
    // request are rejected when their output format is reported.
    AMediaFormat_setInt32(selected, OBF("pcm-encoding").c_str(), 2);
    AMediaExtractor_selectTrack(extractor_, track);
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_ ||
        AMediaCodec_configure(codec_, selected, nullptr, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(codec_) != AMEDIA_OK)
    {
      LOGE(OBF("local audio: decoder configure/start failed for %s").c_str(), mime);
      AMediaFormat_delete(selected);
      close();
      return false;
    }
    AMediaFormat_delete(selected);
    return true;
  }

  void close()
  {
    if (codec_)
    {
      AMediaCodec_stop(codec_);
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
    }
    if (extractor_)
    {
      AMediaExtractor_delete(extractor_);
      extractor_ = nullptr;
    }
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int run(AudioShmWriter& writer)
  {
    bool format_ready = false;
    bool input_eos = false;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint64_t last_pcm_us = monotonic_us();
    while (!g_stop.load(std::memory_order_relaxed))
    {
      if (monotonic_us() - last_pcm_us > 5000000ull)
      {
        LOGE(OBF("local audio: no PCM output for five seconds").c_str());
        return -1;
      }
      const ssize_t input = input_eos ? -1 :
          AMediaCodec_dequeueInputBuffer(codec_, 5000);
      if (input >= 0)
      {
        size_t capacity = 0;
        uint8_t* bytes =
            AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(input), &capacity);
        if (!bytes)
          return -1;
        const ssize_t count = AMediaExtractor_readSampleData(extractor_, bytes,
                                                              capacity);
        const int64_t pts = AMediaExtractor_getSampleTime(extractor_);
        input_eos = count < 0;
        if (AMediaCodec_queueInputBuffer(
                codec_, static_cast<size_t>(input), 0,
                input_eos ? 0 : static_cast<size_t>(count),
                pts < 0 ? 0 : static_cast<uint64_t>(pts),
                input_eos ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0) != AMEDIA_OK)
          return -1;
        if (!input_eos)
          AMediaExtractor_advance(extractor_);
      }

      AMediaCodecBufferInfo info{};
      const ssize_t output = AMediaCodec_dequeueOutputBuffer(codec_, &info, 5000);
      if (output == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
      {
        AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
        if (!format)
          return -1;
        int32_t sr = 0, ch = 0, encoding = 2;
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
        AMediaFormat_getInt32(format, OBF("pcm-encoding").c_str(), &encoding);
        AMediaFormat_delete(format);
        if (sr < 8000 || sr > 192000 || ch < 1 || ch > 2 || encoding != 2 ||
            !writer.configure(static_cast<uint32_t>(sr),
                              static_cast<uint32_t>(ch)))
        {
          LOGE(OBF("local audio: unsupported PCM output sr=%d ch=%d encoding=%d").c_str(),
               sr, ch, encoding);
          return -1;
        }
        sample_rate = static_cast<uint32_t>(sr);
        channels = static_cast<uint32_t>(ch);
        format_ready = true;
        continue;
      }
      if (output < 0)
        continue;

      size_t capacity = 0;
      uint8_t* bytes =
          AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(output), &capacity);
      if (format_ready && bytes && info.offset >= 0 && info.size > 0 &&
          static_cast<size_t>(info.offset) <= capacity &&
          static_cast<size_t>(info.size) <=
              capacity - static_cast<size_t>(info.offset))
      {
        const uint8_t* cursor = bytes + info.offset;
        size_t remaining = static_cast<size_t>(info.size);
        const uint64_t bytes_per_second =
            static_cast<uint64_t>(sample_rate) * channels * 2u;
        while (remaining > 0 && !g_stop.load(std::memory_order_relaxed))
        {
          size_t chunk = std::min<size_t>(remaining, 8192);
          chunk -= chunk % (channels * 2u);
          if (chunk == 0)
            break;
          if (!writer.write(cursor, chunk))
          {
            AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(output),
                                            false);
            return -1;
          }
          last_pcm_us = monotonic_us();
          cursor += chunk;
          remaining -= chunk;
          sleep_interruptible_us(
              static_cast<uint64_t>(chunk) * 1000000ull / bytes_per_second);
        }
      }
      AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(output), false);
      if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
      {
        if (AMediaExtractor_seekTo(extractor_, 0,
                                   AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC) != AMEDIA_OK ||
            AMediaCodec_flush(codec_) != AMEDIA_OK)
          return -1;
        input_eos = false;
      }
    }
    return 0;
  }

private:
  int fd_ = -1;
  AMediaExtractor* extractor_ = nullptr;
  AMediaCodec* codec_ = nullptr;
};

bool read_exact_fd(int fd, void* destination, size_t count)
{
  uint8_t* cursor = static_cast<uint8_t*>(destination);
  while (count > 0)
  {
    const ssize_t got = ::read(fd, cursor, count);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return false;
    cursor += got;
    count -= static_cast<size_t>(got);
  }
  return true;
}

} // namespace

extern "C" int cr_feed_run_file_video(const char* path)
{
  FileVideoDecoder decoder;
  if (!decoder.open(path))
    return -1;
  int output_width = 0;
  int output_height = 0;
  cr_feed::ShmOut shared;

  std::vector<uint8_t> nv21;
  int64_t first_pts = -1;
  uint64_t epoch = monotonic_us();
  uint64_t published = 0;
  uint64_t last_frame_us = epoch;
  while (!g_stop.load(std::memory_order_relaxed))
  {
    if (!decoder.rewind_if_drained())
      return -1;
    VideoFrame frame{};
    if (!decoder.next(frame))
    {
      if (monotonic_us() - last_frame_us > 5000000ull)
      {
        LOGE(OBF("local video: no decoder output for five seconds").c_str());
        return -1;
      }
      if (!g_stop.load(std::memory_order_relaxed))
        sleep_interruptible_us(2000);
      continue;
    }
    if (decoder.take_looped() || first_pts < 0 || frame.pts_us < first_pts)
    {
      first_pts = frame.pts_us;
      epoch = monotonic_us();
    }
    int next_width = 0;
    int next_height = 0;
    if (!choose_output_geometry(frame.width, frame.height, decoder.rotation(),
                                next_width, next_height))
      return -1;
    if (next_width != output_width || next_height != output_height)
    {
      shared.shutdown();
      if (!shared.init(next_width, next_height))
        return -1;
      output_width = next_width;
      output_height = next_height;
    }
    uint64_t target = epoch;
    if (frame.pts_us >= first_pts)
      target += static_cast<uint64_t>(frame.pts_us - first_pts);
    const uint64_t now = monotonic_us();
    if (target > now && target - now <= 2000000)
      sleep_interruptible_us(target - now);
    else if (target > now || now - target > 500000)
    {
      // Drop accumulated scheduling debt after a slow codec or app suspend.
      epoch = now;
      first_pts = frame.pts_us;
    }
    if (!frame_to_nv21(frame, decoder.rotation(), output_width, output_height,
                       nv21))
    {
      LOGE(OBF("local video: failed to convert decoded YUV frame").c_str());
      return -1;
    }
    shared.publish_nv21(nv21.data());
    last_frame_us = monotonic_us();
    ++published;
  }
  LOGI(OBF("local video stopped after %llu frames").c_str(),
       static_cast<unsigned long long>(published));
  return 0;
}

extern "C" int cr_feed_run_file_nv21(const char* path)
{
  int fd = -1;
  off_t length = 0;
  if (!open_regular_media(path, fd, length))
    return -1;
  struct Header
  {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
  } header{};
  if (!read_exact_fd(fd, &header, sizeof(header)) ||
      header.magic != kImageMagic || header.width < 16 ||
      header.height < 16 || header.width > CR_FEED_MAX_WIDTH ||
      header.height > CR_FEED_MAX_HEIGHT || (header.width & 1u) ||
      (header.height & 1u))
  {
    ::close(fd);
    LOGE(OBF("local image: invalid LNV1 header").c_str());
    return -1;
  }
  const uint64_t payload =
      static_cast<uint64_t>(header.width) * header.height * 3ull / 2ull;
  if (payload > CR_FEED_MAX_NV21_BYTES ||
      static_cast<uint64_t>(length) != sizeof(header) + payload)
  {
    ::close(fd);
    LOGE(OBF("local image: payload size mismatch").c_str());
    return -1;
  }
  std::vector<uint8_t> nv21(static_cast<size_t>(payload));
  const bool read = read_exact_fd(fd, nv21.data(), nv21.size());
  ::close(fd);
  if (!read)
    return -1;
  cr_feed::ShmOut shared;
  if (!shared.init(static_cast<int>(header.width),
                   static_cast<int>(header.height)))
    return -1;
  while (!g_stop.load(std::memory_order_relaxed))
  {
    shared.publish_nv21(nv21.data());
    sleep_interruptible_us(33333);
  }
  return 0;
}

extern "C" int cr_feed_run_file_audio(const char* path)
{
  AudioShmWriter shared;
  if (!shared.open())
    return -1;
  FileAudioDecoder decoder;
  if (!decoder.open(path))
    return -1;
  return decoder.run(shared);
}
