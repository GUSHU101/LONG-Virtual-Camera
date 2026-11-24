#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace cr::media
{

inline bool checked_add(std::size_t a, std::size_t b, std::size_t& out) noexcept
{
  if (b > std::numeric_limits<std::size_t>::max() - a)
    return false;
  out = a + b;
  return true;
}

inline bool checked_mul(std::size_t a, std::size_t b, std::size_t& out) noexcept
{
  if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a)
    return false;
  out = a * b;
  return true;
}

struct PlaneSpan
{
  std::size_t offset = 0;
  std::size_t bytes = 0;
};

struct Yuv420Layout
{
  PlaneSpan y;
  PlaneSpan u_or_uv;
  PlaneSpan v;
  bool planar = false;
};

inline bool valid_codec_window(std::size_t capacity, std::int64_t offset,
                               std::int64_t size, PlaneSpan& window) noexcept
{
  if (offset < 0 || size < 0)
    return false;
  const auto off = static_cast<std::size_t>(offset);
  const auto len = static_cast<std::size_t>(size);
  if (off > capacity || len > capacity - off)
    return false;
  window = {off, len};
  return true;
}

inline bool last_row_span(std::size_t offset, std::size_t stride,
                          std::size_t rows, std::size_t row_bytes,
                          PlaneSpan& out) noexcept
{
  if (rows == 0 || row_bytes == 0 || stride < row_bytes)
    return false;
  std::size_t tail = 0;
  std::size_t end = 0;
  if (!checked_mul(stride, rows - 1, tail) ||
      !checked_add(offset, tail, end) ||
      !checked_add(end, row_bytes, end))
    return false;
  out = {offset, end - offset};
  return true;
}

inline bool semiplanar_420(std::size_t available, std::uint32_t width,
                           std::uint32_t height, std::uint32_t y_stride,
                           std::uint32_t slice_height,
                           std::uint32_t uv_stride,
                           Yuv420Layout& out) noexcept
{
  out = {};
  if (width == 0 || height == 0 || (width & 1u) || (height & 1u) ||
      (slice_height & 1u) || y_stride < width || uv_stride < width ||
      slice_height < height)
    return false;
  std::size_t uv_offset = 0;
  if (!checked_mul(y_stride, slice_height, uv_offset) || uv_offset > available)
    return false;
  if (!last_row_span(0, y_stride, height, width, out.y) ||
      !last_row_span(uv_offset, uv_stride, height / 2u, width, out.u_or_uv))
    return false;
  std::size_t uv_end = 0;
  return checked_add(out.u_or_uv.offset, out.u_or_uv.bytes, uv_end) &&
         uv_end <= available;
}

inline bool planar_420(std::size_t available, std::uint32_t width,
                       std::uint32_t height, std::uint32_t y_stride,
                       std::uint32_t slice_height,
                       std::uint32_t chroma_stride,
                       Yuv420Layout& out) noexcept
{
  out = {};
  if (width == 0 || height == 0 || (width & 1u) || (height & 1u) ||
      (slice_height & 1u) || y_stride < width || slice_height < height ||
      chroma_stride < width / 2u)
    return false;
  std::size_t u_offset = 0;
  std::size_t chroma_plane_storage = 0;
  std::size_t v_offset = 0;
  if (!checked_mul(y_stride, slice_height, u_offset) ||
      !checked_mul(chroma_stride, slice_height / 2u, chroma_plane_storage) ||
      !checked_add(u_offset, chroma_plane_storage, v_offset) ||
      v_offset > available)
    return false;
  if (!last_row_span(0, y_stride, height, width, out.y) ||
      !last_row_span(u_offset, chroma_stride, height / 2u, width / 2u,
                     out.u_or_uv) ||
      !last_row_span(v_offset, chroma_stride, height / 2u, width / 2u,
                     out.v))
    return false;
  std::size_t v_end = 0;
  if (!checked_add(out.v.offset, out.v.bytes, v_end) || v_end > available)
    return false;
  out.planar = true;
  return true;
}

} // namespace cr::media
