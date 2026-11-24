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

#include <atomic>
#include <cctype>
#include <exception>
#include <new>
#include <string>
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
extern "C" int cr_feed_run_rtmp(int port, const char* stream_key,
                                int allow_lan, int enable_video,
                                int enable_audio);
#endif

namespace
{

bool read_obs_key(std::string& key)
{
  char config[4096];
  const int fd = open("/data/cr/control/source.conf",
                      O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return false;
  const ssize_t count = read(fd, config, sizeof(config));
  close(fd);
  if (count <= 0 || static_cast<size_t>(count) == sizeof(config))
    return false;
  const std::string text(config, static_cast<size_t>(count));
  const std::string marker = "\nobs_token=";
  const size_t start = text.find(marker);
  if (start == std::string::npos)
    return false;
  const size_t value = start + marker.size();
  const size_t end = text.find('\n', value);
  if (end == std::string::npos || end - value != 64)
    return false;
  key = text.substr(value, end - value);
  return true;
}

bool parse_rtmp_endpoint(const char* value, int& port, std::string& stream_key,
                         bool& allow_lan, bool& enable_video,
                         bool& enable_audio)
{
  if (!value)
    return false;
  const char* separator = strchr(value, ':');
  if (!separator || separator == value || separator[1] == '\0')
    return false;
  std::string port_text(value, static_cast<size_t>(separator - value));
  if (port_text.size() > 5)
    return false;
  for (char c : port_text)
    if (!std::isdigit(static_cast<unsigned char>(c)))
      return false;
  port = atoi(port_text.c_str());
  const char* key_begin = separator + 1;
  const char* key_end = strchr(key_begin, ':');
  if (!key_end)
  {
    // Compatibility with early phone-only payloads; the secure default is
    // loopback and both tracks are accepted.
    stream_key.assign(key_begin);
    allow_lan = false;
    enable_video = true;
    enable_audio = true;
  }
  else
  {
    stream_key.assign(key_begin, static_cast<size_t>(key_end - key_begin));
    const char* bind_begin = key_end + 1;
    const char* bind_end = strchr(bind_begin, ':');
    if (!bind_end || bind_end == bind_begin || bind_end[1] == '\0' ||
        strchr(bind_end + 1, ':'))
      return false;
    const std::string bind_mode(bind_begin,
                                static_cast<size_t>(bind_end - bind_begin));
    const std::string tracks(bind_end + 1);
    if (bind_mode == OBF("loopback").c_str())
      allow_lan = false;
    else if (bind_mode == OBF("lan").c_str())
      allow_lan = true;
    else
      return false;
    enable_video = tracks == OBF("video").c_str() ||
                   tracks == OBF("av").c_str();
    enable_audio = tracks == OBF("audio").c_str() ||
                   tracks == OBF("av").c_str();
    if (!enable_video && !enable_audio)
      return false;
  }
  if (stream_key == "config" && !read_obs_key(stream_key))
    return false;
  if (port < 1 || port > 65535 || stream_key.size() < 24 ||
      stream_key.size() > 96)
    return false;
  for (char c : stream_key)
  {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!std::isalnum(u) && c != '-' && c != '_')
      return false;
  }
  return true;
}

bool trusted_local_media_path(const char* value, std::string& canonical)
{
  if (!value || *value == '\0')
    return false;
  char resolved[PATH_MAX] = {};
  if (!realpath(value, resolved))
    return false;
  constexpr const char* root = "/data/local/tmp/longvc-media/";
  const size_t root_length = strlen(root);
  if (strncmp(resolved, root, root_length) != 0 || resolved[root_length] == '\0')
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
    if (parse_scheme(feed_url, OBF("rtmp:listen:").c_str(), &arg))
    {
      int port = 0;
      std::string stream_key;
      bool allow_lan = false;
      bool enable_video = false;
      bool enable_audio = false;
      if (!parse_rtmp_endpoint(arg, port, stream_key, allow_lan,
                               enable_video, enable_audio))
      {
        LOGE(OBF("rtmp endpoint is invalid").c_str());
        return -1;
      }
      return cr_feed_run_rtmp(port, stream_key.c_str(), allow_lan ? 1 : 0,
                              enable_video ? 1 : 0,
                              enable_audio ? 1 : 0);
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
    LOGE(OBF("unsupported feed URL (local media, authenticated rtmp, or compatibility tcp)").c_str());
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
