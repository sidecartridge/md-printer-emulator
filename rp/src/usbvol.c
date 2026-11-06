/**
 * File: usbvol.c
 * Description: The folder's finished PDFs as a read-only USB drive
 *              (usbvol.h).
 *
 * TinyUSB's callbacks run inside tud_task(), in the main loop, so they reach
 * the card through FatFs as the rest of the loop does. One PDF stays open
 * between reads, since a computer reads a file in pieces and opening one
 * reads the folder.
 */
#include "usbvol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "debug.h"
#include "ff.h"
#include "pages.h"
#include "pdfvol/pdfvol.h"
#include "pico/stdlib.h"
#include "printer.h"
#include "tusb.h"
#ifdef CYW43_WL_GPIO_VBUS_PIN
#include "pico/cyw43_arch.h"
#endif

// SCSI SYNCHRONIZE CACHE (10); TinyUSB has no name for it.
#define SCSI_SYNCHRONIZE_CACHE 0x35
#define NAME_LENGTH 12  // nnnnnnnn.pdf
#define DIGITS 8

static bool ready;    // the card is mounted
static bool started;  // TinyUSB runs
static bool mounted;  // a computer has the device
static bool ejected;  // the computer ejected the volume
typedef enum {
  MEDIUM_READY,
  MEDIUM_REMOVED,  // the volume changed: no medium until removedUntil
  MEDIUM_CHANGED,  // back, and not yet reported as changed
} Medium;
static Medium medium;
static uint32_t changes;  // times the volume changed, in its serial number
static absolute_time_t removedUntil;
static uint32_t jobsListed;  // pages' rendered jobs at the last listing
static absolute_time_t nextVbusPoll;
static Pdfvol vol;
static PdfvolFile *files;
static uint32_t filesSize;  // PDFs files has room for
static FIL pdf;
static uint32_t pdfNumber;  // the PDF open in pdf, 0 for none
static UsbvolCounters counters;

// The number of an nnnnnnnn.pdf name, or 0.
static uint32_t pdf_number(const char *name) {
  if (strlen(name) != NAME_LENGTH ||
      strcasecmp(name + DIGITS, PAGES_PDF_EXTENSION) != 0) {
    return 0;
  }
  uint32_t number = 0;
  for (int i = 0; i < DIGITS; i++) {
    if (name[i] < '0' || name[i] > '9') return 0;
    number = number * 10 + (uint32_t)(name[i] - '0');
  }
  return number;
}

static void close_pdf(void) {
  if (pdfNumber != 0) f_close(&pdf);
  pdfNumber = 0;
}

static bool read_pdf(void *ctx, uint32_t number, uint32_t offset, uint8_t *data,
                     uint32_t size) {
  (void)ctx;
  if (pdfNumber != number) {
    close_pdf();
    char path[PRINTER_PATH_SIZE];
    printer_jobPath(path, sizeof(path), number, PAGES_PDF_EXTENSION);
    if (f_open(&pdf, path, FA_READ) != FR_OK) return false;
    pdfNumber = number;
  }
  UINT got = 0;
  return f_lseek(&pdf, offset) == FR_OK &&
         f_read(&pdf, data, size, &got) == FR_OK && got == size;
}

// The folder's PDFs, in one pass over it.
static void list_folder(void) {
  close_pdf();
  uint32_t count = 0;
  DIR dir;
  FILINFO info;
  if (f_opendir(&dir, printer_folder()) == FR_OK) {
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
      uint32_t number = pdf_number(info.fname);
      if (number == 0 || (info.fattrib & AM_DIR)) continue;
      if (count == filesSize) {
        PdfvolFile *more =
            realloc(files, (filesSize + USBVOL_FILES_STEP) * sizeof(*files));
        if (more == NULL) {
          DPRINTF("usbvol: no RAM for more than %lu PDFs\n",
                  (unsigned long)count);
          break;
        }
        files = more;
        filesSize += USBVOL_FILES_STEP;
      }
      files[count].number = number;
      files[count].size = (uint32_t)info.fsize;
      files[count].date = info.fdate;
      files[count].time = info.ftime;
      count++;
    }
    f_closedir(&dir);
  }
  counters.listings++;
  counters.pdfs =
      pdfvol_build(&vol, files, count, USBVOL_SERIAL + changes, read_pdf, NULL);
  jobsListed = pages_getCounters()->jobs;
}

static bool usb_powered(void) {
#ifdef CYW43_WL_GPIO_VBUS_PIN
  return cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);
#else
  return true;
#endif
}

