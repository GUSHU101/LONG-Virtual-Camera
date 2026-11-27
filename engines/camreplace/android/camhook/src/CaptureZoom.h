#pragma once

#include "nv21_blit.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace cr::camhook
{

inline unsigned capture_zoom_q10(float ratio, float focal_length,
                                 const std::array<int, 4>& crop)
{
  if (!std::isfinite(ratio) || ratio <= 0.f)
    return CR_NV21_ZOOM_ONE;
  // Pixel 5 can report the effective crop on the result and leave zoomRatio
  // at 1. A different aspect ratio alone is not optical/digital zoom.
  if (ratio >= .99f && ratio <= 1.01f && crop[2] > 0 && crop[3] > 0)
  {
    std::array<int, 2> sensor{};
    if (std::abs(focal_length - 2.57f) < .025f)
      sensor = {3280, 2464};
    else if (std::abs(focal_length - 4.38f) < .025f)
      sensor = {4032, 3024};
    else if (std::abs(focal_length - 2.22f) < .025f)
      sensor = {4656, 3496};
    if (crop[0] >= 0 && crop[1] >= 0 &&
        crop[2] <= sensor[0] && crop[3] <= sensor[1] &&
        crop[0] <= sensor[0] - crop[2] && crop[1] <= sensor[1] - crop[3])
      ratio = std::min(static_cast<float>(sensor[0]) / crop[2],
                       static_cast<float>(sensor[1]) / crop[3]);
  }
  if (ratio <= 1.01f)
    return CR_NV21_ZOOM_ONE;
  return static_cast<unsigned>(std::lround(std::min(ratio, 8.f) * CR_NV21_ZOOM_ONE));
}

} // namespace cr::camhook
