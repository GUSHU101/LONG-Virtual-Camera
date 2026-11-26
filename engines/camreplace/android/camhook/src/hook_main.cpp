

#include "cr_feed.h"
#include "shm.h"
#include "nv21_blit.h"
#include "jpeg_encode.h"
#include "HookDispatch.h"
#include "Raw10Bayer.h"
#include "shadowhook.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <array>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>
#include "util/Obf.h"

/*
 * Installs the cameraserver-side camera-frame replacement hook.
 *
 * cr_camhook_init maps the feed shared-memory file, selects the configured
 * NV21 scaler, and installs a
 * ShadowHook proxy for a supported Camera3OutputStream queue method. The
 * proxy validates the mapped feed header, locks the destination buffer,
 * scales the latest committed NV21 frame into it, and calls the original
 * queue method. Multiple mangled symbol and buffer-mapper layouts are tried
 * to cover Android platform variants without runtime SDK branching.
 *
 * Photo mode also handles JPEG BLOBs and reviewed RAW10 layouts. Shared memory is
 * owned by the feed process; this library maps it read-only and never changes
 * its producer state. Global hook and mapper state is process-local to
 * cameraserver. Invalid headers, unavailable CPU mappings, unsupported
 * layouts, or absent frames fall through to the original camera pipeline.
 */

#define TAG "cr_camhook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct PinnedSlot
{
  cr_feed_header* header = nullptr;
  const uint8_t* data = nullptr;
  uint32_t index = 0;
  uint32_t generation = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t slot_size = 0;
  uint64_t frame_seq = 0;

  ~PinnedSlot()
  {
    if (header)
      cr_feed_slot_release_read(header, index);
  }
  PinnedSlot() = default;
  PinnedSlot(const PinnedSlot&) = delete;
  PinnedSlot& operator=(const PinnedSlot&) = delete;
};

struct ShmView
{
  cr_feed_header* header = nullptr;
  const uint8_t* slot0 = nullptr;
  size_t size = 0;
  int fd = -1;

  bool pin_current(PinnedSlot& out) const noexcept
  {
    if (!header || !slot0)
      return false;

    const uint32_t magic = __atomic_load_n(&header->magic, __ATOMIC_ACQUIRE);
    const uint32_t version = __atomic_load_n(&header->version, __ATOMIC_ACQUIRE);
    const uint32_t generation = atomic_load_explicit(&header->generation, memory_order_acquire);
    const uint32_t raw_n = __atomic_load_n(&header->num_slots, __ATOMIC_ACQUIRE);
    if (raw_n == 0 || raw_n > CR_FEED_NUM_SLOTS)
      return false;
    const uint32_t n = raw_n;
    const uint32_t width = __atomic_load_n(&header->width, __ATOMIC_ACQUIRE);
    const uint32_t height = __atomic_load_n(&header->height, __ATOMIC_ACQUIRE);
    const uint32_t stride = __atomic_load_n(&header->stride, __ATOMIC_ACQUIRE);
    const uint32_t slot_size = __atomic_load_n(&header->slot_size, __ATOMIC_ACQUIRE);
    const uint32_t format = __atomic_load_n(&header->format, __ATOMIC_ACQUIRE);
    const uint64_t frame_seq = atomic_load_explicit(&header->frame_counter, memory_order_acquire);
    const uint32_t wi = atomic_load_explicit(&header->write_index, memory_order_acquire) % n;
    if (magic != CR_FEED_MAGIC || version != CR_FEED_SHM_VERSION || frame_seq == 0)
      return false;
    if (!cr_feed_slot_try_read(header, wi))
      return false;

    // Pin first, then prove the publication and geometry snapshot did not
    // change under us. Later generations may start while this guard is alive,
    // but preserving slot_state prevents them from reusing this slot.
    const bool stable = __atomic_load_n(&header->magic, __ATOMIC_ACQUIRE) == magic &&
                        __atomic_load_n(&header->version, __ATOMIC_ACQUIRE) == version &&
                        atomic_load_explicit(&header->generation, memory_order_acquire) == generation &&
                        atomic_load_explicit(&header->write_index, memory_order_acquire) % n == wi &&
                        atomic_load_explicit(&header->frame_counter, memory_order_acquire) == frame_seq &&
                        __atomic_load_n(&header->width, __ATOMIC_ACQUIRE) == width &&
                        __atomic_load_n(&header->height, __ATOMIC_ACQUIRE) == height &&
                        __atomic_load_n(&header->stride, __ATOMIC_ACQUIRE) == stride &&
                        __atomic_load_n(&header->slot_size, __ATOMIC_ACQUIRE) == slot_size &&
                        __atomic_load_n(&header->num_slots, __ATOMIC_ACQUIRE) == n &&
                        __atomic_load_n(&header->format, __ATOMIC_ACQUIRE) == format;
    if (!stable)
    {
      cr_feed_slot_release_read(header, wi);
      return false;
    }

    const size_t off = (size_t) wi * CR_FEED_MAX_NV21_BYTES;
    const size_t hdr = sizeof(cr_feed_header);
    const uint64_t min_slot =
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 3ull /
        2ull;
    if (width == 0 || height == 0 || (width & 1u) || (height & 1u) ||
        width > CR_FEED_MAX_WIDTH || height > CR_FEED_MAX_HEIGHT ||
        stride != width || format != 0 || slot_size == 0 ||
        min_slot == 0 || min_slot > slot_size ||
        slot_size > CR_FEED_MAX_NV21_BYTES || size < hdr || off > (size - hdr) ||
        slot_size > (size - hdr - off))
    {
      cr_feed_slot_release_read(header, wi);
      return false;
    }
    out.header = header;
    out.index = wi;
    out.data = slot0 + off;
    out.generation = generation;
    out.width = width;
    out.height = height;
    out.slot_size = slot_size;
    out.frame_seq = frame_seq;
    return true;
  }
};

static std::atomic<bool> g_installed{false};
static ShmView g_shm;
static std::mutex g_shm_mutex;
static std::atomic<bool> g_shm_ready{false};
static void* g_hook_stub = nullptr;

static bool feed_header_sane(const cr_feed_header* h, size_t map_size)
{
  if (!h || map_size < sizeof(cr_feed_header))
    return false;
  const uint32_t magic = __atomic_load_n(&h->magic, __ATOMIC_ACQUIRE);
  const uint32_t version = __atomic_load_n(&h->version, __ATOMIC_ACQUIRE);
  const uint32_t width = __atomic_load_n(&h->width, __ATOMIC_ACQUIRE);
  const uint32_t height = __atomic_load_n(&h->height, __ATOMIC_ACQUIRE);
  const uint32_t stride = __atomic_load_n(&h->stride, __ATOMIC_ACQUIRE);
  const uint32_t slot_size = __atomic_load_n(&h->slot_size, __ATOMIC_ACQUIRE);
  const uint32_t n = __atomic_load_n(&h->num_slots, __ATOMIC_ACQUIRE);
  const uint32_t format = __atomic_load_n(&h->format, __ATOMIC_ACQUIRE);
  if (magic != CR_FEED_MAGIC)
    return false;
  if (version != CR_FEED_SHM_VERSION)
    return false;
  if (width == 0 || height == 0 || (width & 1u) || (height & 1u) ||
      width > CR_FEED_MAX_WIDTH || height > CR_FEED_MAX_HEIGHT ||
      stride != width || format != 0)
  {
    return false;
  }
  if (slot_size == 0 || slot_size > CR_FEED_MAX_NV21_BYTES)
    return false;
  const uint64_t min_slot = (uint64_t) width * (uint64_t) height * 3ull / 2ull;
  if (min_slot == 0 || min_slot > slot_size)
    return false;

  if (n == 0 || n > CR_FEED_NUM_SLOTS)
    return false;

  uint64_t required = sizeof(cr_feed_header);
  if (n <= 1)
  {
    required += (uint64_t) slot_size;
  }
  else
  {
    required += (uint64_t) n * (uint64_t) CR_FEED_MAX_NV21_BYTES;
  }
  return required <= map_size &&
         __atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) == magic &&
         __atomic_load_n(&h->version, __ATOMIC_ACQUIRE) == version &&
         __atomic_load_n(&h->width, __ATOMIC_ACQUIRE) == width &&
         __atomic_load_n(&h->height, __ATOMIC_ACQUIRE) == height &&
         __atomic_load_n(&h->stride, __ATOMIC_ACQUIRE) == stride &&
         __atomic_load_n(&h->slot_size, __ATOMIC_ACQUIRE) == slot_size &&
         __atomic_load_n(&h->num_slots, __ATOMIC_ACQUIRE) == n &&
         __atomic_load_n(&h->format, __ATOMIC_ACQUIRE) == format;
}

typedef void (*cr_blit_fn)(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride, int dst_uv_swapped);
static cr_blit_fn g_blit_fn = cr_nv21_blit;
enum class BlitKind
{
  Bilinear,
  Lanczos2,
};
static BlitKind g_blit_kind = BlitKind::Bilinear;

static const char* blit_name()
{
  return g_blit_kind == BlitKind::Lanczos2 ? "lanczos2" : "bilinear";
}

static void resolve_scaler()
{
  g_blit_fn = cr_nv21_blit;
  g_blit_kind = BlitKind::Bilinear;

  int fd = open(OBF("/data/cr/scaler").c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    LOGI(OBF("scaler = bilinear (default; no /data/cr/scaler)").c_str());
    return;
  }
  char buf[32] = {};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
  {
    LOGI(OBF("scaler = bilinear (default; empty /data/cr/scaler)").c_str());
    return;
  }
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
  {
    buf[--n] = 0;
  }
  if (strcmp(buf, OBF("lanczos").c_str()) == 0 || strcmp(buf, OBF("lanczos2").c_str()) == 0)
  {
    g_blit_fn = cr_nv21_blit_lanczos;
    g_blit_kind = BlitKind::Lanczos2;
  }
  LOGI(OBF("scaler = %s (/data/cr/scaler=\"%s\")").c_str(), blit_name(), buf);
}

