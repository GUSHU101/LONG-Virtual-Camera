#include "render/H264PreviewDecoder.h"
#include "util/Log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <exception>
#include <new>
#include <string>
using Microsoft::WRL::ComPtr;

namespace cr::render
{

std::atomic<bool> H264PreviewDecoder::g_enabled_{true};
std::atomic<bool> H264PreviewDecoder::g_start_failed_{false};

void H264PreviewDecoder::set_enabled(bool e)
{
  g_enabled_.store(e, std::memory_order_relaxed);
}

bool H264PreviewDecoder::is_enabled()
{
  return g_enabled_.load(std::memory_order_relaxed);
}

bool H264PreviewDecoder::start_failed()
{
  return g_start_failed_.load(std::memory_order_relaxed);
}

namespace
{

class ComWorkerScope
{
public:
  ComWorkerScope() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComWorkerScope() noexcept
  {
    if (result_ == S_OK || result_ == S_FALSE)
      CoUninitialize();
  }
  [[nodiscard]] bool usable() const noexcept
  {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }

private:
  HRESULT result_;
};

// BT.601 limited-range NV12 → BGRA. Same matrix the rest of the project
// uses (TexturePreview, Nv21) so the preview colours match what the
// phone-side hook ultimately blits into the camera buffer.
inline void nv12_to_bgra_row(const uint8_t* y_row, const uint8_t* uv_row, uint8_t* out, int width)
{
  for (int x = 0; x < width; ++x)
  {
    int Y = (int) y_row[x] - 16;
    int U = (int) uv_row[(x & ~1) + 0] - 128;
    int V = (int) uv_row[(x & ~1) + 1] - 128;
    if (Y < 0)
      Y = 0;
    int R = (298 * Y + 409 * V + 128) >> 8;
    int G = (298 * Y - 100 * U - 208 * V + 128) >> 8;
    int B = (298 * Y + 516 * U + 128) >> 8;
    if (R < 0)
      R = 0;
    else if (R > 255)
      R = 255;
    if (G < 0)
      G = 0;
    else if (G > 255)
      G = 255;
    if (B < 0)
      B = 0;
    else if (B > 255)
      B = 255;
    out[x * 4 + 0] = (uint8_t) B;
    out[x * 4 + 1] = (uint8_t) G;
    out[x * 4 + 2] = (uint8_t) R;
    out[x * 4 + 3] = 255;
  }
}

} // namespace

void H264PreviewDecoder::arm(PublishFn publish)
{
  if (worker_.joinable())
    return;
  publish_ = std::move(publish);
  start_failed_ = false;
  g_start_failed_.store(false, std::memory_order_relaxed);

  stopping_.store(false);
  try
  {
    worker_ = std::thread([this]
                          {
                            ComWorkerScope com;
                            if (!com.usable())
                            {
                              cr::log::warn("preview", "COM initialization failed");
                              teardown_();
                              start_failed_ = true;
                              g_start_failed_.store(true, std::memory_order_relaxed);
                              return;
                            }
                            try
                            {
                              worker_loop_();
                            }
                            catch (const std::exception& e)
                            {
                              cr::log::warn("preview", std::string("decoder worker stopped: ") + e.what());
                              teardown_();
                              start_failed_ = true;
                              g_start_failed_.store(true, std::memory_order_relaxed);
                            }
                            catch (...)
                            {
                              cr::log::warn("preview", "decoder worker stopped: unknown exception");
                              teardown_();
                              start_failed_ = true;
                              g_start_failed_.store(true, std::memory_order_relaxed);
                            }
                          });
  }
  catch (const std::exception& e)
  {
    publish_ = nullptr;
    start_failed_ = true;
    g_start_failed_.store(true, std::memory_order_relaxed);
    cr::log::warn("preview", std::string("decoder worker unavailable: ") + e.what());
  }
}

void H264PreviewDecoder::stop()
{
  if (worker_.joinable())
  {
    {
      std::lock_guard<std::mutex> lk(q_mu_);
      stopping_.store(true);
      q_.clear();
    }
    q_cv_.notify_all();
    worker_.join();
  }
  publish_ = nullptr;
}

void H264PreviewDecoder::feed(const uint8_t* annexb, size_t len, int64_t pts_us)
{
  if (!annexb || !len)
    return;
  if (g_start_failed_.load(std::memory_order_relaxed) ||
      !g_enabled_.load(std::memory_order_relaxed))
    return;
  if (!worker_.joinable())
    return;

  try
  {
    NaluPacket p;
    p.data.assign(annexb, annexb + len);
    p.pts_us = pts_us;

    {
      std::lock_guard<std::mutex> lk(q_mu_);
      q_.push_back(std::move(p));
      // Backpressure: if the decoder is hopelessly behind we'd rather
      // drop frames than balloon memory. Drops break inter-frame deps
      // until the next IDR — fine for a preview pane.
      while (q_.size() > kMaxQueue)
        q_.pop_front();
    }
  }
  catch (const std::bad_alloc&)
  {
    // Preview is optional. Under memory pressure drop this access unit rather
    // than unwind through the RTMP server and disconnect the publisher.
    return;
  }
  q_cv_.notify_one();
}

void H264PreviewDecoder::worker_loop_()
{
  while (true)
  {
    NaluPacket p;
    {
      std::unique_lock<std::mutex> lk(q_mu_);
      q_cv_.wait(lk, [this] { return stopping_.load() || !q_.empty(); });
      if (stopping_.load() && q_.empty())
        break;
      p = std::move(q_.front());
      q_.pop_front();
    }
    process_nalu_(p.data.data(), p.data.size(), p.pts_us);
  }
  teardown_();
}

void H264PreviewDecoder::process_nalu_(const uint8_t* annexb, size_t len, int64_t pts_us)
{
  if (len > MAXDWORD || !ensure_started_())
    return;

  using clock = std::chrono::steady_clock;
  const auto t_in = clock::now();

  ComPtr<IMFMediaBuffer> buf;
  if (FAILED(MFCreateMemoryBuffer((DWORD) len, buf.GetAddressOf())))
    return;
  BYTE* p = nullptr;
  DWORD max_len = 0;
  if (FAILED(buf->Lock(&p, &max_len, nullptr)))
    return;
  if (max_len < len)
  {
    buf->Unlock();
    return;
  }
  std::memcpy(p, annexb, len);
  buf->Unlock();
  buf->SetCurrentLength((DWORD) len);

  ComPtr<IMFSample> in;
  if (FAILED(MFCreateSample(in.GetAddressOf())))
    return;
  in->AddBuffer(buf.Get());
  in->SetSampleTime(pts_us * 10); // MF wants 100-ns units

  HRESULT hr = dec_->ProcessInput(0, in.Get(), 0);
  if (hr == MF_E_NOTACCEPTING)
  {
    drain_();
    hr = dec_->ProcessInput(0, in.Get(), 0);
  }
  drain_();

  // Periodic timing log so device-side debugging can spot whether the
  // decoder is the bottleneck. Reports the rolling average duration
  // of process+drain over the last 60 NALUs. >33 ms = sub-30 fps.
  const auto t_out = clock::now();
  const auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(t_out - t_in).count();
  static std::atomic<int> g_n{0};
  static std::atomic<int64_t> g_acc_us{0};
  int n = ++g_n;
  int64_t acc = g_acc_us += dur_us;
  if ((n % 60) == 0)
  {
    const double avg_ms = (double) acc / 60.0 / 1000.0;
    cr::log::info("preview", "decode timing: " + std::to_string(avg_ms) + " ms/NALU avg over 60 (last " + std::to_string((double) dur_us / 1000.0) + " ms)");
    g_acc_us.store(0);
  }
}

bool H264PreviewDecoder::ensure_started_()
{
  if (dec_)
    return true;
  if (start_failed_)
    return false;

  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
  {
    cr::log::warn("preview", "MFStartup failed");
    start_failed_ = true;
    g_start_failed_.store(true, std::memory_order_relaxed);
    return false;
  }
  mf_init_ = true;

  MFT_REGISTER_TYPE_INFO ininfo = {MFMediaType_Video, MFVideoFormat_H264};
  MFT_REGISTER_TYPE_INFO outinfo = {MFMediaType_Video, MFVideoFormat_NV12};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &ininfo, &outinfo, &activates, &count);
  if (FAILED(hr) || count == 0)
  {
    cr::log::warn("preview", "no H.264 decoder MFT available");
    if (activates)
      CoTaskMemFree(activates);
    start_failed_ = true;
    g_start_failed_.store(true, std::memory_order_relaxed);
    return false;
  }

