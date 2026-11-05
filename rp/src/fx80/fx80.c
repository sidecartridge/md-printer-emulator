/**
 * File: fx80.c
 * Description: An Epson FX-80: the ESC/P stream it reads and the dots it
 *              strikes (fx80.h). The commands and their limits are the FX-80
 *              manual's (Appendix D); codes 128-255 print as the Atari ST's
 *              characters, as Atari's SMM804 did, rather than the FX-80's
 *              italic copies, and international set 9 is Norway's.
 *
 * Mode priorities, as the FX-80 sets them: proportional over elite over
 * emphasized over compressed; proportional prints emphasized and never
 * double-strike. Super- and subscript print in two passes, 1/144 inch apart,
 * which the glyph's rows at 1/144 inch are; double-strike adds nothing to
 * them. Expanded prints every dot twice, the second one column of the
 * unexpanded pitch further on.
 */
#include "fx80.h"

#include <string.h>

enum {
  ST_TEXT,
  ST_ESC,    // ESC read, its command next
  ST_ARGS,   // the command's parameters
  ST_IMAGE,  // a bit image's columns
  ST_DEFINE, // RAM characters' attribute and columns
  ST_TABS,   // tab stops up to NUL or a smaller one
  ST_SKIP,   // data of a command this printer does not have
};

enum {
  CELL_RAM = 1 << 0,
  CELL_EMPHASIZED = 1 << 1,
  CELL_DOUBLE = 1 << 2,
  CELL_ITALIC = 1 << 3,
  CELL_UNDERLINE = 1 << 4,
  CELL_SUPERSCRIPT = 1 << 5,
  CELL_SUBSCRIPT = 1 << 6,
  CELL_EXPANDED = 1 << 7,
};

#define NUL 0x00
#define BEL 0x07
#define BS 0x08
#define HT 0x09
#define LF 0x0A
#define VT 0x0B
#define FF 0x0C
#define CR 0x0D
#define SO 0x0E
#define SI 0x0F
#define DC1 0x11
#define DC2 0x12
#define DC3 0x13
#define DC4 0x14
#define CAN 0x18
#define ESC 0x1B
#define DEL 0x7F

#define CELL_COLUMNS 12          // a character's width in its columns
#define EMPHASIZED_SHIFT 6       // 1/240 inch
#define DOUBLE_STRIKE_SHIFT 2    // 1/216 inch
#define SCRIPT_ROW 3             // 1/144 inch between a script glyph's rows
#define SUBSCRIPT_DROP 21        // a subscript's top, below the line's
#define LINE_WIDTH_MAX (8 * FX80_X_PER_INCH)  // 80 pica columns
#define SIXTH_INCH (FX80_Y_PER_INCH / 6)

// The 12 codes an international set (ESC R) replaces, and what each set
// prints there: an Atari code, or one of the two extra glyphs.
static const uint8_t kNational[12] = {0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D,
                                      0x5E, 0x60, 0x7B, 0x7C, 0x7D, 0x7E};
