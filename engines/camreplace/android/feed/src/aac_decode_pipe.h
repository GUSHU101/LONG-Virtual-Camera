#pragma once

// Phone-side AAC → S16LE PCM → /data/cr/audio pipe (sprint B follow-up).
// Lives next to H264DecodePipe for the legacy opt-in rtmp_h264_feed.cpp
// path: when an FLV audio tag (type 0x08) arrives, the RTMP server
// hands the AAC payload to AacDecodePipe::push(); when an AAC sequence
// header (AACPacketType=0) arrives we configure the decoder from the
// AudioSpecificConfig and arm the audio shm.
// The output rate matches the AAC stream natively (typically 44100 or
// 48000, stereo). libcr_audhook reads the shm without resampling, so
// whatever rate we publish is what apps see.
// Why this exists in direct-RTMP mode:
//   In legacy mode the PC's RtmpServer demuxed audio out of the FLV
//   stream and fed it to AudioPump (Media Foundation AAC decoder), which
//   shipped raw PCM to a separate cr_feed_proc instance running with
//   tcp_pcm:listen:8902. With direct-RTMP the AAC is now arriving on
//   the phone — there's no PC-side decode any more. So we decode here.

#include "audio_shm.h"
#include "media_codec_runtime.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <android/log.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <vector>
#include "util/Obf.h"

#define CR_AUDIO_LOGI(...) __android_log_print(ANDROID_LOG_INFO, OBF("cr_feed").c_str(), __VA_ARGS__)
#define CR_AUDIO_LOGW(...) __android_log_print(ANDROID_LOG_WARN, OBF("cr_feed").c_str(), __VA_ARGS__)
#define CR_AUDIO_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, OBF("cr_feed").c_str(), __VA_ARGS__)

namespace cr_feed
{

class AacDecodePipe
{
public:
  // Open + ftruncate /data/cr/audio so the shm exists (we don't need
  // CSD yet at this point — header is rewritten on each csd push).
  bool open_shm()
  {
    if (audio_fd_ >= 0)
      return true;
    mkdir(OBF("/data/cr").c_str(), 0711);
    const size_t want = sizeof(cr_audio_header) + CR_AUDIO_RING_BYTES;
    int fd = ::open(OBF("/data/cr/audio").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
    {
      CR_AUDIO_LOGE(OBF("aac: open %s: %s").c_str(), OBF("/data/cr/audio").c_str(), strerror(errno));
      return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
      CR_AUDIO_LOGE(OBF("aac: another audio producer owns %s: %s").c_str(),
                    OBF("/data/cr/audio").c_str(), strerror(errno));
      ::close(fd);
      return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0)
    {
      ::close(fd);
      return false;
    }
    if ((size_t) st.st_size < want)
    {
      if (ftruncate(fd, (off_t) want) != 0)
      {
        CR_AUDIO_LOGE(OBF("aac: ftruncate %s: %s").c_str(), OBF("/data/cr/audio").c_str(), strerror(errno));
        ::close(fd);
        return false;
      }
    }
    void* m = mmap(nullptr, want, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)
    {
      CR_AUDIO_LOGE(OBF("aac: mmap: %s").c_str(), strerror(errno));
      ::close(fd);
      return false;
    }
    audio_hdr_ = (cr_audio_header*) m;
    audio_ring_ = (uint8_t*) m + sizeof(cr_audio_header);
    audio_map_ = m;
    audio_size_ = want;
    audio_fd_ = fd;
    // A crashed predecessor may have left READY and the magic published.
    // Revoke it while this new writer prepares its first complete header.
    atomic_store_explicit(&audio_hdr_->channel_state, CR_CHANNEL_STATE_NONE,
                          memory_order_release);
    __atomic_store_n(&audio_hdr_->magic, 0u, __ATOMIC_RELEASE);
    return true;
  }

  // Configure the AAC decoder from the AudioSpecificConfig blob (FLV
  // AAC packet type 0). Replaces any prior decoder so a stream
  // restart with a different format works without leaks.
  bool on_csd(const uint8_t* csd, size_t len)
  {
    if (!csd || len < 2 || len > 64)
      return false;

    (void) ensure_media_codec_binder_pool();

    // Parse AudioSpecificConfig: object_type (5b), sr_idx (4b),
    // channel_cfg (4b). Same lookup table the PC AudioPump uses.
    const uint8_t b0 = csd[0];
    const uint8_t b1 = csd[1];
    const int sr_idx = ((b0 & 0x07) << 1) | (b1 >> 7);
    const int channels = (b1 >> 3) & 0x0f;
    static const int kSrTable[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0};
    const int sample_rate = (sr_idx >= 0 && sr_idx < 13) ? kSrTable[sr_idx] : 0;
    if (sample_rate <= 0 || channels < 1 || channels > 2)
    {
      CR_AUDIO_LOGW(OBF("aac: bad csd sr_idx=%d ch=%d").c_str(), sr_idx, channels);
      return false;
    }

    teardown_decoder();

    dec_ = AMediaCodec_createDecoderByType(OBF("audio/mp4a-latm").c_str());
    if (!dec_)
    {
      CR_AUDIO_LOGE(OBF("aac: createDecoderByType failed").c_str());
      return false;
    }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, OBF("audio/mp4a-latm").c_str());
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, sample_rate);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
    AMediaFormat_setBuffer(fmt, "csd-0", csd, len);

