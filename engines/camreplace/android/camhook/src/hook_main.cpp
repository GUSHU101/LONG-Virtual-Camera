

#include "cr_feed.h"
#include "shm.h"
#include "nv21_blit.h"
#include "jpeg_encode.h"
#include "HookDispatch.h"
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
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include "util/Obf.h"

/*
 * Installs the cameraserver-side camera-frame replacement hook.
 *
 * cr_camhook_init maps the feed shared-memory file, selects the configured
 * NV21 scaler, resolves compatible camera buffer APIs, and installs a
 * ShadowHook proxy for a supported Camera3OutputStream queue method. The
 * proxy validates the mapped feed header, locks the destination buffer,
 * scales the latest committed NV21 frame into it, and calls the original
 * queue method. Multiple mangled symbol and buffer-mapper layouts are tried
 * to cover Android platform variants without runtime SDK branching.
 *
 * Photo mode writes a JPEG BLOB instead of a preview frame. Shared memory is
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

struct CR_ARect
{
  int32_t left, top, right, bottom;
};

struct CR_AHB_Plane
{
  void* data;
  uint32_t pixelStride;
  uint32_t rowStride;
};
struct CR_AHB_Planes
{
  CR_AHB_Plane planes[4];
  uint32_t planeCount;
};

constexpr uint64_t kAhbUsageCpuRW = 0x2ull | 0x30ull;

typedef int (*AHB_lockPlanes_fn)(void* buffer, uint64_t usage, int32_t fence, const CR_ARect* rect, CR_AHB_Planes* outPlanes);
typedef int (*AHB_unlock_fn)(void* buffer, int32_t* fence);

typedef int (*AHB_lock_fn)(void* buffer, uint64_t usage, int32_t fence, const CR_ARect* rect, void** outVirtualAddress);
typedef void* (*ANWB_getHardwareBuffer_fn)(void* buffer);

static AHB_lockPlanes_fn g_ahb_lockPlanes = nullptr;
static AHB_unlock_fn g_ahb_unlock = nullptr;
static AHB_lock_fn g_ahb_lock = nullptr;
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
static std::atomic<int> g_stat_no_vu{0};
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

static void try_lazy_init()
{
  if (!g_shm_ready.load(std::memory_order_acquire))
  {
    static std::atomic<int> n{0};
    if ((n.fetch_add(1, std::memory_order_relaxed) % 30) == 0)
    {
      open_shm();
    }
  }
  static std::atomic<int> runtime_attempt{0};
  if ((runtime_attempt.fetch_add(1, std::memory_order_relaxed) % 30) == 0)
    resolve_runtime_ops();
}

struct LockedView
{
  uint8_t* y = nullptr;
  uint8_t* vu = nullptr;
  size_t ystride = 0;
  size_t uvstride = 0;
  bool uv_swapped = false;
  int lock_path = -1;
};

static bool wait_for_fence(int fence)
{
  if (fence < 0)
    return true;
  pollfd pfd{fence, POLLIN, 0};
  int rc;
  do
  {
    rc = poll(&pfd, 1, 3000);
  } while (rc < 0 && errno == EINTR);
  return rc == 1 && (pfd.revents & POLLIN) != 0 &&
         (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
}

static bool lock_via_ahb(const void* anb, int W, int H, int fence,
                         void*& out_handle, LockedView& view)
{
  if (!g_ahb_lockPlanes || !g_ahb_unlock || !g_get_hardware_buffer)
    return false;
  void* ahb = g_get_hardware_buffer((void*) anb);
  if (!ahb)
    return false;
  int lock_fence = -1;
  if (fence >= 0)
  {
    lock_fence = dup(fence);
    if (lock_fence < 0)
      return false;
  }
  // AHardwareBuffer_lockPlanes takes ownership of the fence descriptor,
  // including on failure. The camera framework retains the original fd;
  // only the duplicate is transferred here.
  CR_ARect rect{0, 0, W, H};
  CR_AHB_Planes planes{};
  int rc = g_ahb_lockPlanes(ahb, kAhbUsageCpuRW, lock_fence, &rect, &planes);
  if (rc != 0)
    return false;
  if (planes.planeCount < 3)
  {
    g_ahb_unlock(ahb, nullptr);
    return false;
  }

  auto& p0 = planes.planes[0];
  auto& p1 = planes.planes[1];
  auto& p2 = planes.planes[2];

  const uintptr_t cb = reinterpret_cast<uintptr_t>(p1.data);
  const uintptr_t cr = reinterpret_cast<uintptr_t>(p2.data);
  const uintptr_t chroma_delta = cb > cr ? cb - cr : cr - cb;
  if (!p0.data || !p1.data || !p2.data || p0.pixelStride != 1 ||
      p1.pixelStride != 2 || p2.pixelStride != 2 ||
      p0.rowStride < static_cast<uint32_t>(W) ||
      p1.rowStride < static_cast<uint32_t>(W) ||
      p0.rowStride > std::numeric_limits<int>::max() ||
      p1.rowStride > std::numeric_limits<int>::max() ||
      p2.rowStride != p1.rowStride || chroma_delta != 1u)
  {
    g_ahb_unlock(ahb, nullptr);
    return false;
  }
  view.y = static_cast<uint8_t*>(p0.data);
  view.ystride = p0.rowStride;
  view.uvstride = p1.rowStride;
  if (cr < cb)
  {
    view.vu = (uint8_t*) p2.data;
    view.uv_swapped = false;
  }
  else
  {
    view.vu = (uint8_t*) p1.data;
    view.uv_swapped = true;
  }
  view.lock_path = 0;
  out_handle = ahb;
  return true;
}

static bool lock_via_gbm(const void* anb, const void* handle, int W, int H,
                         int fence, LockedView& view)
{
  if (!gbm_ready() || !wait_for_fence(fence))
    return false;
  Rect bounds{0, 0, W, H};
  android_ycbcr yc = {};

  int rc = g_gbm_lockYCbCr(g_gbm, handle, 0x33u, bounds, &yc);
  if (rc != 0)
    return false;

  const uintptr_t cb = reinterpret_cast<uintptr_t>(yc.cb);
  const uintptr_t cr = reinterpret_cast<uintptr_t>(yc.cr);
  const uintptr_t chroma_delta = cb > cr ? cb - cr : cr - cb;
  if (yc.y && yc.cb && yc.cr && yc.chroma_step == 2 &&
      yc.ystride >= static_cast<size_t>(W) &&
      yc.cstride >= static_cast<size_t>(W) &&
      yc.ystride <= std::numeric_limits<int>::max() &&
      yc.cstride <= std::numeric_limits<int>::max() && chroma_delta == 1u)
  {
    view.y = static_cast<uint8_t*>(yc.y);
    view.ystride = yc.ystride;
    view.uvstride = yc.cstride;
    if (cr < cb)
    {
      view.vu = (uint8_t*) yc.cr;
      view.uv_swapped = false;
    }
    else
    {
      view.vu = (uint8_t*) yc.cb;
      view.uv_swapped = true;
    }
  }
  else
  {
    gbm_call_unlock(handle);
    return false;
  }
  view.lock_path = 1;
  (void) anb;
  return true;
}

static bool unlock(const void* handle, void* ahb, const LockedView& view)
{
  if (view.lock_path == 0 && ahb && g_ahb_unlock)
    return g_ahb_unlock(ahb, nullptr) == 0;
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
    void* ahb = g_get_hardware_buffer((void*) anb);
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
      return g_ahb_unlock(ahb, nullptr) == 0;
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

  EncodedJpeg jpeg;
  {
    std::lock_guard<std::mutex> guard(g_jpeg_cache_mutex);
    if (g_jpeg_cache.data && g_jpeg_cache.generation == source.generation &&
        g_jpeg_cache.frame == cur_frame && g_jpeg_cache.width == source.width &&
        g_jpeg_cache.height == source.height)
      jpeg = g_jpeg_cache;
  }
  const bool cache_hit = static_cast<bool>(jpeg.data);
  if (!cache_hit)
  {
    uint8_t* encoded = nullptr;
    const int result = cr_nv21_to_jpeg(source.data, source.slot_size, src_w, src_h,
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
    jpeg.width = source.width;
    jpeg.height = source.height;
    std::lock_guard<std::mutex> guard(g_jpeg_cache_mutex);
    g_jpeg_cache = jpeg;
  }
  if (jpeg.size > encode_cap)
    return false;

  void* vaddr = nullptr;
  void* ahb_handle = nullptr;
  int lock_path = -1;
  if (!lock_blob(anb, handle, W, H, fence, vaddr, ahb_handle, lock_path))
  {
    int n = ++g_stat_blob_lock_fail;
    if (n <= 5 || (n % 60) == 0)
    {
      LOGW(OBF("blob lock failed (%dx%d cap=%zu fmt=0x%x ahb_lock=%p gbm_lock_v1=%p gbm_lock_v2=%p)").c_str(), W, H, buf_capacity, anb->format, (void*) g_ahb_lock, (void*) g_gbm_lock_v1, (void*) g_gbm_lock_v2);
    }
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
    LOGI(OBF("blob#%d substituted: %zu JPEG bytes (src %dx%d -> buf %dx%d) via=%s %s").c_str(), n, jpeg.size, src_w, src_h, W, H, lock_path == 0 ? OBF("AHB").c_str() : OBF("GBM").c_str(), cache_hit ? OBF("(cache hit)").c_str() : OBF("(re-encoded)").c_str());
  }
  return true;
}

static cr::camhook::ReplacementOutcome overwrite_nv21(ANWB* anb, int fence)
{
  ANWBInfo bi;
  if (!inspect_anwb(anb, bi))
    return cr::camhook::ReplacementOutcome::PassThrough;

  try_lazy_init();
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

  // Runtime buffer-mapper pointers are retried lazily because vendor DSOs may
  // appear after injection. Hold a shared lock for the complete lock/blit/
  // unlock operation so no retry publishes a new function set mid-callback.
  std::shared_lock<std::shared_mutex> runtime_ops(g_runtime_ops_mu);

  const int fmt = bi.format;

  if (fmt == 0x21)
  {
    const bool photo_on = refresh_photo_mode_();
    static std::atomic<int> blob_seen{0};
    int bn = ++blob_seen;
    if (bn <= 5 || (bn % 30) == 0)
    {
      LOGI(OBF("BLOB qbtc#%d %dx%d photo_mode=%s").c_str(), bn, bi.width, bi.height, photo_on ? "ON" : OBF("OFF").c_str());
    }
    if (photo_on)
      return overwrite_blob_jpeg(anb, fence)
                 ? cr::camhook::ReplacementOutcome::JpegWritten
                 : cr::camhook::ReplacementOutcome::AttemptedFailed;
    return cr::camhook::ReplacementOutcome::PassThrough;
  }

  if (fmt != 0x11 && fmt != 0x22 && fmt != 0x23)
  {
    ++g_stat_skip_fmt;
    if (fmt == 0x25)
      ++g_stat_skip_raw10;
    return cr::camhook::ReplacementOutcome::PassThrough;
  }

  if (access("/data/cr/video_mode", R_OK) != 0)
    return cr::camhook::ReplacementOutcome::PassThrough;
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
    locked = lock_via_gbm(anb, handle, W, H, fence, view);
  }
  if (!locked)
  {
    int n = ++g_stat_lock_fail;
    if (n <= 5 || (n % 60) == 0)
    {
      LOGW(OBF("lock failed (%dx%d fmt=0x%x handle=%p)").c_str(), W, H, fmt, handle);
    }
    return cr::camhook::ReplacementOutcome::AttemptedFailed;
  }

  bool replaced = false;
  if (view.y && view.vu)
  {
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
    blit_adaptive(source.data, (int) source.width, (int) source.height,
                  view.y, view.vu, W, H, (int) view.ystride,
                  (int) view.uvstride, view.uv_swapped ? 1 : 0);
    replaced = true;
  }
  else
  {
    ++g_stat_no_vu;
  }

  replaced = unlock(handle, ahb, view) && replaced;
  if (replaced)
    ++g_stat_blit_ok;
  return replaced ? cr::camhook::ReplacementOutcome::YuvWritten
                  : cr::camhook::ReplacementOutcome::AttemptedFailed;
}

using qbtc_surfaces_t = std::int32_t (*)(void* thiz, void* consumer, ANWB* anb,
                                        int fence, const std::vector<std::size_t>& surface_ids);
using qbtc_legacy_t = std::int32_t (*)(void* thiz, void* consumer, ANWB* anb,
                                      int fence);

static qbtc_surfaces_t g_orig_qbtc_surfaces = nullptr;
static qbtc_legacy_t g_orig_qbtc_legacy = nullptr;
static cr::camhook::HookCounters g_hook_counters;

static cr::camhook::ReplacementOutcome replace_for_queue(ANWB* anb, int fence)
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

  return overwrite_nv21(anb, fence);
}

static void report_queue_stats() noexcept
{
  const std::uint64_t n = g_hook_counters.callback_seen.load(std::memory_order_relaxed);
  if (n <= 5 || (n % 30) == 0)
    LOGI(OBF("camera_output v2 yuv=%d jpeg=%d raw10=%d unsupported=%d queued_yuv=%llu queued_jpeg=%llu queue_error=%llu").c_str(),
         g_stat_blit_ok.load(), g_stat_blob_ok.load(), g_stat_skip_raw10.load(),
         g_stat_skip_fmt.load(),
         (unsigned long long) g_hook_counters.yuv_queued.load(),
         (unsigned long long) g_hook_counters.jpeg_queued.load(),
         (unsigned long long) g_hook_counters.original_queue_failed.load());

  if ((n % 60) == 0)
  {
    LOGI(OBF("stats: callbacks=%llu attempted=%llu pass=%llu queue_failed=%llu skip_channel=%d skip_noframe=%d skip_stale=%d skip_noshm=%d skip_fmt=%d skip_lock=%d blit_ok=%d blob_ok=%d blob_cache_hit=%d noanb=%d layout=%d nogbm=%d dims=%d hnd=%d novu=%d").c_str(),
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
         g_stat_skip_handle.load(),
         g_stat_no_vu.load());
  }
}

static std::int32_t proxy_qbtc_surfaces(void* thiz, void* consumer, ANWB* anb,
                                        int fence, const std::vector<std::size_t>& surface_ids)
{
  const auto status = cr::camhook::dispatch_queue(
      g_hook_counters, [=] { return replace_for_queue(anb, fence); },
      g_orig_qbtc_surfaces, thiz, consumer, anb, fence, surface_ids);
  report_queue_stats();
  return status;
}

static std::int32_t proxy_qbtc_legacy(void* thiz, void* consumer, ANWB* anb,
                                      int fence)
{
  const auto status = cr::camhook::dispatch_queue(
      g_hook_counters, [=] { return replace_for_queue(anb, fence); },
      g_orig_qbtc_legacy, thiz, consumer, anb, fence);
  report_queue_stats();
  return status;
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
      void* original = nullptr;
      void* stub = shadowhook_hook_sym_name(lib, abi.symbol, proxy, &original);
      if (stub)
      {
        if (!original)
        {
          LOGE(OBF("hook returned no original target; rolling back").c_str());
          shadowhook_unhook(stub);
          continue;
        }
        if (abi.kind == cr::camhook::TargetAbiKind::QueueWithSurfaceIds)
          g_orig_qbtc_surfaces = reinterpret_cast<qbtc_surfaces_t>(original);
        else
          g_orig_qbtc_legacy = reinterpret_cast<qbtc_legacy_t>(original);
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
  open_shm();
  resolve_runtime_ops();

  int hook_rc = -1;
  if (try_hook_primary() == 0)
  {
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
