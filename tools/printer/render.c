/*
 * render: a printer stream through the FX-80 renderer (rp/src/fx80), each
 * page written as a PNG, with the MD5 of its pixels, the firmware's own C
 * built on the computer.
 *
 * Usage:  render [--letter] FILE OUTDIR
 *         OUTDIR/page-001.png, ...; prints "page N WxH MD5" per page.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "fx80.h"
#include "md5/md5.h"
#include "raster.h"

typedef struct {
  Fx80Raster raster;
  const char *dir;
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
static int write_png(const char *path, const Fx80Raster *r) {
  size_t rawLen = (size_t)(r->stride + 1) * r->height;
  uint8_t *raw = malloc(rawLen);
  uLongf zLen = compressBound(rawLen);
  uint8_t *z = malloc(zLen);
  if (!raw || !z) return -1;
  for (int32_t j = 0; j < r->height; j++) {
    uint8_t *out = raw + (size_t)j * (r->stride + 1);
    out[0] = 0;
    for (int32_t i = 0; i < r->stride; i++) out[1 + i] = ~r->bits[j * r->stride + i];
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

static void on_page(void *ctx) {
  Render *re = ctx;
  Fx80Raster *r = &re->raster;
  re->pages++;
  MD5Context md5;
  md5Init(&md5);
  md5Update(&md5, r->bits, (size_t)r->stride * r->height);
  md5Finalize(&md5);
  char path[1024];
  snprintf(path, sizeof(path), "%s/page-%03d.png", re->dir, re->pages);
  if (write_png(path, r) != 0) {
    fprintf(stderr, "render: cannot write %s\n", path);
    re->failed = 1;
  }
  printf("page %d %dx%d ", re->pages, r->width, r->height);
  for (int i = 0; i < 16; i++) printf("%02x", md5.digest[i]);
  printf("\n");
  memset(r->bits, 0, (size_t)r->stride * r->height);
}

int main(int argc, char **argv) {
  int letter = argc > 1 && strcmp(argv[1], "--letter") == 0;
  if (argc != 3 + letter) {
    fprintf(stderr, "usage: render [--letter] FILE OUTDIR\n");
    return 2;
  }
  const char *file = argv[1 + letter];
  // A4, 210 x 297 mm; Letter, 8.5 x 11 inches.
  int32_t width = letter ? 85 * FX80_X_PER_INCH / 10 : 210 * FX80_X_PER_INCH * 10 / 254;
  int32_t length = letter ? 11 * FX80_Y_PER_INCH : 297 * FX80_Y_PER_INCH * 10 / 254;
  static Render re;
  static Fx80 printer;
  re.dir = argv[2 + letter];
  re.raster.width = fx80_raster_width(width);
  re.raster.height = fx80_raster_height(length);
  re.raster.stride = (re.raster.width + 7) / 8;
  re.raster.rows = re.raster.height;
  re.raster.bits = calloc((size_t)re.raster.stride, re.raster.height);
  FILE *in = fopen(file, "rb");
  if (!in || !re.raster.bits) {
    fprintf(stderr, "render: cannot read %s\n", file);
    return 1;
  }
  Fx80Sink sink = {on_dot, on_page, &re};
  fx80_init(&printer, &sink, length);
  int c;
  while ((c = fgetc(in)) != EOF) fx80_byte(&printer, (uint8_t)c);
  fclose(in);
  fx80_end(&printer);
  return re.failed;
}
