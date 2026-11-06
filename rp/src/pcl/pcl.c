/**
 * File: pcl.c
 * Description: An HP LaserJet II (PCL 4) as it prints a job (pcl.h).
 *
 * A parameterized command is ESC, a parameter character (! to /), an
 * optional group character (` to ~), then values, each a number ended by a
 * letter: lowercase when another value of the same group follows (ESC &l1o66P
 * is ESC &l1O and ESC &l66P), uppercase for the last. A value with a sign is
 * relative where the command moves the cursor.
 *
 * Measured against GhostPCL: after a reset the cursor is at the logical
 * page's left edge, 3/4 of a line below the top margin, and a raster starts
 * where the cursor is, its rows going down from there. A raster row, even
 * one of no ink, makes the page one that is printed.
 */
#include "pcl.h"

#include <string.h>

enum {
  ST_TEXT,
  ST_ESC,    // after ESC
  ST_PARAM,  // after the parameter character
  ST_VALUE,  // in a value
  ST_DATA,   // the data of a command: a raster row, or skipped
};

enum {
  RUN_START,    // a count or a command byte comes next
  RUN_REPEAT,   // the byte to repeat comes next
  RUN_LITERAL,  // runCount bytes as they are
  RUN_OFFSET,   // more of a delta row's offset
};

#define DOT (PCL_UNITS / PCL_DPI)
#define DECIPOINT (PCL_UNITS / 720)
#define VMI_UNIT (PCL_UNITS / 48)       // ESC &l#C
#define HMI_UNIT (PCL_UNITS / 120)      // ESC &k#H
#define HMI_DEFAULT (PCL_UNITS / 10)    // Courier: 10 characters an inch
#define VMI_DEFAULT (PCL_UNITS / 6)     // 6 lines an inch
#define MARGIN_DEFAULT (PCL_UNITS / 2)  // the top margin, and below the text
#define TAB_COLUMNS 8
#define ESC 0x1B

typedef struct {
  bool relative;  // it had a sign
  int32_t milli;  // the value, x 1000
} Value;

static Value parse(const Pcl *p) {
  Value v = {false, 0};
  bool negative = false;
  int32_t scale = 0;  // decimals' place, once past the point
  for (uint8_t i = 0; i < p->valueLength; i++) {
    char c = p->value[i];
    if (c == '+' || c == '-') {
      v.relative = true;
      negative = c == '-';
    } else if (c == '.') {
      scale = 100;
    } else if (scale == 0) {
      if (v.milli < 100000000) v.milli = v.milli * 10 + (c - '0') * 1000;
    } else {
      v.milli += (c - '0') * scale;
      scale /= 10;
    }
  }
  if (negative) v.milli = -v.milli;
  return v;
}

static int32_t whole(Value v) { return v.milli / 1000; }

// pos = the value in unit, or moved by it if it is relative.
static void move(int32_t *pos, Value v, int32_t unit) {
  int32_t d = (int32_t)((int64_t)v.milli * unit / 1000);
  *pos = v.relative ? *pos + d : d;
}

static int32_t first_line(const Pcl *p) { return p->vmi * 3 / 4; }

// Units to dots, to the nearest, a half down: as GhostPCL places them.
static int32_t dots(int32_t units) {
  return units >= 0 ? (units + DOT / 2 - 1) / DOT : -((-units + DOT / 2) / DOT);
}

static void reset(Pcl *p) {
  p->hmi = HMI_DEFAULT;
  p->vmi = VMI_DEFAULT;
  p->topMargin = MARGIN_DEFAULT;
  p->textLength = p->length - p->topMargin - MARGIN_DEFAULT;
  p->leftMargin = 0;
  p->rightMargin = p->width;
  p->perforationSkip = true;
  p->lineTermination = 0;
  p->resolution = 75;
  p->rasterOn = false;
  p->compression = 0;
  p->x = 0;
  p->y = first_line(p);
}

static void page_end(Pcl *p) {
  p->sink.page(p->sink.ctx);
  p->pageDirty = false;
  p->rasterOn = false;
  p->x = p->leftMargin;
  p->y = first_line(p);
}

