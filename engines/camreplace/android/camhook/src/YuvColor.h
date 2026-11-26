#pragma once

// SDR Y'CbCr to RGB coefficients in Q8. NV21 stores V followed by U.
typedef struct cr_yuv_coefficients
{
  int y_scale, y_offset, red_v, green_u, green_v, blue_u;
} cr_yuv_coefficients;

static inline cr_yuv_coefficients cr_yuv_coefficients_for(int bt709, int full_range)
{
  const cr_yuv_coefficients coefficients = {
      full_range ? 256 : 298,
      full_range ? 0 : 16,
      bt709 ? (full_range ? 403 : 459) : (full_range ? 359 : 409),
      bt709 ? (full_range ? 48 : 55) : (full_range ? 88 : 100),
      bt709 ? (full_range ? 120 : 136) : (full_range ? 183 : 208),
      bt709 ? (full_range ? 475 : 541) : (full_range ? 454 : 516)};
  return coefficients;
}
