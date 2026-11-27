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
#include "feed_queues.h"
#include "h264_decode_pipe.h"
#include "media/Avcc.h"
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
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "util/Obf.h"
#include "media/AdaptiveGeometry.h"
#include "media/FrameLayout.h"
#include "media/Yuv420Nv21.h"

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
constexpr int32_t kMaxDecoderDimension = 16384;
// Bound the mapped span of padded decoder YUV420 layouts.
constexpr size_t kMaxDecoderFrameBytes = 96u * 1024u * 1024u;

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
    LOGE(OBF("local media: open failed: %s").c_str(), strerror(errno));
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

struct VideoFrame
{
  cr::media::YuvPlane y;
  cr::media::YuvPlane u;
  cr::media::YuvPlane v;
  int32_t width = 0;
  int32_t height = 0;
  int64_t pts_us = 0;
  uint32_t color_flags = 0;

  VideoFrame() = default;
  VideoFrame(const VideoFrame&) = delete;
  VideoFrame& operator=(const VideoFrame&) = delete;
  ~VideoFrame() { release(); }

  void hold(AMediaCodec* codec, size_t index)
  {
    codec_ = codec;
    index_ = index;
  }

  void release()
  {
    if (codec_)
      AMediaCodec_releaseOutputBuffer(codec_, index_, false);
    codec_ = nullptr;
  }

private:
  AMediaCodec* codec_ = nullptr;
  size_t index_ = 0;
};

