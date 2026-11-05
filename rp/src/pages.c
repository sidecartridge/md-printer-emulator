/**
 * File: pages.c
 * Description: The printer's jobs rendered into pages and written as PDFs
 *              (pages.h).
 */
#include "pages.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "debug.h"
#include "ff.h"
#include "fx80/fx80.h"
#include "fx80/raster.h"
#include "md5/md5.h"
#include "pdf/pdf.h"
#include "pico/stdlib.h"
#include "printer.h"

// A4, 210 x 297 mm, in fx80.h's units.
#define PAPER_WIDTH (210 * FX80_X_PER_INCH * 10 / 254)
#define PAPER_LENGTH (297 * FX80_Y_PER_INCH * 10 / 254)
// The band's buffer, wide enough for Letter (8.5 inches at 240 dpi).
#define BAND_STRIDE ((85 * FX80_RASTER_DPI_X / 10 + 7) / 8)

#define JOB_DIGITS 8

static Fx80 printer;
static Fx80Raster raster;
static uint8_t band[PAGES_BAND_ROWS * BAND_STRIDE];
static Pdf pdf;
static PagesCounters counters;

static uint32_t queue[PAGES_QUEUE];  // job numbers
static uint32_t queueHead;           // the next job to render
static uint32_t queueTail;           // the next free place
// Jobs on the card wait without their PDF that the queue had no room for:
// once it is empty, the folder is searched for them.
static bool backlog;

static bool active;  // a job's file is open and being rendered
static bool atEnd;   // its last byte has been rendered (fx80_end)
static FIL file;
static FIL pdfFile;
static uint32_t jobNumber;
static uint8_t buffer[PAGES_READ_SIZE];
static UINT bufferLength;
static UINT bufferPos;
static uint32_t pagesPending;  // pages ended whose rows are not all handed on
static uint32_t jobPages;
static absolute_time_t jobStart;
static uint32_t jobLongestUs;

#if defined(_DEBUG) && (_DEBUG != 0)
static MD5Context pageMd5;

static void digest_row(int32_t row, const uint8_t *bits) {
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
          (unsigned long)jobNumber, (unsigned long)jobPages, (long)raster.width,
          (long)raster.height, hex);
}
#else
static void digest_row(int32_t row, const uint8_t *bits) {
  (void)row;
  (void)bits;
}
static void page_log(void) {}
#endif

static bool write_card(void *ctx, const uint8_t *data, uint32_t size) {
  UINT written = 0;
  return f_write(ctx, data, size, &written) == FR_OK && written == size;
}

static void on_row(void *ctx, int32_t row, const uint8_t *bits) {
  (void)ctx;
  if (row == 0) {
    pdf_pageBegin(&pdf, raster.width, raster.height, FX80_RASTER_DPI_X,
                  FX80_RASTER_DPI_Y);
  }
  pdf_row(&pdf, bits);
  digest_row(row, bits);
}

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

static bool keep_raw(void) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_KEEP_RAW);
  return entry != NULL && strcmp(entry->value, "true") == 0;
}

// A job's number from its file's name: eight digits, then the extension.
static bool job_number(const char *name, uint32_t *number) {
  char *end = NULL;
  unsigned long value = strtoul(name, &end, 10);
  if (end != name + JOB_DIGITS || *end != '.') return false;
  *number = (uint32_t)value;
  return true;
}

static bool has_pdf(uint32_t number) {
  char path[PRINTER_PATH_SIZE];
  printer_jobPath(path, sizeof(path), number, PAGES_PDF_EXTENSION);
  return f_stat(path, NULL) == FR_OK;
}

static void enqueue(uint32_t number) {
  if (queueTail - queueHead == PAGES_QUEUE) {
    DPRINTF("pages: job %08lu waits: %d jobs are queued\n",
            (unsigned long)number, PAGES_QUEUE);
    backlog = true;
    return;
  }
  queue[queueTail % PAGES_QUEUE] = number;
  queueTail++;
}

// Queues the oldest jobs on the card without their PDF, as many as fit.
static void scan(void) {
  uint32_t oldest[PAGES_QUEUE];
  int count = 0;
  backlog = false;
  DIR dir;
  FILINFO info;
  FRESULT res =
      f_findfirst(&dir, &info, printer_folder(), "*" PRINTER_JOB_EXTENSION);
  while (res == FR_OK && info.fname[0] != '\0') {
    uint32_t number;
    if (job_number(info.fname, &number) && !has_pdf(number)) {
      int i = count < PAGES_QUEUE ? count++ : PAGES_QUEUE;
      if (i == PAGES_QUEUE) backlog = true;  // one more than fits
      while (i > 0 && oldest[i - 1] > number) {
        if (i < PAGES_QUEUE) oldest[i] = oldest[i - 1];
        i--;
      }
      if (i < PAGES_QUEUE) oldest[i] = number;
    }
    res = f_findnext(&dir, &info);
  }
  f_closedir(&dir);
  for (int i = 0; i < count; i++) enqueue(oldest[i]);
}

// Each file of the folder matching the pattern, one at a time: fix renames or
// deletes it, so the search starts over until none is left.
static void each_leftover(const char *pattern, bool (*fix)(uint32_t)) {
  DIR dir;
  FILINFO info;
  while (f_findfirst(&dir, &info, printer_folder(), pattern) == FR_OK &&
         info.fname[0] != '\0') {
    f_closedir(&dir);
    uint32_t number;
    if (!job_number(info.fname, &number) || !fix(number)) return;
  }
  f_closedir(&dir);
}