static const uint16_t kSets[10][12] = {
    // USA
    {0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x60, 0x7B, 0x7C, 0x7D, 0x7E},
    // France: # $ à ° ç § ^ ` é ù è ¨
    {0x23, 0x24, 0x85, 0xF8, 0x87, 0xDD, 0x5E, 0x60, 0x82, 0x97, 0x8A, 0xB9},
    // Germany: # $ § Ä Ö Ü ^ ` ä ö ü ß
    {0x23, 0x24, 0xDD, 0x8E, 0x99, 0x9A, 0x5E, 0x60, 0x84, 0x94, 0x81, 0x9E},
    // United Kingdom: £ $ @ [ \ ] ^ ` { | } ~
    {0x9C, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x60, 0x7B, 0x7C, 0x7D, 0x7E},
    // Denmark: # $ @ Æ Ø Å ^ ` æ ø å ~
    {0x23, 0x24, 0x40, 0x92, 0xB2, 0x8F, 0x5E, 0x60, 0x91, 0xB3, 0x86, 0x7E},
    // Sweden: # ¤ É Ä Ö Å Ü é ä ö å ü
    {0x23, FX80_CODE_CURRENCY, 0x90, 0x8E, 0x99, 0x8F, 0x9A, 0x82, 0x84, 0x94,
     0x86, 0x81},
    // Italy: # $ @ ° \ é ^ ù à ò è ì
    {0x23, 0x24, 0x40, 0xF8, 0x5C, 0x82, 0x5E, 0x97, 0x85, 0x95, 0x8A, 0x8D},
    // Spain: Pt $ @ ¡ Ñ ¿ ^ ` ¨ ñ } ~
    {FX80_CODE_PESETA, 0x24, 0x40, 0xAD, 0xA5, 0xA8, 0x5E, 0x60, 0xB9, 0xA4,
     0x7D, 0x7E},
    // Japan: # $ @ [ ¥ ] ^ ` { | } ~
    {0x23, 0x24, 0x40, 0x5B, 0x9D, 0x5D, 0x5E, 0x60, 0x7B, 0x7C, 0x7D, 0x7E},
    // Norway: # ¤ É Æ Ø Å Ü é æ ø å ü
    {0x23, FX80_CODE_CURRENCY, 0x90, 0x92, 0xB2, 0x8F, 0x9A, 0x82, 0x91, 0xB3,
     0x86, 0x81},
};

// The parameters each ESC command takes before any data: the FX-80's, and
// later ESC/P's for the commands it does not have, so their parameters are
// never printed. Commands not listed take none.
static uint8_t esc_args(uint8_t c) {
  switch (c) {
    case '!': case '-': case '/': case '3': case 'A': case 'C': case 'I':
    case 'J': case 'N': case 'Q': case 'R': case 'S': case 'U': case 'W':
    case 'b': case 'i': case 'j': case 'l': case 'p': case 's':
    // later ESC/P
    case ' ': case '+': case 'a': case 'k': case 'm': case 'r': case 't':
    case 'x': case 0x19:
      return 1;
    case 'K': case 'L': case 'Y': case 'Z': case '%':
    // later ESC/P
    case '$': case '?': case '\\': case 'e': case 'f':
      return 2;
    case '*': case '^': case '&': case ':':
      return 3;
    default:
      return 0;
  }
}

static uint16_t pins_from_byte(uint8_t b) {
  uint16_t pins = 0;
  for (int i = 0; i < 8; i++) {
    if (b & (0x80 >> i)) pins |= 1u << i;
  }
  return pins;
}

static void strike(Fx80 *p, int32_t x, int32_t y) {
  if (y < 0 || x < 0) return;
  p->sink.dot(p->sink.ctx, p->originX + x, p->originY + y);
  p->pageDirty = true;
}

static void page_end(Fx80 *p) {
  p->sink.pageEnd(p->sink.ctx);
  p->pageDirty = false;
}

// ---- Modes -------------------------------------------------------------

static int32_t char_step(const Fx80 *p) {
  int32_t step = 12;  // pica, 1/120 inch
  if (p->proportional) {
    step = 12;
  } else if (p->elite) {
    step = 10;  // 1/144 inch
  } else if (p->compressed && !p->emphasized) {
    step = 7;  // 17.14 cpi
  }
  if (p->expanded || p->expandedLine) step *= 2;
  return step;
}

static int32_t char_width(const Fx80 *p) {
  return CELL_COLUMNS * char_step(p);
}

static uint16_t char_flags(const Fx80 *p) {
  uint16_t f = 0;
  if (p->proportional || (p->emphasized && !p->elite)) f |= CELL_EMPHASIZED;
  if (!p->proportional) {
    if (p->script == 1) {
      f |= CELL_SUPERSCRIPT;
    } else if (p->script == 2) {
      f |= CELL_SUBSCRIPT;
    } else if (p->doubleStrike) {
      f |= CELL_DOUBLE;
    }
  }
  if (p->expanded || p->expandedLine) f |= CELL_EXPANDED;
  if (p->italic) f |= CELL_ITALIC;
  if (p->underline) f |= CELL_UNDERLINE;
  if (p->ramSet) f |= CELL_RAM;
  return f;
}

