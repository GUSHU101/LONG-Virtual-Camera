// Fast CPU bilinear NV21 blit.
// Integer math with 16.16 fixed-point interpolators keeps it branch-lean:
//   step_x = (src_w << 16) / dst_w     (Q16)
//   for each dst row y:
//     sy = (y * src_h) / dst_h
//     for each dst col x (Q16 accumulator):
//       sx = accumulator >> 16
//       fx = accumulator & 0xffff
//       sample <- blend(src[sy][sx], src[sy][sx+1], fx)
// UV plane is half-res → same math with dst_w/2 and dst_h/2.

#include "nv21_blit.h"
#include <string.h>

int cr_nv21_center_crop(int src_w, int src_h, int dst_w, int dst_h,
                        cr_nv21_crop_rect* out)
{
  if (!out || src_w < 2 || src_h < 2 || dst_w < 2 || dst_h < 2 ||
      (src_w & 1) || (src_h & 1) || (dst_w & 1) || (dst_h & 1))
    return 0;

  int crop_w = src_w;
  int crop_h = src_h;
  if ((int64_t) src_w * dst_h > (int64_t) dst_w * src_h)
    crop_w = (int) ((int64_t) src_h * dst_w / dst_h);
  else if ((int64_t) src_w * dst_h < (int64_t) dst_w * src_h)
    crop_h = (int) ((int64_t) src_w * dst_h / dst_w);

  crop_w &= ~1;
  crop_h &= ~1;
  if (crop_w < 2 || crop_h < 2)
    return 0;
  out->x = ((src_w - crop_w) / 2) & ~1;
  out->y = ((src_h - crop_h) / 2) & ~1;
  out->width = crop_w;
  out->height = crop_h;
  return 1;
}

static inline uint8_t blend(uint8_t a, uint8_t b, uint32_t fx_q16)
{
  // fx in [0,65535], blend = a + (b-a)*fx>>16.
  int32_t d = (int32_t) b - (int32_t) a;
  return (uint8_t) ((int32_t) a + ((d * (int32_t) fx_q16) >> 16));
}

// Must be ≥ the `W > 8192` guard in hook_main.cpp::overwrite_nv21. A Redmi
// Note 8T in 48 MP still capture mode hands us an 8000-px-wide buffer —
// the original 4096 stack arrays here overflowed, tripped
// `-fstack-protector`, and SIGABRT'd cameraserver. Bumped to 8192.
#define CR_BLIT_MAX_DST_W 8192

static void scale_plane_bilinear(const uint8_t* src, int src_w, int src_h, int src_stride, uint8_t* dst, int dst_w, int dst_h, int dst_stride)
{
  if (dst_w <= 0 || dst_w > CR_BLIT_MAX_DST_W)
    return;
  // Need at least 2x2 source samples for bilinear taps. Anything smaller
  // would force `sx+1` past the buffer and read 1 byte off the end into
  // the chroma plane — see the bug we hit at right-edge clamp.
  if (src_w < 2 || src_h < 2)
    return;

  // Map the first and last destination pixels to the first and last source
  // pixels. The old src/dst step plus an artificial 0xffff edge fraction
  // rounded a positive one-byte gradient down, duplicating the penultimate
  // pixel. ReCam uses the same endpoint-aligned mapping.
  const uint32_t step_x = dst_w > 1
      ? (uint32_t) (((uint64_t) (src_w - 1) << 16) /
                    (uint32_t) (dst_w - 1))
      : 0;
  const uint32_t step_y = dst_h > 1
      ? (uint32_t) (((uint64_t) (src_h - 1) << 16) /
                    (uint32_t) (dst_h - 1))
      : 0;

  // Per-column precomputed source x + fractional offset on the stack.
  // 64 KB combined; cameraserver pthreads have at least 1 MB stack so
  // this is comfortably safe. We tried `static __thread` but bionic's
  // lazy DTV allocation for dlopen'd-into-already-running-process libs
  // is fragile for large TLS sections (>= 64 KB) — observed cameraserver
  // SIGSEGV on first proxy call into the blit when TLS-allocated.
  // Stack is the boring-but-bulletproof choice.
  // The final destination column is explicitly pinned to the final source
  // sample. All other columns retain a clamped second tap.
  uint32_t cols_sx[CR_BLIT_MAX_DST_W];
  uint32_t cols_fx[CR_BLIT_MAX_DST_W];
  for (int x = 0; x < dst_w; ++x)
  {
    uint32_t ax = x * step_x;
    uint32_t sx = ax >> 16;
    uint32_t fx = ax & 0xffffu;
    if (x == dst_w - 1 || sx >= (uint32_t) (src_w - 1))
    {
      sx = (uint32_t) (src_w - 1);
      fx = 0;
    }
    cols_sx[x] = sx;
    cols_fx[x] = fx;
  }

  for (int y = 0; y < dst_h; ++y)
  {
    uint32_t ay = y * step_y;
    uint32_t sy = ay >> 16;
    uint32_t fy = ay & 0xffffu;
    // Same clamp applies vertically — sy+1 must stay inside the plane.
    if (y == dst_h - 1 || sy >= (uint32_t) (src_h - 1))
    {
      sy = (uint32_t) (src_h - 1);
      fy = 0;
    }

    const uint8_t* r0 = src + (size_t) sy * src_stride;
    const uint32_t sy1 = sy + 1u < (uint32_t) src_h ? sy + 1u : sy;
    const uint8_t* r1 = src + (size_t) sy1 * src_stride;
    uint8_t* drow = dst + (size_t) y * dst_stride;

    for (int x = 0; x < dst_w; ++x)
    {
      uint32_t sx = cols_sx[x];
      uint32_t fx = cols_fx[x];
      uint32_t sx1 = sx + 1u < (uint32_t) src_w ? sx + 1u : sx;
      uint8_t p00 = r0[sx];
      uint8_t p01 = r0[sx1];
      uint8_t p10 = r1[sx];
      uint8_t p11 = r1[sx1];
      uint8_t top = blend(p00, p01, fx);
      uint8_t bot = blend(p10, p11, fx);
      drow[x] = blend(top, bot, fy);
    }
  }
}