  // Activate candidates in priority order. For each one: activate,
  // tag low-latency, try SetInputType. If any step fails, release and
  // try the next candidate. Without this fallback, a single decoder
  // that activates but rejects our input type would lock us out of
  // the rest of the list.
  for (UINT32 i = 0; i < count && !dec_; ++i)
  {
    if (FAILED(activates[i]->ActivateObject(IID_IMFTransform, (void**) &dec_)))
    {
      dec_ = nullptr;
      continue;
    }

    // MF_LOW_LATENCY (codecapi.h) tells H.264 decoders to skip the
    // reference-frame buffering that ordinarily delays output by
    // 1-2 frames waiting for B-frame reordering. For a live preview
    // this is exactly what we want — we trade reference-window
    // accuracy for sub-frame latency, and OBS streams typically use
    // a no-B-frame profile anyway. On software MFT it also disables
    // multi-threaded look-ahead which is the main reason 1080p
    // decode tanks to a few fps on average laptops.
    ComPtr<IMFAttributes> attr;
    if (SUCCEEDED(dec_->GetAttributes(attr.GetAddressOf())) && attr)
    {
      attr->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
      attr->SetUINT32(CODECAPI_AVDecNumWorkerThreads, 0);
    }

    ComPtr<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(in.GetAddressOf())) || FAILED(in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) || FAILED(in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) || FAILED(in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) || FAILED(dec_->SetInputType(0, in.Get(), 0)))
    {
      dec_->Release();
      dec_ = nullptr;
      continue;
    }

    if (!reconfigure_output_())
    {
      dec_->Release();
      dec_ = nullptr;
      continue;
    }
  }
  for (UINT32 i = 0; i < count; ++i)
    activates[i]->Release();
  CoTaskMemFree(activates);

  if (!dec_)
  {
    cr::log::warn("preview", "no usable H.264 decoder MFT (all candidates failed)");
    start_failed_ = true;
    g_start_failed_.store(true, std::memory_order_relaxed);
    return false;
  }

  dec_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  dec_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  cr::log::info("preview", "started (low_latency=on)");
  return true;
}

