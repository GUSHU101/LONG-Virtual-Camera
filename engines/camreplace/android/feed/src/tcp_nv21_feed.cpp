/*
 * Назначение файла:
 *   TCP raw-NV21 receiver для Raw transport. Host уже декодировал OBS video
 *   в NV21 и присылает CRTP header + continuous frame stream; phone-side
 *   процесс только публикует кадры в /data/cr/feed.
 *
 * ABI/совместимость:
 *   The shared ring keeps separate FHD and UHD slot regions.
 *   Input above the ring's pixel budget is rejected before copying frames.
 *
 * Pixel/Android ограничения:
 *   Raw mode требует высокой USB пропускной способности. На USB 2.0 1080p30
 *   практически не помещается, поэтому receiver обязан логировать cap/shape
 *   ошибки понятно, а не молча десинхронизировать stream.
 */

// TCP raw-NV21 feed mode (Transport::Raw on the host side).
// Wire format (the "CRTP" protocol that TcpFeedClient::send_header /
// send_frame on the host emits):
//   header (16 bytes, LE):
//     uint32 magic  = 'CRTP'
//     uint32 width
//     uint32 height
//     uint32 fps
//   then per frame:
//     width*height*3/2 bytes of NV21 (Y plane + interleaved VU plane)
// No per-frame framing — the constant frame size from the header is
// the only delimiter we have, so a single byte missed on the wire would
// permanently desync. That's tolerable for a localhost adb-forward
// loopback (TCP is reliable, no MITM); we'd switch to a framed protocol
// if we ever moved this off-device.
// Skips the AMediaCodec OMX hop entirely — the host is already shipping
// NV21, so we just receive into the next shm slot and bump write_index.
// This is what makes Transport::Raw worth it: ~80 ms of phone-side
// decode latency disappear, plus DCT artefacts and 4:2:0 chroma loss.
// Bandwidth cost: 1080p30 NV21 ≈ 750 Mbps — does not fit USB 2.0
// (~280 Mbps practical). The host gates this with the device's
// negotiated USB speed and warns the user when in doubt.

#include "../include/cr_feed.h"
#include "shm.h"
#include "secure_channel_reader.h"
#include "interruptible_socket.h"

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
#include <vector>
#include "util/Obf.h"
#define TAG "cr_feed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern std::atomic<bool> g_stop; // shared with feed_main.cpp

namespace
{

constexpr uint32_t kMagic = 0x50545243u; // 'CRTP' LE — raw NV21 stream

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

// Same shm writer contract as tcp_h264_feed.cpp — kept private here so
// future divergence (different magic, format=NV21 vs new) doesn't leak
// into the H.264 path.
struct ShmOut
{
  int fd = -1;
  void* map = nullptr;
  size_t mapped = 0;
  int w = 0;
  int h = 0;
  size_t slot_bytes = 0;
  uint32_t cur_wi = 0;
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
      LOGE(OBF("nv21 shm init rejected %dx%d: %zu > %u").c_str(), ww, hh, slot_bytes, CR_FEED_MAX_NV21_BYTES);
      return false;
    }
    const size_t reserve = CR_FEED_MAP_RESERVE;