// The VU plane is two interleaved chroma bytes per sample, so scale them as
// a single 16-bit quantity (conceptually one "VU pixel"). `dst_swap` flips
// the two bytes per pair — use it when the destination is NV12 (U first)
// while our source is NV21 (V first).
static void scale_vu_bilinear(const uint8_t* src, int src_w_pairs, int src_h,
                              int src_stride_bytes, uint8_t* dst,
                              int dst_w_pairs, int dst_h,
                              int dst_stride_bytes, int dst_swap)
{
  // Treat the source and destination as arrays of VU pairs (2 bytes each).
  // Source stride is src_w_pairs*2 bytes; dst stride is dst_stride_bytes.
  if (dst_w_pairs <= 0 || dst_w_pairs > CR_BLIT_MAX_DST_W)
    return;
  if (src_w_pairs < 2 || src_h < 2)
    return;

  const uint32_t step_x = dst_w_pairs > 1
      ? (uint32_t) (((uint64_t) (src_w_pairs - 1) << 16) /
                    (uint32_t) (dst_w_pairs - 1))
      : 0;
  const uint32_t step_y = dst_h > 1
      ? (uint32_t) (((uint64_t) (src_h - 1) << 16) /
                    (uint32_t) (dst_h - 1))
      : 0;

  uint32_t cols_sx[CR_BLIT_MAX_DST_W];
  uint32_t cols_fx[CR_BLIT_MAX_DST_W];
  for (int x = 0; x < dst_w_pairs; ++x)
  {
    uint32_t ax = x * step_x;
    uint32_t sx = ax >> 16;
    uint32_t fx = ax & 0xffffu;
    if (x == dst_w_pairs - 1 || sx >= (uint32_t) (src_w_pairs - 1))
    {
      sx = (uint32_t) (src_w_pairs - 1);
      fx = 0;
    }
    cols_sx[x] = sx;
    cols_fx[x] = fx;
  }

  const int off_v = dst_swap ? 1 : 0;
  const int off_u = dst_swap ? 0 : 1;

  for (int y = 0; y < dst_h; ++y)
  {
    uint32_t ay = y * step_y;
    uint32_t sy = ay >> 16;
    uint32_t fy = ay & 0xffffu;
    if (y == dst_h - 1 || sy >= (uint32_t) (src_h - 1))
    {
      sy = (uint32_t) (src_h - 1);
      fy = 0;
    }

    const uint8_t* r0 = src + (size_t) sy * src_stride_bytes;
    const uint32_t sy1 = sy + 1u < (uint32_t) src_h ? sy + 1u : sy;
    const uint8_t* r1 = src + (size_t) sy1 * src_stride_bytes;
    uint8_t* drow = dst + (size_t) y * dst_stride_bytes;

    for (int x = 0; x < dst_w_pairs; ++x)
    {
      uint32_t sx = cols_sx[x];
      uint32_t fx = cols_fx[x];
      uint32_t sx1 =
          sx + 1u < (uint32_t) src_w_pairs ? sx + 1u : sx;
      uint8_t v00 = r0[sx * 2 + 0], u00 = r0[sx * 2 + 1];
      uint8_t v01 = r0[sx1 * 2 + 0], u01 = r0[sx1 * 2 + 1];
      uint8_t v10 = r1[sx * 2 + 0], u10 = r1[sx * 2 + 1];
      uint8_t v11 = r1[sx1 * 2 + 0], u11 = r1[sx1 * 2 + 1];
      uint8_t vT = blend(v00, v01, fx), vB = blend(v10, v11, fx);
      uint8_t uT = blend(u00, u01, fx), uB = blend(u10, u11, fx);
      drow[x * 2 + off_v] = blend(vT, vB, fy);
      drow[x * 2 + off_u] = blend(uT, uB, fy);
    }
  }
}