static inline void blit_adaptive(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride, int dst_uv_swapped)
{
  if (g_blit_fn != cr_nv21_blit && (int64_t) dst_w * (int64_t) dst_h > 1920LL * 1080LL)
  {
    cr_nv21_blit(src, src_w, src_h, dst_y, dst_vu, dst_w, dst_h, dst_y_stride, dst_uv_stride, dst_uv_swapped);
  }
  else
  {
    g_blit_fn(src, src_w, src_h, dst_y, dst_vu, dst_w, dst_h, dst_y_stride, dst_uv_stride, dst_uv_swapped);
  }
}

static inline int64_t monotonic_ns()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000000000LL + (int64_t) ts.tv_nsec;
}

// ReCam rejects a publication once it has stopped advancing. Preserve that
// fail-open privacy/safety property without changing CamReplace's compact shm
// ABI: cameraserver tracks progress locally and falls back to the real sensor
// if a connected-but-stalled feeder leaves READY on an old frame.
static constexpr int64_t kFrameStaleNs = 2LL * 1000LL * 1000LL * 1000LL;
static std::mutex g_frame_freshness_mu;
static uint32_t g_fresh_generation = 0;
static uint64_t g_fresh_sequence = 0;
static int64_t g_fresh_progress_ns = 0;

static bool frame_is_fresh(const PinnedSlot& frame)
{
  const int64_t now = monotonic_ns();
  std::lock_guard<std::mutex> lock(g_frame_freshness_mu);
  if (frame.generation != g_fresh_generation ||
      frame.frame_seq != g_fresh_sequence || g_fresh_progress_ns <= 0 ||
      now < g_fresh_progress_ns)
  {
    g_fresh_generation = frame.generation;
    g_fresh_sequence = frame.frame_seq;
    g_fresh_progress_ns = now;
    return true;
  }
  return now - g_fresh_progress_ns <= kFrameStaleNs;
}

static std::atomic<bool> g_photo_mode{false};

static constexpr int64_t kPhotoFlagPollNs = 500LL * 1000LL * 1000LL;
static std::atomic<int64_t> g_photo_flag_last_check_ns{0};

static inline bool read_photo_flag_file_()
{
  int fd = open(OBF("/data/cr/photo_mode").c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  char buf[16] = {};
  read(fd, buf, sizeof(buf) - 1);
  close(fd);
  return (buf[0] != 0 && buf[0] != '0');
}

static bool refresh_photo_mode_()
{
  const int64_t now_ns = monotonic_ns();
  const int64_t last = g_photo_flag_last_check_ns.load(std::memory_order_relaxed);
  if (now_ns - last < kPhotoFlagPollNs)
  {
    return g_photo_mode.load(std::memory_order_relaxed);
  }
  g_photo_flag_last_check_ns.store(now_ns, std::memory_order_relaxed);

  const bool on = read_photo_flag_file_();
  const bool prev = g_photo_mode.exchange(on, std::memory_order_relaxed);
  if (prev != on)
  {
    LOGI(OBF("photo mode -> %s (file flag %s)").c_str(), on ? "ON" : OBF("OFF").c_str(), on ? OBF("appeared").c_str() : OBF("removed").c_str());
  }
  return on;
}

static void resolve_photo_mode()
{
  g_photo_mode.store(false);

  int fd = open(OBF("/data/cr/photo_mode").c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    LOGI(OBF("photo mode = OFF (no /data/cr/photo_mode)").c_str());
    return;
  }
  char buf[16] = {};
  read(fd, buf, sizeof(buf) - 1);
  close(fd);

  bool on = (buf[0] != 0 && buf[0] != '0');
  if (on)
  {
    g_photo_mode.store(true);
    LOGI(OBF("photo mode = ON — BLOB capture will be JPEG-encoded from the latest /data/cr/feed slot").c_str());
  }
  else
  {
    LOGI(OBF("photo mode = OFF (/data/cr/photo_mode=\"%s\")").c_str(), buf);
  }
}

constexpr uint64_t kAhbUsageCpuRW =
    AHARDWAREBUFFER_USAGE_CPU_READ_RARELY | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

// Resolve these APIs at runtime so older Android builds can use the mapper.
using AHB_lockPlanes_fn = int (*)(AHardwareBuffer*, uint64_t, int32_t, const ARect*,
                                  AHardwareBuffer_Planes*);
using AHB_unlock_fn = int (*)(AHardwareBuffer*, int32_t*);
using AHB_lock_fn = int (*)(AHardwareBuffer*, uint64_t, int32_t, const ARect*, void**);
using AHB_describe_fn = void (*)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
typedef void* (*ANWB_getHardwareBuffer_fn)(void* buffer);

static AHB_lockPlanes_fn g_ahb_lockPlanes = nullptr;
static AHB_unlock_fn g_ahb_unlock = nullptr;
static AHB_lock_fn g_ahb_lock = nullptr;
static AHB_describe_fn g_ahb_describe = nullptr;
static ANWB_getHardwareBuffer_fn g_get_hardware_buffer = nullptr;

struct Rect
{
  int32_t left, top, right, bottom;
};

struct android_ycbcr
{
  void* y;
  void* cb;
  void* cr;
  size_t ystride;
  size_t cstride;
  size_t chroma_step;
  uint32_t reserved[8];
};

using GBM_get_fn = void* (*) ();
using GBM_lockYCbCr_fn = int (*)(void* self, const void* handle, uint32_t usage, const Rect& bounds, android_ycbcr* out);
using GBM_unlock_v1_fn = int (*)(void* self, const void* handle);
using GBM_unlock_v2_fn = int (*)(void* self, const void* handle, void* outFence);

using GBM_lock_v1_fn = int (*)(void* self, const void* handle, uint32_t usage, const Rect& bounds, void** outVaddr);
using GBM_lock_v2_fn = int (*)(void* self, const void* handle, uint32_t usage, const Rect& bounds, void** outVaddr, int32_t* outBytesPerPixel, int32_t* outBytesPerStride);

static void* g_gbm = nullptr;
static GBM_lockYCbCr_fn g_gbm_lockYCbCr = nullptr;
static GBM_unlock_v1_fn g_gbm_unlock_v1 = nullptr;
static GBM_unlock_v2_fn g_gbm_unlock_v2 = nullptr;
static GBM_lock_v1_fn g_gbm_lock_v1 = nullptr;
static GBM_lock_v2_fn g_gbm_lock_v2 = nullptr;
static std::shared_mutex g_runtime_ops_mu;
static std::atomic<bool> g_runtime_preview_ready{false};

static int gbm_call_unlock(const void* handle)
{
  if (g_gbm_unlock_v1)
    return g_gbm_unlock_v1(g_gbm, handle);
  if (g_gbm_unlock_v2)
    return g_gbm_unlock_v2(g_gbm, handle, nullptr);
  return -1;
}

static bool gbm_ready()
{
  return g_gbm && g_gbm_lockYCbCr && (g_gbm_unlock_v1 || g_gbm_unlock_v2);
}

static bool ahb_planes_ready()
{
  return g_ahb_lockPlanes && g_ahb_unlock && g_get_hardware_buffer;
}

static int open_shm()
{
  std::lock_guard<std::mutex> guard(g_shm_mutex);
  if (g_shm_ready.load(std::memory_order_acquire))
    return 0;
  int fd = open(OBF("/data/cr/feed").c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0)
  {
    LOGW(OBF("open %s: %s").c_str(), OBF("/data/cr/feed").c_str(), strerror(errno));
    return -1;
  }
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size <= 0 || (size_t) st.st_size < sizeof(cr_feed_header))
  {
    LOGW(OBF("fstat/size bad: size=%lld").c_str(), (long long) st.st_size);
    close(fd);
    return -1;
  }
  void* map = mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED)
  {
    LOGW(OBF("mmap %s: %s").c_str(), OBF("/data/cr/feed").c_str(), strerror(errno));
    close(fd);
    return -1;
  }
  auto* h = (cr_feed_header*) map;
  if (!feed_header_sane(h, (size_t) st.st_size))
  {
    LOGW(OBF("bad feed shape: %ux%u slot=%u slots=%u").c_str(),
         __atomic_load_n(&h->width, __ATOMIC_ACQUIRE),
         __atomic_load_n(&h->height, __ATOMIC_ACQUIRE),
         __atomic_load_n(&h->slot_size, __ATOMIC_ACQUIRE),
         __atomic_load_n(&h->num_slots, __ATOMIC_ACQUIRE));
    munmap(map, st.st_size);
    close(fd);
    return -1;
  }
  g_shm.header = h;
  g_shm.slot0 = (const uint8_t*) map + sizeof(cr_feed_header);
  g_shm.size = (size_t) st.st_size;
  g_shm.fd = fd;
  g_shm_ready.store(true, std::memory_order_release);
  LOGI(OBF("shm mapped: %ux%u (%zu bytes)").c_str(),
       __atomic_load_n(&h->width, __ATOMIC_ACQUIRE),
       __atomic_load_n(&h->height, __ATOMIC_ACQUIRE), g_shm.size);
  return 0;
}

template <typename Fn> static Fn resolve_sym(void* dl_handle, void* sh_handle, const char* sym)
{
  Fn fn = (Fn) dlsym(dl_handle, sym);
  if (!fn && sh_handle)
  {
    fn = (Fn) shadowhook_dlsym(sh_handle, sym);
  }
  return fn;
}

