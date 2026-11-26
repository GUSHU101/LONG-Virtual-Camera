// NV21 → JPEG encoder used by photo-mode BLOB substitution.
// We bundle stb_image_write (single-header, public domain). It's about
// 70KB of source and adds ~50KB to the stripped libcr_camhook.so. No
// external link dependency, which matters because cameraserver's
// process is locked down — bringing in libjpeg.so via dlopen works on
// most Androids but is fragile across vendor builds.
// Pipeline per encode call:
//   1. Allocate w*h*3 byte RGB scratch on heap (~6 MB at 1080p, ~36 MB
//      at 4K — fine inside cameraserver, freed immediately).
//   2. NV21 → RGB888 conversion (BT.601 limited-range Y'CbCr math).
//   3. Encode into a bounded scratch buffer. The camera buffer remains
//      untouched until the caller has a complete JPEG.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
// We don't need most of the formats — drop everything except JPEG to
// shave the binary. PNG/BMP/TGA/HDR exports out, JPEG stays in.
#define STBIW_ASSERT(x) ((void) 0)
#include "../../../third_party/stb/stb_image_write.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg_encode.h"
struct EncodeCtx
{
  uint8_t* dst;
  size_t allocated;
  size_t limit;
  size_t off;
  int error;
};

static void write_cb(void* user, void* data, int size)
{
  struct EncodeCtx* ctx = (struct EncodeCtx*) user;
  if (ctx->error || size <= 0)
    return;
  if ((size_t) size > ctx->limit - ctx->off)
  {
    ctx->error = -3;
    return;
  }
  const size_t needed = ctx->off + (size_t) size;
  if (needed > ctx->allocated)
  {
    size_t capacity = ctx->allocated ? ctx->allocated : 1024;
    while (capacity < needed)
      capacity = capacity > ctx->limit / 2 ? ctx->limit : capacity * 2;
    uint8_t* grown = (uint8_t*) realloc(ctx->dst, capacity);
    if (!grown)
    {
      ctx->error = -2;
      return;
    }
    ctx->dst = grown;
    ctx->allocated = capacity;
  }
  memcpy(ctx->dst + ctx->off, data, (size_t) size);
  ctx->off += (size_t) size;
}

