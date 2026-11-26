#pragma once

// NV21 → JPEG encoder for photo-mode BLOB substitution. Backed by
// stb_image_write (single-header public-domain). Encodes RGB-encoded
// JPEG at the requested quality.
// Returns 0 on success, negative error code on failure:
//   -1: invalid arguments or insufficient source data
//   -2: allocation failed
//   -3: encoding failed or exceeded the output capacity
// The caller frees the returned buffer with free(). Failure resets both outputs.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  enum { CR_CAMERA_JPEG_TRAILER_BYTES = 8 };

  int cr_nv21_to_jpeg(const uint8_t* nv21, size_t src_size, int src_w, int src_h,
                      size_t dst_cap, int quality, uint8_t** out, size_t* out_written);

  // Returns the encoded pixel dimensions of the original Camera3 JPEG.
  int cr_camera_jpeg_dimensions(const uint8_t* blob, size_t blob_size,
                               int* width, int* height);

  // Replace only a valid Android Camera3 JPEG BLOB, preserving the buffer on failure.
  int cr_replace_camera_jpeg(uint8_t* dst, size_t dst_size,
                             const uint8_t* jpeg, size_t jpeg_size);

#ifdef __cplusplus
}
#endif
