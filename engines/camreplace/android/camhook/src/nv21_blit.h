#pragma once

// NV21 blit: copy a source NV21 image (any size) into a destination NV21
// image (any size, possibly different stride). Bilinear scaling on CPU —
// good enough quality for camera previews and still captures, and cheap
// enough to keep the hook within a single frame time (sub-10 ms on a
// 1080p-to-1080p copy on this phone's A55 cores).
// Usage inside a hook:
//   nv21_blit(src_data, src_w, src_h,
//             dst_y, dst_vu, dst_w, dst_h, dst_stride);

#include <stddef.h>
#include <stdint.h>

#define CR_NV21_ZOOM_ONE 1024u

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct cr_nv21_crop_rect
  {
    int x;
    int y;
    int width;
    int height;
  } cr_nv21_crop_rect;

  // Aspect-preserving crop used for every camera output geometry. This is
  // deliberately public so the geometry contract can be host-unit-tested.
  // Coordinates and dimensions are even to stay aligned to 4:2:0 chroma.
  int cr_nv21_center_crop(int src_w, int src_h, int dst_w, int dst_h,
                          cr_nv21_crop_rect* out);
  int cr_nv21_zoom_crop(int src_w, int src_h, int dst_w, int dst_h,
                        unsigned zoom_q10, cr_nv21_crop_rect* out);

  // Source: contiguous Y plane at src + then interleaved VU at src + src_w*src_h
  // (i.e. NV21: V byte first in each chroma pair).
  // Destination: Y plane + VU/UV plane. The Y plane uses dst_y_stride between
  // rows; the chroma plane uses dst_uv_stride and each pair occupies 2 bytes
  // at half the row count.
  // `dst_uv_swapped` selects the destination chroma byte order:
  //   - false → dst is NV21 (V first in pair), match source byte-for-byte
  //   - true  → dst is NV12 (U first in pair), swap bytes while copying
  // On most Qualcomm SoCs (including willow/Redmi Note 8T under Pixel Experience)
  // the YCbCr_420_888 buffer is actually NV12 even when the camera format tag
  // says 0x11 (HAL_PIXEL_FORMAT_YCRCB_420_SP). Without the swap, red objects
  // turn grey/blue — see the bug we hit on 2026-04-22.
  void cr_nv21_blit(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride, int dst_uv_swapped);
  void cr_nv21_blit_zoom(const uint8_t* src, int src_w, int src_h,
                        uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h,
                        int dst_y_stride, int dst_uv_stride, int dst_uv_swapped,
                        unsigned zoom_q10);

  // Three-plane YUV420 destination with a one-byte chroma pixel stride.
  // The U and V planes each have dst_w/2 samples per row.
  void cr_nv21_blit_planar(const uint8_t* src, int src_w, int src_h,
                           uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v,
                           int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride);
  void cr_nv21_blit_planar_zoom(const uint8_t* src, int src_w, int src_h,
                               uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v,
                               int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride,
                               unsigned zoom_q10);

  // Sprint E: Lanczos2 alternative. Same signature as cr_nv21_blit, lives
  // in nv21_blit_lanczos.c. Sharper edges on big up-scales at the cost of
  // CPU time per frame (scalar implementation; NEON SIMD is the obvious
  // follow-up). hook_main.cpp picks one based on the CR_BLIT_SCALER env
  // var (default = bilinear).
  void cr_nv21_blit_lanczos(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride, int dst_uv_swapped);
  void cr_nv21_blit_lanczos_zoom(const uint8_t* src, int src_w, int src_h,
                                uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h,
                                int dst_y_stride, int dst_uv_stride, int dst_uv_swapped,
                                unsigned zoom_q10);

#ifdef __cplusplus
}
#endif
