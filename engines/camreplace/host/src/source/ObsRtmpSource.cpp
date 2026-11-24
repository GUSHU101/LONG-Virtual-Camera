#include "source/ObsRtmpSource.h"
#include "render/LivePreview.h"
#include "util/Log.h"

#include <chrono>
#include <exception>
#include <string>
#include <thread>
namespace cr::source
{

namespace
{

void emit(const cr::device::DeployCallback& cb, cr::device::DeployStatus::Kind k, std::string step, std::string detail = {})
{
  if (cb)
  {
    try
    {
      cb({k, std::move(step), std::move(detail)});
    }
    catch (const std::exception& e)
    {
      cr::log::warn("obs", std::string("status callback failed: ") + e.what());
    }
    catch (...)
    {
      cr::log::warn("obs", "status callback failed: unknown exception");
    }
  }
}

} // namespace

bool ObsRtmpSource::start(std::string serial, const Config& cfg, cr::device::DeployCallback cb)
{
  if (running_.load())
    return false;
  serial_ = std::move(serial);
  cfg_ = cfg;
  cb_ = std::move(cb);
  video_forward_.store(cfg.video_forward, std::memory_order_release);
  preview_decode_.store(cfg.preview_decode || cfg.video_forward, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lk(mu_);
    status_ = {};
    status_.rtmp_url = "rtmp://127.0.0.1:" + std::to_string(cfg_.rtmp_port) + "/live/" + cfg_.stream_key;
  }
  {
    std::lock_guard<std::mutex> tx_lk(tx_mu_);
    tx_header_sent_ = false;
  }

  // Connect to cr_feed_proc (H.264 receiver) on the phone first — only
  // when this session forwards video. Audio-only mode (Start replace
  // sound) skips this entirely so we don't try to bind to a port that
  // has no listener and so the phone-side video substitution stays
  // off (no NALUs reach /data/cr/feed → camhook, even if previously
  // injected, has nothing fresh to replay).
  if (video_forward_.load(std::memory_order_acquire))
  {
    std::lock_guard<std::mutex> tx_lk(tx_mu_);
    bool connected = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    for (int i = 0; i < 20 && std::chrono::steady_clock::now() < deadline; ++i)
    {
      if (tx_.open(cfg_.adb_port, cr::secure_channel::v2::StreamKind::Video))
      {
        connected = true;
        break;
      }
      if (std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (!connected)
    {
      emit(cb_, cr::device::DeployStatus::Kind::Err, "obs: connect to cr_feed_proc");
      return false;
    }
    tx_header_sent_ = false;
  }

  // Side-channel: decode H.264 locally only for the preview pane. The
  // product has one phone transport: Annex-B H.264 over the authenticated
  // loopback tunnel, decoded into the shared frame ring on Android.
  bool ok = false;
  try
  {
    preview_dec_.arm([](const uint8_t* bgra, int w, int h, int pitch) {
      cr::render::LivePreview::instance().publish_bgra(bgra, w, h, pitch);
    });

    ok = rtmp_.start(
        cfg_.rtmp_port, [this](const cr::transport::RtmpStreamMeta& m) { on_meta_(m); }, [this](const uint8_t* data, size_t len, int64_t pts_us, bool key) { on_nalu_(data, len, pts_us, key); }, [this](bool c, const std::string& detail) { on_conn_(c, detail); }, [this](const cr::transport::RtmpVideoEvent& event) { on_video_(event); },
        // Audio side-channel — RtmpServer routes both AAC csd and raw
        // frames into our AudioPump. The pump itself drops them when the
        // user hasn't enabled "Start replace sound", so wiring this here
        // costs nothing in the camera-only case.
        [this](const uint8_t* csd, size_t len) { on_aac_csd_(csd, len); }, [this](const uint8_t* aac, size_t len, int64_t pts_us) { audio_.on_aac_frame(aac, len, pts_us); });
  }
  catch (const std::exception& e)
  {
    cr::log::warn("obs", std::string("start failed: ") + e.what());
  }
  catch (...)
  {
    cr::log::warn("obs", "start failed: unknown exception");
  }
  if (!ok)
  {
    emit(cb_, cr::device::DeployStatus::Kind::Err, "obs: RTMP listen failed", "port " + std::to_string(cfg_.rtmp_port) + " in use?");
    rtmp_.stop();
    preview_dec_.stop();
    {
      std::lock_guard<std::mutex> tx_lk(tx_mu_);
      tx_.close();
      tx_header_sent_ = false;
    }
    return false;
  }

  emit(cb_, cr::device::DeployStatus::Kind::Ok, "obs: RTMP listening", status().rtmp_url);
  running_.store(true);
  return true;
}

void ObsRtmpSource::stop()
{
  const bool was_running = running_.exchange(false);
  rtmp_.stop(); // joins server thread — no more on_nalu_ races
  preview_dec_.stop();
  audio_.stop();
  {
    std::lock_guard<std::mutex> tx_lk(tx_mu_);
    tx_.close();
    tx_header_sent_ = false;
  }
  cr::render::LivePreview::instance().reset();
  if (was_running)
    emit(cb_, cr::device::DeployStatus::Kind::Ok, "obs: stopped");
}

void ObsRtmpSource::set_preview_decode(bool on)
{
  preview_decode_.store(on, std::memory_order_release);
  if (!on)
  {
    cr::render::LivePreview::instance().reset();
  }
}

bool ObsRtmpSource::set_video_forward(bool on)
{
  if (!running_.load())
  {
    // Not started — record desired state for the next start().
    video_forward_.store(on, std::memory_order_release);
    cfg_.video_forward = on;
    return true;
  }
  {
    std::lock_guard<std::mutex> tx_lk(tx_mu_);
    if (video_forward_.load(std::memory_order_acquire) == on && (!on || tx_.is_open()))
    {
      return true; // already in the requested state
    }
  }

  if (on)
  {
    // Turning forwarding on — need a live socket to cr_feed_proc.
    // Brief retry loop covers the case where ensure_video_pipeline_
    // has just launched cr_feed_proc and the tcp_h264 listener is
    // a few hundred ms away from accept-ready.
    // Sleep outside tx_mu_ so on_nalu_ / status paths are not stalled
    // for the full retry window (~3s).
    bool connected = false;
    {
      std::lock_guard<std::mutex> tx_lk(tx_mu_);
      connected = tx_.is_open();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    for (int i = 0; !connected && i < 20 &&
                    std::chrono::steady_clock::now() < deadline;
         ++i)
    {
      {
        std::lock_guard<std::mutex> tx_lk(tx_mu_);
        if (tx_.is_open() || tx_.open(cfg_.adb_port, cr::secure_channel::v2::StreamKind::Video))
        {
          connected = true;
          tx_header_sent_ = false; // resend header on next NAL
          break;
        }
      }
      if (std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (!connected)
    {
      cr::log::warn("obs", "set_video_forward(true): cr_feed_proc connect failed");
      return false;
    }
    video_forward_.store(true, std::memory_order_release);
    preview_decode_.store(true, std::memory_order_release);
    cr::log::info("obs", "video forward: ON");
  }
  else
  {
    // Turning off: drop the socket and clear the live-preview pane —
    // on_nalu_ stops feeding preview_dec_ when video_forward_=false
    // (the user explicitly asked for no preview during sound-only
    // replace), so the last decoded BGRA frame would otherwise sit
    // frozen in the panel until something repaints it.
    video_forward_.store(false, std::memory_order_release);
    preview_decode_.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> tx_lk(tx_mu_);
      tx_.close();
      tx_header_sent_ = false;
    }
    cr::render::LivePreview::instance().reset();
    cr::log::info("obs", "video forward: OFF");
  }
  return true;
}

bool ObsRtmpSource::start_audio(const AudioPump::Config& cfg, cr::device::DeployCallback cb)
{
  if (!running_.load())
    return false; // RTMP must be up first
  if (!audio_.start(serial_, cfg, std::move(cb)))
    return false;
  std::vector<std::uint8_t> cached;
  {
    std::lock_guard<std::mutex> lk(mu_);
    cached = last_aac_csd_;
  }
  if (!cached.empty())
    audio_.on_aac_csd(cached.data(), cached.size());
  return true;
}

void ObsRtmpSource::stop_audio()
{
  audio_.stop();
}

ObsRtmpSource::Status ObsRtmpSource::status() const
{
  Status result{};
  {
    std::lock_guard<std::mutex> lk(mu_);
    result = status_;
  }
  const auto audio = audio_.status();
  result.audio_health = audio.health;
  result.audio_bytes_sent = audio.bytes_sent;
  return result;
}

void ObsRtmpSource::on_meta_(const cr::transport::RtmpStreamMeta& m)
{
  std::lock_guard<std::mutex> lk(mu_);
  status_.stream_w = m.width;
  status_.stream_h = m.height;
  status_.stream_fps = m.framerate;
  cr::log::info("obs", "meta " + std::to_string(m.width) + "x" + std::to_string(m.height) + " @ " + std::to_string((int) m.framerate) + " fps");
}

void ObsRtmpSource::on_conn_(bool connected, const std::string& detail)
{
  {
    std::lock_guard<std::mutex> lk(mu_);
    status_.obs_connected = connected;
  }
  cr::log::info("obs", std::string(connected ? "connected" : "disconnected") + (detail.empty() ? "" : " (" + detail + ")"));

  // When OBS disconnects mid-stream we MUST tear down the phone-side TCP
  // too. The receiver on the phone is stateful — it expects a stream
  // header followed by chunk frames. If we just keep the same socket
  // open and re-send the header on reconnect, the phone reads the
  // header magic as the next chunk's length field and trips its sanity
  // guard ("bogus packet len ..."). Closing forces cr_feed_proc to
  // accept() us cleanly when the next NALU triggers a reopen.
  if (!connected)
  {
    {
      std::lock_guard<std::mutex> lk(mu_);
      last_aac_csd_.clear();
    }
    std::lock_guard<std::mutex> tx_lk(tx_mu_);
    tx_.close();
    tx_header_sent_ = false;
  }
}

void ObsRtmpSource::on_aac_csd_(const uint8_t* csd, size_t len)
{
  if (!csd || len < 2 || len > 64)
    return;
  {
    std::lock_guard<std::mutex> lk(mu_);
    last_aac_csd_.assign(csd, csd + len);
  }
  audio_.on_aac_csd(csd, len);
}

void ObsRtmpSource::on_video_(const cr::transport::RtmpVideoEvent& event)
{
  bool log_unsupported = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    ++status_.video_tags;
    status_.last_video_codec_id = static_cast<int>(event.codec_id);
    status_.last_avc_packet_type = event.avc_packet_type;
    if (event.codec_id != 7)
    {
      ++status_.unsupported_video_tags;
      log_unsupported = (status_.unsupported_video_tags == 1);
    }
    else if (event.avc_packet_type == 0)
    {
      ++status_.avc_sequence_headers;
    }
    else if (event.avc_packet_type == 1)
    {
      ++status_.avc_media_packets;
      if (event.keyframe)
      {
        ++status_.avc_keyframes;
      }
    }
  }

  if (log_unsupported)
  {
    cr::log::warn("rtmp", "unsupported OBS video codec_id=" + std::to_string(static_cast<int>(event.codec_id)) + "; set OBS video encoder to H.264/AVC");
  }
}

void ObsRtmpSource::on_nalu_(const uint8_t* data, size_t len, int64_t pts_us, bool is_keyframe)
{
  if (!len)
    return;

  {
    std::lock_guard<std::mutex> lk(mu_);
    ++status_.frames;
    if (is_keyframe)
      ++status_.keyframes;
    status_.last_nalu_pts_us = pts_us;
  }

  // Feed the host-side preview decoder as soon as camera/photo startup
  // asks for it, even before phone forwarding is armed. This lets the
  // UI show OBS while Android arm_video is still coming up. Sound-only
  // sessions keep preview_decode_ false so we do not burn CPU decoding
  // video nobody sees.
  const bool preview_decode = preview_decode_.load(std::memory_order_acquire);
  if (preview_decode)
  {
    preview_dec_.feed(data, len, pts_us);
  }

  if (!video_forward_.load(std::memory_order_acquire))
    return;

  std::lock_guard<std::mutex> tx_lk(tx_mu_);
  if (!video_forward_.load(std::memory_order_acquire))
    return;

  // Reopen the phone-side TCP if we closed it on a previous OBS
  // disconnect. cr_feed_proc loops on accept() so a fresh connection
  // here resets its internal state cleanly.
  if (!tx_.is_open())
  {
    if (!tx_.open(cfg_.adb_port, cr::secure_channel::v2::StreamKind::Video))
      return;
    tx_header_sent_ = false;
  }

  // Defer the phone-side header until we know the stream's resolution
  // (first video tag with a resolved width/height). The hook on the
  // phone reads width/height from our TCP header — sending 0×0 would
  // trip its own sanity guard.
  if (!tx_header_sent_)
  {
    int w = 0, h = 0, fps = 30;
    {
      std::lock_guard<std::mutex> lk(mu_);
      w = status_.stream_w;
      h = status_.stream_h;
      fps = status_.stream_fps > 0 ? (int) (status_.stream_fps + 0.5) : 30;
    }
    if (w <= 0 || h <= 0)
    {
      // No onMetaData yet — wait another frame. Drop this NALU.
      return;
    }
    if (!tx_.send_h264_header(w, h, fps))
    {
      cr::log::warn("obs", "send_h264_header failed");
      return;
    }
    cr::log::info("obs", "h264 header " + std::to_string(w) + "x" + std::to_string(h) + " @" + std::to_string(fps) + " fps");
    tx_header_sent_ = true;
  }

  if (!tx_.send_h264_chunk(data, len, pts_us, is_keyframe))
  {
    cr::log::warn("obs", "send_h264_chunk failed");
    return;
  }
}

} // namespace cr::source