void cr_nv21_blit(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride, int dst_uv_swapped)
{
  if (!src || !dst_y || !dst_vu || src_w > 8192 || src_h > 8192 ||
      dst_w > 8192 || dst_h > 8192 || dst_y_stride < dst_w ||
      dst_uv_stride < dst_w ||
      (uint64_t) src_w * (uint64_t) src_h > 32ull * 1024ull * 1024ull ||
      (uint64_t) dst_w * (uint64_t) dst_h > 32ull * 1024ull * 1024ull)
    return;
  cr_nv21_crop_rect crop;
  if (!cr_nv21_center_crop(src_w, src_h, dst_w, dst_h, &crop))
    return;

  // Stride-aware equal-resolution fast path. The original gate required
  // `dst_y_stride == dst_w && dst_uv_stride == dst_w` which is rarely
  // true on Qualcomm gralloc — it pads strides up to 64-byte boundaries
  // (e.g. dst_w=1280 → dst_y_stride=1344). The padded case used to
  // fall through to the bilinear scaler even though source and dest
  // dimensions matched, wasting CPU on a no-op resize.
  // New gate: same resolution, no chroma swap. Per-row memcpy works
  // regardless of how big the stride padding is — we just memcpy
  // `dst_w` (or `dst_w` for the VU pair = 2 * (dst_w/2)) bytes per row
  // and skip the rest of the stride padding.
  if (crop.width == dst_w && crop.height == dst_h)
  {
    for (int y = 0; y < dst_h; ++y)
    {
      memcpy(dst_y + (size_t) y * dst_y_stride,
             src + (size_t) (crop.y + y) * src_w + crop.x,
             (size_t) dst_w);
    }
    const uint8_t* src_vu = src + (size_t) src_w * src_h +
                            (size_t) (crop.y / 2) * src_w + crop.x;
    for (int y = 0; y < dst_h / 2; ++y)
    {
      const uint8_t* source_row = src_vu + (size_t) y * src_w;
      uint8_t* destination_row = dst_vu + (size_t) y * dst_uv_stride;
      if (!dst_uv_swapped)
        memcpy(destination_row, source_row, (size_t) dst_w);
      else
        for (int x = 0; x < dst_w; x += 2)
        {
          destination_row[x] = source_row[x + 1];
          destination_row[x + 1] = source_row[x];
        }
    }
    return;
  }

  // ReCam's useful resolution behaviour is retained here: every camera
  // output is served at exactly its requested size, but the source is first
  // centre-cropped to the target aspect ratio. This avoids the old stretched
  // faces while still filling preview, recording and photo buffers fully.
  const uint8_t* src_y = src + (size_t) crop.y * src_w + crop.x;
  scale_plane_bilinear(src_y, crop.width, crop.height, src_w,
                       dst_y, dst_w, dst_h, dst_y_stride);

  // VU plane — half the pixel count horizontally/vertically, 2 bytes each.
  const uint8_t* src_vu = src + (size_t) src_w * src_h +
                          (size_t) (crop.y / 2) * src_w + crop.x;
  scale_vu_bilinear(src_vu, crop.width / 2, crop.height / 2, src_w,
                    dst_vu, dst_w / 2, dst_h / 2, dst_uv_stride,
                    dst_uv_swapped);
}
