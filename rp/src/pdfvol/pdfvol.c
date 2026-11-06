/**
 * File: pdfvol.c
 * Description: A read-only FAT32 volume holding the folder's PDFs, made up
 *              as it is read (pdfvol.h).
 *
 * Reserved sectors: the boot sector at 0 and its copy at 6, the FS
 * information sector at 1 and its copy at 7. Both FATs say the same. The
 * root folder starts at cluster 2.
 */
#include "pdfvol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLUSTER_SIZE (PDFVOL_CLUSTER_SECTORS * PDFVOL_SECTOR_SIZE)
#define ENTRY_SIZE 32                         // a folder entry
#define FAT_ENTRIES (PDFVOL_SECTOR_SIZE / 4)  // a FAT sector's entries
#define BACKUP_BOOT 6
#define FSINFO 1
#define END_OF_CHAIN 0x0FFFFFFFu
#define MEDIA 0xF8
#define ATTR_ARCHIVE 0x20
#define ATTR_VOLUME_ID 0x08
#define LOWERCASE_EXTENSION 0x10

static void put16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}

static uint32_t clusters_of(uint32_t size) {
  return (size + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
}

static int by_number(const void *a, const void *b) {
  uint32_t x = ((const PdfvolFile *)a)->number;
  uint32_t y = ((const PdfvolFile *)b)->number;
  return (x > y) - (x < y);
}

uint32_t pdfvol_build(Pdfvol *vol, PdfvolFile *files, uint32_t count,
                      uint32_t serial, PdfvolRead read, void *ctx) {
  qsort(files, count, sizeof(*files), by_number);
  uint32_t dropped = 0;
  for (;;) {
    uint32_t entries = 1 + (count - dropped);  // the label, then the PDFs
    uint32_t used = (entries * ENTRY_SIZE + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
    uint32_t root = used;
    for (uint32_t i = dropped; i < count; i++)
      used += clusters_of(files[i].size);
    if (used <= PDFVOL_CLUSTERS) {
      vol->rootClusters = root;
      vol->usedClusters = used;
      break;
    }
    dropped++;
  }
  vol->files = files + dropped;
  vol->count = count - dropped;
  vol->serial = serial;
  vol->read = read;
  vol->ctx = ctx;
  uint32_t next = 2 + vol->rootClusters;
  for (uint32_t i = 0; i < vol->count; i++) {
    uint32_t n = clusters_of(vol->files[i].size);
    vol->files[i].first = n ? next : 0;
    next += n;
  }
  return vol->count;
}

// The PDF whose clusters hold cluster, or NULL.
static const PdfvolFile *file_at(const Pdfvol *vol, uint32_t cluster) {
  uint32_t lo = 0;
  uint32_t hi = vol->count;
  while (lo < hi) {  // the first file that starts past cluster
    uint32_t mid = (lo + hi) / 2;
    if (vol->files[mid].first != 0 && vol->files[mid].first > cluster) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  while (lo > 0) {  // back to the last one that has clusters
    const PdfvolFile *f = &vol->files[--lo];
    if (f->first == 0) continue;
    return cluster < f->first + clusters_of(f->size) ? f : NULL;
  }
  return NULL;
}

static uint32_t fat_entry(const Pdfvol *vol, uint32_t cluster) {
  if (cluster == 0) return 0x0FFFFF00u | MEDIA;
  if (cluster == 1) return END_OF_CHAIN;
  uint32_t last;
  if (cluster < 2 + vol->rootClusters) {
    last = 2 + vol->rootClusters - 1;
  } else {
    const PdfvolFile *f = file_at(vol, cluster);
    if (f == NULL) return 0;  // free
    last = f->first + clusters_of(f->size) - 1;
  }
  return cluster == last ? END_OF_CHAIN : cluster + 1;
}

static void boot_sector(const Pdfvol *vol, uint8_t *s) {
  static const uint8_t jump[3] = {0xEB, 0x58, 0x90};
  memcpy(s, jump, sizeof(jump));
  memcpy(s + 3, "MSWIN4.1", 8);
  put16(s + 11, PDFVOL_SECTOR_SIZE);
  s[13] = PDFVOL_CLUSTER_SECTORS;
  put16(s + 14, PDFVOL_RESERVED);
  s[16] = PDFVOL_FATS;
  s[21] = MEDIA;
  put16(s + 24, 63);   // sectors a track
  put16(s + 26, 255);  // heads
  put32(s + 32, PDFVOL_SECTORS);
  put32(s + 36, PDFVOL_FAT_SECTORS);
  put32(s + 44, 2);  // the root folder's first cluster
  put16(s + 48, FSINFO);
  put16(s + 50, BACKUP_BOOT);
  s[64] = 0x80;  // drive number
  s[66] = 0x29;  // the serial, label and type follow
  put32(s + 67, vol->serial);
  memcpy(s + 71, PDFVOL_LABEL, 11);
  memcpy(s + 82, "FAT32   ", 8);
  s[510] = 0x55;
  s[511] = 0xAA;
}

static void fsinfo_sector(const Pdfvol *vol, uint8_t *s) {
  put32(s, 0x41615252u);
  put32(s + 484, 0x61417272u);
  put32(s + 488, PDFVOL_CLUSTERS - vol->usedClusters);  // free clusters
  put32(s + 492, 2 + vol->usedClusters);                // the next free one
  put32(s + 508, 0xAA550000u);
}

static void folder_entry(const PdfvolFile *f, uint8_t *e) {
  char name[12];
  snprintf(name, sizeof(name), "%08luPDF", (unsigned long)f->number);
  memcpy(e, name, 11);
  e[11] = ATTR_ARCHIVE;
  e[12] = LOWERCASE_EXTENSION;
  put16(e + 14, f->time);  // created
  put16(e + 16, f->date);
  put16(e + 18, f->date);  // last read
  put16(e + 20, f->first >> 16);
  put16(e + 22, f->time);  // written
  put16(e + 24, f->date);
  put16(e + 26, f->first & 0xFFFF);
  put32(e + 28, f->size);
}

// The root folder's sector index (from its first): the label, then the PDFs,
// then entries of zeros, which end the folder.
static void folder_sector(const Pdfvol *vol, uint32_t index, uint8_t *s) {
  uint32_t perSector = PDFVOL_SECTOR_SIZE / ENTRY_SIZE;
  for (uint32_t k = 0; k < perSector; k++) {
    uint32_t entry = index * perSector + k;
    uint8_t *e = s + k * ENTRY_SIZE;
    if (entry == 0) {
      memcpy(e, PDFVOL_LABEL, 11);
      e[11] = ATTR_VOLUME_ID;
    } else if (entry - 1 < vol->count) {
      folder_entry(&vol->files[entry - 1], e);
    }
  }
}

bool pdfvol_readSectors(const Pdfvol *vol, uint32_t lba, uint8_t *data,
                        uint32_t count) {
  if (lba > PDFVOL_SECTORS || count > PDFVOL_SECTORS - lba) return false;
  while (count > 0) {
    uint32_t done = 1;  // sectors this pass fills
    memset(data, 0, PDFVOL_SECTOR_SIZE);
    if (lba < PDFVOL_RESERVED) {
      if (lba == 0 || lba == BACKUP_BOOT) boot_sector(vol, data);
      if (lba == FSINFO || lba == BACKUP_BOOT + FSINFO) {
        fsinfo_sector(vol, data);
      }
    } else if (lba < PDFVOL_DATA_START) {
      uint32_t first =
          ((lba - PDFVOL_RESERVED) % PDFVOL_FAT_SECTORS) * FAT_ENTRIES;
      for (uint32_t k = 0; k < FAT_ENTRIES; k++) {
        if (first + k < PDFVOL_CLUSTERS + 2) {
          put32(data + 4 * k, fat_entry(vol, first + k));
        }
      }
    } else {
      uint32_t sector = lba - PDFVOL_DATA_START;
      uint32_t cluster = 2 + sector / PDFVOL_CLUSTER_SECTORS;
      const PdfvolFile *f;
      if (cluster < 2 + vol->rootClusters) {
        folder_sector(vol, sector, data);
      } else if ((f = file_at(vol, cluster)) != NULL) {
        // The run of the PDF's sectors from here, read at once.
        uint32_t start = (f->first - 2) * PDFVOL_CLUSTER_SECTORS;
        uint32_t end = start + clusters_of(f->size) * PDFVOL_CLUSTER_SECTORS;
        done = end - sector < count ? end - sector : count;
        uint32_t offset = (sector - start) * PDFVOL_SECTOR_SIZE;
        uint32_t size = done * PDFVOL_SECTOR_SIZE;
        uint32_t held = offset < f->size ? f->size - offset : 0;
        if (held > size) held = size;
        if (held > 0 && !vol->read(vol->ctx, f->number, offset, data, held)) {
          return false;
        }
        memset(data + held, 0, size - held);
      }
    }
    lba += done;
    data += done * PDFVOL_SECTOR_SIZE;
    count -= done;
  }
  return true;
}
