#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace cr::media
{

struct YuvPlane
{
  const std::uint8_t* data = nullptr;
  std::int32_t row_stride = 0;
  std::int32_t pixel_stride = 1;
};

// The geometry is constant for most clips. Keep the crop, orientation and
// interpolation coordinates out of the per-frame pixel loops.
class Yuv420Nv21
{
public:
  bool convert(const YuvPlane& y, const YuvPlane& u, const YuvPlane& v,
               int src_w, int src_h, int rotation, int dst_w, int dst_h,
               std::vector<std::uint8_t>& destination)
  {
    if (src_w < 2 || src_h < 2 || dst_w < 2 || dst_h < 2 ||
        ((src_w | src_h | dst_w | dst_h) & 1) ||
        (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) ||
        !valid_plane(y, src_w) || !valid_plane(u, src_w / 2) || !valid_plane(v, src_w / 2))
      return false;

    const auto bytes = static_cast<std::size_t>(dst_w) * dst_h;
    destination.resize(bytes + bytes / 2);
    std::uint8_t* out_y = destination.data();
    std::uint8_t* out_vu = out_y + bytes;

    const bool transpose = rotation == 90 || rotation == 270;
    if ((transpose ? src_h : src_w) == dst_w && (transpose ? src_w : src_h) == dst_h)
    {
      switch (rotation)
      {
        case 90: copy_or_rotate<90>(y, u, v, src_w, src_h, out_y, out_vu, dst_w); break;
        case 180: copy_or_rotate<180>(y, u, v, src_w, src_h, out_y, out_vu, dst_w); break;
        case 270: copy_or_rotate<270>(y, u, v, src_w, src_h, out_y, out_vu, dst_w); break;
        default: copy_or_rotate<0>(y, u, v, src_w, src_h, out_y, out_vu, dst_w); break;
      }
      return true;
    }

    const Geometry geometry{src_w, src_h, rotation, dst_w, dst_h};
    if (geometry != geometry_)
    {
      auto luma = build_axes(src_w, src_h, rotation, dst_w, dst_h);
      auto chroma = build_axes(src_w / 2, src_h / 2, rotation, dst_w / 2, dst_h / 2);
      luma_ = std::move(luma);
      chroma_ = std::move(chroma);
      geometry_ = geometry;
    }

    if (transpose)
      scale<true>(y, u, v, out_y, out_vu, dst_w);
    else
      scale<false>(y, u, v, out_y, out_vu, dst_w);
    return true;
  }

private:
  struct Geometry
  {
    int src_w, src_h, rotation, dst_w, dst_h;
    bool operator==(const Geometry&) const = default;
  };

  struct Tap
  {
    int first, second, fraction;
  };

  struct Axes
  {
    std::vector<Tap> columns;
    std::vector<Tap> rows;
  };

  static bool valid_plane(const YuvPlane& plane, int width)
  {
    return plane.data && plane.pixel_stride >= 1 && plane.pixel_stride <= 4 &&
           static_cast<std::int64_t>(width - 1) * plane.pixel_stride < plane.row_stride;
  }

  static std::vector<Tap> build_axis(int source, int crop, int destination, bool flip)
  {
    std::vector<Tap> axis(destination);
    const auto start = static_cast<std::int64_t>((source - crop) / 2) << 16;
    for (int i = 0; i < destination; ++i)
    {
      auto position = start + ((static_cast<std::int64_t>(i) * (crop - 1)) << 16) /
                                  std::max(1, destination - 1);
      if (flip)
        position = (static_cast<std::int64_t>(source - 1) << 16) - position;
      const int first = static_cast<int>(position >> 16);
      axis[i] = {first, std::min(first + 1, source - 1),
                 static_cast<int>(position & 0xffff)};
    }
    return axis;
  }

  static Axes build_axes(int src_w, int src_h, int rotation, int dst_w, int dst_h)
  {
    const bool transpose = rotation == 90 || rotation == 270;
    const int oriented_w = transpose ? src_h : src_w;
    const int oriented_h = transpose ? src_w : src_h;
    int crop_w = oriented_w;
    int crop_h = oriented_h;
    if (static_cast<std::int64_t>(oriented_w) * dst_h >
        static_cast<std::int64_t>(dst_w) * oriented_h)
      crop_w = std::max(1, static_cast<int>(static_cast<std::int64_t>(oriented_h) * dst_w / dst_h));
    else
      crop_h = std::max(1, static_cast<int>(static_cast<std::int64_t>(oriented_w) * dst_h / dst_w));
    return {build_axis(oriented_w, crop_w, dst_w, rotation == 90 || rotation == 180),
            build_axis(oriented_h, crop_h, dst_h, rotation == 180 || rotation == 270)};
  }

  template <int Rotation, int PixelStride>
  static void copy_plane(const YuvPlane& plane, std::uint8_t* dst, int width, int height, int stride)
  {
    if constexpr (Rotation == 90 || Rotation == 270)
    {
      // Transpose one tile at a time to keep the source and destination
      // cache lines hot even when their row strides are several KiB.
      constexpr int tile = 32;
      for (int top = 0; top < height; top += tile)
        for (int left = 0; left < width; left += tile)
          for (int row = top; row < std::min(top + tile, height); ++row)
          {
            const auto* src = plane.data + static_cast<std::size_t>(row) * plane.row_stride;
            const int dst_x = Rotation == 90 ? height - 1 - row : row;
            for (int x = left; x < std::min(left + tile, width); ++x)
            {
              const int dst_y = Rotation == 90 ? x : width - 1 - x;
              dst[static_cast<std::size_t>(dst_y) * stride + dst_x * PixelStride] =
                  src[x * plane.pixel_stride];
            }
          }
      return;
    }
    for (int row = 0; row < height; ++row)
    {
      const auto* src = plane.data + static_cast<std::size_t>(row) * plane.row_stride;
      const int dst_row = Rotation == 180 ? height - 1 - row : row;
      auto* out = dst + static_cast<std::size_t>(dst_row) * stride;
      if (Rotation == 0 && PixelStride == 1 && plane.pixel_stride == 1)
        std::memcpy(out, src, static_cast<std::size_t>(width));
      else
        for (int x = 0; x < width; ++x)
        {
          const int dst_x = Rotation == 180 ? width - 1 - x : x;
          out[dst_x * PixelStride] = src[x * plane.pixel_stride];
        }
    }
  }

  template <int Rotation>
  static void copy_or_rotate(const YuvPlane& y, const YuvPlane& u, const YuvPlane& v,
                             int width, int height, std::uint8_t* out_y,
                             std::uint8_t* out_vu, int stride)
  {
    copy_plane<Rotation, 1>(y, out_y, width, height, stride);
    copy_plane<Rotation, 2>(v, out_vu, width / 2, height / 2, stride);
    copy_plane<Rotation, 2>(u, out_vu + 1, width / 2, height / 2, stride);
  }

  template <bool Transpose, int DstPixelStride>
  static void scale_plane(const YuvPlane& plane, const Axes& axes, std::uint8_t* dst, int stride)
  {
    for (std::size_t y = 0; y < axes.rows.size(); ++y)
    {
      const auto& row = axes.rows[y];
      auto* output = dst + y * stride;
      for (std::size_t x = 0; x < axes.columns.size(); ++x)
      {
        const auto& column = axes.columns[x];
        const auto& horizontal = Transpose ? row : column;
        const auto& vertical = Transpose ? column : row;
        const auto* src0 = plane.data + static_cast<std::size_t>(vertical.first) * plane.row_stride;
        const auto* src1 = plane.data + static_cast<std::size_t>(vertical.second) * plane.row_stride;
        const int x0 = horizontal.first * plane.pixel_stride;
        const int x1 = horizontal.second * plane.pixel_stride;
        const int a = src0[x0];
        const int b = src0[x1];
        const int c = src1[x0];
        const int d = src1[x1];
        const int top = a + (((b - a) * horizontal.fraction) >> 16);
        const int bottom = c + (((d - c) * horizontal.fraction) >> 16);
        output[x * DstPixelStride] =
            static_cast<std::uint8_t>(top + (((bottom - top) * vertical.fraction) >> 16));
      }
    }
  }

  template <bool Transpose>
  void scale(const YuvPlane& y, const YuvPlane& u, const YuvPlane& v,
             std::uint8_t* out_y, std::uint8_t* out_vu, int stride) const
  {
    scale_plane<Transpose, 1>(y, luma_, out_y, stride);
    scale_plane<Transpose, 2>(v, chroma_, out_vu, stride);
    scale_plane<Transpose, 2>(u, chroma_, out_vu + 1, stride);
  }

  Geometry geometry_{};
  Axes luma_;
  Axes chroma_;
};

} // namespace cr::media
