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
#include <strings.h>

#include "aconfig.h"
#include "debug.h"
#include "ff.h"
#include "fx80/fx80.h"
#include "fx80/raster.h"
#include "md5/md5.h"
#include "pcl/pcl.h"
#include "pcl/pclpage.h"
#include "pdf/pdf.h"
#include "pico/stdlib.h"
#include "printer.h"

// The band's buffer, wide enough for Letter (8.5 inches at 240 dpi).
#define BAND_STRIDE ((85 * FX80_RASTER_DPI_X / 10 + 7) / 8)

#define JOB_DIGITS 8

// A job is printed by one printer or the other: they share their RAM.
static union {
  Fx80 fx80;
  PclPage laserjet;
} printer;
static bool laserjet;  // this job's printer is the LaserJet
static int32_t dpiX;   // this job's pages' resolution
static int32_t dpiY;
static Fx80Raster raster;
static uint8_t band[PAGES_BAND_ROWS * BAND_STRIDE];
static Pdf pdf;
static PagesCounters counters;

// Jobs are rendered in the order of their numbers: from nextJob up to the
// last one closed. A number without its job (taken by an older file, or a job
// that failed) is passed over.
static uint32_t nextJob;
static uint32_t lastClosed;

typedef enum {
  STATE_IDLE,
  STATE_RENDERING,  // the job's file and its PDF are open
  STATE_NAMING,     // the PDF is whole: named .pdf in the next pass
  STATE_DELETING,   // then the job deleted in the one after
} State;

static State state;
static bool atEnd;  // the job's last byte has been rendered (fx80_end)
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
static uint32_t pdfSize;

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
    pdf_pageBegin(&pdf, raster.width, raster.height, dpiX, dpiY);
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

// What a job has on the card: one bit a file.
#define FILE_JOB 1u       // nnnnnnnn.PRN
#define FILE_TEMP 2u      // nnnnnnnn.TMP
#define FILE_PDF 4u       // nnnnnnnn.pdf
#define FILE_PDF_TEMP 8u  // nnnnnnnn.PDT

static int setting_number(const char *key) {
  SettingsConfigEntry *entry = settings_find_entry(aconfig_getContext(), key);
  return entry != NULL ? atoi(entry->value) : 0;
}

// The PAPER setting: LETTER, or A4 for any other value.
static bool paper_letter(void) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_PAPER);
  return entry != NULL && strcasecmp(entry->value, "LETTER") == 0;
}

// The PRINTER setting: LASERJET, or the FX-80 for any other value.
static bool printer_laserjet(void) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_PRINTER);
  return entry != NULL && strcasecmp(entry->value, "LASERJET") == 0;
}

static bool read_job(void *ctx, uint8_t *data, uint32_t size, uint32_t *got) {
  UINT length = 0;
  FRESULT res = f_read(ctx, data, size, &length);
  *got = length;
  if (res != FR_OK) {
    DPRINTF("pages: job %08lu: read failed (%d)\n", (unsigned long)jobNumber,
            res);
  }
  return res == FR_OK;
}

static bool seek_job(void *ctx, uint32_t offset) {
  return f_lseek(ctx, offset) == FR_OK;
}

// The next job to render, in RAM: it reaches flash with NEXT_JOB, which the
// printer stores once the ST has been quiet, so a reboot looks again at the
// jobs from the last one stored.
static void advance(void) {
  nextJob++;
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_NEXT_PDF,
                       (int)nextJob);
}

