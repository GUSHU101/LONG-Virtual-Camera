// Fast CPU bilinear NV21 blit.
// Use endpoint-aligned Q16 coordinates and truncate after each pass.
// Luma and both chroma channels use the same two-pass filter.

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

int cr_nv21_zoom_crop(int src_w, int src_h, int dst_w, int dst_h,
                        unsigned zoom_q10, cr_nv21_crop_rect* out)
{
  if (!cr_nv21_center_crop(src_w, src_h, dst_w, dst_h, out))
    return 0;
  if (zoom_q10 <= CR_NV21_ZOOM_ONE)
    return 1;
  const int w = (int) ((uint64_t) out->width * CR_NV21_ZOOM_ONE / zoom_q10) & ~1;
  const int h = (int) ((uint64_t) out->height * CR_NV21_ZOOM_ONE / zoom_q10) & ~1;
  if (w < 2 || h < 2)
    return 0;
  out->x += ((out->width - w) / 2) & ~1;
  out->y += ((out->height - h) / 2) & ~1;
  out->width = w;
  out->height = h;
  return 1;
}

static inline uint8_t blend(uint8_t a, uint8_t b, uint32_t fx_q16)
{
  // fx in [0,65535], blend = a + (b-a)*fx>>16.
  int32_t d = (int32_t) b - (int32_t) a;
  return (uint8_t) ((int32_t) a + ((d * (int32_t) fx_q16) >> 16));
}

// Match the destination width cap in the public blit and camera buffer checks.
#define CR_BLIT_MAX_DST_W 8192

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define CR_BLIT_NEON 1
#endif

typedef struct
{
  uint16_t first;
  uint16_t second;
  uint16_t fraction;
} AxisSample;

#ifdef CR_BLIT_NEON
typedef struct
{
  uint16_t offset;
  uint8_t first[8];
  uint8_t second[8];
  uint16_t fraction[8];
} HorizontalBlock;
#endif

typedef struct
{
  AxisSample columns[CR_BLIT_MAX_DST_W];
#ifdef CR_BLIT_NEON
  HorizontalBlock blocks[CR_BLIT_MAX_DST_W / 8];
  int block_count;
#endif
} HorizontalPlan;

typedef enum
{
  CopyOrder, // Luma or interleaved VU
  SwapPairs, // Interleaved UV
  SplitPairs // Separate V and U destinations
} OutputOrder;

static uint32_t axis_step(int source, int dest)
{
  return dest > 1 ? (uint32_t) (((uint64_t) (source - 1) << 16) /
                                (uint32_t) (dest - 1)) : 0;
}

static AxisSample axis_sample(int pos, int last_dest, int last_source,
                              uint32_t step)
{
  const uint32_t coordinate = (uint32_t) pos * step;
  const uint32_t index = coordinate >> 16;
  if (pos == last_dest || index >= (uint32_t) last_source)
  {
    const uint16_t end = (uint16_t) last_source;
    return (AxisSample) {end, end, 0};
  }
  return (AxisSample) {(uint16_t) index, (uint16_t) (index + 1),
                       (uint16_t) coordinate};
}

#ifdef CR_BLIT_NEON
static inline uint16x8_t blend_eight(uint16x8_t a, uint16x8_t b,
                                     uint16x8_t wa, uint16x8_t wb)
{
  uint32x4_t low = vmull_u16(vget_low_u16(a), vget_low_u16(wa));
  uint32x4_t high = vmull_high_u16(a, wa);
  low = vmlal_u16(low, vget_low_u16(b), vget_low_u16(wb));
  high = vmlal_high_u16(high, b, wb);
  return vcombine_u16(vshrn_n_u32(low, 16), vshrn_n_u32(high, 16));
}

static void prepare_horizontal_blocks(HorizontalPlan* plan, int src_w,
                                       int dst_w, int channels)
{
  const int available = src_w * channels;
  plan->block_count = 0;
  if (dst_w < src_w || available < 16)
    return;
  for (int x = 0; x <= dst_w * channels - 8; x += 8)
  {
    HorizontalBlock* block = &plan->blocks[x / 8];
    const int first = plan->columns[x / channels].first * channels;
    const int last = plan->columns[(x + 7) / channels].second * channels + channels - 1;
    const int offset = first < available - 16 ? first : available - 16;
    if (last - offset >= 16)
      return; // The scalar path handles any span wider than a table.
    block->offset = (uint16_t) offset;
    for (int lane = 0; lane < 8; ++lane)
    {
      const AxisSample tap = plan->columns[(x + lane) / channels];
      const int channel = lane % channels;
      block->first[lane] = (uint8_t) (tap.first * channels + channel - offset);
      block->second[lane] = (uint8_t) (tap.second * channels + channel - offset);
      block->fraction[lane] = tap.fraction;
    }
  }
  plan->block_count = dst_w * channels / 8;
}
#endif

