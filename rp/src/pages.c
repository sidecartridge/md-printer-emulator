/**
 * File: pages.c
 * Description: The printer's jobs rendered into pages (pages.h).
 */
#include "pages.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "ff.h"
#include "fx80/fx80.h"
#include "fx80/raster.h"
#include "md5/md5.h"
#include "pico/stdlib.h"

// A4, 210 x 297 mm, in fx80.h's units.
#define PAPER_WIDTH (210 * FX80_X_PER_INCH * 10 / 254)
#define PAPER_LENGTH (297 * FX80_Y_PER_INCH * 10 / 254)
// The band's buffer, wide enough for Letter (8.5 inches at 240 dpi).
#define BAND_STRIDE ((85 * FX80_RASTER_DPI_X / 10 + 7) / 8)

#define PATH_SIZE 112

static Fx80 printer;
static Fx80Raster raster;
static uint8_t band[PAGES_BAND_ROWS * BAND_STRIDE];
static PagesCounters counters;

static char queue[PAGES_QUEUE][PATH_SIZE];
static uint32_t queueNumber[PAGES_QUEUE];
static uint32_t queueHead;  // the next job to render
static uint32_t queueTail;  // the next free place

static bool active;  // a job's file is open and being rendered
static bool atEnd;   // its last byte has been rendered (fx80_end)
static FIL file;
static uint32_t jobNumber;
static uint8_t buffer[PAGES_READ_SIZE];
static UINT bufferLength;
static UINT bufferPos;
static uint32_t pagesPending;  // pages ended whose rows are not all handed on
static uint32_t jobPages;
static absolute_time_t jobStart;

#if defined(_DEBUG) && (_DEBUG != 0)
static MD5Context pageMd5;

static void on_row(void *ctx, int32_t row, const uint8_t *bits) {
  (void)ctx;
  if (row == 0) md5Init(&pageMd5);
  md5Update(&pageMd5, (uint8_t *)bits, raster.stride);
}

static void page_log(void) {
  md5Finalize(&pageMd5);
  char hex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(&hex[i * 2], 3, "%02x", pageMd5.digest[i]);
  }
  DPRINTF("pages: job %08lu page %lu %ldx%ld MD5 %s\n",
          (unsigned long)jobNumber, (unsigned long)jobPages,
          (long)raster.width, (long)raster.height, hex);
}
#else
static void on_row(void *ctx, int32_t row, const uint8_t *bits) {
  (void)ctx;
  (void)row;
  (void)bits;
}
static void page_log(void) {}
#endif

static void on_dot(void *ctx, int32_t x, int32_t y) {
  (void)ctx;
  fx80_raster_dot(&raster, x, y);
}

// The page has ended; its rows are handed on by pages_loop, before the next
// byte, a few at a time.
static void on_page(void *ctx) {
  (void)ctx;
  pagesPending++;
}

static const Fx80Sink sink = {on_dot, on_page, NULL};

void pages_jobClosed(const char *path, uint32_t number) {
  if (queueTail - queueHead == PAGES_QUEUE) {
    DPRINTF("pages: job %08lu not rendered: %d jobs wait already\n",
            (unsigned long)number, PAGES_QUEUE);
    counters.dropped++;
    return;
  }
  uint32_t slot = queueTail % PAGES_QUEUE;
  snprintf(queue[slot], PATH_SIZE, "%s", path);
  queueNumber[slot] = number;
  queueTail++;
}

static void start(void) {
  uint32_t slot = queueHead % PAGES_QUEUE;
  queueHead++;
  jobNumber = queueNumber[slot];
  FRESULT res = f_open(&file, queue[slot], FA_READ);
  if (res != FR_OK) {
    DPRINTF("pages: cannot open %s (%d)\n", queue[slot], res);
    counters.failed++;
    return;
  }
  fx80_raster_init(&raster, band, fx80_raster_width(PAPER_WIDTH),
                   fx80_raster_height(PAPER_LENGTH), PAGES_BAND_ROWS, on_row,
                   NULL);
  fx80_init(&printer, &sink, PAPER_WIDTH, PAPER_LENGTH);
  active = true;
  atEnd = false;
  bufferLength = bufferPos = 0;
  pagesPending = 0;
  jobPages = 0;
  jobStart = get_absolute_time();
}

static void finish(void) {
  f_close(&file);
  active = false;
  counters.jobs++;
  counters.lost += raster.lost;
  DPRINTF("pages: job %08lu rendered, %lu pages, %lld us, %lu dots lost\n",
          (unsigned long)jobNumber, (unsigned long)jobPages,
          absolute_time_diff_us(jobStart, get_absolute_time()),
          (unsigned long)raster.lost);
}

void pages_loop(void) {
  if (!active) {
    if (queueHead == queueTail) return;
    start();
    return;
  }
  absolute_time_t until = make_timeout_time_us(PAGES_BUDGET_US);
  do {
    if (pagesPending) {
      if (!fx80_raster_finish(&raster, PAGES_FINISH_ROWS)) continue;
      pagesPending--;
      jobPages++;
      counters.pages++;
      page_log();
      continue;
    }
    if (atEnd) {
      finish();
      return;
    }
    if (bufferPos == bufferLength) {
      bufferPos = 0;
      FRESULT res = f_read(&file, buffer, sizeof(buffer), &bufferLength);
      if (res != FR_OK) {
        DPRINTF("pages: job %08lu: read failed (%d)\n",
                (unsigned long)jobNumber, res);
        counters.failed++;
        bufferLength = 0;
      }
      if (bufferLength == 0) {
        fx80_end(&printer);
        atEnd = true;
      }
      continue;
    }
    fx80_byte(&printer, buffer[bufferPos++]);
  } while (absolute_time_diff_us(get_absolute_time(), until) > 0);
}

const PagesCounters *pages_getCounters(void) { return &counters; }
