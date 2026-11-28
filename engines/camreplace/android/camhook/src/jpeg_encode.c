// NV21 → JPEG encoder used by photo-mode BLOB substitution.
// We bundle stb_image_write (single-header, public domain). It's about
// 70KB of source and adds ~50KB to the stripped libcr_camhook.so. No
// external link dependency, which matters because cameraserver's
// process is locked down — bringing in libjpeg.so via dlopen works on
// most Androids but is fragile across vendor builds.
// Pipeline per encode call:
//   1. Allocate w*h*3 byte RGB scratch on heap (~6 MB at 1080p, ~36 MB
//      at 4K — fine inside cameraserver, freed immediately).
//   2. NV21 → RGB888 conversion with the feed's SDR matrix and range.
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
#include "YuvColor.h"

enum { JPEG_CAPACITY_EXCEEDED = -4 };

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
    ctx->error = JPEG_CAPACITY_EXCEEDED;
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

static inline void write_rgb_pixel(uint8_t* rgb, int luma, int red, int green, int blue)
{
  rgb[0] = (uint8_t) clampi((luma + red) >> 8, 0, 255);
  rgb[1] = (uint8_t) clampi((luma + green) >> 8, 0, 255);
  rgb[2] = (uint8_t) clampi((luma + blue) >> 8, 0, 255);
}

// NV21 → RGB888. NV21 is Y plane (sw*sh bytes) followed by interleaved
// V,U pairs at half-resolution. Each pair supplies a 2x2 block, so
// reuse its chroma contribution for each of the four luma values.
static void nv21_to_rgb24(const uint8_t* nv21, int sw, int sh, uint8_t* rgb,
                           int bt709, int full_range)
{
  const uint8_t* y_plane = nv21;
  const uint8_t* vu_plane = nv21 + (size_t) sw * sh;
  const cr_yuv_coefficients yuv = cr_yuv_coefficients_for(bt709, full_range);
  const size_t rgb_stride = (size_t) sw * 3;
  for (int y = 0; y < sh; y += 2)
  {
    const uint8_t* top = y_plane + (size_t) y * sw;
    const uint8_t* bottom = top + sw;
    const uint8_t* vu = vu_plane + (size_t) (y / 2) * sw;
    uint8_t* rgb_top = rgb + (size_t) y * rgb_stride;
    uint8_t* rgb_bottom = rgb_top + rgb_stride;
    for (int x = 0; x < sw; x += 2)
    {
      const int v = vu[x] - 128;
      const int u = vu[x + 1] - 128;
      const int red = yuv.red_v * v + 128;
      const int green = 128 - yuv.green_u * u - yuv.green_v * v;
      const int blue = yuv.blue_u * u + 128;
      write_rgb_pixel(rgb_top + x * 3, yuv.y_scale * (top[x] - yuv.y_offset), red, green, blue);
      write_rgb_pixel(rgb_top + x * 3 + 3, yuv.y_scale * (top[x + 1] - yuv.y_offset), red, green, blue);
      write_rgb_pixel(rgb_bottom + x * 3, yuv.y_scale * (bottom[x] - yuv.y_offset), red, green, blue);
      write_rgb_pixel(rgb_bottom + x * 3 + 3, yuv.y_scale * (bottom[x + 1] - yuv.y_offset), red, green, blue);
    }
  }
}

static int has_jpeg_markers(const uint8_t* data, size_t size)
{
  return size >= 4 && data[0] == 0xff && data[1] == 0xd8 &&
         data[size - 2] == 0xff && data[size - 1] == 0xd9;
}

static int encode_rgb_jpeg(const uint8_t* rgb, int width, int height, size_t limit, int quality,
                           uint8_t** out, size_t* out_written)
{
  struct EncodeCtx ctx = {0};
  ctx.limit = limit;
  const int ok = stbi_write_jpg_to_func(write_cb, &ctx, width, height, 3, rgb, quality);
  if (!ok || ctx.error || !has_jpeg_markers(ctx.dst, ctx.off))
  {
    free(ctx.dst);
    return ctx.error ? ctx.error : -3;
  }
  *out = ctx.dst;
  *out_written = ctx.off;
  return 0;
}

