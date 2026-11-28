#include "jpeg_encode.h"

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../../engines/camreplace/third_party/stb/stb_image.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int reference_channel(double value)
{
  return (int) lround(fmin(255.0, fmax(0.0, value)));
}

static void verify_encoded_colors(int bt709, int full_range)
{
  enum { SWATCH = 16, COLUMNS = 4, WIDTH = SWATCH * COLUMNS, HEIGHT = SWATCH * 2 };
  // Include a gray ramp and asymmetric chroma values to catch a U/V swap.
  const uint8_t swatches[][3] = {
      {full_range ? 0 : 16, 128, 128}, {64, 128, 128}, {170, 128, 128},
      {full_range ? 255 : 235, 128, 128},
      {110, 90, 190}, {125, 185, 84}, {147, 68, 102}, {114, 158, 171}};
  uint8_t nv21[WIDTH * HEIGHT * 3 / 2];
  for (int row = 0; row < HEIGHT; ++row)
  {
    for (int col = 0; col < WIDTH; ++col)
    {
      const uint8_t* swatch = swatches[(row / SWATCH) * COLUMNS + col / SWATCH];
      nv21[row * WIDTH + col] = swatch[0];
      if ((row % 2) == 0 && (col % 2) == 0)
      {
        size_t offset = WIDTH * HEIGHT + (row / 2) * WIDTH + col;
        nv21[offset] = swatch[2];
        nv21[offset + 1] = swatch[1];
      }
    }
  }

  uint8_t* jpeg = NULL;
  size_t length = 0;
  assert(cr_nv21_to_jpeg_color(nv21, sizeof(nv21), WIDTH, HEIGHT, 8192, 85,
                               &jpeg, &length, bt709, full_range) == 0);
  int width = 0, height = 0, components = 0;
  uint8_t* rgb = stbi_load_from_memory(jpeg, (int) length, &width, &height, &components, 3);
  assert(rgb && width == WIDTH && height == HEIGHT);
  const double kr = bt709 ? 0.2126 : 0.299;
  const double kb = bt709 ? 0.0722 : 0.114;
  for (size_t i = 0; i < sizeof(swatches) / sizeof(swatches[0]); ++i)
  {
    const double y = (swatches[i][0] - (full_range ? 0 : 16)) * 255.0 / (full_range ? 255 : 219);
    const double cb = (swatches[i][1] - 128) * 255.0 / (full_range ? 255 : 224);
    const double cr = (swatches[i][2] - 128) * 255.0 / (full_range ? 255 : 224);
    const double r = y + 2.0 * (1.0 - kr) * cr;
    const double b = y + 2.0 * (1.0 - kb) * cb;
    const double g = (y - kr * r - kb * b) / (1.0 - kr - kb);
    const size_t row = i / COLUMNS * SWATCH + SWATCH / 2;
    const size_t col = i % COLUMNS * SWATCH + SWATCH / 2;
    const uint8_t* pixel = rgb + (row * WIDTH + col) * 3;
    // JPEG quantization and integer rounding account for a few code values.
    assert(abs(pixel[0] - reference_channel(r)) <= 4);
    assert(abs(pixel[1] - reference_channel(g)) <= 4);
    assert(abs(pixel[2] - reference_channel(b)) <= 4);
  }
  stbi_image_free(rgb);
  free(jpeg);
}

static void verify_jpeg_capacity(const uint8_t* nv21, size_t src_size, int width, int height,
                                 size_t budget, const uint8_t* nominal, size_t nominal_size)
{
  uint8_t* reference = NULL;
  size_t reference_size = 0;
  assert(cr_nv21_to_jpeg(nv21, src_size, width, height, budget, 80,
                        &reference, &reference_size) == 0);
  assert(reference_size > 1024 && reference_size < nominal_size);

  uint8_t* encoded = NULL;
  size_t size = 0;
  assert(cr_nv21_to_jpeg(nv21, src_size, width, height, nominal_size, 85, &encoded, &size) == 0);
  assert(size == nominal_size && memcmp(encoded, nominal, size) == 0);
  free(encoded);

  assert(cr_nv21_to_jpeg(nv21, src_size, width, height, reference_size, 85, &encoded, &size) == 0);
  assert(size == reference_size && memcmp(encoded, reference, size) == 0);
  int decoded_width = 0, decoded_height = 0, components = 0;
  uint8_t* rgb = stbi_load_from_memory(encoded, (int) size, &decoded_width, &decoded_height, &components, 3);
  assert(rgb && decoded_width == width && decoded_height == height);
  stbi_image_free(rgb);
  free(encoded);
  free(reference);

  assert(cr_nv21_to_jpeg(nv21, src_size, width, height, reference_size - 1, 85, &encoded, &size) == -3);
  assert(encoded == NULL && size == 0);
}

