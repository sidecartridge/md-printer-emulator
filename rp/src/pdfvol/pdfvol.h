/**
 * File: pdfvol.h
 * Description: A read-only FAT32 volume holding the folder's PDFs, made up
 *              sector by sector as it is read: the boot sector, the FATs and
 *              the root folder are computed, and a PDF's sectors are read
 *              from the file it stands for.
 *
 * The volume is always PDFVOL_SECTORS sectors of 512 bytes, about 2 GB, in
 * clusters of 32 KB. The root folder takes the first clusters: the volume's
 * label, then each PDF in the order of its number. Each PDF's clusters
 * follow, one run after the other. Its name is its number and `.pdf`, a short
 * name whose extension is marked lowercase, so no long names are needed.
 */
#ifndef PDFVOL_H
#define PDFVOL_H

#include <stdbool.h>
#include <stdint.h>

#define PDFVOL_SECTOR_SIZE 512
#define PDFVOL_CLUSTER_SECTORS 64  // 32 KB
#define PDFVOL_CLUSTERS 65600      // FAT32 needs 65,525 or more
#define PDFVOL_RESERVED 32         // sectors before the first FAT
#define PDFVOL_FATS 2
#define PDFVOL_FAT_SECTORS \
  (((PDFVOL_CLUSTERS + 2) * 4 + PDFVOL_SECTOR_SIZE - 1) / PDFVOL_SECTOR_SIZE)
#define PDFVOL_DATA_START (PDFVOL_RESERVED + PDFVOL_FATS * PDFVOL_FAT_SECTORS)
#define PDFVOL_SECTORS \
  (PDFVOL_DATA_START + PDFVOL_CLUSTERS * PDFVOL_CLUSTER_SECTORS)
#define PDFVOL_LABEL "PDFPRINTER "  // 11 characters

typedef struct {
  uint32_t number;  // the PDF is nnnnnnnn.pdf
  uint32_t size;    // bytes
  uint16_t date;    // as FAT stores them, from the file on the card
  uint16_t time;
  uint32_t first;  // its first cluster, 0 for an empty file (pdfvol_build)
} PdfvolFile;

// Reads size bytes of PDF number from offset into data; false if it could
// not. offset + size never passes the PDF's size.
typedef bool (*PdfvolRead)(void *ctx, uint32_t number, uint32_t offset,
                           uint8_t *data, uint32_t size);

typedef struct {
  PdfvolFile *files;  // the ones on the volume, in the order of their numbers
  uint32_t count;
  uint32_t rootClusters;
  uint32_t usedClusters;  // the root folder's and the PDFs'
  uint32_t serial;        // the volume's serial number
  PdfvolRead read;
  void *ctx;
} Pdfvol;

/**
 * @brief Lays out the volume for files (sorted here by number; their first
 * clusters are set). Files that do not fit are left out, the lowest numbers
 * first. The volume reads files through read. A computer that sees another
 * serial number takes the volume for another one and reads it afresh.
 * @return the files on the volume.
 */
uint32_t pdfvol_build(Pdfvol *vol, PdfvolFile *files, uint32_t count,
                      uint32_t serial, PdfvolRead read, void *ctx);

/**
 * @brief count sectors from lba into data. False if a PDF could not be read
 * or the sectors are past the volume's end.
 */
bool pdfvol_readSectors(const Pdfvol *vol, uint32_t lba, uint8_t *data,
                        uint32_t count);

#endif  // PDFVOL_H