    media_status_t cs = AMediaCodec_configure(dec_, fmt, nullptr, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (cs != AMEDIA_OK)
    {
      CR_AUDIO_LOGE(OBF("aac: configure failed status=%d").c_str(), (int) cs);
      AMediaCodec_delete(dec_);
      dec_ = nullptr;
      return false;
    }
    media_status_t ss = AMediaCodec_start(dec_);
    if (ss != AMEDIA_OK)
    {
      CR_AUDIO_LOGE(OBF("aac: start failed status=%d").c_str(), (int) ss);
      AMediaCodec_delete(dec_);
      dec_ = nullptr;
      return false;
    }

    sample_rate_ = (uint32_t) sample_rate;
    channels_ = (uint16_t) channels;
    pcm_encoding_ = 2; // Android AudioFormat.ENCODING_PCM_16BIT

    if (!open_shm())
    {
      teardown_decoder();
      return false;
    }
    publish_audio_header();
    CR_AUDIO_LOGI(OBF("aac: pipe started sr=%u ch=%u").c_str(), sample_rate_, (unsigned) channels_);
    ready_ = true;
    return true;
  }

  // Feed one AAC raw access unit (FLV AAC packet type 1).
  void push(const uint8_t* aac, size_t len, int64_t pts_us)
  {
    if (!ready_ || !dec_ || !aac || !len)
      return;

    ssize_t in_idx = AMediaCodec_dequeueInputBuffer(dec_, 5'000);
    if (in_idx >= 0)
    {
      size_t cap = 0;
      uint8_t* in_buf = AMediaCodec_getInputBuffer(dec_, (size_t) in_idx, &cap);
      if (in_buf && cap >= len)
      {
        std::memcpy(in_buf, aac, len);
        const media_status_t queued = AMediaCodec_queueInputBuffer(
            dec_, (size_t) in_idx, 0, len,
            pts_us < 0 ? 0u : (uint64_t) pts_us, 0);
        if (queued != AMEDIA_OK)
        {
          CR_AUDIO_LOGW(OBF("aac: queueInput failed status=%d").c_str(),
                        (int) queued);
          ready_ = false;
        }
      }
      else
      {
        // A dequeued codec buffer must always be returned. Leaving it owned by
        // us permanently drains the input pool after a few oversized packets.
        CR_AUDIO_LOGW(OBF("aac: input packet too large len=%zu cap=%zu").c_str(),
                      len, cap);
        AMediaCodec_queueInputBuffer(dec_, (size_t) in_idx, 0, 0,
                                     pts_us < 0 ? 0u : (uint64_t) pts_us, 0);
      }
    }
    drain();
  }

  void drain()
  {
    if (!dec_)
      return;
    for (;;)
    {
      AMediaCodecBufferInfo info;
      ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(dec_, &info, 0);
      if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        break;
      if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
      {
        // Audio format changes on first output; refresh sr/ch
        // from the codec's actual output and republish header.
        AMediaFormat* of = AMediaCodec_getOutputFormat(dec_);
        int32_t sr = sample_rate_, ch = channels_;
        int32_t pcm_encoding = 2;
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
        AMediaFormat_getInt32(of, OBF("pcm-encoding").c_str(), &pcm_encoding);
        AMediaFormat_delete(of);
        if (sr >= 8000 && sr <= 192000)
          sample_rate_ = (uint32_t) sr;
        if (ch == 1 || ch == 2)
          channels_ = (uint16_t) ch;
        pcm_encoding_ = pcm_encoding;
        if (pcm_encoding_ != 2)
          CR_AUDIO_LOGW(OBF("aac: unsupported decoder PCM encoding=%d; dropping output").c_str(),
                        pcm_encoding_);
        publish_audio_header();
        continue;
      }
      if (out_idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
        continue;
      if (out_idx < 0)
        break;

      if (info.size > 0 && audio_hdr_)
      {
        size_t cap = 0;
        uint8_t* out_buf = AMediaCodec_getOutputBuffer(dec_, (size_t) out_idx, &cap);
        if (out_buf && info.offset >= 0 && info.size >= 0 &&
            static_cast<size_t>(info.offset) <= cap &&
            static_cast<size_t>(info.size) <= cap - static_cast<size_t>(info.offset))
        {
          write_to_ring(out_buf + info.offset, static_cast<size_t>(info.size));
        }
        else
        {
          CR_AUDIO_LOGW(OBF("aac: invalid output window off=%d size=%d cap=%zu").c_str(),
                        static_cast<int>(info.offset),
                        static_cast<int>(info.size), cap);
        }
      }
      AMediaCodec_releaseOutputBuffer(dec_, (size_t) out_idx, false);
    }
  }

  void stop()
  {
    teardown_decoder();
    if (audio_hdr_)
    {
      // Revoke replacement before invalidating metadata so the hook returns
      // immediately to the phone microphone.
      atomic_store_explicit(&audio_hdr_->channel_state,
                            CR_CHANNEL_STATE_NONE, memory_order_release);
      __atomic_store_n(&audio_hdr_->magic, 0u, __ATOMIC_RELEASE);
      munmap(audio_map_, audio_size_);
      audio_hdr_ = nullptr;
      audio_map_ = nullptr;
      audio_ring_ = nullptr;
    }
    if (audio_fd_ >= 0)
    {
      ::close(audio_fd_);
      audio_fd_ = -1;
    }
    ready_ = false;
  }

  ~AacDecodePipe()
  {
    stop();
  }

private:
  void teardown_decoder()
  {
    if (dec_)
    {
      AMediaCodec_stop(dec_);
      AMediaCodec_delete(dec_);
      dec_ = nullptr;
    }
  }

  void publish_audio_header()
  {
    if (!audio_hdr_)
      return;
    auto* h = audio_hdr_;
    // Same magic-zero / write / fence / magic-restore dance the
    // legacy tcp_pcm path uses, so a partial header rewrite never
    // tricks the audhook into reading half-old fields.
    __atomic_store_n(&h->magic, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&h->version, CR_AUDIO_VERSION, __ATOMIC_RELAXED);
    __atomic_store_n(&h->sample_rate, sample_rate_, __ATOMIC_RELAXED);
    __atomic_store_n(&h->channels, static_cast<uint32_t>(channels_),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&h->bytes_per_sample, 2u, __ATOMIC_RELAXED);
    __atomic_store_n(&h->ring_bytes, CR_AUDIO_RING_BYTES, __ATOMIC_RELAXED);
    __atomic_store_n(&h->reserved[0], 0u, __ATOMIC_RELAXED);
    atomic_store(&h->write_pos, 0ull);
    atomic_store_explicit(&h->generation, cr_audio_next_generation(h),
                          memory_order_relaxed);
    atomic_store(&h->channel_state, CR_CHANNEL_STATE_NONE);
    __atomic_store_n(&h->magic, CR_AUDIO_MAGIC, __ATOMIC_RELEASE);
  }

  void write_to_ring(const uint8_t* pcm, size_t bytes)
  {
    if (!audio_hdr_ || !audio_ring_ || !pcm || !bytes || pcm_encoding_ != 2)
      return;
    const size_t frame_bytes = static_cast<size_t>(channels_) * 2u;
    if (frame_bytes == 0)
      return;
    bytes -= bytes % frame_bytes;
    if (bytes == 0)
      return;
    const uint32_t ring =
        __atomic_load_n(&audio_hdr_->ring_bytes, __ATOMIC_ACQUIRE);
    if (ring == 0)
      return;
    const uint64_t wp = atomic_load(&audio_hdr_->write_pos);
    // Keep only the newest ring-sized suffix if a codec ever returns a
    // malformed/oversized buffer. The monotonic cursor still accounts for
    // every input byte, so consumers detect the overrun and resynchronise.
    const size_t kept = bytes > ring ? ring : bytes;
    const size_t dropped = bytes - kept;
    const uint8_t* kept_pcm = pcm + dropped;
    const uint64_t write_start = wp + dropped;
    const uint32_t off = static_cast<uint32_t>(write_start % ring);
    const size_t span1 = (off + kept <= ring) ? kept : (ring - off);
    const uint32_t write_token = cr_audio_begin_write(audio_hdr_);
    std::memcpy(audio_ring_ + off, kept_pcm, span1);
    if (span1 < kept)
    {
      std::memcpy(audio_ring_, kept_pcm + span1, kept - span1);
    }
    atomic_store_explicit(&audio_hdr_->write_pos, wp + bytes,
                          memory_order_release);
    atomic_store_explicit(&audio_hdr_->channel_state,
                          CR_CHANNEL_STATE_READY, memory_order_release);
    cr_audio_end_write(audio_hdr_, write_token);
  }

  AMediaCodec* dec_ = nullptr;
  bool ready_ = false;
  uint32_t sample_rate_ = 0;
  uint16_t channels_ = 0;
  int32_t pcm_encoding_ = 2;

  // Owned audio shm.
  int audio_fd_ = -1;
  void* audio_map_ = nullptr;
  size_t audio_size_ = 0;
  cr_audio_header* audio_hdr_ = nullptr;
  uint8_t* audio_ring_ = nullptr;
};

} // namespace cr_feed