cr_feed::SdrColorAspects read_track_color(AMediaFormat* track, const char* mime)
{
  cr_feed::SdrColorAspects color;
  color.update(track);
  if (strcmp(mime, "video/avc") != 0 || (color.known_standard() && color.known_range()))
    return color;

  void* bytes = nullptr;
  size_t size = 0;
  if (!AMediaFormat_getBuffer(track, "csd-0", &bytes, &size) || !bytes || size < 4)
    return color;
  // Android AVC csd-0 contains a start-code-prefixed SPS. Some MP4 files
  // describe color only here, and some device codecs report default aspects.
  auto sps = std::span(static_cast<const uint8_t*>(bytes), size);
  if (sps[0] == 0 && sps[1] == 0)
  {
    if (sps[2] == 1)
      sps = sps.subspan(3);
    else if (sps[2] == 0 && sps[3] == 1)
      sps = sps.subspan(4);
  }
  const auto info = cr::media::parse_h264_sps(sps);
  return color.with_fallback(cr_feed::SdrColorAspects::from_h264_vui(info));
}

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
    track_color_ = read_track_color(selected, mime);
    int32_t fps = 0;
    if (AMediaFormat_getInt32(selected, AMEDIAFORMAT_KEY_FRAME_RATE, &fps) &&
        fps >= 1 && fps <= 240)
      nominal_frame_interval_us_ = 1000000 / fps;
    AMediaFormat_getInt32(selected, OBF("rotation-degrees").c_str(), &rotation_);
    rotation_ = ((rotation_ % 360) + 360) % 360;
    if (rotation_ != 0 && rotation_ != 90 && rotation_ != 180 && rotation_ != 270)
      rotation_ = 0;
    if (coded_width_ < 2 || coded_height_ < 2 ||
        coded_width_ > kMaxDecoderDimension || coded_height_ > kMaxDecoderDimension)
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
    output_layout_ = {};
    output_format_known_ = false;
    track_color_ = {};
    output_color_flags_ = 0;
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
    out.release();
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
        if (count >= 0 && static_cast<size_t>(count) > capacity)
        {
          LOGE(OBF("local video: sample exceeds decoder input buffer").c_str());
          return false;
        }
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

      if (!output_format_known_)
        update_output_format();
      size_t capacity = 0;
      uint8_t* bytes =
          AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(output), &capacity);
      cr::media::PlaneSpan window;
      if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
        rewind_pending_ = true;
      if (bytes && output_layout_.storage_bytes != 0 &&
          cr::media::valid_codec_window(capacity, info.offset, info.size, window) &&
          window.bytes >= output_layout_.storage_bytes)
      {
        // The frame owns this codec buffer until conversion is complete.
        // No full-size intermediate copy is necessary.
        out.hold(codec_, static_cast<size_t>(output));
        describe(bytes + window.offset, out);
        out.pts_us = info.presentationTimeUs;
        return true;
      }
      AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(output), false);
      if (rewind_pending_)
        break;
    }
    return false;
  }

  int rotation() const { return rotation_; }
  uint64_t nominal_frame_interval_us() const { return nominal_frame_interval_us_; }
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
    output_layout_ = {};
    AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
    output_format_known_ = format != nullptr;
    if (!format)
      return;
    int32_t width = coded_width_;
    int32_t height = coded_height_;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
    if (width < 2 || height < 2 || width > kMaxDecoderDimension ||
        height > kMaxDecoderDimension)
    {
      LOGW(OBF("local video: rejected decoder dimensions %dx%d").c_str(), width, height);
      AMediaFormat_delete(format);
      return;
    }
    stride_ = 0;
    slice_height_ = 0;
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
    const bool supported_color = have_color &&
        (color_ == kColorYuv420Planar || color_ == kColorYuv420PackedPlanar ||
         color_ == kColorYuv420SemiPlanar || color_ == kColorYuv420PackedSemiPlanar);
    const bool valid = supported_color && stride_ >= width && slice_height_ >= height &&
        cr::media::padded_yuv420_crop(stride_, slice_height_, crop_left_, crop_top_,
                                    crop_right_, crop_bottom_, kMaxDecoderFrameBytes,
                                    output_layout_);
    // Byte-buffer NV21 preserves the coded sample values. Prefer explicit
    // source metadata over vendor defaults for that YUV output.
    cr_feed::SdrColorAspects output_color;
    output_color.update(format);
    output_color = track_color_.with_fallback(output_color);
    output_color_flags_ = output_color.flags(false);
    __android_log_print(valid ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, TAG,
        OBF("local video output: color=0x%x stride=%d slice=%d crop=%d,%d-%d,%d valid=%d color_flags=%u").c_str(),
        color_, stride_, slice_height_, crop_left_, crop_top_, crop_right_, crop_bottom_, valid,
        output_color_flags_);
    AMediaFormat_delete(format);
  }

  void describe(const uint8_t* base, VideoFrame& out)
  {
    const size_t y_bytes = static_cast<size_t>(stride_) * slice_height_;
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
      const size_t crop = static_cast<size_t>(crop_top_ / 2) * chroma_stride +
                          static_cast<size_t>(crop_left_ / 2);
      u_offset = y_bytes + crop;
      v_offset = y_bytes + chroma_bytes + crop;
    }
    else
    {
      chroma_stride = stride_;
      pixel_stride = 2;
      const size_t crop = static_cast<size_t>(crop_top_ / 2) * stride_ +
                          static_cast<size_t>(crop_left_);
      u_offset = y_bytes + crop;
      v_offset = u_offset + 1;
    }
    out.width = output_layout_.width;
    out.height = output_layout_.height;
    out.color_flags = output_color_flags_;
    out.y = {base + static_cast<size_t>(crop_top_) * stride_ + crop_left_,
             stride_, 1};
    out.u = {base + u_offset, chroma_stride, pixel_stride};
    out.v = {base + v_offset, chroma_stride, pixel_stride};
  }

  int fd_ = -1;
  AMediaExtractor* extractor_ = nullptr;
  AMediaCodec* codec_ = nullptr;
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
  uint64_t nominal_frame_interval_us_ = 33333;
  cr::media::Yuv420Crop output_layout_;
  cr_feed::SdrColorAspects track_color_;
  uint32_t output_color_flags_ = 0;
  bool output_format_known_ = false;
  bool looped_ = false;
  bool input_eos_ = false;
  bool rewind_pending_ = false;
};

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

struct PlaybackFrame
{
  cr_feed::Nv21Frame video;
  bool looped = false;
};

class FileVideoPlayback
{
public:
  ~FileVideoPlayback()
  {
    stopping_.store(true, std::memory_order_relaxed);
    free_.close();
    ready_.close();
    if (worker_.joinable())
      worker_.join();
  }

  bool open(const char* path)
  {
    if (!decoder_.open(path))
      return false;
    try
    {
      for (auto& slot : buffers_)
        free_.push(&slot);
      worker_ = std::thread(&FileVideoPlayback::decode, this);
    }
    catch (const std::exception& error)
    {
      LOGE(OBF("local video: could not start decode worker (%s)").c_str(), error.what());
      return false;
    }

    // Let the decoder start ahead of playback. This gives it time to seek
    // and flush at a loop boundary while queued frames continue to play.
    while (!g_stop.load(std::memory_order_relaxed) && !done_.load(std::memory_order_acquire) &&
           ready_.size() < buffers_.size() - 1)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return true;
  }

  uint64_t nominal_frame_interval_us() const { return decoder_.nominal_frame_interval_us(); }

  PlaybackFrame* next()
  {
    PlaybackFrame* slot = nullptr;
    while (!g_stop.load(std::memory_order_relaxed))
    {
      if (ready_.pop_for(slot, std::chrono::milliseconds(50)))
        return slot;
      if (done_.load(std::memory_order_acquire))
        break;
    }
    return nullptr;
  }

