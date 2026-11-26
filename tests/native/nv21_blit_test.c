#include "nv21_blit.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int crop_contract(void)
{
  cr_nv21_crop_rect crop = {0};
  if (!cr_nv21_center_crop(1920, 1080, 640, 480, &crop)) return 1;
  if (crop.x != 240 || crop.y != 0 || crop.width != 1440 || crop.height != 1080)
    return 2;
  if (!cr_nv21_center_crop(1080, 1920, 1280, 720, &crop)) return 3;
  if (crop.x != 0 || crop.y != 656 || crop.width != 1080 || crop.height != 606)
    return 4;
  if (cr_nv21_center_crop(1919, 1080, 640, 480, &crop)) return 5;
  return 0;
}

static int planar_contract(const uint8_t* source, int width, int height)
{
  uint8_t y[10 * 8], u[6 * 4], v[6 * 4];
  uint8_t reference_y[sizeof(y)], reference_vu[12 * 4];
  memset(y, 0xee, sizeof(y));
  memset(u, 0xee, sizeof(u));
  memset(v, 0xee, sizeof(v));
  memset(reference_y, 0xee, sizeof(reference_y));

  cr_nv21_blit(source, 8, 4, reference_y, reference_vu, width, height, 10, 12, 0);
  cr_nv21_blit_planar(source, 8, 4, y, u, v, width, height, 10, 6);
  if (memcmp(y, reference_y, sizeof(y)) != 0) return 20;
  for (int row = 0; row < height / 2; ++row)
  {
    for (int x = 0; x < width / 2; ++x)
    {
      if (u[row * 6 + x] != reference_vu[row * 12 + x * 2 + 1] ||
          v[row * 6 + x] != reference_vu[row * 12 + x * 2])
        return 21;
    }
    if (u[row * 6 + width / 2] != 0xee || v[row * 6 + width / 2] != 0xee)
      return 22;
  }
  memset(y, 0xee, sizeof(y));
  memset(u, 0xee, sizeof(u));
  memset(v, 0xee, sizeof(v));
  cr_nv21_blit_planar(source, 8, 4, y, u, v, width, height, 10, width / 2 - 1);
  if (y[0] != 0xee || u[0] != 0xee || v[0] != 0xee) return 23;
  return 0;
}

static int blit_contract(void)
{
  // 8x4 (2:1) -> 4x4 (1:1) must crop x=2..5 rather than stretch x=0..7.
  uint8_t source[8 * 4 * 3 / 2];
  memset(source, 0, sizeof(source));
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 8; ++x)
      source[y * 8 + x] = (uint8_t) (y * 16 + x);
  uint8_t* source_vu = source + 8 * 4;
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 8; ++x)
      source_vu[y * 8 + x] = (uint8_t) (100 + y * 16 + x);

  uint8_t y_plane[6 * 4];
  uint8_t vu_plane[6 * 2];
  memset(y_plane, 0xee, sizeof(y_plane));
  memset(vu_plane, 0xee, sizeof(vu_plane));
  cr_nv21_blit(source, 8, 4, y_plane, vu_plane, 4, 4, 6, 6, 0);

  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
      if (y_plane[y * 6 + x] != (uint8_t) (y * 16 + x + 2)) return 10;
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 4; ++x)
      if (vu_plane[y * 6 + x] != (uint8_t) (100 + y * 16 + x + 2)) return 11;
  if (y_plane[4] != 0xee || vu_plane[4] != 0xee) return 12;

  memset(vu_plane, 0, sizeof(vu_plane));
  cr_nv21_blit(source, 8, 4, y_plane, vu_plane, 4, 4, 6, 6, 1);
  if (vu_plane[0] != 103 || vu_plane[1] != 102 ||
      vu_plane[2] != 105 || vu_plane[3] != 104) return 13;
  const int planar_crop = planar_contract(source, 4, 4);
  if (planar_crop) return planar_crop;
  return planar_contract(source, 6, 6);
}

static uint32_t interpolation_coord(int position, int source, int dest)
{
  const uint32_t end = (uint32_t) (source - 1) << 16;
  return position == dest - 1 ? end :
         (uint32_t) position * (end / (uint32_t) (dest - 1));
}

