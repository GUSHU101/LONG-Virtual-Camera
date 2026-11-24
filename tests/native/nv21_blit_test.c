#include "nv21_blit.h"

#include <stdint.h>
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
  return 0;
}

int main(void)
{
  const int crop = crop_contract();
  if (crop) return crop;
  return blit_contract();
}