static void horizontal_row(const uint8_t* source, uint8_t* scaled,
                           const HorizontalPlan* plan, int width, int channels)
{
  int x = 0;
#ifdef CR_BLIT_NEON
  for (int i = 0; i < plan->block_count; ++i)
  {
    const HorizontalBlock* block = &plan->blocks[i];
    const uint8x16_t table = vld1q_u8(source + block->offset);
    const uint16x8_t first = vmovl_u8(vqtbl1_u8(table, vld1_u8(block->first)));
    const uint16x8_t second = vmovl_u8(vqtbl1_u8(table, vld1_u8(block->second)));
    const uint16x8_t fraction = vld1q_u16(block->fraction);
    const uint16x8_t zero = vdupq_n_u16(0);
    // 65536 does not fit in 16 bits. Use the first tap for exact samples.
    const uint16x8_t blended =
        blend_eight(first, second, vsubq_u16(zero, fraction), fraction);
    vst1_u8(scaled + i * 8, vmovn_u16(vbslq_u16(
        vceqq_u16(fraction, zero), first, blended)));
  }
  x = plan->block_count * 8 / channels;
#endif
  if (channels == 1)
  {
    for (; x < width; ++x)
    {
      const AxisSample s = plan->columns[x];
      scaled[x] = blend(source[s.first], source[s.second], s.fraction);
    }
    return;
  }
  for (; x < width; ++x)
  {
    const AxisSample s = plan->columns[x];
    const uint8_t* first = source + 2 * s.first;
    const uint8_t* second = source + 2 * s.second;
    scaled[2 * x] = blend(first[0], second[0], s.fraction);
    scaled[2 * x + 1] = blend(first[1], second[1], s.fraction);
  }
}

static void write_row(const uint8_t* top, const uint8_t* bottom,
                      uint8_t* dest, uint8_t* dest_u, int bytes,
                      OutputOrder order, uint16_t fraction)
{
  if (fraction == 0 && order == CopyOrder)
  {
    memcpy(dest, top, (size_t) bytes);
    return;
  }
  int x = 0;
#ifdef CR_BLIT_NEON
  const uint16x8_t wa = vdupq_n_u16((uint16_t) (65536u - fraction));
  const uint16x8_t wb = vdupq_n_u16(fraction);
  for (; x <= bytes - 16; x += 16)
  {
    uint8x16_t values = vld1q_u8(top + x);
    if (fraction != 0)
    {
      const uint8x16_t other = vld1q_u8(bottom + x);
      // a + ((b-a)*fraction >> 16) is exactly the same truncated Q16 blend.
      const uint16x8_t low = blend_eight(vmovl_u8(vget_low_u8(values)),
          vmovl_u8(vget_low_u8(other)), wa, wb);
      const uint16x8_t high = blend_eight(vmovl_high_u8(values),
          vmovl_high_u8(other), wa, wb);
      values = vcombine_u8(vmovn_u16(low), vmovn_u16(high));
    }
    if (order == SplitPairs)
    {
      const uint8x8_t low = vget_low_u8(values);
      const uint8x8_t high = vget_high_u8(values);
      vst1_u8(dest + x / 2, vuzp1_u8(low, high));
      vst1_u8(dest_u + x / 2, vuzp2_u8(low, high));
    }
    else
    {
      vst1q_u8(dest + x, order == SwapPairs ? vrev16q_u8(values) : values);
    }
  }
#endif
  if (order == CopyOrder)
  {
    for (; x < bytes; ++x)
      dest[x] = blend(top[x], bottom[x], fraction);
    return;
  }
  for (; x < bytes; x += 2)
  {
    uint8_t v = blend(top[x], bottom[x], fraction);
    uint8_t u = blend(top[x + 1], bottom[x + 1], fraction);
    if (order == SplitPairs)
    {
      dest[x / 2] = v;
      dest_u[x / 2] = u;
    }
    else
    {
      dest[x] = u;
      dest[x + 1] = v;
    }
  }
}

// Cache the horizontal results of the two source rows used by the vertical
// blend. Successive upscaled rows reuse them. Use per-call scratch so different
// camera streams and feed threads can scale at the same time.
static void scale_bilinear(const uint8_t* src, int src_w, int src_h,
                           int src_stride, uint8_t* dest, uint8_t* dest_u,
                           int dst_w, int dst_h, int dst_stride,
                           int channels, OutputOrder order)
{
  HorizontalPlan plan;
  uint8_t scratch[2][CR_BLIT_MAX_DST_W];
  const uint32_t step_x = axis_step(src_w, dst_w);
  const uint32_t step_y = axis_step(src_h, dst_h);
  for (int x = 0; x < dst_w; ++x)
    plan.columns[x] = axis_sample(x, dst_w - 1, src_w - 1, step_x);
#ifdef CR_BLIT_NEON
  prepare_horizontal_blocks(&plan, src_w, dst_w, channels);
#endif

  int first = 0;
  int cached[2] = {-1, -1};
  for (int y = 0; y < dst_h; ++y)
  {
    const AxisSample s = axis_sample(y, dst_h - 1, src_h - 1, step_y);
    if (cached[1 - first] == s.first)
      first = 1 - first;
    if (cached[first] != s.first)
    {
      horizontal_row(src + (size_t) s.first * src_stride, scratch[first],
                     &plan, dst_w, channels);
      cached[first] = s.first;
    }
    const int second = s.fraction != 0 ? 1 - first : first;
    if (cached[second] != s.second && s.fraction != 0)
    {
      horizontal_row(src + (size_t) s.second * src_stride, scratch[second],
                     &plan, dst_w, channels);
      cached[second] = s.second;
    }

    const size_t offset = (size_t) y * dst_stride;
    write_row(scratch[first], scratch[second], dest + offset,
              dest_u ? dest_u + offset : NULL, dst_w * channels,
              order, s.fraction);
  }
}