// Past the text's foot the page ends. With the perforation skip off the text
// runs on to the paper's foot, and the next page's text from its top edge.
static void line_feed(Pcl *p) {
  p->y += p->vmi;
  if (p->perforationSkip) {
    if (p->y > p->textLength) page_end(p);
  } else if (p->y > p->length - p->topMargin) {
    page_end(p);
    p->y = first_line(p) - p->topMargin;
  }
}

// ---- Raster graphics ---------------------------------------------------

static void raster_start(Pcl *p, bool atCursor) {
  if (p->rasterOn) return;
  p->rasterOn = true;
  p->rasterX = atCursor ? p->x : 0;
  memset(p->row, 0, sizeof(p->row));
  p->rowLength = 0;
}

static void row_begin(Pcl *p) {
  if (!p->rasterOn) raster_start(p, false);
  if (p->compression != 3) {  // a delta row changes the last row
    memset(p->row, 0, p->rowLength);
    p->rowLength = 0;
  }
  p->rowPos = 0;
  p->runState = RUN_START;
}

static void put(Pcl *p, uint8_t b) {
  if (p->rowPos < PCL_ROW_BYTES) {
    p->row[p->rowPos] = b;
    if (p->rowPos + 1 > p->rowLength) p->rowLength = p->rowPos + 1;
  }
  p->rowPos++;
}

static void row_byte(Pcl *p, uint8_t b) {
  switch (p->compression) {
    case 1:  // run-length: a count less one, then the byte
      if (p->runState == RUN_START) {
        p->runCount = b + 1u;
        p->runState = RUN_REPEAT;
      } else {
        while (p->runCount--) put(p, b);
        p->runState = RUN_START;
      }
      break;
    case 2:  // TIFF: n + 1 bytes as they are, or a byte 1 - n times
      if (p->runState == RUN_START) {
        if (b < 128) {
          p->runCount = b + 1u;
          p->runState = RUN_LITERAL;
        } else if (b > 128) {
          p->runCount = 257u - b;
          p->runState = RUN_REPEAT;
        }
      } else if (p->runState == RUN_LITERAL) {
        put(p, b);
        if (--p->runCount == 0) p->runState = RUN_START;
      } else {
        while (p->runCount--) put(p, b);
        p->runState = RUN_START;
      }
      break;
    case 3:  // delta row: a command byte (bytes to replace, offset), them
      if (p->runState == RUN_START) {
        p->runCount = (b >> 5) + 1u;
        p->rowPos += b & 0x1F;
        p->runState = (b & 0x1F) == 0x1F ? RUN_OFFSET : RUN_LITERAL;
      } else if (p->runState == RUN_OFFSET) {
        p->rowPos += b;
        if (b != 0xFF) p->runState = RUN_LITERAL;
      } else {
        put(p, b);
        if (--p->runCount == 0) p->runState = RUN_START;
      }
      break;
    default:
      put(p, b);
  }
}

// The row is whole: drawn as many rows tall as its resolution needs.
static void row_end(Pcl *p) {
  int32_t scale = PCL_DPI / p->resolution;
  int32_t x = p->offset + dots(p->rasterX);
  int32_t y = dots(p->topMargin + p->y);
  bool ink = false;
  for (uint32_t i = 0; i < p->rowLength && !ink; i++) ink = p->row[i] != 0;
  if (ink) {
    for (int32_t k = 0; k < scale; k++) {
      p->sink.pixels(p->sink.ctx, x, y + k, p->row, (int32_t)p->rowLength * 8,
                     scale);
    }
  }
  p->pageDirty = true;  // a row of no ink too: the page is printed
  p->y += scale * DOT;
}

// ---- Commands ----------------------------------------------------------

// The value is the length of data that follows.
static void data(Pcl *p, Value v, bool raster) {
  p->dataLeft = v.milli > 0 ? (uint32_t)whole(v) : 0;
  p->dataRaster = raster;
  if (raster) {
    row_begin(p);
    if (p->dataLeft == 0) row_end(p);
  }
}

