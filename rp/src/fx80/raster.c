/**
 * File: raster.c
 * Description: The FX-80's dots drawn into a 1-bit page (raster.h).
 */
#include "raster.h"

#include "fx80.h"

// A pixel is 6 x 2 units (1/240 by 1/216 inch). A dot is 16 units across and
// 8 down (0.28 by 0.47 mm): the dots of neighbouring pins touch, so strokes
// print solid, while the columns of a 120 dpi image stay apart enough for its
// dithered greys to print as greys. A pixel is inked when its centre is
// inside the dot.
#define UNITS_X (FX80_X_PER_INCH / FX80_RASTER_DPI_X)
#define UNITS_Y (FX80_Y_PER_INCH / FX80_RASTER_DPI_Y)
#define RADIUS_X 8
#define RADIUS_Y 4

int32_t fx80_raster_width(int32_t xUnits) {
  return (xUnits + UNITS_X - 1) / UNITS_X;
}

int32_t fx80_raster_height(int32_t yUnits) {
  return (yUnits + UNITS_Y - 1) / UNITS_Y;
}

void fx80_raster_dot(Fx80Raster *r, int32_t x, int32_t y) {
  int32_t i0 = (x - RADIUS_X) / UNITS_X - 1;
  int32_t i1 = (x + RADIUS_X) / UNITS_X + 1;
  int32_t j0 = (y - RADIUS_Y) / UNITS_Y - 1;
  int32_t j1 = (y + RADIUS_Y) / UNITS_Y + 1;
  if (i0 < 0) i0 = 0;
  if (i1 >= r->width) i1 = r->width - 1;
  if (j0 < r->top) j0 = r->top;
  if (j1 >= r->top + r->rows) j1 = r->top + r->rows - 1;
  if (j1 >= r->height) j1 = r->height - 1;
  for (int32_t j = j0; j <= j1; j++) {
    int32_t dy = j * UNITS_Y + UNITS_Y / 2 - y;
    uint8_t *row = r->bits + (j - r->top) * r->stride;
    for (int32_t i = i0; i <= i1; i++) {
      int32_t dx = i * UNITS_X + UNITS_X / 2 - x;
      if (RADIUS_Y * RADIUS_Y * dx * dx + RADIUS_X * RADIUS_X * dy * dy <=
          RADIUS_X * RADIUS_X * RADIUS_Y * RADIUS_Y) {
        row[i >> 3] |= 0x80 >> (i & 7);
      }
    }
  }
}
