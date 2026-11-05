/**
 * File: fx80.h
 * Description: An Epson FX-80 in C: the bytes a program prints go in, the
 *              dots the printer's pins would strike come out, page by page.
 *
 * Positions: x in 1/1440 inch from the paper's left edge, y in 1/432 inch
 * from the top of the page. Every pitch, graphics density and line spacing of
 * the FX-80 falls on that grid: the pins are 1/72 inch apart (6 units), a
 * column of a pica character 1/120 inch (12 units), a 240 dpi graphics column
 * 6 units, a 1/216 inch line feed 2 units.
 *
 * Plain C with no allocation: it builds into the firmware and on a computer
 * (tools/printer).
 */
#ifndef FX80_H
#define FX80_H

#include <stdbool.h>
#include <stdint.h>

#define FX80_X_PER_INCH 1440
#define FX80_Y_PER_INCH 432
#define FX80_PIN_PITCH 6  // y units between two pins
#define FX80_PINS 9

#define FX80_GLYPH_COLUMNS 11
// The font's codes: 0x00-0xff, then 0x100 Pt and 0x101 the currency sign,
// which only the international sets (ESC R) print.
#define FX80_FONT_CODES 0x102
#define FX80_CODE_PESETA 0x100
#define FX80_CODE_CURRENCY 0x101

#define FX80_LINE_CELLS 256  // characters waiting for the line to print
#define FX80_HTABS 32
#define FX80_VTAB_CHANNELS 8
#define FX80_VTABS 16

// Bit n of a column fires pin n + 1 (bit 0 the top pin).
extern const uint16_t fx80_font[FX80_FONT_CODES][FX80_GLYPH_COLUMNS];

typedef struct {
  // A pin strikes the paper, the dot centred at x, y.
  void (*dot)(void *ctx, int32_t x, int32_t y);
  // The page is finished: a form feed, the paper past the form's end, or
  // fx80_end.
  void (*pageEnd)(void *ctx);
  void *ctx;
} Fx80Sink;

typedef struct {
  uint16_t code;   // a font code, or a RAM character's (Fx80Cell flag RAM)
  uint16_t flags;  // the modes it prints in (fx80.c)
  int32_t x;       // where its cell starts
  int16_t step;    // x units between two of its columns
  int16_t width;   // x units to the next cell
  int8_t first;    // its first column, drawn at x (proportional), else 0
} Fx80Cell;

typedef struct {
  Fx80Sink sink;
  int32_t defaultFormLength;  // y units

  // The parser.
  uint8_t state;
  uint8_t command;  // the ESC command being read
  uint8_t argc;     // its parameters read so far
  uint8_t argv[4];
  uint8_t argn;     // its parameters in all
  uint32_t dataLeft;   // bytes of a bit image or a RAM definition still due
  uint8_t dataBytes[12];
  uint8_t dataCount;
  uint8_t defineCode;  // the RAM character being defined
  uint8_t tabCount;
  uint8_t tabLast;
  uint8_t tabChannel;
  bool selected;  // DC3 deselects until DC1

  // The bit image being read.
  int16_t imageStep;
  bool imageNinePin;
  bool imageNoAdjacent;
  uint16_t imagePrevious;

  // The head and the page.
  int32_t x;
  int32_t y;
  int32_t lineSpacing;
  int32_t formLength;
  int32_t skipPerforation;  // y units above the form's end skipped, 0 none
  int32_t leftMargin;
  int32_t rightMargin;
  bool pageDirty;  // dots struck on the page

  // Modes.
  bool elite;
  bool compressed;
  bool expanded;      // ESC W
  bool expandedLine;  // SO, until the line prints
  bool emphasized;
  bool doubleStrike;
  bool italic;
  bool underline;
  bool proportional;
  uint8_t script;  // 0 none, 1 superscript, 2 subscript
  uint8_t international;
  uint8_t highBit;     // 0 as is, 1 cleared (ESC =), 2 set (ESC >)
  bool upperControl;   // ESC 7: 128-159 are control codes
  bool lowPrintable;   // ESC I 1
  bool ramSet;         // ESC % 1: characters from RAM

  // The line waiting to print (CAN and DEL change it).
  Fx80Cell line[FX80_LINE_CELLS];
  uint16_t lineCells;
  int32_t lineStart;  // x when the line began

  int32_t htab[FX80_HTABS];
  uint8_t htabs;
  int32_t vtab[FX80_VTAB_CHANNELS][FX80_VTABS];
  uint8_t vtabs[FX80_VTAB_CHANNELS];
  uint8_t vtabChannel;

  uint16_t ram[256][FX80_GLYPH_COLUMNS];
  uint8_t ramAttribute[256];
} Fx80;

/**
 * @brief Powers the printer on: its modes, the top of the form.
 * @param paperLength the paper's length in y units: the form length starts
 * as the whole lines of 1/6 inch that fit it (A4: 70 lines, Letter: 66).
 */
void fx80_init(Fx80 *p, const Fx80Sink *sink, int32_t paperLength);

/**
 * @brief One byte from the computer.
 */
void fx80_byte(Fx80 *p, uint8_t b);

/**
 * @brief The job is over: prints the line and finishes the page, if anything
 * was printed on it.
 */
void fx80_end(Fx80 *p);

#endif  // FX80_H