static void parameterized(Pcl *p, char end) {
  Value v = parse(p);
  int32_t n = whole(v);
  switch (p->param << 8 | p->group) {
    case '&' << 8 | 'l':
      switch (end) {
        case 'E':  // top margin, lines; the text below it as at reset
          p->topMargin = n * p->vmi;
          p->textLength = p->length - p->topMargin - MARGIN_DEFAULT;
          return;
        case 'F':  // text length, lines
          p->textLength = n * p->vmi;
          return;
        case 'D':  // lines an inch
          if (n > 0) p->vmi = PCL_UNITS / n;
          return;
        case 'C':  // VMI, 1/48 inch
          p->vmi = (int32_t)((int64_t)v.milli * VMI_UNIT / 1000);
          return;
        case 'L':
          p->perforationSkip = n != 0;
          return;
        case 'X':  // copies: one PDF
          return;
        case 'H':  // paper source: 0 ejects the page
          if (n == 0 && p->pageDirty) page_end(p);
          return;
        default:  // orientation, paper size, page length...
          p->ignored++;
          return;
      }
    case '&' << 8 | 'k':
      if (end == 'H') {
        p->hmi = (int32_t)((int64_t)v.milli * HMI_UNIT / 1000);
      } else if (end == 'G') {
        p->lineTermination = (uint8_t)(n & 3);
      } else {
        p->ignored++;
      }
      return;
    case '&' << 8 | 'a':
      switch (end) {
        case 'C':
          move(&p->x, v, p->hmi);
          break;
        case 'R':
          if (v.relative) {
            p->y += (int32_t)((int64_t)v.milli * p->vmi / 1000);
          } else {
            p->y = (int32_t)((int64_t)v.milli * p->vmi / 1000) + first_line(p);
          }
          break;
        case 'H':
          move(&p->x, v, DECIPOINT);
          break;
        case 'V':
          move(&p->y, v, DECIPOINT);
          break;
        case 'L':
          p->leftMargin = n * p->hmi;
          break;
        case 'M':
          p->rightMargin = (n + 1) * p->hmi;
          break;
        default:
          p->ignored++;
      }
      if (p->x < 0) p->x = 0;
      return;
    case '&' << 8 | 'p':
      if (end == 'X') {  // transparent print: characters, not drawn yet
        p->text += (uint32_t)(n > 0 ? n : 0);
        data(p, v, false);
        return;
      }
      p->ignored++;
      return;
    case '*' << 8 | 'p':
      if (end == 'X') {
        move(&p->x, v, DOT);
      } else if (end == 'Y') {
        move(&p->y, v, DOT);
      } else {
        p->ignored++;
      }
      if (p->x < 0) p->x = 0;
      return;
    case '*' << 8 | 't':
      if (end == 'R' && !p->rasterOn) {
        p->resolution = n <= 75 ? 75 : n <= 100 ? 100 : n <= 150 ? 150 : 300;
      }
      return;
    case '*' << 8 | 'r':
      if (end == 'A') {
        raster_start(p, n == 1);
      } else if (end == 'B') {
        p->rasterOn = false;
      } else {
        p->ignored++;
      }
      return;
    case '*' << 8 | 'b':
      if (end == 'W') {
        data(p, v, true);
      } else if (end == 'M') {
        p->compression = (uint8_t)(n >= 0 && n <= 3 ? n : 0);
      } else if (end == 'Y') {  // rows of no ink
        p->y += n * (PCL_DPI / p->resolution) * DOT;
        memset(p->row, 0, sizeof(p->row));
      } else {
        p->ignored++;
      }
      return;
    default:
      break;
  }
  // Not emulated: rectangles, fonts, macros... The ones with data skip it.
  p->ignored++;
  if (end == 'W' || (p->param == '&' && p->group == 'p' && end == 'X')) {
    data(p, v, false);
  }
}

static void two_character(Pcl *p, uint8_t c) {
  switch (c) {
    case 'E':  // reset
      if (p->pageDirty) page_end(p);
      reset(p);
      break;
    case '9':  // clear the left and right margins
      p->leftMargin = 0;
      p->rightMargin = p->width;
      break;
    case '=':  // half a line feed
      p->y += p->vmi / 2;
      break;
    default:  // display functions, ...
      p->ignored++;
  }
}

