/**
 * File: pdf.c
 * Description: A PDF of 1-bit page images, written as the rows arrive
 *              (pdf.h).
 *
 * Object 1 is the catalogue, written first; object 2 the page list, written
 * last, when the pages are known. Each page then has four: the page, its
 * contents (the image drawn over the whole page), the image, and the image's
 * length, which is known only once its last row is compressed (deflate.h).
 */
#include "pdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PDF_PAGE_OBJECTS 4
#define PDF_FIRST_PAGE_OBJECT 3
#define PDF_PAGES_STEP 64  // pages objects grows by
#define PDF_TEXT_SIZE 160  // the longest piece of text formatted at once

static const char header[] = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
static const char catalog[] =
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

static void flush(Pdf *pdf) {
  if (pdf->outLength == 0) return;
  if (!pdf->failed && !pdf->write(pdf->ctx, pdf->out, pdf->outLength)) {
    pdf->failed = true;
  }
  pdf->outLength = 0;
}

static void put_byte(Pdf *pdf, uint8_t byte) {
  pdf->out[pdf->outLength++] = byte;
  pdf->offset++;
  if (pdf->outLength == PDF_OUT_SIZE) flush(pdf);
}

static void put_bytes(Pdf *pdf, const void *data, uint32_t size) {
  const uint8_t *bytes = data;
  for (uint32_t i = 0; i < size; i++) put_byte(pdf, bytes[i]);
}

static void put_text(Pdf *pdf, const char *text) {
  put_bytes(pdf, text, strlen(text));
}

static void put_textf(Pdf *pdf, const char *format, long arg1, long arg2,
                      long arg3) {
  char text[PDF_TEXT_SIZE];
  snprintf(text, sizeof(text), format, arg1, arg2, arg3);
  put_text(pdf, text);
}

static uint32_t page_object(uint32_t page, int which) {
  return PDF_FIRST_PAGE_OBJECT + page * PDF_PAGE_OBJECTS + which;
}

// Records where the page's object `which` starts.
static void object_start(Pdf *pdf, int which) {
  pdf->objects[(pdf->pages - 1) * PDF_PAGE_OBJECTS + which] = pdf->offset;
}

static void put_deflated(void *ctx, uint8_t byte) { put_byte(ctx, byte); }

void pdf_begin(Pdf *pdf, PdfWrite write, void *ctx) {
  memset(pdf, 0, sizeof(*pdf));
  pdf->write = write;
  pdf->ctx = ctx;
  put_text(pdf, header);
  put_text(pdf, catalog);
}

void pdf_pageBegin(Pdf *pdf, int32_t width, int32_t height, int32_t dpiX,
                   int32_t dpiY) {
  if (pdf->pages == pdf->pagesSize) {
    uint32_t size = pdf->pagesSize + PDF_PAGES_STEP;
    uint32_t *objects =
        realloc(pdf->objects, size * PDF_PAGE_OBJECTS * sizeof(uint32_t));
    if (objects == NULL) {
      pdf->failed = true;
      return;
    }
    pdf->objects = objects;
    pdf->pagesSize = size;
  }
  pdf->pages++;
  pdf->stride = (width + 7) / 8;
  uint32_t page = pdf->pages - 1;
  // The page's size in hundredths of a point.
  long w = (long)width * 7200 / dpiX;
  long h = (long)height * 7200 / dpiY;

  object_start(pdf, 0);
  put_textf(pdf, "%ld 0 obj\n<< /Type /Page /Parent 2 0 R ",
            page_object(page, 0), 0, 0);
  put_textf(pdf, "/MediaBox [0 0 %ld.%02ld ", w / 100, w % 100, 0);
  put_textf(pdf, "%ld.%02ld] /Contents %ld 0 R ", h / 100, h % 100,
            page_object(page, 1));
  put_textf(pdf, "/Resources << /XObject << /Im %ld 0 R >> >> >>\nendobj\n",
            page_object(page, 2), 0, 0);

  char contents[64];
  snprintf(contents, sizeof(contents), "q %ld.%02ld 0 0 %ld.%02ld 0 0 cm",
           w / 100, w % 100, h / 100, h % 100);
  strcat(contents, " /Im Do Q");
  object_start(pdf, 1);
  put_textf(pdf, "%ld 0 obj\n<< /Length %ld >>\nstream\n", page_object(page, 1),
            (long)strlen(contents), 0);
  put_text(pdf, contents);
  put_text(pdf, "\nendstream\nendobj\n");

  object_start(pdf, 2);
  put_textf(pdf,
            "%ld 0 obj\n<< /Type /XObject /Subtype /Image /Width %ld "
            "/Height %ld ",
            page_object(page, 2), width, height);
  // Ink is 1: decoded the other way round from DeviceGray.
  put_text(pdf,
           "/ColorSpace /DeviceGray /BitsPerComponent 1 /Decode [1 0] "
           "/Filter /FlateDecode ");
  put_textf(pdf, "/Length %ld 0 R >>\nstream\n", page_object(page, 3), 0, 0);
  pdf->streamStart = pdf->offset;
  deflate_begin(&pdf->deflate, put_deflated, pdf);
}

void pdf_row(Pdf *pdf, const uint8_t *bits) {
  if (pdf->failed) return;
  deflate_write(&pdf->deflate, bits, pdf->stride);
}

void pdf_pageEnd(Pdf *pdf) {
  if (pdf->failed) return;
  deflate_end(&pdf->deflate);
  uint32_t length = pdf->offset - pdf->streamStart;
  put_text(pdf, "\nendstream\nendobj\n");
  object_start(pdf, 3);
  put_textf(pdf, "%ld 0 obj\n%ld\nendobj\n", page_object(pdf->pages - 1, 3),
            (long)length, 0);
}

bool pdf_end(Pdf *pdf) {
  if (!pdf->failed) {
    uint32_t pagesOffset = pdf->offset;
    put_textf(pdf, "2 0 obj\n<< /Type /Pages /Count %ld /Kids [", pdf->pages, 0,
              0);
    for (uint32_t page = 0; page < pdf->pages; page++) {
      put_textf(pdf, " %ld 0 R", page_object(page, 0), 0, 0);
    }
    put_text(pdf, " ] >>\nendobj\n");

    uint32_t xref = pdf->offset;
    uint32_t objects = PDF_FIRST_PAGE_OBJECT + pdf->pages * PDF_PAGE_OBJECTS;
    put_textf(pdf, "xref\n0 %ld\n0000000000 65535 f \n", objects, 0, 0);
    put_textf(pdf, "%010ld 00000 n \n", (long)strlen(header), 0, 0);
    put_textf(pdf, "%010ld 00000 n \n", pagesOffset, 0, 0);
    for (uint32_t i = 0; i < pdf->pages * PDF_PAGE_OBJECTS; i++) {
      put_textf(pdf, "%010ld 00000 n \n", pdf->objects[i], 0, 0);
    }
    put_textf(pdf, "trailer\n<< /Size %ld /Root 1 0 R >>\nstartxref\n%ld\n",
              objects, xref, 0);
    put_text(pdf, "%%EOF\n");
    flush(pdf);
  }
  free(pdf->objects);
  pdf->objects = NULL;
  return !pdf->failed;
}
