#include "jpeg_encode.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
  enum { WIDTH = 64, HEIGHT = 64, BLOB_BYTES = 8192 };
  uint8_t nv21[WIDTH * HEIGHT * 3 / 2];
  uint32_t random = 123;
  for (size_t i = 0; i < sizeof(nv21); ++i)
  {
    random = random * UINT32_C(1664525) + UINT32_C(1013904223);
    nv21[i] = (uint8_t)(random >> 24);
  }
  uint8_t blob[BLOB_BYTES];
  uint8_t original[BLOB_BYTES];
  memset(blob, 0xa5, sizeof(blob));
  const size_t tail = sizeof(blob) - CR_CAMERA_JPEG_TRAILER_BYTES;
  const uint8_t trailer[] = {0xff, 0x00, 0xa5, 0xa5, 4, 0, 0, 0};
  const uint8_t original_jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
  memcpy(blob, original_jpeg, sizeof(original_jpeg));
  memcpy(blob + tail, trailer, sizeof(trailer));
  memcpy(original, blob, sizeof(blob));

  uint8_t* jpeg = blob;
  size_t length = 42;
  assert(cr_nv21_to_jpeg(nv21, sizeof(nv21), WIDTH, HEIGHT, 1024, 85,
                        &jpeg, &length) == -3);
  assert(jpeg == NULL && length == 0);
  assert(memcmp(blob, original, sizeof(blob)) == 0);
  assert(cr_nv21_to_jpeg(nv21, sizeof(nv21) - 1, WIDTH, HEIGHT, tail, 85,
                        &jpeg, &length) == -1);
  assert(jpeg == NULL && length == 0);
  assert(cr_nv21_to_jpeg(nv21, sizeof(nv21), WIDTH - 1, HEIGHT, tail, 85,
                        &jpeg, &length) == -1);
  assert(cr_nv21_to_jpeg(nv21, sizeof(nv21), WIDTH, HEIGHT, tail, 85,
                        &jpeg, &length) == 0);
  assert(jpeg && length > 1024 && length <= tail);

  blob[tail] = 0x00;
  blob[tail + 1] = 0x01; // Android JPEG APP segments use the same BLOB format.
  memcpy(original, blob, sizeof(blob));
  assert(cr_replace_camera_jpeg(blob, sizeof(blob), jpeg, length) != 0);
  assert(memcmp(blob, original, sizeof(blob)) == 0);

  memcpy(blob + tail, trailer, sizeof(trailer));
  memcpy(original, blob, sizeof(blob));
  assert(cr_replace_camera_jpeg(blob, sizeof(blob), jpeg, length - 1) != 0);
  assert(memcmp(blob, original, sizeof(blob)) == 0);
  assert(cr_replace_camera_jpeg(blob, sizeof(blob), jpeg, length) == 0);
  assert(memcmp(blob, jpeg, length) == 0);
  assert(blob[tail] == 0xff && blob[tail + 1] == 0x00);
  assert(blob[tail + 2] == 0 && blob[tail + 3] == 0);
  uint32_t written = 0;
  memcpy(&written, blob + tail + 4, sizeof(written));
  assert(written == length);

  const uint8_t rotated_jpeg[] = {
      0xff, 0xd8, 0xff, 0xe1, 0, 0x22, 'E', 'x', 'i', 'f', 0, 0,
      'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1,
      0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 6, 0, 0, 0, 0, 0, 0, 0xff, 0xd9};
  memcpy(blob, rotated_jpeg, sizeof(rotated_jpeg));
  const uint32_t original_size = sizeof(rotated_jpeg);
  memcpy(blob + tail + 4, &original_size, sizeof(original_size));
  assert(cr_replace_camera_jpeg(blob, sizeof(blob), jpeg, length) == 0);
  assert(memcmp(blob + 6, "Exif\0\0", 6) == 0);
  assert(blob[30] == 6); // Retains a 90-degree clockwise display rotation.
  assert(memcmp(blob + 38, jpeg + 2, length - 2) == 0);
  memcpy(&written, blob + tail + 4, sizeof(written));
  assert(written == length + 36);

  memcpy(blob, rotated_jpeg, sizeof(rotated_jpeg));
  blob[8] = 'X'; // A different APP1 type contributes no orientation.
  memcpy(blob + tail + 4, &original_size, sizeof(original_size));
  assert(cr_replace_camera_jpeg(blob, sizeof(blob), jpeg, length) == 0);
  assert(memcmp(blob, jpeg, length) == 0);
  free(jpeg);
}