int cr_nv21_to_jpeg_color(const uint8_t* nv21, size_t src_size, int sw, int sh,
                          size_t dst_cap, int quality, uint8_t** out, size_t* out_written,
                          int bt709, int full_range)
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
  nv21_to_rgb24(nv21, sw, sh, rgb, bt709, full_range);

  int result = encode_rgb_jpeg(rgb, sw, sh, dst_cap, quality, out, out_written);
  // A single bounded retry keeps the dimensions and reuses the RGB conversion.
  // Limit the retry to small quality reductions and leave other failures alone.
  if (result == JPEG_CAPACITY_EXCEEDED && quality >= 85)
    result = encode_rgb_jpeg(rgb, sw, sh, dst_cap, quality - 5, out, out_written);
  free(rgb);
  return result == JPEG_CAPACITY_EXCEEDED ? -3 : result;
}

int cr_nv21_to_jpeg(const uint8_t* nv21, size_t src_size, int sw, int sh,
                    size_t dst_cap, int quality, uint8_t** out, size_t* out_written)
{
  return cr_nv21_to_jpeg_color(nv21, src_size, sw, sh, dst_cap, quality, out, out_written, 0, 0);
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

static void read_jpeg_metadata(const uint8_t* jpeg, size_t size,
                               int* width, int* height, uint8_t* orientation)
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
    if (orientation && marker == 0xe1 && length >= 8 &&
        memcmp(jpeg + position + 2, "Exif\0\0", 6) == 0)
    {
      const uint8_t value = tiff_orientation(jpeg + position + 8, length - 8);
      if (value != 0)
      {
        *orientation = value;
        if (!width || !height)
          return;
      }
    }
    if (width && height && marker >= 0xc0 && marker <= 0xcf &&
        marker != 0xc4 && marker != 0xc8 && marker != 0xcc && length >= 8)
    {
      *height = exif_u16(jpeg + position + 3, 0);
      *width = exif_u16(jpeg + position + 5, 0);
      return;
    }
    position += length;
  }
}

static size_t camera_jpeg_size(const uint8_t* blob, size_t blob_size)
{
  struct CameraJpegTrailer trailer;
  if (!blob || blob_size < sizeof(trailer))
    return 0;
  const size_t capacity = blob_size - sizeof(trailer);
  memcpy(&trailer, blob + capacity, sizeof(trailer));
  if (trailer.id != 0x00ff || trailer.size > capacity || trailer.size < 2 ||
      blob[0] != 0xff || blob[1] != 0xd8)
    return 0;
  return trailer.size;
}

int cr_camera_jpeg_dimensions(const uint8_t* blob, size_t blob_size,
                               int* width, int* height)
{
  if (!width || !height)
    return -1;
  *width = 0;
  *height = 0;
  const size_t size = camera_jpeg_size(blob, blob_size);
  if (size == 0)
    return -1;
  read_jpeg_metadata(blob, size, width, height, NULL);
  return *width > 0 && *height > 0 ? 0 : -1;
}

int cr_replace_camera_jpeg(uint8_t* dst, size_t dst_size,
                           const uint8_t* jpeg, size_t jpeg_size)
{
  const uint16_t jpeg_id = 0x00ff;
  if (!dst || !jpeg || dst_size < sizeof(struct CameraJpegTrailer) ||
      jpeg_size > dst_size - sizeof(struct CameraJpegTrailer) || jpeg_size > UINT32_MAX ||
      !has_jpeg_markers(jpeg, jpeg_size))
    return -1;

  int width = 0, height = 0;
  read_jpeg_metadata(jpeg, jpeg_size, &width, &height, NULL);
  if (width <= 0 || height <= 0)
    return -1;

  const size_t previous_size = camera_jpeg_size(dst, dst_size);
  if (previous_size == 0)
    return -1;

  const size_t capacity = dst_size - sizeof(struct CameraJpegTrailer);
  uint8_t orientation = 1;
  read_jpeg_metadata(dst, previous_size, NULL, NULL, &orientation);
  const uint8_t exif[] = {
      0xff, 0xe1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
      'I', 'I', 42, 0, 8, 0, 0, 0,
      1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, orientation, 0, 0, 0,
      0, 0, 0, 0};
  _Static_assert(sizeof(exif) == CR_CAMERA_JPEG_ORIENTATION_BYTES, "JPEG orientation segment size");
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