void usbvol_init(void) {
  ready = true;
  nextVbusPoll = get_absolute_time();
}

void usbvol_loop(void) {
  if (!ready) return;
  if (!started) {
    if (absolute_time_diff_us(nextVbusPoll, get_absolute_time()) < 0) return;
    nextVbusPoll = make_timeout_time_ms(USBVOL_VBUS_POLL_MS);
    if (!usb_powered()) return;
    list_folder();
    started = tud_init(BOARD_TUD_RHPORT);
    ready = started;
    DPRINTF("usbvol: USB %s, %lu PDFs\n", started ? "started" : "failed",
            (unsigned long)counters.pdfs);
    return;
  }
  tud_task();
  if (pages_getCounters()->jobs != jobsListed) {
    // A job of no page leaves no PDF: the volume changes only with a PDF.
    uint32_t pdfs = vol.count;
    uint32_t last = pdfs ? vol.files[pdfs - 1].number : 0;
    list_folder();
    if (vol.count != pdfs ||
        (vol.count && vol.files[vol.count - 1].number != last)) {
      changes++;
      vol.serial = USBVOL_SERIAL + changes;
      medium = MEDIUM_REMOVED;
      removedUntil = make_timeout_time_ms(USBVOL_REMOVED_MS);
      DPRINTF("usbvol: %lu PDFs\n", (unsigned long)counters.pdfs);
    }
  }
  if (medium == MEDIUM_REMOVED &&
      absolute_time_diff_us(removedUntil, get_absolute_time()) >= 0) {
    medium = MEDIUM_CHANGED;
  }
}

bool usbvol_mounted(void) { return mounted && !ejected; }

const UsbvolCounters *usbvol_getCounters(void) { return &counters; }

// ---- TinyUSB's callbacks -----------------------------------------------

void tud_mount_cb(void) {
  mounted = true;
  ejected = false;
  DPRINTF("usbvol: mounted\n");
}

void tud_umount_cb(void) {
  mounted = false;
  DPRINTF("usbvol: unmounted\n");
}

void tud_suspend_cb(bool remote_wakeup_en) {
  (void)remote_wakeup_en;
  mounted = false;
}

void tud_resume_cb(void) { mounted = true; }

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
  (void)lun;
  static const char vendor[] = "SidecarT";
  static const char product[] = "Printer PDFs";
  const char *version = RELEASE_VERSION;
  if (version[0] == 'v') version++;
  memcpy(vendor_id, vendor, strlen(vendor));
  memcpy(product_id, product, strlen(product));
  size_t length = strlen(version);
  memcpy(product_rev, version, length < 4 ? length : 4);
}

// A changed volume reads as no medium for a while, then is reported once as
// a medium that may have changed.
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  counters.polls++;
  if (ejected || medium == MEDIUM_REMOVED) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return false;
  }
  if (medium == MEDIUM_CHANGED) {
    medium = MEDIUM_READY;
    tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
    return false;
  }
  return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
  (void)lun;
  *block_count = PDFVOL_SECTORS;
  *block_size = PDFVOL_SECTOR_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject) {
  (void)lun;
  (void)power_condition;
  if (load_eject) ejected = !start;
  return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
  if (offset % PDFVOL_SECTOR_SIZE != 0 || bufsize % PDFVOL_SECTOR_SIZE != 0) {
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);
    return -1;
  }
  if (medium == MEDIUM_REMOVED) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return -1;
  }
  uint64_t start = time_us_64();
  counters.reads++;
  if (!pdfvol_readSectors(&vol, lba + offset / PDFVOL_SECTOR_SIZE, buffer,
                          bufsize / PDFVOL_SECTOR_SIZE)) {
    counters.failed++;
    tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
    return -1;
  }
  uint32_t took = (uint32_t)(time_us_64() - start);
  if (took > counters.longestUs) counters.longestUs = took;
  return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  return false;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
  (void)lba;
  (void)offset;
  (void)buffer;
  (void)bufsize;
  counters.refused++;
  tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
  return -1;
}

// Refused, so the computer keeps asking whether the medium is ready
// (usbvol.h). TinyUSB answers it as done without this.
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun,
                                             uint8_t prohibit_removal,
                                             uint8_t control) {
  (void)prohibit_removal;
  (void)control;
  tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
  return false;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
                        uint16_t bufsize) {
  (void)buffer;
  (void)bufsize;
  switch (scsi_cmd[0]) {
    case SCSI_SYNCHRONIZE_CACHE:
      return 0;
    default:
      tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
      return -1;
  }
}