static bool resolve_gbm_in_lib(void* dl_handle, void* sh_handle, const char* lib_tag)
{
  if (!dl_handle && !sh_handle)
    return false;

  auto gbm_get = resolve_sym<GBM_get_fn>(dl_handle, sh_handle, "_ZN7android9SingletonINS_19GraphicBufferMapperEE11getInstanceEv");
  auto lockYCbCr = resolve_sym<GBM_lockYCbCr_fn>(dl_handle, sh_handle, "_ZN7android19GraphicBufferMapper9lockYCbCrEPK13native_handlejRKNS_4RectEP13android_ycbcr");
  auto unlock_v1 = resolve_sym<GBM_unlock_v1_fn>(dl_handle, sh_handle, OBF("_ZN7android19GraphicBufferMapper6unlockEPK13native_handle").c_str());
  auto unlock_v2 = resolve_sym<GBM_unlock_v2_fn>(dl_handle, sh_handle, "_ZN7android19GraphicBufferMapper6unlockEPK13native_handlePNS_4base9unique_fdE");

  auto plain_lock_v1 = resolve_sym<GBM_lock_v1_fn>(dl_handle, sh_handle, "_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPv");

  GBM_lock_v2_fn plain_lock_v2 = nullptr;
  const char* lock_v2_match = nullptr;
  char lock_v2_sym_buf[256] = {};
  {
    static const char* const kV2Suffixes[] = {
        "S5_", "S6_", "S7_", "S8_", "S9_", "SA_", "SB_", "SC_",
    };
    for (const char* suf : kV2Suffixes)
    {
      char sym[256];
      int n = snprintf(sym, sizeof(sym), "_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPi%s", suf);
      if (n <= 0 || (size_t) n >= sizeof(sym))
        continue;
      plain_lock_v2 = resolve_sym<GBM_lock_v2_fn>(dl_handle, sh_handle, sym);
      if (plain_lock_v2)
      {
        strncpy(lock_v2_sym_buf, sym, sizeof(lock_v2_sym_buf) - 1);
        lock_v2_match = lock_v2_sym_buf;
        break;
      }
    }
  }

  void* gbm_inst = gbm_get ? gbm_get() : nullptr;

  if (gbm_inst && lockYCbCr && (unlock_v1 || unlock_v2))
  {
    g_gbm = gbm_inst;
    g_gbm_lockYCbCr = lockYCbCr;
    g_gbm_unlock_v1 = unlock_v1;
    g_gbm_unlock_v2 = unlock_v2;
    g_gbm_lock_v1 = plain_lock_v1;
    g_gbm_lock_v2 = plain_lock_v2;
    LOGI(OBF("resolve: GBM picked from %s (this=%p)").c_str(), lib_tag, gbm_inst);
    LOGI(OBF("resolve: GBM plain lock v1=%p v2=%p%s%s").c_str(), (void*) plain_lock_v1, (void*) plain_lock_v2, lock_v2_match ? OBF(" v2_sym=").c_str() : "", lock_v2_match ? lock_v2_match : "");
    return true;
  }
  LOGW(OBF("resolve: GBM partial in %s — getInst=%p lockYCbCr=%p u_v1=%p u_v2=%p").c_str(), lib_tag, (void*) gbm_get, (void*) lockYCbCr, (void*) unlock_v1, (void*) unlock_v2);
  return false;
}