// The files of one job, found in one pass over the folder.
static uint32_t job_files(uint32_t number) {
  static const struct {
    const char *extension;
    uint32_t file;
  } kinds[] = {{PRINTER_JOB_EXTENSION, FILE_JOB},
               {PRINTER_TEMP_EXTENSION, FILE_TEMP},
               {PAGES_PDF_EXTENSION, FILE_PDF},
               {PAGES_PDF_TEMP_EXTENSION, FILE_PDF_TEMP}};
  char pattern[JOB_DIGITS + 3];
  snprintf(pattern, sizeof(pattern), "%08lu.*", (unsigned long)number);
  uint32_t files = 0;
  DIR dir;
  FILINFO info;
  FRESULT res = f_findfirst(&dir, &info, printer_folder(), pattern);
  while (res == FR_OK && info.fname[0] != '\0') {
    const char *extension = strchr(info.fname, '.');
    for (size_t i = 0;
         extension != NULL && i < sizeof(kinds) / sizeof(kinds[0]); i++) {
      if (strcasecmp(extension, kinds[i].extension) == 0)
        files |= kinds[i].file;
    }
    res = f_findnext(&dir, &info);
  }
  f_closedir(&dir);
  return files;
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
static void delete_cut_pdf(uint32_t number) {
  char temp[PRINTER_PATH_SIZE];
  printer_jobPath(temp, sizeof(temp), number, PAGES_PDF_TEMP_EXTENSION);
  FRESULT res = f_unlink(temp);
  DPRINTF("pages: job %08lu's PDF cut short by a reboot, %s (%d)\n",
          (unsigned long)number, res == FR_OK ? "deleted" : "not deleted", res);
}

// A job a reboot left beside its whole PDF.
static bool delete_rendered_job(uint32_t number) {
  char job[PRINTER_PATH_SIZE];
  printer_jobPath(job, sizeof(job), number, PRINTER_JOB_EXTENSION);
  FRESULT res = f_unlink(job);
  DPRINTF("pages: job %08lu already in its PDF, %s (%d)\n",
          (unsigned long)number, res == FR_OK ? "deleted" : "not deleted", res);
  return res == FR_OK;
}

// The jobs from NEXT_PDF on, one number at a time, up to the last one on the
// card: the folder is never listed whole. Each job a reboot cut short is
// finished, and the jobs left are rendered from the main loop.
void pages_init(void) {
  int next = setting_number(ACONFIG_PARAM_NEXT_JOB);
  int from = setting_number(ACONFIG_PARAM_NEXT_PDF);
  if (next < 1) next = 1;
  if (from < 1 || from > next) from = next;
  uint32_t first = 0;  // the first job found
  uint32_t number;
  // Past NEXT_JOB, the jobs a power cut kept from storing it.
  for (number = (uint32_t)from; number <= PRINTER_JOB_LAST; number++) {
    uint32_t files = job_files(number);
    if (files == 0 && number >= (uint32_t)next) break;
    if (files & FILE_PDF_TEMP) delete_cut_pdf(number);
    if ((files & (FILE_TEMP | FILE_JOB)) == FILE_TEMP &&
        close_cut_job(number)) {
      files |= FILE_JOB;
    }
    if ((files & (FILE_JOB | FILE_PDF)) == (FILE_JOB | FILE_PDF) &&
        delete_rendered_job(number)) {
      files &= ~FILE_JOB;
    }
    if (files & FILE_JOB) {
      if (first == 0) first = number;
      lastClosed = number;
    }
  }
  // Rendering starts at the first job found, or past every number looked at.
  nextJob = first != 0 ? first : number;
  if (first == 0) lastClosed = nextJob - 1;
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_NEXT_PDF,
                       (int)nextJob);
  // The printer numbers its next job past every number looked at: a job
  // without a page leaves no file, and its number must not come round again.
  if (number > (uint32_t)next) {
    settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_NEXT_JOB,
                         (int)number);
  }
}

void pages_jobClosed(uint32_t number) {
  if (number > lastClosed) lastClosed = number;
}