static void text(Pcl *p, uint8_t b) {
  switch (b) {
    case '\r':
      p->x = p->leftMargin;
      if (p->lineTermination & 1) line_feed(p);
      return;
    case '\n':
      if (p->lineTermination & 2) p->x = p->leftMargin;
      line_feed(p);
      return;
    case '\f':
      page_end(p);
      return;
    case '\b':
      p->x = p->x > p->hmi ? p->x - p->hmi : 0;
      return;
    case '\t': {
      int32_t tab = TAB_COLUMNS * p->hmi;
      p->x = p->leftMargin + ((p->x - p->leftMargin) / tab + 1) * tab;
      return;
    }
    default:
      break;
  }
  if (b < 0x20) return;  // SO, SI and the rest: no fonts yet
  if (b != ' ') {
    p->text++;
    p->pageDirty = true;
  }
  p->x += p->hmi;
}

// ---- Entry points ------------------------------------------------------

void pcl_init(Pcl *p, const PclSink *sink, int32_t width, int32_t length,
              int32_t offset) {
  memset(p, 0, sizeof(*p));
  p->sink = *sink;
  p->length = length * DOT;
  p->width = (width - 2 * offset) * DOT;
  p->offset = offset;
  reset(p);
  p->state = ST_TEXT;
}

void pcl_byte(Pcl *p, uint8_t b) {
  switch (p->state) {
    case ST_TEXT:
      if (b == ESC) {
        p->state = ST_ESC;
      } else {
        text(p, b);
      }
      return;
    case ST_ESC:
      if (b >= 0x21 && b <= 0x2F) {
        p->param = (char)b;
        p->state = ST_PARAM;
      } else {
        p->state = ST_TEXT;
        if (b >= 0x30 && b <= 0x7E) two_character(p, b);
      }
      return;
    case ST_PARAM:
      p->valueLength = 0;
      p->state = ST_VALUE;
      if (b >= 0x60 && b <= 0x7E) {
        p->group = (char)b;
        return;
      }
      p->group = 0;
      break;  // a value's first character, below
    case ST_DATA:
      if (p->dataRaster) row_byte(p, b);
      if (--p->dataLeft == 0) {
        if (p->dataRaster) row_end(p);
        p->valueLength = 0;
        p->state = p->moreValues ? ST_VALUE : ST_TEXT;
      }
      return;
    default:
      break;
  }
  // ST_VALUE
  if ((b >= '0' && b <= '9') || b == '+' || b == '-' || b == '.') {
    if (p->valueLength < PCL_VALUE_SIZE) p->value[p->valueLength++] = (char)b;
    return;
  }
  bool upper = b >= 0x40 && b <= 0x5E;
  if (!upper && !(b >= 0x60 && b <= 0x7E)) {  // not a command: abandoned
    p->state = ST_TEXT;
    pcl_byte(p, b);
    return;
  }
  p->moreValues = !upper;
  p->state = ST_TEXT;
  parameterized(p, (char)(upper ? b : b - 0x20));
  if (p->state == ST_TEXT && p->dataLeft > 0) {
    p->state = ST_DATA;
  } else if (p->moreValues) {
    p->state = ST_VALUE;
  }
  p->valueLength = 0;
}

void pcl_end(Pcl *p) {
  if (p->pageDirty) page_end(p);
}

bool pcl_recognise(const uint8_t *data, uint32_t length) {
  for (uint32_t i = 0; i + 2 < length; i++) {
    if (data[i] != ESC) continue;
    uint8_t c = data[i + 1];
    if (c == 'E') {  // a reset: both have it
      i++;
      continue;
    }
    uint8_t next = data[i + 2];
    bool pclNext = (next >= 'a' && next <= 'z') ||
                   (next >= '0' && next <= '9') || next == '-';
    return (c == '&' || c == '*' || c == '(' || c == '%') && pclNext;
  }
  return false;
}
