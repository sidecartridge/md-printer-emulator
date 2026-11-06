/**
 * File: pclpage.h
 * Description: A LaserJet job (pcl.h) drawn page by page into a band of
 *              rows (fx80/raster.h), although PCL may move back up the page.
 *
 * Each page is read twice or more. First its bytes are only followed: the
 * band is simulated, and whether anything would be drawn above it is noted.
 * Then the printer is put back as the page began, its bytes read again, and
 * the page drawn: through the band, as it moves down the page, or, for a page
 * that moves up, window by window, a pass over its bytes for each window of
 * the band's rows. Either way the rows are handed on in order, each once.
 *
 * A draw far below the band (the cursor sent down the page) would hand on
 * every row between at once. The scan notes where that happens, the byte and
 * the band's first row there; drawing, the band is moved there a few rows a
 * step before that byte, so no step takes long.
 */
#ifndef PCL_PAGE_H
#define PCL_PAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "fx80/raster.h"
#include "pcl.h"

#define PCL_PAGE_ROWS 32    // rows handed on at a time
#define PCL_PAGE_JUMPS 256  // the band's long jumps noted a page

// Reads up to size bytes; false if the source failed. *got 0 is its end.
typedef bool (*PclRead)(void *ctx, uint8_t *data, uint32_t size, uint32_t *got);
// Goes back to the source's offset; false if it could not.
typedef bool (*PclSeek)(void *ctx, uint32_t offset);

typedef enum {
  PCL_PAGE_BUSY,  // call again
  PCL_PAGE_DONE,  // a page's rows are all handed on
  PCL_PAGE_END,   // the job is over
} PclPageStep;

typedef struct {
  uint32_t offset;  // the source's offset of the byte that draws
  int32_t top;      // the band's first row it needs
} PclJump;

typedef struct {
  Pcl pcl;
  Pcl start;  // the printer as the page began
  Fx80Raster *raster;
  PclRead read;
  PclSeek seek;
  void *ctx;
  uint8_t *buffer;
  uint32_t bufferSize;
  uint32_t length;     // bytes in buffer
  uint32_t pos;        // the next byte of buffer
  uint32_t offset;     // the source's offset of buffer[0]
  uint32_t pageStart;  // the source's offset of the page's first byte
  uint8_t phase;
  bool pageEnded;                 // the printer has finished the page
  int32_t bandTop;                // the simulated band's first row
  bool upward;                    // the page draws above the band
  PclJump jumps[PCL_PAGE_JUMPS];  // the page's long jumps, from its scan
  uint16_t jumpCount;
  uint16_t jumpNext;  // the next one, drawing
  uint32_t passes;    // passes over pages' bytes, the first scan included
  bool failed;        // the source could not be read or rewound
} PclPage;

/**
 * @brief Sets the job up: the printer powered on (pcl_init's width, length
 * and offset), drawing into raster, its bytes from read and seek through
 * buffer.
 */
void pcl_page_init(PclPage *pp, Fx80Raster *raster, int32_t width,
                   int32_t length, int32_t offset, PclRead read, PclSeek seek,
                   void *ctx, uint8_t *buffer, uint32_t bufferSize);

/**
 * @brief One step: a byte followed, or a few rows handed on.
 */
PclPageStep pcl_page_step(PclPage *pp);

#endif  // PCL_PAGE_H