static void blit_nv21(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y,
                      uint8_t* dst_chroma, uint8_t* dst_u, int dst_w, int dst_h,
                      int dst_y_stride, int dst_uv_stride, OutputOrder order,
                      unsigned zoom_q10)
{
  if (!src || !dst_y || !dst_chroma || (order == SplitPairs && !dst_u) ||
      src_w > 8192 || src_h > 8192 || dst_w > CR_BLIT_MAX_DST_W || dst_h > 8192 ||
      dst_y_stride < dst_w || dst_uv_stride < (order == SplitPairs ? dst_w / 2 : dst_w) ||
      (uint64_t) src_w * (uint64_t) src_h > 32ull * 1024ull * 1024ull ||
      (uint64_t) dst_w * (uint64_t) dst_h > 32ull * 1024ull * 1024ull)
    return;
  cr_nv21_crop_rect crop;
  if (!cr_nv21_zoom_crop(src_w, src_h, dst_w, dst_h, zoom_q10, &crop))
    return;

  const uint8_t* src_y = src + (size_t) crop.y * src_w + crop.x;
  const uint8_t* src_vu = src + (size_t) src_w * src_h +
                          (size_t) (crop.y / 2) * src_w + crop.x;

  // With no scaling, copy the visible luma and preserve all row padding.
  if (crop.width == dst_w && crop.height == dst_h)
  {
    if (src_w == dst_w && dst_y_stride == dst_w)
      memcpy(dst_y, src_y, (size_t) dst_w * dst_h);
    else
    {
      for (int y = 0; y < dst_h; ++y)
        memcpy(dst_y + (size_t) y * dst_y_stride,
               src_y + (size_t) y * src_w, (size_t) dst_w);
    }
    if (order == CopyOrder && src_w == dst_w && dst_uv_stride == dst_w)
    {
      memcpy(dst_chroma, src_vu, (size_t) dst_w * (dst_h / 2));
      return;
    }
    for (int y = 0; y < dst_h / 2; ++y)
    {
      const uint8_t* source_row = src_vu + (size_t) y * src_w;
      const size_t offset = (size_t) y * dst_uv_stride;
      write_row(source_row, source_row, dst_chroma + offset,
                dst_u ? dst_u + offset : NULL, dst_w, order, 0);
    }
    return;
  }

  // ReCam's useful resolution behaviour is retained here: every camera
  // output is served at exactly its requested size, but the source is first
  // centre-cropped to the target aspect ratio. This avoids the old stretched
  // faces while still filling preview, recording and photo buffers fully.
  scale_bilinear(src_y, crop.width, crop.height, src_w,
                  dst_y, NULL, dst_w, dst_h, dst_y_stride, 1, CopyOrder);
  scale_bilinear(src_vu, crop.width / 2, crop.height / 2, src_w,
                  dst_chroma, dst_u, dst_w / 2, dst_h / 2, dst_uv_stride,
                  2, order);
}

void cr_nv21_blit(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y,
                  uint8_t* dst_vu, int dst_w, int dst_h, int dst_y_stride,
                  int dst_uv_stride, int dst_uv_swapped)
{
  cr_nv21_blit_zoom(src, src_w, src_h, dst_y, dst_vu, dst_w, dst_h,
                   dst_y_stride, dst_uv_stride, dst_uv_swapped, CR_NV21_ZOOM_ONE);
}

void cr_nv21_blit_zoom(const uint8_t* src, int src_w, int src_h,
                       uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h,
                       int dst_y_stride, int dst_uv_stride, int dst_uv_swapped,
                       unsigned zoom_q10)
{
  blit_nv21(src, src_w, src_h, dst_y, dst_vu, NULL, dst_w, dst_h,
            dst_y_stride, dst_uv_stride, dst_uv_swapped ? SwapPairs : CopyOrder, zoom_q10);
}

void cr_nv21_blit_planar(const uint8_t* src, int src_w, int src_h,
                         uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v,
                         int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride)
{
  cr_nv21_blit_planar_zoom(src, src_w, src_h, dst_y, dst_u, dst_v, dst_w, dst_h,
                          dst_y_stride, dst_uv_stride, CR_NV21_ZOOM_ONE);
}

void cr_nv21_blit_planar_zoom(const uint8_t* src, int src_w, int src_h,
                              uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v,
                              int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride,
                              unsigned zoom_q10)
{
  blit_nv21(src, src_w, src_h, dst_y, dst_v, dst_u, dst_w, dst_h,
            dst_y_stride, dst_uv_stride, SplitPairs, zoom_q10);
}
