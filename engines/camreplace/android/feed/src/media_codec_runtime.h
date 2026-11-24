#pragma once

#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>

#include <mutex>

#include "util/Obf.h"

namespace cr_feed
{

// ReCam explicitly starts binder_ndk's process pool before synchronous
// MediaCodec use. Some vendor codecs appear to configure successfully but
// never return output without it. Keep one process-wide pool for local files,
// OBS H.264 and OBS AAC rather than letting each decoder guess independently.
inline bool ensure_media_codec_binder_pool()
{
  static std::once_flag once;
  static bool available = false;
  std::call_once(once, []
  {
    void* binder = dlopen(OBF("libbinder_ndk.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!binder)
    {
      __android_log_print(ANDROID_LOG_WARN, "cr_feed",
                          "MediaCodec binder runtime is unavailable");
      return;
    }
    auto set_max = reinterpret_cast<bool (*)(uint32_t)>(
        dlsym(binder,
              OBF("ABinderProcess_setThreadPoolMaxThreadCount").c_str()));
    auto start = reinterpret_cast<void (*)()>(
        dlsym(binder, OBF("ABinderProcess_startThreadPool").c_str()));
    if (!start)
    {
      __android_log_print(ANDROID_LOG_WARN, "cr_feed",
                          "MediaCodec binder thread-pool symbol is unavailable");
      return;
    }
    if (set_max) (void) set_max(4);
    start();
    available = true;
  });
  return available;
}

} // namespace cr_feed