static void default_tabs(Fx80 *p) {
  // Every eight pica columns.
  p->htabs = 0;
  for (int32_t t = 8 * 144; t < LINE_WIDTH_MAX && p->htabs < FX80_HTABS;
       t += 8 * 144) {
    p->htab[p->htabs++] = t;
  }
  memset(p->vtabs, 0, sizeof(p->vtabs));
  p->vtabChannel = 0;
}

static void reset_modes(Fx80 *p) {
  p->elite = p->compressed = p->expanded = p->expandedLine = false;
  p->emphasized = p->doubleStrike = p->italic = p->underline = false;
  p->proportional = false;
  p->script = 0;
  p->international = 0;
  p->highBit = 0;
  p->upperControl = false;
  p->lowPrintable = false;
  p->ramSet = false;
  p->selected = true;
  p->lineSpacing = SIXTH_INCH;
  p->formLength = p->defaultFormLength;
  p->skipPerforation = 0;
  p->leftMargin = 0;
  p->rightMargin = LINE_WIDTH_MAX;
  default_tabs(p);
}

// ---- The line and the paper ----------------------------------------------

static void strike_cell(Fx80 *p, const Fx80Cell *c, uint16_t flags, int32_t x,
                        int32_t y) {
  int32_t again = (flags & CELL_EXPANDED) ? c->step / 2 : 0;
  for (int pass = 0; pass < ((flags & CELL_DOUBLE) ? 2 : 1); pass++) {
    int32_t py = y + pass * DOUBLE_STRIKE_SHIFT;
    strike(p, x, py);
    if (again) strike(p, x + again, py);
    if (flags & CELL_EMPHASIZED) {
      strike(p, x + EMPHASIZED_SHIFT, py);
      if (again) strike(p, x + again + EMPHASIZED_SHIFT, py);
    }
  }
}

static void draw_cell(Fx80 *p, const Fx80Cell *c) {
  const uint16_t *cols =
      (c->flags & CELL_RAM) ? p->ram[c->code & 0xFF] : fx80_font[c->code];
  int32_t x0 = c->x - c->first * c->step;
  for (int col = 0; col < FX80_GLYPH_COLUMNS; col++) {
    uint16_t bits = cols[col];
    for (int pin = 0; bits; pin++, bits >>= 1) {
      if (!(bits & 1)) continue;
      int32_t x = x0 + col * c->step;
      // Italic leans the glyph: half a column per pin above the baseline.
      if (c->flags & CELL_ITALIC) x += (6 - pin) * c->step / 2;
      int32_t y;
      if (c->flags & (CELL_SUPERSCRIPT | CELL_SUBSCRIPT)) {
        y = p->y + pin * SCRIPT_ROW +
            ((c->flags & CELL_SUBSCRIPT) ? SUBSCRIPT_DROP : 0);
      } else {
        y = p->y + pin * FX80_PIN_PITCH;
      }
      strike_cell(p, c, c->flags, x, y);
    }
  }
  if (c->flags & CELL_UNDERLINE) {
    // Pin 9 under the whole cell, every main column.
    for (int32_t dx = 0; dx < c->width; dx += 2 * c->step) {
      strike_cell(p, c, c->flags & ~CELL_DOUBLE, c->x + dx,
                  p->y + (FX80_PINS - 1) * FX80_PIN_PITCH);
    }
  }
}

static void print_line(Fx80 *p) {
  for (uint16_t i = 0; i < p->lineCells; i++) draw_cell(p, &p->line[i]);
  p->lineCells = 0;
}

static void feed(Fx80 *p, int32_t dy) {
  p->y += dy;
  while (p->y >= p->formLength) {
    page_end(p);
    p->y -= p->formLength;
  }
}