int main(void)
{
  verify_encoded_colors(0, 0);
  verify_encoded_colors(0, 1);
  verify_encoded_colors(1, 0);
  verify_encoded_colors(1, 1);

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
  verify_jpeg_capacity(nv21, sizeof(nv21), WIDTH, HEIGHT, tail, jpeg, length);

  blob[tail] = 0x00;
  blob[tail + 1] = 0x01; // Android JPEG APP segments use the same BLOB format.
  memcpy(original, blob, sizeof(blob));
  assert(cr_replace_camera_jpeg(blob, sizeof(blob), jpeg, length) != 0);
  assert(memcmp(blob, original, sizeof(blob)) == 0);

  int capture_width = 0, capture_height = 0;
  assert(cr_camera_jpeg_dimensions(blob, sizeof(blob), &capture_width, &capture_height) != 0);
  memcpy(blob + tail, trailer, sizeof(trailer));
  assert(cr_camera_jpeg_dimensions(blob, sizeof(blob), &capture_width, &capture_height) != 0);
  memcpy(original, blob, sizeof(blob));
  assert(cr_replace_camera_jpeg(blob, sizeof(blob), jpeg, length - 1) != 0);
  assert(cr_replace_camera_jpeg(blob, sizeof(blob), original_jpeg, sizeof(original_jpeg)) != 0);
  assert(memcmp(blob, original, sizeof(blob)) == 0);
  assert(cr_replace_camera_jpeg(blob, sizeof(blob), jpeg, length) == 0);
  assert(memcmp(blob, jpeg, length) == 0);
  assert(blob[tail] == 0xff && blob[tail + 1] == 0x00);
  assert(blob[tail + 2] == 0 && blob[tail + 3] == 0);
  uint32_t written = 0;
  memcpy(&written, blob + tail + 4, sizeof(written));
  assert(written == length);
  assert(cr_camera_jpeg_dimensions(blob, sizeof(blob), &capture_width, &capture_height) == 0);
  assert(capture_width == WIDTH && capture_height == HEIGHT);
  const uint32_t truncated_size = 6;
  memcpy(blob + tail + 4, &truncated_size, sizeof(truncated_size));
  assert(cr_camera_jpeg_dimensions(blob, sizeof(blob), &capture_width, &capture_height) != 0);
  assert(capture_width == 0 && capture_height == 0);

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

  const size_t exact_tail = length + CR_CAMERA_JPEG_ORIENTATION_BYTES;
  const size_t exact_size = exact_tail + CR_CAMERA_JPEG_TRAILER_BYTES;
  assert(exact_size <= sizeof(blob));
  memcpy(blob, rotated_jpeg, sizeof(rotated_jpeg));
  memcpy(blob + exact_tail, trailer, sizeof(trailer));
  memcpy(blob + exact_tail + 4, &original_size, sizeof(original_size));
  assert(cr_replace_camera_jpeg(blob, exact_size, jpeg, length) == 0);
  memcpy(&written, blob + exact_tail + 4, sizeof(written));
  assert(written == exact_tail);
  memcpy(blob, rotated_jpeg, sizeof(rotated_jpeg));
  memcpy(blob + exact_tail - 1, trailer, sizeof(trailer));
  memcpy(blob + exact_tail - 1 + 4, &original_size, sizeof(original_size));
  memcpy(original, blob, exact_size - 1);
  assert(cr_replace_camera_jpeg(blob, exact_size - 1, jpeg, length) != 0);
  assert(memcmp(blob, original, exact_size - 1) == 0);

  memcpy(blob, rotated_jpeg, sizeof(rotated_jpeg));
  blob[8] = 'X'; // A different APP1 type contributes no orientation.
  memcpy(blob + tail, trailer, sizeof(trailer));
  memcpy(blob + tail + 4, &original_size, sizeof(original_size));
  assert(cr_replace_camera_jpeg(blob, sizeof(blob), jpeg, length) == 0);
  assert(memcmp(blob, jpeg, length) == 0);
  free(jpeg);

  memset(nv21, 128, sizeof(nv21));
  assert(cr_nv21_to_jpeg(nv21, sizeof(nv21), WIDTH, HEIGHT, 1024, 85, &jpeg, &length) == 0);
  assert(length < 1024);
  int components = 0;
  uint8_t* pixels = stbi_load_from_memory(jpeg, (int) length, &capture_width, &capture_height, &components, 3);
  assert(pixels && capture_width == WIDTH && capture_height == HEIGHT);
  stbi_image_free(pixels);
  free(jpeg);
}