  void release(PlaybackFrame* slot)
  {
    free_.push(std::move(slot));
  }

private:
  bool stopping() const
  {
    return stopping_.load(std::memory_order_relaxed) || g_stop.load(std::memory_order_relaxed);
  }

  void decode() noexcept
  {
    try
    {
      cr::media::Yuv420Nv21 converter;
      while (!stopping())
      {
        PlaybackFrame* slot = nullptr;
        if (!free_.pop_for(slot, std::chrono::milliseconds(50)))
          continue;
        if (!fill(*slot, converter))
          break;
        // Each queue holds pointers to at most the number of backing
        // buffers. A slot is never in both queues at the same time.
        if (!ready_.push(std::move(slot)))
          break;
      }
    }
    catch (const std::bad_alloc&)
    {
      LOGE(OBF("local video: insufficient memory for NV21 conversion").c_str());
    }
    done_.store(true, std::memory_order_release);
    ready_.close();
  }

  bool fill(PlaybackFrame& slot, cr::media::Yuv420Nv21& converter)
  {
    const uint64_t deadline = monotonic_us() + 5000000ull;
    while (!stopping())
    {
      if (!decoder_.rewind_if_drained())
        return false;
      VideoFrame frame;
      if (decoder_.next(frame))
      {
        auto& output = slot.video;
        if (!choose_output_geometry(frame.width, frame.height, decoder_.rotation(),
                                    output.width, output.height) ||
            !converter.convert(frame.y, frame.u, frame.v, frame.width, frame.height,
                               decoder_.rotation(), output.width, output.height, output.data))
        {
          LOGE(OBF("local video: failed to convert decoded YUV frame").c_str());
          return false;
        }
        output.pts_us = frame.pts_us;
        output.color_flags = frame.color_flags;
        slot.looped = decoder_.take_looped();
        return true;
      }
      if (monotonic_us() >= deadline)
      {
        LOGE(OBF("local video: no decoder output for five seconds").c_str());
        return false;
      }
      sleep_interruptible_us(2000);
    }
    return false;
  }

  static constexpr std::size_t kBuffers = 6;
  FileVideoDecoder decoder_;
  std::array<PlaybackFrame, kBuffers> buffers_;
  cr_feed::BoundedQueue<PlaybackFrame*> free_{kBuffers};
  cr_feed::BoundedQueue<PlaybackFrame*> ready_{kBuffers};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> done_{false};
  std::thread worker_;
};

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
  FileVideoPlayback playback;
  if (!playback.open(path))
    return -1;
  int output_width = 0;
  int output_height = 0;
  cr_feed::ShmOut shared;

  int64_t first_pts = -1;
  int64_t previous_pts = -1;
  uint64_t epoch = monotonic_us();
  uint64_t previous_target = 0;
  uint64_t interval = playback.nominal_frame_interval_us();
  uint64_t published = 0;
  while (!g_stop.load(std::memory_order_relaxed))
  {
    auto* slot = playback.next();
    if (!slot)
    {
      if (!g_stop.load(std::memory_order_relaxed))
        return -1;
      break;
    }
    const auto& frame = slot->video;
    const bool looped = slot->looped;
    const int64_t pts = std::max<int64_t>(0, frame.pts_us);
    if (looped || first_pts < 0 || pts < previous_pts)
    {
      first_pts = pts;
      const uint64_t now = monotonic_us();
      epoch = looped ? std::max(now, previous_target + interval) : now;
    }
    if (!looped && previous_pts >= 0 && pts > previous_pts && pts - previous_pts <= 2000000)
    {
      interval = static_cast<uint64_t>(pts - previous_pts);
    }
    if (frame.width != output_width || frame.height != output_height)
    {
      shared.shutdown();
      if (!shared.init(frame.width, frame.height))
        return -1;
      output_width = frame.width;
      output_height = frame.height;
    }
    uint64_t target = epoch + static_cast<uint64_t>(pts - first_pts);
    const uint64_t now = monotonic_us();
    if (target > now && target - now <= 2000000)
      sleep_interruptible_us(target - now);
    else if (target > now || now - target > 500000)
    {
      // Drop accumulated scheduling debt after a slow codec or app suspend.
      epoch = now;
      first_pts = pts;
      target = now;
    }
    if (g_stop.load(std::memory_order_relaxed))
      break;
    shared.publish_nv21(frame.data.data(), frame.color_flags);
    playback.release(slot);
    previous_pts = pts;
    previous_target = target;
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