static void form_feed(Fx80 *p) {
  print_line(p);
  p->x = p->leftMargin;
  p->expandedLine = false;
  page_end(p);
  p->y = 0;
}

static void line_feed(Fx80 *p) {
  print_line(p);
  p->x = p->leftMargin;
  p->expandedLine = false;
  feed(p, p->lineSpacing);
  if (p->skipPerforation && p->y >= p->formLength - p->skipPerforation) {
    page_end(p);
    p->y = 0;
  }
}

static void print_char(Fx80 *p, uint16_t code) {
  Fx80Cell c = {.code = code, .flags = char_flags(p), .step = char_step(p)};
  if (!(c.flags & CELL_RAM) && p->international) {
    for (int i = 0; i < 12; i++) {
      if (code == kNational[i]) {
        c.code = kSets[p->international][i];
        break;
      }
    }
  }
  c.width = CELL_COLUMNS * c.step;
  if (p->proportional) {
    // As wide as the glyph's dots and a gap of two columns; a blank glyph
    // (the space) ten columns.
    int first = 0, last = -1;
    if (c.flags & CELL_RAM) {
      first = (p->ramAttribute[c.code] >> 4) & 0x07;
      last = (p->ramAttribute[c.code] & 0x0F) - 2;
    } else {
      const uint16_t *cols = fx80_font[c.code];
      first = FX80_GLYPH_COLUMNS;
      for (int col = 0; col < FX80_GLYPH_COLUMNS; col++) {
        if (cols[col]) {
          if (col < first) first = col;
          last = col;
        }
      }
    }
    if (last < first) {
      first = 0;
      c.width = 10 * c.step;
    } else {
      c.width = (last - first + 3) * c.step;
    }
    c.first = (int8_t)first;
  }
  if (p->x + c.width > p->rightMargin) line_feed(p);
  if (p->lineCells == FX80_LINE_CELLS) print_line(p);
  if (p->lineCells == 0) p->lineStart = p->x;
  c.x = p->x;
  p->line[p->lineCells++] = c;
  p->x += c.width;
}

static void horizontal_tab(Fx80 *p) {
  print_line(p);
  for (uint8_t i = 0; i < p->htabs; i++) {
    int32_t t = p->leftMargin + p->htab[i];
    if (t > p->x) {
      if (t < p->rightMargin) p->x = t;
      return;
    }
  }
}

static void vertical_tab(Fx80 *p) {
  uint8_t ch = p->vtabChannel;
  if (p->vtabs[ch] == 0) {
    line_feed(p);
    return;
  }
  for (uint8_t i = 0; i < p->vtabs[ch]; i++) {
    if (p->vtab[ch][i] > p->y) {
      print_line(p);
      p->x = p->leftMargin;
      p->expandedLine = false;
      feed(p, p->vtab[ch][i] - p->y);
      return;
    }
  }
  form_feed(p);
}

// ---- Bit images and RAM characters --------------------------------------

static void image_start(Fx80 *p, int16_t step, bool ninePin, bool noAdjacent,
                        uint32_t columns) {
  print_line(p);
  p->imageStep = step;
  p->imageNinePin = ninePin;
  p->imageNoAdjacent = noAdjacent;
  p->imagePrevious = 0;
  p->dataLeft = columns * (ninePin ? 2 : 1);
  p->dataCount = 0;
  p->state = p->dataLeft ? ST_IMAGE : ST_TEXT;
}

static void image_column(Fx80 *p, uint16_t pins) {
  // At 120 dpi in high speed (ESC Y) a pin cannot fire in two columns in a
  // row: the second dot is not printed.
  if (p->imageNoAdjacent) pins &= ~p->imagePrevious;
  p->imagePrevious = pins;
  if (p->x < p->rightMargin) {
    for (int pin = 0; pins; pin++, pins >>= 1) {
      if (pins & 1) strike(p, p->x, p->y + pin * FX80_PIN_PITCH);
    }
  }
  p->x += p->imageStep;
}