static inline int clampi(int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// NV21 → RGB888. NV21 is Y plane (sw*sh bytes) followed by interleaved
// V,U pairs at half-resolution ((sw/2)*(sh/2) pairs * 2 bytes). The
// chroma row index is y/2; the chroma byte pair starts at (x/2)*2.
static void nv21_to_rgb24(const uint8_t* nv21, int sw, int sh, uint8_t* rgb)
{
  const uint8_t* y_plane = nv21;
  const uint8_t* vu_plane = nv21 + (size_t) sw * sh;
  for (int y = 0; y < sh; ++y)
  {
    const int sy_uv = y >> 1;
    const uint8_t* yr = y_plane + (size_t) y * sw;
    const uint8_t* vu = vu_plane + (size_t) sy_uv * sw;
    uint8_t* drow = rgb + (size_t) y * sw * 3;
    for (int x = 0; x < sw; ++x)
    {
      const int sx_uv = (x >> 1) << 1;
      const int Y = yr[x];
      const int V = vu[sx_uv + 0];
      const int U = vu[sx_uv + 1];
      // BT.601 limited-range YUV → RGB.
      const int C = Y - 16;
      const int D = U - 128;
      const int E = V - 128;
      int R = (298 * C + 409 * E + 128) >> 8;
      int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
      int B = (298 * C + 516 * D + 128) >> 8;
      drow[x * 3 + 0] = (uint8_t) clampi(R, 0, 255);
      drow[x * 3 + 1] = (uint8_t) clampi(G, 0, 255);
      drow[x * 3 + 2] = (uint8_t) clampi(B, 0, 255);
    }
  }
}

int cr_nv21_to_jpeg(const uint8_t* nv21, size_t src_size, int sw, int sh,
                    size_t dst_cap, int quality, uint8_t** out, size_t* out_written)
{
  if (out)
    *out = NULL;
  if (out_written)
    *out_written = 0;
  if (!out || !out_written || !nv21 || sw < 8 || sh < 8 || sw > 8192 ||
      sh > 8192 || (sw & 1) || (sh & 1) || dst_cap < 1024)
    return -1;
  const size_t pixels = (size_t) sw * sh;
  if (pixels + pixels / 2 > src_size)
    return -1;
  if (quality < 1 || quality > 100)
    quality = 85;

  uint8_t* rgb = (uint8_t*) malloc(pixels * 3);
  if (!rgb)
    return -2;
  nv21_to_rgb24(nv21, sw, sh, rgb);

  struct EncodeCtx ctx = {0};
  ctx.limit = dst_cap;

  int ok = stbi_write_jpg_to_func(write_cb, &ctx, sw, sh, 3, rgb, quality);
  free(rgb);

  if (!ok || ctx.error || ctx.off == 0)
  {
    free(ctx.dst);
    return ctx.error ? ctx.error : -3;
  }
  *out = ctx.dst;
  *out_written = ctx.off;
  return 0;
}

struct CameraJpegTrailer
{
  uint16_t id;
  uint16_t padding;
  uint32_t size;
};

_Static_assert(sizeof(struct CameraJpegTrailer) == CR_CAMERA_JPEG_TRAILER_BYTES,
               "Camera3 JPEG trailer size");
_Static_assert(offsetof(struct CameraJpegTrailer, size) == 4,
               "Camera3 JPEG trailer size offset");

static int has_jpeg_markers(const uint8_t* data, size_t size)
{
  return size >= 4 && data[0] == 0xff && data[1] == 0xd8 &&
         data[size - 2] == 0xff && data[size - 1] == 0xd9;
}

static uint16_t exif_u16(const uint8_t* data, int little_endian)
{
  const size_t high = little_endian ? 1 : 0;
  return (uint16_t) ((data[high] << 8) | data[1 - high]);
}

static uint32_t exif_u32(const uint8_t* data, int little_endian)
{
  const size_t high = little_endian ? 2 : 0;
  return ((uint32_t) exif_u16(data + high, little_endian) << 16) |
         exif_u16(data + 2 - high, little_endian);
}

static uint8_t tiff_orientation(const uint8_t* tiff, size_t size)
{
  if (size < 8)
    return 0;
  const int little_endian = tiff[0] == 'I' && tiff[1] == 'I';
  if ((!little_endian && (tiff[0] != 'M' || tiff[1] != 'M')) ||
      exif_u16(tiff + 2, little_endian) != 42)
    return 0;
  const uint32_t offset = exif_u32(tiff + 4, little_endian);
  if (offset < 8 || offset > size - 2)
    return 0;
  const uint16_t count = exif_u16(tiff + offset, little_endian);
  const uint8_t* entry = tiff + offset + 2;
  if (count > (size - offset - 2) / 12)
    return 0;
  for (uint16_t i = 0; i < count; ++i, entry += 12)
  {
    if (exif_u16(entry, little_endian) != 0x0112 ||
        exif_u16(entry + 2, little_endian) != 3 ||
        exif_u32(entry + 4, little_endian) != 1)
      continue;
    const uint16_t orientation = exif_u16(entry + 8, little_endian);
    return orientation >= 1 && orientation <= 8 ? (uint8_t) orientation : 0;
  }
  return 0;
}

static uint8_t camera_jpeg_orientation(const uint8_t* jpeg, size_t size)
{
  size_t position = 2;
  while (position < size && jpeg[position] == 0xff)
  {
    while (position < size && jpeg[position] == 0xff)
      ++position;
    if (size - position < 3 || jpeg[position] == 0xda || jpeg[position] == 0xd9)
      break;
    const uint8_t marker = jpeg[position++];
    const size_t length = exif_u16(jpeg + position, 0);
    if (length < 2 || length > size - position)
      break;
    if (marker == 0xe1 && length >= 8 && memcmp(jpeg + position + 2, "Exif\0\0", 6) == 0)
    {
      const uint8_t orientation = tiff_orientation(jpeg + position + 8, length - 8);
      if (orientation != 0)
        return orientation;
    }
    position += length;
  }
  return 1;
}

int cr_replace_camera_jpeg(uint8_t* dst, size_t dst_size,
                           const uint8_t* jpeg, size_t jpeg_size)
{
  const uint16_t jpeg_id = 0x00ff;
  struct CameraJpegTrailer previous;
  if (!dst || !jpeg || dst_size < sizeof(previous) ||
      jpeg_size > dst_size - sizeof(previous) || jpeg_size > UINT32_MAX ||
      !has_jpeg_markers(jpeg, jpeg_size))
    return -1;

  const size_t capacity = dst_size - sizeof(previous);
  memcpy(&previous, dst + capacity, sizeof(previous));
  if (previous.id != jpeg_id || previous.size > capacity ||
      previous.size < 2 || dst[0] != 0xff || dst[1] != 0xd8)
    return -1;

  const uint8_t orientation = camera_jpeg_orientation(dst, previous.size);
  const uint8_t exif[] = {
      0xff, 0xe1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
      'I', 'I', 42, 0, 8, 0, 0, 0,
      1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, orientation, 0, 0, 0,
      0, 0, 0, 0};
  const size_t new_size = jpeg_size + (orientation == 1 ? 0 : sizeof(exif));
  if (new_size > capacity || new_size > UINT32_MAX || new_size < jpeg_size)
    return -1;
  if (orientation != 1)
  {
    // Retain the capture's orientation without carrying sensor metadata or location.
    memcpy(dst, jpeg, 2);
    memcpy(dst + 2, exif, sizeof(exif));
    memcpy(dst + 2 + sizeof(exif), jpeg + 2, jpeg_size - 2);
  }
  else
    memcpy(dst, jpeg, jpeg_size);
  const struct CameraJpegTrailer replacement = {jpeg_id, 0, (uint32_t) new_size};
  memcpy(dst + capacity, &replacement, sizeof(replacement));
  return 0;
}