static void start(void) {
  jobNumber = nextJob;
  char path[PRINTER_PATH_SIZE];
  printer_jobPath(path, sizeof(path), jobNumber, PRINTER_JOB_EXTENSION);
  FRESULT res = f_open(&file, path, FA_READ);
  if (res != FR_OK) {
    if (res != FR_NO_FILE) {
      DPRINTF("pages: cannot open %s (%d)\n", path, res);
      counters.failed++;
    }
    advance();
    return;
  }
  printer_jobPath(path, sizeof(path), jobNumber, PAGES_PDF_TEMP_EXTENSION);
  res = f_open(&pdfFile, path, FA_WRITE | FA_CREATE_ALWAYS);
  if (res != FR_OK) {
    DPRINTF("pages: cannot open %s (%d)\n", path, res);
    f_close(&file);
    counters.failed++;
    advance();
    return;
  }
  // The setting's printer, or the LaserJet for a job that starts in PCL.
  bufferLength = 0;
  laserjet = printer_laserjet() ||
             (f_read(&file, buffer, sizeof(buffer), &bufferLength) == FR_OK &&
              pcl_recognise(buffer, bufferLength));
  f_lseek(&file, 0);
  bool letter = paper_letter();
  pdf_begin(&pdf, write_card, &pdfFile);
  if (laserjet) {
    int32_t width = letter ? PCL_LETTER_WIDTH : PCL_A4_WIDTH;
    int32_t length = letter ? PCL_LETTER_LENGTH : PCL_A4_LENGTH;
    dpiX = dpiY = PCL_DPI;
    fx80_raster_init(&raster, band, width, length,
                     (int32_t)sizeof(band) / ((width + 7) / 8), on_row, NULL);
    pcl_page_init(&printer.laserjet, &raster, width, length,
                  letter ? PCL_LETTER_OFFSET : PCL_A4_OFFSET, read_job,
                  seek_job, &file, buffer, sizeof(buffer));
  } else {
    int32_t width = letter ? FX80_LETTER_WIDTH : FX80_A4_WIDTH;
    int32_t length = letter ? FX80_LETTER_LENGTH : FX80_A4_LENGTH;
    dpiX = FX80_RASTER_DPI_X;
    dpiY = FX80_RASTER_DPI_Y;
    fx80_raster_init(&raster, band, fx80_raster_width(width),
                     fx80_raster_height(length), PAGES_BAND_ROWS, on_row,
                     NULL);
    fx80_init(&printer.fx80, &sink, width, length);
  }
  state = STATE_RENDERING;
  atEnd = false;
  bufferLength = bufferPos = 0;
  pagesPending = 0;
  jobPages = 0;
  jobStart = get_absolute_time();
  jobLongestUs = 0;
}

// A job whose PDF failed stays on the card.
static void fail(FRESULT res) {
  DPRINTF("pages: job %08lu: its PDF not written (%d)\n",
          (unsigned long)jobNumber, res);
  counters.failed++;
  advance();
  state = STATE_IDLE;
}

// The job's last byte rendered: its PDF closed. A job that printed no page
// has no PDF.
static void close_pdf(void) {
  f_close(&file);
  bool written = pdf_end(&pdf);
  pdfSize = (uint32_t)f_size(&pdfFile);
  if (f_close(&pdfFile) != FR_OK) written = false;
  if (written && jobPages > 0) {
    state = STATE_NAMING;
    return;
  }
  char temp[PRINTER_PATH_SIZE];
  printer_jobPath(temp, sizeof(temp), jobNumber, PAGES_PDF_TEMP_EXTENSION);
  f_unlink(temp);
  pdfSize = 0;
  if (!written) {
    fail(FR_OK);
    return;
  }
  state = STATE_DELETING;
}

static void name_pdf(void) {
  char temp[PRINTER_PATH_SIZE];
  char path[PRINTER_PATH_SIZE];
  printer_jobPath(temp, sizeof(temp), jobNumber, PAGES_PDF_TEMP_EXTENSION);
  printer_jobPath(path, sizeof(path), jobNumber, PAGES_PDF_EXTENSION);
  FRESULT res = f_rename(temp, path);
  if (res != FR_OK) {
    f_unlink(temp);
    fail(res);
    return;
  }
  state = STATE_DELETING;
}

