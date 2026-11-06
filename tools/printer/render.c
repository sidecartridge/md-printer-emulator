/*
 * render: a printer stream through the FX-80 renderer (rp/src/fx80), each
 * page written as a PNG, with the MD5 of its pixels, the firmware's own C
 * built on the computer.
 *
 * Usage:  render [--letter] [--laserjet|--fx80] [--band ROWS] [--pdf OUT.pdf]
 *         FILE OUTDIR
 *         OUTDIR/page-001.png, ...; prints "page N WxH MD5" per page.
 *         The printer as the firmware's PRINTER setting picks it: with
 *         neither option, Auto, the HP LaserJet II (rp/src/pcl, 300 dpi) for
 *         a stream that starts in PCL (pcl_recognise) and the FX-80 for the
 *         rest; --laserjet or --fx80, that printer whatever the stream.
 *         --band draws through a band of ROWS rows, as the firmware does;
 *         the pages must come out the same as with the whole page.
 *         --pdf also writes the pages as a PDF (rp/src/pdf), as the firmware
 *         does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "fx80.h"
#include "md5/md5.h"
#include "pcl/pcl.h"
#include "pcl/pclpage.h"
#include "pdf/pdf.h"
#include "raster.h"

typedef struct {
  Fx80Raster raster;
  uint8_t *page;  // the page's rows as they are handed on, for the PNG
  MD5Context md5;
  const char *dir;
  Pdf *pdf;  // NULL without --pdf
  int32_t dpiX, dpiY;
  int pages;
  int failed;
} Render;

static void put32(FILE *f, uint32_t v) {
  uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
  fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
  put32(f, len);
  fwrite(type, 1, 4, f);
  if (len) fwrite(data, 1, len, f);
  uint32_t crc = crc32(0, (const uint8_t *)type, 4);
  if (len) crc = crc32(crc, data, len);
  put32(f, crc);
}

// A 1-bit greyscale PNG: 0 is black, so the ink bits are inverted.
static int write_png(const char *path, const Fx80Raster *r, const uint8_t *page,
                     int32_t dpiX, int32_t dpiY) {
  size_t rawLen = (size_t)(r->stride + 1) * r->height;
  uint8_t *raw = malloc(rawLen);
  uLongf zLen = compressBound(rawLen);
  uint8_t *z = malloc(zLen);
  if (!raw || !z) return -1;
  for (int32_t j = 0; j < r->height; j++) {
    uint8_t *out = raw + (size_t)j * (r->stride + 1);
    out[0] = 0;
    for (int32_t i = 0; i < r->stride; i++) out[1 + i] = ~page[j * r->stride + i];
  }
  compress2(z, &zLen, raw, rawLen, 9);
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13] = {r->width >> 24, r->width >> 16, r->width >> 8, r->width,
                      r->height >> 24, r->height >> 16, r->height >> 8, r->height,
                      1, 0, 0, 0, 0};
  chunk(f, "IHDR", ihdr, 13);
  // pHYs: the pixels' size in pixels per metre.
  uint32_t ppmX = (uint32_t)(dpiX / 0.0254 + 0.5);
  uint32_t ppmY = (uint32_t)(dpiY / 0.0254 + 0.5);
  uint8_t phys[9] = {ppmX >> 24, ppmX >> 16, ppmX >> 8, ppmX,
                     ppmY >> 24, ppmY >> 16, ppmY >> 8, ppmY, 1};
  chunk(f, "pHYs", phys, 9);
  chunk(f, "IDAT", z, zLen);
  chunk(f, "IEND", NULL, 0);
  free(raw);
  free(z);
  return fclose(f);
}

static void on_dot(void *ctx, int32_t x, int32_t y) {
  fx80_raster_dot(&((Render *)ctx)->raster, x, y);
}

static bool write_file(void *ctx, const uint8_t *data, uint32_t size) {
  return fwrite(data, 1, size, ctx) == size;
}

static void on_row(void *ctx, int32_t row, const uint8_t *bits) {
  Render *re = ctx;
  if (row == 0) md5Init(&re->md5);
  if (re->pdf) {
    if (row == 0) {
      pdf_pageBegin(re->pdf, re->raster.width, re->raster.height, re->dpiX,
                    re->dpiY);
    }
    pdf_row(re->pdf, bits);
  }
  md5Update(&re->md5, (uint8_t *)bits, re->raster.stride);
  memcpy(re->page + (size_t)row * re->raster.stride, bits, re->raster.stride);
}

// The page's rows are all handed on.
static void page_done(Render *re) {
  Fx80Raster *r = &re->raster;
  md5Finalize(&re->md5);
  if (re->pdf) pdf_pageEnd(re->pdf);
  re->pages++;
  char path[1024];
  snprintf(path, sizeof(path), "%s/page-%03d.png", re->dir, re->pages);
  if (write_png(path, r, re->page, re->dpiX, re->dpiY) != 0) {
    fprintf(stderr, "render: cannot write %s\n", path);
    re->failed = 1;
  }
  printf("page %d %dx%d ", re->pages, r->width, r->height);
  for (int i = 0; i < 16; i++) printf("%02x", re->md5.digest[i]);
  printf("\n");
}

static void on_page(void *ctx) {
  Render *re = ctx;
  fx80_raster_finish(&re->raster, re->raster.height);
  page_done(re);
}

static bool read_file(void *ctx, uint8_t *data, uint32_t size, uint32_t *got) {
  *got = (uint32_t)fread(data, 1, size, ctx);
  return !ferror((FILE *)ctx);
}

static bool seek_file(void *ctx, uint32_t offset) {
  return fseek(ctx, (long)offset, SEEK_SET) == 0;
}

int main(int argc, char **argv) {
  int letter = 0;
  int laserjet = 0;
  int fx80 = 0;
  int32_t band = 0;
  const char *pdfPath = NULL;
  int a = 1;
  for (; a < argc && argv[a][0] == '-' && argv[a][1] == '-'; a++) {
    if (strcmp(argv[a], "--letter") == 0) {
      letter = 1;
    } else if (strcmp(argv[a], "--laserjet") == 0) {
      laserjet = 1;
    } else if (strcmp(argv[a], "--fx80") == 0) {
      fx80 = 1;
    } else if (strcmp(argv[a], "--band") == 0 && a + 1 < argc) {
      band = atoi(argv[++a]);
    } else if (strcmp(argv[a], "--pdf") == 0 && a + 1 < argc) {
      pdfPath = argv[++a];
    } else {
      break;
    }
  }
  if (argc - a != 2) {
    fprintf(stderr,
            "usage: render [--letter] [--laserjet|--fx80] [--band ROWS] "
            "[--pdf OUT.pdf] FILE OUTDIR\n");
    return 2;
  }
  const char *file = argv[a];
  int32_t width = letter ? FX80_LETTER_WIDTH : FX80_A4_WIDTH;
  int32_t length = letter ? FX80_LETTER_LENGTH : FX80_A4_LENGTH;
  static Render re;
  static Fx80 printer;
  re.dir = argv[a + 1];
  FILE *in = fopen(file, "rb");
  if (!in) {
    fprintf(stderr, "render: cannot read %s\n", file);
    return 1;
  }
  uint8_t start[512];  // as much as the firmware looks at
  size_t got = fread(start, 1, sizeof(start), in);
  if (!fx80 && pcl_recognise(start, (uint32_t)got)) laserjet = 1;
  rewind(in);
  int32_t w = fx80_raster_width(width);
  int32_t h = fx80_raster_height(length);
  re.dpiX = FX80_RASTER_DPI_X;
  re.dpiY = FX80_RASTER_DPI_Y;
  if (laserjet) {
    w = letter ? PCL_LETTER_WIDTH : PCL_A4_WIDTH;
    h = letter ? PCL_LETTER_LENGTH : PCL_A4_LENGTH;
    re.dpiX = re.dpiY = PCL_DPI;
  }
  int32_t rows = (band > 0 && band < h) ? band : h;
  int32_t stride = (w + 7) / 8;
  uint8_t *bits = malloc((size_t)stride * rows);
  re.page = malloc((size_t)stride * h);
  if (!bits || !re.page) {
    fprintf(stderr, "render: cannot read %s\n", file);
    return 1;
  }
  static Pdf pdf;
  FILE *pdfFile = NULL;
  if (pdfPath) {
    pdfFile = fopen(pdfPath, "wb");
    if (!pdfFile) {
      fprintf(stderr, "render: cannot write %s\n", pdfPath);
      return 1;
    }
    pdf_begin(&pdf, write_file, pdfFile);
    re.pdf = &pdf;
  }
  fx80_raster_init(&re.raster, bits, w, h, rows, on_row, &re);
  int c;
  if (laserjet) {
    static PclPage page;
    static uint8_t buffer[512];
    pcl_page_init(&page, &re.raster, w, h,
                  letter ? PCL_LETTER_OFFSET : PCL_A4_OFFSET, read_file,
                  seek_file, in, buffer, sizeof(buffer));
    PclPageStep step;
    while ((step = pcl_page_step(&page)) != PCL_PAGE_END) {
      if (step == PCL_PAGE_DONE) page_done(&re);
    }
    if (page.passes > 2 * (uint32_t)re.pages + 1) {
      fprintf(stderr, "render: %u passes over the pages' bytes\n", page.passes);
    }
  } else {
    Fx80Sink sink = {on_dot, on_page, &re};
    fx80_init(&printer, &sink, width, length);
    while ((c = fgetc(in)) != EOF) fx80_byte(&printer, (uint8_t)c);
    fx80_end(&printer);
  }
  fclose(in);
  if (pdfFile) {
    bool written = pdf_end(&pdf);
    if (fclose(pdfFile) != 0) written = false;
    if (!written) {
      fprintf(stderr, "render: cannot write %s\n", pdfPath);
      re.failed = 1;
    }
  }
  if (re.raster.lost) {
    printf("lost %lu dots above the band\n", (unsigned long)re.raster.lost);
  }
  return re.failed;
}
