/**
 * File: pcl.h
 * Description: An HP LaserJet II (PCL 4) as it prints a job: the stream's
 *              commands followed byte by byte, its raster graphics drawn
 *              pixel for pixel into a 300 dpi page (the band of
 *              fx80/raster.h), with the cursor, the margins and the page.
 *
 * Positions are kept in 1/3600 inch, which holds PCL's dots (1/300 inch),
 * decipoints (1/720), columns (the HMI, in 1/120) and rows (the VMI, in 1/48)
 * exactly. X runs from the logical page's left edge, which is the paper's
 * less PCL_*_OFFSET dots; Y from the top margin, which is the paper's top
 * edge plus the top margin. Text is drawn in the built-in fonts of
 * pclfont.h, its baseline at the cursor, the font picked by its pitch and
 * stroke weight as the LaserJet II picks among its own.
 */
#ifndef PCL_H
#define PCL_H

#include <stdbool.h>
#include <stdint.h>

#define PCL_DPI 300
#define PCL_UNITS 3600     // a position's units an inch
#define PCL_A4_WIDTH 2480  // the paper, in dots
#define PCL_A4_LENGTH 3507
#define PCL_A4_OFFSET 71  // the logical page's left edge, in dots
#define PCL_LETTER_WIDTH 2550
#define PCL_LETTER_LENGTH 3300
#define PCL_LETTER_OFFSET 75
#define PCL_ROW_BYTES 320  // a raster row's bytes kept: Letter at 300 dpi
#define PCL_VALUE_SIZE 12  // a parameter's characters kept

typedef struct {
  // Inks count pixels of bits (the first in bit 7) from dot x of page row y,
  // each repeated scale times across (fx80_raster_pixels).
  void (*pixels)(void *ctx, int32_t x, int32_t y, const uint8_t *bits,
                 int32_t count, int32_t scale);
  // The page is finished: a form feed, a reset or the end of the text area.
  void (*page)(void *ctx);
  void *ctx;
} PclSink;

typedef struct {
  int32_t symbolSet;  // its value x 32 + its letter's (8U is 277)
  int32_t pitch;      // characters an inch, x 100
  uint8_t weight;     // stroke weight: 0 medium, more bold
  uint8_t font;       // the built-in font it picks (pcl_fonts)
} PclFontSelect;

typedef struct {
  PclSink sink;
  int32_t length;  // the paper's length, units
  int32_t width;   // the logical page's width, units
  int32_t offset;  // the logical page's left edge on the paper, dots

  // The parser.
  uint8_t state;
  char param;  // ESC's parameter character, '&' in ESC &l1O
  char group;  // its group character, 'l' in ESC &l1O, or 0
  char value[PCL_VALUE_SIZE];
  uint8_t valueLength;
  uint32_t dataLeft;  // bytes of a command's data still to come
  uint8_t dataKind;   // a raster row, characters, or skipped
  bool moreValues;    // the command ended in lowercase: another follows

  // The page.
  int32_t x, y;  // the cursor, units
  int32_t hmi;   // a column, units
  int32_t vmi;   // a line, units
  int32_t topMargin;
  int32_t textLength;
  int32_t leftMargin;
  int32_t rightMargin;
  bool perforationSkip;
  uint8_t lineTermination;  // ESC &k#G
  bool wrap;                // past the right margin onto the next line
  bool underline;
  PclFontSelect fonts[2];  // the primary and the secondary
  uint8_t active;          // the font in use: 0 primary, 1 secondary
  bool pageDirty;

  // Raster graphics.
  int32_t resolution;  // dpi
  bool rasterOn;
  int32_t rasterX;  // the left raster margin, units
  uint8_t compression;
  uint8_t row[PCL_ROW_BYTES];  // the row being decoded: the seed row
  uint32_t rowLength;          // its bytes
  uint32_t rowPos;             // where the next byte goes
  uint8_t runState;            // inside a compressed row
  uint32_t runCount;

  uint32_t missing;  // characters with no glyph in the font
  uint32_t ignored;  // commands not emulated
} Pcl;

/**
 * @brief Powers the printer on: its modes, the top of the page.
 * @param width, length the paper, in dots; offset the logical page's left
 * edge on it, in dots.
 */
void pcl_init(Pcl *p, const PclSink *sink, int32_t width, int32_t length,
              int32_t offset);

/**
 * @brief One byte from the computer.
 */
void pcl_byte(Pcl *p, uint8_t b);

/**
 * @brief The job is over: finishes the page, if anything was printed on it.
 */
void pcl_end(Pcl *p);

/**
 * @brief Whether a job's first bytes are PCL: its first escape sequence,
 * past any reset (ESC E, which ESC/P has too), is PCL's alone (ESC &, ESC *,
 * ESC ( or ESC % followed by a lowercase letter, a digit or a minus), which
 * ESC/P never sends.
 */
bool pcl_recognise(const uint8_t *data, uint32_t length);

#endif  // PCL_H
