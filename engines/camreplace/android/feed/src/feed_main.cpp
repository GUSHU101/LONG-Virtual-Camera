// Phone-side source dispatcher. Local files and authenticated OBS/RTMP are
// adapters into one MediaCodec/shared-memory/hook runtime, not separate modes.

#include "../include/cr_feed.h"

#include <android/log.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <atomic>
#include <cctype>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include "util/Obf.h"

#define TAG "cr_feed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Shared with tcp_h264_feed.cpp — flipped by the SIGTERM/SIGINT handler so
// the receive loop unwinds cleanly on shutdown.
std::atomic<bool> g_stop{false};

namespace
{

void on_signal(int /*sig*/)
{
  g_stop.store(true);
}

bool parse_scheme(const char* url, const char* prefix, const char** out_path)
{
  if (!url || !prefix)
    return false;
  size_t plen = strlen(prefix);
  if (strncmp(url, prefix, plen) != 0)
    return false;
  *out_path = url + plen;
  return **out_path != '\0';
}

} // namespace

extern "C" int cr_feed_run_tcp_h264(int port); // tcp_h264_feed.cpp
extern "C" int cr_feed_run_tcp_nv21(int port); // compatibility receiver
extern "C" int cr_feed_run_tcp_pcm(int port);  // tcp_pcm_feed.cpp
extern "C" int cr_feed_run_file_video(const char* path);
extern "C" int cr_feed_run_file_nv21(const char* path);
extern "C" int cr_feed_run_file_audio(const char* path);
#if defined(CR_FEED_ENABLE_RTMP_DIRECT)
extern "C" int cr_feed_run_rtmp_pull(const char* url, int video_enabled, int audio_enabled);
#endif

namespace
{

#if defined(CR_FEED_ENABLE_RTMP_DIRECT)
bool read_obs_url(std::string& url)
{
  char config[4096];
  const int fd = open("/data/cr/control/source.conf",
                      O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct stat metadata{};
  if (fstat(fd, &metadata) < 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_uid != 0 || (metadata.st_mode & 0077) != 0)
  {
    close(fd);
    return false;
  }
  const ssize_t count = read(fd, config, sizeof(config));
  close(fd);
  if (count <= 0 || static_cast<size_t>(count) == sizeof(config))
    return false;
  const std::string_view text(config, static_cast<size_t>(count));
  constexpr std::string_view marker = "\nobs_url=";
  const size_t start = text.find(marker);
  if (start == std::string_view::npos)
    return false;
  const size_t value = start + marker.size();
  const size_t end = text.find('\n', value);
  if (end == std::string_view::npos || end == value || end - value > 512)
    return false;
  url = text.substr(value, end - value);
  return true;
}
#endif

bool trusted_local_media_path(const char* value, std::string& canonical)
{
  if (!value || *value == '\0')
    return false;
  char resolved[PATH_MAX] = {};
  if (!realpath(value, resolved))
    return false;
  std::string_view path(resolved);
  constexpr std::string_view current = "/data/cr/media/";
  constexpr std::string_view legacy = "/data/local/tmp/longvc-media/";
  if (!(path.size() > current.size() && path.starts_with(current)) &&
      !(path.size() > legacy.size() && path.starts_with(legacy)))
    return false;
  canonical.assign(resolved);
  return true;
}

} // namespace

extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_feed_run(const char* feed_url)
{
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);

  if (!feed_url)
  {
    LOGE(OBF("cr_feed_run: null URL").c_str());
    return -1;
  }
  // feed_url may contain the private OBS stream key or a local document
  // path. Never copy either value into logcat.
  LOGI(OBF("cr_feed_run: source accepted for parsing").c_str());

  try
  {
    const char* arg = nullptr;
    if (parse_scheme(feed_url, OBF("tcp_h264:listen:").c_str(), &arg))
    {
      int port = atoi(arg);
      if (port < 1 || port > 65535)
      {
        LOGE(OBF("tcp_h264:listen:<port> missing or invalid port").c_str());
        return -1;
      }
      return cr_feed_run_tcp_h264(port);
    }
    if (parse_scheme(feed_url, OBF("tcp_nv21:listen:").c_str(), &arg))
    {
      int port = atoi(arg);
      if (port < 1 || port > 65535)
      {
        LOGE(OBF("tcp_nv21:listen:<port> missing or invalid port").c_str());
        return -1;
      }
      return cr_feed_run_tcp_nv21(port);
    }
#if defined(CR_FEED_ENABLE_RTMP_DIRECT)
    if (parse_scheme(feed_url, "rtmp:pull:config:", &arg))
    {
      const std::string_view tracks(arg);
      const bool video = tracks == "video" || tracks == "av";
      const bool audio = tracks == "audio" || tracks == "av";
      std::string url;
      if ((!video && !audio) || !read_obs_url(url))
      {
        LOGE("rtmp: desktop playback configuration unavailable or not private");
        return -1;
      }
      return cr_feed_run_rtmp_pull(url.c_str(), video, audio);
    }
#endif
    if (parse_scheme(feed_url, OBF("tcp_pcm:listen:").c_str(), &arg))
    {
      int port = atoi(arg);
      if (port < 1 || port > 65535)
      {
        LOGE(OBF("tcp_pcm:listen:<port> missing or invalid port").c_str());
        return -1;
      }
      return cr_feed_run_tcp_pcm(port);
    }
    if (parse_scheme(feed_url, OBF("file_video:").c_str(), &arg))
    {
      std::string path;
      if (!trusted_local_media_path(arg, path))
      {
        LOGE(OBF("file_video source is outside the managed media directory").c_str());
        return -1;
      }
      return cr_feed_run_file_video(path.c_str());
    }
    if (parse_scheme(feed_url, OBF("file_nv21:").c_str(), &arg))
    {
      std::string path;
      if (!trusted_local_media_path(arg, path))
      {
        LOGE(OBF("file_nv21 source is outside the managed media directory").c_str());
        return -1;
      }
      return cr_feed_run_file_nv21(path.c_str());
    }
    if (parse_scheme(feed_url, OBF("file_audio:").c_str(), &arg))
    {
      std::string path;
      if (!trusted_local_media_path(arg, path))
      {
        LOGE(OBF("file_audio source is outside the managed media directory").c_str());
        return -1;
      }
      return cr_feed_run_file_audio(path.c_str());
    }

#if defined(CR_FEED_ENABLE_RTMP_DIRECT)
    LOGE(OBF("unsupported feed URL (local media, desktop RTMP playback, or compatibility tcp)").c_str());
#else
    LOGE(OBF("unsupported feed URL (local media or compatibility tcp)").c_str());
#endif
    return -1;
  }
  catch (const std::bad_alloc&)
  {
    LOGE(OBF("cr_feed_run: memory exhausted; channel revoked during unwind").c_str());
    return -1;
  }
  catch (const std::exception& e)
  {
    LOGE(OBF("cr_feed_run: exception: %s").c_str(), e.what());
    return -1;
  }
  catch (...)
  {
    LOGE(OBF("cr_feed_run: unknown exception").c_str());
    return -1;
  }
}