static void image_byte(Fx80 *p, uint8_t b) {
  if (p->imageNinePin) {
    p->dataBytes[p->dataCount++] = b;
    if (p->dataCount == 2) {
      uint16_t pins = pins_from_byte(p->dataBytes[0]);
      if (p->dataBytes[1] & 0x80) pins |= 1u << 8;
      image_column(p, pins);
      p->dataCount = 0;
    }
  } else {
    image_column(p, pins_from_byte(b));
  }
  if (--p->dataLeft == 0) p->state = ST_TEXT;
}

static void define_byte(Fx80 *p, uint8_t b) {
  p->dataBytes[p->dataCount++] = b;
  if (p->dataCount == 12) {
    uint8_t code = p->defineCode++;
    uint8_t attribute = p->dataBytes[0];
    // 128 or more: the top eight pins; less: the bottom eight (descenders).
    int shift = (attribute & 0x80) ? 0 : 1;
    p->ramAttribute[code] = attribute;
    for (int col = 0; col < FX80_GLYPH_COLUMNS; col++) {
      p->ram[code][col] = pins_from_byte(p->dataBytes[1 + col]) << shift;
    }
    p->dataCount = 0;
  }
  if (--p->dataLeft == 0) p->state = ST_TEXT;
}

static void tab_byte(Fx80 *p, uint8_t b) {
  bool end = b == NUL || (p->tabCount > 0 && b <= p->tabLast);
  if (!end) {
    if (p->command == 'D') {
      if (p->htabs < FX80_HTABS) {
        p->htab[p->htabs++] = (int32_t)b * char_width(p);
      }
    } else {
      uint8_t ch = p->tabChannel;
      if (p->vtabs[ch] < FX80_VTABS) {
        p->vtab[ch][p->vtabs[ch]++] = (int32_t)b * p->lineSpacing;
      }
    }
    p->tabCount++;
    p->tabLast = b;
    if (p->tabCount < (p->command == 'D' ? FX80_HTABS : FX80_VTABS)) return;
  }
  p->state = ST_TEXT;
}

static void tabs_start(Fx80 *p, uint8_t channel) {
  p->tabCount = 0;
  p->tabLast = 0;
  if (p->command == 'D') {
    p->htabs = 0;
  } else {
    p->tabChannel = channel & 0x07;
    p->vtabs[p->tabChannel] = 0;
  }
  p->state = ST_TABS;
}

// ---- ESC commands ------------------------------------------------------

static void cancel_line(Fx80 *p) {
  if (p->lineCells) p->x = p->lineStart;
  p->lineCells = 0;
}