static void delete_job(void) {
  char path[PRINTER_PATH_SIZE];
  printer_jobPath(path, sizeof(path), jobNumber, PRINTER_JOB_EXTENSION);
  f_unlink(path);
  counters.jobs++;
  counters.lost += raster.lost;
  counters.lastJob = jobNumber;
  counters.lastPages = jobPages;
  DPRINTF(
      "pages: job %08lu rendered, %lu pages, PDF %lu bytes, %lld us, "
      "longest pass %lu us, %lu dots lost\n",
      (unsigned long)jobNumber, (unsigned long)jobPages, (unsigned long)pdfSize,
      absolute_time_diff_us(jobStart, get_absolute_time()),
      (unsigned long)jobLongestUs, (unsigned long)raster.lost);
  if (laserjet) {
    DPRINTF("pages: job %08lu: LaserJet, %lu passes over its pages%s\n",
            (unsigned long)jobNumber, (unsigned long)printer.laserjet.passes,
            printer.laserjet.failed ? ", the job could not be read again" : "");
  }
  advance();
  state = STATE_IDLE;
}

// A page's rows all handed on.
static void page_done(void) {
  jobPages++;
  counters.pages++;
  pdf_pageEnd(&pdf);
  page_log();
}

// The LaserJet: each page followed, then drawn (pcl/pclpage.h).
static void render_laserjet(absolute_time_t until) {
  do {
    PclPageStep step = pcl_page_step(&printer.laserjet);
    if (step == PCL_PAGE_DONE) {
      page_done();
    } else if (step == PCL_PAGE_END) {
      close_pdf();
      return;
    }
  } while (absolute_time_diff_us(get_absolute_time(), until) > 0);
}

static void render(void) {
  absolute_time_t until = make_timeout_time_us(PAGES_BUDGET_US);
  if (laserjet) {
    render_laserjet(until);
    return;
  }
  do {
    if (pagesPending) {
      if (!fx80_raster_finish(&raster, PAGES_FINISH_ROWS)) continue;
      pagesPending--;
      page_done();
      continue;
    }
    if (atEnd) {
      close_pdf();
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
        fx80_end(&printer.fx80);
        atEnd = true;
      }
      continue;
    }
    fx80_byte(&printer.fx80, buffer[bufferPos++]);
  } while (absolute_time_diff_us(get_absolute_time(), until) > 0);
}

#if defined(_DEBUG) && (_DEBUG != 0)
// Debug builds: where the longest pass was, for swd.py to read: the state
// before it and after it, and the LaserJet's phase before it (0xFF for none).
static volatile uint8_t longestPass[3] __attribute__((used));
#endif

// One step a pass: each of the card's slow steps (opening, naming, deleting
// a file) has a pass of its own.
void pages_loop(void) {
  if (state == STATE_IDLE && nextJob > lastClosed) return;
#if defined(_DEBUG) && (_DEBUG != 0)
  uint8_t before = (uint8_t)state;
  uint8_t phase = state == STATE_RENDERING && laserjet
                      ? printer.laserjet.phase
                      : 0xFF;
#endif
  absolute_time_t passStart = get_absolute_time();
  switch (state) {
    case STATE_IDLE:
      start();
      break;
    case STATE_RENDERING:
      render();
      break;
    case STATE_NAMING:
      name_pdf();
      break;
    case STATE_DELETING:
      delete_job();
      break;
  }
  uint32_t us = (uint32_t)absolute_time_diff_us(passStart, get_absolute_time());
  if (us > jobLongestUs) jobLongestUs = us;
  if (us > counters.longestUs) {
    counters.longestUs = us;
#if defined(_DEBUG) && (_DEBUG != 0)
    longestPass[0] = before;
    longestPass[1] = (uint8_t)state;
    longestPass[2] = phase;
#endif
  }
}

const PagesCounters *pages_getCounters(void) { return &counters; }

uint32_t pages_rendering(void) { return state == STATE_IDLE ? 0 : jobNumber; }

bool pages_busy(void) { return state != STATE_IDLE || nextJob <= lastClosed; }