static int reference_blend(int a, int b, uint32_t coordinate)
{
  return (a * (int) (65536u - (coordinate & 65535u)) +
          b * (int) (coordinate & 65535u)) / 65536;
}

static uint8_t reference_sample(const uint8_t* src, int stride, int channels,
                                int sw, int sh, int dw, int dh, int x, int y)
{
  const uint32_t sx = interpolation_coord(x, sw, dw);
  const uint32_t sy = interpolation_coord(y, sh, dh);
  const int x0 = (int) (sx >> 16), y0 = (int) (sy >> 16);
  const int x1 = x0 + (x0 + 1 < sw), y1 = y0 + (y0 + 1 < sh);
  const int a = reference_blend(src[y0 * stride + x0 * channels],
                                src[y0 * stride + x1 * channels], sx);
  const int b = reference_blend(src[y1 * stride + x0 * channels],
                                src[y1 * stride + x1 * channels], sx);
  return (uint8_t) reference_blend(a, b, sy);
}

static int exact_scaling(int sw, int sh, int dw, int dh)
{
  const int y_stride = dw + 5;
  const int uv_stride = dw + 7;
  const size_t y_bytes = (size_t) y_stride * dh;
  const size_t uv_bytes = (size_t) uv_stride * dh / 2;
  const size_t src_bytes = (size_t) sw * sh * 3 / 2;
  uint8_t* source = malloc(src_bytes);
  uint8_t* buffer = malloc(y_bytes + uv_bytes * 2);
  int error = 30;
  if (!source || !buffer)
    goto done;
  for (size_t i = 0; i < src_bytes; ++i)
    source[i] = (uint8_t) (i * 97u + (i >> 4));
  cr_nv21_crop_rect crop;
  if (!cr_nv21_center_crop(sw, sh, dw, dh, &crop))
    goto done;
  const uint8_t* y_src = source + crop.y * sw + crop.x;
  const uint8_t* vu_src = source + sw * sh + crop.y / 2 * sw + crop.x;
  uint8_t* vu = buffer + y_bytes;
  uint8_t* u = vu + uv_bytes;
  for (int format = 0; format < 3; ++format)
  {
    memset(buffer, 0xee, y_bytes + uv_bytes * 2);
    if (format == 2)
      cr_nv21_blit_planar(source, sw, sh, buffer, u, vu, dw, dh, y_stride, uv_stride);
    else
      cr_nv21_blit(source, sw, sh, buffer, vu, dw, dh, y_stride, uv_stride, format);
    for (int y = 0; y < dh; ++y)
      for (int x = 0; x < y_stride; ++x)
      {
        const uint8_t expected = x < dw ?
            reference_sample(y_src, sw, 1, crop.width, crop.height, dw, dh, x, y) : 0xee;
        if (buffer[y * y_stride + x] != expected)
          goto done;
      }
    for (int y = 0; y < dh / 2; ++y)
    {
      for (int x = 0; x < dw / 2; ++x)
      {
        const uint8_t v_expected = reference_sample(vu_src, sw, 2, crop.width / 2,
            crop.height / 2, dw / 2, dh / 2, x, y);
        const uint8_t u_expected = reference_sample(vu_src + 1, sw, 2, crop.width / 2,
            crop.height / 2, dw / 2, dh / 2, x, y);
        if (format == 2)
        {
          if (vu[y * uv_stride + x] != v_expected || u[y * uv_stride + x] != u_expected)
            goto done;
        }
        else if (vu[y * uv_stride + 2 * x + format] != v_expected ||
                 vu[y * uv_stride + 2 * x + (1 - format)] != u_expected)
          goto done;
      }
      for (int x = format == 2 ? dw / 2 : dw; x < uv_stride; ++x)
        if (vu[y * uv_stride + x] != 0xee || u[y * uv_stride + x] != 0xee)
          goto done;
    }
  }
  error = 0;
done:
  free(buffer);
  free(source);
  return error;
}

int main(void)
{
  const int crop = crop_contract();
  if (crop) return crop;
  const int basic = blit_contract();
  if (basic) return basic;
  if (exact_scaling(40, 24, 110, 82) || exact_scaling(70, 110, 26, 32) ||
      exact_scaling(40, 24, 24, 24) || exact_scaling(2, 2, 34, 34) ||
      exact_scaling(10, 6, 2, 2))
    return 30;
  return 0;
}
