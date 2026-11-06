/*
 * volume: the RP's PDF volume (rp/src/pdfvol) built on this computer from a
 * folder, as the firmware builds it from the card's, and written as a disk
 * image.
 *
 * Usage:  volume FOLDER IMAGE
 *         Every nnnnnnnn.pdf in FOLDER (any case) goes on the volume, its
 *         date the file's. IMAGE is the whole volume, sparse: only the
 *         sectors before the end of the last PDF are written. Prints how many
 *         PDFs are on it.
 */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pdfvol.h"

#define RUN_SECTORS 64             // sectors read at a time
#define VOLUME_SERIAL 0x4D445052u  // "MDPR"

static const char *folder;

// The number of an nnnnnnnn.pdf name, or 0.
static uint32_t pdf_number(const char *name) {
  if (strlen(name) != 12 || strcasecmp(name + 8, ".pdf") != 0) return 0;
  uint32_t n = 0;
  for (int i = 0; i < 8; i++) {
    if (!isdigit((unsigned char)name[i])) return 0;
    n = n * 10 + (uint32_t)(name[i] - '0');
  }
  return n;
}

static char *path_of(uint32_t number) {
  static char path[4096];
  DIR *d = opendir(folder);
  struct dirent *e;
  path[0] = '\0';
  while (d && (e = readdir(d)) != NULL) {
    if (pdf_number(e->d_name) == number) {
      snprintf(path, sizeof(path), "%s/%s", folder, e->d_name);
      break;
    }
  }
  if (d) closedir(d);
  return path;
}

static bool read_pdf(void *ctx, uint32_t number, uint32_t offset, uint8_t *data,
                     uint32_t size) {
  (void)ctx;
  FILE *f = fopen(path_of(number), "rb");
  bool ok =
      f && fseek(f, offset, SEEK_SET) == 0 && fread(data, 1, size, f) == size;
  if (f) fclose(f);
  return ok;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: volume FOLDER IMAGE\n");
    return 2;
  }
  folder = argv[1];
  DIR *d = opendir(folder);
  if (!d) {
    fprintf(stderr, "volume: cannot read %s\n", folder);
    return 1;
  }
  PdfvolFile *files = NULL;
  uint32_t count = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    uint32_t number = pdf_number(e->d_name);
    char path[4096];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", folder, e->d_name);
    if (number == 0 || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
    files = realloc(files, (count + 1) * sizeof(*files));
    struct tm *t = localtime(&st.st_mtime);
    files[count].number = number;
    files[count].size = (uint32_t)st.st_size;
    files[count].date =
        0;  // before 1980, as FatFs writes a file it cannot date
    files[count].time = 0;
    if (t->tm_year >= 80) {
      files[count].date = (uint16_t)(((t->tm_year - 80) << 9) |
                                     ((t->tm_mon + 1) << 5) | t->tm_mday);
      files[count].time =
          (uint16_t)((t->tm_hour << 11) | (t->tm_min << 5) | (t->tm_sec / 2));
    }
    count++;
  }
  closedir(d);

  Pdfvol vol;
  uint32_t on = pdfvol_build(&vol, files, count, VOLUME_SERIAL, read_pdf, NULL);
  FILE *out = fopen(argv[2], "wb");
  if (!out) {
    fprintf(stderr, "volume: cannot write %s\n", argv[2]);
    return 1;
  }
  uint32_t end = PDFVOL_DATA_START + vol.usedClusters * PDFVOL_CLUSTER_SECTORS;
  static uint8_t run[RUN_SECTORS * PDFVOL_SECTOR_SIZE];
  for (uint32_t lba = 0; lba < end; lba += RUN_SECTORS) {
    uint32_t n = end - lba < RUN_SECTORS ? end - lba : RUN_SECTORS;
    if (!pdfvol_readSectors(&vol, lba, run, n) ||
        fwrite(run, PDFVOL_SECTOR_SIZE, n, out) != n) {
      fprintf(stderr, "volume: cannot write sector %u\n", lba);
      return 1;
    }
  }
  // The last sector, so the image is the whole volume.
  if (!pdfvol_readSectors(&vol, PDFVOL_SECTORS - 1, run, 1) ||
      fseeko(out, (off_t)(PDFVOL_SECTORS - 1) * PDFVOL_SECTOR_SIZE, SEEK_SET) ||
      fwrite(run, PDFVOL_SECTOR_SIZE, 1, out) != 1 || fclose(out) != 0) {
    fprintf(stderr, "volume: cannot write %s\n", argv[2]);
    return 1;
  }
  printf("%u PDFs of %u on the volume, %u clusters used\n", on, count,
         vol.usedClusters);
  free(files);
  return 0;
}
