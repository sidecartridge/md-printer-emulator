/**
 * File: raster.c
 * Description: The FX-80's dots drawn into a band of a 1-bit page (raster.h).
 */
#include "raster.h"

#include <string.h>

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

void fx80_raster_init(Fx80Raster *r, uint8_t *bits, int32_t width,
                      int32_t height, int32_t rows, Fx80RowOut rowOut,
                      void *ctx) {
  memset(r, 0, sizeof(*r));
  r->bits = bits;
  r->width = width;
  r->height = height;
  r->stride = (width + 7) / 8;
  r->rows = rows;
  r->rowOut = rowOut;
  r->ctx = ctx;
  memset(bits, 0, (size_t)rows * r->stride);
}

static uint8_t *row_bits(Fx80Raster *r, int32_t row) {
  return r->bits + (size_t)(row % r->rows) * r->stride;
}

// Hands on the band's top row and clears it for the row `rows` below.
static void hand_on(Fx80Raster *r) {
  uint8_t *bits = row_bits(r, r->top);
  r->rowOut(r->ctx, r->top, bits);
  memset(bits, 0, r->stride);
  r->top++;
}

void fx80_raster_dot(Fx80Raster *r, int32_t x, int32_t y) {
  int32_t i0 = (x - RADIUS_X) / UNITS_X - 1;
  int32_t i1 = (x + RADIUS_X) / UNITS_X + 1;
  int32_t j0 = (y - RADIUS_Y) / UNITS_Y - 1;
  int32_t j1 = (y + RADIUS_Y) / UNITS_Y + 1;
  if (i0 < 0) i0 = 0;
  if (i1 >= r->width) i1 = r->width - 1;
  if (j0 < 0) j0 = 0;
  if (j1 >= r->height) j1 = r->height - 1;
  if (j1 < r->top) {
    r->lost++;
    return;
  }
  if (j0 < r->top) j0 = r->top;
  while (j1 >= r->top + r->rows) hand_on(r);
  for (int32_t j = j0; j <= j1; j++) {
    int32_t dy = j * UNITS_Y + UNITS_Y / 2 - y;
    uint8_t *row = row_bits(r, j);
    for (int32_t i = i0; i <= i1; i++) {
      int32_t dx = i * UNITS_X + UNITS_X / 2 - x;
      if (RADIUS_Y * RADIUS_Y * dx * dx + RADIUS_X * RADIUS_X * dy * dy <=
          RADIUS_X * RADIUS_X * RADIUS_Y * RADIUS_Y) {
        row[i >> 3] |= 0x80 >> (i & 7);
      }
    }
  }
}

bool fx80_raster_finish(Fx80Raster *r, int32_t maxRows) {
  while (r->top < r->height && maxRows-- > 0) hand_on(r);
  if (r->top < r->height) return false;
  r->top = 0;
  return true;
}