static void esc_command(Fx80 *p) {
  uint8_t n = p->argv[0];
  uint32_t columns = p->argv[1] | ((uint32_t)p->argv[2] << 8);
  p->state = ST_TEXT;
  switch (p->command) {
    case '@':
      print_line(p);
      reset_modes(p);
      p->x = p->leftMargin;
      break;
    case '!':
      p->elite = n & 0x01;
      p->compressed = n & 0x04;
      p->emphasized = n & 0x08;
      p->doubleStrike = n & 0x10;
      p->expanded = n & 0x20;
      break;
    case '#': p->highBit = 0; break;
    case '=': p->highBit = 1; break;
    case '>': p->highBit = 2; break;
    case '-': p->underline = n & 1; break;
    case '/': p->vtabChannel = n & 0x07; break;
    case '0': p->lineSpacing = FX80_Y_PER_INCH / 8; break;
    case '1': p->lineSpacing = 7 * FX80_PIN_PITCH; break;
    case '2': p->lineSpacing = SIXTH_INCH; break;
    case '3': p->lineSpacing = 2 * n; break;
    case 'A':
      if (n <= 85) p->lineSpacing = n * FX80_PIN_PITCH;
      break;
    case '4': p->italic = true; break;
    case '5': p->italic = false; break;
    case '6': p->upperControl = false; break;
    case '7': p->upperControl = true; break;
    case 'C': {
      int32_t length = (p->argn == 2) ? (int32_t)p->argv[1] * FX80_Y_PER_INCH
                                      : (int32_t)n * p->lineSpacing;
      if (length >= FX80_PIN_PITCH) p->formLength = length;
      break;
    }
    case 'E': p->emphasized = true; break;
    case 'F': p->emphasized = false; break;
    case 'G': p->doubleStrike = true; break;
    case 'H': p->doubleStrike = false; break;
    case 'I': p->lowPrintable = n & 1; break;
    case 'J':
      print_line(p);
      feed(p, 2 * n);
      break;
    case 'j':
      print_line(p);
      p->y = (p->y > 2 * n) ? p->y - 2 * n : 0;
      break;
    case 'K': image_start(p, 24, false, false, n | (p->argv[1] << 8)); break;
    case 'L': image_start(p, 12, false, false, n | (p->argv[1] << 8)); break;
    case 'Y': image_start(p, 12, false, true, n | (p->argv[1] << 8)); break;
    case 'Z': image_start(p, 6, false, false, n | (p->argv[1] << 8)); break;
    case '*': {
      // Modes 0-6: 60, 120, 120 high speed, 240, 80, 72 and 90 dpi. Later
      // printers' 24-pin modes (32 and up) send three bytes a column.
      static const int16_t kStep[7] = {24, 12, 12, 6, 18, 20, 16};
      if (n < 7) {
        image_start(p, kStep[n], false, n == 2, columns);
      } else {
        p->dataLeft = columns * (n >= 32 ? 3 : 1);
        p->state = p->dataLeft ? ST_SKIP : ST_TEXT;
      }
      break;
    }
    case '^':
      image_start(p, (n & 1) ? 12 : 24, true, false, columns);
      break;
    case '&':
      if (p->argv[2] >= p->argv[1]) {
        p->defineCode = p->argv[1];
        p->dataLeft = (uint32_t)(p->argv[2] - p->argv[1] + 1) * 12;
        p->dataCount = 0;
        p->state = ST_DEFINE;
      }
      break;
    case '%': p->ramSet = n & 1; break;
    case ':':
      for (int code = 0; code < 256; code++) {
        memcpy(p->ram[code], fx80_font[code], sizeof(p->ram[code]));
        p->ramAttribute[code] = 0x8B;  // top pins, columns 0-11
      }
      break;
    case 'B': tabs_start(p, 0); break;
    case 'D': tabs_start(p, 0); break;
    case 'b': tabs_start(p, n); break;
    case 'M': p->elite = true; break;
    case 'P': p->elite = false; break;
    case 'N':
      if (n >= 1) p->skipPerforation = n * p->lineSpacing;
      break;
    case 'O': p->skipPerforation = 0; break;
    case 'Q':
      cancel_line(p);
      if (n >= 1) {
        int32_t right = n * char_width(p);
        p->rightMargin = right < LINE_WIDTH_MAX ? right : LINE_WIDTH_MAX;
      }
      break;
    case 'l': {
      cancel_line(p);
      int32_t left = n * char_width(p);
      if (left < p->rightMargin) p->leftMargin = left;
      p->x = p->leftMargin;
      break;
    }
    case 'R':
      if (n <= 9) p->international = n;
      break;
    case 'S': p->script = (n & 1) ? 2 : 1; break;
    case 'T': p->script = 0; break;
    case 'W': p->expanded = n & 1; break;
    case 'p': p->proportional = n & 1; break;
    case SO: p->expandedLine = true; break;
    case SI: p->compressed = true; break;
    default:
      // Not the FX-80's, or nothing to draw: BEL, the paper-out sensor,
      // unidirectional, half speed, immediate mode, later ESC/P's.
      break;
  }
}

