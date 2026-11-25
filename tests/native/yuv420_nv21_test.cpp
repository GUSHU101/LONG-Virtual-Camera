#include "media/Yuv420Nv21.h"

#include <array>
#include <cassert>

namespace
{
using cr::media::YuvPlane;

// Reference for the existing center crop and fixed-point bilinear rounding.
std::uint8_t sample(YuvPlane plane, int sw, int sh, int rotation,
                    int dw, int dh, int x, int y)
{
  const bool transpose = rotation == 90 || rotation == 270;
  const int ow = transpose ? sh : sw;
  const int oh = transpose ? sw : sh;
  const bool crop_width = ow * dh > dw * oh;
  const int cw = crop_width ? std::max(1, oh * dw / dh) : ow;
  const int ch = crop_width ? oh : std::max(1, ow * dh / dw);
  const int ox = ((ow - cw) / 2 << 16) + ((x * (cw - 1)) << 16) / std::max(1, dw - 1);
  const int oy = ((oh - ch) / 2 << 16) + ((y * (ch - 1)) << 16) / std::max(1, dh - 1);
  const int flip_x = ((sw - 1) << 16);
  const int flip_y = ((sh - 1) << 16);
  int sx = ox, sy = oy;
  if (rotation == 90) { sx = oy; sy = flip_y - ox; }
  if (rotation == 180) { sx = flip_x - ox; sy = flip_y - oy; }
  if (rotation == 270) { sx = flip_x - oy; sy = ox; }

  const int x0 = sx >> 16, x1 = std::min(x0 + 1, sw - 1);
  const int y0 = sy >> 16, y1 = std::min(y0 + 1, sh - 1);
  auto at = [&](int X, int Y) { return plane.data[Y * plane.row_stride + X * plane.pixel_stride]; };
  const int fx = sx & 0xffff, fy = sy & 0xffff;
  const int top = at(x0, y0) + (((at(x1, y0) - at(x0, y0)) * fx) >> 16);
  const int bottom = at(x0, y1) + (((at(x1, y1) - at(x0, y1)) * fx) >> 16);
  return static_cast<std::uint8_t>(top + (((bottom - top) * fy) >> 16));
}
}

int main()
{
  constexpr int sw = 68;
  constexpr int sh = 44;
  constexpr int row = 80;
  std::vector<std::uint8_t> luma(row * sh);
  std::vector<std::uint8_t> chroma(row * (sh / 2));
  for (std::size_t i = 0; i < luma.size(); ++i)
    luma[i] = static_cast<std::uint8_t>((i * 53u) ^ (i >> 3));
  for (std::size_t i = 0; i < chroma.size(); ++i)
    chroma[i] = static_cast<std::uint8_t>((i * 29u) ^ (i >> 2));

  cr::media::Yuv420Nv21 converter;
  std::vector<std::uint8_t> output;
  const YuvPlane y{luma.data(), row, 1};
  for (const int rotation : {0, 90, 180, 270})
  {
    const bool transpose = rotation == 90 || rotation == 270;
    const std::array<std::array<int, 2>, 4> sizes{{
        {{transpose ? sh : sw, transpose ? sw : sh}}, {{30, 24}}, {{2, 2}}, {{30, 24}}}};
    for (const auto [width, height] : sizes)
    {
      for (const int pixel_stride : {1, 2})
      {
        const YuvPlane u{chroma.data(), row, pixel_stride};
        const YuvPlane v{chroma.data() + (pixel_stride == 1 ? row / 2 : 1), row, pixel_stride};
        assert(converter.convert(y, u, v, sw, sh, rotation, width, height, output));
        assert(output.size() == static_cast<std::size_t>(width * height * 3 / 2));
        for (int r = 0; r < height; ++r)
          for (int x = 0; x < width; ++x)
            assert(output[r * width + x] == sample(y, sw, sh, rotation, width, height, x, r));
        for (int r = 0; r < height / 2; ++r)
          for (int x = 0; x < width / 2; ++x)
          {
            const int i = width * height + r * width + x * 2;
            assert(output[i] == sample(v, sw / 2, sh / 2, rotation, width / 2, height / 2, x, r));
            assert(output[i + 1] == sample(u, sw / 2, sh / 2, rotation, width / 2, height / 2, x, r));
          }
      }
    }
  }
  assert(!converter.convert(y, {chroma.data(), 12, 2}, y, sw, sh, 0, sw, sh, output));
  assert(!converter.convert(y, y, y, sw, sh, 32, sw, sh, output));
  assert(!converter.convert(y, y, y, sw, sh, 0, sw - 1, sh, output));
}