    fd = open(OBF("/data/cr/feed").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
      return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
      LOGE(OBF("nv21 shm writer lease is already owned: %s").c_str(), strerror(errno));
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
      // Preserve existing mapping size — see tcp_h264_feed.cpp for why
      // (camhook may already mmap a previous, larger reservation).
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

  // Receive into a writer-owned slot. Camera callbacks pin their slot, so a
  // slow JPEG encode cannot be overwritten by a later network frame.
  bool recv_into_next_slot(cr::feed::SecureChannelReader& channel)
  {
    if (!map)
      return false;
    auto* hd = (cr_feed_header*) map;
    uint32_t next = 0;
    if (!cr_feed_claim_write(hd, cur_wi, &next))
      return true;
    auto* dst = static_cast<uint8_t*>(map) + cr_feed_slot_offset(slot_bytes, next);
    if (!channel.read_exact(dst, slot_bytes))
    {
      cr_feed_abort_write(hd, next);
      return false;
    }
    cr_feed_publish(hd, next);
    atomic_store_explicit(&hd->channel_state, CR_CHANNEL_STATE_READY,
                          memory_order_release);
    if (!ready_logged)
    {
      LOGI(OBF("nv21 shm READY transition on first publish (%dx%d slot=%zu)").c_str(), w, h, slot_bytes);
      ready_logged = true;
    }
    cur_wi = next;
    return true;
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
        LOGI(OBF("nv21 shm channel_state %u -> NONE teardown=%s").c_str(), prev, reason ? reason : OBF("unknown").c_str());
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

int serve_client(int client)
{
  int nodelay = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  cr::feed::SecureChannelReader channel;
  if (!channel.handshake(client, cr::secure_channel::v2::StreamKind::Video))
  {
    LOGW(OBF("nv21: secure-channel handshake rejected").c_str());
    return -1;
  }

  // --- Header ----------------------------------------------------------
  uint32_t hdr[4] = {};
  if (!channel.read_exact(hdr, sizeof(hdr)))
  {
    LOGW(OBF("nv21: short header").c_str());
    return -1;
  }
  if (hdr[0] != kMagic)
  {
    LOGE(OBF("nv21: bad magic 0x%x (expected 'CRTP')").c_str(), hdr[0]);
    return -1;
  }
  const int w = (int) hdr[1];
  const int h = (int) hdr[2];
  const int fps = (int) hdr[3];
  if (!nv21_shape_supported(w, h, OBF("nv21").c_str()))
    return -1;
  LOGI(OBF("nv21 header: %dx%d %dfps frame=%zu B").c_str(), w, h, fps, (size_t) w * h * 3 / 2);

  ShmOut shm;
  if (!shm.init(w, h))
    return -1;

  // Stall diagnostics — same shape as tcp_h264_feed.cpp so the timeline
  // log compares apples to apples between the two transports.
  auto now_ms = []() -> int64_t
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1'000'000;
  };
  int64_t last_ms = now_ms();
  uint64_t frames = 0;

  while (!g_stop.load(std::memory_order_relaxed))
  {
    if (!shm.recv_into_next_slot(channel))
      break;

    const int64_t pub = now_ms();
    if (frames > 0 && (pub - last_ms) > 500)
    {
      LOGW(OBF("nv21 STALL on wire: %lldms since last frame (likely OBS / USB / adb-forward backpressure)").c_str(), (long long) (pub - last_ms));
    }
    last_ms = pub;
    ++frames;

    if (frames <= 3 || (frames % 120) == 0)
    {
      LOGI(OBF("nv21: published frame #%llu").c_str(), (unsigned long long) frames);
    }
  }

  LOGI(OBF("nv21: client gone (frames=%llu)").c_str(), (unsigned long long) frames);
  shm.shutdown(OBF("client gone").c_str());
  return 0;
}

} // namespace

extern "C" int cr_feed_run_tcp_nv21(int port);

int cr_feed_run_tcp_nv21(int port)
{
  int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (srv < 0)
  {
    LOGE(OBF("nv21: socket: %s").c_str(), strerror(errno));
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
    LOGE(OBF("nv21: bind :%d: %s").c_str(), port, strerror(errno));
    close(srv);
    return -1;
  }
  if (listen(srv, 1) != 0)
  {
    LOGE(OBF("nv21: listen: %s").c_str(), strerror(errno));
    close(srv);
    return -1;
  }
  LOGI(OBF("tcp_nv21 feed listening on 127.0.0.1:%d").c_str(), port);
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
      LOGW(OBF("nv21: accept: %s").c_str(), strerror(errno));
      break;
    }
    LOGI(OBF("nv21: client connected").c_str());
    serve_client(client);
    close(client);
  }

  close(srv);
  return 0;
}
