/**
 * File: pdf.h
 * Description: A PDF of 1-bit page images, written as the rows arrive: each
 *              page is one image, its rows compressed as they come (Flate,
 *              deflate.h). The cross-reference table and the page list are
 *              written at the end.
 *
 * Bits as in fx80/raster.h: the left pixel in bit 7, 1 is ink. A page's size
 * in points comes from its pixels and their resolution, so a reader renders
 * it at that resolution pixel for pixel.
 */
#ifndef PDF_H
#define PDF_H

#include <stdbool.h>
#include <stdint.h>

#include "deflate.h"

#define PDF_OUT_SIZE 512  // bytes gathered before each write

// Writes size bytes; false if they could not be written.
typedef bool (*PdfWrite)(void *ctx, const uint8_t *data, uint32_t size);

typedef struct {
  PdfWrite write;
  void *ctx;
  bool failed;         // a write failed or RAM ran out: nothing more is written
  uint32_t offset;     // bytes written
  uint32_t *objects;   // each page's objects' offsets (PDF_PAGE_OBJECTS)
  uint32_t pages;      // pages begun
  uint32_t pagesSize;  // pages objects has room for
  int32_t stride;      // bytes a row of the page
  uint32_t streamStart;  // the offset of the image's first byte
  Deflate deflate;
  uint8_t out[PDF_OUT_SIZE];
  uint32_t outLength;
} Pdf;

/**
 * @brief Starts a PDF: write is called with its bytes, in order.
 */
void pdf_begin(Pdf *pdf, PdfWrite write, void *ctx);

/**
 * @brief Starts a page of width x height pixels at dpiX x dpiY.
 */
void pdf_pageBegin(Pdf *pdf, int32_t width, int32_t height, int32_t dpiX,
                   int32_t dpiY);

/**
 * @brief The page's next row, (width + 7) / 8 bytes.
 */
void pdf_row(Pdf *pdf, const uint8_t *bits);

/**
 * @brief Ends the page, after its last row.
 */
void pdf_pageEnd(Pdf *pdf);

/**
 * @brief Ends the PDF and frees what it took.
 * @return false if any write failed or RAM ran out: the file is not a PDF.
 */
bool pdf_end(Pdf *pdf);

#endif  // PDF_H
