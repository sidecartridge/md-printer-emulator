/**
 * File: pclpage.c
 * Description: A LaserJet job drawn page by page into a band of rows
 *              (pclpage.h).
 */
#include "pclpage.h"

#include <string.h>

enum {
  PHASE_SCAN,    // the page's bytes followed, the band simulated
  PHASE_DRAW,    // drawn through the band
  PHASE_WINDOW,  // drawn into one window of rows
  PHASE_FINISH,  // the band's last rows handed on
  PHASE_FLUSH,   // the window's rows handed on
  PHASE_END,
};

// The scan: is anything drawn above where the band would be?
static void scan_pixels(void *ctx, int32_t x, int32_t y, const uint8_t *bits,
                        int32_t count, int32_t scale) {
  (void)x;
  (void)bits;
  (void)count;
  (void)scale;
  PclPage *pp = ctx;
  if (y < 0 || y >= pp->raster->height) return;
  if (y < pp->bandTop) {
    pp->upward = true;
  } else if (y >= pp->bandTop + pp->raster->rows) {
    pp->bandTop = y - pp->raster->rows + 1;
  }
}

static void draw_pixels(void *ctx, int32_t x, int32_t y, const uint8_t *bits,
                        int32_t count, int32_t scale) {
  fx80_raster_pixels(((PclPage *)ctx)->raster, x, y, bits, count, scale);
}

static void page_ended(void *ctx) { ((PclPage *)ctx)->pageEnded = true; }

static void set_sink(PclPage *pp, bool scan) {
  pp->pcl.sink.pixels = scan ? scan_pixels : draw_pixels;
  pp->pcl.sink.page = page_ended;
  pp->pcl.sink.ctx = pp;
}

// The next byte, or -1 at the source's end (or a failed read).
static int next_byte(PclPage *pp) {
  if (pp->pos == pp->length) {
    pp->offset += pp->length;
    pp->pos = 0;
    uint32_t got = 0;
    if (!pp->read(pp->ctx, pp->buffer, pp->bufferSize, &got)) {
      pp->failed = true;
      got = 0;
    }
    pp->length = got;
    if (got == 0) return -1;
  }
  return pp->buffer[pp->pos++];
}

// A page's scan begins here, with the printer as it is now.
static void page_begin(PclPage *pp) {
  pp->start = pp->pcl;
  pp->pageStart = pp->offset + pp->pos;
  pp->bandTop = 0;
  pp->upward = false;
  pp->pageEnded = false;
  set_sink(pp, true);
  pp->phase = PHASE_SCAN;
  pp->passes++;
}

// Back to the page's first byte, the printer as the page began, to draw.
static bool rewind_page(PclPage *pp) {
  pp->pcl = pp->start;
  set_sink(pp, false);
  pp->pageEnded = false;
  pp->passes++;
  if (pp->pageStart >= pp->offset && pp->pageStart <= pp->offset + pp->length) {
    pp->pos = pp->pageStart - pp->offset;  // still in the buffer
    return true;
  }
  if (!pp->seek(pp->ctx, pp->pageStart)) {
    pp->failed = true;
    pp->phase = PHASE_END;
    return false;
  }
  pp->offset = pp->pageStart;
  pp->length = 0;
  pp->pos = 0;
  return true;
}

void pcl_page_init(PclPage *pp, Fx80Raster *raster, int32_t width,
                   int32_t length, int32_t offset, PclRead read, PclSeek seek,
                   void *ctx, uint8_t *buffer, uint32_t bufferSize) {
  memset(pp, 0, sizeof(*pp));
  pp->raster = raster;
  pp->read = read;
  pp->seek = seek;
  pp->ctx = ctx;
  pp->buffer = buffer;
  pp->bufferSize = bufferSize;
  PclSink sink = {scan_pixels, page_ended, pp};
  pcl_init(&pp->pcl, &sink, width, length, offset);
  page_begin(pp);
}

PclPageStep pcl_page_step(PclPage *pp) {
  switch (pp->phase) {
    case PHASE_SCAN:
    case PHASE_DRAW:
    case PHASE_WINDOW: {
      int b = next_byte(pp);
      if (b >= 0) {
        pcl_byte(&pp->pcl, (uint8_t)b);
      } else {
        pcl_end(&pp->pcl);
        if (!pp->pageEnded) {  // nothing more on the page: the job is over
          pp->phase = PHASE_END;
          return PCL_PAGE_END;
        }
      }
      if (!pp->pageEnded) return PCL_PAGE_BUSY;
      if (pp->phase == PHASE_SCAN) {
        if (!rewind_page(pp)) return PCL_PAGE_END;
        if (pp->upward) {
          fx80_raster_window(pp->raster);
          pp->phase = PHASE_WINDOW;
        } else {
          pp->phase = PHASE_DRAW;
        }
      } else {
        pp->phase = pp->phase == PHASE_DRAW ? PHASE_FINISH : PHASE_FLUSH;
      }
      return PCL_PAGE_BUSY;
    }
    case PHASE_FINISH:
      if (!fx80_raster_finish(pp->raster, PCL_PAGE_ROWS)) return PCL_PAGE_BUSY;
      page_begin(pp);
      return PCL_PAGE_DONE;
    case PHASE_FLUSH:
      if (!fx80_raster_windowDone(pp->raster, PCL_PAGE_ROWS)) {
        return PCL_PAGE_BUSY;
      }
      if (pp->raster->top != 0) {  // the next window down the page
        if (!rewind_page(pp)) return PCL_PAGE_END;
        fx80_raster_window(pp->raster);
        pp->phase = PHASE_WINDOW;
        return PCL_PAGE_BUSY;
      }
      page_begin(pp);
      return PCL_PAGE_DONE;
    default:
      return PCL_PAGE_END;
  }
}
