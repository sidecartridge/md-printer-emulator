/**
 * File: raster.h
 * Description: The FX-80's dots (fx80.h) drawn into a 1-bit image of the
 *              page at 240 x 216 dpi, the printer's own finest grid. A dot is
 *              0.28 mm wide and 0.47 mm tall: neighbouring pins' dots touch.
 *
 * The image is a band of rows that moves down the page: its buffer holds
 * `rows` rows, used as a ring, and the rows are handed on (rowOut) in order,
 * each exactly once, as the dots move below the band, then the rest at the
 * page's end. A buffer of the whole page's rows never moves. The paper fed
 * back (ESC j) past the band's top loses the dots that fall above it; they
 * are counted.
 */
#ifndef FX80_RASTER_H
#define FX80_RASTER_H

#include <stdbool.h>
#include <stdint.h>

#define FX80_RASTER_DPI_X 240
#define FX80_RASTER_DPI_Y 216

// A finished row: bits as in Fx80Raster, page row `row`.
typedef void (*Fx80RowOut)(void *ctx, int32_t row, const uint8_t *bits);

typedef struct {
  uint8_t *bits;   // rows of stride bytes, the left pixel in bit 7; 1 is ink
  int32_t width;   // pixels
  int32_t height;  // pixels of the whole page
  int32_t stride;  // bytes a row
  int32_t rows;    // rows bits holds; page row j is at j % rows
  int32_t top;     // the first row not handed on yet
  Fx80RowOut rowOut;
  void *ctx;
  uint32_t lost;   // dots that fell above the band
  int32_t windowEnd;  // in a window: the row past it, else 0
} Fx80Raster;

/**
 * @brief The page's size in pixels, from its size in fx80.h's units.
 */
int32_t fx80_raster_width(int32_t xUnits);
int32_t fx80_raster_height(int32_t yUnits);

/**
 * @brief Sets the raster up over bits (rows x stride bytes, cleared here).
 */
void fx80_raster_init(Fx80Raster *r, uint8_t *bits, int32_t width,
                      int32_t height, int32_t rows, Fx80RowOut rowOut,
                      void *ctx);

/**
 * @brief Draws a dot centred at x, y (fx80.h's units). Rows above it that
 * leave the band are handed on first.
 */
void fx80_raster_dot(Fx80Raster *r, int32_t x, int32_t y);

/**
 * @brief Inks pixels of page row y from pixel x: count pixels of bits, the
 * first in bit 7 of bits[0], each repeated scale times across (another
 * printer's image, drawn pixel for pixel; a LaserJet's at 150 dpi on a 300
 * dpi page). Pixels past the page's edges are left out. Rows above it that
 * leave the band are handed on first; inked pixels above the band are lost
 * (counted).
 */
void fx80_raster_pixels(Fx80Raster *r, int32_t x, int32_t y,
                        const uint8_t *bits, int32_t count, int32_t scale);

/**
 * @brief Hands on rows until row is the band's first, up to maxRows of them:
 * a long jump down the page made a little at a time.
 * @return true once row is reached.
 */
bool fx80_raster_advance(Fx80Raster *r, int32_t row, int32_t maxRows);

/**
 * @brief A window of the page: from the first row not handed on, as many
 * rows as the band holds. Only what falls in it is drawn; nothing else is
 * lost, nor is a row handed on before fx80_raster_windowDone. A page drawn
 * window by window, a pass over its bytes for each, comes out whole however
 * its printer moved up and down.
 */
void fx80_raster_window(Fx80Raster *r);

/**
 * @brief The window's pass is over: hands on up to maxRows of its rows.
 * @return true once they are all handed on; the next window then starts at
 * the next row (fx80_raster_window), or, at the page's foot, the raster is
 * at the top of a blank page.
 */
bool fx80_raster_windowDone(Fx80Raster *r, int32_t maxRows);

/**
 * @brief The page's end: hands on up to maxRows of the rows left.
 * @return true once every row of the page is handed on; the raster is then
 * at the top of a blank page.
 */
bool fx80_raster_finish(Fx80Raster *r, int32_t maxRows);

#endif  // FX80_RASTER_H