static void esc_args_done(Fx80 *p) {
  // ESC C 0 n: the form length in inches, a second parameter.
  if (p->command == 'C' && p->argn == 1 && p->argv[0] == 0) {
    p->argn = 2;
    return;
  }
  esc_command(p);
}

// ---- Text --------------------------------------------------------------

static void control(Fx80 *p, uint8_t c) {
  switch (c) {
    case BS:
      print_line(p);
      p->x -= char_width(p);
      if (p->x < p->leftMargin) p->x = p->leftMargin;
      break;
    case HT: horizontal_tab(p); break;
    case LF: line_feed(p); break;
    case VT: vertical_tab(p); break;
    case FF: form_feed(p); break;
    case CR:
      print_line(p);
      p->x = p->leftMargin;
      p->expandedLine = false;
      break;
    case SO: p->expandedLine = true; break;
    case SI: p->compressed = true; break;
    case DC2: p->compressed = false; break;
    case DC3: p->selected = false; break;
    case DC4: p->expandedLine = false; break;
    case CAN: cancel_line(p); break;
    case ESC: p->state = ST_ESC; break;
    default: break;  // NUL, BEL, DC1 and the unused codes
  }
}

static bool is_control(const Fx80 *p, uint8_t b) {
  if (b >= 0x80) {
    // ESC 7: 128-159 are the control codes again.
    if (!p->upperControl || b >= 0xA0) return false;
    b &= 0x7F;
  }
  if (b >= 0x20) return false;
  if (!p->lowPrintable) return true;
  // ESC I 1: the codes the FX-80 does not take as commands print.
  switch (b) {
    case BEL: case BS: case HT: case LF: case VT: case FF: case CR: case SO:
    case SI: case DC1: case DC2: case DC3: case DC4: case CAN: case ESC:
      return true;
    default:
      return false;
  }
}

static void text_byte(Fx80 *p, uint8_t b) {
  if (!p->selected) {
    if ((b & 0x7F) == DC1) p->selected = true;
    return;
  }
  if (is_control(p, b)) {
    control(p, b & 0x7F);
    return;
  }
  if (b == DEL) {
    if (p->lineCells) p->x = p->line[--p->lineCells].x;
    return;
  }
  if (p->highBit == 1) b &= 0x7F;
  if (p->highBit == 2) b |= 0x80;
  print_char(p, b);
}

// ---- Entry points ------------------------------------------------------

void fx80_init(Fx80 *p, const Fx80Sink *sink, int32_t paperWidth,
               int32_t paperLength) {
  memset(p, 0, sizeof(*p));
  p->sink = *sink;
  p->originX =
      paperWidth > LINE_WIDTH_MAX ? (paperWidth - LINE_WIDTH_MAX) / 2 : 0;
  p->originY = SIXTH_INCH;
  p->defaultFormLength =
      ((paperLength - p->originY) / SIXTH_INCH) * SIXTH_INCH;
  reset_modes(p);
  p->state = ST_TEXT;
}

void fx80_byte(Fx80 *p, uint8_t b) {
  switch (p->state) {
    case ST_TEXT:
      text_byte(p, b);
      break;
    case ST_ESC:
      p->command = b;
      p->argc = 0;
      p->argn = esc_args(b);
      if (p->argn) {
        p->state = ST_ARGS;
      } else {
        esc_command(p);
      }
      break;
    case ST_ARGS:
      p->argv[p->argc++] = b;
      if (p->argc == p->argn) esc_args_done(p);
      break;
    case ST_IMAGE:
      image_byte(p, b);
      break;
    case ST_DEFINE:
      define_byte(p, b);
      break;
    case ST_TABS:
      tab_byte(p, b);
      break;
    case ST_SKIP:
      if (--p->dataLeft == 0) p->state = ST_TEXT;
      break;
  }
}

void fx80_end(Fx80 *p) {
  print_line(p);
  if (p->pageDirty || p->y > 0) page_end(p);
  p->y = 0;
  p->x = p->leftMargin;
}