// A job a reboot cut short: closed with what it holds.
static bool close_cut_job(uint32_t number) {
  char temp[PRINTER_PATH_SIZE];
  char job[PRINTER_PATH_SIZE];
  printer_jobPath(temp, sizeof(temp), number, PRINTER_TEMP_EXTENSION);
  printer_jobPath(job, sizeof(job), number, PRINTER_JOB_EXTENSION);
  FRESULT res = f_rename(temp, job);
  DPRINTF("pages: job %08lu cut short by a reboot, %s (%d)\n",
          (unsigned long)number, res == FR_OK ? "closed" : "not closed", res);
  return res == FR_OK;
}

// A PDF a reboot cut short: its job renders it again.
static bool delete_cut_pdf(uint32_t number) {
  char temp[PRINTER_PATH_SIZE];
  printer_jobPath(temp, sizeof(temp), number, PAGES_PDF_TEMP_EXTENSION);
  FRESULT res = f_unlink(temp);
  DPRINTF("pages: job %08lu's PDF cut short by a reboot, %s (%d)\n",
          (unsigned long)number, res == FR_OK ? "deleted" : "not deleted", res);
  return res == FR_OK;
}

void pages_init(void) {
  each_leftover("*" PRINTER_TEMP_EXTENSION, close_cut_job);
  each_leftover("*" PAGES_PDF_TEMP_EXTENSION, delete_cut_pdf);
  backlog = true;  // the first pass looks for jobs without their PDF
}

void pages_jobClosed(uint32_t number) { enqueue(number); }

static void start(void) {
  jobNumber = queue[queueHead % PAGES_QUEUE];
  queueHead++;
  char path[PRINTER_PATH_SIZE];
  printer_jobPath(path, sizeof(path), jobNumber, PRINTER_JOB_EXTENSION);
  FRESULT res = f_open(&file, path, FA_READ);
  if (res != FR_OK) {
    DPRINTF("pages: cannot open %s (%d)\n", path, res);
    counters.failed++;
    return;
  }
  printer_jobPath(path, sizeof(path), jobNumber, PAGES_PDF_TEMP_EXTENSION);
  res = f_open(&pdfFile, path, FA_WRITE | FA_CREATE_ALWAYS);
  if (res != FR_OK) {
    DPRINTF("pages: cannot open %s (%d)\n", path, res);
    f_close(&file);
    counters.failed++;
    return;
  }
  pdf_begin(&pdf, write_card, &pdfFile);
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
  jobLongestUs = 0;
}

// The PDF under its final name, and the job deleted unless kept. A job whose
// PDF failed stays, to be rendered again at the next boot; a job that printed
// no page has no PDF.
static void finish(void) {
  f_close(&file);
  active = false;
  bool written = pdf_end(&pdf);
  uint32_t size = (uint32_t)f_size(&pdfFile);
  if (f_close(&pdfFile) != FR_OK) written = false;
  char temp[PRINTER_PATH_SIZE];
  char path[PRINTER_PATH_SIZE];
  printer_jobPath(temp, sizeof(temp), jobNumber, PAGES_PDF_TEMP_EXTENSION);
  printer_jobPath(path, sizeof(path), jobNumber, PAGES_PDF_EXTENSION);
  FRESULT res = FR_OK;
  if (written && jobPages > 0) {
    res = f_rename(temp, path);
    written = res == FR_OK;
  }
  if (!written || jobPages == 0) f_unlink(temp);
  if (!written) {
    DPRINTF("pages: job %08lu: its PDF not written (%d)\n",
            (unsigned long)jobNumber, res);
    counters.failed++;
    return;
  }
  if (!keep_raw()) {
    printer_jobPath(path, sizeof(path), jobNumber, PRINTER_JOB_EXTENSION);
    f_unlink(path);
  }
  counters.jobs++;
  counters.lost += raster.lost;
  DPRINTF(
      "pages: job %08lu rendered, %lu pages, PDF %lu bytes, %lld us, "
      "longest pass %lu us, %lu dots lost\n",
      (unsigned long)jobNumber, (unsigned long)jobPages,
      (unsigned long)(jobPages > 0 ? size : 0),
      absolute_time_diff_us(jobStart, get_absolute_time()),
      (unsigned long)jobLongestUs, (unsigned long)raster.lost);
}

static void render(void) {
  absolute_time_t until = make_timeout_time_us(PAGES_BUDGET_US);
  do {
    if (pagesPending) {
      if (!fx80_raster_finish(&raster, PAGES_FINISH_ROWS)) continue;
      pagesPending--;
      jobPages++;
      counters.pages++;
      pdf_pageEnd(&pdf);
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

void pages_loop(void) {
  if (!active && queueHead == queueTail && !backlog) return;
  absolute_time_t passStart = get_absolute_time();
  if (active) {
    render();
  } else {
    if (queueHead == queueTail) scan();
    if (queueHead != queueTail) start();
  }
  uint32_t us = (uint32_t)absolute_time_diff_us(passStart, get_absolute_time());
  if (us > jobLongestUs) jobLongestUs = us;
  if (us > counters.longestUs) counters.longestUs = us;
}

const PagesCounters *pages_getCounters(void) { return &counters; }