static void resolve_runtime_ops_unlocked()
{
  void* libnw = dlopen(OBF("libnativewindow.so").c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (!libnw)
    libnw = dlopen(OBF("libnativewindow.so").c_str(), RTLD_NOW);
  if (libnw)
  {
    g_ahb_lockPlanes = (AHB_lockPlanes_fn) dlsym(libnw, OBF("AHardwareBuffer_lockPlanes").c_str());
    g_ahb_unlock = (AHB_unlock_fn) dlsym(libnw, OBF("AHardwareBuffer_unlock").c_str());

    g_ahb_lock = (AHB_lock_fn) dlsym(libnw, OBF("AHardwareBuffer_lock").c_str());
    g_ahb_describe = (AHB_describe_fn) dlsym(libnw, "AHardwareBuffer_describe");
    g_get_hardware_buffer = (ANWB_getHardwareBuffer_fn) dlsym(libnw, OBF("ANativeWindowBuffer_getHardwareBuffer").c_str());
  }

  static const char* const kGbmLibs[] = {
      "libui.so",
      "libgui.so",
  };
  for (auto* lib_name : kGbmLibs)
  {
    if (g_gbm)
      break;
    void* dl_handle = dlopen(lib_name, RTLD_NOW | RTLD_NOLOAD);
    if (!dl_handle)
      dl_handle = dlopen(lib_name, RTLD_NOW);

    void* sh_handle = shadowhook_dlopen(lib_name);
    if (!dl_handle && !sh_handle)
    {
      LOGW(OBF("resolve: %s not loadable (no dl + no sh handle)").c_str(), lib_name);
      continue;
    }
    resolve_gbm_in_lib(dl_handle, sh_handle, lib_name);
    if (sh_handle)
      shadowhook_dlclose(sh_handle);
  }

  LOGI(OBF("resolve: AHB lock=%p unlock=%p | GBM this=%p lockYCbCr=%p unlock_v1=%p unlock_v2=%p").c_str(), g_ahb_lockPlanes, g_ahb_unlock, g_gbm, g_gbm_lockYCbCr, g_gbm_unlock_v1, g_gbm_unlock_v2);
}

static void resolve_runtime_ops()
{
  if (g_runtime_preview_ready.load(std::memory_order_acquire))
    return;
  static std::atomic<std::uint32_t> attempts{0};
  if ((attempts.fetch_add(1, std::memory_order_relaxed) % 30) != 0)
    return;

  std::unique_lock<std::shared_mutex> lock(g_runtime_ops_mu);
  if (ahb_planes_ready() || gbm_ready())
  {
    g_runtime_preview_ready.store(true, std::memory_order_release);
    return;
  }
  resolve_runtime_ops_unlocked();
  if (ahb_planes_ready() || gbm_ready())
    g_runtime_preview_ready.store(true, std::memory_order_release);
}

struct ANativeBaseHeader
{
  int32_t magic;
  int32_t version;
  void* reserved[4];
  void* incRef;
  void* decRef;
};

struct ANWB
{
  ANativeBaseHeader common;
  int32_t width;
  int32_t height;
  int32_t stride;
  int32_t format;
  int32_t usage_deprecated;
  int32_t _pad;
  uintptr_t layerCount;
  void* reserved_ptr[1];
  const void* handle;
  uint64_t usage;
};

static_assert(sizeof(ANativeBaseHeader) == 56, "ANB common" /*OBF_SKIP*/);
static_assert(offsetof(ANWB, width) == 56, "ANWB width" /*OBF_SKIP*/);
static_assert(offsetof(ANWB, height) == 60, "ANWB height" /*OBF_SKIP*/);
static_assert(offsetof(ANWB, stride) == 64, "ANWB stride" /*OBF_SKIP*/);
static_assert(offsetof(ANWB, format) == 68, "ANWB format" /*OBF_SKIP*/);
static_assert(offsetof(ANWB, handle) == 96, "ANWB handle" /*OBF_SKIP*/);

constexpr int32_t kAndroidNativeBufferMagic = ((int32_t) '_' << 24) | ((int32_t) 'b' << 16) | ((int32_t) 'f' << 8) | (int32_t) 'r';

struct ANWBInfo
{
  int32_t width = 0;
  int32_t height = 0;
  int32_t stride = 0;
  int32_t format = 0;
  const void* handle = nullptr;
};

static std::atomic<int> g_stat_skip_noshm{0};
static std::atomic<int> g_stat_skip_channel{0};
static std::atomic<int> g_stat_skip_noframe{0};
static std::atomic<int> g_stat_skip_stale{0};
static std::atomic<int> g_stat_skip_layout{0};
static std::atomic<int> g_stat_skip_nogbm{0};
static std::atomic<int> g_stat_skip_noanb{0};
static std::atomic<int> g_stat_skip_dims{0};
static std::atomic<int> g_stat_skip_fmt{0};
static std::atomic<int> g_stat_skip_raw10{0};
static std::atomic<int> g_stat_skip_handle{0};
static std::atomic<int> g_stat_lock_fail{0};
static std::atomic<int> g_stat_blit_ok{0};

static bool inspect_anwb(const ANWB* anb, ANWBInfo& out)
{
  if (!anb)
  {
    ++g_stat_skip_noanb;
    return false;
  }
  const int32_t version = anb->common.version;
  if (anb->common.magic != kAndroidNativeBufferMagic || version < (int32_t) (offsetof(ANWB, handle) + sizeof(anb->handle)) || version > 4096)
  {
    int n = ++g_stat_skip_layout;
    if (n <= 5 || (n % 60) == 0)
    {
      LOGW(OBF("ANWB layout rejected magic=0x%x version=%d anb=%p").c_str(), anb->common.magic, version, anb);
    }
    return false;
  }

  out.width = anb->width;
  out.height = anb->height;
  out.stride = anb->stride;
  out.format = anb->format;
  out.handle = anb->handle;
  if (out.width < 1 || out.height < 1)
  {
    ++g_stat_skip_dims;
    return false;
  }
  if (!out.handle)
  {
    ++g_stat_skip_handle;
    return false;
  }
  return true;
}

static void try_map_feed()
{
  if (!g_shm_ready.load(std::memory_order_acquire))
  {
    static std::atomic<int> n{0};
    if ((n.fetch_add(1, std::memory_order_relaxed) % 30) == 0)
    {
      open_shm();
    }
  }
}

struct LockedView
{
  uint8_t* y = nullptr;
  uint8_t* vu = nullptr;
  uint8_t* cb = nullptr;
  uint8_t* cr = nullptr;
  size_t ystride = 0;
  size_t uvstride = 0;
  bool uv_swapped = false;
  int lock_path = -1;
  int ahb_status = -ENOTSUP;
  int gbm_status = -ENOTSUP;
  uint32_t ahb_plane_count = 0;
};

static bool describe_yuv420(LockedView& view, int width, int height,
                            void* y, void* cb, void* cr,
                            size_t ystride, size_t cstride, size_t chroma_step)
{
  if (!y || !cb || !cr || (chroma_step != 1 && chroma_step != 2) ||
      ystride < static_cast<size_t>(width) ||
      cstride < static_cast<size_t>(width / 2) * chroma_step ||
      ystride > std::numeric_limits<int>::max() ||
      cstride > std::numeric_limits<int>::max())
    return false;

  const auto u = reinterpret_cast<uintptr_t>(cb);
  const auto v = reinterpret_cast<uintptr_t>(cr);
  const auto distance = u > v ? u - v : v - u;
  if (chroma_step == 2)
  {
    if (distance != 1)
      return false;
    view.vu = static_cast<uint8_t*>(v < u ? cr : cb);
    view.uv_swapped = u < v;
  }
  else
  {
    const size_t chroma_span = static_cast<size_t>(height / 2 - 1) * cstride + width / 2;
    if (distance < chroma_span)
      return false;
    view.cb = static_cast<uint8_t*>(cb);
    view.cr = static_cast<uint8_t*>(cr);
  }
  view.y = static_cast<uint8_t*>(y);
  view.ystride = ystride;
  view.uvstride = cstride;
  return true;
}

static bool wait_for_fence(int fence, int timeout_ms = 3000)
{
  if (fence < 0)
    return true;
  pollfd pfd{fence, POLLIN, 0};
  int rc;
  do
  {
    rc = poll(&pfd, 1, timeout_ms);
  } while (rc < 0 && errno == EINTR);
  return rc == 1 && (pfd.revents & POLLIN) != 0 &&
         (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
}

static bool lock_via_ahb(const void* anb, int W, int H, int fence,
                         void*& out_handle, LockedView& view)
{
  if (!g_ahb_lockPlanes || !g_ahb_unlock || !g_get_hardware_buffer)
    return false;
  auto* ahb = static_cast<AHardwareBuffer*>(g_get_hardware_buffer((void*) anb));
  if (!ahb)
    return false;
  int lock_fence = -1;
  if (fence >= 0)
  {
    lock_fence = dup(fence);
    if (lock_fence < 0)
    {
      view.ahb_status = -errno;
      return false;
    }
  }
  // AHardwareBuffer_lockPlanes takes ownership of the fence descriptor,
  // including on failure. The camera framework retains the original fd;
  // only the duplicate is transferred here.
  ARect rect{0, 0, W, H};
  AHardwareBuffer_Planes planes{};
  view.ahb_status = g_ahb_lockPlanes(ahb, kAhbUsageCpuRW, lock_fence, &rect, &planes);
  view.ahb_plane_count = planes.planeCount;
  if (view.ahb_status != 0)
    return false;

  const auto& p0 = planes.planes[0];
  const auto& p1 = planes.planes[1];
  const auto& p2 = planes.planes[2];
  if (planes.planeCount != 3 || p0.pixelStride != 1 ||
      p1.pixelStride != p2.pixelStride || p1.rowStride != p2.rowStride ||
      !describe_yuv420(view, W, H, p0.data, p1.data, p2.data,
                       p0.rowStride, p1.rowStride, p1.pixelStride))
  {
    view.ahb_status = -EINVAL;
    g_ahb_unlock(ahb, nullptr);
    return false;
  }
  view.lock_path = 0;
  out_handle = ahb;
  return true;
}

static bool lock_via_gbm(const void* handle, int W, int H,
                         int fence, LockedView& view)
{
  if (!gbm_ready())
    return false;
  if (!wait_for_fence(fence))
  {
    view.gbm_status = -EIO;
    return false;
  }
  Rect bounds{0, 0, W, H};
  android_ycbcr yc = {};

  view.gbm_status = g_gbm_lockYCbCr(g_gbm, handle, 0x33u, bounds, &yc);
  if (view.gbm_status != 0)
    return false;

  if (!describe_yuv420(view, W, H, yc.y, yc.cb, yc.cr,
                       yc.ystride, yc.cstride, yc.chroma_step))
  {
    view.gbm_status = -EINVAL;
    gbm_call_unlock(handle);
    return false;
  }
  view.lock_path = 1;
  return true;
}

static bool unlock(const void* handle, void* ahb, const LockedView& view)
{
  if (view.lock_path == 0 && ahb && g_ahb_unlock)
    return g_ahb_unlock(static_cast<AHardwareBuffer*>(ahb), nullptr) == 0;
  return view.lock_path == 1 && gbm_call_unlock(handle) == 0;
}

static std::atomic<int> g_stat_blob_lock_fail{0};
static std::atomic<int> g_stat_blob_too_big{0};
static std::atomic<int> g_stat_blob_ok{0};
static std::atomic<int> g_stat_blob_cache_hit{0};

struct EncodedJpeg
{
  std::shared_ptr<const uint8_t> data;
  size_t size = 0;
  uint32_t generation = 0;
  uint64_t frame = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

static std::mutex g_jpeg_cache_mutex;
static EncodedJpeg g_jpeg_cache;

static bool lock_blob(const void* anb, const void* handle, int W, int H, int fence,
                      void*& out_vaddr, void*& out_ahb_handle, int& out_lock_path)
{
  out_vaddr = nullptr;
  out_ahb_handle = nullptr;
  out_lock_path = -1;

  // BLOB buffers use their width as byte capacity and commonly have height 1.
  // Lock the whole allocation instead of applying the camera stream rectangle,
  // and never enter the AHB path unless the matching unlock API is available.
  if (g_ahb_lock && g_ahb_unlock && g_get_hardware_buffer)
  {
    auto* ahb = static_cast<AHardwareBuffer*>(g_get_hardware_buffer((void*) anb));
    int lock_fence = ahb && fence >= 0 ? dup(fence) : -1;
    if (ahb && (fence < 0 || lock_fence >= 0))
    {
      void* vaddr = nullptr;
      // AHardwareBuffer_lock takes ownership of lock_fence on every return.
      int rc = g_ahb_lock(ahb, kAhbUsageCpuRW, lock_fence, nullptr, &vaddr);
      if (rc == 0 && vaddr)
      {
        out_vaddr = vaddr;
        out_ahb_handle = ahb;
        out_lock_path = 0;
        return true;
      }
      if (rc == 0)
        g_ahb_unlock(ahb, nullptr);
    }
  }
  if (!wait_for_fence(fence))
    return false;
  if (g_gbm && g_gbm_lock_v2)
  {
    Rect bounds{0, 0, W, H};
    void* vaddr = nullptr;
    int rc = g_gbm_lock_v2(g_gbm, handle, 0x33u, bounds, &vaddr, nullptr, nullptr);
    if (rc == 0 && vaddr)
    {
      out_vaddr = vaddr;
      out_lock_path = 1;
      return true;
    }
    if (rc == 0)
      gbm_call_unlock(handle);
  }
  if (g_gbm && g_gbm_lock_v1)
  {
    Rect bounds{0, 0, W, H};
    void* vaddr = nullptr;
    int rc = g_gbm_lock_v1(g_gbm, handle, 0x33u, bounds, &vaddr);
    if (rc == 0 && vaddr)
    {
      out_vaddr = vaddr;
      out_lock_path = 1;
      return true;
    }
    if (rc == 0)
      gbm_call_unlock(handle);
  }
  return false;
}

struct BlobLockGuard
{
  const void* handle = nullptr;
  void* ahb = nullptr;
  int path = -1;

  ~BlobLockGuard() { unlock(); }
  BlobLockGuard(const void* native_handle, void* ahb_handle, int lock_path)
      : handle(native_handle), ahb(ahb_handle), path(lock_path)
  {
  }
  BlobLockGuard(const BlobLockGuard&) = delete;
  BlobLockGuard& operator=(const BlobLockGuard&) = delete;

  bool unlock() noexcept
  {
    const int previous = path;
    path = -1;
    if (previous == 0 && ahb && g_ahb_unlock)
      return g_ahb_unlock(static_cast<AHardwareBuffer*>(ahb), nullptr) == 0;
    return previous == 1 && handle && gbm_call_unlock(handle) == 0;
  }
};

static bool overwrite_blob_jpeg(ANWB* anb, int fence)
{
  if (!g_shm_ready.load(std::memory_order_acquire))
    return false;
  if (!feed_header_sane(g_shm.header, g_shm.size))
    return false;
  PinnedSlot source;
  if (!g_shm.pin_current(source))
    return false;
  if (!frame_is_fresh(source))
  {
    ++g_stat_skip_stale;
    return false;
  }
  const uint64_t cur_frame = source.frame_seq;
  if (cur_frame == 0)
    return false;

  const int src_w = (int) source.width;
  const int src_h = (int) source.height;
  if (src_w < 16 || src_h < 16)
    return false;

  const int W = anb->width;
  const int H = anb->height;
  // Android's BLOB ABI is one-dimensional: height must be exactly one and
  // width is the allocation size in bytes. Refuse malformed descriptors
  // rather than multiplying dimensions and risking an out-of-bounds trailer.
  if (W < 16 || W > 64 * 1024 * 1024)
    return false;
  if (H != 1)
    return false;
  const size_t buf_capacity = (size_t) W;
  const size_t trailer_size = CR_CAMERA_JPEG_TRAILER_BYTES;
  if (buf_capacity < trailer_size + 1024)
    return false;
  const size_t encode_cap = buf_capacity - trailer_size;

  const void* handle = anb->handle;
  if (!handle)
    return false;

  void* vaddr = nullptr;
  void* ahb_handle = nullptr;
  int lock_path = -1;
  if (!lock_blob(anb, handle, W, H, fence, vaddr, ahb_handle, lock_path))
  {
    const int n = ++g_stat_blob_lock_fail;
    if (n <= 5 || (n % 60) == 0)
      LOGW("blob: could not lock %zu-byte JPEG buffer", buf_capacity);
    return false;
  }
  int target_w = 0, target_h = 0;
  {
    BlobLockGuard blob_lock(handle, ahb_handle, lock_path);
    if (cr_camera_jpeg_dimensions(static_cast<const uint8_t*>(vaddr), buf_capacity,
                                  &target_w, &target_h) != 0 || !blob_lock.unlock())
      return false;
  }
  const size_t target_pixels = static_cast<size_t>(target_w) * target_h;
  constexpr size_t kMaxJpegPixels = 16u * 1024u * 1024u;
  cr_nv21_crop_rect crop;
  if (target_w < 8 || target_h < 8 || target_w > 8192 || target_h > 8192 ||
      target_pixels > kMaxJpegPixels ||
      !cr_nv21_center_crop(src_w, src_h, target_w, target_h, &crop))
  {
    LOGW("blob: unsupported JPEG dimensions %dx%d", target_w, target_h);
    return false;
  }

  EncodedJpeg jpeg;
  {
    std::lock_guard<std::mutex> guard(g_jpeg_cache_mutex);
    if (g_jpeg_cache.data && g_jpeg_cache.generation == source.generation &&
        g_jpeg_cache.frame == cur_frame &&
        g_jpeg_cache.width == static_cast<uint32_t>(target_w) &&
        g_jpeg_cache.height == static_cast<uint32_t>(target_h))
      jpeg = g_jpeg_cache;
  }
  const bool cache_hit = static_cast<bool>(jpeg.data);
  if (!cache_hit)
  {
    const uint8_t* pixels = source.data;
    size_t pixel_bytes = source.slot_size;
    std::vector<uint8_t> scaled;
    if (src_w != target_w || src_h != target_h)
    {
      scaled.resize(target_pixels + target_pixels / 2);
      // Use the same aspect-preserving crop as the live camera outputs.
      cr_nv21_blit(source.data, src_w, src_h, scaled.data(), scaled.data() + target_pixels,
                    target_w, target_h, target_w, target_w, 0);
      pixels = scaled.data();
      pixel_bytes = scaled.size();
    }
    uint8_t* encoded = nullptr;
    const int result = cr_nv21_to_jpeg(pixels, pixel_bytes, target_w, target_h,
                                      encode_cap, 85, &encoded, &jpeg.size);
    if (result != 0)
    {
      const int n = ++g_stat_blob_too_big;
      if (n <= 5 || (n % 60) == 0)
        LOGW(OBF("blob: JPEG preparation failed rc=%d cap=%zu").c_str(), result, encode_cap);
      return false;
    }
    jpeg.data = std::shared_ptr<const uint8_t>(encoded, std::free);
    jpeg.generation = source.generation;
    jpeg.frame = cur_frame;
    jpeg.width = target_w;
    jpeg.height = target_h;
    std::lock_guard<std::mutex> guard(g_jpeg_cache_mutex);
    g_jpeg_cache = jpeg;
  }
  if (jpeg.size > encode_cap)
    return false;

  // This buffer has not been queued to its consumer yet and the first
  // lock already waited for the producer fence.
  if (!lock_blob(anb, handle, W, H, -1, vaddr, ahb_handle, lock_path))
  {
    ++g_stat_blob_lock_fail;
    return false;
  }
  BlobLockGuard blob_lock(handle, ahb_handle, lock_path);
  if (access("/data/cr/photo_mode", R_OK) != 0 ||
      atomic_load_explicit(&source.header->channel_state, memory_order_acquire) !=
          CR_CHANNEL_STATE_READY ||
      cr_replace_camera_jpeg(static_cast<uint8_t*>(vaddr), buf_capacity,
                             jpeg.data.get(), jpeg.size) != 0)
    return false;
  if (!blob_lock.unlock())
    return false;

  if (cache_hit)
    ++g_stat_blob_cache_hit;
  int n = ++g_stat_blob_ok;
  if (n <= 5 || (n % 30) == 0)
  {
    LOGI("blob#%d substituted: %zu JPEG bytes (src %dx%d -> JPEG %dx%d)",
         n, jpeg.size, src_w, src_h, target_w, target_h);
  }
  return true;
}

struct CameraModes
{
  bool video;
  bool photo;

  bool active() const { return video || photo; }
};

static constexpr int kRaw10Format = 0x25;
struct Raw10Layout
{
  int width, height, stride;
};
static constexpr std::array<Raw10Layout, 3> kPixel5RawLayouts{{
    {4032, 3024, 5056}, {4656, 3496, 5824}, {3280, 2464, 4160}}};
static bool g_pixel5_raw10 = false;
static std::atomic<unsigned> g_raw10_written{0};
static std::atomic<unsigned> g_raw10_lock_failed{0};

struct Raw10Frame
{
  std::unique_ptr<uint8_t[]> pixels;
  uint32_t generation = 0;
  int64_t prepared = 0;
};

class Raw10Cache
{
public:
  ~Raw10Cache() { stop(); }

  void enable()
  {
    std::lock_guard lock(mutex_);
    enabled_ = true;
  }

  void stop()
  {
    {
      std::lock_guard lock(mutex_);
      enabled_ = false;
    }
    changed_.notify_one();
    if (worker_.joinable())
      worker_.join();
    std::lock_guard lock(mutex_);
    slots_ = {};
  }

  std::shared_ptr<const Raw10Frame> get(size_t profile, const PinnedSlot& source)
  {
    std::lock_guard lock(mutex_);
    if (!enabled_)
      return {};
    auto& slot = slots_[profile];
    const auto now = monotonic_ns();
    constexpr int64_t kRefreshNs = 250'000'000LL;
    if (slot.generation != source.generation ||
        (slot.sequence != source.frame_seq && now - slot.requested >= kRefreshNs))
    {
      if (!worker_.joinable())
        worker_ = std::thread(&Raw10Cache::run, this);
      slot.generation = source.generation;
      slot.sequence = source.frame_seq;
      slot.requested = now;
      slot.pending = true;
      changed_.notify_one();
    }
    constexpr int64_t kMaxRawAgeNs = 750'000'000LL;
    if (slot.frame && slot.frame->generation == source.generation &&
        now - slot.frame->prepared <= kMaxRawAgeNs)
      return slot.frame;
    return {};
  }

private:
  struct Slot
  {
    std::shared_ptr<const Raw10Frame> frame;
    uint32_t generation = 0;
    uint64_t sequence = 0;
    int64_t requested = 0;
    bool pending = false;
  };

  void run() noexcept
  {
    for (;;)
    {
      size_t profile;
      uint32_t generation;
      {
        std::unique_lock lock(mutex_);
        const auto has_pending = [](const Slot& slot) { return slot.pending; };
        changed_.wait(lock, [&] {
          return !enabled_ || std::any_of(slots_.begin(), slots_.end(), has_pending);
        });
        if (!enabled_)
          return;
        profile = next_;
        while (!slots_[profile].pending)
          profile = (profile + 1) % slots_.size();
        slots_[profile].pending = false;
        generation = slots_[profile].generation;
        next_ = (profile + 1) % slots_.size();
      }
      try
      {
        const auto [width, height, stride] = kPixel5RawLayouts[profile];
        PinnedSlot source;
        if (access("/data/cr/photo_mode", R_OK) != 0 ||
            !g_shm.pin_current(source) || source.generation != generation ||
            atomic_load_explicit(&source.header->channel_state, memory_order_acquire) !=
                CR_CHANNEL_STATE_READY)
          continue;
        const size_t size = static_cast<size_t>(width / 4) * 5 * height;
        auto result = std::make_shared<Raw10Frame>();
        result->pixels.reset(new uint8_t[size]);
        result->generation = generation;
        if (!cr::camhook::nv21_to_bggr10({source.data, source.slot_size}, source.width,
                                        source.height, {result->pixels.get(), size}, width, height))
          continue;
        result->prepared = monotonic_ns();
        std::lock_guard lock(mutex_);
        if (enabled_ && slots_[profile].generation == generation)
          slots_[profile].frame = std::move(result);
      }
      catch (const std::exception&)
      {
        // Continue serving the camera if memory is temporarily unavailable.
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  std::array<Slot, kPixel5RawLayouts.size()> slots_{};
  std::thread worker_;
  size_t next_ = 0;
  bool enabled_ = false;
};

static Raw10Cache g_raw10_cache;

static bool pixel5_stock_android14()
{
  char product[PROP_VALUE_MAX]{}, build[PROP_VALUE_MAX]{}, incremental[PROP_VALUE_MAX]{};
  __system_property_get("ro.product.device", product);
  __system_property_get("ro.build.id", build);
  __system_property_get("ro.build.version.incremental", incremental);
  return strcmp(product, "redfin") == 0 && strcmp(build, "UP1A.231105.001.B2") == 0 &&
         strcmp(incremental, "11260668") == 0;
}

static bool overwrite_raw10(ANWB* anb, const ANWBInfo& bi, int fence)
{
  const int width = bi.width, height = bi.height;
  if (!g_pixel5_raw10 || !g_ahb_lock || !g_ahb_unlock || !g_ahb_describe ||
      !g_get_hardware_buffer || anb->common.version < static_cast<int32_t>(sizeof(ANWB)) ||
      (anb->usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT) != 0)
    return false;
  const auto layout = std::find_if(kPixel5RawLayouts.begin(), kPixel5RawLayouts.end(),
      [&](const Raw10Layout& item) {
        return item.width == width && item.height == height && item.stride == bi.stride;
      });
  if (layout == kPixel5RawLayouts.end())
    return false;
  auto* ahb = static_cast<AHardwareBuffer*>(g_get_hardware_buffer(anb));
  if (!ahb)
    return false;
  AHardwareBuffer_Desc desc{};
  g_ahb_describe(ahb, &desc);
  if (desc.format != kRaw10Format || desc.layers != 1 ||
      desc.width != static_cast<uint32_t>(width) ||
      desc.height != static_cast<uint32_t>(height) ||
      desc.stride != static_cast<uint32_t>(bi.stride))
    return false;

  PinnedSlot source;
  if (!g_shm.pin_current(source) || !frame_is_fresh(source))
    return false;

  const auto cache = g_raw10_cache.get(static_cast<size_t>(layout - kPixel5RawLayouts.begin()), source);
  if (!cache)
    return false;
  const size_t row_bytes = static_cast<size_t>(width / 4) * 5;

  if (!wait_for_fence(fence, 8) || access("/data/cr/photo_mode", R_OK) != 0 ||
      atomic_load_explicit(&source.header->channel_state, memory_order_acquire) !=
          CR_CHANNEL_STATE_READY ||
      atomic_load_explicit(&source.header->generation, memory_order_acquire) != source.generation)
    return false;
  void* mapped = nullptr;
  const int status = g_ahb_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                -1, nullptr, &mapped);
  if (status != 0 || !mapped)
  {
    if (status == 0)
      g_ahb_unlock(ahb, nullptr);
    const unsigned failed = ++g_raw10_lock_failed;
    if (failed <= 5)
      LOGW("camera raw10: lock failed %dx%d stride=%d usage=0x%llx status=%d",
           width, height, bi.stride, static_cast<unsigned long long>(anb->usage), status);
    return false;
  }
  auto* destination = static_cast<uint8_t*>(mapped);
  for (int y = 0; y < height; ++y)
    memcpy(destination + static_cast<size_t>(y) * bi.stride,
           cache->pixels.get() + static_cast<size_t>(y) * row_bytes, row_bytes);
  if (g_ahb_unlock(ahb, nullptr) != 0)
    return false;
  const unsigned written = ++g_raw10_written;
  if (written <= 5 || written % 60 == 0)
    LOGI("camera raw10: replaced %u buffers (%dx%d stride=%d)",
         written, width, height, bi.stride);
  return true;
}

static CameraModes read_camera_modes(bool photo_supported)
{
  return {access("/data/cr/video_mode", R_OK) == 0,
          photo_supported && access("/data/cr/photo_mode", R_OK) == 0};
}

static cr::camhook::ReplacementOutcome overwrite_nv21(ANWB* anb, int fence,
                                                       CameraModes modes, bool deferred)
{
  ANWBInfo bi;
  if (!inspect_anwb(anb, bi))
    return cr::camhook::ReplacementOutcome::PassThrough;

  const int fmt = bi.format;
  const bool raw10 = fmt == kRaw10Format;
  if (fmt != 0x21 && fmt != 0x11 && fmt != 0x22 && fmt != 0x23 &&
      !(raw10 && g_pixel5_raw10 && modes.photo))
  {
    ++g_stat_skip_fmt;
    if (raw10)
      ++g_stat_skip_raw10;
    return cr::camhook::ReplacementOutcome::PassThrough;
  }
  if ((fmt == 0x21 || raw10) ? !modes.photo || !refresh_photo_mode_() : !modes.video)
    return cr::camhook::ReplacementOutcome::PassThrough;

  try_map_feed();
  if (!g_shm_ready.load(std::memory_order_acquire))
  {
    ++g_stat_skip_noshm;
    return cr::camhook::ReplacementOutcome::PassThrough;
  }
  if (!feed_header_sane(g_shm.header, g_shm.size))
    return cr::camhook::ReplacementOutcome::PassThrough;

  const uint32_t channel_state = atomic_load_explicit(
      &g_shm.header->channel_state, memory_order_acquire);
  if (channel_state != CR_CHANNEL_STATE_READY)
  {
    int n = ++g_stat_skip_channel;
    if (n <= 5 || (n % 60) == 0)
    {
      LOGW(OBF("skip_channel#%d state=%u frame=%llu gen=%u magic=0x%x").c_str(),
           n,
           channel_state,
           (unsigned long long) atomic_load(&g_shm.header->frame_counter),
           atomic_load(&g_shm.header->generation),
           __atomic_load_n(&g_shm.header->magic, __ATOMIC_ACQUIRE));
    }
    return cr::camhook::ReplacementOutcome::PassThrough;
  }

  if (atomic_load(&g_shm.header->frame_counter) == 0)
  {
    int n = ++g_stat_skip_noframe;
    if (n <= 5 || (n % 60) == 0)
    {
      LOGW(OBF("skip_noframe#%d state=%u dims=%ux%u gen=%u").c_str(),
           n,
           channel_state,
           __atomic_load_n(&g_shm.header->width, __ATOMIC_ACQUIRE),
           __atomic_load_n(&g_shm.header->height, __ATOMIC_ACQUIRE),
           atomic_load(&g_shm.header->generation));
    }
    return cr::camhook::ReplacementOutcome::PassThrough;
  }

  // Mapper initialization can call into Android services. Run it on a
  // normal frame callback, after confirming that the current route is live.
  resolve_runtime_ops();
  // Keep the mapper's operations stable through lock, write and unlock.
  std::shared_lock<std::shared_mutex> runtime_ops(g_runtime_ops_mu);

  if (raw10)
  {
    if (overwrite_raw10(anb, bi, fence))
      return cr::camhook::ReplacementOutcome::Raw10Written;
    ++g_stat_skip_raw10;
    ++g_stat_skip_fmt;
    return cr::camhook::ReplacementOutcome::PassThrough;
  }
  if (fmt == 0x21)
    return overwrite_blob_jpeg(anb, fence)
               ? cr::camhook::ReplacementOutcome::JpegWritten
               : cr::camhook::ReplacementOutcome::AttemptedFailed;

  if (!ahb_planes_ready() && !gbm_ready())
  {
    ++g_stat_skip_nogbm;
    return cr::camhook::ReplacementOutcome::PassThrough;
  }

  const int W = bi.width;
  const int H = bi.height;
  if (W < 16 || H < 16 || W > 8192 || H > 8192 || (W & 1) || (H & 1) ||
      static_cast<int64_t>(W) * H > 32LL * 1024 * 1024)
  {
    ++g_stat_skip_dims;
    return cr::camhook::ReplacementOutcome::PassThrough;
  }

  const void* handle = bi.handle;

  LockedView view;
  void* ahb = nullptr;
  bool locked = lock_via_ahb(anb, W, H, fence, ahb, view);
  if (!locked)
  {
    locked = lock_via_gbm(handle, W, H, fence, view);
  }
  if (!locked)
  {
    int n = ++g_stat_lock_fail;
    if (n <= 5 || (n % 60) == 0)
    {
      const auto usage = anb->common.version >= static_cast<int32_t>(sizeof(ANWB))
                             ? static_cast<unsigned long long>(anb->usage) : 0;
      LOGW(OBF("lock failed (%dx%d fmt=0x%x usage=0x%llx ahb=%d planes=%u mapper=%d)").c_str(),
           W, H, fmt, usage, view.ahb_status, view.ahb_plane_count, view.gbm_status);
    }
    return cr::camhook::ReplacementOutcome::AttemptedFailed;
  }

  PinnedSlot source;
  if (!g_shm.pin_current(source))
  {
    unlock(handle, ahb, view);
    return cr::camhook::ReplacementOutcome::AttemptedFailed;
  }
  if (!frame_is_fresh(source))
  {
    ++g_stat_skip_stale;
    unlock(handle, ahb, view);
    return cr::camhook::ReplacementOutcome::PassThrough;
  }
  cr_nv21_crop_rect crop;
  if (!cr_nv21_center_crop(source.width, source.height, W, H, &crop) ||
      access("/data/cr/video_mode", R_OK) != 0 ||
      atomic_load_explicit(&source.header->channel_state, memory_order_acquire) !=
          CR_CHANNEL_STATE_READY)
  {
    unlock(handle, ahb, view);
    return cr::camhook::ReplacementOutcome::PassThrough;
  }

  if (view.vu)
    blit_adaptive(source.data, (int) source.width, (int) source.height,
                  view.y, view.vu, W, H, (int) view.ystride,
                  (int) view.uvstride, view.uv_swapped ? 1 : 0);
  else
    cr_nv21_blit_planar(source.data, (int) source.width, (int) source.height,
                        view.y, view.cb, view.cr, W, H,
                        (int) view.ystride, (int) view.uvstride);

  const bool replaced = unlock(handle, ahb, view);
  if (replaced && !deferred)
    ++g_stat_blit_ok;
  return replaced ? cr::camhook::ReplacementOutcome::YuvWritten
                  : cr::camhook::ReplacementOutcome::AttemptedFailed;
}

using qbtc_surfaces_t = std::int32_t (*)(void* thiz, void* consumer, ANWB* anb,
                                        int fence, const std::vector<std::size_t>& surface_ids);
using qbtc_legacy_t = std::int32_t (*)(void* thiz, void* consumer, ANWB* anb,
                                      int fence);
using preview_spacer_t = std::int32_t (*)(void* thiz, std::int64_t timestamp,
                                         std::int64_t readout_timestamp, std::int32_t transform,
                                         ANWB* anb, int fence);

static void* g_orig_qbtc_surfaces = nullptr;
static void* g_orig_qbtc_legacy = nullptr;
static void* g_orig_preview_spacer = nullptr;
static void* g_preview_spacer_stub = nullptr;
static void* g_video_encoder_usage_stub = nullptr;
static void* g_orig_video_encoder_usage = nullptr;
static void* g_video_encoder_format_stub = nullptr;
static void* g_orig_video_encoder_format = nullptr;
static void* g_display_usage_stub = nullptr;
static void* g_orig_display_usage = nullptr;
using SurfaceConsumerUsage = int32_t (*)(const void*, uint64_t*);
static SurfaceConsumerUsage g_surface_consumer_usage = nullptr;
static cr::camhook::HookCounters g_hook_counters;
static cr::camhook::HookCounters g_preview_counters;

static cr::camhook::ReplacementOutcome replace_for_queue(ANWB* anb, int fence, CameraModes modes)
{
  const std::uint64_t n = g_hook_counters.callback_seen.load(std::memory_order_relaxed);
  if (n <= 5 || (n % 60) == 0)
  {
    ANWBInfo bi{};
    const bool layout_ok = inspect_anwb(anb, bi);
    LOGI(OBF("qbtc#%llu anb=%p %dx%d stride=%d fmt=0x%x handle=%p").c_str(),
         (unsigned long long) n, anb, layout_ok ? bi.width : -1,
         layout_ok ? bi.height : -1, layout_ok ? bi.stride : -1,
         layout_ok ? bi.format : -1, layout_ok ? bi.handle : nullptr);
  }

  using Outcome = cr::camhook::ReplacementOutcome;
  const auto outcome = overwrite_nv21(anb, fence, modes, false);
  if (outcome == Outcome::YuvWritten &&
      anb->common.version >= static_cast<int32_t>(sizeof(ANWB)) &&
      (anb->usage & AHARDWAREBUFFER_USAGE_VIDEO_ENCODE) != 0)
    return Outcome::RecordingYuvWritten;
  return outcome;
}

static void report_queue_stats() noexcept
{
  const std::uint64_t n = g_hook_counters.callback_seen.load(std::memory_order_relaxed);
  if (n <= 5 || (n % 30) == 0)
  {
    LOGI(OBF("camera_output v2 yuv=%d jpeg=%d raw10=%d unsupported=%d queued_yuv=%llu queued_jpeg=%llu queue_error=%llu").c_str(),
         g_stat_blit_ok.load(), g_stat_blob_ok.load(), g_stat_skip_raw10.load(),
         g_stat_skip_fmt.load(),
         (unsigned long long) g_hook_counters.yuv_queued.load(),
         (unsigned long long) g_hook_counters.jpeg_queued.load(),
         (unsigned long long) g_hook_counters.original_queue_failed.load());
    if (const auto recording = g_hook_counters.recording_queued.load(); recording != 0)
      LOGI("camera_recording v1 queued=%llu", static_cast<unsigned long long>(recording));
    const auto raw_written = g_raw10_written.load();
    const auto raw_failed = g_raw10_lock_failed.load();
    if (raw_written != 0 || raw_failed != 0)
      LOGI("camera_raw10 v1 written=%u queued=%llu lock_error=%u",
           raw_written, static_cast<unsigned long long>(g_hook_counters.raw10_queued.load()), raw_failed);
  }

  if ((n % 60) == 0)
  {
    LOGI(OBF("stats: callbacks=%llu attempted=%llu pass=%llu queue_failed=%llu skip_channel=%d skip_noframe=%d skip_stale=%d skip_noshm=%d skip_fmt=%d skip_lock=%d blit_ok=%d blob_ok=%d blob_cache_hit=%d noanb=%d layout=%d nogbm=%d dims=%d hnd=%d").c_str(),
         (unsigned long long) g_hook_counters.callback_seen.load(),
         (unsigned long long) g_hook_counters.replacement_attempted.load(),
         (unsigned long long) g_hook_counters.pass_through.load(),
         (unsigned long long) g_hook_counters.original_queue_failed.load(),
         g_stat_skip_channel.load(),
         g_stat_skip_noframe.load(),
         g_stat_skip_stale.load(),
         g_stat_skip_noshm.load(),
         g_stat_skip_fmt.load(),
         g_stat_lock_fail.load(),
         g_stat_blit_ok.load(),
         g_stat_blob_ok.load(),
         g_stat_blob_cache_hit.load(),
         g_stat_skip_noanb.load(),
         g_stat_skip_layout.load(),
         g_stat_skip_nogbm.load(),
         g_stat_skip_dims.load(),
         g_stat_skip_handle.load());
  }
}

static std::int32_t proxy_qbtc_surfaces(void* thiz, void* consumer, ANWB* anb,
                                        int fence, const std::vector<std::size_t>& surface_ids)
{
  const auto modes = read_camera_modes(true);
  const auto status = cr::camhook::dispatch_queue(
      g_hook_counters, modes.active(), [=] { return replace_for_queue(anb, fence, modes); },
      reinterpret_cast<qbtc_surfaces_t>(__atomic_load_n(&g_orig_qbtc_surfaces, __ATOMIC_ACQUIRE)),
      thiz, consumer, anb, fence, surface_ids);
  if (modes.active())
    report_queue_stats();
  return status;
}

static std::int32_t proxy_qbtc_legacy(void* thiz, void* consumer, ANWB* anb,
                                      int fence)
{
  const auto modes = read_camera_modes(true);
  const auto status = cr::camhook::dispatch_queue(
      g_hook_counters, modes.active(), [=] { return replace_for_queue(anb, fence, modes); },
      reinterpret_cast<qbtc_legacy_t>(__atomic_load_n(&g_orig_qbtc_legacy, __ATOMIC_ACQUIRE)),
      thiz, consumer, anb, fence);
  if (modes.active())
    report_queue_stats();
  return status;
}

static std::int32_t proxy_preview_spacer(void* thiz, std::int64_t timestamp,
                                        std::int64_t readout_timestamp, std::int32_t transform,
                                        ANWB* anb, int fence)
{
  const auto modes = read_camera_modes(false);
  const auto status = cr::camhook::dispatch_queue(
      g_preview_counters, modes.active(), [=] { return overwrite_nv21(anb, fence, modes, true); },
      reinterpret_cast<preview_spacer_t>(__atomic_load_n(&g_orig_preview_spacer, __ATOMIC_ACQUIRE)),
      thiz, timestamp, readout_timestamp, transform, anb, fence);
  // A successful return acknowledges the spacer's pending queue. Its worker
  // submits the buffer to the preview consumer asynchronously.
  const auto seen = g_preview_counters.callback_seen.load(std::memory_order_relaxed);
  if (modes.active() && (seen <= 5 || (seen % 30) == 0))
    LOGI("camera_preview v1 seen=%llu staged=%llu queue_error=%llu",
         (unsigned long long) seen,
         (unsigned long long) g_preview_counters.yuv_queued.load(std::memory_order_relaxed),
         (unsigned long long) g_preview_counters.original_queue_failed.load(std::memory_order_relaxed));
  return status;
}

static void try_hook_preview_spacer()
{
  // Use the exact two-timestamp Android 14 arm64 signature verified on Pixel 5.
  auto symbol = OBF("_ZN7android7camera318PreviewFrameSpacer18queuePreviewBufferElliP19ANativeWindowBufferi");
  __atomic_store_n(&g_orig_preview_spacer, nullptr, __ATOMIC_RELAXED);
  void* stub = shadowhook_hook_sym_name("libcameraservice.so", symbol.c_str(),
                                        reinterpret_cast<void*>(proxy_preview_spacer),
                                        &g_orig_preview_spacer);
  if (stub && __atomic_load_n(&g_orig_preview_spacer, __ATOMIC_ACQUIRE))
  {
    g_preview_spacer_stub = stub;
    LOGI("hook installed on PreviewFrameSpacer");
    return;
  }
  if (stub)
    shadowhook_unhook(stub);
  LOGW("PreviewFrameSpacer ABI unavailable");
}

static constexpr uint64_t kQtiUbwcUsage = uint64_t{1} << 28;

static bool recorder_needs_linear_yuv(uint64_t usage)
{
  constexpr uint64_t kRecorderUbwc = AHARDWAREBUFFER_USAGE_VIDEO_ENCODE | kQtiUbwcUsage;
  return (usage & (kRecorderUbwc | AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT)) == kRecorderUbwc &&
         access("/data/cr/video_mode", R_OK) == 0;
}

static int32_t proxy_video_encoder_usage(const void* stream, uint64_t* usage)
{
  using EndpointUsage = int32_t (*)(const void*, uint64_t*);
  auto original = reinterpret_cast<EndpointUsage>(
      __atomic_load_n(&g_orig_video_encoder_usage, __ATOMIC_ACQUIRE));
  if (!original)
    return -ENOSYS;
  const int32_t status = original(stream, usage);
  if (status == 0 && usage && recorder_needs_linear_yuv(*usage))
  {
    *usage = (*usage & ~kQtiUbwcUsage) | kAhbUsageCpuRW;
  }
  return status;
}

static int32_t proxy_video_encoder_format(void* surface, int32_t format)
{
  using SetFormat = int32_t (*)(void*, int32_t);
  const auto original = reinterpret_cast<SetFormat>(
      __atomic_load_n(&g_orig_video_encoder_format, __ATOMIC_ACQUIRE));
  if (!original)
    return -ENOSYS;
  uint64_t usage = 0;
  if (format == 0x22 && g_surface_consumer_usage(surface, &usage) == 0 &&
      recorder_needs_linear_yuv(usage))
  {
    // The recorder still contributes its consumer usage to the BufferQueue.
    // An explicit flexible YUV format selects linear NV12 on this allocator.
    LOGI("camera recorder requested a linear YUV420 output surface");
    return original(surface, 0x23);
  }
  return original(surface, format);
}

static int32_t proxy_camera_display_usage(void* surface, uint64_t usage)
{
  using SetUsage = int32_t (*)(void*, uint64_t);
  const auto original = reinterpret_cast<SetUsage>(
      __atomic_load_n(&g_orig_display_usage, __ATOMIC_ACQUIRE));
  if (!original)
    return -ENOSYS;
  constexpr uint64_t kCameraOutput = uint64_t{1} << 17;
  constexpr uint64_t kDisplay = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
  uint64_t consumer = 0;
  if ((usage & (kQtiUbwcUsage | kCameraOutput | AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT)) ==
          (kQtiUbwcUsage | kCameraOutput) &&
      access("/data/cr/video_mode", R_OK) == 0 &&
      g_surface_consumer_usage(surface, &consumer) == 0 && consumer == kDisplay)
  {
    // Keep the stream's PRIVATE format and the HAL's negotiated usage.
    // Only change the display BufferQueue's producer allocation request.
    LOGI("camera display requested CPU-writable allocation");
    usage = (usage & ~kQtiUbwcUsage) | kAhbUsageCpuRW;
  }
  return original(surface, usage);
}

static void try_hook_video_encoder_buffers()
{
  char device[PROP_VALUE_MAX] = {};
  char sdk[PROP_VALUE_MAX] = {};
  __system_property_get("ro.product.device", device);
  __system_property_get("ro.build.version.sdk", sdk);
  if (strcmp(device, "redfin") != 0 || strcmp(sdk, "34") != 0)
    return;

  void* gui = dlopen("libgui.so", RTLD_NOW | RTLD_NOLOAD);
  if (!gui)
    return;
  g_surface_consumer_usage = reinterpret_cast<SurfaceConsumerUsage>(
      dlsym(gui, "_ZNK7android7Surface16getConsumerUsageEPm"));
  dlclose(gui);
  if (!g_surface_consumer_usage)
    return;

  if (pixel5_stock_android14())
  {
    __atomic_store_n(&g_orig_display_usage, nullptr, __ATOMIC_RELAXED);
    void* display_hook = shadowhook_hook_sym_name("libgui.so", "_ZN7android7Surface8setUsageEm",
        reinterpret_cast<void*>(proxy_camera_display_usage), &g_orig_display_usage);
    if (display_hook && __atomic_load_n(&g_orig_display_usage, __ATOMIC_ACQUIRE))
      g_display_usage_stub = display_hook;
    else if (display_hook)
      shadowhook_unhook(display_hook);
  }

  __atomic_store_n(&g_orig_video_encoder_format, nullptr, __ATOMIC_RELAXED);
  void* format_hook = shadowhook_hook_sym_name("libgui.so",
      "_ZN7android7Surface16setBuffersFormatEi", reinterpret_cast<void*>(proxy_video_encoder_format),
      &g_orig_video_encoder_format);
  if (!format_hook || !__atomic_load_n(&g_orig_video_encoder_format, __ATOMIC_ACQUIRE))
  {
    if (format_hook)
      shadowhook_unhook(format_hook);
    return;
  }
  const auto symbol = OBF("_ZNK7android7camera319Camera3OutputStream16getEndpointUsageEPm");
  __atomic_store_n(&g_orig_video_encoder_usage, nullptr, __ATOMIC_RELAXED);
  void* stub = shadowhook_hook_sym_name("libcameraservice.so", symbol.c_str(),
      reinterpret_cast<void*>(proxy_video_encoder_usage), &g_orig_video_encoder_usage);
  if (stub && __atomic_load_n(&g_orig_video_encoder_usage, __ATOMIC_ACQUIRE))
  {
    g_video_encoder_usage_stub = stub;
    g_video_encoder_format_stub = format_hook;
    return;
  }
  shadowhook_unhook(format_hook);
  if (stub)
    shadowhook_unhook(stub);
  LOGW("Camera3 recorder endpoint usage ABI unavailable");
}

static int try_hook_primary()
{
  shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);

  auto try_one = [&](cr::camhook::TargetAbiDescriptor abi) -> bool
  {
    auto libcameraservice = OBF("libcameraservice.so");
    auto cameraserver = OBF("cameraserver");
    const char* libs[] = {
        libcameraservice.c_str(),
        cameraserver.c_str(),
    };
    for (const char* lib : libs)
    {
      void* proxy = abi.kind == cr::camhook::TargetAbiKind::QueueWithSurfaceIds
                        ? reinterpret_cast<void*>(proxy_qbtc_surfaces)
                        : reinterpret_cast<void*>(proxy_qbtc_legacy);
      // ShadowHook publishes this storage before redirecting the target.
      // Keep it alive and usable by a callback during installation itself.
      void** original = abi.kind == cr::camhook::TargetAbiKind::QueueWithSurfaceIds
                            ? &g_orig_qbtc_surfaces : &g_orig_qbtc_legacy;
      __atomic_store_n(original, nullptr, __ATOMIC_RELAXED);
      void* stub = shadowhook_hook_sym_name(lib, abi.symbol, proxy, original);
      if (stub)
      {
        if (!__atomic_load_n(original, __ATOMIC_ACQUIRE))
        {
          LOGE(OBF("hook returned no original target; rolling back").c_str());
          shadowhook_unhook(stub);
          continue;
        }
        g_hook_stub = stub;
        LOGI(OBF("hook installed on queueBufferToConsumer (lib=%s sym=%s abi=%s)").c_str(),
             lib, abi.symbol,
             abi.kind == cr::camhook::TargetAbiKind::QueueWithSurfaceIds
                 ? OBF("surface_ids").c_str()
                 : OBF("legacy").c_str());
        return true;
      }
    }
    LOGW(OBF("qbtc try %.40s… → %s").c_str(), abi.symbol,
         shadowhook_to_errmsg(shadowhook_get_errno()));
    return false;
  };
  if (try_one({OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferiRKNSt3__16vectorImNS8_"
                       "9allocatorImEEEE")
                       .c_str(),
               cr::camhook::TargetAbiKind::QueueWithSurfaceIds}))
    return 0;

  if (try_one({OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferiRKNSt3__16vectorImNS5_"
                       "9allocatorImEEEE")
                       .c_str(),
               cr::camhook::TargetAbiKind::QueueWithSurfaceIds}))
    return 0;

  if (try_one({OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferiRKNSt3__16vectorImNS6_"
                       "9allocatorImEEEE")
                       .c_str(),
               cr::camhook::TargetAbiKind::QueueWithSurfaceIds}))
    return 0;

  if (try_one({OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferiRKNSt3__16vectorImNS7_"
                       "9allocatorImEEEE")
                       .c_str(),
               cr::camhook::TargetAbiKind::QueueWithSurfaceIds}))
    return 0;

  if (try_one({OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferi").c_str(),
               cr::camhook::TargetAbiKind::QueueLegacy}))
    return 0;

  // ReCam's central ABI lesson is intentionally applied here: a symbol prefix
  // is not a calling convention. Only exact, reviewed mangled signatures may
  // enter proxy_qbtc. Unknown firmware fails closed instead of guessing a C++
  // parameter layout and crashing cameraserver.
  return -1;
}

extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_camhook_shutdown();

extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_camhook_init(const char* feed_url)
{
  const char* process = getprogname();
  if (!process || strcmp(process, "cameraserver") != 0)
  {
    LOGE("Refusing camera hooks outside cameraserver");
    return -1;
  }
  if (g_installed.exchange(true))
  {
    LOGI(OBF("already installed").c_str());
    return 0;
  }
  LOGI(OBF("cr_camhook_init feed_url=%s").c_str(), feed_url ? feed_url : OBF("(null)").c_str());

  resolve_scaler();
  resolve_photo_mode();
  g_pixel5_raw10 = pixel5_stock_android14();
  if (g_pixel5_raw10)
    g_raw10_cache.enable();
  open_shm();

  int hook_rc = -1;
  if (try_hook_primary() == 0)
  {
    try_hook_preview_spacer();
    try_hook_video_encoder_buffers();
    hook_rc = 0;
  }
  else
  {
    LOGE(OBF("reviewed queueBufferToConsumer ABI not found; refusing unsafe fallback").c_str());
  }

  if (hook_rc == 0)
  {
    // The root controller pre-creates this owner-restricted marker for
    // cameraserver. Failure to open it keeps host verification fail-closed.
    int fd = open(OBF("/data/cr/camhook.ready").c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd >= 0)
    {
      char buf[32];
      int n = snprintf(buf, sizeof(buf), "%ld\n", (long) getpid());
      const bool written = n > 0 && n < static_cast<int>(sizeof(buf)) &&
                           write(fd, buf, static_cast<size_t>(n)) == n;
      close(fd);
      if (!written)
      {
        LOGE("Cannot write camera readiness marker");
        cr_camhook_shutdown();
        return -1;
      }
    }
    else
    {
      LOGE(OBF("cannot publish camera readiness marker: %s").c_str(), strerror(errno));
      cr_camhook_shutdown();
      return -1;
    }
    return 0;
  }

  LOGE(OBF("no supported camera symbol found — hook not installed").c_str());
  g_installed = false;
  return -1;
}

extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_camhook_shutdown()
{
  if (!g_installed.exchange(false))
    return 0;
  g_raw10_cache.stop();
  if (g_display_usage_stub)
  {
    shadowhook_unhook(g_display_usage_stub);
    g_display_usage_stub = nullptr;
  }
  if (g_preview_spacer_stub)
  {
    shadowhook_unhook(g_preview_spacer_stub);
    g_preview_spacer_stub = nullptr;
  }
  if (g_video_encoder_usage_stub)
  {
    shadowhook_unhook(g_video_encoder_usage_stub);
    g_video_encoder_usage_stub = nullptr;
  }
  if (g_video_encoder_format_stub)
  {
    shadowhook_unhook(g_video_encoder_format_stub);
    g_video_encoder_format_stub = nullptr;
  }
  if (g_hook_stub)
  {
    shadowhook_unhook(g_hook_stub);
    g_hook_stub = nullptr;
  }
  if (g_shm.fd >= 0)
  {
    // Keep the mapping resident. A callback can already be inside ShadowHook's
    // trampoline when unhook returns; service restart reclaims this small map
    // without risking an asynchronous use-after-unmap.
    LOGI(OBF("camhook mapping retained until cameraserver exits").c_str());
  }
  LOGI(OBF("cr_camhook_shutdown done").c_str());
  return 0;
}
