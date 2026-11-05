/**
 * File: raster.h
 * Description: The FX-80's dots (fx80.h) drawn into a 1-bit image of the
 *              page at 240 x 216 dpi, the printer's own finest grid. A dot is
 *              0.28 mm wide and 0.47 mm tall: neighbouring pins' dots touch.
 *
 * The image may hold the whole page or a band of its rows: a dot draws the
 * part of it that falls in the band.
 */
#ifndef FX80_RASTER_H
#define FX80_RASTER_H

#include <stdint.h>

#define FX80_RASTER_DPI_X 240
#define FX80_RASTER_DPI_Y 216

typedef struct {
  uint8_t *bits;   // rows of stride bytes, the left pixel in bit 7; 1 is ink
  int32_t width;   // pixels
  int32_t height;  // pixels of the whole page
  int32_t stride;  // bytes a row
  int32_t top;     // the page row bits starts at
  int32_t rows;    // rows bits holds
} Fx80Raster;

/**
 * @brief The page's size in pixels, from its size in fx80.h's units.
 */
int32_t fx80_raster_width(int32_t xUnits);
int32_t fx80_raster_height(int32_t yUnits);

/**
 * @brief Draws a dot centred at x, y (fx80.h's units).
 */
void fx80_raster_dot(Fx80Raster *r, int32_t x, int32_t y);

#endif  // FX80_RASTER_H