bool H264PreviewDecoder::reconfigure_output_()
{
  if (!dec_)
    return false;
  for (DWORD i = 0;; ++i)
  {
    ComPtr<IMFMediaType> ot;
    HRESULT hr = dec_->GetOutputAvailableType(0, i, ot.GetAddressOf());
    if (FAILED(hr))
      return false;
    GUID sub = {};
    ot->GetGUID(MF_MT_SUBTYPE, &sub);
    if (sub != MFVideoFormat_NV12)
      continue;
    if (FAILED(dec_->SetOutputType(0, ot.Get(), 0)))
      continue;

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, &w, &h);
    cur_w_ = (int) w;
    cur_h_ = (int) h;

    UINT32 sigstride = 0;
    LONG def_stride = 0;
    if (SUCCEEDED(ot->GetUINT32(MF_MT_DEFAULT_STRIDE, &sigstride)))
    {
      def_stride = (LONG) sigstride;
    }
    else if (w > 0)
    {
      MFGetStrideForBitmapInfoHeader(MFVideoFormat_NV12.Data1, w, &def_stride);
    }
    cur_stride_ = def_stride < 0 ? -def_stride : def_stride;
    if (cur_stride_ <= 0)
      cur_stride_ = cur_w_;
    return true;
  }
}

void H264PreviewDecoder::drain_()
{
  if (!dec_)
    return;

  while (true)
  {
    MFT_OUTPUT_STREAM_INFO si{};
    dec_->GetOutputStreamInfo(0, &si);

    const bool we_alloc = !(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));

    ComPtr<IMFSample> out_sample;
    ComPtr<IMFMediaBuffer> out_buf;
    if (we_alloc)
    {
      DWORD sz = si.cbSize;
      if (cur_w_ > 0 && cur_h_ > 0)
      {
        const std::uint64_t need = static_cast<std::uint64_t>(cur_w_) *
                                   static_cast<std::uint64_t>(cur_h_) * 3 / 2;
        if (need > MAXDWORD)
          return;
        if (sz < need)
          sz = static_cast<DWORD>(need);
      }
      if (sz < 4096)
        sz = 4096;
      if (FAILED(MFCreateSample(out_sample.GetAddressOf())))
        return;
      if (FAILED(MFCreateMemoryBuffer(sz, out_buf.GetAddressOf())))
        return;
      out_sample->AddBuffer(out_buf.Get());
    }

    MFT_OUTPUT_DATA_BUFFER db{};
    db.dwStreamID = 0;
    db.pSample = out_sample.Get();
    DWORD status = 0;
    HRESULT hr = dec_->ProcessOutput(0, 1, &db, &status);

    if (db.pEvents)
    {
      db.pEvents->Release();
      db.pEvents = nullptr;
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
      return;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
    {
      reconfigure_output_();
      continue;
    }
    if (FAILED(hr))
    {
      // Demoted to info so the noise doesn't reach the UI panel.
      cr::log::info("preview", "ProcessOutput hr=0x" + std::to_string((unsigned long) hr));
      return;
    }

    IMFSample* outs = db.pSample;
    if (!outs || cur_w_ <= 0 || cur_h_ <= 0)
    {
      if (db.pSample && db.pSample != out_sample.Get())
        db.pSample->Release();
      continue;
    }

    if (publish_ && g_enabled_.load(std::memory_order_relaxed))
    {
      ComPtr<IMFMediaBuffer> mbuf;
      if (SUCCEEDED(outs->ConvertToContiguousBuffer(mbuf.GetAddressOf())) && mbuf)
      {
        BYTE* src = nullptr;
        DWORD ml = 0, cl = 0;
        if (SUCCEEDED(mbuf->Lock(&src, &ml, &cl)))
        {
          const int stride = cur_stride_ > 0 ? cur_stride_ : cur_w_;
          const std::uint64_t y_bytes = static_cast<std::uint64_t>(stride) * cur_h_;
          const std::uint64_t uv_bytes = static_cast<std::uint64_t>(stride) *
                                         ((static_cast<std::uint64_t>(cur_h_) + 1) / 2);
          const std::uint64_t bgra_bytes = static_cast<std::uint64_t>(cur_w_) *
                                           cur_h_ * 4;

          // Do not trust MFT dimensions/stride blindly. A short sample or
          // malformed media type must never drive an out-of-bounds Y/UV read.
          const bool valid = (cur_w_ & 1) == 0 && (cur_h_ & 1) == 0 &&
                             stride >= cur_w_ && y_bytes + uv_bytes <= cl &&
                             bgra_bytes <= MAXDWORD && cur_w_ <= INT_MAX / 4;
          if (!valid)
          {
            mbuf->Unlock();
          }
          else
          {
            try
            {
              if (publish_ && g_enabled_.load(std::memory_order_relaxed))
              {
                bgra_.resize(static_cast<std::size_t>(bgra_bytes));
                const uint8_t* yp = src;
                const uint8_t* uvp = src + static_cast<std::size_t>(y_bytes);
                for (int y = 0; y < cur_h_; ++y)
                {
                  nv12_to_bgra_row(yp + static_cast<std::size_t>(y) * stride,
                                   uvp + static_cast<std::size_t>(y / 2) * stride,
                                   bgra_.data() + static_cast<std::size_t>(y) * cur_w_ * 4,
                                   cur_w_);
                }
                publish_(bgra_.data(), cur_w_, cur_h_, cur_w_ * 4);
              }
            }
            catch (...)
            {
              mbuf->Unlock();
              if (db.pSample && db.pSample != out_sample.Get())
                db.pSample->Release();
              throw;
            }
            mbuf->Unlock();
          }
        }
      }
    }

    if (db.pSample && db.pSample != out_sample.Get())
      db.pSample->Release();
  }
}

void H264PreviewDecoder::teardown_()
{
  if (dec_)
  {
    dec_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    dec_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    dec_->Release();
    dec_ = nullptr;
  }
  if (mf_init_)
  {
    MFShutdown();
    mf_init_ = false;
  }
  cur_w_ = cur_h_ = cur_stride_ = 0;
  start_failed_ = false;
  g_start_failed_.store(false, std::memory_order_relaxed);
  bgra_.clear();
  {
    std::lock_guard<std::mutex> lk(q_mu_);
    q_.clear();
  }
}

} // namespace cr::render
