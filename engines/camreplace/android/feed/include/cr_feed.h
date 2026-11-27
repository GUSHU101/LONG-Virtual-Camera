#pragma once

// libcr_feed.so — public C entry point.
// Called by the standalone cr_feed_proc helper (and, in principle, by any
// other process that wants to seed /data/cr/feed). The library binds the
// adb-forwarded TCP port, accepts H.264 NAL units from the host, decodes
// them via NDK MediaCodec into NV21, and writes each decoded frame to the
// shm slot the camhook is reading.

#ifdef __cplusplus
extern "C"
{
#endif

  // Only one feed URL is supported:
  //   tcp_h264:listen:<port>   — accept H.264 from 127.0.0.1:<port>
  // Blocks until SIGTERM/SIGINT or an unrecoverable error. Returns 0 on
  // clean shutdown.
  int cr_feed_run(const char* feed_url);

// Version 3 keeps the v2 header and FHD slots, followed by UHD slots.
#define CR_FEED_SHM_VERSION_FHD 2
#define CR_FEED_SHM_VERSION 3

// NV21 remains the byte layout. These bits describe the decoded samples.
// A zero format is the original contract (BT.601 limited range).
#define CR_FEED_COLOR_BT709 1u
#define CR_FEED_COLOR_FULL_RANGE 2u
#define CR_FEED_COLOR_FLAGS (CR_FEED_COLOR_BT709 | CR_FEED_COLOR_FULL_RANGE)

#ifdef __cplusplus
}
#endif
