/*
 * render: a printer stream through the FX-80 renderer (rp/src/fx80), each
 * page written as a PNG, with the MD5 of its pixels, the firmware's own C
 * built on the computer.
 *
 * Usage:  render [--letter] [--band ROWS] [--pdf OUT.pdf] FILE OUTDIR
 *         OUTDIR/page-001.png, ...; prints "page N WxH MD5" per page.
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
#include "pdf/pdf.h"
#include "raster.h"

typedef struct {
  Fx80Raster raster;
  uint8_t *page;  // the page's rows as they are handed on, for the PNG
  MD5Context md5;
  const char *dir;
  Pdf *pdf;  // NULL without --pdf
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
static int write_png(const char *path, const Fx80Raster *r,
                     const uint8_t *page) {
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
  // pHYs: the pixels' size, 240 x 216 dpi in pixels per metre.
  uint32_t ppmX = (uint32_t)(FX80_RASTER_DPI_X / 0.0254 + 0.5);
  uint32_t ppmY = (uint32_t)(FX80_RASTER_DPI_Y / 0.0254 + 0.5);
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
      pdf_pageBegin(re->pdf, re->raster.width, re->raster.height,
                    FX80_RASTER_DPI_X, FX80_RASTER_DPI_Y);
    }
    pdf_row(re->pdf, bits);
  }
  md5Update(&re->md5, (uint8_t *)bits, re->raster.stride);
  memcpy(re->page + (size_t)row * re->raster.stride, bits, re->raster.stride);
}

static void on_page(void *ctx) {
  Render *re = ctx;
  Fx80Raster *r = &re->raster;
  fx80_raster_finish(r, r->height);
  md5Finalize(&re->md5);
  if (re->pdf) pdf_pageEnd(re->pdf);
  re->pages++;
  char path[1024];
  snprintf(path, sizeof(path), "%s/page-%03d.png", re->dir, re->pages);
  if (write_png(path, r, re->page) != 0) {
    fprintf(stderr, "render: cannot write %s\n", path);
    re->failed = 1;
  }
  printf("page %d %dx%d ", re->pages, r->width, r->height);
  for (int i = 0; i < 16; i++) printf("%02x", re->md5.digest[i]);
  printf("\n");
}

int main(int argc, char **argv) {
  int letter = 0;
  int32_t band = 0;
  const char *pdfPath = NULL;
  int a = 1;
  for (; a < argc && argv[a][0] == '-' && argv[a][1] == '-'; a++) {
    if (strcmp(argv[a], "--letter") == 0) {
      letter = 1;
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
            "usage: render [--letter] [--band ROWS] "
            "[--pdf OUT.pdf] FILE OUTDIR\n");
    return 2;
  }
  const char *file = argv[a];
  // A4, 210 x 297 mm; Letter, 8.5 x 11 inches.
  int32_t width = letter ? 85 * FX80_X_PER_INCH / 10 : 210 * FX80_X_PER_INCH * 10 / 254;
  int32_t length = letter ? 11 * FX80_Y_PER_INCH : 297 * FX80_Y_PER_INCH * 10 / 254;
  static Render re;
  static Fx80 printer;
  re.dir = argv[a + 1];
  int32_t w = fx80_raster_width(width);
  int32_t h = fx80_raster_height(length);
  int32_t rows = (band > 0 && band < h) ? band : h;
  int32_t stride = (w + 7) / 8;
  uint8_t *bits = malloc((size_t)stride * rows);
  re.page = malloc((size_t)stride * h);
  FILE *in = fopen(file, "rb");
  if (!in || !bits || !re.page) {
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
  Fx80Sink sink = {on_dot, on_page, &re};
  fx80_init(&printer, &sink, width, length);
  int c;
  while ((c = fgetc(in)) != EOF) fx80_byte(&printer, (uint8_t)c);
  fclose(in);
  fx80_end(&printer);
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
